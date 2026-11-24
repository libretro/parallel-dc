#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
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
#include <stdlib.h>
#include <string.h>
#include "vk_drawer.h"
#include "pso.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta_words.h"
#include "hw/pvr/pvr_regs.h"
#include "rend/texconv.h"
#include "rend/shadows.h"
#include "rend/modvol_clip.h"

#define PPW(w, f) TAW_GET(w, f)

#define MAIN_BUFFER_USAGE (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
#define MAIN_BUFFER_PROPS (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

static u32 align_pad(VkDeviceSize offset, u32 alignment)
{
	return (u32)(alignment - (offset & (alignment - 1)));
}

void vk_tri_drawer_construct(vk_tri_drawer *d, vk_base_drawer *b)
{
	grow_buf_t init = GROW_BUF_INIT;
	memset(d, 0, sizeof(*d));
	d->b = b;
	d->sortedPasses = init;
	d->prepassPasses = init;
}

static void free_main_buffers(vk_tri_drawer *d)
{
	u32 i;
	for (i = 0; i < d->mainBufferCount; i++)
		vk_buffer_free(&d->mainBuffers[i]);
	free(d->mainBuffers);
	d->mainBuffers = NULL;
	d->mainBufferCount = 0;
}

void vk_tri_drawer_free(vk_tri_drawer *d)
{
	u32 i;
	SortPassesFree(&d->sortedPasses);
	SortPassesFree(&d->prepassPasses);
	for (i = 0; i < d->descriptorSetCount; i++)
		vk_desc_sets_term(&d->descriptorSets[i]);
	free(d->descriptorSets);
	d->descriptorSets = NULL;
	d->descriptorSetCount = 0;
	free_main_buffers(d);
}

/* This image's main buffer, at least @size bytes */
static vk_buffer *main_buffer(vk_tri_drawer *d, u32 size)
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

bool vk_tri_drawer_init(vk_tri_drawer *d, vk_samplers *samplers, vk_pipeline_mgr *pipelineManager)
{
	const u32 size = (u32)vkc.swapChainSize;
	d->pipelineManager = pipelineManager;
	/* (the passes' sorted draws: room before the first frame) */
	SortPassesReserve(&d->sortedPasses);
	SortPassesReserve(&d->prepassPasses);

	while (d->descriptorSetCount > size)
		vk_desc_sets_term(&d->descriptorSets[--d->descriptorSetCount]);
	if (d->descriptorSetCount < size)
	{
		vk_desc_sets *more = (vk_desc_sets *)realloc(d->descriptorSets, size * sizeof(vk_desc_sets));
		if (more == NULL)
			return false;
		d->descriptorSets = more;
		for (; d->descriptorSetCount < size; d->descriptorSetCount++)
		{
			vk_desc_sets *s = &more[d->descriptorSetCount];
			vk_desc_sets_construct(s);
			vk_desc_sets_init(s, samplers, pipelineManager->pipelineLayout, pipelineManager->perFrameLayout,
					pipelineManager->perPolyLayout);
		}
	}
	/* (a frame's buffers made now, not when it first draws: a render
	 * to a texture may come long after the game started) */
	return vk_base_drawer_make_vertex_uniforms(d->b, size) && main_buffer(d, 0) != NULL;
}

static float palette_index_of(const PolyParam *poly)
{
	if (PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_4BPP)
		return ((float)(PPW(poly->tcw, TCW_PalSelect) << 4) + 0.5f) / 1024.f;
	return ((float)((PPW(poly->tcw, TCW_PalSelect) >> 4) << 8) + 0.5f) / 1024.f;
}

static void push_floats(vk_tri_drawer *d, VkCommandBuffer cmd, const float *f)
{
	vkCmdPushConstants(cmd, d->pipelineManager->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 6 * sizeof(float), f);
}

static void scissor_of(vk_tri_drawer *d, VkCommandBuffer cmd, u32 tileclip)
{
	VkRect2D scissorRect;
	if (vk_base_drawer_tile_clip(d->b, tileclip, &scissorRect) == TILECLIP_OUTSIDE)
		vk_base_drawer_set_scissor(d->b, cmd, &scissorRect);
	else
		vk_base_drawer_set_scissor(d->b, cmd, &d->b->baseScissor);
}

