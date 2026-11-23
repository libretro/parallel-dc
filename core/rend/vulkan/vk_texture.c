#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
/* snprintf(), which a C89 compile of the C library's headers leaves out */
#if !defined(_MSC_VER) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
/*
 *  Created on: Oct 3, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <compat/msvc.h>
#include "vk_texture.h"
#include "rend/upload_layout.h"
#include "rend/texconv.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/ta_words.h"
#include "oslib/oslib.h"

vk_tex_decoder *vk_texture_decoder;
vk_tex_pool *vk_texture_pool;
u32 vk_texture_outside_cleared;

/* What an image is made for: written by the CPU as before; by the decoding
 * kernel; by a copy of the texture's texels as they are */
enum { MAKER_CPU, MAKER_KERNEL, MAKER_COPY };

static const texcache_ops vk_texture_ops;

#define TEX(t) ((vk_texture *)(t))

void vk_texture_construct(vk_texture *t)
{
	memset(t, 0, sizeof(*t));
	t->tc.ops = &vk_texture_ops;
	t->mipmapLevels = 1;
	t->decodeJob = -1;
}

/* the job this texture has with the decoder this frame is not done after
 * all: it is made another way, or goes */
static void cancel_decode(vk_texture *t)
{
	if (t->decodeJob >= 0 && vk_texture_decoder != NULL && t->decodeSerial == vk_texture_decoder->serial
			&& vk_tex_decoder_cancel(vk_texture_decoder, t->decodeJob))
		/* what was to make it from nothing did not: the next upload does */
		t->fresh = true;
	t->decodeJob = -1;
}

/* The pool's image given back: when the frames that may use it are done, or now */
static void release_pooled(vk_texture *t, bool now)
{
	if (t->pooled == NULL)
		return;
	cancel_decode(t);
	if (vk_texture_pool != NULL)
	{
		if (now)
			vk_tex_pool_give(vk_texture_pool, t->pooled);
		else
			vk_tex_pool_retire(vk_texture_pool, t->pooled);
	}
	t->pooled = NULL;
	t->storage = false;
}

/* its own image and views destroyed, its memory given back */
static void destroy_own(vk_texture *t)
{
	u32 i;
	for (i = 0; i < TEXD_MAX_LEVELS; i++)
		if (t->storageViews[i] != VK_NULL_HANDLE)
		{
			vkDestroyImageView(vkc.device, t->storageViews[i], NULL);
			t->storageViews[i] = VK_NULL_HANDLE;
		}
	if (t->imageView != VK_NULL_HANDLE)
		vkDestroyImageView(vkc.device, t->imageView, NULL);
	if (t->image != VK_NULL_HANDLE)
		vkDestroyImage(vkc.device, t->image, NULL);
	if (t->allocated)
		vk_heap_free(&vkc.heap, &t->alloc);
	t->imageView = VK_NULL_HANDLE;
	t->image = VK_NULL_HANDLE;
	t->allocated = false;
}

void vk_texture_destroy(vk_texture *t)
{
	cancel_decode(t);
	/* (a cache's texture goes when nothing has drawn with it for long, or
	 * when the renderer does: its image is free at once) */
	release_pooled(t, true);
	destroy_own(t);
}

static void begin(VkCommandBuffer cmd)
{
	VkCommandBufferBeginInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &info);
}

static void clear_color(VkCommandBuffer cmd, VkImage image, float a)
{
	VkClearColorValue color;
	VkImageSubresourceRange range;
	color.float32[0] = color.float32[1] = color.float32[2] = 0.f;
	color.float32[3] = a;
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
}

