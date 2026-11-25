#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
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
/* The per-pixel renderer: its screen's and textures' drawers over
 * vk_oit_drawer, and the renderer's renderer_ops (rend_OITVulkan()), with
 * the compute renderer drawing the frames it can (vk_compute_drawer.h) */
#include <stdlib.h>
#include <string.h>
#include "vk_oit_drawer.h"
#include "../vk_renderer.h"
#include "../vk_screen_drawer.h"
#include "../vk_compute_drawer.h"
#include "../vk_quad.h"
#include "../rtt_read.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/ta_words.h"
#include "rend/TexCache.h"
#include "rend/last_picture.h"
#include "rend/upscaler.h"
#include "rend/ssaa_pattern.h"
#include "rend/rend_method.h"
#include "rend/ta_gpu.h"

/* ---- What goes once the frames that may use it are done ---- */

typedef struct framebuffer_box
{
	VkFramebuffer framebuffer;
} framebuffer_box;

static void framebuffer_gone(void *object)
{
	vkDestroyFramebuffer(vkc.device, ((framebuffer_box *)object)->framebuffer, NULL);
	free(object);
}

/* (now with no pool) */
static void defer_framebuffer(vk_cmd_pool *pool, VkFramebuffer *framebuffer)
{
	framebuffer_box *kept;
	if (*framebuffer == VK_NULL_HANDLE)
		return;
	kept = pool != NULL ? (framebuffer_box *)malloc(sizeof(*kept)) : NULL;
	if (kept != NULL)
	{
		kept->framebuffer = *framebuffer;
		vk_cmd_pool_defer(pool, kept, framebuffer_gone);
	}
	else
		vkDestroyFramebuffer(vkc.device, *framebuffer, NULL);
	*framebuffer = VK_NULL_HANDLE;
}

static VkCommandBuffer begin_commands(vk_cmd_pool *pool)
{
	VkCommandBufferBeginInfo begin;
	VkCommandBuffer cmd = vk_cmd_pool_allocate(pool);
	if (cmd == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);
	return cmd;
}

static void set_viewport(VkCommandBuffer cmd, float x, float y, float width, float height)
{
	VkViewport viewport;
	viewport.x = x;
	viewport.y = y;
	viewport.width = width;
	viewport.height = height;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);
}

/* ---- The screen's drawer ---- */

typedef struct vk_oit_screen
{
	vk_base_drawer base;
	vk_oit_drawer o;
	vk_oit_pipeline_mgr pipelines;
	vk_oit_shaders *shaders;
	/* each swap chain image's: the picture, and the framebuffer of the
	 * frame's final render pass into it */
	vk_attachment *finalColors;
	VkFramebuffer *framebuffers;
	u32 images;
	vk_quad_pipeline *lastPicturePipeline;
	vk_motion_pass motionPass;
	vk_motion_pass computeMotionPass;
	vk_quad_drawer lastPicture;
	bool havePicture;		/* the image before this one holds the last frame */
	VkImageView restoredPicture;	/* ...or this does: the picture kept from the context before */
	VkExtent2D restoredSize;	/* (its size: of another shape, its 4:3 drawn where this one's is) */
	/* ...or the image the last frame was drawn to at the size before this
	 * one (see the per-triangle screen drawer) */
	vk_attachment carriedPicture;
	vk_gpu_timer timer;
	bool storagePicture;
	/* the chip's write-out over the pictures (../writeout.h), which are then made for it to write */
	vk_writeout writeOut;
	bool overlaid;		/* this frame's write-out was recorded before the overlays */
} vk_oit_screen;

/* The attachments renders to a texture draw into as well, when they can:
 * of a single sample, in the textures' format, and not the upscaler's
 * (one an image) */
static bool screen_shares_attachments(const vk_oit_screen *s)
{
	return s->o.pipelines != NULL && s->pipelines.passes.samples == 1 && !s->o.upscaled
			&& vkc.colorFormat == VK_FORMAT_R8G8B8A8_UNORM;
}

static VkFramebuffer screen_final_framebuffer(void *self)
{
	vk_oit_screen *s = (vk_oit_screen *)self;
	return s->framebuffers[s->o.imageIndex];
}

/* (the render will cover only part of the screen: see vk_oit_drawer_kind) */
static bool screen_wants_last_picture(vk_oit_screen *s)
{
	return (s->havePicture || s->restoredPicture != VK_NULL_HANDLE) && s->lastPicturePipeline != NULL && !pvrrc.clearFramebuffer
			&& transform_matrix_clipped(&s->base.matrices);
}

static bool screen_draw_last_picture(void *self, VkCommandBuffer cmd)
{
	static const float opaque[4] = { 1.f, 1.f, 1.f, 1.f };
	vk_oit_screen *s = (vk_oit_screen *)self;
	const int count = (int)s->images;
	VkPipeline pipeline;
	if (!screen_wants_last_picture(s))
		return false;
	vk_oit_drawer_set_scissor(&s->o, cmd, &s->o.viewport);
	vkCmdSetBlendConstants(cmd, opaque);
	pipeline = vk_quad_pipeline_get(s->lastPicturePipeline, false);
	if (pipeline == VK_NULL_HANDLE)
		return false;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	/* (the picture kept from another context can be of another size) */
	if (s->havePicture)
		vk_quad_drawer_draw(&s->lastPicture, cmd, s->finalColors[(s->o.imageIndex + count - 1) % count].view, NULL, true, s->base.pool);
	else
	{
		QuadVertex area[4];
		vk_quad_drawer_draw(&s->lastPicture, cmd, s->restoredPicture,
				vk_quad_game_area(area, s->restoredSize.width, s->restoredSize.height, s->o.viewport.extent.width,
					s->o.viewport.extent.height) ? area : NULL, false, s->base.pool);
	}
	return true;
}

static const vk_oit_drawer_kind screen_kind = { screen_final_framebuffer, screen_draw_last_picture };

static void screen_construct(vk_oit_screen *s)
{
	memset(s, 0, sizeof(*s));
	vk_base_drawer_construct(&s->base);
	vk_oit_drawer_construct(&s->o, &s->base, &screen_kind, s);
	vk_oit_pipeline_mgr_construct(&s->pipelines, false);
	vk_motion_pass_construct(&s->motionPass);
	vk_motion_pass_construct(&s->computeMotionPass);
	vk_writeout_construct(&s->writeOut);
	s->timer.what = "";
}

/* the pictures and their framebuffers, now (the frames on the GPU were waited for) */
static void screen_images_free(vk_oit_screen *s, vk_cmd_pool *pool)
{
	u32 i;
	for (i = 0; i < s->images; i++)
	{
		defer_framebuffer(pool, &s->framebuffers[i]);
		vk_attachment_defer(pool, &s->finalColors[i]);
	}
	free(s->framebuffers);
	free(s->finalColors);
	s->framebuffers = NULL;
	s->finalColors = NULL;
	s->images = 0;
}

