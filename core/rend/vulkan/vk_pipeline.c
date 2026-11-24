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
/* See vk_pipeline.h */
#include <stdlib.h>
#include <string.h>
#include <retro_miscellaneous.h>
#include "vk_pipeline.h"
#include "vk_texture.h"
#include "hw/pvr/ta_words.h"
#include "rend/texconv.h"
#include "rend/ssaa_rate.h"

#define PPW(w, f) TAW_GET(w, f)

/* one subpass dependency, at @d; the next place */
static VkSubpassDependency *dep(VkSubpassDependency *d, u32 src, u32 dst, VkPipelineStageFlags srcStages, VkPipelineStageFlags dstStages,
		VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkDependencyFlags flags)
{
	d->srcSubpass = src;
	d->dstSubpass = dst;
	d->srcStageMask = srcStages;
	d->dstStageMask = dstStages;
	d->srcAccessMask = srcAccess;
	d->dstAccessMask = dstAccess;
	d->dependencyFlags = flags;
	return d + 1;
}

/* ---- the descriptor sets ---- */

void vk_desc_sets_construct(vk_desc_sets *d)
{
	memset(d, 0, sizeof(*d));
	vk_desc_cache_construct(&d->perPoly);
}

void vk_desc_sets_init(vk_desc_sets *d, vk_samplers *samplers, VkPipelineLayout pipelineLayout,
		VkDescriptorSetLayout perFrameLayout, VkDescriptorSetLayout perPolyLayout)
{
	d->samplers = samplers;
	d->pipelineLayout = pipelineLayout;
	d->perFrameLayout = perFrameLayout;
	d->perPolyLayout = perPolyLayout;
	vk_desc_cache_set_layout(&d->perPoly, perPolyLayout);
	if (d->perFrameDescSet == VK_NULL_HANDLE)
		d->perFrameDescSet = vkc_alloc_desc_set(perFrameLayout);
	if (d->noTextureDescSet == VK_NULL_HANDLE)
		d->noTextureDescSet = vkc_alloc_desc_set(perPolyLayout);
}

static void write_set(VkWriteDescriptorSet *w, VkDescriptorSet set, u32 binding, VkDescriptorType type,
		const VkDescriptorImageInfo *image, const VkDescriptorBufferInfo *buffer)
{
	memset(w, 0, sizeof(*w));
	w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w->dstSet = set;
	w->dstBinding = binding;
	w->descriptorCount = 1;
	w->descriptorType = type;
	w->pImageInfo = image;
	w->pBufferInfo = buffer;
}

/* a single entry's sampler, read at the middle of its texels: see UpdateFogTexture() in gles/gles.c */
static VkSampler entry_sampler(vk_samplers *samplers)
{
	u32 tsp = 0;
	tsp = TAW_SET(tsp, TSP_FilterMode, 0);
	tsp = TAW_SET(tsp, TSP_ClampU, 1);
	tsp = TAW_SET(tsp, TSP_ClampV, 1);
	return vk_samplers_get(samplers, tsp);
}

void vk_desc_sets_update_uniforms(vk_desc_sets *d, VkBuffer vertexUniformBuffer, VkBuffer buffer, u32 fragmentUniformOffset,
		VkImageView fogImageView, VkImageView paletteImageView)
{
	VkDescriptorBufferInfo bufferInfos[2];
	VkDescriptorImageInfo fogInfo, paletteInfo;
	/* (on the stack: nothing allocated for them) */
	VkWriteDescriptorSet writes[5];
	u32 n = 0;

	if (d->perFrameDescSet == VK_NULL_HANDLE)
		d->perFrameDescSet = vkc_alloc_desc_set(d->perFrameLayout);
	bufferInfos[0].buffer = vertexUniformBuffer;
	bufferInfos[0].offset = 0;
	bufferInfos[0].range = sizeof(VertexShaderUniforms);
	bufferInfos[1].buffer = buffer;
	bufferInfos[1].offset = fragmentUniformOffset;
	bufferInfos[1].range = sizeof(FragmentShaderUniforms);
	write_set(&writes[n++], d->perFrameDescSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bufferInfos[0]);
	write_set(&writes[n++], d->perFrameDescSet, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bufferInfos[1]);
	if (fogImageView != VK_NULL_HANDLE)
	{
		fogInfo.sampler = entry_sampler(d->samplers);
		fogInfo.imageView = fogImageView;
		fogInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		write_set(&writes[n++], d->perFrameDescSet, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fogInfo, NULL);
		/* What an untextured polygon has as its texture: the general
		 * pipelines read one only for a textured polygon, but have it in
		 * their shader (pso.h) */
		if (d->noTextureDescSet == VK_NULL_HANDLE)
			d->noTextureDescSet = vkc_alloc_desc_set(d->perPolyLayout);
		if (d->noTextureView != fogImageView)
		{
			d->noTextureView = fogImageView;
			write_set(&writes[n++], d->noTextureDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fogInfo, NULL);
		}
	}
	if (paletteImageView != VK_NULL_HANDLE)
	{
		paletteInfo.sampler = entry_sampler(d->samplers);
		paletteInfo.imageView = paletteImageView;
		paletteInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		write_set(&writes[n++], d->perFrameDescSet, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &paletteInfo, NULL);
	}
	vkUpdateDescriptorSets(vkc.device, n, writes, 0, NULL);
}

VkDescriptorSet vk_desc_sets_set_texture(vk_desc_sets *d, u64 textureId, u32 tsp)
{
	const vk_texture *texture = (const vk_texture *)(uintptr_t)textureId;
	TexDescKey key;
	VkDescriptorImageInfo imageInfo;
	VkWriteDescriptorSet write;
	VkDescriptorSet set;

	key.view0 = vk_texture_read_only_serial(texture);
	key.view1 = 0;
	key.sampler0 = tsp & VK_SAMPLER_TSP_MASK;
	key.sampler1 = 0;
	set = vk_desc_cache_find(&d->perPoly, &key);
	if (set != VK_NULL_HANDLE)
		return set;
	set = vk_desc_cache_add(&d->perPoly, &key);
	imageInfo.sampler = vk_samplers_get(d->samplers, tsp);
	imageInfo.imageView = vk_texture_read_only_view(texture);
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	write_set(&write, set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo, NULL);
	vkUpdateDescriptorSets(vkc.device, 1, &write, 0, NULL);
	return set;
}

void vk_desc_sets_bind_per_frame(const vk_desc_sets *d, VkCommandBuffer cmd)
{
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipelineLayout, 0, 1, &d->perFrameDescSet, 0, NULL);
	/* (a texture's replaces it; until one does, an untextured polygon has this) */
	if (d->noTextureDescSet != VK_NULL_HANDLE)
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipelineLayout, 1, 1, &d->noTextureDescSet, 0, NULL);
}

void vk_desc_sets_bind_per_poly(const vk_desc_sets *d, VkCommandBuffer cmd, VkDescriptorSet set)
{
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipelineLayout, 1, 1, &set, 0, NULL);
}

