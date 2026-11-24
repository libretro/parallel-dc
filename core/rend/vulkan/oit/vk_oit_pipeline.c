/*
    Created on: Nov 6, 2019

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
/* See vk_oit_pipeline.h */
#include <stdlib.h>
#include <string.h>
#include <retro_miscellaneous.h>
#include "vk_oit_pipeline.h"
#include "../vk_texture.h"
#include "../vk_quad.h"
#include "hw/pvr/ta_words.h"
#include "rend/texconv.h"
#include "rend/ssaa_rate.h"

#define PPW(w, f) TAW_GET(w, f)

/* (the shaders' push constants: shaders/oit.frag) */
typedef char vk_oit_push_check[sizeof(vk_oit_push_constants) == OIT_PUSH_KEY_OFFSET + 4 ? 1 : -1];

/* ---- the shaders ---- */

void vk_oit_shaders_construct(vk_oit_shaders *sh)
{
	memset(sh, 0, sizeof(*sh));
	sh->samples = 1;
}

bool vk_oit_shaders_init(vk_oit_shaders *sh)
{
	if (!vk_raster_modules_make(&sh->modules, RSPV_MODVOL_VERT, RSPV_MODVOL_VERT)
			|| !vk_raster_modules_make(&sh->modules, RSPV_OIT_VERT_G0, RSPV_OIT_FRAG_G1_P2_R2))
		return false;
	/* (SSAA's lists taking the samples the depth test passed: only where the device has that) */
	sh->postDepthCoverage = vkc.postDepthCoverage;
	if (sh->postDepthCoverage && !vk_raster_modules_make(&sh->modules, RSPV_OIT_FRAG_G0_P2_R2_PDC, RSPV_OIT_FRAG_G1_P2_R2_PDC))
		return false;
	return vk_raster_modules_make(&sh->modules, RSPV_OIT_MODVOL_FRAG, RSPV_OIT_FINAL_VERT)
		&& vk_raster_modules_make(&sh->modules, RSPV_MV_VERT, RSPV_MV_DEPTH_FRAG);
}

void vk_oit_shaders_term(vk_oit_shaders *sh)
{
	vk_raster_modules_term(&sh->modules);
}

VkShaderModule vk_oit_shaders_fragment(const vk_oit_shaders *sh, bool gouraud, int pass, int sampleRate)
{
	/* G0_P0_R0 ... G0_P2_R2, without the lists' at each sample: eight a gouraud */
	static const int byPassRate[3][3] = { { 0, 1, 2 }, { 3, 4, 5 }, { 6, -1, 7 } };
	if (pass == VK_OIT_PASS_OIT && sampleRate == 2 && sh->postDepthCoverage)
		return sh->modules.m[gouraud ? RSPV_OIT_FRAG_G1_P2_R2_PDC : RSPV_OIT_FRAG_G0_P2_R2_PDC];
	return sh->modules.m[RSPV_OIT_FRAG_G0_P0_R0 + (gouraud ? 8 : 0) + byPassRate[pass][sampleRate]];
}

VkShaderModule vk_oit_shaders_trmodvol(const vk_oit_shaders *sh, int mode)
{
	return sh->modules.m[RSPV_OIT_TRMODVOL_FRAG_M0_S0 + 2 * mode + (sh->samples > 1 ? 1 : 0)];
}

VkShaderModule vk_oit_shaders_final(const vk_oit_shaders *sh, bool perSample, bool sums)
{
	return sh->modules.m[sh->samples <= 1 ? RSPV_OIT_FINAL_FRAG_S0 : perSample ? RSPV_OIT_FINAL_FRAG_S2
			: sums ? RSPV_OIT_FINAL_FRAG_S1_SUMS : RSPV_OIT_FINAL_FRAG_S1];
}

/* ---- the descriptor sets ---- */

void vk_oit_desc_sets_construct(vk_oit_desc_sets *d)
{
	memset(d, 0, sizeof(*d));
	vk_desc_cache_construct(&d->perPoly);
}

