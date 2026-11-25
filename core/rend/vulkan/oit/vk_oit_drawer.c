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
#include <stdlib.h>
#include <string.h>
#include "vk_oit_drawer.h"
#include "../pso.h"
#include "../vk_quad.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta_words.h"
#include "rend/TexCache.h"
#include "rend/modvol_clip.h"

#define PPW(w, f) TAW_GET(w, f)

#define MAIN_BUFFER_USAGE (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT \
		| VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
#define MAIN_BUFFER_PROPS (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

static u32 align_pad(VkDeviceSize offset, u32 alignment)
{
	return (u32)(alignment - (offset & (alignment - 1)));
}

void vk_oit_drawer_construct(vk_oit_drawer *d, vk_base_drawer *b, const vk_oit_drawer_kind *kind, void *self)
{
	memset(d, 0, sizeof(*d));
	d->b = b;
	d->kind = kind;
	d->self = self;
	d->bands = 1;
	d->attachmentsOf = d;
	d->attachmentSamples = 1;
	d->colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
}

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

static void temp_framebuffers_defer(vk_oit_drawer *d, vk_cmd_pool *pool)
{
	u32 i;
	for (i = 0; i < d->tempFramebufferCount; i++)
	{
		defer_framebuffer(pool, &d->tempFramebuffers[i][0]);
		defer_framebuffer(pool, &d->tempFramebuffers[i][1]);
	}
	free(d->tempFramebuffers);
	d->tempFramebuffers = NULL;
	d->tempFramebufferCount = 0;
}

static void depth_attachments_defer(vk_oit_drawer *d, vk_cmd_pool *pool)
{
	u32 i;
	for (i = 0; i < d->depthAttachmentCount; i++)
		vk_attachment_defer(pool, &d->depthAttachments[i]);
	free(d->depthAttachments);
	d->depthAttachments = NULL;
	d->depthAttachmentCount = 0;
}

/* ---- Set-up ---- */

static void free_main_buffers(vk_oit_drawer *d)
{
	u32 i;
	for (i = 0; i < d->mainBufferCount; i++)
		vk_buffer_free(&d->mainBuffers[i]);
	free(d->mainBuffers);
	d->mainBuffers = NULL;
	d->mainBufferCount = 0;
}

/* this image's main buffer, at least @size bytes */
static vk_buffer *main_buffer(vk_oit_drawer *d, u32 size)
{
	vk_buffer *buffer;
	if (d->mainBufferCount == 0)
	{
		const u32 count = (u32)vkc.swapChainSize;
		d->mainBuffers = (vk_buffer *)calloc(count, sizeof(vk_buffer));
		if (d->mainBuffers == NULL)
			return NULL;
		for (; d->mainBufferCount < count; d->mainBufferCount++)
			if (vk_buffer_init(&d->mainBuffers[d->mainBufferCount], size > 512 * 1024u ? size : 512 * 1024u,
					MAIN_BUFFER_USAGE, MAIN_BUFFER_PROPS) != VK_SUCCESS)
				return NULL;
	}
	buffer = &d->mainBuffers[d->imageIndex];
	if (buffer->size < size)
	{
		u32 newSize = (u32)buffer->size;
		while (newSize < size)
			newSize *= 2;
		INFO_LOG(RENDERER, "Increasing main buffer size %d -> %d\n", (u32)buffer->size, newSize);
		vk_buffer_free(buffer);
		if (vk_buffer_init(buffer, newSize, MAIN_BUFFER_USAGE, MAIN_BUFFER_PROPS) != VK_SUCCESS)
			return NULL;
	}
	return buffer;
}