static void screen_term(vk_oit_screen *s)
{
	vk_gpu_timer_term(&s->timer);
	vk_writeout_term(&s->writeOut);
	vk_motion_pass_term(&s->motionPass, NULL);
	vk_motion_pass_term(&s->computeMotionPass, NULL);
	vk_oit_pipeline_mgr_term(&s->pipelines);
	vk_oit_pipeline_mgr_construct(&s->pipelines, false);
	screen_images_free(s, NULL);
	vk_attachment_free(&s->carriedPicture);
	memset(&s->carriedPicture, 0, sizeof(s->carriedPicture));
	s->restoredPicture = VK_NULL_HANDLE;
	vk_quad_drawer_term(&s->lastPicture);
	s->havePicture = false;
	vk_oit_drawer_term(&s->o);
}

static void screen_free(vk_oit_screen *s)
{
	screen_term(s);
	vk_base_drawer_term(&s->base);
}

static bool screen_make_framebuffers(vk_oit_screen *s)
{
	vk_cmd_pool *pool = s->base.pool;
	const u32 count = (u32)vkc.swapChainSize;
	const bool storage = s->storagePicture || vk_writeout_wanted();
	VkExtent2D size, heads;
	VkImageView *views;
	bool made;
	u32 i;
	s->o.viewport.offset.x = 0;
	s->o.viewport.offset.y = 0;
	s->o.viewport.extent.width = vkc.width;
	s->o.viewport.extent.height = vkc.height;

	vk_oit_buffers_set_screen_size(s->o.buffers, (int)vkc.width, (int)vkc.height);
	/* (renders to textures draw into the attachments too, when they can: as
	 * large as those need, vk_rtt_share_extent(); the lists' heads as large
	 * as those renders too, made while the game runs, for the first of
	 * them, otherwise) */
	size = screen_shares_attachments(s) ? vk_rtt_share_extent(s->o.viewport.extent) : s->o.viewport.extent;
	heads = vk_rtt_share_extent(s->o.viewport.extent);
	if (!vk_oit_buffers_init(s->o.buffers, (int)heads.width, (int)heads.height, pool)
			|| !vk_oit_drawer_make_attachments(&s->o, (int)size.width, (int)size.height))
		return false;
	/* (kept until the frames that could still use them have gone by: see the per-triangle screen drawer) */
	if (pool != NULL)
	{
		/* the image with the last frame stays, as the picture the next render starts from */
		vk_attachment_defer(pool, &s->carriedPicture);
		if (s->havePicture && (u32)s->o.imageIndex < s->images && s->finalColors[s->o.imageIndex].image != VK_NULL_HANDLE)
		{
			s->carriedPicture = s->finalColors[s->o.imageIndex];
			memset(&s->finalColors[s->o.imageIndex], 0, sizeof(vk_attachment));
			s->restoredPicture = s->carriedPicture.view;
			s->restoredSize = s->carriedPicture.extent;
		}
	}
	screen_images_free(s, pool);
	s->havePicture = false;
	s->finalColors = (vk_attachment *)calloc(count, sizeof(vk_attachment));
	s->framebuffers = (VkFramebuffer *)calloc(count, sizeof(VkFramebuffer));
	if (s->finalColors == NULL || s->framebuffers == NULL)
		return false;
	for (; s->images < count; s->images++)
	{
		vk_attachment *picture = &s->finalColors[s->images];
		VkImageView attachments[3];
		VkFramebufferCreateInfo createInfo;
		/* (copied from when the context goes, to keep the picture: rend/last_picture.h) */
		if (!vk_attachment_init(picture, s->o.viewport.extent.width, s->o.viewport.extent.height, vkc.colorFormat,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
				| (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0), false, VK_SAMPLE_COUNT_1_BIT))
			return false;
		attachments[0] = picture->view;
		attachments[1] = s->o.colorAttachments[0].view;
		attachments[2] = s->o.upscaled ? s->o.depthAttachments[s->images].view : s->o.depthAttachment.view;
		memset(&createInfo, 0, sizeof(createInfo));
		createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		createInfo.renderPass = vk_oit_passes_get(&s->pipelines.passes, true, true, false, false);
		createInfo.attachmentCount = 3;
		createInfo.pAttachments = attachments;
		createInfo.width = s->o.viewport.extent.width;
		createInfo.height = s->o.viewport.extent.height;
		createInfo.layers = 1;
		if (createInfo.renderPass == VK_NULL_HANDLE
				|| vkCreateFramebuffer(vkc.device, &createInfo, NULL, &s->framebuffers[s->images]) != VK_SUCCESS)
		{
			s->images++;	/* (its picture goes with the rest) */
			return false;
		}
	}
	/* (the chip's write-out over the pictures: ../writeout.h) */
	views = (VkImageView *)malloc(s->images * sizeof(VkImageView));
	if (views == NULL)
		return false;
	for (i = 0; i < s->images; i++)
		views[i] = s->finalColors[i].view;
	made = vk_writeout_init(&s->writeOut, views, s->images, s->o.viewport.extent.width, s->o.viewport.extent.height);
	free(views);
	return made;
}

/* @samples: SSAA's (1: off), which the shaders have been given */
static bool screen_init(vk_oit_screen *s, vk_samplers *samplers, vk_oit_shaders *shaders, vk_oit_buffers *buffers, u32 samples)
{
	vk_cmd_pool *pool = s->base.pool;
	/* (the upscaler wants the depth kept, which SSAA's samples are not) */
	s->o.upscaled = upscaler_active() && samples == 1;
	s->o.colorFormat = vkc.colorFormat;
	s->shaders = shaders;
	if (!vk_oit_pipeline_mgr_init(&s->pipelines, shaders, buffers->descSetLayout, buffers->sums, samples, s->o.upscaled)
			|| !vk_oit_drawer_init(&s->o, samplers, &s->pipelines, buffers) || !screen_make_framebuffers(s))
		return false;
	if (s->o.upscaled)
	{
		VkImageView *views = (VkImageView *)malloc(s->o.depthAttachmentCount * sizeof(VkImageView));
		bool made;
		u32 i;
		if (views == NULL)
			return false;
		for (i = 0; i < s->o.depthAttachmentCount; i++)
			views[i] = s->o.depthAttachments[i].view;
		made = vk_motion_pass_init(&s->motionPass, s->o.viewport.extent, vkc.depthFormat, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
				views, s->o.depthAttachmentCount, s->pipelines.pipelineLayout, shaders->modules.m[RSPV_MV_VERT],
				shaders->modules.m[RSPV_MV_FRAG], pool);
		free(views);
		if (!made)
			return false;
	}
	else
		vk_motion_pass_term(&s->motionPass, pool);
	upscaler_reset();
	vk_gpu_timer_init(&s->timer, vkc.swapChainSize, "per-pixel renderer");
	return true;
}