void vk_oit_desc_sets_init(vk_oit_desc_sets *d, vk_samplers *samplers, VkPipelineLayout pipelineLayout,
		VkDescriptorSetLayout perFrameLayout, VkDescriptorSetLayout perPolyLayout, VkDescriptorSetLayout colorInputLayout)
{
	unsigned i;
	d->samplers = samplers;
	d->pipelineLayout = pipelineLayout;
	d->perFrameLayout = perFrameLayout;
	d->perPolyLayout = perPolyLayout;
	vk_desc_cache_set_layout(&d->perPoly, perPolyLayout);
	d->colorInputLayout = colorInputLayout;
	if (d->perFrameDescSet == VK_NULL_HANDLE)
		d->perFrameDescSet = vkc_alloc_desc_set(perFrameLayout);
	if (d->noTextureDescSet == VK_NULL_HANDLE)
		d->noTextureDescSet = vkc_alloc_desc_set(perPolyLayout);
	for (i = 0; i < 2; i++)
		if (d->colorInputDescSets[i] == VK_NULL_HANDLE)
			d->colorInputDescSets[i] = vkc_alloc_desc_set(colorInputLayout);
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

void vk_oit_desc_sets_update_uniforms(vk_oit_desc_sets *d, VkBuffer vertexUniformBuffer, VkBuffer buffer, u32 fragmentUniformOffset,
		VkImageView fogImageView, u32 polyParamsOffset, u32 polyParamsSize, VkImageView stencilImageView, VkImageView depthImageView,
		VkImageView paletteImageView, VkImageLayout depthInputLayout, VkBuffer gpuPolyParams, VkDeviceSize gpuPolyParamsSize)
{
	VkDescriptorBufferInfo bufferInfos[2], polyParamsInfo;
	VkDescriptorImageInfo fogInfo, paletteInfo, stencilInfo, depthInfo;
	/* (on the stack: nothing allocated for them) */
	VkWriteDescriptorSet writes[9];
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
		/* What an untextured polygon has as its textures: the general
		 * pipelines read them only for a textured polygon, but have them
		 * in their shader (pso.h) */
		if (d->noTextureDescSet == VK_NULL_HANDLE)
			d->noTextureDescSet = vkc_alloc_desc_set(d->perPolyLayout);
		if (d->noTextureView != fogImageView)
		{
			d->noTextureView = fogImageView;
			write_set(&writes[n++], d->noTextureDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fogInfo, NULL);
			write_set(&writes[n++], d->noTextureDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fogInfo, NULL);
		}
	}
	if (paletteImageView != VK_NULL_HANDLE)
	{
		paletteInfo.sampler = entry_sampler(d->samplers);
		paletteInfo.imageView = paletteImageView;
		paletteInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		write_set(&writes[n++], d->perFrameDescSet, 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &paletteInfo, NULL);
	}
	if (polyParamsSize > 0)
	{
		/* (of a frame decoded on the GPU, the translucent polygon parameters are the GPU's) */
		polyParamsInfo.buffer = gpuPolyParams != VK_NULL_HANDLE ? gpuPolyParams : buffer;
		polyParamsInfo.offset = gpuPolyParams != VK_NULL_HANDLE ? 0 : polyParamsOffset;
		polyParamsInfo.range = gpuPolyParams != VK_NULL_HANDLE ? gpuPolyParamsSize : polyParamsSize;
		write_set(&writes[n++], d->perFrameDescSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &polyParamsInfo);
	}
	stencilInfo.sampler = VK_NULL_HANDLE;
	stencilInfo.imageView = stencilImageView;
	stencilInfo.imageLayout = depthInputLayout;
	write_set(&writes[n++], d->perFrameDescSet, 4, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &stencilInfo, NULL);
	depthInfo.sampler = VK_NULL_HANDLE;
	depthInfo.imageView = depthImageView;
	depthInfo.imageLayout = depthInputLayout;
	write_set(&writes[n++], d->perFrameDescSet, 5, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &depthInfo, NULL);
	vkUpdateDescriptorSets(vkc.device, n, writes, 0, NULL);
}

void vk_oit_desc_sets_update_color_input(vk_oit_desc_sets *d, int index, VkImageView colorImageView, VkImageLayout layout)
{
	VkDescriptorImageInfo info;
	VkWriteDescriptorSet write;
	if (d->colorInputDescSets[index] == VK_NULL_HANDLE)
		d->colorInputDescSets[index] = vkc_alloc_desc_set(d->colorInputLayout);
	info.sampler = VK_NULL_HANDLE;
	info.imageView = colorImageView;
	info.imageLayout = layout;
	write_set(&write, d->colorInputDescSets[index], 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &info, NULL);
	vkUpdateDescriptorSets(vkc.device, 1, &write, 0, NULL);
}

VkDescriptorSet vk_oit_desc_sets_set_texture(vk_oit_desc_sets *d, u64 textureId0, u32 tsp0, u64 textureId1, u32 tsp1)
{
	const vk_texture *texture = (const vk_texture *)(uintptr_t)textureId0;
	const vk_texture *texture1 = textureId1 != (u64)-1 ? (const vk_texture *)(uintptr_t)textureId1 : NULL;
	VkDescriptorImageInfo images[2];
	VkWriteDescriptorSet writes[2];
	TexDescKey key;
	VkDescriptorSet set;

	key.view0 = vk_texture_read_only_serial(texture);
	key.view1 = texture1 != NULL ? vk_texture_read_only_serial(texture1) : 0;
	key.sampler0 = tsp0 & VK_SAMPLER_TSP_MASK;
	key.sampler1 = texture1 != NULL ? tsp1 & VK_SAMPLER_TSP_MASK : 0;
	set = vk_desc_cache_find(&d->perPoly, &key);
	if (set != VK_NULL_HANDLE)
		return set;
	set = vk_desc_cache_add(&d->perPoly, &key);
	images[0].sampler = vk_samplers_get(d->samplers, tsp0);
	images[0].imageView = vk_texture_read_only_view(texture);
	images[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	/* (a polygon with one volume has the first texture as its second: the general pipelines' shader has both) */
	images[1] = images[0];
	if (texture1 != NULL)
	{
		images[1].sampler = vk_samplers_get(d->samplers, tsp1);
		images[1].imageView = vk_texture_read_only_view(texture1);
	}
	write_set(&writes[0], set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[0], NULL);
	write_set(&writes[1], set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[1], NULL);
	vkUpdateDescriptorSets(vkc.device, 2, writes, 0, NULL);
	return set;
}

void vk_oit_desc_sets_bind_per_frame(const vk_oit_desc_sets *d, VkCommandBuffer cmd)
{
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipelineLayout, 0, 1, &d->perFrameDescSet, 0, NULL);
	/* (a texture's replaces it; until one does, an untextured polygon has this) */
	if (d->noTextureDescSet != VK_NULL_HANDLE)
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipelineLayout, 1, 1, &d->noTextureDescSet, 0, NULL);
}

void vk_oit_desc_sets_bind_color_input(const vk_oit_desc_sets *d, VkCommandBuffer cmd, int index)
{
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipelineLayout, 2, 1, &d->colorInputDescSets[index], 0, NULL);
}

void vk_oit_desc_sets_bind_per_poly(const vk_oit_desc_sets *d, VkCommandBuffer cmd, VkDescriptorSet set)
{
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipelineLayout, 1, 1, &set, 0, NULL);
}

void vk_oit_desc_sets_reset(vk_oit_desc_sets *d)
{
	vk_desc_cache_next_use(&d->perPoly);
}