static void draw_poly(vk_tri_drawer *d, VkCommandBuffer cmd, u32 listType, bool sortTriangles, const PolyParam *poly, u32 first, u32 count,
		int shadowPass, const VkRect2D *within)
{
	vk_base_drawer *b = d->b;
	VkRect2D scissorRect;
	const int tileClip = vk_base_drawer_tile_clip(b, poly->tileclip, &scissorRect);
	bool trilinear, palette;
	float trilinearAlpha = 1.f;
	float palette_index = 0.f;
	PsoChoice pso;
	VkDescriptorSet texSet = VK_NULL_HANDLE;
	if (within != NULL)
	{
		/* only the part of it inside this rectangle, and nothing if there is none */
		const VkRect2D *clip = tileClip == TILECLIP_OUTSIDE ? &scissorRect : &b->baseScissor;
		const int x0 = within->offset.x > clip->offset.x ? within->offset.x : clip->offset.x;
		const int y0 = within->offset.y > clip->offset.y ? within->offset.y : clip->offset.y;
		const int wx1 = within->offset.x + (int)within->extent.width, cx1 = clip->offset.x + (int)clip->extent.width;
		const int wy1 = within->offset.y + (int)within->extent.height, cy1 = clip->offset.y + (int)clip->extent.height;
		const int x1 = wx1 < cx1 ? wx1 : cx1;
		const int y1 = wy1 < cy1 ? wy1 : cy1;
		VkRect2D r;
		if (x1 <= x0 || y1 <= y0)
			return;
		r.offset.x = x0;
		r.offset.y = y0;
		r.extent.width = (u32)(x1 - x0);
		r.extent.height = (u32)(y1 - y0);
		vk_base_drawer_set_scissor(b, cmd, &r);
	}
	else if (tileClip == TILECLIP_OUTSIDE)
		vk_base_drawer_set_scissor(b, cmd, &scissorRect);
	else
		vk_base_drawer_set_scissor(b, cmd, &b->baseScissor);

	/* (what the shader multiplies by when it is trilinear: pushed for every such polygon, 1 included) */
	trilinear = PPW(poly->tsp, TSP_FilterMode) > 1 && PPW(poly->pcw, PCW_Texture) && listType != ListType_Punch_Through
			&& PPW(poly->tcw, TCW_MipMapped) == 1;
	if (trilinear)
	{
		trilinearAlpha = 0.25f * (float)(PPW(poly->tsp, TSP_MipMapD) & 0x3);
		if (PPW(poly->tsp, TSP_FilterMode) == 2)
			/* Trilinear pass A */
			trilinearAlpha = 1.0f - trilinearAlpha;
	}
	palette = tex_gpu_paletted(poly->tsp, poly->tcw, 0);
	if (palette)
		palette_index = palette_index_of(poly);

	pso = vk_pipeline_mgr_get(d->pipelineManager, listType, sortTriangles, poly, shadowPass);
	if (pso.pipeline == VK_NULL_HANDLE)
		return;
	/* (the general pipeline is told the polygon's key too: pso.h) */
	if (tileClip == TILECLIP_INSIDE || trilinear || palette || pso.general)
	{
		struct
		{
			float f[6];
			s32 key;
		} pushConstants;
		pushConstants.f[0] = (float)scissorRect.offset.x;
		pushConstants.f[1] = (float)scissorRect.offset.y;
		pushConstants.f[2] = (float)scissorRect.offset.x + (float)scissorRect.extent.width;
		pushConstants.f[3] = (float)scissorRect.offset.y + (float)scissorRect.extent.height;
		pushConstants.f[4] = trilinearAlpha;
		pushConstants.f[5] = palette_index;
		pushConstants.key = (s32)(pso.key & ((1u << TRI_SC_BITS) - 1));
		vkCmdPushConstants(cmd, d->pipelineManager->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
				pso.general ? TRI_PUSH_KEY_OFFSET + 4 : TRI_PUSH_KEY_OFFSET, &pushConstants);
		memcpy(d->pushed, pushConstants.f, sizeof(d->pushed));
	}

	if (PPW(poly->pcw, PCW_Texture))
		texSet = vk_desc_sets_set_texture(vk_tri_drawer_desc_set(d), poly->texid, poly->tsp);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso.pipeline);
	if (pso.general)
		vk_pipeline_mgr_set_dynamic_state(d->pipelineManager, cmd, pso.key);
	if (PPW(poly->pcw, PCW_Texture))
		vk_desc_sets_bind_per_poly(vk_tri_drawer_desc_set(d), cmd, texSet);

	vk_base_drawer_draw_indices(b, cmd, listType, first, count);
}

