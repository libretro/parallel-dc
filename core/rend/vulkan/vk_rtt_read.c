#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
/* See rtt_read.h. */
#include <string.h>
#include "rtt_read.h"
#include "rend/rtt_watch.h"
#include "vk_cmd_pool.h"
#include "hw/pvr/pvr_regs.h"

static vk_cmd_pool read_pool;       /* vk_read_picture()'s */
static bool read_pool_ready;

/* Copies of pictures off the card, each into memory this side reads: begun
 * ahead of the read where rtt_watch says which (vk_rtt_prefetch()), or by
 * the read itself. A batch of them is one submit with one fence. The read
 * runs in the signal handler, where nothing may be allocated, and nothing
 * is while a game runs: the batches, the ring every copy takes its room
 * from and each batch's image a finer picture is made the console's size
 * in, are made with the renderer (vk_rtt_init()), for renders of a
 * screen's size; a larger one has them made larger when it is watched
 * (copies_reserve()), which is said. */
#define RTT_COPIES 18
#define RTT_BATCHES 4
/* the ring: four of the largest render, no less than four of a screen's */
#define RTT_RING_RENDERS 4
#define RTT_SCREEN_W 640
#define RTT_SCREEN_H 480

typedef struct RttBatch
{
	VkCommandPool pool;
	VkCommandBuffer cmd;	/* the pool's: freed with it */
	VkFence fence;
	bool busy;				/* submitted, its fence not seen signalled */
	/* a finer picture made the console's size, for each copy recorded in
	 * turn; and whether one has been, in the batch being recorded */
	vk_attachment shrink;
	bool shrunk;
} RttBatch;

typedef struct RttCopy
{
	u32 serial;			/* the render's (RttWatch::serial), 0 if none */
	uintptr_t tex;
	u32 w, h;
	int batch;			/* the batch that writes it */
	u64 stamp;			/* when it was last begun, for the oldest */
	VkDeviceSize at, len;	/* its room in the ring; none if len is 0 */
} RttCopy;

static RttBatch batches[RTT_BATCHES];
static RttCopy copies[RTT_COPIES];
static bool copies_ready;
static u64 copy_stamp;
static vk_buffer ring;
static VkDeviceSize ring_size, ring_head;
static u32 shrink_w, shrink_h;		/* what each batch's shrink image is */
static bool said_larger;

static void wait_fence(VkFence fence)
{
	vkWaitForFences(vkc.device, 1, &fence, VK_TRUE, UINT64_MAX);
}

static bool batch_done(RttBatch *b)
{
	if (b->busy && vkGetFenceStatus(vkc.device, b->fence) == VK_SUCCESS)
		b->busy = false;
	return !b->busy;
}

static void copies_init(void)
{
	unsigned i;
	if (copies_ready)
		return;
	for (i = 0; i < RTT_BATCHES; i++)
	{
		RttBatch *b = &batches[i];
		VkCommandPoolCreateInfo poolInfo;
		VkCommandBufferAllocateInfo allocInfo;
		VkFenceCreateInfo fenceInfo;
		memset(&poolInfo, 0, sizeof(poolInfo));
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = vkc.iface->queue_index;
		vkCreateCommandPool(vkc.device, &poolInfo, NULL, &b->pool);
		memset(&allocInfo, 0, sizeof(allocInfo));
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = b->pool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		vkAllocateCommandBuffers(vkc.device, &allocInfo, &b->cmd);
		memset(&fenceInfo, 0, sizeof(fenceInfo));
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		vkCreateFence(vkc.device, &fenceInfo, NULL, &b->fence);
		b->busy = false;
	}
	for (i = 0; i < RTT_COPIES; i++)
	{
		copies[i].serial = 0;
		copies[i].batch = -1;
		copies[i].len = 0;
	}
	ring_size = ring_head = 0;
	shrink_w = shrink_h = 0;
	said_larger = false;
	copies_ready = true;
}

/* The room for renders of w by h pixels (the console's), finer than the
 * console's by @scale: the ring, and each batch's shrink image where
 * @scale is above 1. Made larger if it is not that large - every batch
 * waited for then, and every copy gone: at the start, or for a render
 * larger than a screen, which is said. */