void vk_oit_desc_sets_term(vk_oit_desc_sets *d)
{
	unsigned i;
	vk_desc_cache_term(&d->perPoly);
	for (i = 0; i < 2; i++)
	{
		vkc_free_desc_set(d->colorInputDescSets[i]);
		d->colorInputDescSets[i] = VK_NULL_HANDLE;
	}
	vkc_free_desc_set(d->noTextureDescSet);
	vkc_free_desc_set(d->perFrameDescSet);
	d->noTextureDescSet = VK_NULL_HANDLE;
	d->perFrameDescSet = VK_NULL_HANDLE;
	d->noTextureView = VK_NULL_HANDLE;
}

/* ---- the pipelines ---- */

/* a key's stencil state: none, or the shadow bit set or cleared */
static void stencil_state(VkStencilOpState *s, u32 stencil)
{
	if (stencil == 1)
		vk_pipe_stencil_op(s, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0, 0x80, 0x80);
	else if (stencil == 2)
		vk_pipe_stencil_op(s, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0, 0x80, 0);
	else
		vk_pipe_stencil_op(s, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_NEVER, 0, 0, 0);
}

#define MODULE_BITS (((u64)1 << VK_OIT_KEY_GOURAUD) | ((u64)3 << VK_OIT_KEY_RATE) | ((u64)3 << VK_OIT_KEY_PASS) \
		| ((u64)3 << VK_OIT_KEY_SUBPASS))
#define DEPTH_STENCIL_BITS (((u64)3 << VK_OIT_KEY_CULL) | ((u64)1 << VK_OIT_KEY_ZWRITE) | ((u64)7 << VK_OIT_KEY_DEPTH_OP) \
		| ((u64)3 << VK_OIT_KEY_STENCIL))
#define BLEND_BITS (((u64)1 << VK_OIT_KEY_BLEND) | ((u64)1 << VK_OIT_KEY_COLOR_WRITE) | ((u64)7 << VK_OIT_KEY_SRC) \
		| ((u64)7 << VK_OIT_KEY_DST))

static u64 key_of(const vk_oit_pipeline_mgr *m, u32 listType, bool autosort, const PolyParam *pp, int pass)
{
	const bool texture = PPW(pp->pcw, PCW_Texture) != 0;
	const u32 pixelFmt = PPW(pp->tcw, TCW_PixelFmt);
	const bool twoVolume = pp->tsp1 != (u32)-1 || pp->tcw1 != (u32)-1;
	const bool shadowed = pass == VK_OIT_PASS_DEPTH && (listType == ListType_Opaque || listType == ListType_Punch_Through);
	bool colourPassWrites, depthWriteEnable;
	u32 depthOp;
	u64 key;

	/* the shader. By the polygon alone. The clamping limits were looked at
	 * too, to leave the clamp out of the shader while they clamp nothing -
	 * but a pipeline is made once and kept under a key that has only the
	 * polygon's bit in it, and the limits change: a polygon first drawn
	 * while they clamped nothing was never clamped after. (Upstream: Red Dog.) */
	key = ((u64)(listType == ListType_Punch_Through) << OIT_SC_ALPHA_TEST)
		| ((u64)((pp->tileclip >> 28) == 3) << OIT_SC_CLIP_INSIDE)
		| ((u64)PPW(pp->tsp, TSP_UseAlpha) << OIT_SC_USE_ALPHA)
		| ((u64)texture << OIT_SC_TEXTURE)
		| ((u64)(PPW(pp->tsp, TSP_IgnoreTexA) || pixelFmt == TA_PIXEL_565) << OIT_SC_IGNORE_TEX_A)
		| ((u64)PPW(pp->tsp, TSP_ShadInstr) << OIT_SC_SHAD_INSTR)
		| ((u64)PPW(pp->pcw, PCW_Offset) << OIT_SC_OFFSET)
		| ((u64)PPW(pp->tsp, TSP_FogCtrl) << OIT_SC_FOG_CTRL)
		| ((u64)twoVolume << OIT_SC_TWO_VOLUMES)
		| ((u64)(pixelFmt == TA_PIXEL_BUMPMAP) << OIT_SC_BUMP_MAP)
		| ((u64)PPW(pp->tsp, TSP_ColorClamp) << OIT_SC_CLAMPING)
		| ((u64)tex_gpu_paletted(pp->tsp, pp->tcw, 0) << OIT_SC_PALETTE)
		| ((u64)PPW(pp->pcw, PCW_Gouraud) << VK_OIT_KEY_GOURAUD)
		| ((u64)pass << VK_OIT_KEY_PASS);
	/* SSAA: shaded at each sample, but for what is drawn at a single
	 * rate; and a translucent fragment put in the lists once a pixel,
	 * with the samples it covers, so the lists are no longer for it */
	if (m->passes.samples > 1)
		key |= (u64)(pass != VK_OIT_PASS_OIT && !ssaa_single_rate(pp) ? 1 : 2) << VK_OIT_KEY_RATE;
	key |= (u64)(pass == VK_OIT_PASS_DEPTH ? (listType == ListType_Translucent ? 2 : 0) : 1) << VK_OIT_KEY_SUBPASS;

	/* the fixed functions (the ISP's depth modes are VkCompareOp's, in order) */
	key |= (u64)(PPW(pp->isp, ISP_TSP_CullMode) == 3 ? 2 : PPW(pp->isp, ISP_TSP_CullMode) == 2 ? 1 : 0) << VK_OIT_KEY_CULL;
	if (pass == VK_OIT_PASS_COLOR && !PPW(pp->isp, ISP_TSP_ZWriteDis) && listType != ListType_Translucent)
		depthOp = VK_COMPARE_OP_EQUAL;
	else if (listType == ListType_Punch_Through || autosort)
		depthOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
	else
		depthOp = PPW(pp->isp, ISP_TSP_DepthMode);
	/* Depth is written in the depth pass. In the colour pass the depth buffer
	 * is read-only - it is an input to the shaders there - and depth writes
	 * are not allowed. For opaque and punch-through polygons there is nothing
	 * to write anyway: they are drawn where their depth is the one the depth
	 * pass left, so a write would put the same value back.
	 *
	 * Translucent polygons that are not sorted are the exception. They are
	 * drawn in the colour pass only, in order, each tested against the depth
	 * the ones before it wrote, so they still write. So does everything on
	 * Intel, which is the driver bug workaround this condition came with. */
	colourPassWrites = pass == VK_OIT_PASS_COLOR
			&& (vkc.vendorID == VENDOR_INTEL || (!autosort && listType == ListType_Translucent));
	if (pass != VK_OIT_PASS_DEPTH && !colourPassWrites)
		depthWriteEnable = false;
	/* Z Write Disable seems to be ignored for punch-through.
	 * Fixes Worms World Party, Bust-a-Move 4 and Re-Volt */
	else if (listType == ListType_Punch_Through)
		depthWriteEnable = true;
	else
		depthWriteEnable = !PPW(pp->isp, ISP_TSP_ZWriteDis);
	key |= ((u64)depthWriteEnable << VK_OIT_KEY_ZWRITE) | ((u64)depthOp << VK_OIT_KEY_DEPTH_OP)
			| ((u64)(shadowed ? (PPW(pp->pcw, PCW_Shadow) != 0 ? 1 : 2) : 0) << VK_OIT_KEY_STENCIL);
	/* Apparently punch-through polys support blending, or at least some combinations */
	if (listType == ListType_Punch_Through || pass == VK_OIT_PASS_COLOR)
		key |= ((u64)1 << VK_OIT_KEY_BLEND) | ((u64)1 << VK_OIT_KEY_COLOR_WRITE)
			| ((u64)PPW(pp->tsp, TSP_SrcInstr) << VK_OIT_KEY_SRC) | ((u64)PPW(pp->tsp, TSP_DstInstr) << VK_OIT_KEY_DST);
	return key;
}