static VkCommandBuffer screen_new_frame(vk_oit_screen *s)
{
	vk_base_drawer *b = &s->base;
	VkCommandBuffer cmd;
	vk_oit_drawer_new_image(&s->o);
	cmd = begin_commands(b->pool);
	if (cmd == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	vk_gpu_timer_begin(&s->timer, cmd, b->pool->index);

	transform_matrix_calc(&b->matrices, &pvrrc);
	/* the frame drawn under this frame's jitter, its strips matched for the motion vector pass */
	if (s->o.upscaled && vk_motion_pass_begin(&s->motionPass, s->o.imageIndex))
	{
		float jx, jy;
		upscaler_jitter(&jx, &jy);
		transform_matrix_jitter(&b->matrices, jx, jy);
	}
	b->vertexUniformBuffer = vk_base_drawer_write_vertex_uniforms(b, cmd, s->o.imageIndex);
	vk_base_drawer_set_base_scissor(b);
	vkCmdSetScissor(cmd, 0, 1, &b->baseScissor);
	set_viewport(cmd, (float)s->o.viewport.offset.x, (float)s->o.viewport.offset.y, (float)s->o.viewport.extent.width,
			(float)s->o.viewport.extent.height);
	s->o.cmd = cmd;
	return b->vertexUniformBuffer != VK_NULL_HANDLE ? cmd : VK_NULL_HANDLE;
}

/* The frame submitted and its picture handed over: with the HDR output,
 * the picture encoded first, in the same submission, from the channel
 * sums @sums where there are */
static void screen_present(vk_oit_screen *s, VkImageView sums)
{
	const vk_attachment *picture = &s->finalColors[s->o.imageIndex];
	vk_renderer_present(s->base.pool, picture->image, picture->view, s->o.viewport.extent.width, s->o.viewport.extent.height,
			(s32)s->o.viewport.extent.width, (s32)s->o.viewport.extent.height, sums);
}

/* the picture this frame was drawn to is the last one now */
static void screen_have_picture(vk_oit_screen *s)
{
	s->havePicture = true;
	s->restoredPicture = VK_NULL_HANDLE;
	vk_attachment_defer(s->base.pool, &s->carriedPicture);
}

static bool screen_end_frame(vk_oit_screen *s)
{
	VkCommandBuffer cmd = s->o.cmd;
	const int image = s->o.imageIndex;
	const vk_attachment *picture = &s->finalColors[image];
	vk_attachment *depth;
	bool ok = true;
	screen_have_picture(s);
	vkCmdEndRenderPass(cmd);
	if (s->overlaid)
		s->overlaid = false;
	else
		vk_writeout_record(&s->writeOut, cmd, (u32)image, picture->image, picture->view, s->o.viewport.extent.width,
				s->o.viewport.extent.height);
	if (s->motionPass.recorded)
	{
		/* the motion vectors over the depth the frame left (what went over the picture may have bound its own) */
		vk_oit_desc_sets_bind_per_frame(vk_oit_drawer_desc_set(&s->o), cmd);
		ok = vk_motion_pass_record(&s->motionPass, cmd, image, s->base.geometry, s->o.offsets.indexOffset, &s->base.baseScissor,
				s->base.geometryVertices);
	}
	vk_oit_buffers_count_pass(s->o.buffers, cmd);
	vk_gpu_timer_end(&s->timer, cmd, s->base.pool->index);
	vkEndCommandBuffer(cmd);
	s->o.cmd = VK_NULL_HANDLE;
	/* (with SSAA, the samples' channel sums the last pass stored, when it
	 * did: no write-out pass came after it) */
	screen_present(s, s->pipelines.sums && !vk_writeout_runs(&s->writeOut, s->o.viewport.extent.height)
			? s->o.buffers->sumsAttachment.view : VK_NULL_HANDLE);
	if (s->motionPass.recorded)
	{
		depth = vk_oit_drawer_depth(&s->o);
		vk_motion_pass_submit(&s->motionPass, image, depth->image, depth->view, vkc.depthFormat);
	}
	return ok;
}

/* The chip's write-out (../writeout.h), when the frame has one: the
 * frame's render pass ended and the pass recorded now, before the VMUs and
 * crosshairs, which are then drawn in the overlay's render pass; false if
 * there is none */
static bool screen_writeout_before_osd(vk_oit_screen *s)
{
	const vk_attachment *picture = &s->finalColors[s->o.imageIndex];
	if (s->writeOut.overlayPass == VK_NULL_HANDLE || !vk_writeout_runs(&s->writeOut, s->o.viewport.extent.height))
		return false;
	vkCmdEndRenderPass(s->o.cmd);
	vk_writeout_record(&s->writeOut, s->o.cmd, (u32)s->o.imageIndex, picture->image, picture->view, s->o.viewport.extent.width,
			s->o.viewport.extent.height);
	vk_writeout_begin_overlay(&s->writeOut, s->o.cmd, (u32)s->o.imageIndex);
	s->overlaid = true;
	return true;
}

static void screen_keep_picture(vk_oit_screen *s)
{
	u8 *pixels;
	if (!s->havePicture)
		return;
	pixels = last_picture_keep((int)s->o.viewport.extent.width, (int)s->o.viewport.extent.height);
	if (pixels != NULL)
		vk_read_picture(s->finalColors[s->o.imageIndex].image, vkc.colorFormat, s->o.viewport.extent.width,
				s->o.viewport.extent.height, pixels);
}

/* A quad pipeline made for the colour subpass of this drawer's render pass */
static bool screen_set_last_picture_pipeline(vk_oit_screen *s, vk_quad_pipeline *pipeline)
{
	s->lastPicturePipeline = pipeline;
	return vk_quad_drawer_init(&s->lastPicture, pipeline);
}

/* The compute renderer draws frames into this drawer's pictures: whether
 * the frame in pvrrc starts from the picture before (@last), and whether
 * that is the picture kept from another context (@scaled), which can be of
 * another size and is filtered over the screen as this drawer's quad does */
static void screen_compute_start(vk_oit_screen *s, bool *last, bool *scaled)
{
	transform_matrix_calc(&s->base.matrices, &pvrrc);
	*last = screen_wants_last_picture(s);
	*scaled = *last && !s->havePicture;
}

/* the motion vector pass over the compute renderer's frames (over the
 * depth its walk stored, @depthViews, one a slot of the pool), with this
 * drawer's vertex uniforms; NULL when the upscaler has no frames */
static vk_motion_pass *screen_compute_motion_pass(vk_oit_screen *s, const VkImageView *depthViews, u32 count)
{
	VkBuffer uniforms[16];
	u32 i;
	if (!s->o.upscaled || count == 0 || count > sizeof(uniforms) / sizeof(uniforms[0])
			|| !vk_base_drawer_make_vertex_uniforms(&s->base, count))
	{
		vk_motion_pass_term(&s->computeMotionPass, s->base.pool);
		return NULL;
	}
	for (i = 0; i < count; i++)
		uniforms[i] = s->base.vertexUniformBuffers[i].buffer;
	if (!vk_motion_pass_init_sampled(&s->computeMotionPass, s->o.viewport.extent, depthViews, count, uniforms,
			sizeof(VertexShaderUniforms), s->shaders->modules.m[RSPV_MV_VERT], s->shaders->modules.m[RSPV_MV_DEPTH_FRAG],
			s->base.pool))
		return NULL;
	return &s->computeMotionPass;
}

/* ---- A render to a texture's drawer ---- */

typedef struct vk_oit_rtt
{
	vk_base_drawer base;
	vk_oit_drawer o;
	vk_oit_pipeline_mgr pipelines;
	u32 textureAddr;
	vk_oit_screen *screen;		/* whose attachments are shared */
	bool saidLarger;
	u32 drewShared, drewOwn;	/* (said when it goes: tools/vulkan/vk_vram.sh) */
	VkImageUsageFlags storageUsage;
	vk_texture *texture;
	vk_kept_framebuffer *framebuffers;
	u32 framebufferCount;
	vk_texcache *textureCache;
} vk_oit_rtt;

/* The render's sizes: the frame from its first pixel to the pixel clip's
 * last, the clip, and the texture, a power of two, all times Render To
 * Texture Upscaling */
typedef struct rtt_size
{
	u32 origWidth, origHeight;
	u32 clipX, clipY, clipWidth, clipHeight;
	u32 upscaledWidth, upscaledHeight;
	u32 widthPow2, heightPow2;
} rtt_size;

static VkFramebuffer rtt_final_framebuffer(void *self)
{
	vk_oit_rtt *t = (vk_oit_rtt *)self;
	return t->framebuffers[t->o.imageIndex].framebuffer;
}

static const vk_oit_drawer_kind rtt_kind = { rtt_final_framebuffer, NULL };

static void rtt_construct(vk_oit_rtt *t)
{
	memset(t, 0, sizeof(*t));
	vk_base_drawer_construct(&t->base);
	vk_oit_drawer_construct(&t->o, &t->base, &rtt_kind, t);
	vk_oit_pipeline_mgr_construct(&t->pipelines, true);
}

static bool rtt_framebuffers(vk_oit_rtt *t, u32 count)
{
	vk_kept_framebuffer *more;
	while (t->framebufferCount > count)
		vk_kept_framebuffer_free(&t->framebuffers[--t->framebufferCount]);
	if (t->framebufferCount == count)
		return true;
	more = (vk_kept_framebuffer *)realloc(t->framebuffers, count * sizeof(vk_kept_framebuffer));
	if (more == NULL)
		return false;
	t->framebuffers = more;
	memset(&more[t->framebufferCount], 0, (count - t->framebufferCount) * sizeof(vk_kept_framebuffer));
	t->framebufferCount = count;
	return true;
}

static bool rtt_init(vk_oit_rtt *t, vk_samplers *samplers, vk_oit_shaders *shaders, vk_texcache *textureCache, vk_oit_buffers *buffers)
{
	if (!vk_oit_pipeline_mgr_rtt_init(&t->pipelines, shaders, buffers->descSetLayout, buffers->sums)
			|| !vk_oit_drawer_init(&t->o, samplers, &t->pipelines, buffers))
		return false;
	t->textureCache = textureCache;
	/* (a framebuffer's place for each image; the attachments: rtt_share_attachments()) */
	return rtt_framebuffers(t, (u32)vkc.swapChainSize);
}

/* The screen's drawer's attachments, which this draws into when they are
 * large enough (screen_shares_attachments(), and vk_rtt_share_extent());
 * made of its own, as large as vk_rtt_common_render(), where there are
 * none to share - now, not while the game runs. Said after the screen's
 * drawer is made, or made again. */
static bool rtt_share_attachments(vk_oit_rtt *t, vk_oit_screen *screen)
{
	VkExtent2D common;
	t->screen = screen;
	t->o.attachmentsOf = &t->o;
	if (screen != NULL && screen_shares_attachments(screen))
	{
		vk_oit_drawer_drop_attachments(&t->o);
		return true;
	}
	common = vk_rtt_common_render();
	return vk_oit_drawer_make_attachments(&t->o, (int)common.width, (int)common.height);
}

static void rtt_term(vk_oit_rtt *t)
{
	if (t->drewShared + t->drewOwn != 0)
		NOTICE_LOG(RENDERER, "Renders to textures: %u drawn with the screen's attachments, %u with their own; %u cleared outside\n",
				t->drewShared, t->drewOwn, vk_texture_outside_cleared);
	vk_texture_outside_cleared = 0;
	t->drewShared = t->drewOwn = 0;
	rtt_framebuffers(t, 0);
	free(t->framebuffers);
	t->framebuffers = NULL;
	vk_oit_pipeline_mgr_term(&t->pipelines);
	vk_oit_pipeline_mgr_construct(&t->pipelines, true);
	vk_oit_drawer_term(&t->o);
}

static void rtt_free(vk_oit_rtt *t)
{
	rtt_term(t);
	vk_base_drawer_term(&t->base);
}

static rtt_size rtt_get_size(void)
{
	const u32 scale = vk_rtt_scale();
	rtt_size s;
	/* the frame from its first pixel, as far as the pixel clip goes (see transform_matrix.h) */
	s.origWidth = pvrrc.fb_X_CLIP.max + 1;
	s.origHeight = pvrrc.fb_Y_CLIP.max + 1;
	s.clipX = pvrrc.fb_X_CLIP.min;
	s.clipY = pvrrc.fb_Y_CLIP.min;
	s.clipWidth = s.origWidth - s.clipX;
	s.clipHeight = s.origHeight - s.clipY;
	s.upscaledWidth = s.origWidth;
	s.upscaledHeight = s.origHeight;
	s.heightPow2 = 8;
	while (s.heightPow2 < s.upscaledHeight)
		s.heightPow2 *= 2;
	s.widthPow2 = 8;
	while (s.widthPow2 < s.upscaledWidth)
		s.widthPow2 *= 2;
	if (scale > 1)
	{
		s.upscaledWidth *= scale;
		s.upscaledHeight *= scale;
		s.widthPow2 *= scale;
		s.heightPow2 *= scale;
	}
	return s;
}

VkExtent2D vk_oit_rtt_texture_size(void)
{
	const rtt_size s = rtt_get_size();
	VkExtent2D e;
	e.width = s.widthPow2;
	e.height = s.heightPow2;
	return e;
}

/* The texture the render goes to, taken from the cache and given an image
 * to render to, with @usage too; the layout it is in (VK_IMAGE_LAYOUT_MAX_ENUM: none could be made) */
static VkImageLayout rtt_take_texture(vk_oit_rtt *t, const rtt_size *size, VkImageUsageFlags usage)
{
	/* (read back when the game touches it in video memory) */
	const VkImageUsageFlags renderTargetUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT | t->storageUsage | usage;
	vk_texture *texture;
	u32 tcw, tsp, u, v;
	VkImageLayout layout;

	/* what an earlier render left waiting in this memory, while its picture is still what it was: see rtt_read.h */
	vk_rtt_supersede(t->textureAddr, size->clipX, size->clipY, size->clipWidth, size->clipHeight);
	/* TexAddr : fb_rtt.TexAddr, Reserved : 0, StrideSel : 0, ScanOrder : 1 */
	tcw = TAW_SET(TAW_SET(0, TCW_TexAddr, t->textureAddr >> 3), TCW_ScanOrder, 1);
	switch (FB_W_CTRL.fb_packmode)
	{
	case 0:
	case 3:
		tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_1555);
		break;
	case 1:
		tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_565);
		break;
	case 2:
		tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_4444);
		break;
	}
	for (u = 0; u < 7 && (8u << u) < size->origWidth; u++)
		;
	for (v = 0; v < 7 && (8u << v) < size->origHeight; v++)
		;
	tsp = TAW_SET(TAW_SET(0, TSP_TexU, u), TSP_TexV, v);

	texture = vk_texcache_get(t->textureCache, tsp, tcw, 0);
	if (vk_texture_is_new(texture))
		texcache_create(&texture->tc);
	else if (vk_texcache_is_in_flight(t->textureCache, texture))
	{
		vk_texture_keep_read_only_view(texture);
		vk_texcache_destroy_later(t->textureCache, texture);
	}
	vk_texcache_set_in_flight(t->textureCache, texture);
	t->texture = texture;

	/* A texture the game uploaded can become one it renders to, at the
	 * same address, the same size and in this format: its image was not
	 * made to be rendered to, nor to be copied from, and is made again. */
	if (texture->format != VK_FORMAT_R8G8B8A8_UNORM || texture->extent.width != size->widthPow2
			|| texture->extent.height != size->heightPow2 || (texture->imageUsage & renderTargetUsage) != renderTargetUsage)
	{
		texture->extent.width = size->widthPow2;
		texture->extent.height = size->heightPow2;
		texture->format = VK_FORMAT_R8G8B8A8_UNORM;
		texture->needsStaging = true;
		if (!vk_texture_create_render_target(texture, renderTargetUsage))
			return VK_IMAGE_LAYOUT_MAX_ENUM;
		layout = VK_IMAGE_LAYOUT_UNDEFINED;
	}
	else
		layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	t->o.viewport.offset.x = 0;
	t->o.viewport.offset.y = 0;
	t->o.viewport.extent.width = size->widthPow2;
	t->o.viewport.extent.height = size->heightPow2;
	return layout;
}

