/*
    Created on: Nov 7, 2019

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
/* The per-pixel drawer's frame (the screen's and the renders to
 * textures'): its main buffers, descriptor sets and attachments, and the
 * frame's render passes, in bands when its translucent fragments do not
 * all fit in the buffers (vk_oit_buffer.h) */
#ifndef VK_OIT_DRAWER_H
#define VK_OIT_DRAWER_H
#include "../vk_drawer_base.h"
#include "../vk_image.h"
#include "vk_oit_pipeline.h"
#include "vk_oit_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vk_oit_drawer vk_oit_drawer;

/* what the screen's and the textures' drawers have of their own */
typedef struct vk_oit_drawer_kind
{
	/* the framebuffer of the frame's final render pass */
	VkFramebuffer (*final_framebuffer)(void *self);
	/* In the colour subpass of a frame's first pass, before any polygon:
	 * the screen's drawer puts the last picture there when the render will
	 * cover only part of it. True if it drew, and what was bound is gone. */
	bool (*draw_last_picture)(void *self, VkCommandBuffer cmd);
} vk_oit_drawer_kind;

#ifdef RACE_TEST
typedef struct vk_oit_held_draw
{
	VkPipeline pipeline;
	u32 cullIndex;
	VkRect2D scissor;
	u32 vertices, first;
} vk_oit_held_draw;
#endif

struct vk_oit_drawer
{
	vk_base_drawer *b;
	const vk_oit_drawer_kind *kind;
	void *self;
	vk_oit_pipeline_mgr *pipelines;
	vk_oit_buffers *buffers;
	vk_samplers *samplers;
	VkFormat colorFormat;	/* the attachments' */
	VkRect2D viewport;
	/* A frame's translucent fragments that do not all fit in the buffer
	 * are not dropped: the frame is drawn in this many horizontal bands,
	 * all of it once a band, clipped to the band. Each band's lists start
	 * empty, so they need the room of one band's fragments. How many is
	 * told by the frames before (vk_oit_buffers_choose_bands()). */
	u32 bands;
	VkRect2D band;	/* the one being drawn; no height when not in bands */
	vk_attachment colorAttachments[2];
	vk_attachment depthAttachment;
	/* For the temporal upscaler (rend/upscaler.h): the depth is one an
	 * image, kept past the frame and readable, as the frontend has the
	 * frames for a while; one for all otherwise */
	bool upscaled;
	vk_attachment *depthAttachments;
	u32 depthAttachmentCount;
	/* the render passes' before the last, two taking turns; by image when the depth is one an image */
	VkFramebuffer (*tempFramebuffers)[2];
	u32 tempFramebufferCount;
	bool needDepthTransition;
	int maxWidth, maxHeight;
	/* The attachments a frame is drawn into: this drawer's own, or another
	 * one's that it shares (the texture drawer's, the screen's) */
	vk_oit_drawer *attachmentsOf;
	u32 attachmentSamples;		/* SSAA's, of the attachments made */
	bool attachmentsUpscaled;	/* ...and whether they were made for the upscaler */
	VkCommandBuffer cmd;
	struct
	{
		VkDeviceSize indexOffset;
		VkDeviceSize modVolOffset;
		VkDeviceSize fragmentUniformOffset;
		VkDeviceSize polyParamsOffset;
		VkDeviceSize polyParamsSize;
	} offsets;
	vk_buffer quad;		/* the whole framebuffer's four vertices */
	int imageIndex;
	/* every swap chain image's */
	vk_oit_desc_sets *descSets;
	u32 descSetCount;
	vk_buffer *mainBuffers;
	u32 mainBufferCount;
#ifdef RACE_TEST
	vk_oit_held_draw held[256];
	u32 heldCount;
#endif
};

/* drawing with @b's geometry and scissor, its kind's @kind of @self */
void vk_oit_drawer_construct(vk_oit_drawer *d, vk_base_drawer *b, const vk_oit_drawer_kind *kind, void *self);
/* With @pipelines and @buffers: every swap chain image's sets, vertex
 * uniforms and main buffer made now, not when a frame first draws (a
 * render to a texture may come long after the game started). False if
 * something could not be made. */
bool vk_oit_drawer_init(vk_oit_drawer *d, vk_samplers *samplers, vk_oit_pipeline_mgr *pipelines, vk_oit_buffers *buffers);
void vk_oit_drawer_term(vk_oit_drawer *d);
static INLINE void vk_oit_drawer_new_image(vk_oit_drawer *d)
{
	d->imageIndex = (d->imageIndex + 1) % (int)vkc.swapChainSize;
}
static INLINE vk_oit_desc_sets *vk_oit_drawer_desc_set(vk_oit_drawer *d)
{
	return &d->descSets[d->imageIndex];
}
/* the depth this frame is drawn with */
static INLINE vk_attachment *vk_oit_drawer_depth(vk_oit_drawer *d)
{
	return d->upscaled ? &d->depthAttachments[d->imageIndex] : &d->depthAttachment;
}
/* clipped to the band being drawn, when a frame is drawn in bands */
void vk_oit_drawer_set_scissor(vk_oit_drawer *d, VkCommandBuffer cmd, const VkRect2D *scissor);
/* The buffers and the attachments, with their framebuffers, at least
 * @width by @height: those they replace go once the frames that may use
 * them are done. False if something could not be made. */
bool vk_oit_drawer_make_buffers(vk_oit_drawer *d, int width, int height);
bool vk_oit_drawer_make_attachments(vk_oit_drawer *d, int width, int height);
/* ...and none */
void vk_oit_drawer_drop_attachments(vk_oit_drawer *d);
/* The frame's render passes recorded into @cmd (begun, the drawer's
 * frame), with the fog and palette textures' @fogView and @paletteView;
 * the last band's final render pass left open. False if something could
 * not be made. */
bool vk_oit_drawer_draw(vk_oit_drawer *d, VkCommandBuffer cmd, VkImageView fogView, VkImageView paletteView);

/* the size of the texture the render to a texture in pvrrc goes to, with
 * Render To Texture Upscaling (vk_oit_renderer.c's; the compute renderer
 * draws into it too) */
VkExtent2D vk_oit_rtt_texture_size(void);

#ifdef __cplusplus
}
#endif

#endif