static u64 general_key(u64 key)
{
	u64 fixed = MODULE_BITS;
	if (!vkc.dynamicState.depthStencil)
		fixed |= DEPTH_STENCIL_BITS;
	if (!vkc.dynamicState.blend)
		fixed |= BLEND_BITS;
	return (key & fixed) | ((u64)VK_OIT_KIND_GENERAL << VK_OIT_KEY_KIND);
}

PsoChoice vk_oit_pipeline_mgr_get(vk_oit_pipeline_mgr *m, u32 listType, bool autosort, const PolyParam *pp, int pass)
{
	const u64 key = key_of(m, listType, autosort, pp, pass);
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

void vk_oit_pipeline_mgr_set_dynamic_state(const vk_oit_pipeline_mgr *m, VkCommandBuffer cmd, u64 key)
{
	const vk_pso_dyn *f = &vkc.dynamicState;
	(void)m;
	if (f->depthStencil)
	{
		const u32 stencil = (u32)(key >> VK_OIT_KEY_STENCIL) & 3;
		VkStencilOpState state;
		stencil_state(&state, stencil);
		f->CmdSetCullMode(cmd, vk_pipe_cull_mode((u32)(key >> VK_OIT_KEY_CULL) & 3));
		f->CmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
		f->CmdSetDepthTestEnable(cmd, VK_TRUE);
		f->CmdSetDepthWriteEnable(cmd, (VkBool32)((key >> VK_OIT_KEY_ZWRITE) & 1));
		f->CmdSetDepthCompareOp(cmd, (VkCompareOp)((key >> VK_OIT_KEY_DEPTH_OP) & 7));
		f->CmdSetStencilTestEnable(cmd, stencil != 0);
		f->CmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.failOp, state.passOp, state.depthFailOp, state.compareOp);
		vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.compareMask);
		vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.writeMask);
		vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.reference);
	}
	if (f->blend)
	{
		const VkBool32 enable = (VkBool32)((key >> VK_OIT_KEY_BLEND) & 1);
		const VkColorComponentFlags writeMask = ((key >> VK_OIT_KEY_COLOR_WRITE) & 1)
				? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT : 0;
		vk_pso_blend_equation equation;
		memset(&equation, 0, sizeof(equation));
		f->CmdSetColorBlendEnable(cmd, 0, 1, &enable);
		equation.colorBlendOp = VK_BLEND_OP_ADD;
		equation.alphaBlendOp = VK_BLEND_OP_ADD;
		if (enable)
		{
			equation.srcColorBlendFactor = equation.srcAlphaBlendFactor = vk_pipe_blend_factor((u32)(key >> VK_OIT_KEY_SRC) & 7, true);
			equation.dstColorBlendFactor = equation.dstAlphaBlendFactor = vk_pipe_blend_factor((u32)(key >> VK_OIT_KEY_DST) & 7, false);
		}
		f->CmdSetColorBlendEquation(cmd, 0, 1, &equation);
		f->CmdSetColorWriteMask(cmd, 0, 1, &writeMask);
	}
}

void vk_oit_pipeline_mgr_reset(vk_oit_pipeline_mgr *m)
{
	unsigned i;
	/* (nothing of it is to be made any more; what is being made, finished) */
	vk_pso_builder_new_generation(&m->builder);
	vk_pso_worker_end_batch(&m->builder);
	vk_pso_worker_wait_idle();
	if (vkc.device != VK_NULL_HANDLE)
	{
		vk_pso_table_clear(&m->pipelines, vkc.device);
		vk_pso_table_clear(&m->general, vkc.device);
		for (i = 0; i < VK_OIT_FIXED_COUNT; i++)
		{
			if (m->fixedSlots[i].pipeline != VK_NULL_HANDLE)
				vkDestroyPipeline(vkc.device, m->fixedSlots[i].pipeline, NULL);
			m->fixedSlots[i].pipeline = VK_NULL_HANDLE;
			m->fixedSlots[i].owner = NULL;
		}
	}
	m->lastKey = ~(u64)0;
	m->prepared = false;
}