/* The polygons of a list that take shadows, drawn again where they are in a
 * modifier volume: see rend/shadows.h. */
static void draw_shadowed(vk_tri_drawer *d, VkCommandBuffer cmd, u32 listType, const PolyParam *polys, u32 first, u32 last,
		const ScreenBounds *area, const VkRect2D *scissor)
{
	const PolyParam *pp_end = polys + last;
	const PolyParam *pp;
	/* (of a frame decoded on the GPU, the GPU has said which groups reach
	 * into the volumes: the others' second commands draw nothing) */
	d->b->gpuDraws = VK_GPU_DRAWS_SHADOWS;
	for (pp = polys + first; pp != pp_end; pp++)
		if (CanDrawShadowed(listType, pp) && (pvrrc.gpu || poly_overlaps(pp, area)))
			draw_poly(d, cmd, listType, false, pp, pp->first, pp->count,
					shadow_later_wins(listType, pp) ? VK_SHADOW_LATER_WINS : VK_SHADOW_EARLIER_WINS, scissor);
	d->b->gpuDraws = VK_GPU_DRAWS_GROUPS;
}

static void draw_sorted(vk_tri_drawer *d, VkCommandBuffer cmd, const SortTrigDrawParam *polys, u32 count, bool multipass)
{
	vk_base_drawer *b = d->b;
	/* (a frame decoded on the GPU: its sorted indices are this frame's in
	 * the decoder's room for them, and name the GPU's vertices) */
	const u32 base = pvrrc.gpu ? 0 : (u32)TA_LIST_USED(pvrrc.idx, u32);
	u32 i;
	if (pvrrc.gpu)
	{
		if (count == 0)
			return;
		vkCmdBindIndexBuffer(cmd, b->gpuDraw->sorted, b->gpuDraw->sortedOffset, VK_INDEX_TYPE_UINT32);
		b->gpuDraws = VK_GPU_DRAWS_SORTED;
	}
	for (i = 0; i < count; i++)
		draw_poly(d, cmd, ListType_Translucent, true, polys[i].ppid, base + polys[i].first, polys[i].count, VK_SHADOW_NONE, NULL);

	if (multipass && settings.rend.TranslucentPolygonDepthMask)
	{
		/* Sorted translucent polygons are drawn without writing depth. Where
		 * a later render pass needs their depth (Cosmic Smash), it is written
		 * now, for those that do not have Z write disabled. */
		vk_pso_cull cull;
		vk_pso_cull_init(&cull, cmd, d->pipelineManager->cullSetter);
		for (i = 0; i < count; i++)
		{
			const SortTrigDrawParam *param = &polys[i];
			const u32 cullMode = PPW(param->ppid->isp, ISP_TSP_CullMode);
			if (param->count <= 2 || PPW(param->ppid->isp, ISP_TSP_ZWriteDis))
				continue;
			scissor_of(d, cmd, param->ppid->tileclip);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_mgr_modvol(d->pipelineManager, VK_MODVOL_DEPTH, cullMode));
			vk_pso_cull_set(&cull, PsoCullIndex(cullMode));
			vkCmdDrawIndexed(cmd, param->count, 1, base + param->first, 0, 0);
		}
	}
	if (pvrrc.gpu)
	{
		vkCmdBindIndexBuffer(cmd, b->gpuDraw->indices, 0, VK_INDEX_TYPE_UINT32);
		b->gpuDraws = VK_GPU_DRAWS_GROUPS;
	}
}