void vk_desc_sets_reset(vk_desc_sets *d)
{
	vk_desc_cache_next_use(&d->perPoly);
}

void vk_desc_sets_term(vk_desc_sets *d)
{
	vk_desc_cache_term(&d->perPoly);
	vkc_free_desc_set(d->noTextureDescSet);
	vkc_free_desc_set(d->perFrameDescSet);
	d->noTextureDescSet = VK_NULL_HANDLE;
	d->perFrameDescSet = VK_NULL_HANDLE;
	d->noTextureView = VK_NULL_HANDLE;
}

/* ---- the pipelines ---- */

static VkCullModeFlags cull_mode(u32 cull)
{
	return cull == 2 ? VK_CULL_MODE_BACK_BIT : cull == 1 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
}

static void stencil_op(VkStencilOpState *s, VkStencilOp fail, VkStencilOp pass, VkStencilOp depthFail, VkCompareOp compare,
		u32 compareMask, u32 writeMask, u32 reference)
{
	s->failOp = fail;
	s->passOp = pass;
	s->depthFailOp = depthFail;
	s->compareOp = compare;
	s->compareMask = compareMask;
	s->writeMask = writeMask;
	s->reference = reference;
}

/* a key's stencil state (VK_STENCIL_*) */
static void stencil_state(VkStencilOpState *s, u32 stencil)
{
	switch (stencil)
	{
	case VK_STENCIL_SHADOW_SET:
		stencil_op(s, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0, 0x80, 0x80);
		break;
	case VK_STENCIL_SHADOW_CLEAR:
		stencil_op(s, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0, 0x80, 0);
		break;
	/* Drawn again for its shadow: where this polygon is the one showing
	 * (same depth), takes shadows (stencil bit 7) and is in a volume
	 * (bit 0). Bit 6 marks the pixels drawn this way; whether one already
	 * marked may be drawn over is what tells the two kinds apart. */
	case VK_STENCIL_LATER_WINS:
		stencil_op(s, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0x81, 0x40, 0xC1);
		break;
	case VK_STENCIL_EARLIER_WINS:
		stencil_op(s, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xC1, 0x40, 0x81);
		break;
	default:
		stencil_op(s, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_NEVER, 0, 0, 0);
		break;
	}
}

static VkBlendFactor blend_factor(u32 instr, bool src)
{
	switch (instr)
	{
	case 0:	/* zero */
		return VK_BLEND_FACTOR_ZERO;
	case 1:	/* one */
		return VK_BLEND_FACTOR_ONE;
	case 2:	/* other color */
		return src ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
	case 3:	/* inverse other color */
		return src ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case 4:	/* src alpha */
		return VK_BLEND_FACTOR_SRC_ALPHA;
	case 5:	/* inverse src alpha */
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case 6:	/* dst alpha */
		return VK_BLEND_FACTOR_DST_ALPHA;
	default:	/* inverse dst alpha */
		return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	}
}

#define MODULE_BITS (((u64)1 << VK_KEY_GOURAUD) | ((u64)3 << VK_KEY_RATE))
#define DEPTH_STENCIL_BITS (((u64)3 << VK_KEY_CULL) | ((u64)1 << VK_KEY_LIST) | ((u64)1 << VK_KEY_ZWRITE) \
		| ((u64)7 << VK_KEY_DEPTH_OP) | ((u64)7 << VK_KEY_STENCIL))
#define BLEND_BITS (((u64)1 << VK_KEY_BLEND) | ((u64)7 << VK_KEY_SRC) | ((u64)7 << VK_KEY_DST))

static u64 key_of(const vk_pipeline_mgr *m, u32 listType, bool sortTriangles, const PolyParam *pp, int shadowPass)
{
	/* the shader */
	const bool texture = PPW(pp->pcw, PCW_Texture) != 0;
	const u32 pixelFmt = PPW(pp->tcw, TCW_PixelFmt);
	const bool trilinear = texture && PPW(pp->tsp, TSP_FilterMode) > 1 && listType != ListType_Punch_Through
			&& PPW(pp->tcw, TCW_MipMapped) == 1;
	const bool shadowed = listType == ListType_Opaque || listType == ListType_Punch_Through;
	u64 key;
	u32 depthOp, stencil = VK_STENCIL_NONE;
	bool depthWriteEnable;

	/* By the polygon alone. The clamping limits were looked at too, to leave
	 * the clamp out of the shader while they clamp nothing - but a pipeline
	 * is made once and kept under a key that has only the polygon's bit in
	 * it, and the limits change: a polygon first drawn while they clamped
	 * nothing was never clamped after. (Upstream: Red Dog.) */
	key = ((u64)(listType == ListType_Punch_Through) << TRI_SC_ALPHA_TEST)
		| ((u64)((pp->tileclip >> 28) == 3) << TRI_SC_CLIP_INSIDE)
		| ((u64)PPW(pp->tsp, TSP_UseAlpha) << TRI_SC_USE_ALPHA)
		| ((u64)texture << TRI_SC_TEXTURE)
		| ((u64)(PPW(pp->tsp, TSP_IgnoreTexA) || pixelFmt == TA_PIXEL_565) << TRI_SC_IGNORE_TEX_A)
		| ((u64)PPW(pp->tsp, TSP_ShadInstr) << TRI_SC_SHAD_INSTR)
		| ((u64)PPW(pp->pcw, PCW_Offset) << TRI_SC_OFFSET)
		| ((u64)PPW(pp->tsp, TSP_FogCtrl) << TRI_SC_FOG_CTRL)
		| ((u64)(pixelFmt == TA_PIXEL_BUMPMAP) << TRI_SC_BUMP_MAP)
		| ((u64)PPW(pp->tsp, TSP_ColorClamp) << TRI_SC_CLAMPING)
		| ((u64)trilinear << TRI_SC_TRILINEAR)
		| ((u64)tex_gpu_paletted(pp->tsp, pp->tcw, 0) << TRI_SC_PALETTE)
		| ((u64)(shadowPass != VK_SHADOW_NONE) << TRI_SC_SHADOWED)
		| ((u64)PPW(pp->pcw, PCW_Gouraud) << VK_KEY_GOURAUD);
	/* (SSAA: shaded at each sample, but for what is drawn at a single rate) */
	if (m->samples > 1)
		key |= (u64)(ssaa_single_rate(pp) ? 2 : 1) << VK_KEY_RATE;

	/* the fixed functions */
	key |= (u64)(PPW(pp->isp, ISP_TSP_CullMode) == 3 ? 2 : PPW(pp->isp, ISP_TSP_CullMode) == 2 ? 1 : 0) << VK_KEY_CULL;
	if (sortTriangles && !settings.pvr.Emulation.AlphaSortMode)
		key |= (u64)1 << VK_KEY_LIST;

	/* (the ISP's depth modes are VkCompareOp's, in order) */
	if (listType == ListType_Punch_Through || sortTriangles)
		depthOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
	else
		depthOp = PPW(pp->isp, ISP_TSP_DepthMode);
	/* Sorted translucent polygons, per triangle or per strip, do not write
	 * depth: a strip drawn in sorted order would hide parts of the ones after it. */
	if (sortTriangles)
		/* FIXME temporary work-around for intel driver bug */
		depthWriteEnable = vkc.vendorID == VENDOR_INTEL;
	/* Z Write Disable seems to be ignored for punch-through.
	 * Fixes Worms World Party, Bust-a-Move 4 and Re-Volt */
	else if (listType == ListType_Punch_Through)
		depthWriteEnable = true;
	else
		depthWriteEnable = !PPW(pp->isp, ISP_TSP_ZWriteDis);
	if (shadowPass != VK_SHADOW_NONE)
	{
		depthOp = VK_COMPARE_OP_EQUAL;
		depthWriteEnable = false;
		if (shadowed)
			stencil = shadowPass == VK_SHADOW_LATER_WINS ? VK_STENCIL_LATER_WINS : VK_STENCIL_EARLIER_WINS;
	}
	else if (shadowed)
		stencil = PPW(pp->pcw, PCW_Shadow) != 0 ? VK_STENCIL_SHADOW_SET : VK_STENCIL_SHADOW_CLEAR;
	key |= ((u64)depthWriteEnable << VK_KEY_ZWRITE) | ((u64)depthOp << VK_KEY_DEPTH_OP) | ((u64)stencil << VK_KEY_STENCIL);

	/* Apparently punch-through polys support blending, or at least some
	 * combinations. An opaque polygon is blended by its instructions too
	 * when it is in a list continuation; the ones of the first render pass
	 * have had theirs set to source x 1 and destination x 0 when the list
	 * was read. That pair is no blending at all, and is left as that so it
	 * costs nothing. */
	if (shadowPass == VK_SHADOW_NONE
			&& (listType == ListType_Translucent || listType == ListType_Punch_Through
				|| PPW(pp->tsp, TSP_SrcInstr) != 1 || PPW(pp->tsp, TSP_DstInstr) != 0))
		key |= ((u64)1 << VK_KEY_BLEND) | ((u64)PPW(pp->tsp, TSP_SrcInstr) << VK_KEY_SRC)
			| ((u64)PPW(pp->tsp, TSP_DstInstr) << VK_KEY_DST);
	return key;
}