/* ---- making them ---- */

static const VkVertexInputBindingDescription main_bindings[] = { { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX } };
static const VkVertexInputAttributeDescription main_attributes[] =
{
	{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, x) },		/* pos */
	{ 1, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vertex, col) },		/* base color */
	{ 2, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vertex, vtx_spc) },	/* offset color */
	{ 3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u) },			/* tex coord */
	{ 4, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vertex, col1) },		/* base1 color */
	{ 5, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vertex, spc1) },		/* offset1 color */
	{ 6, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u1) },		/* tex1 coord */
};
static const VkVertexInputBindingDescription modvol_bindings[] = { { 0, sizeof(float) * 3, VK_VERTEX_INPUT_RATE_VERTEX } };
static const VkVertexInputAttributeDescription modvol_attributes[] = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 } };	/* pos */

static void modvol_input(vk_pipe_states *s)
{
	s->vertexInput.vertexBindingDescriptionCount = 1;
	s->vertexInput.pVertexBindingDescriptions = modvol_bindings;
	s->vertexInput.vertexAttributeDescriptionCount = 1;
	s->vertexInput.pVertexAttributeDescriptions = modvol_attributes;
}

/* A polygon's pipeline, all of it from @key; @general: the one that draws
 * for the keys that differ from it only in what the draw sets (pso.h) */
static VkPipeline build_poly(const vk_oit_pipeline_mgr *m, u64 key, bool general, VkPipelineCache cache)
{
	const u32 rate = (u32)(key >> VK_OIT_KEY_RATE) & 3;
	const u32 stencil = (u32)(key >> VK_OIT_KEY_STENCIL) & 3;
	const bool gouraud = ((key >> VK_OIT_KEY_GOURAUD) & 1) != 0;
	vk_pipe_states s;

	vk_pipe_states_init(&s, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, (u32)(key >> VK_OIT_KEY_CULL) & 3, m->passes.samples, rate == 1);
	s.vertexInput.vertexBindingDescriptionCount = ARRAY_SIZE(main_bindings);
	s.vertexInput.pVertexBindingDescriptions = main_bindings;
	s.vertexInput.vertexAttributeDescriptionCount = ARRAY_SIZE(main_attributes);
	s.vertexInput.pVertexAttributeDescriptions = main_attributes;

	/* depth and stencil */
	stencil_state(&s.depthStencil.front, stencil);
	s.depthStencil.back = s.depthStencil.front;
	s.depthStencil.depthTestEnable = VK_TRUE;
	s.depthStencil.depthWriteEnable = (VkBool32)((key >> VK_OIT_KEY_ZWRITE) & 1);
	s.depthStencil.depthCompareOp = (VkCompareOp)((key >> VK_OIT_KEY_DEPTH_OP) & 7);
	s.depthStencil.stencilTestEnable = stencil != 0;

	/* colour and blending (none written without) */
	s.blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	s.blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	if ((key >> VK_OIT_KEY_BLEND) & 1)
	{
		const u32 src = (u32)(key >> VK_OIT_KEY_SRC) & 7, dst = (u32)(key >> VK_OIT_KEY_DST) & 7;
		s.blendAttachment.blendEnable = VK_TRUE;
		s.blendAttachment.srcColorBlendFactor = s.blendAttachment.srcAlphaBlendFactor = vk_pipe_blend_factor(src, true);
		s.blendAttachment.dstColorBlendFactor = s.blendAttachment.dstAlphaBlendFactor = vk_pipe_blend_factor(dst, false);
		s.blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
				| VK_COLOR_COMPONENT_A_BIT;
	}

	/* the general pipeline's states that are set with each draw (vk_oit_pipeline_mgr_set_dynamic_state()) */
	if (general)
		vk_pipe_states_general(&s);

	/* the module of the interpolation, pass and sample rate: the rest is the key's */
	vk_pipe_states_stages(&s, m->shaders->modules.m[gouraud ? RSPV_OIT_VERT_G1 : RSPV_OIT_VERT_G0],
			vk_oit_shaders_fragment(m->shaders, gouraud, (int)((key >> VK_OIT_KEY_PASS) & 3), (int)rate),
			true, general ? -1 : (s32)(key & ((1u << OIT_SC_BITS) - 1)), m->passes.samples);
	return vk_pipe_states_make(&s, m->pipelineLayout, m->renderPass, (u32)((key >> VK_OIT_KEY_SUBPASS) & 3), cache);
}

/* the last subpass's: @perSample, SSAA's for a render pass that is not the last */
static VkPipeline build_final(const vk_oit_pipeline_mgr *m, bool perSample, bool sums, VkPipelineCache cache)
{
	vk_pipe_states s;
	/* (SSAA: the last subpass is of single samples; the one before, which puts each sample together, of all of them) */
	vk_pipe_states_init(&s, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0, perSample ? m->passes.samples : 1, perSample);
	vk_quad_input_state(&s.vertexInput, false);
	s.blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	s.blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	s.blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
			| VK_COLOR_COMPONENT_A_BIT;
	vk_pipe_states_stages(&s, m->shaders->modules.m[RSPV_OIT_FINAL_VERT], vk_oit_shaders_final(m->shaders, perSample, sums),
			true, 0, m->passes.samples);
	return vk_pipe_states_make(&s, m->pipelineLayout, m->renderPass, perSample ? 2 : (u32)vk_oit_final_subpass(&m->passes), cache);
}