/* The depth prepass of a sorted list: rend/sorter.h */
static void draw_prepass(vk_tri_drawer *d, VkCommandBuffer cmd, const SortTrigDrawParam *polys, u32 count)
{
	vk_base_drawer *b = d->b;
	/* (as draw_sorted(): of a frame decoded on the GPU, the indices are in the decoder's room) */
	const u32 base = pvrrc.gpu ? 0 : (u32)TA_LIST_USED(pvrrc.idx, u32);
	vk_pso_cull cull;
	u32 i;
	if (count == 0)
		return;
	if (pvrrc.gpu)
	{
		vkCmdBindIndexBuffer(cmd, b->gpuDraw->sorted, b->gpuDraw->sortedOffset, VK_INDEX_TYPE_UINT32);
		b->gpuDraws = VK_GPU_DRAWS_SORTED;
	}
	vk_pso_cull_init(&cull, cmd, d->pipelineManager->cullSetter);
	for (i = 0; i < count; i++)
	{
		const PolyParam *poly = polys[i].ppid;
		VkRect2D scissorRect;
		const int tileClip = vk_base_drawer_tile_clip(b, poly->tileclip, &scissorRect);
		/* what the colour pass's shader has as macros (see FragmentShaderParams) */
		const bool palette = tex_gpu_paletted(poly->tsp, poly->tcw, 0);
		const u32 pixelFmt = PPW(poly->tcw, TCW_PixelFmt);
		const u32 flags = (tileClip == TILECLIP_INSIDE ? PREPASS_CLIP_INSIDE : 0)
				| (PPW(poly->tsp, TSP_UseAlpha) ? PREPASS_USE_ALPHA : 0)
				| (palette ? PREPASS_PALETTE : 0)
				| (pixelFmt == TA_PIXEL_BUMPMAP ? PREPASS_BUMP_MAP : 0)
				| (PPW(poly->tsp, TSP_IgnoreTexA) || pixelFmt == TA_PIXEL_565 ? PREPASS_IGNORE_TEX_A : 0)
				| (PPW(poly->tsp, TSP_ShadInstr) * PREPASS_SHAD_INSTR)
				| (PPW(poly->tsp, TSP_ColorClamp) ? PREPASS_CLAMP : 0)
				| (PPW(poly->tsp, TSP_FogCtrl) == 3 ? PREPASS_FOG_ALPHA : 0)
				| (PrepassAlwaysHides(poly) ? PREPASS_ALWAYS : 0);
		float pushConstants[6];
		VkDescriptorSet texSet = VK_NULL_HANDLE;
		vk_base_drawer_set_scissor(b, cmd, tileClip == TILECLIP_OUTSIDE ? &scissorRect : &b->baseScissor);
		pushConstants[0] = (float)scissorRect.offset.x;
		pushConstants[1] = (float)scissorRect.offset.y;
		pushConstants[2] = (float)scissorRect.offset.x + (float)scissorRect.extent.width;
		pushConstants[3] = (float)scissorRect.offset.y + (float)scissorRect.extent.height;
		pushConstants[4] = (float)flags;
		pushConstants[5] = palette ? palette_index_of(poly) : 0.f;
		push_floats(d, cmd, pushConstants);

		if (PPW(poly->pcw, PCW_Texture))
			texSet = vk_desc_sets_set_texture(vk_tri_drawer_desc_set(d), poly->texid, poly->tsp);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_mgr_prepass(d->pipelineManager, poly));
		vk_pso_cull_set(&cull, PsoCullIndex(PPW(poly->isp, ISP_TSP_CullMode)));
		if (PPW(poly->pcw, PCW_Texture))
			vk_desc_sets_bind_per_poly(vk_tri_drawer_desc_set(d), cmd, texSet);
		vk_base_drawer_draw_indices(b, cmd, ListType_Translucent, base + polys[i].first, polys[i].count);
	}
	push_floats(d, cmd, d->pushed);
	if (pvrrc.gpu)
	{
		vkCmdBindIndexBuffer(cmd, b->gpuDraw->indices, 0, VK_INDEX_TYPE_UINT32);
		b->gpuDraws = VK_GPU_DRAWS_GROUPS;
	}
}

static void draw_list(vk_tri_drawer *d, VkCommandBuffer cmd, u32 listType, bool sortTriangles, const PolyParam *polys, u32 first, u32 last)
{
	const PolyParam *pp_end = polys + last;
	const PolyParam *pp;
	for (pp = polys + first; pp != pp_end; pp++)
		if (pp->count > 2)
			draw_poly(d, cmd, listType, sortTriangles, pp, pp->first, pp->count, VK_SHADOW_NONE, NULL);
}