static void copies_reserve(u32 w, u32 h, u32 scale)
{
	const VkDeviceSize need = (VkDeviceSize)w * h * 4 * RTT_RING_RENDERS;
	const bool shrink = scale > 1 && (w > shrink_w || h > shrink_h);
	unsigned i;

	if (need <= ring_size && !shrink)
		return;
	if (ring_size != 0 && !said_larger)
	{
		said_larger = true;
		WARN_LOG(RENDERER, "Render to a texture of %u x %u read back: its room made larger\n", w, h);
	}
	/* (what a batch on the card is still writing keeps its room until it
	 * is done) */
	for (i = 0; i < RTT_BATCHES; i++)
		if (batches[i].busy)
		{
			wait_fence(batches[i].fence);
			batches[i].busy = false;
		}
	for (i = 0; i < RTT_COPIES; i++)
	{
		copies[i].serial = 0;
		copies[i].len = 0;
	}
	if (need > ring_size)
	{
		vk_buffer_free(&ring);
		ring_size = 0;
		if (vk_buffer_init(&ring, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == VK_SUCCESS)
			ring_size = need;
		ring_head = 0;
	}
	if (shrink)
	{
		if (w > shrink_w)
			shrink_w = w;
		if (h > shrink_h)
			shrink_h = h;
		for (i = 0; i < RTT_BATCHES; i++)
			/* (sampled: its view, made with it, needs a use a view is for) */
			vk_attachment_init(&batches[i].shrink, shrink_w, shrink_h, VK_FORMAT_R8G8B8A8_UNORM,
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false,
					VK_SAMPLE_COUNT_1_BIT);
	}
}

/* A batch free to record into: none if every one is still on the card
 * and @wait is false; with @wait, the oldest waited for. */
static int batch_take(bool wait)
{
	int i;
	for (i = 0; i < RTT_BATCHES; i++)
		if (batch_done(&batches[i]))
			return i;
	if (!wait)
		return -1;
	wait_fence(batches[0].fence);
	batches[0].busy = false;
	return 0;
}

static RttCopy *copy_of(const RttWatch *watch)
{
	unsigned i;
	for (i = 0; i < RTT_COPIES; i++)
		if (copies[i].serial == watch->serial && copies[i].serial != 0 && copies[i].tex == watch->tex)
			return &copies[i];
	return NULL;
}

/* A copy may give up its room and its place: no batch on the card is
 * writing it, and it was not begun after @since (in the batch being
 * recorded) */
static bool copy_droppable(RttCopy *c, u64 since)
{
	if (c->len != 0 && c->batch >= 0 && !batch_done(&batches[c->batch]))
		return false;
	return c->serial == 0 || c->stamp <= since;
}

/* One to begin a copy of @len bytes in, with its room in the ring: the
 * ring's next, from its start where the end is too near; what was there
 * before goes, and the copy that held none or was begun longest ago gives
 * its place. Null if what is there may not go (droppable()). */
static RttCopy *copy_free(u64 since, VkDeviceSize len)
{
	RttCopy *place = NULL;
	VkDeviceSize at = ring_head;
	unsigned i;

	if (len > ring_size)
		return NULL;
	if (at + len > ring_size)
		at = 0;
	for (i = 0; i < RTT_COPIES; i++)
	{
		RttCopy *c = &copies[i];
		if (c->len != 0 && c->at < at + len && at < c->at + c->len && !copy_droppable(c, since))
			return NULL;
	}
	for (i = 0; i < RTT_COPIES; i++)
	{
		RttCopy *c = &copies[i];
		if (c->len != 0 && c->at < at + len && at < c->at + c->len)
		{
			c->serial = 0;
			c->len = 0;
		}
		if (!copy_droppable(c, since))
			continue;
		if (place == NULL || (place->len != 0 && (c->len == 0 || c->stamp < place->stamp)))
			place = c;
	}
	if (place == NULL)
		return NULL;
	place->at = at;
	place->len = len;
	ring_head = at + len;
	return place;
}

/* The picture as the console would have it, w by h, R G B A, copied into
 * c's buffer by @cmd: from the texture at the clip; or made the console's
 * size on the card first, filtered, where it is finer. */
static void record_copy(VkCommandBuffer cmd, int batch, const RttWatch *watch, RttCopy *c)
{
	const vk_texture *texture = (const vk_texture *)watch->tex;
	const u32 w = watch->w, h = watch->h, scale = watch->scale;
	const s32 x0 = (s32)(watch->x0 * (scale > 1 ? scale : 1)), y0 = (s32)(watch->y0 * (scale > 1 ? scale : 1));
	const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageSubresourceLayers layers;
	VkBufferImageCopy region;
	/* (its room made when the render was watched: copies_reserve()) */
	VkImage source = vk_texture_image(texture);

	memset(&layers, 0, sizeof(layers));
	layers.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	layers.layerCount = 1;
	vk_set_image_layout(cmd, source, format, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	if (scale > 1)
	{
		RttBatch *b = &batches[batch];
		const VkImage shrink = b->shrink.image;
		VkImageBlit blit;
		/* (the batch's one image: a copy before this one in the batch read
		 * it, and is done with it first) */
		vk_set_image_layout(cmd, shrink, format, 1, b->shrunk ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource = layers;
		blit.srcOffsets[0].x = x0;
		blit.srcOffsets[0].y = y0;
		blit.srcOffsets[1].x = (int32_t)(x0 + w * scale);
		blit.srcOffsets[1].y = (int32_t)(y0 + h * scale);
		blit.srcOffsets[1].z = 1;
		blit.dstSubresource = layers;
		blit.dstOffsets[1].x = (int32_t)w;
		blit.dstOffsets[1].y = (int32_t)h;
		blit.dstOffsets[1].z = 1;
		vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shrink, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
				VK_FILTER_LINEAR);
		vk_set_image_layout(cmd, shrink, format, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		b->shrunk = true;
		source = shrink;
	}
	/* (from the console-sized image at 0; from the texture at the clip) */
	memset(&region, 0, sizeof(region));
	region.bufferOffset = c->at;
	region.bufferRowLength = w;
	region.bufferImageHeight = h;
	region.imageSubresource = layers;
	region.imageOffset.x = scale > 1 ? 0 : x0;
	region.imageOffset.y = scale > 1 ? 0 : y0;
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ring.buffer, 1, &region);
	vk_set_image_layout(cmd, vk_texture_image(texture), format, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	c->serial = watch->serial;
	c->tex = watch->tex;
	c->w = w;
	c->h = h;
	c->stamp = ++copy_stamp;
}

/* Copies of @watches recorded into batch @b and submitted, those that have
 * one already left alone; false if there was nothing to copy. */
static bool batch_copies(int b, const RttWatch *const *watches, unsigned count)
{
	RttBatch *batch = &batches[b];
	const u64 since = copy_stamp;
	bool begun = false;
	VkMemoryBarrier barrier;
	unsigned i;

	for (i = 0; i < count; i++)
	{
		RttCopy *c;
		if (copy_of(watches[i]) != NULL)
			continue;
		c = copy_free(since, (VkDeviceSize)watches[i]->w * watches[i]->h * 4);
		if (c == NULL)
			break;
		if (!begun)
		{
			VkCommandBufferBeginInfo info;
			vkResetCommandBuffer(batch->cmd, 0);
			memset(&info, 0, sizeof(info));
			info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			vkBeginCommandBuffer(batch->cmd, &info);
			batch->shrunk = false;
			begun = true;
		}
		record_copy(batch->cmd, b, watches[i], c);
		c->batch = b;
	}
	if (!begun)
		return false;
	/* (one barrier for every buffer: the host reads them after the fence) */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(batch->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	vkEndCommandBuffer(batch->cmd);
	vkResetFences(vkc.device, 1, &batch->fence);
	vkc_submit(1, &batch->cmd, batch->fence);
	batch->busy = true;
	return true;
}

/* rtt_watch's: begun now, not waited for; nothing if no batch is free */
static void vk_rtt_prefetch(const RttWatch *const *watches, unsigned count)
{
	const int b = batch_take(false);
	if (b >= 0)
		batch_copies(b, watches, count);
}

/* The picture, from its copy - begun ahead as often as not, and done by
 * now - or from one begun here and waited for. */
static void vk_rtt_read(const RttWatch *watch, u8 *scratch, RttWatchTake take)
{
	RttCopy *c = copy_of(watch);
	RttBatch *batch;
	if (c == NULL)
	{
		if (!batch_copies(batch_take(true), &watch, 1))
		{
			/* every copy is being written: all of them waited for */
			unsigned i;
			for (i = 0; i < RTT_BATCHES; i++)
				if (batches[i].busy)
				{
					wait_fence(batches[i].fence);
					batches[i].busy = false;
				}
			batch_copies(0, &watch, 1);
		}
		c = copy_of(watch);
		if (c == NULL)
			return;
	}
	batch = &batches[c->batch];
	if (batch->busy)
	{
		wait_fence(batch->fence);
		batch->busy = false;
	}
	/* Read where the copy put it, with the host's cache in front of it (as
	 * the heap prefers for what is read back); memory without is slow to
	 * read a byte at a time, and is copied out first. */
	if (vk_buffer_memory_flags(&ring) & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
		take(watch, (const u8 *)vk_buffer_map(&ring) + c->at, (size_t)c->w * 4);
	else
	{
		vk_buffer_download(&ring, c->w * c->h * 4, scratch, (u32)c->at);
		take(watch, scratch, (size_t)c->w * 4);
	}
	c->serial = 0;
	c->len = 0;
}

static void copies_term(void)
{
	unsigned i;
	if (!copies_ready)
		return;
	for (i = 0; i < RTT_BATCHES; i++)
	{
		RttBatch *b = &batches[i];
		if (b->busy)
			wait_fence(b->fence);
		b->busy = false;
		/* (the command buffer with its pool) */
		if (b->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(vkc.device, b->pool, NULL);
		if (b->fence != VK_NULL_HANDLE)
			vkDestroyFence(vkc.device, b->fence, NULL);
		b->cmd = VK_NULL_HANDLE;
		b->pool = VK_NULL_HANDLE;
		b->fence = VK_NULL_HANDLE;
		vk_attachment_free(&b->shrink);
	}
	for (i = 0; i < RTT_COPIES; i++)
	{
		copies[i].serial = 0;
		copies[i].batch = -1;
		copies[i].len = 0;
	}
	vk_buffer_free(&ring);
	ring_size = ring_head = 0;
	copies_ready = false;
}

void vk_read_picture(VkImage image, VkFormat format, u32 width, u32 height, u8 *rgba)
{
	VkImageSubresourceLayers layers;
	VkBufferImageCopy region;
	VkBufferMemoryBarrier barrier;
	VkCommandBufferBeginInfo info;
	VkCommandBuffer cmd;
	VkFence fence;
	vk_buffer buffer;

	if (vk_buffer_init(&buffer, (VkDeviceSize)width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
		return;
	if (!vk_cmd_pool_init(&read_pool))
	{
		vk_buffer_free(&buffer);
		return;
	}
	read_pool_ready = true;
	vk_cmd_pool_begin_frame(&read_pool);
	cmd = vk_cmd_pool_allocate(&read_pool);
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &info);
	vk_set_image_layout(cmd, image, format, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	memset(&layers, 0, sizeof(layers));
	layers.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	layers.layerCount = 1;
	memset(&region, 0, sizeof(region));
	region.bufferRowLength = width;
	region.bufferImageHeight = height;
	region.imageSubresource = layers;
	region.imageExtent.width = width;
	region.imageExtent.height = height;
	region.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &region);
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = buffer.buffer;
	barrier.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
	vk_set_image_layout(cmd, image, format, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkEndCommandBuffer(cmd);
	vk_cmd_pool_end_frame(&read_pool);

	fence = vk_cmd_pool_fence(&read_pool);
	wait_fence(fence);
	vk_buffer_download(&buffer, width * height * 4, rgba, 0);
	vk_buffer_free(&buffer);

	if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB)
	{
		u32 i;
		for (i = 0; i < width * height; i++)
		{
			const u8 blue = rgba[i * 4];
			rgba[i * 4] = rgba[i * 4 + 2];
			rgba[i * 4 + 2] = blue;
		}
	}
}

/* The texture cache's, never this module's to delete. */
static void vk_rtt_release(uintptr_t tex)
{
	(void)tex;
}

static const RttWatchBackend vk_rtt_backend = { vk_rtt_read, vk_rtt_release, vk_rtt_prefetch };

/* What the render covers of video memory, as the drawers' render pass end
 * has it: the clip's pixels, from (x0, y0) of the frame at addr, which
 * moves to the first of them. */
static void covered(u32 *addr, u32 x0, u32 y0, u32 *width, u32 height, u32 *stride, u32 *bytes)
{
	*stride = FB_W_LINESTRIDE.stride * 8;
	if (*stride == 0)
		*stride = (x0 + *width) * 2;
	else if ((x0 + *width) * 2 > *stride)
		/* happens for Virtua Tennis */
		*width = x0 * 2 >= *stride ? 0 : *stride / 2 - x0;
	*addr += y0 * *stride + x0 * 2;
	*bytes = *width == 0 || height == 0 ? 0 : *stride * (height - 1) + *width * 2;
}

void vk_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	u32 stride, bytes;
	covered(&addr, x0, y0, &width, height, &stride, &bytes);
	if (bytes != 0)
		rtt_watch_supersede(addr, bytes);
}

void vk_rtt_watch_texture(vk_texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	RttWatch watch;
	memset(&watch, 0, sizeof(watch));
	covered(&addr, x0, y0, &width, height, &watch.stride, &watch.bytes);
	watch.addr = addr;
	watch.w = width;
	watch.h = height;
	watch.x0 = x0;
	watch.y0 = y0;
	watch.packmode = FB_W_CTRL.fb_packmode;
	watch.kval_bit = (FB_W_CTRL.fb_kval & 0x80) << 8;
	watch.alpha_threshold = FB_W_CTRL.fb_alpha_threshold;
	watch.dither = FB_W_CTRL.fb_dither;
	watch.tex = (uintptr_t)texture;
	watch.target = &texture->tc;
	watch.scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	if (watch.bytes == 0)
		return;
	copies_init();
	copies_reserve(watch.w, watch.h, watch.scale);
	rtt_watch_add(&watch, &vk_rtt_backend);
}

void vk_rtt_init(void)
{
	copies_init();
	/* (the room for a screen's render now, finer by the upscaling if it is) */
	copies_reserve(RTT_SCREEN_W, RTT_SCREEN_H, settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1);
}

void vk_rtt_term(void)
{
	rtt_watch_term();
	copies_term();
	if (read_pool_ready)
		vk_cmd_pool_term(&read_pool);
	read_pool_ready = false;
}
