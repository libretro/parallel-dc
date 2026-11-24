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
/* The per-triangle renderer's drawers of a frame: the screen's, into
 * pictures it hands the frontend (vk_screen_drawer), and a render to a
 * texture's (vk_rtt_drawer) */
#ifndef VK_SCREEN_DRAWER_H
#define VK_SCREEN_DRAWER_H
#include "vk_drawer.h"
#include "vk_image.h"
#include "vk_texture.h"
#include "writeout.h"
#include "vk_gpu_timer.h"
#include "vk_motion_pass.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Renders to textures at Render To Texture Upscaling: the largest texture
 * rendered to, a side (SSAA's attachments are made that large) */
static INLINE u32 vk_rtt_scale(void)
{
	return settings.rend.RenderToTextureUpscale > 1 ? (u32)settings.rend.RenderToTextureUpscale : 1;
}
static INLINE u32 vk_rtt_max_side(void)
{
	return 1024u * vk_rtt_scale();
}
/* the render of a screen: 640x480 at that upscaling */
static INLINE VkExtent2D vk_rtt_common_render(void)
{
	VkExtent2D e;
	e.width = 640u * vk_rtt_scale();
	e.height = 480u * vk_rtt_scale();
	return e;
}
/* A render to a texture draws its own pixels (vk_texture_clear_outside()),
 * and its depth is the screen's, which is made at least as large as the
 * render of a screen (@screen: the screen's size) - which is the screen's
 * own size, or less, unless that upscaling is the higher. A larger render
 * (PAL's whole 576 lines at the console's own resolution) has a depth of
 * its own, made when it comes, as all were before. */
static INLINE VkExtent2D vk_rtt_share_extent(VkExtent2D screen)
{
	const VkExtent2D common = vk_rtt_common_render();
	VkExtent2D e;
	e.width = screen.width > common.width ? screen.width : common.width;
	e.height = screen.height > common.height ? screen.height : common.height;
	return e;
}

typedef struct vk_screen_drawer
{
	vk_base_drawer base;
	vk_tri_drawer tri;
	vk_pipeline_mgr pipelines;
	vk_raster_modules *modules;
	/* SSAA: the samples a pixel of the render pass (1: off), and the
	 * resolve its last subpass does into the picture */
	u32 samples;
	vk_ssaa_resolve ssaaResolve;
	VkCommandBuffer cmd;	/* the frame being recorded */
	VkRenderPass renderPass;
	/* each swap chain image's: its framebuffer, picture, the HDR output's
	 * image (VK_NULL_HANDLE ones without it) and the depth kept for the
	 * upscaler (ditto) */
	u32 images;
	VkFramebuffer *framebuffers;
	vk_attachment *colorAttachments;
	vk_attachment *hdrAttachments;
	vk_attachment *depthAttachments;
	/* the chip's write-out over the pictures (vk_writeout.h), which are then made for it to write */
	vk_writeout writeOut;
	bool storagePictures;
	bool overlaid;		/* this frame's write-out was recorded before the overlays */
	/* The HDR output: its images, one for each picture, written by the
	 * render pass's last subpass from the picture (vkc.hdr) */
	VkPipeline hdrPipeline;
	vk_quad_pipeline *quadPipeline;
	vk_quad_drawer lastPicture;
	bool havePicture;		/* the image before this one holds the last frame */
	VkImageView restoredPicture;	/* ...or this does: the picture kept from the context before */
	VkExtent2D restoredSize;	/* (its size: of another shape, its 4:3 drawn where this one's is) */
	/* ...or the image the last frame was drawn to at the size before this
	 * one, which is then this: kept as it is, with nothing copied */
	vk_attachment carriedPicture;
	vk_attachment depthAttachment;
	vk_attachment samplesAttachment;	/* SSAA's colour */
	vk_quad_pipeline msQuadPipeline;	/* ...and the last picture's pipeline into it */
	vk_gpu_timer timer;
	VkExtent2D viewport;
	/* For the upscaler (rend/upscaler.h): the depth is one an image, kept
	 * and readable, and the motion vector pass draws over it after the
	 * frame's render pass */
	bool upscaled;
	vk_motion_pass motionPass;
} vk_screen_drawer;

void vk_screen_drawer_construct(vk_screen_drawer *d);
/* everything it made gone (the frames using them are done) */
void vk_screen_drawer_free(vk_screen_drawer *d);
/* (@pool: what its frames are recorded with) */
static INLINE void vk_screen_drawer_set_pool(vk_screen_drawer *d, vk_cmd_pool *pool)
{
	d->base.pool = pool;
}
/* For the context's size, with @samples of SSAA (1: off), drawing with
 * @samplers and @modules; made again for a new size. False if something
 * could not be made. */
bool vk_screen_drawer_init(vk_screen_drawer *d, vk_samplers *samplers, vk_raster_modules *modules, u32 samples);
/* what its pipelines are made of goes (the pipeline thread finishes what
 * it is making for it now, while the shaders are still there) */
void vk_screen_drawer_term(vk_screen_drawer *d);
/* What draws the last picture back: see vk_screen_drawer_draw(). False if
 * the drawer of it could not be made. */
bool vk_screen_drawer_set_quad_pipeline(vk_screen_drawer *d, vk_quad_pipeline *pipeline);
/* the subpass the screen's last things are drawn in (the VMUs, the OSD): after the resolve, with SSAA */
static INLINE int vk_screen_drawer_osd_subpass(const vk_screen_drawer *d)
{
	return d->samples > 1 ? 1 : 0;
}
/* The depth a render to a texture draws into as well, made large enough
 * for one (vk_rtt_share_extent()): NULL with SSAA or the upscaler, when
 * the screen's is not one of a single sample and of no use past its frame */
static INLINE const vk_attachment *vk_screen_drawer_shared_depth(const vk_screen_drawer *d)
{
	return d->samples == 1 && !d->upscaled && d->depthAttachment.image != VK_NULL_HANDLE ? &d->depthAttachment : NULL;
}
/* rend/last_picture.h: the context is going */
void vk_screen_drawer_keep_picture(vk_screen_drawer *d);
/* ...and in the new one, the picture that was kept is this image */
static INLINE void vk_screen_drawer_set_restored_picture(vk_screen_drawer *d, VkImageView view, VkExtent2D size)
{
	d->restoredPicture = view;
	d->restoredSize = size;
}
/* The frame drawn, its render pass begun (with the fog and palette
 * textures' views), and left open for what goes over it. False if
 * something could not be made. */
bool vk_screen_drawer_draw(vk_screen_drawer *d, VkImageView fogView, VkImageView paletteView);
/* The chip's write-out (vk_writeout.h), when the frame has one: the
 * frame's render pass ended and the pass recorded now, before the VMUs
 * and crosshairs, which are then drawn in the overlay's render pass (with
 * a quad pipeline made for its overlayPass); false if there is none */
static INLINE bool vk_screen_drawer_has_writeout(const vk_screen_drawer *d)
{
	return d->writeOut.overlayPass != VK_NULL_HANDLE;
}
bool vk_screen_drawer_writeout_before_osd(vk_screen_drawer *d);
/* ...and the frame ended, submitted and handed to the frontend. False if
 * something could not be made. */
bool vk_screen_drawer_end(vk_screen_drawer *d);

typedef struct vk_rtt_drawer
{
	vk_base_drawer base;
	vk_tri_drawer tri;
	vk_pipeline_mgr pipelines;
	u32 samples;
	vk_ssaa_resolve ssaaResolve;
	VkCommandBuffer cmd;
	u32 width, height;
	u32 textureAddr;
	vk_texcache *textureCache;
	vk_texture *texture;
	vk_kept_framebuffer *framebuffers;	/* each swap chain image's */
	u32 framebufferCount;
	const vk_screen_drawer *screen;		/* whose depth is shared */
	vk_attachment depthAttachment;		/* ...or this, of its own */
	bool saidLarger;
	u32 drewShared, drewOwn;	/* (said when it goes: tools/vulkan/vk_vram.sh) */
	vk_attachment samplesAttachment;	/* SSAA's colour */
} vk_rtt_drawer;

void vk_rtt_drawer_construct(vk_rtt_drawer *d);
void vk_rtt_drawer_free(vk_rtt_drawer *d);
static INLINE void vk_rtt_drawer_set_pool(vk_rtt_drawer *d, vk_cmd_pool *pool)
{
	d->base.pool = pool;
}
/* @samples: SSAA's (1: off). False if something could not be made. */
bool vk_rtt_drawer_init(vk_rtt_drawer *d, vk_samplers *samplers, vk_raster_modules *modules, vk_texcache *textureCache, u32 samples);
/* The screen's drawer's depth, the one this draws into when it is large
 * enough (vk_screen_drawer_shared_depth()); made of its own, as large as
 * vk_rtt_common_render(), where there is none to share. Said after the
 * screen's drawer is made, or made again. */
bool vk_rtt_drawer_share_depth(vk_rtt_drawer *d, const vk_screen_drawer *screen);
void vk_rtt_drawer_term(vk_rtt_drawer *d);
/* the render drawn into its texture, and ended (the texture watched in
 * video memory). False if something could not be made. */
bool vk_rtt_drawer_draw(vk_rtt_drawer *d, VkImageView fogView, VkImageView paletteView);
void vk_rtt_drawer_end(vk_rtt_drawer *d);

#ifdef __cplusplus
}
#endif

#endif