static void draw_modvols(vk_tri_drawer *d, VkCommandBuffer cmd, int first, int count, const RenderPass *previous_pass,
		const RenderPass *current_pass, int render_pass)
{
	vk_base_drawer *b = d->b;
	const ModifierVolumeParam *mvo = TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam);
	const ModifierVolumeParam *params = &mvo[first];
	int mod_base = -1;
	int cmv;
	vk_pso_cull cull;
	ScreenBounds area;
	float pushConstants[6];

	if (count == 0 || vk_base_drawer_modvol_triangles() == 0)
		return;

	vk_base_drawer_bind_modvol_triangles(b, cmd, d->mainBuffers[d->imageIndex].buffer, d->offsets.modVolOffset);
	vk_base_drawer_set_scissor(b, cmd, &b->baseScissor);
	vk_pso_cull_init(&cull, cmd, d->pipelineManager->cullSetter);

	for (cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam *param = &params[cmv];
		const u32 mv_mode = PPW(param->isp, ISP_Modvol_DepthMode);
		const u32 cullMode = PPW(param->isp, ISP_Modvol_CullMode);
		u32 mvFirst, mvCount;
		mv_clip_range_get(&pvrrc, mvo, param, &mvFirst, &mvCount);

		if (mvCount == 0)
			continue;

		/* clipped like a polygon, to the rectangle it was sent under if it asked to be */
		scissor_of(d, cmd, param->tileclip);

		if (mod_base == -1)
			mod_base = (int)mvFirst;

		/* OR'ing (open volume or quad), or XOR'ing (closed volume) */
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_mgr_modvol(d->pipelineManager,
				!PPW(param->isp, ISP_Modvol_VolumeLast) && mv_mode > 0 ? VK_MODVOL_OR : VK_MODVOL_XOR, cullMode));
		vk_pso_cull_set(&cull, PsoCullIndex(cullMode));
		vkCmdDraw(cmd, mvCount * 3, 1, mvFirst * 3, 0);

		if (mv_mode == 1 || mv_mode == 2)
		{
			/* Sum the area */
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_mgr_modvol(d->pipelineManager,
					mv_mode == 1 ? VK_MODVOL_INCLUSION : VK_MODVOL_EXCLUSION, cullMode));
			vk_pso_cull_set(&cull, PsoCullIndex(cullMode));
			vkCmdDraw(cmd, (mvFirst + mvCount - (u32)mod_base) * 3, 1, (u32)mod_base * 3, 0);
			mod_base = -1;
		}
	}
	vk_base_drawer_set_scissor(b, cmd, &b->baseScissor);
	vk_base_drawer_bind_vertices(b, cmd);

	/* The polygons that take shadows, again, where they are in a volume.
	 * Of a frame decoded on the GPU, the volumes' bounds are the header
	 * pass's, from their blocks (TA_GPU_SHADOWS) */
	if (pvrrc.gpu)
	{
		const f32 *ga = pvrrc.gpu_shadow_area[render_pass < 10 ? render_pass : 0];
		area.min_x = ga[0];
		area.min_y = ga[1];
		area.max_x = ga[2];
		area.max_y = ga[3];
	}
	else
		area = shadow_modvol_bounds(first, count);
	if (!screen_bounds_empty(&area))
	{
		int rect[4];
		VkRect2D scissor;
		bounds_to_scissor(&area, &b->matrices.viewport, rect);
		scissor.offset.x = rect[0];
		scissor.offset.y = rect[1];
		scissor.extent.width = (u32)rect[2];
		scissor.extent.height = (u32)rect[3];
		draw_shadowed(d, cmd, ListType_Opaque, TA_LIST_HEAD(pvrrc.global_param_op, PolyParam), previous_pass->op_count,
				current_pass->op_count, &area, &scissor);
		draw_shadowed(d, cmd, ListType_Punch_Through, TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam), previous_pass->pt_count,
				current_pass->pt_count, &area, &scissor);
		vk_base_drawer_set_scissor(b, cmd, &b->baseScissor);
	}

	/* What is left: darkened as it is
	 * (all six: the polygon shaders' block is six floats, and what is not pushed is not anything in particular) */
	memset(pushConstants, 0, sizeof(pushConstants));
	pushConstants[0] = 1 - FPU_SHAD_SCALE.scale_factor / 256.f;
	push_floats(d, cmd, pushConstants);
	memcpy(d->pushed, pushConstants, sizeof(d->pushed));

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_mgr_modvol(d->pipelineManager, VK_MODVOL_FINAL, 0));
	vkCmdDrawIndexed(cmd, 4, 1, 0, 0, 0);
}

/* The main buffer of this render, laid out - the vertices, the volumes'
 * triangles, the indices, room for the sorted passes' indices after those,
 * and the fragment uniforms - with the sorted passes sorted into it */