static VkCommandBuffer rtt_new_frame(vk_oit_rtt *t)
{
	vk_base_drawer *b = &t->base;
	const rtt_size size = rtt_get_size();
	const u32 scale = vk_rtt_scale();
	const vk_oit_drawer *att;
	VkImageLayout layout;
	VkImageView views[3];
	u64 serials[3];
	VkFramebuffer framebuffer;
	VkCommandBuffer cmd;

	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d\n", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max);
	vk_oit_drawer_new_image(&t->o);
	transform_matrix_calc(&b->matrices, &pvrrc);
	t->textureAddr = FB_W_SOF1 & VRAM_MASK;

	cmd = begin_commands(b->pool);
	if (cmd == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	t->o.cmd = cmd;
	b->vertexUniformBuffer = vk_base_drawer_write_vertex_uniforms(b, cmd, t->o.imageIndex);
	if (b->vertexUniformBuffer == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	/* What the render draws: its own pixels, the texture's from the first
	 * (vk_texture_clear_outside()) */
	if (!vk_oit_buffers_init(t->o.buffers, (int)size.upscaledWidth, (int)size.upscaledHeight, b->pool))
		return VK_NULL_HANDLE;
	/* The attachments: the screen's drawer's when they are large enough -
	 * drawn into by the screen's frames and this one by one, on the same
	 * queue, each one's first render pass starting them anew -, or this
	 * drawer's own; which a render larger than both has made for it. */
	t->o.attachmentsOf = &t->o;
	if (t->screen != NULL && screen_shares_attachments(t->screen) && t->screen->o.colorAttachments[0].image != VK_NULL_HANDLE
			&& (int)size.upscaledWidth <= t->screen->o.maxWidth && (int)size.upscaledHeight <= t->screen->o.maxHeight)
	{
		t->o.attachmentsOf = &t->screen->o;
		t->drewShared++;
	}
	else
	{
		t->drewOwn++;
		if ((t->o.colorAttachments[0].image == VK_NULL_HANDLE || (int)size.upscaledWidth > t->o.maxWidth
				|| (int)size.upscaledHeight > t->o.maxHeight) && !t->saidLarger)
		{
			t->saidLarger = true;
			WARN_LOG(RENDERER, "Render to a texture of %u x %u: attachments made for it\n", size.upscaledWidth, size.upscaledHeight);
		}
		if (!vk_oit_drawer_make_attachments(&t->o, (int)size.upscaledWidth, (int)size.upscaledHeight))
			return VK_NULL_HANDLE;
	}
	att = t->o.attachmentsOf;

	layout = rtt_take_texture(t, &size, 0);
	if (layout == VK_IMAGE_LAYOUT_MAX_ENUM)
		return VK_NULL_HANDLE;
	t->o.viewport.extent.width = size.upscaledWidth;
	t->o.viewport.extent.height = size.upscaledHeight;
	vk_texture_clear_outside(t->texture, cmd, size.upscaledWidth, size.upscaledHeight, &layout);
	vk_set_image_layout(cmd, vk_texture_image(t->texture), VK_FORMAT_R8G8B8A8_UNORM, 1, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	views[0] = vk_texture_view(t->texture);
	views[1] = att->colorAttachments[0].view;
	views[2] = att->depthAttachment.view;
	serials[0] = vk_texture_view_serial(t->texture);
	serials[1] = att->colorAttachments[0].viewSerial;
	serials[2] = att->depthAttachment.viewSerial;
	/* (made again only for other attachments: a texture rendered to every frame is the same one) */
	if (!rtt_framebuffers(t, (u32)vkc.swapChainSize))
		return VK_NULL_HANDLE;
	framebuffer = vk_kept_framebuffer_get(&t->framebuffers[t->o.imageIndex], vk_oit_passes_get(&t->pipelines.passes, true, true, false, false),
			3, views, serials, size.upscaledWidth, size.upscaledHeight);
	if (framebuffer == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	set_viewport(cmd, 0.f, 0.f, (float)size.upscaledWidth, (float)size.upscaledHeight);
	/* only the pixel clip is written */
	b->baseScissor.offset.x = (s32)(size.clipX * scale);
	b->baseScissor.offset.y = (s32)(size.clipY * scale);
	b->baseScissor.extent.width = size.clipWidth * scale;
	b->baseScissor.extent.height = size.clipHeight * scale;
	vkCmdSetScissor(cmd, 0, 1, &b->baseScissor);
	return cmd;
}

/* the render is submitted: video memory gets it when something first touches it there */
static void rtt_watch(vk_oit_rtt *t)
{
	vk_texture *texture = t->texture;
	texture->tc.dirty = 0;
	texture->tc.stride_read = 0;	/* (rendered, not read) */
	texture->tc.made_type = 0;
	libCore_vramlock_Lock(texture->tc.sa_tex, texture->tc.sa + texture->tc.size - 1, &texture->tc);
	vk_rtt_watch_texture(texture, t->textureAddr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
}

static void rtt_end_frame(vk_oit_rtt *t)
{
	VkCommandBuffer cmd = t->o.cmd;
	vkCmdEndRenderPass(cmd);
	vk_oit_buffers_count_pass(t->o.buffers, cmd);
	vkEndCommandBuffer(cmd);
	t->o.cmd = VK_NULL_HANDLE;
	vk_cmd_pool_end_frame(t->base.pool);
	rtt_watch(t);
}

/* ---- The renderer ---- */

typedef struct vk_oit_renderer
{
	vk_renderer r;
	vk_oit_buffers buffers;
	vk_samplers samplers;
	vk_oit_shaders shaders;
	/* Textures are rendered to with one sample a pixel, SSAA or not: their
	 * sample attachments would be made as large as such a texture can be,
	 * more video memory than SSAA's screen attachments, and these renders'
	 * attachments are stored between render passes */
	vk_oit_shaders rttShaders;
	vk_oit_screen screen;
	/* (a quad pipeline is for one subpass: the base's is the last subpass's, this the colour one's) */
	vk_quad_pipeline lastPicturePipeline;
	vk_oit_rtt rtt;
	vk_compute_drawer compute;
} vk_oit_renderer;

static vk_oit_renderer *oit;

static bool compute_usable(void)
{
	return rend_compute_wanted && vk_compute_drawer_device_lacks() == NULL;
}

/* SSAA's samples a pixel, for the screen: as far as the device and the
 * video memory budget go for its attachments, after the lists (the
 * upscaler is the anti-aliasing then, and reads the depth, which SSAA's
 * samples are not) */
static u32 oit_ssaa_samples(void)
{
	if (upscaler_active())
		return 1;
	return vkc_ssaa_samples((u32)settings.rend.Ssaa, ssaa_per_sample_kib(vkc.width, vkc.height, 4 + vkc_depth_bytes()),
			(u32)(vk_oit_buffers_planned_size(&oit->buffers, (int)vkc.width, (int)vkc.height) >> 10), false, "Vulkan per-pixel renderer");
}

/* The compute renderer, when it is the rendering method. (Made before the
 * drawers when the renderer starts: its kernels are the first the pipeline
 * thread is given, as every frame draws with them.) */
static void oit_make_compute(void)
{
	if (rend_compute_wanted)
		/* (for the temporal upscaler, with the depth stored) */
		vk_compute_drawer_init(&oit->compute, vkc.width, vkc.height, oit->r.samples, &oit->r.texCommandPool, &oit->samplers,
				upscaler_active() && oit->r.samples == 1);
	else
		vk_compute_drawer_term(&oit->compute);
}

/* ...then the screen's drawer's motion vector pass over it */
static void oit_init_compute_motion(void)
{
	VkImageView views[16];
	u32 count = 0;
	if (rend_compute_wanted && oit->compute.pool != NULL)
	{
		const u32 most = (u32)oit->compute.pool->slotCount < 16 ? (u32)oit->compute.pool->slotCount : 16;
		count = vk_compute_drawer_depth_views(&oit->compute, views, most);
	}
	if (rend_compute_wanted)
		oit->compute.motion = screen_compute_motion_pass(&oit->screen, views, count);
	else
		screen_compute_motion_pass(&oit->screen, views, 0);
}

/* (and the textures', and what draws over it) */
static bool oit_init_screen(void)
{
	vk_renderer *r = &oit->r;
	if (!screen_init(&oit->screen, &oit->samplers, &oit->shaders, &oit->buffers, r->samples)
			|| !rtt_share_attachments(&oit->rtt, &oit->screen))
		return false;
	if (!vk_renderer_base_init(r, vk_oit_passes_get(&oit->screen.pipelines.passes, false, true, false, false),
			vk_oit_final_subpass(&oit->screen.pipelines.passes))
			|| !vk_renderer_init_overlay(r, oit->screen.writeOut.overlayPass))
		return false;
	return vk_quad_pipeline_init(&oit->lastPicturePipeline, &r->modules, vk_oit_passes_get(&oit->screen.pipelines.passes, false, true,
			false, false), 1, r->samples)
			&& screen_set_last_picture_pipeline(&oit->screen, &oit->lastPicturePipeline);
}

/* The per-pixel lists sort the translucent polygons, by depth and then by
 * polygon number: a frame decoded on the GPU numbers its strips by their
 * draws' firstInstance, and without that it has no sorted pass */
static u32 oit_ta_flags(void *self)
{
	(void)self;
	return TA_GPU_DRAW | TA_GPU_PROVOKE | (oit->r.taDecoder.instance ? TA_GPU_INSTANCE : TA_GPU_NO_SORT);
}

static void oit_set_restored_picture(void *self, VkImageView view, u32 width, u32 height)
{
	(void)self;
	oit->screen.restoredPicture = view;
	oit->screen.restoredSize.width = width;
	oit->screen.restoredSize.height = height;
}

/* Rendering Method: Compute draws the frames it can (vk_compute_drawer.h)
 * into the same pictures, and the renders to a texture into the same
 * textures. True if it drew this one (*@frame: what render_frame says). */
static bool oit_compute_frame(bool *frame)
{
	vk_compute_drawer *c = &oit->compute;
	if (!c->ready)
		return false;
	if (pvrrc.isRTT)
	{
		vk_oit_rtt *t = &oit->rtt;
		const rtt_size size = rtt_get_size();
		if (!vk_tex_pool_targets_storable(&oit->r.texPool))
			vk_compute_drawer_fallback(c, CF_WHY_RTT);
		else if (!vk_compute_drawer_can_draw(c))
			vk_compute_drawer_fallback(c, CF_WHY_DECODE);
		else if (vk_compute_drawer_prepare(c, false, false, oit->r.paletteTexture, size.widthPow2, size.heightPow2))
		{
			/* The compute renderer's render to a texture goes to the texture
			 * a render of this drawer's would, which is left as one of its
			 * is: in the cache, locked in video memory and watched. (Its own
			 * barrier makes it ready to be written, from whatever it held.) */
			bool taken;
			vk_oit_drawer_new_image(&t->o);
			t->textureAddr = FB_W_SOF1 & VRAM_MASK;
			taken = rtt_take_texture(t, &size, VK_IMAGE_USAGE_STORAGE_BIT) != VK_IMAGE_LAYOUT_MAX_ENUM;
			if (!taken)
				ERROR_LOG(RENDERER, "Vulkan: a texture could not be made for a render to it\n");
			else
			{
				t->texture->rttArea = 0;	/* (written as the compute renderer writes it: vk_texture_clear_outside()) */
				if (!vk_compute_drawer_record(c, vk_texture_image(t->texture), vk_texture_view(t->texture), VK_NULL_HANDLE))
					ERROR_LOG(RENDERER, "Vulkan: the compute renderer's kernels could not be made\n");
			}
			vk_cmd_pool_end_frame(t->base.pool);
			if (taken)
				rtt_watch(t);
			*frame = false;
			return true;
		}
	}
	else
	{
		vk_oit_screen *s = &oit->screen;
		bool last, scaled;
		if (!vk_compute_drawer_can_draw(c))
			vk_compute_drawer_fallback(c, CF_WHY_DECODE);
		else if (vk_osd_drawing(&oit->r.osd))
			vk_compute_drawer_fallback(c, CF_WHY_OSD);
		else
		{
			screen_compute_start(s, &last, &scaled);
			if (vk_compute_drawer_prepare(c, last, scaled, oit->r.paletteTexture, 0, 0))
			{
				const int slot = vk_compute_drawer_slot(c);
				const int count = (int)s->images;
				VkImageView before = VK_NULL_HANDLE;
				/* (the upscaler's strips matched once the frame is this
				 * renderer's, jittered as this drawer's matrices are here: the
				 * raster drawer matches its own) */
				if (s->o.upscaled && vk_motion_pass_begin(&s->computeMotionPass, slot))
				{
					float jx, jy;
					upscaler_jitter(&jx, &jy);
					transform_matrix_jitter(&s->base.matrices, jx, jy);
					vk_base_drawer_write_vertex_uniforms(&s->base, VK_NULL_HANDLE, slot);
				}
				/* the picture it draws the frame in */
				vk_oit_drawer_new_image(&s->o);
				if (last)
					before = scaled ? s->restoredPicture : s->finalColors[(s->o.imageIndex + count - 1) % count].view;
				if (!vk_compute_drawer_record(c, s->finalColors[s->o.imageIndex].image, s->finalColors[s->o.imageIndex].view, before))
					ERROR_LOG(RENDERER, "Vulkan: the compute renderer's kernels could not be made\n");
				/* ...and once it is recorded, as the drawer ends one of its own */
				screen_have_picture(s);
				screen_present(s, vk_compute_drawer_sums(c));
				/* the vectors and the depth to the upscaler */
				if (s->computeMotionPass.recorded)
					vk_motion_pass_submit(&s->computeMotionPass, slot, vk_compute_drawer_depth_image(c), vk_compute_drawer_depth_view(c),
							VK_FORMAT_R32_SFLOAT);
				*frame = true;
				return true;
			}
		}
	}
	return false;
}

static bool oit_render_frame(void *self)
{
	vk_renderer *r = &oit->r;
	VkImageView fogView, paletteView;
	bool frame, after;
	(void)self;
	if (pvrrc.isRenderFramebuffer)
		return true;
	if (pvrrc.isRTT && (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min))
	{
		/* A pixel clip that ends before it starts holds no pixel: the chip
		 * writes nothing. (Its size would be negative here.) */
		WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d\n",
				pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
		return false;
	}
	if (oit_compute_frame(&frame))
		return frame;

	fogView = vk_texture_view(r->fogTexture);
	paletteView = vk_texture_view(r->paletteTexture);
	if (pvrrc.isRTT)
	{
		VkCommandBuffer cmd;
		vk_renderer_set_parsed(r, &oit->rtt.base);
		cmd = rtt_new_frame(&oit->rtt);
		if (cmd == VK_NULL_HANDLE || !vk_oit_drawer_draw(&oit->rtt.o, cmd, fogView, paletteView))
			ERROR_LOG(RENDERER, "Vulkan: a render to a texture could not be drawn\n");
		if (oit->rtt.o.cmd != VK_NULL_HANDLE)
			rtt_end_frame(&oit->rtt);
		else
			vk_cmd_pool_end_frame(&r->texCommandPool);
		return false;
	}

	vk_renderer_set_parsed(r, &oit->screen.base);
	{
		const VkCommandBuffer cmd = screen_new_frame(&oit->screen);
		if (cmd == VK_NULL_HANDLE || !vk_oit_drawer_draw(&oit->screen.o, cmd, fogView, paletteView))
			ERROR_LOG(RENDERER, "Vulkan: the frame could not be drawn\n");
	}
	if (oit->screen.o.cmd == VK_NULL_HANDLE)
	{
		vk_cmd_pool_end_frame(&r->texCommandPool);
		return false;
	}
	/* (the chip's write-out before what goes over the picture, which then
	 * goes in a render pass of its own: writeout.h) */
	after = vk_osd_drawing(&r->osd) && screen_writeout_before_osd(&oit->screen);
	if (!vk_osd_draw(&r->osd, oit->screen.o.cmd, (u32)screen_width, (u32)screen_height, &r->texCommandPool,
			after ? &r->overlayQuadPipeline : NULL))
		ERROR_LOG(RENDERER, "Vulkan: the OSD's pipeline could not be made\n");
	if (!screen_end_frame(&oit->screen))
		ERROR_LOG(RENDERER, "Vulkan: the frame's motion vectors could not be drawn\n");
	return true;
}

static const vk_renderer_kind oit_kind = { oit_render_frame, oit_ta_flags, oit_set_restored_picture };

static void oit_create(void)
{
	oit = (vk_oit_renderer *)calloc(1, sizeof(*oit));
	if (oit == NULL)
		return;
	vk_renderer_construct(&oit->r, &oit_kind, oit);
	vk_oit_buffers_construct(&oit->buffers);
	vk_oit_shaders_construct(&oit->shaders);
	vk_oit_shaders_construct(&oit->rttShaders);
	screen_construct(&oit->screen);
	vk_quad_pipeline_construct(&oit->lastPicturePipeline);
	rtt_construct(&oit->rtt);
	vk_compute_drawer_construct(&oit->compute);
}

static void oit_release(void)
{
	if (oit == NULL)
		return;
	vk_compute_drawer_term(&oit->compute);
	rtt_free(&oit->rtt);
	vk_quad_pipeline_term(&oit->lastPicturePipeline);
	screen_free(&oit->screen);
	vk_oit_shaders_term(&oit->rttShaders);
	vk_oit_shaders_term(&oit->shaders);
	vk_samplers_term(&oit->samplers);
	vk_oit_buffers_free(&oit->buffers);
	vk_renderer_free(&oit->r);
	free(oit);
	oit = NULL;
}

static bool oit_init(void)
{
	vk_renderer *r;
	DEBUG_LOG(RENDERER, "OITVulkanRenderer::Init\n");
	if (oit == NULL)
		return false;
	r = &oit->r;
	vk_oit_buffers_set_compact(&oit->buffers, compute_usable());
	r->samples = oit_ssaa_samples();
	vk_samplers_init(&oit->samplers);
	/* (with SSAA and the HDR output, the last pass's channel sums, for the encode) */
	oit->buffers.sums = r->samples > 1 && vkc_hdr_sums_ready();
	if (!vk_oit_buffers_init(&oit->buffers, 0, 0, &r->texCommandPool))
		return false;
	oit->shaders.samples = r->samples;
	/* (the compute renderer's slots are the pool's: made now, the base's init finds them made) */
	if (!vk_cmd_pool_init(&r->texCommandPool))
		return false;
	oit_make_compute();
	NOTICE_LOG(RENDERER, "Vulkan per-pixel renderer: with SSAA, a translucent fragment takes the samples %s\n",
			vkc.postDepthCoverage ? "the depth test passed (VK_EXT_post_depth_coverage)" : "its own depth test passes, at each sample");
	oit->rtt.base.pool = &r->texCommandPool;
	if (!rtt_init(&oit->rtt, &oit->samplers, &oit->rttShaders, &r->textureCache, &oit->buffers))
		return false;
	/* (renders to a texture drawn by the compute renderer go to the pool's
	 * images, which it then writes as storage images) */
	vk_tex_pool_set_target_storage(&r->texPool, compute_usable());
	oit->rtt.base.gpuDraw = &r->taDecoder.draw;

	oit->screen.storagePicture = rend_compute_wanted;
	oit->screen.base.pool = &r->texCommandPool;
	oit->screen.base.gpuDraw = &r->taDecoder.draw;
	oit->compute.gpuDraw = &r->taDecoder.draw;
	if (!oit_init_screen())
		return false;
	oit->rtt.storageUsage = vk_tex_pool_targets_storable(&r->texPool) ? VK_IMAGE_USAGE_STORAGE_BIT : 0;
	oit_init_compute_motion();
	return true;
}

static void oit_resize(int w, int h)
{
	vk_renderer *r = &oit->r;
	const u32 was = r->samples;
	NOTICE_LOG(RENDERER, "OIT Resize %d x %d\n", w, h);
	/* what is replaced below may be in use by frames still on the GPU */
	vk_cmd_pool_wait_all(&r->texCommandPool);
	vk_oit_buffers_set_compact(&oit->buffers, compute_usable());
	r->samples = oit_ssaa_samples();
	oit->buffers.sums = r->samples > 1 && vkc_hdr_sums_ready();
	if (r->samples != was)
	{
		/* (a change of SSAA: the shaders, render passes and pipelines of the other count go) */
		oit->shaders.samples = r->samples;
		vk_quad_pipeline_forget(&r->quadPipeline);
		vk_quad_pipeline_forget(&oit->lastPicturePipeline);
	}
	if (!oit_init_screen())
		ERROR_LOG(RENDERER, "Vulkan: the per-pixel screen drawer could not be made again\n");
	oit_make_compute();
	oit_init_compute_motion();
	/* what this size no longer needs is given back once the frames that used it are done */
	vkc_heap_trim_soon();
}

static void oit_term(void)
{
	DEBUG_LOG(RENDERER, "VulkanRenderer::Term\n");
	vkc_log_vram_usage();
	/* (the frontend lets go of the pictures before they go) */
	vkc_release_picture();
	vk_cmd_pool_wait_all(&oit->r.texCommandPool);
	vk_renderer_drop_quads(&oit->r);
	vk_compute_drawer_term(&oit->compute);
	vk_samplers_term(&oit->samplers);
	screen_term(&oit->screen);
	vk_quad_pipeline_term(&oit->lastPicturePipeline);
	rtt_term(&oit->rtt);
	/* (after the drawers: the pipeline thread may be making pipelines of
	 * these shaders for them until their pipelines go) */
	vk_oit_shaders_term(&oit->shaders);
	vk_oit_shaders_term(&oit->rttShaders);
	vk_oit_buffers_term(&oit->buffers);
	vk_renderer_term(&oit->r);
}

static bool oit_process(TA_context *ctx) { return vk_renderer_process(&oit->r, ctx); }
static bool oit_render(void) { return vk_renderer_render(&oit->r); }
static void oit_keep_picture(void) { screen_keep_picture(&oit->screen); }
static u64 oit_get_texture(u32 tsp, u32 tcw, int area) { return vk_renderer_get_texture(&oit->r, tsp, tcw, area); }
static void oit_options_changed(void) { vk_renderer_options_changed(&oit->r); }
static void oit_present(void) { }
static bool oit_present_kept_picture(void) { return vk_renderer_present_kept_picture(&oit->r); }
static bool oit_ready(void) { return vk_pso_worker_batch_ready(); }

static const renderer_ops oit_ops =
{
	oit_init,
	oit_resize,
	oit_term,
	oit_process,
	oit_render,
	oit_keep_picture,
	oit_get_texture,
	oit_options_changed,
	oit_create,
	oit_release,
	oit_present,
	oit_present_kept_picture,
	oit_ready
};

const renderer_ops *rend_OITVulkan(void)
{
	return &oit_ops;
}
