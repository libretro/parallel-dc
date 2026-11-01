#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/* See rtt_read.h. */
#include "rtt_read.h"
#include "rend/rtt_watch.h"
#include "vulkan.h"
#include "texture.h"
#include "commandpool.h"
#include "hw/pvr/pvr_regs.h"
#include <memory>

static CommandPool read_pool;       /* vk_read_picture()'s */
static bool read_pool_ready;

/* Copies of pictures off the card, each into memory this side reads: begun
 * ahead of the read where rtt_watch says which (vk_rtt_prefetch()), or by
 * the read itself. A batch of them is one submit with one fence. */
#define RTT_COPIES 18
#define RTT_BATCHES 4

struct RttBatch
{
	vk::UniqueCommandPool pool;
	vk::UniqueCommandBuffer cmd;
	vk::UniqueFence fence;
	bool busy;          /* submitted, its fence not seen signalled */
};

struct RttCopy
{
	u32 serial;         /* the render's (RttWatch::serial), 0 if none */
	uintptr_t tex;
	u32 w, h;
	int batch;          /* the batch that writes it */
	u64 stamp;          /* when it was last begun, for the oldest */
	std::unique_ptr<BufferData> buffer;               /* grown, never shrunk */
	std::unique_ptr<FramebufferAttachment> shrink;    /* the console's size, for a finer picture */
};

static RttBatch batches[RTT_BATCHES];
static RttCopy copies[RTT_COPIES];
static bool copies_ready;
static u64 copy_stamp;

static bool batch_done(RttBatch& b)
{
	if (b.busy && VulkanContext::Instance()->GetDevice().getFenceStatus(*b.fence) == vk::Result::eSuccess)
		b.busy = false;
	return !b.busy;
}

static void copies_init(void)
{
	if (copies_ready)
		return;
	vk::Device device = VulkanContext::Instance()->GetDevice();
	for (RttBatch& b : batches)
	{
		b.pool = device.createCommandPoolUnique(vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eTransient
				| vk::CommandPoolCreateFlagBits::eResetCommandBuffer, VulkanContext::Instance()->GetGraphicsQueueFamilyIndex()));
		b.cmd = std::move(device.allocateCommandBuffersUnique(vk::CommandBufferAllocateInfo(*b.pool, vk::CommandBufferLevel::ePrimary, 1)).front());
		b.fence = device.createFenceUnique(vk::FenceCreateInfo());
		b.busy = false;
	}
	for (RttCopy& c : copies)
	{
		c.serial = 0;
		c.batch = -1;
	}
	copies_ready = true;
}

/* A batch free to record into: none if every one is still on the card
 * and @wait is false; with @wait, the oldest waited for. */
static int batch_take(bool wait)
{
	for (int i = 0; i < RTT_BATCHES; i++)
		if (batch_done(batches[i]))
			return i;
	if (!wait)
		return -1;
	vk::Device device = VulkanContext::Instance()->GetDevice();
	device.waitForFences(1, &batches[0].fence.get(), true, UINT64_MAX);
	batches[0].busy = false;
	return 0;
}

static RttCopy *copy_of(const RttWatch *watch)
{
	for (RttCopy& c : copies)
		if (c.serial == watch->serial && c.serial != 0 && c.tex == watch->tex)
			return &c;
	return nullptr;
}

/* One to begin a copy in: one that holds none, or the one begun longest
 * ago - never one a batch on the card is still writing, nor one begun
 * after @since (in the batch being recorded). */
static RttCopy *copy_free(u64 since)
{
	RttCopy *oldest = nullptr;

	for (RttCopy& c : copies)
	{
		if (c.batch >= 0 && !batch_done(batches[c.batch]))
			continue;
		if (c.serial != 0 && c.stamp > since)
			continue;
		if (c.serial == 0)
			return &c;
		if (oldest == nullptr || c.stamp < oldest->stamp)
			oldest = &c;
	}
	return oldest;
}

/* The picture as the console would have it, w by h, R G B A, copied into
 * c's buffer by @cmd: from the texture at the clip; or made the console's
 * size on the card first, filtered, where it is finer. */