/* the key of the general pipeline that draws for @key */
static u64 general_key(u64 key)
{
	u64 fixed = MODULE_BITS;
	if (!vkc.dynamicState.depthStencil)
		fixed |= DEPTH_STENCIL_BITS;
	if (!vkc.dynamicState.blend)
		fixed |= BLEND_BITS;
	return (key & fixed) | ((u64)VK_KIND_GENERAL << VK_KEY_KIND);
}

PsoChoice vk_pipeline_mgr_get(vk_pipeline_mgr *m, u32 listType, bool sortTriangles, const PolyParam *pp, int shadow)
{
	const u64 key = key_of(m, listType, sortTriangles, pp, shadow);
	PsoSlot *slot, *slotGeneral;
	PsoChoice choice;
	u64 generalKey;

	if (key == m->lastKey)
		return m->lastChoice;
	slot = vk_pso_table_find(&m->pipelines, key);
	if (slot == NULL && vk_pso_worker_running() && !PsoTestGeneral)
	{
		/* its own pipeline, asked for now; drawn with the general one until it is made */
		slot = vk_pso_table_insert(&m->pipelines, key);
		if (slot != NULL)
		{
			vk_pso_worker_request(slot);
			vk_pso_keys_record(&vkc.psoKeys, m->who, key);
		}
	}
	choice.key = key;
	if (PsoReady(slot))
	{
		m->lastKey = key;
		choice.pipeline = slot->pipeline;
		choice.general = false;
		m->lastChoice = choice;
		return choice;
	}
	generalKey = general_key(key);
	slotGeneral = vk_pso_table_find(&m->general, generalKey);
	if (slotGeneral == NULL)
	{
		/* A device that cannot set this polygon's depth, stencil or blend
		 * states with the draw, and a general pipeline for them not made
		 * when the renderer started: made now, the draw waiting for it */
		slotGeneral = vk_pso_table_insert(&m->general, generalKey);
		if (slotGeneral != NULL)
			vk_pso_worker_build_sync(slotGeneral, vkc.pipelineCache);
	}
	if (slotGeneral == NULL)
	{
		choice.pipeline = VK_NULL_HANDLE;
		choice.general = false;
		return choice;
	}
	if (PsoReady(slotGeneral))
	{
		choice.pipeline = slotGeneral->pipeline;
		choice.general = true;
		return choice;
	}
	/* (one handed to the worker at the start, not made yet: taken, made or waited for) */
	choice.pipeline = vk_pso_worker_need(slotGeneral);
	choice.general = choice.pipeline != VK_NULL_HANDLE;
	return choice;
}

void vk_pipeline_mgr_set_dynamic_state(const vk_pipeline_mgr *m, VkCommandBuffer cmd, u64 key)
{
	const vk_pso_dyn *f = &vkc.dynamicState;
	(void)m;
	if (f->depthStencil)
	{
		const u32 stencil = (u32)(key >> VK_KEY_STENCIL) & 7;
		VkStencilOpState state;
		stencil_state(&state, stencil);
		f->CmdSetCullMode(cmd, cull_mode((u32)(key >> VK_KEY_CULL) & 3));
		f->CmdSetPrimitiveTopology(cmd, ((key >> VK_KEY_LIST) & 1) ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
		f->CmdSetDepthTestEnable(cmd, VK_TRUE);
		f->CmdSetDepthWriteEnable(cmd, (VkBool32)((key >> VK_KEY_ZWRITE) & 1));
		f->CmdSetDepthCompareOp(cmd, (VkCompareOp)((key >> VK_KEY_DEPTH_OP) & 7));
		f->CmdSetStencilTestEnable(cmd, stencil != VK_STENCIL_NONE);
		f->CmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.failOp, state.passOp, state.depthFailOp, state.compareOp);
		vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.compareMask);
		vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.writeMask);
		vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.reference);
	}
	if (f->blend)
	{
		const VkBool32 enable = (VkBool32)((key >> VK_KEY_BLEND) & 1);
		const VkColorComponentFlags writeMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
				| VK_COLOR_COMPONENT_A_BIT;
		vk_pso_blend_equation equation;
		memset(&equation, 0, sizeof(equation));
		f->CmdSetColorBlendEnable(cmd, 0, 1, &enable);
		equation.colorBlendOp = VK_BLEND_OP_ADD;
		equation.alphaBlendOp = VK_BLEND_OP_ADD;
		if (enable)
		{
			equation.srcColorBlendFactor = equation.srcAlphaBlendFactor = blend_factor((u32)(key >> VK_KEY_SRC) & 7, true);
			equation.dstColorBlendFactor = equation.dstAlphaBlendFactor = blend_factor((u32)(key >> VK_KEY_DST) & 7, false);
		}
		f->CmdSetColorBlendEquation(cmd, 0, 1, &equation);
		f->CmdSetColorWriteMask(cmd, 0, 1, &writeMask);
	}
}