static vk_buffer *sort_into_main_buffer(vk_tri_drawer *d)
{
	/* (a frame decoded on the GPU has its geometry there, and is sorted
	 * there: TA_GPU_SORT; one the parse wrote where the card reads it has
	 * its geometry there, and is sorted after its indices) */
	const u32 sortRoom = pvrrc.gpu ? 0 : SortPassesRoom() * (u32)sizeof(u32);
	const bool here = vk_base_drawer_use_parsed(d->b, sortRoom);
	const u32 vertexBytes = pvrrc.gpu || here ? 0 : TA_LIST_BYTES(pvrrc.verts);
	const u32 indexBytes = pvrrc.gpu || here ? 0 : TA_LIST_BYTES(pvrrc.idx);
	const u32 sortedBytes = here ? 0 : sortRoom;
	const u32 uniformAlign = vkc.uniformBufferAlignment > 4 ? (u32)vkc.uniformBufferAlignment : 4;
	u32 modVolBytes = 0;
	u32 indexSize;
	vk_buffer *buffer;
	if (vk_base_drawer_modvols_on_cpu() && !here)
		vk_base_drawer_modvol_list(&modVolBytes);

	d->offsets.modVolOffset = vertexBytes + align_pad(vertexBytes, 4);
	d->offsets.indexOffset = d->offsets.modVolOffset + modVolBytes + align_pad(d->offsets.modVolOffset + modVolBytes, 4);
	indexSize = indexBytes + sortedBytes;
	d->offsets.fragmentUniformOffset = d->offsets.indexOffset + indexSize
			+ align_pad(d->offsets.indexOffset + indexSize, uniformAlign);

	buffer = main_buffer(d, (u32)d->offsets.fragmentUniformOffset + (u32)sizeof(FragmentShaderUniforms));
	if (buffer == NULL)
		return NULL;
	vk_base_drawer_set_geometry(d->b, buffer->buffer);
	if (pvrrc.gpu)
		d->sortedIndexCount = 0;
	else if (here)
	{
		d->offsets.indexOffset = d->b->parsed.indices;
		d->sortedIndexCount = SortPassesTo(&d->sortedPasses, &d->prepassPasses, (u32 *)pvrrc.idx.l.cur, true, true);
	}
	else
		d->sortedIndexCount = SortPassesTo(&d->sortedPasses, &d->prepassPasses,
				(u32 *)((u8 *)vk_buffer_map(buffer) + d->offsets.indexOffset + indexBytes), true, true);
	return buffer;
}

/* ...and the rest of it */
static void upload_main_buffer(vk_tri_drawer *d, const vk_buffer *buffer, const FragmentShaderUniforms *fragmentUniforms)
{
	/* (not one the parse wrote where the card reads it: geometryParsed) */
	if (!pvrrc.gpu && !d->b->geometryParsed)
	{
		vk_buffer_upload(buffer, TA_LIST_BYTES(pvrrc.verts), TA_LIST_HEAD(pvrrc.verts, Vertex), 0);
		vk_buffer_upload(buffer, TA_LIST_BYTES(pvrrc.idx), TA_LIST_HEAD(pvrrc.idx, u32), (u32)d->offsets.indexOffset);
	}
	if (vk_base_drawer_modvols_on_cpu() && !d->b->geometryParsed)
	{
		u32 bytes;
		const ModTriangle *list = vk_base_drawer_modvol_list(&bytes);
		vk_buffer_upload(buffer, bytes, list, (u32)d->offsets.modVolOffset);
	}
	vk_buffer_upload(buffer, sizeof(*fragmentUniforms), fragmentUniforms, (u32)d->offsets.fragmentUniformOffset);
}