/* the lists' clear: no colour written */
static VkPipeline build_clear(const vk_oit_pipeline_mgr *m, VkPipelineCache cache)
{
	vk_pipe_states s;
	vk_pipe_states_init(&s, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0, 1, false);
	vk_quad_input_state(&s.vertexInput, false);
	s.multisample.minSampleShading = 0.f;
	vk_pipe_states_stages(&s, m->shaders->modules.m[RSPV_OIT_FINAL_VERT], m->shaders->modules.m[RSPV_OIT_CLEAR_FRAG], false, 0, 0);
	return vk_pipe_states_make(&s, m->pipelineLayout, m->renderPass, (u32)vk_oit_final_subpass(&m->passes), cache);
}

/* the opaque modifier volumes' (subpass 0), or the translucent ones' (@tr,
 * subpass 2); @cull: PsoCullIndex()'s (none where it is dynamic) */
static VkPipeline build_modvol(const vk_oit_pipeline_mgr *m, bool tr, int mode, u32 cull, VkPipelineCache cache)
{
	vk_pipe_states s;
	vk_pipe_states_init(&s, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, cull, m->passes.samples, false);
	modvol_input(&s);
	if (!tr)
	{
		VkStencilOpState stencil;
		memset(&stencil, 0, sizeof(stencil));
		switch (mode)
		{
		case VK_MODVOL_XOR:
			vk_pipe_stencil_op(&stencil, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0, 2, 2);
			break;
		case VK_MODVOL_OR:
			vk_pipe_stencil_op(&stencil, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 2, 2, 2);
			break;
		case VK_MODVOL_INCLUSION:
			vk_pipe_stencil_op(&stencil, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_ZERO, VK_COMPARE_OP_LESS_OR_EQUAL, 3, 3, 1);
			break;
		case VK_MODVOL_EXCLUSION:
			vk_pipe_stencil_op(&stencil, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_COMPARE_OP_EQUAL, 3, 3, 1);
			break;
		default:
			break;
		}
		s.depthStencil.depthTestEnable = mode == VK_MODVOL_XOR || mode == VK_MODVOL_OR;
		s.depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER;
		s.depthStencil.stencilTestEnable = VK_TRUE;
		s.depthStencil.front = stencil;
		s.depthStencil.back = stencil;
	}
	/* (the cull mode set with the draws, where it can be: vk_pso_cull) */
	if (m->cullSetter != NULL)
		vk_pipe_states_dynamic(&s, (VkDynamicState)VK_PSO_DYNAMIC_STATE_CULL_MODE);
	vk_pipe_states_stages(&s, m->shaders->modules.m[RSPV_MODVOL_VERT],
			tr ? vk_oit_shaders_trmodvol(m->shaders, mode) : m->shaders->modules.m[RSPV_OIT_MODVOL_FRAG], false, 0, 0);
	return vk_pipe_states_make(&s, m->pipelineLayout, m->renderPass, tr ? 2 : 0, cache);
}

/* the pipeline thread's, and a draw's */
static VkPipeline build(vk_pso_builder *self, u64 key, VkPipelineCache cache)
{
	const vk_oit_pipeline_mgr *m = (const vk_oit_pipeline_mgr *)self;
	switch ((key >> VK_OIT_KEY_KIND) & 15)
	{
	case VK_OIT_KIND_POLY:
		return build_poly(m, key, false, cache);
	case VK_OIT_KIND_GENERAL:
		return build_poly(m, key, true, cache);
	case VK_OIT_KIND_FIXED:
		{
			const u32 i = (u32)(key & 0xff);
			/* (the modifier volumes': Xor, Or, Inclusion, Exclusion, by cull mode none, front, back) */
			if (i < VK_OIT_FIXED_TRMODVOL)
				return build_modvol(m, false, (int)(i / 3), i % 3, cache);
			if (i < VK_OIT_FIXED_FINAL)
				return build_modvol(m, true, (int)((i - VK_OIT_FIXED_TRMODVOL) / 3), (i - VK_OIT_FIXED_TRMODVOL) % 3, cache);
			if (i == VK_OIT_FIXED_FINAL || i == VK_OIT_FIXED_FINAL_PER_SAMPLE || i == VK_OIT_FIXED_FINAL_SUMS)
				return build_final(m, i == VK_OIT_FIXED_FINAL_PER_SAMPLE, i == VK_OIT_FIXED_FINAL_SUMS, cache);
			if (i == VK_OIT_FIXED_CLEAR)
				return build_clear(m, cache);
			return VK_NULL_HANDLE;
		}
	default:
		return VK_NULL_HANDLE;
	}
}

void vk_oit_pipeline_mgr_construct(vk_oit_pipeline_mgr *m, bool rtt)
{
	memset(m, 0, sizeof(*m));
	m->builder.build = build;
	retro_atomic_int_init(&m->builder.generation, 0);
	m->cullKept = 3;
	m->lastKey = ~(u64)0;
	m->who = rtt ? 4 : 3;
	vk_oit_passes_construct(&m->passes, rtt);
}

/* prepare()'s list of slots to make now */
typedef struct slot_list
{
	PsoSlot **slots;
	u32 count, cap;
} slot_list;

static void slot_add(slot_list *now, PsoSlot *slot)
{
	if (now->count < now->cap)
		now->slots[now->count++] = slot;
}

static void add_general(vk_oit_pipeline_mgr *m, slot_list *now, u64 key)
{
	const u64 generalKey = general_key(key);
	if (vk_pso_table_find(&m->general, generalKey) == NULL)
	{
		PsoSlot *slot = vk_pso_table_insert(&m->general, generalKey);
		if (slot != NULL)
			slot_add(now, slot);
	}
}

static bool key_fits(const vk_oit_pipeline_mgr *m, u64 key)
{
	const u32 rate = (u32)(key >> VK_OIT_KEY_RATE) & 3;
	return ((key >> VK_OIT_KEY_KIND) & 15) == VK_OIT_KIND_POLY && (m->passes.samples > 1 ? rate == 1 || rate == 2 : rate == 0);
}