bool vk_oit_drawer_init(vk_oit_drawer *d, vk_samplers *samplers, vk_oit_pipeline_mgr *pipelines, vk_oit_buffers *buffers)
{
	const u32 size = (u32)vkc.swapChainSize;
	d->pipelines = pipelines;
	d->samplers = samplers;
	d->buffers = buffers;
	if (d->quad.buffer == VK_NULL_HANDLE && vk_buffer_init(&d->quad, sizeof(QuadVertex) * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
		return false;
	while (d->descSetCount > size)
		vk_oit_desc_sets_term(&d->descSets[--d->descSetCount]);
	if (d->descSetCount < size)
	{
		vk_oit_desc_sets *more = (vk_oit_desc_sets *)realloc(d->descSets, size * sizeof(vk_oit_desc_sets));
		if (more == NULL)
			return false;
		d->descSets = more;
		for (; d->descSetCount < size; d->descSetCount++)
		{
			vk_oit_desc_sets *s = &more[d->descSetCount];
			vk_oit_desc_sets_construct(s);
			vk_oit_desc_sets_init(s, samplers, pipelines->pipelineLayout, pipelines->perFrameLayout, pipelines->perPolyLayout,
					pipelines->colorInputLayout);
		}
	}
	/* (a frame's buffers made now, not when it first draws: a render to a
	 * texture may come long after the game started) */
	return vk_base_drawer_make_vertex_uniforms(d->b, size) && main_buffer(d, 0) != NULL;
}

void vk_oit_drawer_term(vk_oit_drawer *d)
{
	u32 i;
	vk_buffer_free(&d->quad);
	memset(&d->quad, 0, sizeof(d->quad));
	vk_attachment_free(&d->colorAttachments[0]);
	vk_attachment_free(&d->colorAttachments[1]);
	vk_attachment_free(&d->depthAttachment);
	memset(d->colorAttachments, 0, sizeof(d->colorAttachments));
	memset(&d->depthAttachment, 0, sizeof(d->depthAttachment));
	temp_framebuffers_defer(d, NULL);
	depth_attachments_defer(d, NULL);
	free_main_buffers(d);
	vk_base_drawer_free_vertex_uniforms(d->b);
	for (i = 0; i < d->descSetCount; i++)
		vk_oit_desc_sets_term(&d->descSets[i]);
	free(d->descSets);
	d->descSets = NULL;
	d->descSetCount = 0;
}

/* ---- Attachments ---- */

/* SSAA: one image of samples for the colour, one for the depth, for all the
 * render passes of a frame (vk_make_ssaa_render_pass()), of the largest
 * size asked for. Not transient: a frame of several render passes stores
 * them for the next one (and a device may drop what is stored to an
 * attachment made transient). */
static bool make_sample_attachments(vk_oit_drawer *d)
{
	vk_cmd_pool *pool = d->b->pool;
	const VkSampleCountFlagBits n = (VkSampleCountFlagBits)d->pipelines->passes.samples;
	vk_attachment_defer(pool, &d->colorAttachments[0]);
	vk_attachment_defer(pool, &d->colorAttachments[1]);
	temp_framebuffers_defer(d, pool);
	depth_attachments_defer(d, pool);
	if (!vk_attachment_init(&d->colorAttachments[0], (u32)d->maxWidth, (u32)d->maxHeight, d->colorFormat,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, true, n))
		return false;
	vk_attachment_defer(pool, &d->depthAttachment);
	if (!vk_attachment_init(&d->depthAttachment, (u32)d->maxWidth, (u32)d->maxHeight, vkc.depthFormat,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, true, n))
		return false;
	d->needDepthTransition = true;
	return true;
}

bool vk_oit_drawer_make_attachments(vk_oit_drawer *d, int width, int height)
{
	vk_cmd_pool *pool = d->b->pool;
	const u32 samples = d->pipelines->passes.samples;
	const u32 images = d->upscaled ? (u32)vkc.swapChainSize : 1;
	VkFramebufferCreateInfo createInfo;
	VkImageView attachments[3];
	u32 i;
	/* (made again for another count of samples) */
	if (samples != d->attachmentSamples || d->upscaled != d->attachmentsUpscaled)
	{
		d->maxWidth = d->maxHeight = 0;
		d->attachmentSamples = samples;
		d->attachmentsUpscaled = d->upscaled;
	}
	if (width <= d->maxWidth && height <= d->maxHeight)
		return true;
	if (width > d->maxWidth)
		d->maxWidth = width;
	if (height > d->maxHeight)
		d->maxHeight = height;

	if (samples > 1)
		return make_sample_attachments(d);

	for (i = 0; i < 2; i++)
	{
		vk_attachment_defer(pool, &d->colorAttachments[i]);
		if (!vk_attachment_init(&d->colorAttachments[i], (u32)d->maxWidth, (u32)d->maxHeight, d->colorFormat,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, true, VK_SAMPLE_COUNT_1_BIT))
			return false;
	}
	vk_attachment_defer(pool, &d->depthAttachment);
	depth_attachments_defer(d, pool);
	if (d->upscaled)
	{
		/* the frame's depth, kept, sampled and copied from (the frontend reads it as it likes) */
		d->depthAttachments = (vk_attachment *)calloc(images, sizeof(vk_attachment));
		if (d->depthAttachments == NULL)
			return false;
		for (; d->depthAttachmentCount < images; d->depthAttachmentCount++)
			if (!vk_attachment_init(&d->depthAttachments[d->depthAttachmentCount], (u32)d->maxWidth, (u32)d->maxHeight, vkc.depthFormat,
					VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
					| VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true, VK_SAMPLE_COUNT_1_BIT))
				return false;
	}
	else if (!vk_attachment_init(&d->depthAttachment, (u32)d->maxWidth, (u32)d->maxHeight, vkc.depthFormat,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, true, VK_SAMPLE_COUNT_1_BIT))
		return false;
	d->needDepthTransition = true;

	temp_framebuffers_defer(d, pool);
	d->tempFramebuffers = (VkFramebuffer (*)[2])calloc(images, sizeof(*d->tempFramebuffers));
	if (d->tempFramebuffers == NULL)
		return false;
	d->tempFramebufferCount = images;
	memset(&createInfo, 0, sizeof(createInfo));
	createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	createInfo.renderPass = vk_oit_passes_get(&d->pipelines->passes, true, true, false, false);
	if (createInfo.renderPass == VK_NULL_HANDLE)
		return false;
	createInfo.attachmentCount = 3;
	createInfo.pAttachments = attachments;
	createInfo.width = (u32)width;
	createInfo.height = (u32)height;
	createInfo.layers = 1;
	for (i = 0; i < images; i++)
	{
		attachments[0] = d->colorAttachments[1].view;
		attachments[1] = d->colorAttachments[0].view;
		attachments[2] = d->upscaled ? d->depthAttachments[i].view : d->depthAttachment.view;
		if (vkCreateFramebuffer(vkc.device, &createInfo, NULL, &d->tempFramebuffers[i][0]) != VK_SUCCESS)
			return false;
		attachments[0] = attachments[1];
		attachments[1] = d->colorAttachments[1].view;
		if (vkCreateFramebuffer(vkc.device, &createInfo, NULL, &d->tempFramebuffers[i][1]) != VK_SUCCESS)
			return false;
	}
	return true;
}

void vk_oit_drawer_drop_attachments(vk_oit_drawer *d)
{
	vk_cmd_pool *pool = d->b->pool;
	vk_attachment_defer(pool, &d->colorAttachments[0]);
	vk_attachment_defer(pool, &d->colorAttachments[1]);
	vk_attachment_defer(pool, &d->depthAttachment);
	depth_attachments_defer(d, pool);
	temp_framebuffers_defer(d, pool);
	d->maxWidth = d->maxHeight = 0;
}

bool vk_oit_drawer_make_buffers(vk_oit_drawer *d, int width, int height)
{
	return vk_oit_buffers_init(d->buffers, width, height, d->b->pool) && vk_oit_drawer_make_attachments(d, width, height);
}

/* ---- The frame ---- */

void vk_oit_drawer_set_scissor(vk_oit_drawer *d, VkCommandBuffer cmd, const VkRect2D *scissor)
{
	s32 top, bottom, end;
	VkRect2D r;
	if (d->band.extent.height == 0)
	{
		vk_base_drawer_set_scissor(d->b, cmd, scissor);
		return;
	}
	top = scissor->offset.y > d->band.offset.y ? scissor->offset.y : d->band.offset.y;
	end = scissor->offset.y + (s32)scissor->extent.height;
	if (end > d->band.offset.y + (s32)d->band.extent.height)
		end = d->band.offset.y + (s32)d->band.extent.height;
	bottom = top > end ? top : end;
	r.offset.x = scissor->offset.x;
	r.offset.y = top;
	r.extent.width = scissor->extent.width;
	r.extent.height = (u32)(bottom - top);
	vk_base_drawer_set_scissor(d->b, cmd, &r);
}

/* The translucent modifier volumes' draws change the per-pixel lists from
 * their shaders (oit_trmodvol.frag), and nothing orders what the shaders of
 * two draws do to the same fragment but a barrier between them: a GPU runs
 * the draws of a subpass at once, in any order, where a software device
 * runs them one after the other. They go through here. Built with
 * RACE_TEST (tools/rend/speckle_test.sh; never in a release) the draws
 * between two barriers are recorded a triangle at a time, the draws'
 * triangles taken in turn, the last draw's first: one draw's shaders run
 * while another's are half done, as on a GPU, and a picture that changes
 * then depends on an order nothing keeps. */
static void list_draws_record(vk_oit_drawer *d, VkCommandBuffer cmd, vk_pso_cull *cull, VkPipeline pipeline, u32 cullIndex,
		const VkRect2D *scissor, u32 vertices, u32 first)
{
	vk_oit_drawer_set_scissor(d, cmd, scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vk_pso_cull_set(cull, cullIndex);
	vkCmdDraw(cmd, vertices, 1, first, 0);
}

static void list_draws_flush(vk_oit_drawer *d, VkCommandBuffer cmd, vk_pso_cull *cull)
{
#ifdef RACE_TEST
	u32 v, more, i;
	for (v = 0, more = d->heldCount; more != 0; v += 3)
	{
		more = 0;
		for (i = d->heldCount; i-- > 0; )
			if (v < d->held[i].vertices)
			{
				list_draws_record(d, cmd, cull, d->held[i].pipeline, d->held[i].cullIndex, &d->held[i].scissor, 3, d->held[i].first + v);
				more++;
			}
	}
	d->heldCount = 0;
#else
	(void)d;
	(void)cmd;
	(void)cull;
#endif
}

/* @cullIndex: PsoCullIndex()'s */
static void list_draws_draw(vk_oit_drawer *d, VkCommandBuffer cmd, vk_pso_cull *cull, VkPipeline pipeline, u32 cullIndex,
		const VkRect2D *scissor, u32 vertices, u32 first)
{
#ifdef RACE_TEST
	vk_oit_held_draw *h;
	if (d->heldCount == sizeof(d->held) / sizeof(d->held[0]))
		list_draws_flush(d, cmd, cull);
	h = &d->held[d->heldCount++];
	h->pipeline = pipeline;
	h->cullIndex = cullIndex;
	h->scissor = *scissor;
	h->vertices = vertices;
	h->first = first;
#else
	list_draws_record(d, cmd, cull, pipeline, cullIndex, scissor, vertices, first);
#endif
}

/* what the draws before did to the lists is done before the draws after start */
static void list_draws_barrier(vk_oit_drawer *d, VkCommandBuffer cmd, vk_pso_cull *cull)
{
	VkMemoryBarrier barrier;
	list_draws_flush(d, cmd, cull);
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT,
			1, &barrier, 0, NULL, 0, NULL);
}

static void draw_poly(vk_oit_drawer *d, VkCommandBuffer cmd, u32 listType, bool autosort, int pass, const PolyParam *poly, u32 first,
		u32 count)
{
	vk_base_drawer *b = d->b;
	VkRect2D scissorRect;
	const int tileClip = vk_base_drawer_tile_clip(b, poly->tileclip, &scissorRect);
	const bool twoVolumes = poly->tsp1 != (u32)-1 || poly->tcw1 != (u32)-1;
	const bool keepAll = twoVolumes || PPW(poly->tsp, TSP_SrcSelect);
	const bool palette = tex_gpu_paletted(poly->tsp, poly->tcw, 0);
	const bool needTexture = PPW(poly->pcw, PCW_Texture) != 0;
	float trilinearAlpha = 1.f;
	vk_oit_push_constants pushConstants;
	VkDescriptorSet texSet = VK_NULL_HANDLE;
	PsoChoice pso;
	vk_oit_drawer_set_scissor(d, cmd, tileClip == TILECLIP_OUTSIDE ? &scissorRect : &b->baseScissor);

	if (PPW(poly->tsp, TSP_FilterMode) > 1 && needTexture && listType != ListType_Punch_Through && PPW(poly->tcw, TCW_MipMapped) == 1)
	{
		trilinearAlpha = 0.25f * (float)(PPW(poly->tsp, TSP_MipMapD) & 0x3);
		if (PPW(poly->tsp, TSP_FilterMode) == 2)
			/* Trilinear pass A */
			trilinearAlpha = 1.0f - trilinearAlpha;
	}

	memset(&pushConstants, 0, sizeof(pushConstants));
	pushConstants.clipTest[0] = (float)scissorRect.offset.x;
	pushConstants.clipTest[1] = (float)scissorRect.offset.y;
	pushConstants.clipTest[2] = (float)scissorRect.offset.x + (float)scissorRect.extent.width;
	pushConstants.clipTest[3] = (float)scissorRect.offset.y + (float)scissorRect.extent.height;
	/* The per-pixel pass drops a fragment whose blend would leave the pixel
	 * as it is. It cannot tell that when the source is the second
	 * accumulation buffer, or when the polygon has a second volume and
	 * which blend applies is known only when the lists are put together:
	 * those get a blend (8) that none of its cases take. */
	pushConstants.blend_mode0[0] = keepAll ? 8 : (int)PPW(poly->tsp, TSP_SrcInstr);
	pushConstants.blend_mode0[1] = keepAll ? 8 : (int)PPW(poly->tsp, TSP_DstInstr);
	pushConstants.trilinearAlpha = trilinearAlpha;
	/* (a frame decoded on the GPU numbers its strips by their draws: see TA_GPU_INSTANCE) */
	pushConstants.pp_Number = listType == ListType_Translucent && !pvrrc.gpu
			? (int)(poly - TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam)) : 0;
	if (palette)
	{
		if (PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_4BPP)
			pushConstants.palette_index = ((float)(PPW(poly->tcw, TCW_PalSelect) << 4) + 0.5f) / 1024.f;
		else
			pushConstants.palette_index = ((float)((PPW(poly->tcw, TCW_PalSelect) >> 4) << 8) + 0.5f) / 1024.f;
	}
	if (twoVolumes)
	{
		pushConstants.blend_mode1[0] = (int)PPW(poly->tsp1, TSP_SrcInstr);
		pushConstants.blend_mode1[1] = (int)PPW(poly->tsp1, TSP_DstInstr);
		pushConstants.shading_instr0 = (int)PPW(poly->tsp, TSP_ShadInstr);
		pushConstants.shading_instr1 = (int)PPW(poly->tsp1, TSP_ShadInstr);
		pushConstants.fog_control0 = (int)PPW(poly->tsp, TSP_FogCtrl);
		pushConstants.fog_control1 = (int)PPW(poly->tsp1, TSP_FogCtrl);
		pushConstants.use_alpha0 = (int)PPW(poly->tsp, TSP_UseAlpha);
		pushConstants.use_alpha1 = (int)PPW(poly->tsp1, TSP_UseAlpha);
		pushConstants.ignore_tex_alpha0 = (int)PPW(poly->tsp, TSP_IgnoreTexA);
		pushConstants.ignore_tex_alpha1 = (int)PPW(poly->tsp1, TSP_IgnoreTexA);
	}
	pso = vk_oit_pipeline_mgr_get(d->pipelines, listType, autosort, poly, pass);
	if (pso.pipeline == VK_NULL_HANDLE)
		return;
	/* (the general pipeline is told the polygon's key too: pso.h) */
	pushConstants.key = (int)(pso.key & ((1u << OIT_SC_BITS) - 1));
	vkCmdPushConstants(cmd, d->pipelines->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			pso.general ? (u32)sizeof(pushConstants) : (u32)OIT_PUSH_KEY_OFFSET, &pushConstants);

	if (needTexture)
		texSet = vk_oit_desc_sets_set_texture(vk_oit_drawer_desc_set(d), poly->texid, poly->tsp, poly->texid1, poly->tsp1);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso.pipeline);
	if (pso.general)
		vk_oit_pipeline_mgr_set_dynamic_state(d->pipelines, cmd, pso.key);
	if (needTexture)
		vk_oit_desc_sets_bind_per_poly(vk_oit_drawer_desc_set(d), cmd, texSet);

	vk_base_drawer_draw_indices(b, cmd, listType, first, count);
}