static void record_copy(vk::CommandBuffer cmd, const RttWatch *watch, RttCopy& c)
{
	Texture *texture = (Texture *)watch->tex;
	VulkanContext *context = VulkanContext::Instance();
	const u32 w = watch->w, h = watch->h, scale = watch->scale;
	const s32 x0 = (s32)(watch->x0 * (scale > 1 ? scale : 1)), y0 = (s32)(watch->y0 * (scale > 1 ? scale : 1));
	const vk::Format format = vk::Format::eR8G8B8A8Unorm;
	const vk::ImageSubresourceLayers layers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
	const vk::DeviceSize bytes = (vk::DeviceSize)w * h * 4;

	if (!c.buffer || c.buffer->bufferSize < bytes)
		c.buffer = std::unique_ptr<BufferData>(new BufferData(bytes, vk::BufferUsageFlagBits::eTransferDst));
	vk::Image source = texture->GetImage();
	setImageLayout(cmd, source, format, 1, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal);
	if (scale > 1)
	{
		if (!c.shrink || c.shrink->getExtent().width < w || c.shrink->getExtent().height < h)
		{
			c.shrink = std::unique_ptr<FramebufferAttachment>(new FramebufferAttachment(context->GetPhysicalDevice(), context->GetDevice()));
			/* (sampled: its view, made with it, needs a use a view is for) */
			c.shrink->Init(w, h, format, vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst
					| vk::ImageUsageFlagBits::eSampled, false);
		}
		const std::array<vk::Offset3D, 2> from = { vk::Offset3D(x0, y0, 0), vk::Offset3D(x0 + w * scale, y0 + h * scale, 1) };
		const std::array<vk::Offset3D, 2> to = { vk::Offset3D(0, 0, 0), vk::Offset3D(w, h, 1) };

		setImageLayout(cmd, c.shrink->GetImage(), format, 1, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
		cmd.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, c.shrink->GetImage(), vk::ImageLayout::eTransferDstOptimal,
				vk::ImageBlit(layers, from, layers, to), vk::Filter::eLinear);
		setImageLayout(cmd, c.shrink->GetImage(), format, 1, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal);
		source = c.shrink->GetImage();
	}
	/* (from the console-sized image at 0; from the texture at the clip) */
	const vk::Offset3D at = scale > 1 ? vk::Offset3D(0, 0, 0) : vk::Offset3D(x0, y0, 0);
	cmd.copyImageToBuffer(source, vk::ImageLayout::eTransferSrcOptimal, *c.buffer->buffer,
			vk::BufferImageCopy(0, w, h, layers, at, vk::Extent3D(w, h, 1)));
	setImageLayout(cmd, texture->GetImage(), format, 1, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
	c.serial = watch->serial;
	c.tex = watch->tex;
	c.w = w;
	c.h = h;
	c.stamp = ++copy_stamp;
}

/* Copies of @watches recorded into batch @b and submitted, those that have
 * one already left alone; false if there was nothing to copy. */
static bool batch_copies(int b, const RttWatch *const *watches, unsigned count)
{
	RttBatch& batch = batches[b];
	const u64 since = copy_stamp;
	bool begun = false;

	for (unsigned i = 0; i < count; i++)
	{
		if (copy_of(watches[i]) != nullptr)
			continue;
		RttCopy *c = copy_free(since);
		if (c == nullptr)
			break;
		if (!begun)
		{
			batch.cmd->reset(vk::CommandBufferResetFlags());
			batch.cmd->begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
			begun = true;
		}
		record_copy(*batch.cmd, watches[i], *c);
		c->batch = b;
	}
	if (!begun)
		return false;
	/* (one barrier for every buffer: the host reads them after the fence) */
	const vk::MemoryBarrier barrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead);
	batch.cmd->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, barrier, nullptr, nullptr);
	batch.cmd->end();
	VulkanContext::Instance()->GetDevice().resetFences(1, &batch.fence.get());
	vk::CommandBuffer cmd = *batch.cmd;
	VulkanContext::Instance()->SubmitCommandBuffers(1, &cmd, *batch.fence);
	batch.busy = true;
	return true;
}

/* rtt_watch's: begun now, not waited for; nothing if no batch is free */
static void vk_rtt_prefetch(const RttWatch *const *watches, unsigned count)
{
	copies_init();
	const int b = batch_take(false);
	if (b >= 0)
		batch_copies(b, watches, count);
}

/* The picture, from its copy - begun ahead as often as not, and done by
 * now - or from one begun here and waited for. */