static int compare_keys(const void *a, const void *b)
{
	const u64 x = *(const u64 *)a, y = *(const u64 *)b;
	return x < y ? -1 : x > y;
}

/* (each list in each pass the drawer draws it in) */
static const struct { u32 list; bool autosort; int pass; } reach_draws[] =
{
	{ ListType_Opaque, false, VK_OIT_PASS_DEPTH }, { ListType_Punch_Through, false, VK_OIT_PASS_DEPTH },
	{ ListType_Opaque, false, VK_OIT_PASS_COLOR }, { ListType_Punch_Through, false, VK_OIT_PASS_COLOR },
	{ ListType_Translucent, true, VK_OIT_PASS_OIT }, { ListType_Translucent, false, VK_OIT_PASS_COLOR },
	{ ListType_Translucent, false, VK_OIT_PASS_DEPTH }, { ListType_Translucent, true, VK_OIT_PASS_DEPTH },
};

/* A device that has a general pipeline for each fixed state: one for every
 * state a draw can reach, worked out by key_of() itself from the draws the
 * drawer makes and every value of the fields it reads. All made at the
 * start, before the first frame. Where they are too many (a device with
 * neither extension, the per-pixel renderer's passes: thousands, until the
 * blend is folded as Direct3D 12's is), those of earlier runs are, and one
 * not seen before is made when first drawn (vk_pso_worker_sync_built()
 * counts them). */