VkPipeline vk_pipeline_mgr_prepass(vk_pipeline_mgr *m, const PolyParam *pp)
{
	const bool sampleRate = m->samples > 1 && !ssaa_single_rate(pp);
	return vk_pso_get(&m->prepassSlots[PPW(pp->pcw, PCW_Texture)][PPW(pp->pcw, PCW_Gouraud)]
			[PsoCullIndex(PPW(pp->isp, ISP_TSP_CullMode)) & m->cullKept][sampleRate]);
}

static void drop_slots(PsoSlot *slots, u32 n)
{
	u32 i;
	for (i = 0; i < n; i++)
	{
		if (slots[i].pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(vkc.device, slots[i].pipeline, NULL);
		slots[i].pipeline = VK_NULL_HANDLE;
		slots[i].owner = NULL;
	}
}

void vk_pipeline_mgr_reset(vk_pipeline_mgr *m)
{
	/* (nothing of it is to be made any more; what is being made, finished) */
	vk_pso_builder_new_generation(&m->builder);
	vk_pso_worker_end_batch(&m->builder);
	vk_pso_worker_wait_idle();
	if (vkc.device != VK_NULL_HANDLE)
	{
		vk_pso_table_clear(&m->pipelines, vkc.device);
		vk_pso_table_clear(&m->general, vkc.device);
		drop_slots(m->modVolSlots, ARRAY_SIZE(m->modVolSlots));
		drop_slots(&m->prepassSlots[0][0][0][0], sizeof(m->prepassSlots) / sizeof(PsoSlot));
	}
	m->lastKey = ~(u64)0;
}

/* ---- making them ---- */

static const VkVertexInputBindingDescription main_bindings[] = { { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX } };
static const VkVertexInputAttributeDescription main_attributes[] =
{
	{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, x) },		/* pos */
	{ 1, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vertex, col) },		/* base color */
	{ 2, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vertex, vtx_spc) },	/* offset color */
	{ 3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u) },			/* tex coord */
};
static const VkVertexInputBindingDescription modvol_bindings[] = { { 0, sizeof(float) * 3, VK_VERTEX_INPUT_RATE_VERTEX } };
static const VkVertexInputAttributeDescription modvol_attributes[] = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 } };	/* pos */

/* What every pipeline here has the same of, or nearly: filled in by the
 * builders, then given to vk_pso_create() */
typedef struct pipe_states
{
	VkPipelineVertexInputStateCreateInfo vertexInput;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depthStencil;
	VkPipelineColorBlendAttachmentState blendAttachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkDynamicState dynamicStates[18];
	VkPipelineDynamicStateCreateInfo dynamic;
	VkPipelineShaderStageCreateInfo stages[2];
	VkSpecializationMapEntry entries[3];
	VkSpecializationInfo specialization;
	s32 values[3];
} pipe_states;

/* @full: the main vertices' every attribute, else their position only;
 * @samples a pixel, shaded at each sample (@perSample) or once a pixel */
static void states_init(pipe_states *s, bool full, VkPrimitiveTopology topology, u32 cull, u32 samples, bool perSample)
{
	memset(s, 0, sizeof(*s));
	s->vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	s->vertexInput.vertexBindingDescriptionCount = ARRAY_SIZE(main_bindings);
	s->vertexInput.pVertexBindingDescriptions = main_bindings;
	s->vertexInput.vertexAttributeDescriptionCount = full ? ARRAY_SIZE(main_attributes) : 1;
	s->vertexInput.pVertexAttributeDescriptions = main_attributes;
	s->inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	s->inputAssembly.topology = topology;
	s->viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	s->viewport.viewportCount = 1;
	s->viewport.scissorCount = 1;
	s->rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	s->rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	s->rasterization.cullMode = cull_mode(cull);
	s->rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	s->rasterization.lineWidth = 1.0f;
	s->multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	s->multisample.rasterizationSamples = (VkSampleCountFlagBits)samples;
	s->multisample.sampleShadingEnable = perSample && samples > 1;
	s->multisample.minSampleShading = 1.f;
	s->depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	s->blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	s->blend.logicOp = VK_LOGIC_OP_NO_OP;
	s->blend.attachmentCount = 1;
	s->blend.pAttachments = &s->blendAttachment;
	s->blend.blendConstants[0] = s->blend.blendConstants[1] = s->blend.blendConstants[2] = s->blend.blendConstants[3] = 1.0f;
	s->dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
	s->dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
	s->dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	s->dynamic.dynamicStateCount = 2;
	s->dynamic.pDynamicStates = s->dynamicStates;
}

static void states_dynamic(pipe_states *s, VkDynamicState state)
{
	s->dynamicStates[s->dynamic.dynamicStateCount++] = state;
}

/* the stages: @specialized, with the pipeline's specialization constants
 * (pso_key.h): the key, -1 for the one read from the push constants, and
 * SSAA's samples a pixel */
static void states_stages(pipe_states *s, VkShaderModule vertex, VkShaderModule fragment, bool specialized, s32 key, u32 samples)
{
	u32 log2n = 0, i;
	for (i = 0; i < 2; i++)
	{
		s->stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		s->stages[i].pName = "main";
	}
	s->stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	s->stages[0].module = vertex;
	s->stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	s->stages[1].module = fragment;
	if (!specialized)
		return;
	while ((2u << log2n) <= samples)
		log2n++;
	s->values[0] = key;
	s->values[1] = (s32)samples;
	s->values[2] = (s32)log2n;
	s->entries[0].constantID = PSO_SC_KEY;
	s->entries[0].offset = 0;
	s->entries[0].size = sizeof(s32);
	s->entries[1].constantID = PSO_SC_SSAA_SAMPLES;
	s->entries[1].offset = sizeof(s32);
	s->entries[1].size = sizeof(s32);
	s->entries[2].constantID = PSO_SC_SSAA_LOG2N;
	s->entries[2].offset = 2 * sizeof(s32);
	s->entries[2].size = sizeof(s32);
	s->specialization.mapEntryCount = 3;
	s->specialization.pMapEntries = s->entries;
	s->specialization.dataSize = sizeof(s->values);
	s->specialization.pData = s->values;
	s->stages[1].pSpecializationInfo = &s->specialization;
}

static VkPipeline states_make(const vk_pipeline_mgr *m, pipe_states *s, VkPipelineCache cache)
{
	VkGraphicsPipelineCreateInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = 2;
	info.pStages = s->stages;
	info.pVertexInputState = &s->vertexInput;
	info.pInputAssemblyState = &s->inputAssembly;
	info.pViewportState = &s->viewport;
	info.pRasterizationState = &s->rasterization;
	info.pMultisampleState = &s->multisample;
	info.pDepthStencilState = &s->depthStencil;
	info.pColorBlendState = &s->blend;
	info.pDynamicState = &s->dynamic;
	info.layout = m->pipelineLayout;
	info.renderPass = m->renderPass;
	return vk_pso_create(vkc.device, cache, &info);
}