static void vk_rtt_read(const RttWatch *watch, u8 *scratch, RttWatchTake take)
{
	copies_init();
	RttCopy *c = copy_of(watch);
	if (c == nullptr)
	{
		if (!batch_copies(batch_take(true), &watch, 1))
		{
			/* every copy is being written: all of them waited for */
			vk::Device device = VulkanContext::Instance()->GetDevice();
			for (RttBatch& b : batches)
				if (b.busy)
				{
					device.waitForFences(1, &b.fence.get(), true, UINT64_MAX);
					b.busy = false;
				}
			batch_copies(0, &watch, 1);
		}
		c = copy_of(watch);
	}
	RttBatch& batch = batches[c->batch];
	if (batch.busy)
	{
		VulkanContext::Instance()->GetDevice().waitForFences(1, &batch.fence.get(), true, UINT64_MAX);
		batch.busy = false;
	}
	const BufferData *data = c->buffer.get();
	/* Read where the copy put it, with the host's cache in front of it (as
	 * the heap prefers for what is read back); memory without is slow to
	 * read a byte at a time, and is copied out first. */
	if (data->allocation.PropertyFlags() & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
		take(watch, (const u8 *)data->allocation.MapMemory(), (size_t)c->w * 4);
	else
	{
		data->download(c->w * c->h * 4, scratch);
		take(watch, scratch, (size_t)c->w * 4);
	}
	c->serial = 0;
}

static void copies_term(void)
{
	if (!copies_ready)
		return;
	vk::Device device = VulkanContext::Instance()->GetDevice();
	for (RttBatch& b : batches)
	{
		if (b.busy)
			device.waitForFences(1, &b.fence.get(), true, UINT64_MAX);
		b.busy = false;
		b.cmd.reset();
		b.pool.reset();
		b.fence.reset();
	}
	for (RttCopy& c : copies)
	{
		c.buffer.reset();
		c.shrink.reset();
		c.serial = 0;
		c.batch = -1;
	}
	copies_ready = false;
}

void vk_read_picture(vk::Image image, vk::Format format, u32 width, u32 height, u8 *rgba)
{
	VulkanContext *context = VulkanContext::Instance();
	const vk::ImageSubresourceLayers layers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
	BufferData buffer((vk::DeviceSize)width * height * 4, vk::BufferUsageFlagBits::eTransferDst);

	read_pool.Init();
	read_pool_ready = true;
	read_pool.BeginFrame();
	vk::CommandBuffer cmd = read_pool.Allocate();
	cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	setImageLayout(cmd, image, format, 1, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal);
	cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, *buffer.buffer,
			vk::BufferImageCopy(0, width, height, layers, vk::Offset3D(0, 0, 0), vk::Extent3D(width, height, 1)));
	vk::BufferMemoryBarrier barrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *buffer.buffer, 0, VK_WHOLE_SIZE);
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, nullptr, barrier, nullptr);
	setImageLayout(cmd, image, format, 1, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
	cmd.end();
	read_pool.EndFrame();

	vk::Fence fence = read_pool.GetCurrentFence();
	context->GetDevice().waitForFences(1, &fence, true, UINT64_MAX);
	buffer.download(width * height * 4, rgba);

	if (format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb)
		for (u32 i = 0; i < width * height; i++)
		{
			const u8 blue = rgba[i * 4];
			rgba[i * 4] = rgba[i * 4 + 2];
			rgba[i * 4 + 2] = blue;
		}
}

/* The texture cache's, never this module's to delete. */
static void vk_rtt_release(uintptr_t)
{
}

static const RttWatchBackend vk_rtt_backend = { vk_rtt_read, vk_rtt_release, vk_rtt_prefetch };

/* What the render covers of video memory, as EndRenderPass() has it: the
 * clip's pixels, from (x0, y0) of the frame at addr, which moves to the
 * first of them. */
static void covered(u32& addr, u32 x0, u32 y0, u32& width, u32 height, u32& stride, u32& bytes)
{
	stride = FB_W_LINESTRIDE.stride * 8;
	if (stride == 0)
		stride = (x0 + width) * 2;
	else if ((x0 + width) * 2 > stride)
		// Happens for Virtua Tennis
		width = x0 * 2 >= stride ? 0 : stride / 2 - x0;
	addr += y0 * stride + x0 * 2;
	bytes = width == 0 || height == 0 ? 0 : stride * (height - 1) + width * 2;
}

void vk_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	u32 stride, bytes;

	covered(addr, x0, y0, width, height, stride, bytes);
	if (bytes != 0)
		rtt_watch_supersede(addr, bytes);
}

void vk_rtt_watch(Texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	RttWatch watch = {};

	covered(addr, x0, y0, width, height, watch.stride, watch.bytes);
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
	watch.target = texture;
	watch.scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	if (watch.bytes != 0)
		rtt_watch_add(&watch, &vk_rtt_backend);
}

void vk_rtt_term(void)
{
	rtt_watch_term();
	copies_term();
	if (read_pool_ready)
		read_pool.Term();
	read_pool_ready = false;
}