static void add_reachable(vk_oit_pipeline_mgr *m, slot_list *now)
{
	const u32 rates = m->passes.samples > 1 ? 2 : 1;
	const u64 firstRate = m->passes.samples > 1 ? 1 : 0;
	const u64 kept = DEPTH_STENCIL_BITS | BLEND_BITS | ((u64)3 << VK_OIT_KEY_PASS) | ((u64)3 << VK_OIT_KEY_SUBPASS);
	const size_t most = ARRAY_SIZE(reach_draws) * ((size_t)1 << 13) * 2 * rates;
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
			u64 fixed, gouraud, rate;
			memset(&pp, 0, sizeof(pp));
			pp.isp = TAW_SET(pp.isp, ISP_TSP_CullMode, bits & 3);
			pp.isp = TAW_SET(pp.isp, ISP_TSP_DepthMode, (bits >> 2) & 7);
			pp.isp = TAW_SET(pp.isp, ISP_TSP_ZWriteDis, (bits >> 5) & 1);
			pp.pcw = TAW_SET(pp.pcw, PCW_Shadow, (bits >> 6) & 1);
			pp.tsp = TAW_SET(pp.tsp, TSP_SrcInstr, (bits >> 7) & 7);
			pp.tsp = TAW_SET(pp.tsp, TSP_DstInstr, (bits >> 10) & 7);
			pp.tsp1 = (u32)-1;
			pp.tcw1 = (u32)-1;
			fixed = key_of(m, reach_draws[d].list, reach_draws[d].autosort, &pp, reach_draws[d].pass) & kept;
			for (gouraud = 0; gouraud < 2; gouraud++)
				for (rate = firstRate; rate < firstRate + rates; rate++)
					if (reach_draws[d].pass != VK_OIT_PASS_OIT || rate != 1)
						reach[n++] = general_key(fixed | (gouraud << VK_OIT_KEY_GOURAUD) | (rate << VK_OIT_KEY_RATE));
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

static void prepare(vk_oit_pipeline_mgr *m)
{
	const u32 samples = m->passes.samples;
	slot_list now;
	u64 gouraud, pass, subpass, rate;
	u32 i;

	/* (taken here: the render passes are made when first asked for, which
	 * is not something two threads may do at once) */
	m->renderPass = vk_oit_passes_get(&m->passes, true, true, false, false);
	/* (made with the renderer, not while a game runs) */
	now.cap = m->general.mask + 1 + VK_OIT_FIXED_COUNT;
	now.count = 0;
	now.slots = (PsoSlot **)malloc(now.cap * sizeof(PsoSlot *));
	if (now.slots == NULL)
		now.cap = 0;
	/* The fixed ones first: every frame draws with them */
	m->cullSetter = vkc.dynamicState.depthStencil ? vkc.dynamicState.CmdSetCullMode : NULL;
	m->cullKept = m->cullSetter != NULL ? 0 : 3;
	for (i = 0; i < VK_OIT_FIXED_COUNT; i++)
	{
		PsoSlot *slot = &m->fixedSlots[i];
		/* (with the cull mode dynamic, the modifier volumes' none ones draw them all) */
		if (i < VK_OIT_FIXED_FINAL && m->cullSetter != NULL && i % 3 != 0)
			continue;
		if (i == VK_OIT_FIXED_FINAL_PER_SAMPLE && samples <= 1)
			continue;
		if (i == VK_OIT_FIXED_FINAL_SUMS && !m->sums)
			continue;
		slot->key = ((u64)VK_OIT_KIND_FIXED << VK_OIT_KEY_KIND) | i;
		slot->owner = &m->builder;
		slot->pipeline = VK_NULL_HANDLE;
		slot_add(&now, slot);
	}
	/* The general pipelines: of each module and subpass, and of each fixed
	 * state the device must have made with them. The depth pass draws the
	 * opaque polygons in subpass 0, the translucent ones in 2; the colour
	 * pass and the lists are in 1. */
	for (gouraud = 0; gouraud < 2; gouraud++)
		for (pass = 0; pass < 3; pass++)
			for (subpass = 0; subpass < 3; subpass++)
			{
				if ((pass == VK_OIT_PASS_DEPTH) != (subpass != 1))
					continue;
				for (rate = samples > 1 ? 1 : 0; rate <= (samples > 1 ? 2u : 0u); rate++)
				{
					if (pass == VK_OIT_PASS_OIT && rate == 1)
						continue;
					add_general(m, &now, (gouraud << VK_OIT_KEY_GOURAUD) | (rate << VK_OIT_KEY_RATE)
							| (pass << VK_OIT_KEY_PASS) | (subpass << VK_OIT_KEY_SUBPASS));
				}
			}
	if (!vkc.dynamicState.depthStencil || !vkc.dynamicState.blend)
		add_reachable(m, &now);
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
	m->prepared = true;
}

static void binding(VkDescriptorSetLayoutBinding *b, u32 index, VkDescriptorType type, VkShaderStageFlags stages)
{
	memset(b, 0, sizeof(*b));
	b->binding = index;
	b->descriptorType = type;
	b->descriptorCount = 1;
	b->stageFlags = stages;
}

static bool make_layout(VkDescriptorSetLayout *layout, const VkDescriptorSetLayoutBinding *bindings, u32 count)
{
	VkDescriptorSetLayoutCreateInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = count;
	info.pBindings = bindings;
	return vkCreateDescriptorSetLayout(vkc.device, &info, NULL, layout) == VK_SUCCESS;
}

static bool make_layouts(vk_oit_pipeline_mgr *m, VkDescriptorSetLayout buffersLayout)
{
	VkDescriptorSetLayoutBinding perFrame[7], colorInput[1], perPoly[2];
	VkDescriptorSetLayout layouts[4];
	VkPushConstantRange pushConstant;
	VkPipelineLayoutCreateInfo layoutInfo;

	binding(&perFrame[0], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);			/* vertex uniforms */
	binding(&perFrame[1], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);			/* fragment uniforms */
	binding(&perFrame[2], 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);	/* fog texture */
	binding(&perFrame[3], 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);			/* Tr poly params */
	binding(&perFrame[4], 4, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT);		/* stencil input attachment */
	binding(&perFrame[5], 5, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT);		/* depth input attachment */
	binding(&perFrame[6], 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);	/* palette texture */
	binding(&colorInput[0], 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT);		/* color input attachment */
	binding(&perPoly[0], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);	/* texture 0 */
	binding(&perPoly[1], 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);	/* texture 1 (two volumes) */
	if (!make_layout(&m->perFrameLayout, perFrame, ARRAY_SIZE(perFrame))
			|| !make_layout(&m->colorInputLayout, colorInput, ARRAY_SIZE(colorInput))
			|| !make_layout(&m->perPolyLayout, perPoly, ARRAY_SIZE(perPoly)))
		return false;

	pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstant.offset = 0;
	pushConstant.size = sizeof(vk_oit_push_constants);
	layouts[0] = m->perFrameLayout;
	layouts[1] = m->perPolyLayout;
	layouts[2] = m->colorInputLayout;
	layouts[3] = buffersLayout;
	memset(&layoutInfo, 0, sizeof(layoutInfo));
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 4;
	layoutInfo.pSetLayouts = layouts;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pushConstant;
	if (vkCreatePipelineLayout(vkc.device, &layoutInfo, NULL, &m->pipelineLayout) != VK_SUCCESS)
		return false;
	vk_pso_table_init(&m->pipelines, 4096, &m->builder);
	vk_pso_table_init(&m->general, 4096, &m->builder);
	return true;
}

bool vk_oit_pipeline_mgr_init(vk_oit_pipeline_mgr *m, vk_oit_shaders *shaders, VkDescriptorSetLayout buffersLayout, bool sums,
		u32 samples, bool keepDepth)
{
	m->shaders = shaders;
	if (!vk_oit_shaders_init(shaders))
		return false;
	if (samples != m->passes.samples || keepDepth != m->passes.keepDepth)
	{
		/* everything was made for the other render passes */
		vk_oit_pipeline_mgr_reset(m);
		vk_oit_passes_set_samples(&m->passes, samples);
		vk_oit_passes_set_keep_depth(&m->passes, keepDepth);
	}
	if ((samples > 1 && sums) != m->sums)
	{
		vk_oit_pipeline_mgr_reset(m);
		m->sums = samples > 1 && sums;
	}
	if (!vk_oit_passes_make_all(&m->passes))
		return false;
	if (m->perFrameLayout == VK_NULL_HANDLE && !make_layouts(m, buffersLayout))
		return false;
	/* (made once for the render passes: a new size keeps them) */
	if (!m->prepared)
		prepare(m);
	return true;
}

bool vk_oit_pipeline_mgr_rtt_init(vk_oit_pipeline_mgr *m, vk_oit_shaders *shaders, VkDescriptorSetLayout buffersLayout, bool sums)
{
	/* (what was made for the render passes goes before they do) */
	vk_oit_pipeline_mgr_reset(m);
	vk_oit_passes_reset(&m->passes);
	return vk_oit_pipeline_mgr_init(m, shaders, buffersLayout, sums, 1, false);
}

void vk_oit_pipeline_mgr_term(vk_oit_pipeline_mgr *m)
{
	vk_oit_pipeline_mgr_reset(m);
	vk_pso_table_free(&m->pipelines);
	vk_pso_table_free(&m->general);
	if (vkc.device != VK_NULL_HANDLE)
	{
		if (m->pipelineLayout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(vkc.device, m->pipelineLayout, NULL);
		if (m->perFrameLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(vkc.device, m->perFrameLayout, NULL);
		if (m->colorInputLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(vkc.device, m->colorInputLayout, NULL);
		if (m->perPolyLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(vkc.device, m->perPolyLayout, NULL);
		vk_oit_passes_reset(&m->passes);
	}
	m->pipelineLayout = VK_NULL_HANDLE;
	m->perFrameLayout = VK_NULL_HANDLE;
	m->colorInputLayout = VK_NULL_HANDLE;
	m->perPolyLayout = VK_NULL_HANDLE;
	m->renderPass = VK_NULL_HANDLE;
}