static void draw_list(vk_oit_drawer *d, VkCommandBuffer cmd, u32 listType, bool sortTriangles, int pass, const PolyParam *polys,
		u32 first, u32 last)
{
	const PolyParam *pp_end = polys + last;
	const PolyParam *pp;
	for (pp = polys + first; pp != pp_end; pp++)
		if (pp->count > 2)
			draw_poly(d, cmd, listType, sortTriangles, pass, pp, pp->first, pp->count);
}

static void draw_modvols(vk_oit_drawer *d, VkCommandBuffer cmd, bool translucent, int first, int count,
		const ModifierVolumeParam *modVolParams)
{
	vk_base_drawer *b = d->b;
	const ModifierVolumeParam *params = &modVolParams[first];
	int mod_base = -1;
	int cmv;
	vk_pso_cull cull;

	if (count == 0 || vk_base_drawer_modvol_triangles() == 0)
		return;

	vk_base_drawer_bind_modvol_triangles(b, cmd, d->mainBuffers[d->imageIndex].buffer, d->offsets.modVolOffset);
	vk_oit_drawer_set_scissor(d, cmd, &b->baseScissor);
	vk_pso_cull_init(&cull, cmd, d->pipelines->cullSetter);

	for (cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam *param = &params[cmv];
		const u32 mv_mode = PPW(param->isp, ISP_Modvol_DepthMode);
		const u32 cullMode = PPW(param->isp, ISP_Modvol_CullMode);
		const u32 cullIndex = PsoCullIndex(cullMode);
		const int mode = !PPW(param->isp, ISP_Modvol_VolumeLast) && mv_mode > 0 ? VK_MODVOL_OR : VK_MODVOL_XOR;
		u32 mvFirst, mvCount;
		VkRect2D scissor, clipRect;
		VkPipeline pipeline;
		mv_clip_range_get(&pvrrc, modVolParams, param, &mvFirst, &mvCount);

		if (mvCount == 0)
			continue;

		/* clipped like a polygon, to the rectangle it was sent under if it asked to be */
		scissor = b->baseScissor;
		if (vk_base_drawer_tile_clip(b, param->tileclip, &clipRect) == TILECLIP_OUTSIDE)
			scissor = clipRect;

		if (mod_base == -1)
			mod_base = (int)mvFirst;

		/* OR'ing (open volume or quad), or XOR'ing (closed volume) */
		if (translucent)
			list_draws_draw(d, cmd, &cull, vk_oit_pipeline_mgr_trmodvol(d->pipelines, mode, cullMode), cullIndex, &scissor,
					mvCount * 3, mvFirst * 3);
		else
		{
			vk_oit_drawer_set_scissor(d, cmd, &scissor);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_oit_pipeline_mgr_modvol(d->pipelines, mode, cullMode));
			vk_pso_cull_set(&cull, cullIndex);
			vkCmdDraw(cmd, mvCount * 3, 1, mvFirst * 3, 0);
		}

		if (mv_mode == 1 || mv_mode == 2)
		{
			/* Sum the area */
			const int sum = mv_mode == 1 ? VK_MODVOL_INCLUSION : VK_MODVOL_EXCLUSION;
			if (translucent)
			{
				list_draws_barrier(d, cmd, &cull);
				pipeline = vk_oit_pipeline_mgr_trmodvol(d->pipelines, sum, cullMode);
				list_draws_draw(d, cmd, &cull, pipeline, cullIndex, &scissor, (mvFirst + mvCount - (u32)mod_base) * 3, (u32)mod_base * 3);
				/* ...and done before the next volume's draws start: this
				 * draw clears the stencil bit the next volume's toggle, and
				 * sets the fragments it finds inside from what it read */
				list_draws_barrier(d, cmd, &cull);
			}
			else
			{
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_oit_pipeline_mgr_modvol(d->pipelines, sum, cullMode));
				vk_pso_cull_set(&cull, cullIndex);
				vkCmdDraw(cmd, (mvFirst + mvCount - (u32)mod_base) * 3, 1, (u32)mod_base * 3, 0);
			}
			mod_base = -1;
		}
	}
	list_draws_flush(d, cmd, &cull);
	vk_oit_drawer_set_scissor(d, cmd, &b->baseScissor);
	vk_base_drawer_bind_vertices(b, cmd);
}