/* @cull: PsoCullIndex()'s (none where it is dynamic) */
static VkPipeline build_modvol(const vk_pipeline_mgr *m, int mode, u32 cull, VkPipelineCache cache)
{
	const bool main = mode == VK_MODVOL_FINAL || mode == VK_MODVOL_DEPTH;
	VkStencilOpState stencil;
	pipe_states s;

	states_init(&s, false, mode == VK_MODVOL_FINAL ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
			cull, m->samples, false);
	if (!main)
	{
		s.vertexInput.pVertexBindingDescriptions = modvol_bindings;
		s.vertexInput.pVertexAttributeDescriptions = modvol_attributes;
	}

	/* depth and stencil */
	memset(&stencil, 0, sizeof(stencil));
	switch (mode)
	{
	case VK_MODVOL_XOR:
		stencil_op(&stencil, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0, 2, 2);
		break;
	case VK_MODVOL_OR:
		stencil_op(&stencil, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 2, 2, 2);
		break;
	case VK_MODVOL_INCLUSION:
		stencil_op(&stencil, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_ZERO, VK_COMPARE_OP_LESS_OR_EQUAL, 3, 3, 1);
		break;
	case VK_MODVOL_EXCLUSION:
		stencil_op(&stencil, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_COMPARE_OP_EQUAL, 3, 3, 1);
		break;
	case VK_MODVOL_FINAL:
		/* takes shadows, in a volume, and not drawn again for it (bit 6);
		 * clears the volume bits and that mark */
		stencil_op(&stencil, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_ZERO, VK_COMPARE_OP_EQUAL, 0xC1, 0x43, 0x81);
		break;
	default:
		break;
	}
	s.depthStencil.depthTestEnable = mode == VK_MODVOL_XOR || mode == VK_MODVOL_OR || mode == VK_MODVOL_DEPTH;
	s.depthStencil.depthWriteEnable = mode == VK_MODVOL_DEPTH;
	s.depthStencil.depthCompareOp = mode == VK_MODVOL_DEPTH ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_GREATER;
	s.depthStencil.stencilTestEnable = mode != VK_MODVOL_DEPTH;
	s.depthStencil.front = stencil;
	s.depthStencil.back = stencil;

	/* colour and blending */
	s.blendAttachment.blendEnable = mode == VK_MODVOL_FINAL;
	s.blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	s.blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	s.blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	s.blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	s.blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	s.blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	s.blendAttachment.colorWriteMask = mode != VK_MODVOL_FINAL ? 0
			: VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	/* (the cull mode set with the draws, where it can be: vk_pso_cull) */
	if (m->cullSetter != NULL && mode != VK_MODVOL_FINAL)
		states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_CULL_MODE);

	states_stages(&s, m->modules->m[RSPV_MODVOL_VERT], m->modules->m[RSPV_MODVOL_FRAG], false, 0, 0);
	return states_make(m, &s, cache);
}

/* A polygon's pipeline, all of it from @key; @general: the one that draws
 * for the keys that differ from it only in what the draw sets (pso.h) */
static VkPipeline build_poly(const vk_pipeline_mgr *m, u64 key, bool general, VkPipelineCache cache)
{
	const u32 rate = (u32)(key >> VK_KEY_RATE) & 3;
	const u32 stencil = (u32)(key >> VK_KEY_STENCIL) & 7;
	const bool gouraud = ((key >> VK_KEY_GOURAUD) & 1) != 0;
	pipe_states s;

	states_init(&s, true, ((key >> VK_KEY_LIST) & 1) ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
			(u32)(key >> VK_KEY_CULL) & 3, m->samples, rate == 1);

	/* depth and stencil */
	stencil_state(&s.depthStencil.front, stencil);
	s.depthStencil.back = s.depthStencil.front;
	s.depthStencil.depthTestEnable = VK_TRUE;
	s.depthStencil.depthWriteEnable = (VkBool32)((key >> VK_KEY_ZWRITE) & 1);
	s.depthStencil.depthCompareOp = (VkCompareOp)((key >> VK_KEY_DEPTH_OP) & 7);
	s.depthStencil.stencilTestEnable = stencil != VK_STENCIL_NONE;

	/* colour and blending */
	s.blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	s.blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	s.blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
			| VK_COLOR_COMPONENT_A_BIT;
	if ((key >> VK_KEY_BLEND) & 1)
	{
		const u32 src = (u32)(key >> VK_KEY_SRC) & 7, dst = (u32)(key >> VK_KEY_DST) & 7;
		s.blendAttachment.blendEnable = VK_TRUE;
		s.blendAttachment.srcColorBlendFactor = s.blendAttachment.srcAlphaBlendFactor = blend_factor(src, true);
		s.blendAttachment.dstColorBlendFactor = s.blendAttachment.dstAlphaBlendFactor = blend_factor(dst, false);
	}

	/* the general pipeline's states that are set with each draw (vk_pipeline_mgr_set_dynamic_state()) */
	if (general)
	{
		if (vkc.dynamicState.depthStencil)
		{
			states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_CULL_MODE);
			states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY);
			states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_DEPTH_TEST_ENABLE);
			states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_DEPTH_WRITE_ENABLE);
			states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_DEPTH_COMPARE_OP);
			states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_STENCIL_TEST_ENABLE);
			states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_STENCIL_OP);
			states_dynamic(&s, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
			states_dynamic(&s, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
			states_dynamic(&s, VK_DYNAMIC_STATE_STENCIL_REFERENCE);
		}
		if (vkc.dynamicState.blend)
		{
			states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_COLOR_BLEND_ENABLE);
			states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_COLOR_BLEND_EQUATION);
			states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_COLOR_WRITE_MASK);
		}
	}

	/* the module of the interpolation and sample rate: the rest is the key's */
	states_stages(&s, m->modules->m[gouraud ? RSPV_TRI_VERT_G1 : RSPV_TRI_VERT_G0],
			m->modules->m[RSPV_TRI_FRAG_G0_R0 + (gouraud ? 3 : 0) + rate],
			true, general ? -1 : (s32)(key & ((1u << TRI_SC_BITS) - 1)), m->samples);
	return states_make(m, &s, cache);
}

/* A pipeline of the depth prepass of a sorted translucent list (rend/sorter.h):
 * depth only, written where the depth test passes (the colour pass's test) */
