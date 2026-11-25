/*
    Created on: Oct 2, 2019

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
/* What the Vulkan renderers share of a frame's way to the drawers: the
 * texture command pool, the textures and their cache, the GPU's decodes
 * (of textures and of the TA's lists), the parse's memory where the card
 * reads it, the fog and palette textures, the quads and the OSD */
#ifndef VK_RENDERER_H
#define VK_RENDERER_H
#include "vk_drawer_base.h"
#include "vk_texture.h"
#include "vk_osd.h"
#include "parse_arena_vk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vk_renderer vk_renderer;

/* What a renderer has of its own: the frame drawn, how its drawers draw
 * one decoded on the GPU (ta_gpu_prepare()'s TA_GPU_*), and the picture
 * kept from another context its screen starts from (@self: owner) */
typedef struct vk_renderer_kind
{
	bool (*render_frame)(void *self);
	u32 (*ta_flags)(void *self);
	void (*set_restored_picture)(void *self, VkImageView view, u32 width, u32 height);
} vk_renderer_kind;

struct vk_renderer
{
	const vk_renderer_kind *kind;
	void *self;
	u32 samples;	/* SSAA's, a pixel (1: off) */
	vk_raster_modules modules;	/* the quads' and the per-triangle drawers' */
	vk_texture *fogTexture;
	u8 fogUploaded[256];
	bool fogUploadedValid;
	vk_texture *paletteTexture;
	vk_cmd_pool texCommandPool;
	/* the textures a picture of the CPU's is handed to the frontend in, by its frame index */
	vk_texture **framebufferTextures;
	u32 framebufferTextureCount;
	u8 *framebufferPixels;	/* (where the upload ring has no room for one) */
	size_t framebufferBytes;
	vk_tex_decoder texDecoder;	/* (before the cache: textures going away tell it) */
	vk_tex_pool texPool;		/* (and they give their images back) */
	vk_ta_decoder taDecoder;
	vk_texcache textureCache;
	vk_texture *keptPicture;
	vk_quad_pipeline quadPipeline;
	/* the VMUs' and crosshairs' after the chip's write-out, in the screen
	 * drawer's overlay render pass (writeout.h) */
	vk_quad_pipeline overlayQuadPipeline;
	vk_osd osd;
	bool osdMade;
	parse_arena_vk_t parseArena;
	bool parseArenaOk;
	int parsedSlot;
	u32 parsedHere, parsedCopied;
};

void vk_renderer_construct(vk_renderer *r, const vk_renderer_kind *kind, void *self);
void vk_renderer_free(vk_renderer *r);
/* After the drawers' render passes are made (the screen's @renderPass,
 * whose @subpass the quads draw in): false if something could not be */
bool vk_renderer_base_init(vk_renderer *r, VkRenderPass renderPass, int subpass);
/* ...and the overlay's, for @pass (none: there is no write-out) */
bool vk_renderer_init_overlay(vk_renderer *r, VkRenderPass pass);
/* The quads' pipelines dropped (what the pipeline thread still has of
 * them, finished or dropped): first in a renderer's term, before the
 * render passes they are made for go with the drawers */
void vk_renderer_drop_quads(vk_renderer *r);
void vk_renderer_term(vk_renderer *r);
bool vk_renderer_present_kept_picture(vk_renderer *r);
void vk_renderer_options_changed(vk_renderer *r);
u64 vk_renderer_get_texture(vk_renderer *r, u32 tsp, u32 tcw, int area);
bool vk_renderer_process(vk_renderer *r, TA_context *ctx);
/* The frame drawn (its kind's render_frame()), and its lists its own again
 * before it goes back to the TA: what the parse wrote stays in its slice
 * for the card, till the slot's next frame (the slot's begin waits) */
bool vk_renderer_render(vk_renderer *r);
/* What @drawer draws this frame from: the slice the parse wrote, or
 * (nothing) the main buffer it copies the lists into */
void vk_renderer_set_parsed(vk_renderer *r, vk_base_drawer *drawer);

/* The frame of @pool submitted and @image (@width x @height) handed over
 * as @extentX x @extentY: with the HDR output, encoded first in the same
 * submission, from SSAA's channel sums @sums where there are */
void vk_renderer_present(vk_cmd_pool *pool, VkImage image, VkImageView view, u32 width, u32 height, s32 extentX, s32 extentY,
		VkImageView sums);

/* The per-triangle renderer: renderer_ops' own (rend_Vulkan()) */

#ifdef __cplusplus
}
#endif

#endif