static bool upload_main_buffer(vk_oit_drawer *d, const FragmentShaderUniforms *fragmentUniforms)
{
	/* (on the stack: nothing allocated for them) */
	const void *chunks[10];
	u32 chunkSizes[10];
	u32 nchunks = 0;
	/* (a frame decoded on the GPU has its geometry and translucent polygon
	 * parameters there; one the parse wrote where the card reads it, its
	 * geometry there) */
	const bool here = vk_base_drawer_use_parsed(d->b, 0);
	const u32 vertexBytes = pvrrc.gpu || here ? 0 : TA_LIST_BYTES(pvrrc.verts);
	const u32 indexBytes = pvrrc.gpu || here ? 0 : TA_LIST_BYTES(pvrrc.idx);
	const u32 uniformAlign = vkc.uniformBufferAlignment > 4 ? (u32)vkc.uniformBufferAlignment : 4;
	const u32 storageAlign = vkc.storageBufferAlignment > 4 ? (u32)vkc.storageBufferAlignment : 4;
	/* (written below, straight into the buffer: two words a translucent
	 * polygon, or the one the validation layers want when there is none) */
	const u32 trPolys = pvrrc.gpu ? 0 : (u32)TA_LIST_USED(pvrrc.global_param_tr, PolyParam);
	u32 modVolBytes = 0;
	const ModTriangle *modVols = vk_base_drawer_modvol_list(&modVolBytes);
	u32 padding, totalSize;
	u32 *trPolyParams;
	vk_buffer *buffer;
	if (!vk_base_drawer_modvols_on_cpu() || here)
		modVolBytes = 0;

	/* Vertex */
	chunks[nchunks] = TA_LIST_HEAD(pvrrc.verts, Vertex);
	chunkSizes[nchunks++] = vertexBytes;
	padding = align_pad(vertexBytes, 4);
	d->offsets.modVolOffset = vertexBytes + padding;
	chunks[nchunks] = NULL;
	chunkSizes[nchunks++] = padding;
	/* Modifier Volumes */
	chunks[nchunks] = modVols;
	chunkSizes[nchunks++] = modVolBytes;
	padding = align_pad(d->offsets.modVolOffset + modVolBytes, 4);
	d->offsets.indexOffset = d->offsets.modVolOffset + modVolBytes + padding;
	chunks[nchunks] = NULL;
	chunkSizes[nchunks++] = padding;
	/* Index */
	chunks[nchunks] = TA_LIST_HEAD(pvrrc.idx, u32);
	chunkSizes[nchunks++] = indexBytes;
	/* Fragment uniform buffer (the vertex one is its own: vk_base_drawer_write_vertex_uniforms()) */
	padding = align_pad(d->offsets.indexOffset + indexBytes, uniformAlign);
	d->offsets.fragmentUniformOffset = d->offsets.indexOffset + indexBytes + padding;
	chunks[nchunks] = NULL;
	chunkSizes[nchunks++] = padding;
	chunks[nchunks] = fragmentUniforms;
	chunkSizes[nchunks++] = sizeof(*fragmentUniforms);
	/* Translucent poly params */
	padding = align_pad(d->offsets.fragmentUniformOffset + sizeof(*fragmentUniforms), storageAlign);
	d->offsets.polyParamsOffset = d->offsets.fragmentUniformOffset + sizeof(*fragmentUniforms) + padding;
	chunks[nchunks] = NULL;
	chunkSizes[nchunks++] = padding;
	d->offsets.polyParamsSize = (trPolys != 0 ? trPolys * 2 : 1) * 4;
	chunks[nchunks] = NULL;
	chunkSizes[nchunks++] = (u32)d->offsets.polyParamsSize;
	totalSize = (u32)(d->offsets.polyParamsOffset + d->offsets.polyParamsSize);

	buffer = main_buffer(d, totalSize);
	if (buffer == NULL)
		return false;
	vk_buffer_upload_many(buffer, nchunks, chunkSizes, chunks, 0);
	vk_base_drawer_set_geometry(d->b, buffer->buffer);
	if (here)
		d->offsets.indexOffset = d->b->parsed.indices;
	trPolyParams = (u32 *)((u8 *)vk_buffer_map(buffer) + d->offsets.polyParamsOffset);
	if (trPolys == 0)
		trPolyParams[0] = 0;
	else
	{
		const PolyParam *pp = TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam);
		u32 i;
		for (i = 0; i < trPolys; i++, pp++)
		{
			trPolyParams[i * 2] = (pp->tsp & 0xffff00c0) | ((pp->isp >> 16) & 0xe400) | ((pp->pcw >> 7) & 1);
			trPolyParams[i * 2 + 1] = pp->tsp1;
		}
	}
	return true;
}

