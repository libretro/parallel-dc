/* See vk_image.h. */
#include <stdlib.h>
#include <string.h>
#include "vk_image.h"
#include "hw/pvr/ta_words.h"
#include "oslib/oslib.h"
#include "rend/TexCache.h"

void vk_set_image_layout(VkCommandBuffer cmd, VkImage image, VkFormat format, u32 levels, VkImageLayout from, VkImageLayout to)
{
	VkAccessFlags srcAccess = 0, dstAccess = 0;
	VkPipelineStageFlags srcStage = 0, dstStage = 0;
	VkImageMemoryBarrier barrier;

	switch (from)
	{
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
		break;
	case VK_IMAGE_LAYOUT_PREINITIALIZED:
		srcAccess = VK_ACCESS_HOST_WRITE_BIT;
		break;
	case VK_IMAGE_LAYOUT_GENERAL:	/* nothing to wait for */
	case VK_IMAGE_LAYOUT_UNDEFINED:
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		srcAccess = VK_ACCESS_SHADER_READ_BIT;
		break;
	default:
		verify(false);
		break;
	}

	switch (from)
	{
	case VK_IMAGE_LAYOUT_GENERAL:
	case VK_IMAGE_LAYOUT_PREINITIALIZED:
		srcStage = VK_PIPELINE_STAGE_HOST_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_UNDEFINED:
		srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	default:
		verify(false);
		break;
	}

	switch (to)
	{
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		dstAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		dstAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		break;
	case VK_IMAGE_LAYOUT_GENERAL:	/* nothing to make wait */
		dstStage = VK_PIPELINE_STAGE_HOST_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		dstAccess = VK_ACCESS_SHADER_READ_BIT;
		dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		dstAccess = VK_ACCESS_TRANSFER_READ_BIT;
		dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
		dstAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		break;
	default:
		verify(false);
		break;
	}

	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	if (to == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || to == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT
				| (vk_format_has_stencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
	else
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = levels;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static u64 viewSerials;

u64 vk_new_view_serial(void)
{
	return ++viewSerials;
}

/* an image of @format, optimal, in no layout yet */
static VkImage make_image(VkImageCreateFlags flags, VkFormat format, u32 width, u32 height, u32 levels,
		VkSampleCountFlagBits samples, VkImageUsageFlags usage)
{
	VkImageCreateInfo info;
	VkImage image = VK_NULL_HANDLE;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.flags = flags;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = levels;
	info.arrayLayers = 1;
	info.samples = samples;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(vkc.device, &info, NULL, &image) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return image;
}

/* a 2D view of @image's @levels from @level, @aspect; @next chained */
static VkImageView make_view(VkImage image, VkFormat format, VkImageAspectFlags aspect, u32 level, u32 levels,
		const VkComponentMapping *components, const void *next)
{
	VkImageViewCreateInfo info;
	VkImageView view = VK_NULL_HANDLE;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	info.pNext = next;
	info.image = image;
	info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	info.format = format;
	if (components != NULL)
		info.components = *components;
	info.subresourceRange.aspectMask = aspect;
	info.subresourceRange.baseMipLevel = level;
	info.subresourceRange.levelCount = levels;
	info.subresourceRange.layerCount = 1;
	if (vkCreateImageView(vkc.device, &info, NULL, &view) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return view;
}

bool vk_attachment_init(vk_attachment *a, u32 width, u32 height, VkFormat format, VkImageUsageFlags usage, bool readable,
		VkSampleCountFlagBits samples)
{
	const bool depth = vk_format_has_stencil(format);
	vk_attachment_free(a);
	a->format = format;
	a->extent.width = width;
	a->extent.height = height;
	if (readable && (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
			&& vk_buffer_init(&a->staging, (VkDeviceSize)width * height * 4,
					VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
		return false;
	a->image = make_image(0, format, width, height, 1, samples, usage);
	if (a->image == VK_NULL_HANDLE)
	{
		vk_attachment_free(a);
		return false;
	}
	if (!vkc_heap_alloc_image(a->image, false, false, (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0, &a->alloc))
	{
		vk_attachment_free(a);
		return false;
	}
	a->allocated = true;
	a->view = make_view(a->image, format, depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, NULL, NULL);
	if (a->view == VK_NULL_HANDLE)
	{
		vk_attachment_free(a);
		return false;
	}
	a->viewSerial = vk_new_view_serial();
	/* the stencil's too, for an input attachment */
	if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && (usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
	{
		a->stencilView = make_view(a->image, format, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, NULL, NULL);
		if (a->stencilView == VK_NULL_HANDLE)
		{
			vk_attachment_free(a);
			return false;
		}
	}
	return true;
}

void vk_attachment_free(vk_attachment *a)
{
	if (a->stencilView != VK_NULL_HANDLE)
		vkDestroyImageView(vkc.device, a->stencilView, NULL);
	if (a->view != VK_NULL_HANDLE)
		vkDestroyImageView(vkc.device, a->view, NULL);
	if (a->image != VK_NULL_HANDLE)
		vkDestroyImage(vkc.device, a->image, NULL);
	if (a->allocated)
		vk_heap_free(&vkc.heap, &a->alloc);
	vk_buffer_free(&a->staging);
	a->stencilView = a->view = VK_NULL_HANDLE;
	a->image = VK_NULL_HANDLE;
	a->allocated = false;
}

VkFramebuffer vk_kept_framebuffer_get(vk_kept_framebuffer *k, VkRenderPass pass, u32 count, const VkImageView *views,
		const u64 *serials, u32 width, u32 height)
{
	bool same = k->framebuffer != VK_NULL_HANDLE && pass == k->pass && count == k->count && width == k->width
			&& height == k->height;
	u32 i;
	for (i = 0; same && i < count; i++)
		same = serials[i] == k->serials[i] && serials[i] != 0;
	if (!same)
	{
		VkFramebufferCreateInfo info;
		vk_kept_framebuffer_free(k);
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		info.renderPass = pass;
		info.attachmentCount = count;
		info.pAttachments = views;
		info.width = width;
		info.height = height;
		info.layers = 1;
		if (vkCreateFramebuffer(vkc.device, &info, NULL, &k->framebuffer) != VK_SUCCESS)
		{
			k->framebuffer = VK_NULL_HANDLE;
			return VK_NULL_HANDLE;
		}
		k->pass = pass;
		k->count = count;
		k->width = width;
		k->height = height;
		for (i = 0; i < count && i < 3; i++)
			k->serials[i] = serials[i];
	}
	return k->framebuffer;
}

void vk_kept_framebuffer_free(vk_kept_framebuffer *k)
{
	if (k->framebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(vkc.device, k->framebuffer, NULL);
	k->framebuffer = VK_NULL_HANDLE;
}

static u32 axis(u32 clamp, u32 flip)
{
	return clamp ? 0 : flip ? 1 : 2;
}

static VkSamplerAddressMode address_mode(u32 a)
{
	return a == 0 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : a == 1 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
			: VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

static VkSampler make_sampler(u32 i)
{
	VkSamplerCreateInfo info;
	VkSampler sampler = VK_NULL_HANDLE;
	const VkFilter filter = i / 9 % 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	const u32 mipMapD = i / 18;
	const bool anisotropic = settings.rend.AnisotropicFiltering > 1 && vkc.samplerAnisotropy && filter == VK_FILTER_LINEAR;
	const float maxAnisotropy = vkc.samplerAnisotropy ? vkc.maxSamplerAnisotropy : 1.f;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.magFilter = info.minFilter = filter;
	info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = address_mode(i / 3 % 3);
	info.addressModeV = address_mode(i % 3);
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.mipLodBias = D_Adjust_LoD_Bias[mipMapD];
	info.anisotropyEnable = anisotropic;
	info.maxAnisotropy = (float)settings.rend.AnisotropicFiltering < maxAnisotropy ? (float)settings.rend.AnisotropicFiltering : maxAnisotropy;
	info.compareOp = VK_COMPARE_OP_NEVER;
	info.minLod = 0.0f;
	info.maxLod = 256.0f;
	info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
	if (vkCreateSampler(vkc.device, &info, NULL, &sampler) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return sampler;
}

void vk_samplers_init(vk_samplers *s)
{
	u32 i;
	vk_samplers_term(s);
	for (i = 0; i < VK_SAMPLERS; i++)
		s->samplers[i] = make_sampler(i);
}

void vk_samplers_term(vk_samplers *s)
{
	u32 i;
	for (i = 0; i < VK_SAMPLERS; i++)
		if (s->samplers[i] != VK_NULL_HANDLE)
		{
			vkDestroySampler(vkc.device, s->samplers[i], NULL);
			s->samplers[i] = VK_NULL_HANDLE;
		}
}

VkSampler vk_samplers_get(vk_samplers *s, u32 tsp)
{
	const u32 i = ((TAW_GET(tsp, TSP_MipMapD) * 2 + (TAW_GET(tsp, TSP_FilterMode) != 0 ? 1 : 0)) * 3
			+ axis(TAW_GET(tsp, TSP_ClampU), TAW_GET(tsp, TSP_FlipU))) * 3 + axis(TAW_GET(tsp, TSP_ClampV), TAW_GET(tsp, TSP_FlipV));
	if (s->samplers[i] == VK_NULL_HANDLE)
		s->samplers[i] = make_sampler(i);
	return s->samplers[i];
}

vk_upload_ring *vk_upload_ring_get(void)
{
	static vk_upload_ring ring;
	return &ring;
}

bool vk_upload_ring_init(vk_upload_ring *r, size_t bytes, vk_cmd_pool *pool)
{
	u32 i;
	r->pool = pool;
	if (r->mapped != NULL && r->size == (u64)bytes)
		return true;
	vk_upload_ring_term(r);
	r->pool = pool;
	if (vk_buffer_init(&r->buffer, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
		return false;
	r->mapped = (u8 *)vk_buffer_map(&r->buffer);
	r->size = bytes;
	r->head = r->tail = 0;
	r->current = 0;
	for (i = 0; i < VK_UPLOAD_RING_FRAMES; i++)
		r->frameLive[i] = false;
	INFO_LOG(RENDERER, "Vulkan: upload ring of %u KB\n", (u32)(bytes >> 10));
	return true;
}

void vk_upload_ring_term(vk_upload_ring *r)
{
	u32 i;
	if (r->mapped != NULL)
		NOTICE_LOG(RENDERER, "Vulkan: upload ring: %u MB handed out, %u frames found done before their turn, %u textures put off for want of room\n",
				(u32)(r->head >> 20), (u32)r->statPolled, (u32)r->statFull);
	r->mapped = NULL;
	vk_buffer_free(&r->buffer);
	r->size = r->head = r->tail = 0;
	for (i = 0; i < VK_UPLOAD_RING_FRAMES; i++)
		r->frameLive[i] = false;
}

void vk_upload_ring_set_current_index(vk_upload_ring *r, int index)
{
	u32 now;
	if (r->mapped == NULL)
		return;
	/* where the frame that ends here ends; then this place's last frame is done */
	r->frameEnd[r->current % VK_UPLOAD_RING_FRAMES] = r->head;
	r->frameLive[r->current % VK_UPLOAD_RING_FRAMES] = true;
	r->current = (u32)index;
	now = r->current % VK_UPLOAD_RING_FRAMES;
	if (r->frameLive[now])
	{
		if (r->frameEnd[now] > r->tail)
			r->tail = r->frameEnd[now];
		r->frameLive[now] = false;
	}
}

u8 *vk_upload_ring_allocate(vk_upload_ring *r, size_t bytes, VkBuffer *buffer, VkDeviceSize *offset)
{
	if (r->mapped == NULL)
		return NULL;
	bytes = (bytes + 15) & ~(size_t)15;
	if (bytes > r->size)
	{
		r->statFull++;
		return NULL;
	}
	for (;;)
	{
		const u64 at = r->head % r->size;
		/* (a piece is never split: from the start again if it would be) */
		const u64 skip = at + bytes > r->size ? r->size - at : 0;
		int frames, k;
		bool freed = false;
		if (r->head + skip + bytes - r->tail <= r->size)
		{
			u8 *memory;
			r->head += skip;
			memory = r->mapped + r->head % r->size;
			*buffer = r->buffer.buffer;
			*offset = r->head % r->size;
			r->head += bytes;
			return memory;
		}
		/* Full: the oldest frame still out, if its fence says it is done
		 * (the frames are done in the order they went) */
		frames = r->pool != NULL ? ((int)r->pool->slotCount < VK_UPLOAD_RING_FRAMES ? (int)r->pool->slotCount
				: VK_UPLOAD_RING_FRAMES) : 0;
		for (k = 1; k < frames && !freed; k++)
		{
			const u32 slot = (r->current + (u32)k) % (u32)frames;
			if (!r->frameLive[slot])
				continue;
			if (!vk_cmd_pool_is_done(r->pool, (int)slot))
				break;
			if (r->frameEnd[slot] > r->tail)
				r->tail = r->frameEnd[slot];
			r->frameLive[slot] = false;
			r->statPolled++;
			freed = true;
		}
		if (!freed)
		{
			r->statFull++;
			return NULL;
		}
	}
}

/* The formats the pool has images of, by its numbers for them: the
 * converters' five, the two the decoder copies planar textures into, and
 * the pictures rendered to */
static const VkFormat poolFormats[VK_TEX_POOL_FORMATS] = {
	VK_FORMAT_R5G5B5A1_UNORM_PACK16, VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_R4G4B4A4_UNORM_PACK16,
	VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM,
	VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_FORMAT_B4G4R4A4_UNORM_PACK16,
	VK_FORMAT_R8G8B8A8_UNORM,
};
static const char * const poolFormatNames[VK_TEX_POOL_FORMATS] = { "5551", "565", "4444", "8888", "8-bit", "1555", "4444 (planar)",
		"8888 rendered to" };
/* what the pictures rendered to are used for (the drawers' renderTargetUsage, and to be written as a texture after) */
#define BASE_TARGET_USAGE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT \
		| VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)

static int format_index(VkFormat format)
{
	int i;
	for (i = 0; i < VK_TEX_POOL_FORMATS; i++)
		if (poolFormats[i] == format)
			return i;
	return -1;
}

void vk_tex_pool_construct(vk_tex_pool *p)
{
	memset(p, 0, sizeof(*p));
	p->book.frames = 1;
}

static void free_image(vk_pooled_image *pi)
{
	u32 level;
	for (level = 0; level < TEXD_MAX_LEVELS; level++)
		if (pi->storageViews[level] != VK_NULL_HANDLE)
			vkDestroyImageView(vkc.device, pi->storageViews[level], NULL);
	if (pi->view != VK_NULL_HANDLE)
		vkDestroyImageView(vkc.device, pi->view, NULL);
	if (pi->image != VK_NULL_HANDLE)
		vkDestroyImage(vkc.device, pi->image, NULL);
	if (pi->alloc.memory != VK_NULL_HANDLE)
		vk_heap_free(&vkc.heap, &pi->alloc);
	free(pi);
}

static vk_pooled_image *make(vk_tex_pool *p, int cls, bool late)
{
	static const VkComponentMapping swizzled = {
		VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_B
	};
	const u32 f = tex_pool_format(cls);
	const u32 width = tex_pool_width(cls), height = tex_pool_height(cls);
	const VkFormat format = p->formats[f];
	VkImageCreateFlags flags = 0;
	VkImageViewUsageCreateInfo viewUsage;
	vk_pooled_image *pi = (vk_pooled_image *)calloc(1, sizeof(vk_pooled_image));
	if (pi == NULL)
		return NULL;

	pi->format = format;
	pi->levels = tex_pool_mips(cls) ? 1 + bitscanrev(width > height ? width : height) : 1;
	pi->storage = p->storable[f];
	pi->usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (f == VK_TEX_POOL_TARGET)
		pi->usage = p->targetUsage;
	if (pi->storage)
	{
		/* written through views of a format of integers of the same size:
		 * the 16- and 8-bit formats cannot be storage images themselves */
		pi->usage |= VK_IMAGE_USAGE_STORAGE_BIT;
		flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
		if (format != VK_FORMAT_R8G8B8A8_UNORM)
			flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	}
	pi->image = make_image(flags, format, width, height, pi->levels, VK_SAMPLE_COUNT_1_BIT, pi->usage);
	if (pi->image == VK_NULL_HANDLE || !vkc_heap_alloc_image(pi->image, false, false, false, &pi->alloc))
	{
		ERROR_LOG(RENDERER, "Vulkan: a texture pool image could not be made\n");
		free_image(pi);
		return NULL;
	}

	/* A 4444 texture copied as it is: the PowerVR has alpha, red, green
	 * and blue from the top, which this format calls blue, green, red and
	 * alpha. (Sampled only: the image's other uses are through views of
	 * other formats.) */
	memset(&viewUsage, 0, sizeof(viewUsage));
	viewUsage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
	viewUsage.usage = pi->usage & ~VK_IMAGE_USAGE_STORAGE_BIT;
	pi->view = make_view(pi->image, format, VK_IMAGE_ASPECT_COLOR_BIT, 0, pi->levels,
			format == VK_FORMAT_B4G4R4A4_UNORM_PACK16 ? &swizzled : NULL,
			(flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) ? &viewUsage : NULL);
	if (pi->view == VK_NULL_HANDLE)
	{
		free_image(pi);
		return NULL;
	}
	pi->viewSerial = vk_new_view_serial();
	if (pi->storage)
	{
		const VkFormat viewFormat = format == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R32_UINT
				: format == VK_FORMAT_R8_UNORM ? VK_FORMAT_R8_UINT : VK_FORMAT_R16_UINT;
		VkImageViewUsageCreateInfo storageUsage;
		u32 level;
		memset(&storageUsage, 0, sizeof(storageUsage));
		storageUsage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
		storageUsage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
		for (level = 0; level < pi->levels; level++)
		{
			pi->storageViews[level] = make_view(pi->image, viewFormat, VK_IMAGE_ASPECT_COLOR_BIT, level, 1, NULL, &storageUsage);
			if (pi->storageViews[level] == VK_NULL_HANDLE)
			{
				free_image(pi);
				return NULL;
			}
		}
	}
	tex_pool_made(&p->book, &pi->entry, cls, tex_pool_bytes(cls, f == 3 || f == VK_TEX_POOL_TARGET ? 4 : f == 4 ? 1 : 2), late,
			poolFormatNames[f]);
	pi->all = p->all;
	p->all = pi;
	return pi;
}

void vk_tex_pool_init(vk_tex_pool *p, const vk_tex_decoder *decoder)
{
	static const u32 texel[VK_TEX_POOL_FORMATS] = { 2, 2, 2, 4, 1, 2, 2, 4 };
	/* how much games use them, in eighths of what they use 565 */
	static const u32 use[VK_TEX_POOL_FORMATS] = { 6, 8, 6, 4, 2, 1, 1, 0 };
	static const u8 kind[VK_TEX_POOL_FORMATS] = { 0, 0, 0, 0, 0, 0, 0, TEX_POOL_TARGETS };
	static const int types[3] = { TEX_TYPE_5551, TEX_TYPE_565, TEX_TYPE_4444 };
	static u32 counts[TEX_POOL_CLASSES];
	u32 have[VK_TEX_POOL_FORMATS];
	const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT
			| VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	u64 bytes;
	int f, c;
	u32 i;

	if (p->ready)
		return;
	p->decoder = decoder;
	tex_pool_reset(&p->book, TEX_POOL_MAX_FRAMES);
	for (f = 0; f < VK_TEX_POOL_FORMATS; f++)
	{
		VkFormatProperties props;
		p->formats[f] = poolFormats[f];
		vkGetPhysicalDeviceFormatProperties(vkc.gpu, p->formats[f], &props);
		p->usable[f] = (props.optimalTilingFeatures & needed) == needed;
		if (f == VK_TEX_POOL_TARGET)
		{
			p->usable[f] = p->usable[f] && (props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
			/* (the compute renderer writes them as storage images: vk_tex_pool_set_target_storage()) */
			if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
				p->targetStorage = false;
			p->targetUsage = BASE_TARGET_USAGE | (p->targetStorage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
		}
		/* the 16-bit ones the device cannot sample are converted to 32 bits */
		if (f < 3 && !vkc_format_supported(types[f]))
			p->usable[f] = false;
		if (f >= 5 && f != VK_TEX_POOL_TARGET)
			p->usable[f] = p->usable[f] && decoder != NULL
					&& vk_tex_decoder_native_format(decoder, f == 5 ? TEX_TYPE_5551 : TEX_TYPE_4444) == p->formats[f];
		p->storable[f] = f < 5 && f != VK_TEX_POOL_TARGET && decoder != NULL && decoder->ready
				&& vk_tex_decoder_takes(decoder, p->formats[f]);
		have[f] = p->usable[f] ? texel[f] : 0;
	}
	bytes = tex_pool_plan(&vkc.vramBudget, have, use, kind, VK_TEX_POOL_FORMATS, counts,
			texcache_upscale_factor(), (u32)settings.rend.MaxFilteredTextureSize,
			settings.rend.RenderToTextureUpscale > 1 ? (u32)settings.rend.RenderToTextureUpscale : 1u);
	p->ready = true;
	for (c = 0; c < TEX_POOL_CLASSES; c++)
		for (i = 0; i < counts[c]; i++)
		{
			vk_pooled_image *image = make(p, c, false);
			if (image == NULL)
				break;
			vk_tex_pool_give(p, image);
		}
	NOTICE_LOG(RENDERER, "Vulkan: texture pool of %u images, %u MB (planned %u MB)\n", p->book.made,
			(u32)(p->book.bytesMade >> 20), (u32)(bytes >> 20));
}

void vk_tex_pool_term(vk_tex_pool *p)
{
	if (!p->ready)
		return;
	tex_pool_summary(&p->book, "Vulkan");
	while (p->all != NULL)
	{
		vk_pooled_image *next = p->all->all;
		free_image(p->all);
		p->all = next;
	}
	tex_pool_reset(&p->book, TEX_POOL_MAX_FRAMES);
	p->ready = false;
	p->decoder = NULL;
}

vk_pooled_image *vk_tex_pool_take(vk_tex_pool *p, VkFormat format, u32 width, u32 height, bool mips)
{
	const int f = format_index(format);
	int cls;
	TexPoolEntry *e;
	if (!p->ready || f < 0 || !p->usable[f])
		return NULL;
	cls = tex_pool_class((u32)f, width, height, mips);
	if (cls < 0)
		return NULL;
	e = tex_pool_take(&p->book, cls);
	return e != NULL ? (vk_pooled_image *)e : make(p, cls, true);
}

vk_pooled_image *vk_tex_pool_take_target(vk_tex_pool *p, u32 width, u32 height, VkImageUsageFlags usage)
{
	int cls;
	TexPoolEntry *e;
	if (!p->ready || !p->usable[VK_TEX_POOL_TARGET] || (usage & p->targetUsage) != usage)
		return NULL;
	cls = tex_pool_class(VK_TEX_POOL_TARGET, width, height, false);
	if (cls < 0)
		return NULL;
	e = tex_pool_take(&p->book, cls);
	return e != NULL ? (vk_pooled_image *)e : make(p, cls, true);
}