static VkPipeline build_prepass(const vk_pipeline_mgr *m, int texture, int gouraud, u32 cull, int rate, VkPipelineCache cache)
{
	pipe_states s;
	states_init(&s, true, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, cull, m->samples, rate == 1);
	s.depthStencil.depthTestEnable = VK_TRUE;
	s.depthStencil.depthWriteEnable = VK_TRUE;
	s.depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
	s.blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	s.blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	/* (the cull mode set with the draws, where it can be: vk_pso_cull) */
	if (m->cullSetter != NULL)
		states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_CULL_MODE);
	/* (the module: textured, Gouraud, and the rate: 1 at each sample, 2 once a pixel) */
	states_stages(&s, m->modules->m[gouraud ? RSPV_TRI_VERT_G1 : RSPV_TRI_VERT_G0],
			m->modules->m[RSPV_PREPASS_FRAG_T0_G0_R0 + (texture ? 6 : 0) + (gouraud ? 3 : 0)
				+ (m->samples > 1 ? (rate == 1 ? 1 : 2) : 0)],
			true, 0, m->samples);
	return states_make(m, &s, cache);
}

/* the pipeline thread's, and a draw's */
static VkPipeline build(vk_pso_builder *self, u64 key, VkPipelineCache cache)
{
	const vk_pipeline_mgr *m = (const vk_pipeline_mgr *)self;
	switch ((key >> VK_KEY_KIND) & 15)
	{
	case VK_KIND_POLY:
		return build_poly(m, key, false, cache);
	case VK_KIND_GENERAL:
		return build_poly(m, key, true, cache);
	case VK_KIND_MODVOL:
		return build_modvol(m, (int)((key >> 2) & 7), (u32)(key & 3), cache);
	case VK_KIND_PREPASS:
		return build_prepass(m, (int)((key >> 4) & 1), (int)((key >> 3) & 1), (u32)((key >> 1) & 3), (int)(key & 1), cache);
	default:
		return VK_NULL_HANDLE;
	}
}

void vk_pipeline_mgr_construct(vk_pipeline_mgr *m, u32 who)
{
	memset(m, 0, sizeof(*m));
	m->builder.build = build;
	retro_atomic_int_init(&m->builder.generation, 0);
	m->cullKept = 3;
	m->lastKey = ~(u64)0;
	m->samples = 1;
	m->who = who;
}

/* Prepare()'s list of slots to make now */
typedef struct slot_list
{
	PsoSlot **slots;
	u32 count, cap;
} slot_list;

static void add_general(vk_pipeline_mgr *m, slot_list *now, u64 key)
{
	const u64 generalKey = general_key(key);
	if (vk_pso_table_find(&m->general, generalKey) == NULL)
	{
		PsoSlot *slot = vk_pso_table_insert(&m->general, generalKey);
		if (slot != NULL && now->count < now->cap)
			now->slots[now->count++] = slot;
	}
}

static bool key_fits(const vk_pipeline_mgr *m, u64 key)
{
	const u32 rate = (u32)(key >> VK_KEY_RATE) & 3;
	return ((key >> VK_KEY_KIND) & 15) == VK_KIND_POLY && (m->samples > 1 ? rate == 1 || rate == 2 : rate == 0);
}

static int compare_keys(const void *a, const void *b)
{
	const u64 x = *(const u64 *)a, y = *(const u64 *)b;
	return x < y ? -1 : x > y;
}

/* (the drawer's lists, sorted or not, and those drawn again for their shadows) */
static const struct { u32 list; bool sort; int shadow; } reach_draws[] =
{
	{ ListType_Opaque, false, VK_SHADOW_NONE }, { ListType_Punch_Through, false, VK_SHADOW_NONE },
	{ ListType_Translucent, false, VK_SHADOW_NONE }, { ListType_Translucent, true, VK_SHADOW_NONE },
	{ ListType_Opaque, false, VK_SHADOW_LATER_WINS }, { ListType_Opaque, false, VK_SHADOW_EARLIER_WINS },
	{ ListType_Punch_Through, false, VK_SHADOW_LATER_WINS }, { ListType_Punch_Through, false, VK_SHADOW_EARLIER_WINS },
};

/* A device that has a general pipeline for each fixed state: one for every
 * state a draw can reach, worked out by key_of() itself from the draws the
 * drawer makes and every value of the fields it reads. All made at the
 * start, before the first frame. Where they are too many (a device with
 * neither extension, the per-pixel renderer's passes: thousands, until the
 * blend is folded as Direct3D 12's is), those of earlier runs are, and one
 * not seen before is made when first drawn (vk_pso_worker_sync_built()
 * counts them). */
static void add_reachable(vk_pipeline_mgr *m, slot_list *now)
{
	const u32 rates = m->samples > 1 ? 2 : 1;
	const u64 firstRate = m->samples > 1 ? 1 : 0;
	const size_t most = ARRAY_SIZE(reach_draws) * ((size_t)1 << 13) * 2 * 2 * rates;
	u64 *reach = (u64 *)malloc(most * sizeof(u64));
	size_t n = 0, unique = 0, i;
	unsigned d;

	if (reach == NULL)
		return;
	for (d = 0; d < ARRAY_SIZE(reach_draws); d++)
	{
		u32 bits;
		for (bits = 0; bits < (1u << 13); bits++)
		{
			PolyParam pp;
			u64 fixed, list, gouraud, rate;
			memset(&pp, 0, sizeof(pp));
			pp.isp = TAW_SET(pp.isp, ISP_TSP_CullMode, bits & 3);
			pp.isp = TAW_SET(pp.isp, ISP_TSP_DepthMode, (bits >> 2) & 7);
			pp.isp = TAW_SET(pp.isp, ISP_TSP_ZWriteDis, (bits >> 5) & 1);
			pp.pcw = TAW_SET(pp.pcw, PCW_Shadow, (bits >> 6) & 1);
			pp.tsp = TAW_SET(pp.tsp, TSP_SrcInstr, (bits >> 7) & 7);
			pp.tsp = TAW_SET(pp.tsp, TSP_DstInstr, (bits >> 10) & 7);
			/* (drawn again only as CanDrawShadowed() lets it) */
			if (reach_draws[d].shadow != VK_SHADOW_NONE && (!PPW(pp.pcw, PCW_Shadow)
					|| PPW(pp.tsp, TSP_SrcInstr) != 1 || PPW(pp.tsp, TSP_DstInstr) != 0))
				continue;
			fixed = key_of(m, reach_draws[d].list, reach_draws[d].sort, &pp, reach_draws[d].shadow) & (DEPTH_STENCIL_BITS | BLEND_BITS);
			/* (sorted: as triangles or strips, which the sort setting decides while a game runs) */
			for (list = 0; list < (reach_draws[d].sort ? 2u : 1u); list++)
				for (gouraud = 0; gouraud < 2; gouraud++)
					for (rate = firstRate; rate < firstRate + rates; rate++)
						reach[n++] = general_key((fixed ^ (list << VK_KEY_LIST)) | (gouraud << VK_KEY_GOURAUD) | (rate << VK_KEY_RATE));
		}
	}
	qsort(reach, n, sizeof(u64), compare_keys);
	for (i = 0; i < n; i++)
		if (unique == 0 || reach[i] != reach[unique - 1])
			reach[unique++] = reach[i];
	if (unique <= PSO_GENERAL_AT_START)
		for (i = 0; i < unique; i++)
			add_general(m, now, reach[i]);
	else
	{
		WARN_LOG(RENDERER, "Vulkan: %u general pipelines a draw can reach, too many to make at the start\n", (u32)unique);
		for (i = 0; i < vkc.psoKeys.count; i++)
			if ((vkc.psoKeys.keys[i] >> 60) == m->who && key_fits(m, vkc.psoKeys.keys[i] & ~((u64)15 << 60)))
				add_general(m, now, vkc.psoKeys.keys[i] & ~((u64)15 << 60));
	}
	free(reach);
}