static void quad_draw(vk_oit_drawer *d, VkCommandBuffer cmd, bool bind)
{
	if (bind)
	{
		const VkDeviceSize zero = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &d->quad.buffer, &zero);
	}
	vkCmdDraw(cmd, 4, 1, 0, 0);
}

static VkRenderPass render_pass_of(vk_oit_drawer *d, bool initial, bool last, bool depthWrites, bool resumed)
{
	return vk_oit_passes_get(&d->pipelines->passes, initial, last, depthWrites, resumed);
}

bool vk_oit_drawer_draw(vk_oit_drawer *d, VkCommandBuffer cmd, VkImageView fogView, VkImageView paletteView)
{
	static const QuadVertex whole[4] = {
		{ { -1.f, -1.f, 0.f }, { 0.f, 0.f } },
		{ {  1.f, -1.f, 0.f }, { 1.f, 0.f } },
		{ { -1.f,  1.f, 0.f }, { 0.f, 1.f } },
		{ {  1.f,  1.f, 0.f }, { 1.f, 1.f } },
	};
	vk_base_drawer *b = d->b;
	/* (the attachments drawn into: this drawer's, or the screen's drawer's) */
	vk_oit_drawer *const att = d->attachmentsOf;
	const RenderPass *passes = TA_LIST_HEAD(pvrrc.render_passes, RenderPass);
	const int passCount = TA_LIST_USED(pvrrc.render_passes, RenderPass);
	const PolyParam *op = TA_LIST_HEAD(pvrrc.global_param_op, PolyParam);
	const PolyParam *pt = TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam);
	const PolyParam *tr = TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam);
	const ModifierVolumeParam *mvo = TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam);
	const ModifierVolumeParam *mvoTr = TA_LIST_HEAD(pvrrc.global_param_mvo_tr, ModifierVolumeParam);
	const bool ssaa = d->pipelines->passes.samples > 1;
	const vk_ta_gpu_draw *gpu = b->gpuDraw;
	FragmentShaderUniforms fragUniforms;
	vk_oit_push_constants pushConstants;
	VkClearValue clear[3];
	vk_attachment *depth;
	bool depthWrites = false;
	u32 height, bandHeight, bandCount, bandIndex, most;
	int i, tr_count;

	if (att->needDepthTransition)
	{
		att->needDepthTransition = false;
		/* Not convinced that this is really needed but it makes validation layers happy */
		if (att->upscaled)
		{
			u32 k;
			for (k = 0; k < att->depthAttachmentCount; k++)
				vk_set_image_layout(cmd, att->depthAttachments[k].image, vkc.depthFormat, 1, VK_IMAGE_LAYOUT_UNDEFINED,
						VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
		}
		else
			vk_set_image_layout(cmd, att->depthAttachment.image, vkc.depthFormat, 1, VK_IMAGE_LAYOUT_UNDEFINED,
					VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
	}

	vk_base_drawer_fragment_uniforms(&fragUniforms);
	fragUniforms.shade_scale_factor = FPU_SHAD_SCALE.scale_factor / 256.f;

	memset(&b->currentScissor, 0, sizeof(b->currentScissor));
	vk_oit_buffers_on_new_frame(d->buffers, cmd);

	/* (the GPU's decode has done it to a frame it made) */
	if (!pvrrc.gpu)
		vk_drawer_provoking_vertices();

	/* Upload vertex and index buffers */
	if (!upload_main_buffer(d, &fragUniforms))
		return false;
	vk_buffer_upload(&d->quad, sizeof(whole), whole, 0);

	/* Translucent polygons that are not sorted are drawn in the colour
	 * subpass and write depth there. A frame that has any is drawn with the
	 * render passes that allow that: see vk_oit_passes_get(). */
	tr_count = 0;
	for (i = 0; i < passCount; i++)
	{
		if (!passes[i].autosort && (int)passes[i].tr_count > tr_count)
			depthWrites = true;
		tr_count = (int)passes[i].tr_count;
	}

	/* Update per-frame descriptor set and bind it */
	depth = vk_oit_drawer_depth(att);
	vk_oit_desc_sets_update_uniforms(vk_oit_drawer_desc_set(d), b->vertexUniformBuffer, d->mainBuffers[d->imageIndex].buffer,
			(u32)d->offsets.fragmentUniformOffset, fogView, (u32)d->offsets.polyParamsOffset, (u32)d->offsets.polyParamsSize,
			depth->stencilView, depth->view, paletteView, vk_oit_depth_input_layout(depthWrites),
			pvrrc.gpu ? gpu->trParams : VK_NULL_HANDLE, pvrrc.gpu ? gpu->trParamsSize : 0);
	vk_oit_desc_sets_bind_per_frame(vk_oit_drawer_desc_set(d), cmd);
	/* SSAA: the samples' colour, one image for every render pass, read in
	 * the general layout; without, the two that take turns */
	if (ssaa)
		vk_oit_desc_sets_update_color_input(vk_oit_drawer_desc_set(d), 0, att->colorAttachments[0].view, VK_IMAGE_LAYOUT_GENERAL);
	else
	{
		vk_oit_desc_sets_update_color_input(vk_oit_drawer_desc_set(d), 0, att->colorAttachments[0].view,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vk_oit_desc_sets_update_color_input(vk_oit_drawer_desc_set(d), 1, att->colorAttachments[1].view,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipelines->pipelineLayout, 3, 1, &d->buffers->descSet, 0, NULL);

	/* Reset per-poly descriptor set pool */
	vk_oit_desc_sets_reset(vk_oit_drawer_desc_set(d));

	/* Bind vertex and index buffers */
	vk_base_drawer_bind_geometry(b, cmd, d->offsets.indexOffset);

	/* Make sure to push constants even if not used */
	memset(&pushConstants, 0, sizeof(pushConstants));
	vkCmdPushConstants(cmd, d->pipelines->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushConstants), &pushConstants);

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour, which is what the OpenGL renderers clear it to (never a
	 * render to a texture: that is not such a framebuffer); any other
	 * starts black. */
	memset(clear, 0, sizeof(clear));
	if (pvrrc.clearFramebuffer)
	{
		clear[0].color.float32[0] = VO_BORDER_COL.Red / 255.f;
		clear[0].color.float32[1] = VO_BORDER_COL.Green / 255.f;
		clear[0].color.float32[2] = VO_BORDER_COL.Blue / 255.f;
	}
	clear[0].color.float32[3] = 1.f;
	clear[1] = clear[0];
	clear[2].depthStencil.depth = 0.f;
	clear[2].depthStencil.stencil = 0;

	/* In bands, when the frames before had more translucent fragments than
	 * there is room for: the bands are drawn one after the other, each with
	 * all of the frame's render passes. The picture is cleared in the first
	 * band only (vk_oit_passes_get(), @resumed), and the last band's final
	 * render pass is the whole picture's, as the frame's final render pass
	 * always is: what is drawn after it (the screen's VMUs, the texture's
	 * copy) is drawn over the whole of it. */
	d->bands = vk_oit_buffers_choose_bands(d->buffers, b->pool->index, pvrrc.isRTT);
	height = d->viewport.extent.height > 1 ? d->viewport.extent.height : 1;
	most = d->bands < height ? d->bands : height;
	bandHeight = (height + most - 1) / most;
	bandCount = (height + bandHeight - 1) / bandHeight;
	vk_oit_buffers_set_bands(d->buffers, bandCount);
	for (bandIndex = 0; bandIndex < bandCount; bandIndex++)
	{
		const bool lastBand = bandIndex == bandCount - 1;
		RenderPass previous_pass;
		int render_pass;
		if (bandCount > 1)
		{
			const u32 top = bandIndex * bandHeight;
			d->band.offset.x = d->viewport.offset.x;
			d->band.offset.y = d->viewport.offset.y + (s32)top;
			d->band.extent.width = d->viewport.extent.width;
			d->band.extent.height = bandHeight < height - top ? bandHeight : height - top;
			memset(&b->currentScissor, 0, sizeof(b->currentScissor));
			if (bandIndex > 0)
				/* (the band before ended with the quad's) */
				vk_base_drawer_bind_geometry(b, cmd, d->offsets.indexOffset);
		}
		memset(&previous_pass, 0, sizeof(previous_pass));
		for (render_pass = 0; render_pass < passCount; render_pass++)
		{
			const RenderPass *current_pass = &passes[render_pass];
			const bool initialPass = render_pass == 0;
			const bool finalPass = render_pass == passCount - 1;
			VkRenderPassBeginInfo begin;
			VkMemoryBarrier memoryBarrier;
			VkFramebuffer targetFramebuffer;

			DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d TrMV %d autosort %d\n", render_pass + 1,
					current_pass->op_count - previous_pass.op_count,
					current_pass->pt_count - previous_pass.pt_count,
					current_pass->tr_count - previous_pass.tr_count,
					current_pass->mvo_count - previous_pass.mvo_count,
					current_pass->mvo_tr_count - previous_pass.mvo_tr_count, current_pass->autosort);

			/* Reset the pixel counter */
			vk_oit_buffers_reset_pixel_counter(d->buffers, cmd);

			/* (SSAA: the samples carry on to the next render pass; the picture is written by the last) */
			if (ssaa || finalPass)
				targetFramebuffer = d->kind->final_framebuffer(d->self);
			else
				targetFramebuffer = att->tempFramebuffers[att->upscaled ? d->imageIndex : 0][(passCount - 1 - render_pass) % 2];
			memset(&begin, 0, sizeof(begin));
			begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			begin.renderPass = render_pass_of(d, initialPass, finalPass, depthWrites, bandIndex > 0);
			if (begin.renderPass == VK_NULL_HANDLE)
				return false;
			begin.framebuffer = targetFramebuffer;
			begin.renderArea = bandCount > 1 && !(finalPass && lastBand) ? d->band : d->viewport;
			begin.clearValueCount = 3;
			begin.pClearValues = clear;
			vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

			/* Depth + stencil subpass */
			draw_list(d, cmd, ListType_Opaque, false, VK_OIT_PASS_DEPTH, op, previous_pass.op_count, current_pass->op_count);
			draw_list(d, cmd, ListType_Punch_Through, false, VK_OIT_PASS_DEPTH, pt, previous_pass.pt_count, current_pass->pt_count);
			draw_modvols(d, cmd, false, (int)previous_pass.mvo_count, (int)(current_pass->mvo_count - previous_pass.mvo_count), mvo);

			/* Color subpass */
			vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);

			/* A render that covers only part of the screen leaves the rest as
			 * it was: the last picture goes in first, under everything, and
			 * what this frame binds is bound again after it. */
			if (render_pass == 0 && d->kind->draw_last_picture != NULL && d->kind->draw_last_picture(d->self, cmd))
			{
				vk_oit_desc_sets_bind_per_frame(vk_oit_drawer_desc_set(d), cmd);
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipelines->pipelineLayout, 3, 1, &d->buffers->descSet,
						0, NULL);
				vk_base_drawer_bind_geometry(b, cmd, d->offsets.indexOffset);
				vkCmdPushConstants(cmd, d->pipelines->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushConstants),
						&pushConstants);
				vk_oit_drawer_set_scissor(d, cmd, &b->baseScissor);
			}

			/* OP + PT */
			draw_list(d, cmd, ListType_Opaque, false, VK_OIT_PASS_COLOR, op, previous_pass.op_count, current_pass->op_count);
			draw_list(d, cmd, ListType_Punch_Through, false, VK_OIT_PASS_COLOR, pt, previous_pass.pt_count, current_pass->pt_count);

			/* TR (in the first frame after the per-pixel lists are made, too:
			 * they start empty, and that frame's translucent polygons are drawn
			 * like any other's. Upstream: the background of the progress
			 * window in Fighting Force 2.) */
			if (current_pass->autosort)
				draw_list(d, cmd, ListType_Translucent, true, VK_OIT_PASS_OIT, tr, previous_pass.tr_count, current_pass->tr_count);
			else
				draw_list(d, cmd, ListType_Translucent, false, VK_OIT_PASS_COLOR, tr, previous_pass.tr_count, current_pass->tr_count);

			/* Final subpass (SSAA: the translucent modifier volumes' one) */
			vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
			vk_oit_desc_sets_bind_color_input(vk_oit_drawer_desc_set(d), cmd, ssaa ? 0 : (passCount - 1 - render_pass) % 2);
			vk_oit_drawer_set_scissor(d, cmd, &b->baseScissor);

			/* Tr modifier volumes */
			if (vkc.vendorID != VENDOR_QUALCOMM)	/* Adreno bug */
			{
				if (current_pass->mv_op_tr_shared)
					draw_modvols(d, cmd, true, (int)previous_pass.mvo_count, (int)(current_pass->mvo_count - previous_pass.mvo_count), mvo);
				else
					draw_modvols(d, cmd, true, (int)previous_pass.mvo_tr_count,
							(int)(current_pass->mvo_tr_count - previous_pass.mvo_tr_count), mvoTr);
			}

			/* The whole picture is put together here, not the part the game
			 * clips its drawing to: what is outside that has to be written
			 * too, or it is left as it was in whichever of the picture's
			 * images this frame goes to - two or three frames old, and a
			 * different one every frame. (Upstream: the loading screen
			 * before a fight in Soul Calibur blinking.) */
			vk_oit_drawer_set_scissor(d, cmd, &d->viewport);
			if (!ssaa)
			{
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_oit_pipeline_mgr_final(d->pipelines, false, false));
				quad_draw(d, cmd, true);
			}
			vk_oit_drawer_set_scissor(d, cmd, &d->viewport);
			if (ssaa)
			{
				if (!finalPass)
				{
					/* each sample put together, into the samples the next render pass draws over... */
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_oit_pipeline_mgr_final(d->pipelines, true, false));
					quad_draw(d, cmd, true);
					/* ...which sees the translucent depth too */
					vk_base_drawer_bind_geometry(b, cmd, d->offsets.indexOffset);
					draw_list(d, cmd, ListType_Translucent, current_pass->autosort, VK_OIT_PASS_DEPTH, tr, previous_pass.tr_count,
							current_pass->tr_count);
				}
				/* The picture put together and resolved, by the last render pass, all of it */
				vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
				vk_oit_drawer_set_scissor(d, cmd, &d->viewport);
				{
					const VkDeviceSize zero = 0;
					vkCmdBindVertexBuffers(cmd, 0, 1, &d->quad.buffer, &zero);
				}
				if (finalPass)
				{
					VkViewport viewport;
					viewport.x = viewport.y = 0.f;
					viewport.width = (float)d->viewport.extent.width;
					viewport.height = (float)d->viewport.extent.height;
					viewport.minDepth = 0.f;
					viewport.maxDepth = 1.f;
					vkCmdSetViewport(cmd, 0, 1, &viewport);
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_oit_pipeline_mgr_final(d->pipelines, false, true));
					quad_draw(d, cmd, false);
				}
			}

			/* Clear */
			memset(&memoryBarrier, 0, sizeof(memoryBarrier));
			memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
					VK_DEPENDENCY_BY_REGION_BIT, 1, &memoryBarrier, 0, NULL, 0, NULL);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_oit_pipeline_mgr_clear(d->pipelines));
			quad_draw(d, cmd, false);

			if (!finalPass && ssaa)
			{
				/* (the next render pass draws from the frame's geometry, not the quad) */
				vk_base_drawer_bind_geometry(b, cmd, d->offsets.indexOffset);
				vkCmdEndRenderPass(cmd);
				vk_oit_buffers_count_pass(d->buffers, cmd);
			}
			else if (!finalPass)
			{
				/* Re-bind vertex and index buffers */
				vk_base_drawer_bind_geometry(b, cmd, d->offsets.indexOffset);
				/* Tr depth-only pass */
				draw_list(d, cmd, ListType_Translucent, current_pass->autosort, VK_OIT_PASS_DEPTH, tr, previous_pass.tr_count,
						current_pass->tr_count);
				vkCmdEndRenderPass(cmd);
				vk_oit_buffers_count_pass(d->buffers, cmd);
			}
			else if (!lastBand)
			{
				/* (the last band's is ended by the drawer's end of the frame) */
				vkCmdEndRenderPass(cmd);
				vk_oit_buffers_count_pass(d->buffers, cmd);
			}
			previous_pass = *current_pass;
		}
	}
	memset(&d->band, 0, sizeof(d->band));
	return true;
}
