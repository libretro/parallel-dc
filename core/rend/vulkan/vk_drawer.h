/*
	Created on: Oct 8, 2019

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
/* The per-triangle drawer's frame (the screen's and the renders to
 * textures'): its main buffers, descriptor sets and the sorted passes, and
 * the frame's lists drawn into a render pass begun for it */
#ifndef VK_DRAWER_H
#define VK_DRAWER_H
#include "vk_drawer_base.h"
#include "vk_pipeline.h"
#include "vk_quad.h"
#include "rend/sorter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vk_tri_drawer
{
	vk_base_drawer *b;
	vk_pipeline_mgr *pipelineManager;
	struct
	{
		VkDeviceSize indexOffset;
		VkDeviceSize modVolOffset;
		VkDeviceSize fragmentUniformOffset;
	} offsets;
	int imageIndex;
	/* every swap chain image's */
	vk_desc_sets *descriptorSets;
	u32 descriptorSetCount;
	vk_buffer *mainBuffers;
	u32 mainBufferCount;
	/* Per-triangle sort results: SortPasses(), a SortList a pass (their
	 * indices: in the main buffer, sorted there), and the depth prepasses
	 * of the sorted lists (rend/sorter.h), their indices after each pass's
	 * sorted ones */
	grow_buf_t sortedPasses;
	u32 sortedIndexCount;
	grow_buf_t prepassPasses;
	/* The push constants last pushed for the colours: the prepass pushes
	 * its own, and puts these back for the polygons after it that push none */
	float pushed[6];
} vk_tri_drawer;

/* drawing with @b's geometry and scissor */
void vk_tri_drawer_construct(vk_tri_drawer *d, vk_base_drawer *b);
void vk_tri_drawer_free(vk_tri_drawer *d);
/* With the pipelines of @pipelineManager: every swap chain image's sets,
 * vertex uniforms and main buffer made now, not when a frame first draws
 * (a render to a texture may come long after the game started). False if
 * one could not be made. */
bool vk_tri_drawer_init(vk_tri_drawer *d, vk_samplers *samplers, vk_pipeline_mgr *pipelineManager);
static INLINE void vk_tri_drawer_new_image(vk_tri_drawer *d)
{
	d->imageIndex = (d->imageIndex + 1) % (int)vkc.swapChainSize;
}
static INLINE vk_desc_sets *vk_tri_drawer_desc_set(vk_tri_drawer *d)
{
	return &d->descriptorSets[d->imageIndex];
}
/* The frame's lists drawn into @cmd, whose render pass is begun, with the
 * fog and palette textures' @fogView and @paletteView; with SSAA (@samples
 * over 1) the samples then resolved by @resolve into the @width x @height
 * picture, in the last subpass. False if something could not be made. */
bool vk_tri_drawer_draw(vk_tri_drawer *d, VkCommandBuffer cmd, VkImageView fogView, VkImageView paletteView,
		u32 samples, vk_ssaa_resolve *resolve, u32 width, u32 height);

#ifdef __cplusplus
}
#endif

#endif