static void prepare(vk_pipeline_mgr *m)
{
	const int culls = vkc.dynamicState.depthStencil ? 1 : 3;
	slot_list now;
	u64 gouraud, rate;
	int mode, cull, texture, g, r;
	u32 i;

	/* (made with the renderer, not while a game runs) */
	now.cap = m->general.mask + 1 + ARRAY_SIZE(m->modVolSlots) + sizeof(m->prepassSlots) / sizeof(PsoSlot);
	now.count = 0;
	now.slots = (PsoSlot **)malloc(now.cap * sizeof(PsoSlot *));
	if (now.slots == NULL)
		now.cap = 0;
	/* the general pipelines: of each module, and of each fixed state the device must have made with them */
	for (gouraud = 0; gouraud < 2; gouraud++)
		for (rate = m->samples > 1 ? 1 : 0; rate <= (m->samples > 1 ? 2u : 0u); rate++)
			add_general(m, &now, (gouraud << VK_KEY_GOURAUD) | (rate << VK_KEY_RATE));
	if (!vkc.dynamicState.depthStencil || !vkc.dynamicState.blend)
		add_reachable(m, &now);
	m->cullSetter = vkc.dynamicState.depthStencil ? vkc.dynamicState.CmdSetCullMode : NULL;
	m->cullKept = m->cullSetter != NULL ? 0 : 3;
	/* the modifier volumes' */
	for (mode = 0; mode <= VK_MODVOL_DEPTH; mode++)
		for (cull = 0; cull < culls; cull++)
		{
			PsoSlot *slot = &m->modVolSlots[mode * 3 + cull];
			if (mode == VK_MODVOL_FINAL && cull != 0)
				continue;
			slot->key = ((u64)VK_KIND_MODVOL << VK_KEY_KIND) | ((u64)mode << 2) | (u64)cull;
			slot->owner = &m->builder;
			slot->pipeline = VK_NULL_HANDLE;
			if (now.count < now.cap)
				now.slots[now.count++] = slot;
		}
	/* the depth prepass's (rend/sorter.h) */
	for (texture = 0; texture < 2; texture++)
		for (g = 0; g < 2; g++)
			for (cull = 0; cull < culls; cull++)
				for (r = 0; r < (m->samples > 1 ? 2 : 1); r++)
				{
					PsoSlot *slot = &m->prepassSlots[texture][g][cull][r];
					slot->key = ((u64)VK_KIND_PREPASS << VK_KEY_KIND) | (u64)((texture << 4) | (g << 3) | (cull << 1) | r);
					slot->owner = &m->builder;
					slot->pipeline = VK_NULL_HANDLE;
					if (now.count < now.cap)
						now.slots[now.count++] = slot;
				}
	NOTICE_LOG(RENDERER, "Vulkan: %u general pipelines made at the start\n", m->general.used);
	vk_pso_worker_build_soon(now.slots, now.count, vkc.pipelineCache);
	free(now.slots);
	/* The polygons' own pipelines made in earlier runs: asked for now, made
	 * while nothing more pressing is */
	if (vk_pso_worker_running())
		for (i = 0; i < vkc.psoKeys.count; i++)
		{
			const u64 key = vkc.psoKeys.keys[i] & ~((u64)15 << 60);
			if ((vkc.psoKeys.keys[i] >> 60) == m->who && key_fits(m, key) && vk_pso_table_find(&m->pipelines, key) == NULL)
			{
				PsoSlot *slot = vk_pso_table_insert(&m->pipelines, key);
				if (slot != NULL)
					vk_pso_worker_prefetch(slot);
			}
		}
}