void vk_texture_clear_outside(vk_texture *t, VkCommandBuffer cmd, u32 width, u32 height, VkImageLayout *layout)
{
	const u32 area = width << 16 | height;
	const bool whole = width >= t->extent.width && height >= t->extent.height;
	if (!whole && (area != t->rttArea || *layout == VK_IMAGE_LAYOUT_UNDEFINED))
	{
		vk_set_image_layout(cmd, vk_texture_image(t), t->format, 1, *layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		clear_color(cmd, vk_texture_image(t), 1.f);
		*layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		vk_texture_outside_cleared++;
	}
	t->rttArea = area;
}

/* what it had goes, as create_image() lets it go */
static void drop_image(vk_texture *t)
{
	cancel_decode(t);
	release_pooled(t, false);
	destroy_own(t);
}

/* An image of its own, @tiling, @usage, made from @initialLayout; false
 * if the device could not make it, and it has none */
static bool create_image(vk_texture *t, VkImageTiling tiling, VkImageUsageFlags usage, VkImageLayout initialLayout,
		VkImageCreateFlags flags)
{
	static const VkComponentMapping swizzled = {
		VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_B
	};
	VkImageCreateInfo info;
	VkImageViewCreateInfo viewInfo;
	VkImageViewUsageCreateInfo viewUsage;

	/* the old image's storage views go with it */
	drop_image(t);
	t->storage = false;
	t->fresh = true;
	t->imageUsage = usage;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.flags = flags;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = t->format;
	info.extent.width = t->extent.width;
	info.extent.height = t->extent.height;
	info.extent.depth = 1;
	info.mipLevels = t->mipmapLevels;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = tiling;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = initialLayout;
	if (vkCreateImage(vkc.device, &info, NULL, &t->image) != VK_SUCCESS)
	{
		t->image = VK_NULL_HANDLE;
		ERROR_LOG(RENDERER, "Vulkan: a %ux%u texture image could not be made\n", t->extent.width, t->extent.height);
		return false;
	}
	if (!vkc_heap_alloc_image(t->image, tiling == VK_IMAGE_TILING_LINEAR, !t->needsStaging, false, &t->alloc))
	{
		ERROR_LOG(RENDERER, "Vulkan: no memory for a %ux%u texture\n", t->extent.width, t->extent.height);
		destroy_own(t);
		return false;
	}
	t->allocated = true;

	/* A 4444 texture copied as it is: the PowerVR has alpha, red, green and
	 * blue from the top, which this format calls blue, green, red and
	 * alpha. (Sampled only: the image's other uses are through views of
	 * other formats.) */
	memset(&viewInfo, 0, sizeof(viewInfo));
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = t->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = t->format;
	if (t->format == VK_FORMAT_B4G4R4A4_UNORM_PACK16)
		viewInfo.components = swizzled;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = t->mipmapLevels;
	viewInfo.subresourceRange.layerCount = 1;
	memset(&viewUsage, 0, sizeof(viewUsage));
	viewUsage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
	viewUsage.usage = usage & ~VK_IMAGE_USAGE_STORAGE_BIT;
	if (flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
		viewInfo.pNext = &viewUsage;
	if (vkCreateImageView(vkc.device, &viewInfo, NULL, &t->imageView) != VK_SUCCESS)
	{
		t->imageView = VK_NULL_HANDLE;
		destroy_own(t);
		return false;
	}
	t->ownSerial = vk_new_view_serial();
	return true;
}

bool vk_texture_create_render_target(vk_texture *t, VkImageUsageFlags usage)
{
	vk_pooled_image *pi = vk_texture_pool != NULL
			? vk_tex_pool_take_target(vk_texture_pool, t->extent.width, t->extent.height, usage) : NULL;
	if (pi == NULL)
		return create_image(t, VK_IMAGE_TILING_OPTIMAL, usage, VK_IMAGE_LAYOUT_UNDEFINED, 0);
	drop_image(t);
	t->pooled = pi;
	/* (a texture of the cache's, whose image is the pool's to take back) */
	t->cached = true;
	t->mipmapLevels = 1;
	t->imageUsage = pi->usage;
	t->storage = false;
	t->fresh = true;
	t->debugPath = VK_TEX_DBG_TARGET;
	t->debugLevels = 0;
	return true;
}

/* The image from the pool, for one of this shape; false if the pool has no
 * such shape */
static bool take_pooled(vk_texture *t, u32 width, u32 height, bool mipmapped)
{
	vk_pooled_image *pi;
	if (vk_texture_pool == NULL || !t->cached)
		return false;
	pi = vk_tex_pool_take(vk_texture_pool, t->format, width, height, mipmapped);
	if (pi == NULL)
		return false;
	drop_image(t);
	t->pooled = pi;
	t->mipmapLevels = pi->levels;
	t->imageUsage = pi->usage;
	t->storage = pi->storage;
	t->needsStaging = true;
	/* nothing in it is kept: whatever was drawn into it is another texture's */
	t->fresh = true;
	return true;
}

/* A texel of this texture's, in bytes */
static u32 texel_bytes(int type)
{
	return type == TEX_TYPE_8888 ? 4 : type == TEX_TYPE_8 ? 1 : 2;
}

/* The image's format for texels of @type */
static VkFormat type_format(int type)
{
	switch (type)
	{
	case TEX_TYPE_5551: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
	case TEX_TYPE_565: return VK_FORMAT_R5G6B5_UNORM_PACK16;
	case TEX_TYPE_4444: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
	case TEX_TYPE_8888: return VK_FORMAT_R8G8B8A8_UNORM;
	default: return VK_FORMAT_R8_UNORM;
	}
}

static VkImageView storage_view(const vk_texture *t, u32 level)
{
	return t->pooled != NULL ? t->pooled->storageViews[level] : t->storageViews[level];
}

static void init_image(vk_texture *t, u32 width, u32 height, VkFormat format, u32 dataSize, bool mipmapped,
		bool mipmapsIncluded, int maker)
{
	VkFormatProperties formatProperties;
	VkImageTiling tiling;
	VkImageLayout initialLayout;
	VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;

	t->extent.width = width;
	t->extent.height = height;
	t->format = format;
	t->mipmapLevels = 1;
	if (mipmapped)
		t->mipmapLevels += bitscanrev(width > height ? width : height);
	/* an image of the pool's, made before the game: optimal, written through a staging copy or by the GPU */
	if (take_pooled(t, width, height, mipmapped))
		return;

	if (maker != MAKER_CPU)
	{
		/* Optimal, written by the GPU, and copied to as well: a texture
		 * made here once can be converted on the CPU the next time */
		VkImageCreateFlags flags = 0;
		t->needsStaging = true;
		usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (maker == MAKER_KERNEL)
		{
			/* written through views of a format of integers of the same
			 * size: the 16- and 8-bit formats cannot be storage images
			 * themselves */
			usage |= VK_IMAGE_USAGE_STORAGE_BIT;
			flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
			if (format != VK_FORMAT_R8G8B8A8_UNORM)
				flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
		}
		if (!create_image(t, VK_IMAGE_TILING_OPTIMAL, usage, VK_IMAGE_LAYOUT_UNDEFINED, flags))
			return;
		if (maker == MAKER_KERNEL)
		{
			const VkFormat viewFormat = format == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R32_UINT
					: format == VK_FORMAT_R8_UNORM ? VK_FORMAT_R8_UINT : VK_FORMAT_R16_UINT;
			VkImageViewUsageCreateInfo viewUsage;
			u32 level;
			memset(&viewUsage, 0, sizeof(viewUsage));
			viewUsage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
			viewUsage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
			for (level = 0; level < t->mipmapLevels; level++)
			{
				VkImageViewCreateInfo viewInfo;
				memset(&viewInfo, 0, sizeof(viewInfo));
				viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
				viewInfo.pNext = &viewUsage;
				viewInfo.image = t->image;
				viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
				viewInfo.format = viewFormat;
				viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				viewInfo.subresourceRange.baseMipLevel = level;
				viewInfo.subresourceRange.levelCount = 1;
				viewInfo.subresourceRange.layerCount = 1;
				if (vkCreateImageView(vkc.device, &viewInfo, NULL, &t->storageViews[level]) != VK_SUCCESS)
				{
					t->storageViews[level] = VK_NULL_HANDLE;
					destroy_own(t);
					return;
				}
			}
			t->storage = true;
		}
		return;
	}
	vkGetPhysicalDeviceFormatProperties(vkc.gpu, format, &formatProperties);
	tiling = (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? VK_IMAGE_TILING_OPTIMAL
			: VK_IMAGE_TILING_LINEAR;
	if (height <= 32 && dataSize / height <= 64 && !mipmapped
			&& (formatProperties.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
		tiling = VK_IMAGE_TILING_LINEAR;
	t->needsStaging = tiling != VK_IMAGE_TILING_LINEAR;
	if (t->needsStaging)
	{
		usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	}
	else
	{
		verify((formatProperties.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0);
		initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	}
	if (mipmapped && !mipmapsIncluded)
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (t->presented)
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	create_image(t, tiling, usage, initialLayout, 0);
}

/* Makes the image what a texture of this size and kind needs, if it is not
 * already, for @maker to write; in format @native, if one is given.
 * Returns whether it was made anew. */
static bool prepare(vk_texture *t, int width, int height, bool mipmapped, bool mipmapsIncluded, int maker, VkFormat native)
{
	VkFormat format = VK_FORMAT_UNDEFINED;
	t->rttArea = 0;	/* (written other than by a render: vk_texture_clear_outside()) */
	switch (t->tc.tex_type)
	{
	case TEX_TYPE_5551: format = VK_FORMAT_R5G5B5A1_UNORM_PACK16; break;
	case TEX_TYPE_565: format = VK_FORMAT_R5G6B5_UNORM_PACK16; break;
	case TEX_TYPE_4444: format = VK_FORMAT_R4G4B4A4_UNORM_PACK16; break;
	case TEX_TYPE_8888: format = VK_FORMAT_R8G8B8A8_UNORM; break;
	case TEX_TYPE_8: format = VK_FORMAT_R8_UNORM; break;
	}
	if (native != VK_FORMAT_UNDEFINED)
		format = native;
	if (width == (int)t->extent.width && height == (int)t->extent.height && format == t->format
			/* an image made for the CPU to write where it is, or not for the kernel */
			&& (maker == MAKER_CPU || t->needsStaging) && (maker != MAKER_KERNEL || t->storage)
			&& ((t->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !t->needsStaging))
		return false;
	init_image(t, width, height, format, width * height * texel_bytes(t->tc.tex_type), mipmapped, mipmapsIncluded, maker);
	return true;
}

/* The kernel's, or a copy of the texels as they are: see tex_decoder.h.
 * Everything that can say no does before anything is changed. */
static bool tex_gpu_decode(texcache_data *tc, const struct texd_plan *plan)
{
	vk_texture *t = TEX(tc);
	vk_tex_decoder *d = vk_texture_decoder;
	VkFormat nativeFormat;
	bool copy;
	u32 bytes, offset, i;
	u8 *to;

	if (d == NULL || !d->ready)
		return false;
	nativeFormat = plan->native ? vk_tex_decoder_native_format(d, t->tc.tex_type) : VK_FORMAT_UNDEFINED;
	copy = nativeFormat != VK_FORMAT_UNDEFINED;
	if (!copy && !vk_tex_decoder_takes(d, type_format(t->tc.tex_type)))
		return false;
	bytes = plan->pal + plan->pal_entries * 4;
	if (!vk_tex_decoder_room(d, bytes, copy ? 0 : plan->levels))
		return false;

	cancel_decode(t);
	t->prepared = false;
	prepare(t, plan->width, plan->height, texcache_is_mipmapped(&t->tc), plan->mipmapped != 0, copy ? MAKER_COPY : MAKER_KERNEL,
			nativeFormat);
	if (vk_texture_image(t) == VK_NULL_HANDLE)
		return false;
	to = vk_tex_decoder_reserve(d, bytes, &offset);
	memcpy(to, &vram.data[plan->src], plan->bytes);
	if (plan->pal_entries != 0)
		memcpy(to + plan->pal, (plan->out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan->pal_index, plan->pal_entries * 4);

	if (copy)
		t->decodeJob = vk_tex_decoder_add_copy(d, vk_texture_image(t), t->fresh, offset, plan->level[0].sr & 0xFFFF,
				plan->width, plan->height, 0);
	else
	{
		vk_texd_level levels[TEXD_MAX_LEVELS];
		for (i = 0; i < plan->levels; i++)
		{
			const struct texd_level *lv = &plan->level[i];
			levels[i].view = storage_view(t, lv->mip);
			levels[i].pc[0] = offset / 4;
			levels[i].pc[1] = lv->data;
			levels[i].pc[2] = plan->pal;
			levels[i].pc[3] = lv->wh;
			levels[i].pc[4] = lv->sr;
			levels[i].pc[5] = lv->mode;
			levels[i].group[0] = 0;
			levels[i].group[1] = 0;
			levels[i].group[2] = ((lv->wh & 0xFFFF) + 7) / 8;
			levels[i].group[3] = ((lv->wh >> 16) + 7) / 8;
		}
		t->decodeJob = vk_tex_decoder_add_decode(d, vk_texture_image(t), t->fresh, t->mipmapLevels, plan->out, levels, plan->levels);
	}
	t->decodeSerial = d->serial;
	t->fresh = false;
	t->debugPath = copy ? VK_TEX_DBG_COPY : VK_TEX_DBG_KERNEL;
	t->debugLevels = 0;
	return true;
}

/* A copy, or a blit where the render is finer, from the render's image -
 * one that can be copied from, sampled where it is - in the commands the
 * texture's uploads go in, which the GPU runs after the render's. */
static bool tex_copy_render(texcache_data *tc, texcache_data *fromTc, u32 x, u32 y, u32 scale, u32 columns)
{
	vk_texture *t = TEX(tc);
	const vk_texture *src = TEX(fromTc);
	const VkFormat rgba = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageSubresourceLayers layers;
	VkCommandBuffer cmd;
	bool isNew;

	t->rttArea = 0;	/* (written other than by a render: vk_texture_clear_outside()) */
	if (t->commandPool == NULL || vk_texture_is_new(src) || src->format != rgba || !(src->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
			|| (x + columns) * scale > src->extent.width || (y + t->tc.h) * scale > src->extent.height)
		return false;
	cancel_decode(t);
	t->prepared = false;
	t->tc.tex_type = TEX_TYPE_8888;
	isNew = prepare(t, t->tc.w, t->tc.h, false, false, MAKER_COPY, rgba) || t->fresh;
	if (!(t->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || vk_texture_image(t) == VK_NULL_HANDLE)
		return false;
	if (t->commandBuffer == VK_NULL_HANDLE)
		t->commandBuffer = vk_cmd_pool_allocate(t->commandPool);
	cmd = t->commandBuffer;
	begin(cmd);
	vk_set_image_layout(cmd, vk_texture_image(src), rgba, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vk_set_image_layout(cmd, vk_texture_image(t), rgba, t->mipmapLevels,
			isNew ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	memset(&layers, 0, sizeof(layers));
	layers.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	layers.layerCount = 1;
	if (columns < t->tc.w)
	{
		/* (the rest of each row, as a texture read through a narrower stride has it) */
		VkMemoryBarrier barrier;
		clear_color(cmd, vk_texture_image(t), texcache_clear_opaque(&t->tc) ? 1.f : 0.f);
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	}
	if (scale <= 1)
	{
		VkImageCopy region;
		memset(&region, 0, sizeof(region));
		region.srcSubresource = layers;
		region.srcOffset.x = (int32_t)x;
		region.srcOffset.y = (int32_t)y;
		region.dstSubresource = layers;
		region.extent.width = columns;
		region.extent.height = t->tc.h;
		region.extent.depth = 1;
		vkCmdCopyImage(cmd, vk_texture_image(src), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_texture_image(t),
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}
	else
	{
		VkImageBlit blit;
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource = layers;
		blit.srcOffsets[0].x = (int32_t)(x * scale);
		blit.srcOffsets[0].y = (int32_t)(y * scale);
		blit.srcOffsets[1].x = (int32_t)((x + columns) * scale);
		blit.srcOffsets[1].y = (int32_t)((y + t->tc.h) * scale);
		blit.srcOffsets[1].z = 1;
		blit.dstSubresource = layers;
		blit.dstOffsets[1].x = (int32_t)columns;
		blit.dstOffsets[1].y = (int32_t)t->tc.h;
		blit.dstOffsets[1].z = 1;
		vkCmdBlitImage(cmd, vk_texture_image(src), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_texture_image(t),
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
	}
	vk_set_image_layout(cmd, vk_texture_image(t), rgba, t->mipmapLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vk_set_image_layout(cmd, vk_texture_image(src), rgba, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkEndCommandBuffer(cmd);
	t->fresh = false;
	t->debugPath = VK_TEX_DBG_COPY;
	t->debugLevels = 0;
	return true;
}

/* The image the last whole update made, as it made it (in @format), and
 * nothing for it still to be done this frame */
static bool part_ready(const vk_texture *t, VkFormat format)
{
	const u32 levels = t->tc.made_mips ? 1 + bitscanrev(t->tc.w) : 1;
	if (t->tc.made_type != (u8)t->tc.tex_type + 1 || vk_texture_is_new(t) || t->fresh || t->format != format
			|| t->extent.width != t->tc.w || t->extent.height != t->tc.h || t->mipmapLevels != levels
			|| t->debugPath == VK_TEX_DBG_TARGET)
		return false;
	return t->decodeJob < 0 || vk_texture_decoder == NULL || t->decodeSerial != vk_texture_decoder->serial;
}

static bool tex_part_memory(texcache_data *tc, size_t bytes, void **memory)
{
	vk_texture *t = TEX(tc);
	if (!t->needsStaging || !(t->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !part_ready(t, type_format(t->tc.tex_type)))
		return false;
	t->partSlot = vk_upload_ring_allocate(vk_upload_ring_get(), bytes, &t->partBuffer, &t->partOffset);
	*memory = t->partSlot;
	return t->partSlot != NULL;
}

static void tex_upload_part(texcache_data *tc, const TexRegion *regions, u32 count, u8 *data, const size_t *offset)
{
	vk_texture *t = TEX(tc);
	VkBufferImageCopy copies[TEXP_REGIONS];
	u32 i;
	(void)data;

	t->rttArea = 0;	/* (written other than by a render: vk_texture_clear_outside()) */
	if (t->commandBuffer == VK_NULL_HANDLE && t->commandPool != NULL)
		t->commandBuffer = vk_cmd_pool_allocate(t->commandPool);
	verify(t->commandBuffer != VK_NULL_HANDLE);
	memset(copies, 0, sizeof(copies));
	for (i = 0; i < count; i++)
	{
		copies[i].bufferOffset = t->partOffset + offset[i];
		copies[i].bufferRowLength = regions[i].w;
		copies[i].bufferImageHeight = regions[i].h;
		copies[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copies[i].imageSubresource.mipLevel = regions[i].level;
		copies[i].imageSubresource.layerCount = 1;
		copies[i].imageOffset.x = (int32_t)regions[i].x;
		copies[i].imageOffset.y = (int32_t)regions[i].y;
		copies[i].imageExtent.width = regions[i].w;
		copies[i].imageExtent.height = regions[i].h;
		copies[i].imageExtent.depth = 1;
	}
	/* (what the image has is kept: from what it is sampled as) */
	begin(t->commandBuffer);
	vk_set_image_layout(t->commandBuffer, vk_texture_image(t), t->format, t->mipmapLevels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdCopyBufferToImage(t->commandBuffer, t->partBuffer, vk_texture_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, copies);
	vk_set_image_layout(t->commandBuffer, vk_texture_image(t), t->format, t->mipmapLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkEndCommandBuffer(t->commandBuffer);
	t->partSlot = NULL;
	t->debugPath = VK_TEX_DBG_STAGED;
	t->debugLevels = 0;
}

static int tex_gpu_decode_part(texcache_data *tc, const struct texd_plan *plan, const TexRegion *regions, u32 count)
{
	vk_texture *t = TEX(tc);
	vk_tex_decoder *d = vk_texture_decoder;
	VkFormat nativeFormat, format;
	bool copy;
	u8 *to;
	u32 offset, i;

	t->rttArea = 0;	/* (written other than by a render: vk_texture_clear_outside()) */
	if (d == NULL || !d->ready)
		return TEXP_CPU;
	nativeFormat = plan->native ? vk_tex_decoder_native_format(d, t->tc.tex_type) : VK_FORMAT_UNDEFINED;
	copy = nativeFormat != VK_FORMAT_UNDEFINED;
	format = copy ? nativeFormat : type_format(t->tc.tex_type);
	if (!copy && !vk_tex_decoder_takes(d, format))
		return TEXP_CPU;
	/* an image the whole update made otherwise is made in part as that was */
	if (!part_ready(t, format))
		return part_ready(t, type_format(t->tc.tex_type)) ? TEXP_CPU : TEXP_WHOLE;
	if (copy ? !(t->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) : storage_view(t, 0) == VK_NULL_HANDLE || !vk_tex_decoder_runs_part(d))
		return TEXP_CPU;

	if (copy)
	{
		/* the rows, as they are: from the first region's to the last's, in one copy */
		const u32 stride = plan->level[0].sr & 0xFFFF;
		u32 y0 = plan->height, y1 = 0, bytes;
		for (i = 0; i < count; i++)
		{
			if (regions[i].y < y0)
				y0 = regions[i].y;
			if (regions[i].y + regions[i].h > y1)
				y1 = regions[i].y + regions[i].h;
		}
		bytes = (y1 - y0) * stride * 2;
		if (y0 >= y1 || !vk_tex_decoder_room(d, bytes, 0))
			return TEXP_CPU;
		to = vk_tex_decoder_reserve(d, bytes, &offset);
		memcpy(to, &vram.data[plan->src + plan->level[0].data + y0 * stride * 2], bytes);
		t->decodeJob = vk_tex_decoder_add_copy(d, vk_texture_image(t), false, offset, stride, plan->width, y1 - y0, y0);
	}
	else
	{
		vk_texd_level levels[TEXP_REGIONS];
		TexPartPlan part;
		if (!tex_plan_part(plan, regions, count, &part) || !vk_tex_decoder_room(d, part.total, count))
			return TEXP_CPU;
		to = vk_tex_decoder_reserve(d, part.total, &offset);
		memcpy(to, &vram.data[plan->src], part.codebook);
		for (i = 0; i < count; i++)
			memcpy(to + part.at[i], &vram.data[plan->src + part.from[i]], part.bytes[i]);
		if (plan->pal_entries != 0)
			memcpy(to + part.pal, (plan->out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan->pal_index, plan->pal_entries * 4);
		for (i = 0; i < count; i++)
		{
			const struct texd_level *lv = &plan->level[part.level[i]];
			levels[i].view = storage_view(t, lv->mip);
			levels[i].pc[0] = offset / 4;
			levels[i].pc[1] = part.data[i];
			levels[i].pc[2] = part.pal;
			levels[i].pc[3] = lv->wh;
			levels[i].pc[4] = lv->sr;
			levels[i].pc[5] = lv->mode;
			levels[i].group[0] = regions[i].x / 8;
			levels[i].group[1] = regions[i].y / 8;
			levels[i].group[2] = (regions[i].w + 7) / 8;
			levels[i].group[3] = (regions[i].h + 7) / 8;
		}
		t->decodeJob = vk_tex_decoder_add_decode(d, vk_texture_image(t), false, t->mipmapLevels, plan->out, levels, count);
	}
	t->decodeSerial = d->serial;
	t->debugPath = copy ? VK_TEX_DBG_COPY : VK_TEX_DBG_KERNEL;
	t->debugLevels = 0;
	return TEXP_DONE;
}

void *vk_texture_upload_memory(vk_texture *t, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	u32 pad;
	cancel_decode(t);
	t->prepared = true;
	t->preparedNew = prepare(t, width, height, texcache_is_mipmapped(&t->tc), mipmaps, MAKER_CPU, VK_FORMAT_UNDEFINED);
	if (!t->needsStaging)
		/* a small image the texture is written into where it is, at the pitch it has */
		return NULL;
	pad = mipmaps && t->mipmapLevels > 1 ? upload_levels_pad(pixel) : 0;
	t->slot = vk_upload_ring_allocate(vk_upload_ring_get(), pad + bytes, &t->slotBuffer, &t->slotOffset);
	/* (no room: converted into the cache's memory, and set_image() finds none either) */
	return t->slot != NULL ? t->slot + pad : NULL;
}

/* No room in the upload ring: the texture is made with the next frame, and
 * until then its image is left as it is */
static void defer(vk_texture *t, bool isNew)
{
	if (isNew)
	{
		/* (an image never written is sampled all the same: it is made one
		 * that can be, whatever is in it) */
		begin(t->commandBuffer);
		vk_set_image_layout(t->commandBuffer, vk_texture_image(t), t->format, t->mipmapLevels, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vkEndCommandBuffer(t->commandBuffer);
	}
	t->fresh = false;
	t->tc.made_type = 0;
	if (t->cached)
		t->tc.dirty = FrameCount != 0 ? FrameCount : 1;
}

/* The texture debug log's checksum of @size bytes (FNV-1a) */
static u32 debug_sum(const u8 *p, u32 size)
{
	u32 h = 2166136261u, i;
	for (i = 0; i < size; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

static void image_barrier(VkCommandBuffer cmd, VkImageMemoryBarrier *b, VkImageLayout from, VkImageLayout to,
		VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
	b->oldLayout = from;
	b->newLayout = to;
	b->srcAccessMask = srcAccess;
	b->dstAccessMask = dstAccess;
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, b);
}

static void generate_mipmaps(vk_texture *t)
{
	VkCommandBuffer cmd = t->commandBuffer;
	u32 mipWidth = t->extent.width, mipHeight = t->extent.height, i;
	VkImageMemoryBarrier barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = vk_texture_image(t);
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;

	for (i = 1; i < t->mipmapLevels; i++)
	{
		VkImageBlit blit;
		const u32 nextWidth = mipWidth / 2 > 1 ? mipWidth / 2 : 1, nextHeight = mipHeight / 2 > 1 ? mipHeight / 2 : 1;
		/* the level before, from what it was written as to what it is blitted from */
		barrier.subresourceRange.baseMipLevel = i - 1;
		if (i == 1 && !t->needsStaging)
			image_barrier(cmd, &barrier, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		else
			image_barrier(cmd, &barrier, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		/* the level before blitted onto this one */
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.mipLevel = i - 1;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[1].x = (int32_t)mipWidth;
		blit.srcOffsets[1].y = (int32_t)mipHeight;
		blit.srcOffsets[1].z = 1;
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.mipLevel = i;
		blit.dstSubresource.layerCount = 1;
		blit.dstOffsets[1].x = (int32_t)nextWidth;
		blit.dstOffsets[1].y = (int32_t)nextHeight;
		blit.dstOffsets[1].z = 1;
		vkCmdBlitImage(cmd, vk_texture_image(t), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_texture_image(t),
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

		/* and the level before to be sampled */
		image_barrier(cmd, &barrier, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		mipWidth = nextWidth;
		mipHeight = nextHeight;
	}
	/* the last level, from what it was written as to be sampled */
	barrier.subresourceRange.baseMipLevel = t->mipmapLevels - 1;
	image_barrier(cmd, &barrier, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

static void set_image(vk_texture *t, u32 srcSize, void *srcData, bool isNew, bool genMipmaps)
{
	VkCommandBuffer cmd;
	t->rttArea = 0;	/* (written other than by a render: vk_texture_clear_outside()) */
	if (vk_texture_image(t) == VK_NULL_HANDLE)
		return;
	if (t->commandBuffer == VK_NULL_HANDLE && t->commandPool != NULL)
		t->commandBuffer = vk_cmd_pool_allocate(t->commandPool);
	verify(t->commandBuffer != VK_NULL_HANDLE);
	cmd = t->commandBuffer;
	isNew = isNew || t->fresh;
	t->debugLevels = 0;
	if (t->needsStaging)
	{
		const u32 pixel = texel_bytes(t->tc.tex_type);
		const u32 pad = t->mipmapLevels > 1 && !genMipmaps ? upload_levels_pad(pixel) : 0;
		/* Converted into its piece of the ring already - or, for what was
		 * not (a custom texture, one scaled up, the renderer's own small
		 * textures, one there was no room for), a piece for it now, and the
		 * one copy. None, and it waits for the next frame. */
		if (t->slot == NULL || (u8 *)srcData != t->slot + pad)
		{
			t->slot = vk_upload_ring_allocate(vk_upload_ring_get(), pad + srcSize, &t->slotBuffer, &t->slotOffset);
			if (t->slot == NULL)
			{
				defer(t, isNew);
				return;
			}
			memcpy(t->slot + pad, srcData, srcSize);
		}
	}
	t->fresh = false;
	begin(cmd);

	if (!isNew && !t->needsStaging)
		vk_set_image_layout(cmd, vk_texture_image(t), t->format, t->mipmapLevels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_IMAGE_LAYOUT_GENERAL);

	if (t->needsStaging)
	{
		const u32 pixel = texel_bytes(t->tc.tex_type);
		const bool levels = t->mipmapLevels > 1 && !genMipmaps;
		const u32 pad = levels ? upload_levels_pad(pixel) : 0;
		VkBufferImageCopy region;
		if (pad != 0)
			/* the 1x1 level, to where a copy can start: see upload_layout.h */
			memcpy(t->slot, t->slot + pad, pixel);

		/* to be copied to */
		vk_set_image_layout(cmd, vk_texture_image(t), t->format, t->mipmapLevels,
				isNew ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		memset(&region, 0, sizeof(region));
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.depth = 1;
		if (levels)
		{
			u32 i;
			for (i = 0; i < t->mipmapLevels; i++)
			{
				region.bufferOffset = t->slotOffset + upload_level_at(pixel, i);
				region.bufferRowLength = region.bufferImageHeight = 1u << i;
				region.imageSubresource.mipLevel = t->mipmapLevels - i - 1;
				region.imageExtent.width = region.imageExtent.height = 1u << i;
				vkCmdCopyBufferToImage(cmd, t->slotBuffer, vk_texture_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
				if (settings.rend.TextureDebug)
					t->debugSum[t->mipmapLevels - i - 1] = debug_sum(t->slot + upload_level_at(pixel, i), (1u << i) * (1u << i) * pixel);
			}
			t->debugLevels = settings.rend.TextureDebug ? (u8)t->mipmapLevels : 0;
		}
		else
		{
			if (settings.rend.TextureDebug)
			{
				t->debugSum[0] = debug_sum(t->slot, t->extent.width * t->extent.height * pixel);
				t->debugLevels = 1;
			}
			region.bufferOffset = t->slotOffset;
			region.bufferRowLength = t->extent.width;
			region.bufferImageHeight = t->extent.height;
			region.imageExtent.width = t->extent.width;
			region.imageExtent.height = t->extent.height;
			vkCmdCopyBufferToImage(cmd, t->slotBuffer, vk_texture_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		}
		if (t->mipmapLevels > 1 && !levels)
			/* (every level left for the shaders to read) */
			generate_mipmaps(t);
		else
			vk_set_image_layout(cmd, vk_texture_image(t), t->format, t->mipmapLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		t->debugPath = VK_TEX_DBG_STAGED;
	}
	else
	{
		u8 *data = (u8 *)t->alloc.mapped;
		VkImageSubresource subresource;
		VkSubresourceLayout layout;
		verify(data != NULL);
		subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresource.mipLevel = 0;
		subresource.arrayLayer = 0;
		vkGetImageSubresourceLayout(vkc.device, vk_texture_image(t), &subresource, &layout);
		if (layout.size != srcSize)
		{
			const u8 *src = (const u8 *)srcData;
			u32 rowBytes = t->extent.width * 2, y;
			if (t->tc.tex_type == TEX_TYPE_8888)
				rowBytes *= 2;
			else if (t->tc.tex_type == TEX_TYPE_8)
				rowBytes /= 2;
			for (y = 0; y < t->extent.height; y++, src += rowBytes, data += layout.rowPitch)
				memcpy(data, src, rowBytes);
		}
		else
			memcpy(data, srcData, srcSize);
		t->debugPath = VK_TEX_DBG_LINEAR;
		t->debugLevels = 0;
		if (t->mipmapLevels > 1)
			generate_mipmaps(t);
		else
			/* the linear image sampled as it is */
			vk_set_image_layout(cmd, vk_texture_image(t), t->format, t->mipmapLevels,
					isNew ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	vkEndCommandBuffer(cmd);
}

void vk_texture_upload_to_gpu(vk_texture *t, int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded)
{
	const u32 pixel = texel_bytes(t->tc.tex_type);
	/* the texels as they are handed over: the levels after one another, or the one image */
	u32 dataSize = width * height * pixel;
	if (mipmapsIncluded)
	{
		int w;
		for (w = width / 2; w != 0; w /= 2)
			dataSize += w * w * pixel;
	}
	if (!t->prepared)
	{
		cancel_decode(t);
		t->preparedNew = prepare(t, width, height, mipmapped, mipmapsIncluded, MAKER_CPU, VK_FORMAT_UNDEFINED);
	}
	t->prepared = false;
	set_image(t, dataSize, data, t->preparedNew, mipmapped && !mipmapsIncluded);
	t->slot = NULL;
}

/* ---- The texture cache's calls ---- */

static void tex_get_id(texcache_data *tc, char *s, size_t n)
{
	snprintf(s, n, "%p", (void *)tc);
}

static void tex_upload_to_gpu(texcache_data *tc, int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded)
{
	vk_texture_upload_to_gpu(TEX(tc), width, height, data, mipmapped, mipmapsIncluded);
}

static void *tex_upload_memory(texcache_data *tc, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	return vk_texture_upload_memory(TEX(tc), width, height, bytes, pixel, mipmaps);
}

static bool tex_force_32bit(const texcache_data *tc, int type)
{
	(void)tc;
	return !vkc_format_supported(type);
}

static bool tex_delete(texcache_data *tc)
{
	return texcache_delete(tc);
}

static const texcache_ops vk_texture_ops = {
	tex_get_id,
	tex_upload_to_gpu,
	tex_upload_memory,
	tex_gpu_decode,
	tex_part_memory,
	tex_upload_part,
	tex_gpu_decode_part,
	tex_copy_render,
	tex_force_32bit,
	tex_delete
};

/* ---- The cache ---- */

typedef struct vk_texcache_list
{
	grow_buf_t items;
	u32 count;
} vk_texcache_list;

/* an image of a texture's own, retired until its frames are done */
typedef struct vk_trash
{
	VkImage image;
	VkImageView views[1 + TEXD_MAX_LEVELS];
	vk_heap_alloc_t alloc;
	bool allocated;
} vk_trash;

/* (a swap chain has a few images: past 31 places, which none has, the last bit is shared) */
static u32 slot_bit(u32 s)
{
	return s < 32 ? 1u << s : 0x80000000u;
}

static vk_texcache_list *in_flight(const vk_texcache *c, u32 s)
{
	return (vk_texcache_list *)c->inFlightLists.data + s;
}

/* @n places at least, the new ones empty */
static bool grow_lists(grow_buf_t *lists, u32 *count, u32 n)
{
	if (n <= *count)
		return true;
	if (grow_buf_reserve_keep(lists, n * sizeof(vk_texcache_list)) == NULL)
		return false;
	memset((vk_texcache_list *)lists->data + *count, 0, (n - *count) * sizeof(vk_texcache_list));
	*count = n;
	return true;
}

static void empty_in_flight(vk_texcache *c, u32 s)
{
	vk_texcache_list *l = in_flight(c, s);
	vk_texture **list = (vk_texture **)l->items.data;
	u32 i;
	for (i = 0; i < l->count; i++)
		if (list[i] != NULL)
			list[i]->inFlightSlots &= ~slot_bit(s);
	l->count = 0;
}

static void empty_all_in_flight(vk_texcache *c)
{
	u32 s;
	for (s = 0; s < c->inFlightCount; s++)
		empty_in_flight(c, s);
}

static void destroy_trash(vk_texcache_list *l)
{
	vk_trash *items = (vk_trash *)l->items.data;
	u32 i, k;
	for (i = 0; i < l->count; i++)
	{
		for (k = 0; k < 1 + TEXD_MAX_LEVELS; k++)
			if (items[i].views[k] != VK_NULL_HANDLE)
				vkDestroyImageView(vkc.device, items[i].views[k], NULL);
		if (items[i].image != VK_NULL_HANDLE)
			vkDestroyImage(vkc.device, items[i].image, NULL);
		if (items[i].allocated)
			vk_heap_free(&vkc.heap, &items[i].alloc);
	}
	l->count = 0;
}

static void free_lists(grow_buf_t *lists, u32 count)
{
	u32 i;
	for (i = 0; i < count; i++)
		grow_buf_free(&((vk_texcache_list *)lists->data)[i].items);
	grow_buf_free(lists);
}

/* (the texture going: what it holds given back, as a texture's own end) */
static void tex_make(void *e)
{
	vk_texture_construct((vk_texture *)e);
	((vk_texture *)e)->cached = false;
}

static void tex_unmake(void *e)
{
	vk_texture_destroy((vk_texture *)e);
}

void vk_texcache_construct(vk_texcache *c)
{
	memset(c, 0, sizeof(*c));
	tex_map_init(&c->map, sizeof(vk_texture), tex_make, tex_unmake);
	c->map.name = "Texture cache";
}

void vk_texcache_free(vk_texcache *c)
{
	u32 s;
	for (s = 0; s < c->trashCount; s++)
		destroy_trash((vk_texcache_list *)c->trashLists.data + s);
	free_lists(&c->trashLists, c->trashCount);
	free_lists(&c->inFlightLists, c->inFlightCount);
	c->trashCount = c->inFlightCount = 0;
	tex_map_free(&c->map);
}

vk_texture *vk_texcache_get(vk_texcache *c, u32 tsp, u32 tcw, int area)
{
	return (vk_texture *)texcache_map_get(&c->map, NULL, tsp, tcw, area);
}

void vk_texcache_reserve_for(vk_texcache *c, u32 images)
{
	texcache_map_reserve_for(&c->map, images);
}

/* The texture debug log (settings.rend.TextureDebug), for finding out on a
 * machine where a texture is drawn wrong what the textures and their
 * samplers are there. About every ten seconds, a line for each texture of
 * the frame drawn last: what the game asked for, the image made of it, how
 * it was written, the sampler it is drawn with, and each level of the image
 * read back from the GPU with its checksum, beside the checksum of what the
 * CPU uploaded into that level (the GPU's own decode and generated levels
 * have none). The read back waits for the GPU, and the buffer for it is made
 * each time: this is a diagnostic, off unless asked for. */
static void debug_log(vk_texture *const *textures, u32 count);

void vk_texcache_set_current_index(vk_texcache *c, int index)
{
	vk_texcache_list *l;
	if (c->currentIndex < c->inFlightCount)
	{
		vk_texture **list = (vk_texture **)in_flight(c, c->currentIndex)->items.data;
		const u32 n = in_flight(c, c->currentIndex)->count;
		u32 i;
		if (settings.rend.TextureDebug)
			debug_log(list, n);
		for (i = 0; i < n; i++)
			if (list[i] != NULL)
				list[i]->readOnlyImageView = VK_NULL_HANDLE;
	}
	c->currentIndex = (u32)index;
	vk_upload_ring_set_current_index(vk_upload_ring_get(), index);
	if (vk_texture_pool != NULL)
		vk_tex_pool_frame(vk_texture_pool, (u32)index);
	/* the textures this place had: in no frame now but those after it */
	if (grow_lists(&c->inFlightLists, &c->inFlightCount, c->currentIndex + 1))
		empty_in_flight(c, c->currentIndex);
	/* (each frame's list with room for a game's textures of its own, those
	 * no image of the pool's has the shape of, retired in a frame: made
	 * with the frame's place, before the first, and kept) */
	if (grow_lists(&c->trashLists, &c->trashCount, c->currentIndex + 1))
	{
		l = (vk_texcache_list *)c->trashLists.data + c->currentIndex;
		destroy_trash(l);
		grow_buf_reserve_keep(&l->items, 64 * sizeof(vk_trash));
	}
}

bool vk_texcache_is_in_flight(const vk_texcache *c, const vk_texture *t)
{
	return (t->inFlightSlots & ~slot_bit(c->currentIndex)) != 0;
}

void vk_texcache_set_in_flight(vk_texcache *c, vk_texture *t)
{
	const u32 bit = slot_bit(c->currentIndex);
	vk_texcache_list *l;
	vk_texture **list;
	if ((t->inFlightSlots & bit) != 0 || !grow_lists(&c->inFlightLists, &c->inFlightCount, c->currentIndex + 1))
		return;
	l = in_flight(c, c->currentIndex);
	list = (vk_texture **)grow_buf_reserve_keep(&l->items, (l->count + 1) * sizeof(vk_texture *));
	if (list == NULL)
		return;
	list[l->count++] = t;
	t->inFlightSlots |= bit;
}

void vk_texcache_destroy_later(vk_texcache *c, vk_texture *t)
{
	vk_texcache_list *l;
	vk_trash *items;
	u32 i, v = 0;
	if (t->pooled != NULL)
	{
		release_pooled(t, false);
		t->format = VK_FORMAT_UNDEFINED;
		return;
	}
	if (t->image == VK_NULL_HANDLE)
		return;
	cancel_decode(t);
	items = grow_lists(&c->trashLists, &c->trashCount, c->currentIndex + 1)
			? (vk_trash *)grow_buf_reserve_keep(&((vk_texcache_list *)c->trashLists.data + c->currentIndex)->items,
					(((vk_texcache_list *)c->trashLists.data + c->currentIndex)->count + 1) * sizeof(vk_trash))
			: NULL;
	if (items == NULL)
	{
		/* (no room to keep it: the queue waited out, and destroyed now) */
		vkc_wait_queue_idle();
		destroy_own(t);
	}
	else
	{
		l = (vk_texcache_list *)c->trashLists.data + c->currentIndex;
		memset(&items[l->count], 0, sizeof(vk_trash));
		items[l->count].image = t->image;
		items[l->count].views[v++] = t->imageView;
		if (t->storage)
			for (i = 0; i < TEXD_MAX_LEVELS; i++)
				if (t->storageViews[i] != VK_NULL_HANDLE)
					items[l->count].views[v++] = t->storageViews[i];
		items[l->count].alloc = t->alloc;
		items[l->count].allocated = t->allocated;
		l->count++;
		/* (what storage views it had and was not made for go now: none is in a set) */
		for (i = 0; i < TEXD_MAX_LEVELS; i++)
			if (!t->storage && t->storageViews[i] != VK_NULL_HANDLE)
				vkDestroyImageView(vkc.device, t->storageViews[i], NULL);
		for (i = 0; i < TEXD_MAX_LEVELS; i++)
			t->storageViews[i] = VK_NULL_HANDLE;
		t->image = VK_NULL_HANDLE;
		t->imageView = VK_NULL_HANDLE;
		t->allocated = false;
	}
	t->storage = false;
	t->format = VK_FORMAT_UNDEFINED;
}

/* out of the lists that have it (a texture gone is not seen there again),
 * and deleted: false while the custom texture loader has it */
static bool clear_texture(vk_texcache *c, vk_texture *t)
{
	u32 s, i;
	for (s = 0; s < c->inFlightCount && t->inFlightSlots != 0; s++)
	{
		vk_texture **list;
		u32 n;
		if ((t->inFlightSlots & slot_bit(s)) == 0)
			continue;
		list = (vk_texture **)in_flight(c, s)->items.data;
		n = in_flight(c, s)->count;
		for (i = 0; i < n; i++)
			if (list[i] == t)
				list[i] = NULL;
		t->inFlightSlots &= ~slot_bit(s);
	}
	return texcache_delete(&t->tc);
}

void vk_texcache_cleanup(vk_texcache *c)
{
	/* (a few a frame, those not used for 120 frames) */
	const u32 targetFrame = (FrameCount > 120 ? FrameCount : 120) - 120;
	u64 list[6], key;
	u32 n = 0, e, i;
	for (e = 0; e < tex_map_places(&c->map) && n < 6; e++)
	{
		vk_texture *t = (vk_texture *)tex_map_at(&c->map, e, &key);
		if (t != NULL && t->tc.dirty && t->tc.dirty < targetFrame)
			list[n++] = key;
	}
	for (i = 0; i < n; i++)
		if (clear_texture(c, (vk_texture *)tex_map_find(&c->map, list[i])))
			tex_map_remove(&c->map, list[i]);
}

void vk_texcache_clear_later(vk_texcache *c)
{
	u64 key;
	u32 e;
	for (e = 0; e < tex_map_places(&c->map); e++)
	{
		vk_texture *t = (vk_texture *)tex_map_at(&c->map, e, &key);
		if (t != NULL)
			vk_texcache_destroy_later(c, t);
	}
	empty_all_in_flight(c);
	texcache_map_clear(&c->map);
}

void vk_texcache_clear(vk_texcache *c)
{
	u32 s;
	empty_all_in_flight(c);
	texcache_map_clear(&c->map);
	free_lists(&c->inFlightLists, c->inFlightCount);
	c->inFlightCount = 0;
	for (s = 0; s < c->trashCount; s++)
		destroy_trash((vk_texcache_list *)c->trashLists.data + s);
	vk_upload_ring_term(vk_upload_ring_get());
}

/* Bytes a texel of a texture image's @format */
static u32 debug_texel_bytes(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_R8_UNORM:
		return 1;
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_UNORM:
		return 4;
	default:
		return 2;
	}
}

typedef struct debug_read
{
	vk_texture *t;
	u32 level;
	u32 offset;
	u32 bytes;
} debug_read;

static void debug_log(vk_texture *const *textures, u32 count)
{
	static const char * const paths[] = { "none", "cpu-staged", "cpu-linear", "gpu-kernel", "gpu-copy", "render-target" };
	static const char * const filters[] = { "point", "bilinear", "trilinear-a", "trilinear-b" };
	static u32 lastFrame;
	static VkPhysicalDeviceProperties props;
	debug_read *reads;
	u32 readCount = 0, k, l;
	VkDeviceSize total = 0;
	vk_buffer buffer;
	bool haveBuffer = false;
	const u8 *mem = NULL;

	if (FrameCount < 300 || (lastFrame != 0 && FrameCount - lastFrame < 600) || count == 0)
		return;
	lastFrame = FrameCount;
	vkGetPhysicalDeviceProperties(vkc.gpu, &props);
	NOTICE_LOG(RENDERER, "TexDebug: frame %u, %u textures; %s (driver %08x); sub-pixel bits %u, sub-texel bits %u, mipmap bits %u, LOD bias max %.1f; force LOD0 %d, anisotropy %d, GPU decode %d, upscale %d\n",
			FrameCount, count, (const char *)&props.deviceName[0], props.driverVersion, props.limits.subPixelPrecisionBits,
			props.limits.subTexelPrecisionBits, props.limits.mipmapPrecisionBits, props.limits.maxSamplerLodBias,
			settings.rend.ForceTextureLOD0, settings.rend.AnisotropicFiltering, settings.rend.GpuTextureDecode, settings.rend.TextureUpscale);

	/* what is read back, and where */
	reads = (debug_read *)malloc((size_t)count * TEXD_MAX_LEVELS * sizeof(debug_read));
	if (reads == NULL)
		return;
	for (k = 0; k < count; k++)
	{
		vk_texture *t = textures[k];
		u32 texel;
		if (t == NULL || t->format == VK_FORMAT_UNDEFINED || t->fresh || t->decodeJob >= 0 || vk_texture_is_new(t)
				|| !(t->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || t->debugPath == VK_TEX_DBG_TARGET)
			continue;
		texel = debug_texel_bytes(t->format);
		for (l = 0; l < t->mipmapLevels && l < TEXD_MAX_LEVELS; l++)
		{
			const u32 w = t->extent.width >> l > 1 ? t->extent.width >> l : 1, h = t->extent.height >> l > 1 ? t->extent.height >> l : 1;
			reads[readCount].t = t;
			reads[readCount].level = l;
			reads[readCount].offset = (u32)total;
			reads[readCount].bytes = w * h * texel;
			readCount++;
			total = (total + w * h * texel + 3) & ~(VkDeviceSize)3;
		}
	}
	memset(&buffer, 0, sizeof(buffer));
	if (total != 0 && vk_buffer_init(&buffer, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == VK_SUCCESS)
	{
		VkCommandPoolCreateInfo poolInfo;
		VkCommandBufferAllocateInfo allocInfo;
		VkFenceCreateInfo fenceInfo;
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkMemoryBarrier toHost;
		vk_texture *last = NULL;
		u32 r;
		haveBuffer = true;
		memset(&poolInfo, 0, sizeof(poolInfo));
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		poolInfo.queueFamilyIndex = vkc.iface->queue_index;
		memset(&allocInfo, 0, sizeof(allocInfo));
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		memset(&fenceInfo, 0, sizeof(fenceInfo));
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (vkCreateCommandPool(vkc.device, &poolInfo, NULL, &pool) == VK_SUCCESS
				&& (allocInfo.commandPool = pool, vkAllocateCommandBuffers(vkc.device, &allocInfo, &cmd) == VK_SUCCESS)
				&& vkCreateFence(vkc.device, &fenceInfo, NULL, &fence) == VK_SUCCESS)
		{
			begin(cmd);
			for (r = 0; r < readCount; r++)
			{
				vk_texture *t = reads[r].t;
				const u32 w = t->extent.width >> reads[r].level > 1 ? t->extent.width >> reads[r].level : 1;
				const u32 h = t->extent.height >> reads[r].level > 1 ? t->extent.height >> reads[r].level : 1;
				VkImageMemoryBarrier barrier;
				VkBufferImageCopy region;
				memset(&barrier, 0, sizeof(barrier));
				barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.image = vk_texture_image(t);
				barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				barrier.subresourceRange.levelCount = t->mipmapLevels;
				barrier.subresourceRange.layerCount = 1;
				if (t != last)
					/* (after everything sent before, which wrote it or drew with it) */
					image_barrier(cmd, &barrier, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
							VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
							VK_PIPELINE_STAGE_TRANSFER_BIT);
				memset(&region, 0, sizeof(region));
				region.bufferOffset = reads[r].offset;
				region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				region.imageSubresource.mipLevel = reads[r].level;
				region.imageSubresource.layerCount = 1;
				region.imageExtent.width = w;
				region.imageExtent.height = h;
				region.imageExtent.depth = 1;
				vkCmdCopyImageToBuffer(cmd, vk_texture_image(t), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &region);
				if (reads[r].level + 1 == t->mipmapLevels)
					image_barrier(cmd, &barrier, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
							VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
							VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
				last = t;
			}
			memset(&toHost, 0, sizeof(toHost));
			toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, NULL, 0, NULL);
			vkEndCommandBuffer(cmd);
			vkc_submit(1, &cmd, fence);
			if (vkWaitForFences(vkc.device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
				readCount = 0;
		}
		else
			readCount = 0;
		if (fence != VK_NULL_HANDLE)
			vkDestroyFence(vkc.device, fence, NULL);
		if (pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(vkc.device, pool, NULL);
		mem = (const u8 *)vk_buffer_map(&buffer);
	}
	else
		readCount = 0;

	for (k = 0; k < count; k++)
	{
		vk_texture *t = textures[k];
		char line[1024];
		u32 tsp, tcw, r;
		const char *addu, *addv;
		bool any = false;
		int n;
		if (t == NULL)
			continue;
		tsp = TAW_FULL(t->tc.tsp);
		tcw = TAW_FULL(t->tc.tcw);
		addu = TAW_GET(tsp, TSP_ClampU) ? "clamp" : TAW_GET(tsp, TSP_FlipU) ? "mirror" : "repeat";
		addv = TAW_GET(tsp, TSP_ClampV) ? "clamp" : TAW_GET(tsp, TSP_FlipV) ? "mirror" : "repeat";
		n = snprintf(line, sizeof(line), "TexDebug: %08x tcw %08x tsp %08x %ux%u %s%s%s%s image %ux%u vk-format %d levels %u %s%s path %s; sampler %s u %s v %s bias %.2f aniso %d lod 0-256 mip nearest;",
				t->tc.sa_tex, tcw, tsp, t->tc.w, t->tc.h, texcache_pixel_format_name(&t->tc), TAW_GET(tcw, TCW_VQ_Comp) ? " vq" : "",
				TAW_GET(tcw, TCW_MipMapped) ? " mip-bit" : "", TAW_GET(tcw, TCW_ScanOrder) && !texcache_is_paletted(&t->tc) ? " planar" : "",
				t->extent.width, t->extent.height, (int)t->format, t->mipmapLevels, t->pooled != NULL ? "pooled" : "own",
				t->storage ? " storage" : "", paths[t->debugPath < 6 ? t->debugPath : 0],
				filters[TAW_GET(tsp, TSP_FilterMode)], addu, addv, D_Adjust_LoD_Bias[TAW_GET(tsp, TSP_MipMapD)],
				TAW_GET(tsp, TSP_FilterMode) != 0 && settings.rend.AnisotropicFiltering > 1 && vkc.samplerAnisotropy
						? settings.rend.AnisotropicFiltering : 0);
		for (r = 0; r < readCount; r++)
		{
			u32 gpu;
			if (reads[r].t != t || n >= (int)sizeof(line) - 48)
				continue;
			any = true;
			gpu = debug_sum(mem + reads[r].offset, reads[r].bytes);
			if (reads[r].level < t->debugLevels)
				n += snprintf(line + n, sizeof(line) - n, " L%u %08x/%08x %s", reads[r].level, gpu, t->debugSum[reads[r].level],
						gpu == t->debugSum[reads[r].level] ? "ok" : "DIFF");
			else
				n += snprintf(line + n, sizeof(line) - n, " L%u %08x", reads[r].level, gpu);
		}
		if (!any)
			snprintf(line + n, sizeof(line) - n, " not read back");
		NOTICE_LOG(RENDERER, "%s\n", line);
	}
	if (haveBuffer)
		vk_buffer_free(&buffer);
	free(reads);
}