bool vk_tri_drawer_draw(vk_tri_drawer *d, VkCommandBuffer cmd, VkImageView fogView, VkImageView paletteView,
		u32 samples, vk_ssaa_resolve *resolve, u32 width, u32 height)
{
	vk_base_drawer *b = d->b;
	FragmentShaderUniforms fragUniforms;
	vk_buffer *mainBuffer;
	RenderPass previous_pass;
	const RenderPass *passes = TA_LIST_HEAD(pvrrc.render_passes, RenderPass);
	const int passCount = TA_LIST_USED(pvrrc.render_passes, RenderPass);
	const PolyParam *op = TA_LIST_HEAD(pvrrc.global_param_op, PolyParam);
	const PolyParam *pt = TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam);
	const PolyParam *tr = TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam);
	int render_pass;

	vk_base_drawer_fragment_uniforms(&fragUniforms);
	fragUniforms.shade_scale_factor = FPU_SHAD_SCALE.scale_factor / 256.f;

	/* Sorted where the card reads the indices (this frame's main buffer:
	 * the render pass is begun), from the vertices as the game sent them.
	 * (A frame decoded on the GPU has its sorted passes sorted already, and
	 * their indices went straight to the decoder's room for them when the
	 * decode was recorded: see TA_GPU_SORT.) */
	mainBuffer = sort_into_main_buffer(d);
	if (mainBuffer == NULL)
		return false;

	/* (the GPU's decode has done it to a frame it made) */
	if (!pvrrc.gpu)
		vk_drawer_provoking_vertices();

	upload_main_buffer(d, mainBuffer, &fragUniforms);

	/* the per-frame descriptor set, and the per-poly sets' pool reset */
	vk_desc_sets_update_uniforms(vk_tri_drawer_desc_set(d), b->vertexUniformBuffer, mainBuffer->buffer,
			(u32)d->offsets.fragmentUniformOffset, fogView, paletteView);
	vk_desc_sets_bind_per_frame(vk_tri_drawer_desc_set(d), cmd);
	vk_desc_sets_reset(vk_tri_drawer_desc_set(d));

	vk_base_drawer_bind_geometry(b, cmd, d->offsets.indexOffset);

	/* (pushed even if not used) */
	memset(d->pushed, 0, sizeof(d->pushed));
	push_floats(d, cmd, d->pushed);

	memset(&previous_pass, 0, sizeof(previous_pass));
	for (render_pass = 0; render_pass < passCount; render_pass++)
	{
		const RenderPass *current_pass = &passes[render_pass];
		const bool multipass = render_pass < passCount - 1;
		u32 n;

		DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d autosort %d\n", render_pass + 1,
				current_pass->op_count - previous_pass.op_count,
				current_pass->pt_count - previous_pass.pt_count,
				current_pass->tr_count - previous_pass.tr_count,
				current_pass->mvo_count - previous_pass.mvo_count, current_pass->autosort);
		draw_list(d, cmd, ListType_Opaque, false, op, previous_pass.op_count, current_pass->op_count);
		draw_list(d, cmd, ListType_Punch_Through, false, pt, previous_pass.pt_count, current_pass->pt_count);
		draw_modvols(d, cmd, (int)previous_pass.mvo_count, (int)(current_pass->mvo_count - previous_pass.mvo_count), &previous_pass,
				current_pass, render_pass);
		if (current_pass->autosort)
		{
			/* (a frame decoded on the GPU with a sorted pass was sorted by triangle: TA_GPU_SORT) */
			if (pvrrc.gpu)
			{
				const SortTrigDrawParam *draws = ta_gpu_prepass((u32)render_pass, &n);
				draw_prepass(d, cmd, draws, n);
				draws = ta_gpu_sorted((u32)render_pass, &n);
				draw_sorted(d, cmd, draws, n, multipass);
			}
			else if (!settings.pvr.Emulation.AlphaSortMode)
			{
				const SortTrigDrawParam *draws = SortPassDraws(&d->prepassPasses, (u32)render_pass, &n);
				draw_prepass(d, cmd, draws, n);
				draws = SortPassDraws(&d->sortedPasses, (u32)render_pass, &n);
				draw_sorted(d, cmd, draws, n, multipass);
			}
			else
			{
				const SortTrigDrawParam *draws = SortPassDraws(&d->prepassPasses, (u32)render_pass, &n);
				draw_prepass(d, cmd, draws, n);
				SortPParams((int)previous_pass.tr_count, (int)(current_pass->tr_count - previous_pass.tr_count));
				draw_list(d, cmd, ListType_Translucent, true, tr, previous_pass.tr_count, current_pass->tr_count);
			}
		}
		else
			draw_list(d, cmd, ListType_Translucent, false, tr, previous_pass.tr_count, current_pass->tr_count);
		previous_pass = *current_pass;
	}

	/* SSAA: the samples resolved into the picture, in the last subpass */
	if (samples > 1)
	{
		vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
		return vk_ssaa_resolve_draw(resolve, cmd, width, height);
	}
	return true;
}