static bool make_layouts(vk_pipeline_mgr *m)
{
	VkDescriptorSetLayoutBinding perFrame[4], perPoly[1];
	VkDescriptorSetLayoutCreateInfo setInfo;
	VkDescriptorSetLayout layouts[2];
	VkPushConstantRange pushConstant;
	VkPipelineLayoutCreateInfo layoutInfo;
	unsigned i;

	memset(perFrame, 0, sizeof(perFrame));
	memset(perPoly, 0, sizeof(perPoly));
	for (i = 0; i < 4; i++)
	{
		perFrame[i].binding = i;
		perFrame[i].descriptorCount = 1;
		perFrame[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		perFrame[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	}
	perFrame[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;	/* vertex uniforms */
	perFrame[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	perFrame[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;	/* fragment uniforms */
	/* 2: the fog texture, 3: the palette texture */
	perPoly[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;	/* texture */
	perPoly[0].descriptorCount = 1;
	perPoly[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	memset(&setInfo, 0, sizeof(setInfo));
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setInfo.bindingCount = ARRAY_SIZE(perFrame);
	setInfo.pBindings = perFrame;
	if (vkCreateDescriptorSetLayout(vkc.device, &setInfo, NULL, &m->perFrameLayout) != VK_SUCCESS)
		return false;
	setInfo.bindingCount = ARRAY_SIZE(perPoly);
	setInfo.pBindings = perPoly;
	if (vkCreateDescriptorSetLayout(vkc.device, &setInfo, NULL, &m->perPolyLayout) != VK_SUCCESS)
		return false;
	layouts[0] = m->perFrameLayout;
	layouts[1] = m->perPolyLayout;
	/* (six floats, then the general pipelines' key: pso_key.h) */
	pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstant.offset = 0;
	pushConstant.size = TRI_PUSH_KEY_OFFSET + 4;
	memset(&layoutInfo, 0, sizeof(layoutInfo));
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 2;
	layoutInfo.pSetLayouts = layouts;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pushConstant;
	if (vkCreatePipelineLayout(vkc.device, &layoutInfo, NULL, &m->pipelineLayout) != VK_SUCCESS)
		return false;
	vk_pso_table_init(&m->pipelines, 4096, &m->builder);
	vk_pso_table_init(&m->general, 4096, &m->builder);
	return true;
}

bool vk_pipeline_mgr_init(vk_pipeline_mgr *m, vk_raster_modules *modules, VkRenderPass renderPass, u32 samples)
{
	m->modules = modules;
	if (!vk_raster_modules_make(modules, RSPV_TRI_VERT_G0, RSPV_PREPASS_FRAG_T1_G1_R2))
		return false;
	if (m->perFrameLayout == VK_NULL_HANDLE && !make_layouts(m))
		return false;
	if (m->renderPass != renderPass || m->samples != samples)
	{
		vk_pipeline_mgr_reset(m);
		m->renderPass = renderPass;
		m->samples = samples;
		prepare(m);
	}
	return true;
}

/* The render pass of a render to a texture without SSAA */
static VkRenderPass make_rtt_render_pass(void)
{
	VkAttachmentDescription attachments[2];
	VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthReference = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass;
	VkSubpassDependency deps[3];
	VkSubpassDependency *d = deps;
	VkRenderPassCreateInfo info;
	VkRenderPass renderPass = VK_NULL_HANDLE;

	memset(attachments, 0, sizeof(attachments));
	attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	attachments[1].format = vkc.depthFormat;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorReference;
	subpass.pDepthStencilAttachment = &depthReference;
	d = dep(d, VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
	d = dep(d, 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0);
	/* one depth attachment for every render to a texture: the last one done with it before this one clears it */
	d = dep(d, VK_SUBPASS_EXTERNAL, 0,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0);
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 2;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = (u32)(d - deps);
	info.pDependencies = deps;
	if (vkCreateRenderPass(vkc.device, &info, NULL, &renderPass) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return renderPass;
}

bool vk_pipeline_mgr_rtt_init(vk_pipeline_mgr *m, vk_raster_modules *modules, u32 samples)
{
	/* (made before the one it replaces goes: never the same handle) */
	const VkRenderPass old = m->rttRenderPass;
	bool ok;
	m->rttRenderPass = samples > 1
			? vk_make_ssaa_render_pass(VK_FORMAT_R8G8B8A8_UNORM, samples, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_UNDEFINED)
			: make_rtt_render_pass();
	if (m->rttRenderPass == VK_NULL_HANDLE)
	{
		m->rttRenderPass = old;
		return false;
	}
	ok = vk_pipeline_mgr_init(m, modules, m->rttRenderPass, samples > 1 ? samples : 1);
	if (old != VK_NULL_HANDLE)
		vkDestroyRenderPass(vkc.device, old, NULL);
	return ok;
}

void vk_pipeline_mgr_term(vk_pipeline_mgr *m)
{
	vk_pipeline_mgr_reset(m);
	vk_pso_table_free(&m->pipelines);
	vk_pso_table_free(&m->general);
	if (vkc.device != VK_NULL_HANDLE)
	{
		if (m->pipelineLayout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(vkc.device, m->pipelineLayout, NULL);
		if (m->perFrameLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(vkc.device, m->perFrameLayout, NULL);
		if (m->perPolyLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(vkc.device, m->perPolyLayout, NULL);
		if (m->rttRenderPass != VK_NULL_HANDLE)
			vkDestroyRenderPass(vkc.device, m->rttRenderPass, NULL);
	}
	m->pipelineLayout = VK_NULL_HANDLE;
	m->perFrameLayout = VK_NULL_HANDLE;
	m->perPolyLayout = VK_NULL_HANDLE;
	m->rttRenderPass = VK_NULL_HANDLE;
	m->renderPass = VK_NULL_HANDLE;
	m->samples = 1;
}

VkRenderPass vk_make_ssaa_render_pass(VkFormat colorFormat, u32 samples, VkImageLayout pictureLayout, VkFormat hdrFormat)
{
	const bool hdr = hdrFormat != VK_FORMAT_UNDEFINED;
	const VkSampleCountFlagBits n = (VkSampleCountFlagBits)samples;
	const VkPipelineStageFlags fbStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
			| VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	const VkAccessFlags fbWrites = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	const VkAccessFlags fbAll = fbWrites | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
			| VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
	VkAttachmentDescription attachments[4];
	VkAttachmentReference pictureReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference hdrReference = { 3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference colorReference = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthReference = { 2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkAttachmentReference samplesInput = { 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkAttachmentReference hdrInputs[2] = { { 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
	VkSubpassDescription subpasses[3];
	VkSubpassDependency deps[8];
	VkSubpassDependency *d = deps;
	VkRenderPassCreateInfo info;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	unsigned i;

	memset(attachments, 0, sizeof(attachments));
	for (i = 0; i < 4; i++)
	{
		attachments[i].samples = i == 1 || i == 2 ? n : VK_SAMPLE_COUNT_1_BIT;
		attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	}
	/* the picture */
	attachments[0].format = colorFormat;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].finalLayout = pictureLayout;
	/* the samples' colour and depth */
	attachments[1].format = colorFormat;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	attachments[2].format = vkc.depthFormat;
	attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	/* the HDR output's image */
	attachments[3].format = hdrFormat;
	attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[3].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	memset(subpasses, 0, sizeof(subpasses));
	for (i = 0; i < 3; i++)
	{
		subpasses[i].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpasses[i].colorAttachmentCount = 1;
	}
	subpasses[0].pColorAttachments = &colorReference;
	subpasses[0].pDepthStencilAttachment = &depthReference;
	subpasses[1].inputAttachmentCount = 1;
	subpasses[1].pInputAttachments = &samplesInput;
	subpasses[1].pColorAttachments = &pictureReference;
	/* the HDR output: the picture encoded where it is, from the samples' exact average where it is their resolve */
	subpasses[2].inputAttachmentCount = 2;
	subpasses[2].pInputAttachments = hdrInputs;
	subpasses[2].pColorAttachments = &hdrReference;

	/* The sample attachments are the same every frame: the one before has
	 * to be done with them, the resolve's reading included, before this one
	 * clears them; and the picture is read where it was shown or sampled */
	d = dep(d, VK_SUBPASS_EXTERNAL, 0, fbStages, fbStages, fbWrites | VK_ACCESS_SHADER_READ_BIT, fbAll, 0);
	d = dep(d, VK_SUBPASS_EXTERNAL, 1, fbStages, fbStages, fbWrites | VK_ACCESS_SHADER_READ_BIT, fbAll, 0);
	/* the samples drawn, then resolved */
	d = dep(d, 0, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT);
	/* the picture, to be shown or sampled */
	d = dep(d, 1, VK_SUBPASS_EXTERNAL, fbStages, fbStages | VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fbAll | VK_ACCESS_TRANSFER_READ_BIT, 0);
	if (hdr)
	{
		/* the HDR output: the picture resolved, then encoded where it is (with the samples)... */
		d = dep(d, 1, 2, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT);
		d = dep(d, 0, 2, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT);
		/* ...into the image the frontend was done with... */
		d = dep(d, VK_SUBPASS_EXTERNAL, 2, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
		/* ...and which it is handed to read */
		d = dep(d, 2, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0);
	}

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = hdr ? 4 : 3;
	info.pAttachments = attachments;
	info.subpassCount = hdr ? 3 : 2;
	info.pSubpasses = subpasses;
	info.dependencyCount = (u32)(d - deps);
	info.pDependencies = deps;
	if (vkCreateRenderPass(vkc.device, &info, NULL, &renderPass) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return renderPass;
}
