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
/* The per-pixel renderer's shaders, descriptor sets and pipelines */
#ifndef VK_OIT_PIPELINE_H
#define VK_OIT_PIPELINE_H
#include "../vk_pipeline.h"
#include "vk_oit_renderpass.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the passes a polygon is drawn in */
enum { VK_OIT_PASS_DEPTH, VK_OIT_PASS_COLOR, VK_OIT_PASS_OIT };

/* ---- the shaders: SPIR-V of vulkan/shaders/ (raster_spv.inc), made by
 * vk_oit_shaders_init() before any pipeline is ---- */
typedef struct vk_oit_shaders
{
	vk_raster_modules modules;
	/* SSAA's samples a pixel (1: off): what the pipelines are specialized
	 * for, and which modules they take */
	u32 samples;
	bool postDepthCoverage;	/* the device's, as vk_oit_shaders_init() found it */
} vk_oit_shaders;

void vk_oit_shaders_construct(vk_oit_shaders *sh);
/* false if a module could not be made */
bool vk_oit_shaders_init(vk_oit_shaders *sh);
void vk_oit_shaders_term(vk_oit_shaders *sh);
/* the module of an interpolation, pass (VK_OIT_PASS_*) and sample rate */
VkShaderModule vk_oit_shaders_fragment(const vk_oit_shaders *sh, bool gouraud, int pass, int sampleRate);
/* VK_MODVOL_*'s, of the translucent modifier volumes */
VkShaderModule vk_oit_shaders_trmodvol(const vk_oit_shaders *sh, int mode);
/* @perSample: SSAA's, of a render pass that is not the last, which puts
 * each sample together; else the last one's, which resolves them too,
 * and with @sums stores their channel sums (OITBuffers::SetSums()) */
VkShaderModule vk_oit_shaders_final(const vk_oit_shaders *sh, bool perSample, bool sums);

/* ---- the descriptor sets a frame slot draws with ---- */

/* the shaders' push constants (shaders/oit.frag) */
typedef struct vk_oit_push_constants
{
	float clipTest[4];
	int blend_mode0[4];	/* Only using 2 elements but easier for std140 */
	float trilinearAlpha;
	int pp_Number;
	float palette_index;
	int _pad;

	/* two volume mode */
	int blend_mode1[4];	/* Only using 2 elements but easier for std140 */
	int shading_instr0;
	int shading_instr1;
	int fog_control0;
	int fog_control1;
	int use_alpha0;
	int use_alpha1;
	int ignore_tex_alpha0;
	int ignore_tex_alpha1;
	int key;	/* the general pipelines' (pso_key.h): at OIT_PUSH_KEY_OFFSET */
} vk_oit_push_constants;

typedef struct vk_oit_desc_sets
{
	vk_samplers *samplers;
	VkPipelineLayout pipelineLayout;
	VkDescriptorSetLayout perFrameLayout;
	VkDescriptorSetLayout perPolyLayout;
	VkDescriptorSetLayout colorInputLayout;
	VkDescriptorSet perFrameDescSet;
	VkDescriptorSet noTextureDescSet;
	VkImageView noTextureView;
	VkDescriptorSet colorInputDescSets[2];
	vk_desc_cache perPoly;
} vk_oit_desc_sets;

void vk_oit_desc_sets_construct(vk_oit_desc_sets *d);
/* (the sets made now, with the drawer: one that first draws while the game
 * runs - a render to a texture - makes none then) */
void vk_oit_desc_sets_init(vk_oit_desc_sets *d, vk_samplers *samplers, VkPipelineLayout pipelineLayout,
		VkDescriptorSetLayout perFrameLayout, VkDescriptorSetLayout perPolyLayout, VkDescriptorSetLayout colorInputLayout);
/* (the vertex uniforms are a buffer of their own: BaseDrawer::WriteVertexUniforms()).
 * Of a frame decoded on the GPU, the translucent polygon parameters are
 * the GPU's (@gpuPolyParams, not null: vk_ta_gpu_draw). */
void vk_oit_desc_sets_update_uniforms(vk_oit_desc_sets *d, VkBuffer vertexUniformBuffer, VkBuffer buffer, u32 fragmentUniformOffset,
		VkImageView fogImageView, u32 polyParamsOffset, u32 polyParamsSize, VkImageView stencilImageView, VkImageView depthImageView,
		VkImageView paletteImageView, VkImageLayout depthInputLayout, VkBuffer gpuPolyParams, VkDeviceSize gpuPolyParamsSize);
/* @layout: the colour attachment's where it is read (SSAA's are in the general one) */
void vk_oit_desc_sets_update_color_input(vk_oit_desc_sets *d, int index, VkImageView colorImageView, VkImageLayout layout);
/* The set a polygon with these textures is drawn with (@textureId1
 * (u64)-1: one volume): written once, and kept while it is drawn with
 * (desc_cache.h) */
VkDescriptorSet vk_oit_desc_sets_set_texture(vk_oit_desc_sets *d, u64 textureId0, u32 tsp0, u64 textureId1, u32 tsp1);
void vk_oit_desc_sets_bind_per_frame(const vk_oit_desc_sets *d, VkCommandBuffer cmd);
void vk_oit_desc_sets_bind_color_input(const vk_oit_desc_sets *d, VkCommandBuffer cmd, int index);
void vk_oit_desc_sets_bind_per_poly(const vk_oit_desc_sets *d, VkCommandBuffer cmd, VkDescriptorSet set);
/* the slot used again */
void vk_oit_desc_sets_reset(vk_oit_desc_sets *d);
void vk_oit_desc_sets_term(vk_oit_desc_sets *d);

/* ---- the pipelines (pso.h). A polygon's is kept under a key of
 * everything it is made of: its shader's bits (pso_key.h), the module and
 * subpass it takes, and its depth, stencil, cull and blend states as they
 * are made. ---- */

/* The key's fields. Bits 0 to 13 are the shader's (pso_key.h); 56 to 59
 * say what kind of pipeline it is, 60 to 63 are the key log's. */
#define VK_OIT_KEY_GOURAUD 14
#define VK_OIT_KEY_RATE 15		/* 2 bits: 0 without SSAA, 1 at each sample, 2 once a pixel */
#define VK_OIT_KEY_PASS 17		/* 2 bits: VK_OIT_PASS_* */
#define VK_OIT_KEY_SUBPASS 19	/* 2 bits */
#define VK_OIT_KEY_CULL 21		/* 2 bits: none, front, back */
#define VK_OIT_KEY_ZWRITE 23
#define VK_OIT_KEY_DEPTH_OP 24	/* 3 bits: VkCompareOp */
#define VK_OIT_KEY_STENCIL 27	/* 2 bits: none, the shadow bit set, cleared */
#define VK_OIT_KEY_BLEND 29
#define VK_OIT_KEY_COLOR_WRITE 30
#define VK_OIT_KEY_SRC 31		/* 3 bits: the PVR's */
#define VK_OIT_KEY_DST 34		/* 3 bits */
#define VK_OIT_KEY_KIND 56		/* 4 bits: VK_OIT_KIND_* */
enum { VK_OIT_KIND_POLY, VK_OIT_KIND_GENERAL, VK_OIT_KIND_FIXED };

/* the fixed pipelines' slots: the modifier volumes' by mode and cull mode, the last subpass's, the lists' */
enum { VK_OIT_FIXED_MODVOL = 0, VK_OIT_FIXED_TRMODVOL = 12, VK_OIT_FIXED_FINAL = 24, VK_OIT_FIXED_FINAL_PER_SAMPLE,
	VK_OIT_FIXED_CLEAR, VK_OIT_FIXED_FINAL_SUMS, VK_OIT_FIXED_COUNT };

typedef struct vk_oit_pipeline_mgr
{
	vk_pso_builder builder;	/* (first: the worker's) */
	vk_pso_table pipelines;	/* the polygons' own */
	vk_pso_table general;	/* the general ones */
	PsoSlot fixedSlots[VK_OIT_FIXED_COUNT];
	/* prepare()'s, for the device: with VK_EXT_extended_dynamic_state the
	 * cull mode is not in the modifier volumes' pipelines (only their none
	 * slots are made) but set with the draws (cullSetter); otherwise a
	 * pipeline each */
	u32 cullKept;
	void (VKAPI_PTR *cullSetter)(VkCommandBuffer, VkCullModeFlags);
	bool sums;		/* the last pass stores SSAA's channel sums */
	bool prepared;
	/* what the pipelines are made for (one of a kind: all of them work with any) */
	VkRenderPass renderPass;
	u64 lastKey;
	PsoChoice lastChoice;

	VkPipelineLayout pipelineLayout;
	VkDescriptorSetLayout perFrameLayout;
	VkDescriptorSetLayout colorInputLayout;
	VkDescriptorSetLayout perPolyLayout;
	vk_oit_passes passes;
	vk_oit_shaders *shaders;
	/* the key log's: the screen's 3, the textures' 4 */
	u32 who;
} vk_oit_pipeline_mgr;

/* @rtt: the renders to a texture's (their render passes, their keys) */
void vk_oit_pipeline_mgr_construct(vk_oit_pipeline_mgr *m, bool rtt);
/* @samples: SSAA's (1: off), which the shaders have been given too;
 * @buffersLayout and @sums: the fragment buffers' (OITBuffers). The
 * general pipelines and the fixed ones are made here. False if something
 * could not be made. */
bool vk_oit_pipeline_mgr_init(vk_oit_pipeline_mgr *m, vk_oit_shaders *shaders, VkDescriptorSetLayout buffersLayout, bool sums,
		u32 samples, bool keepDepth);
/* the renders to a texture's: what was made for the render passes goes before they do */
bool vk_oit_pipeline_mgr_rtt_init(vk_oit_pipeline_mgr *m, vk_oit_shaders *shaders, VkDescriptorSetLayout buffersLayout, bool sums);
void vk_oit_pipeline_mgr_term(vk_oit_pipeline_mgr *m);
/* Everything made dropped: what the worker has of it first. (Render
 * passes made again, another count of samples.) */
void vk_oit_pipeline_mgr_reset(vk_oit_pipeline_mgr *m);
/* @pass: VK_OIT_PASS_* */
PsoChoice vk_oit_pipeline_mgr_get(vk_oit_pipeline_mgr *m, u32 listType, bool autosort, const PolyParam *pp, int pass);
/* the dynamic states of a general pipeline's draw, after it is bound */
void vk_oit_pipeline_mgr_set_dynamic_state(const vk_oit_pipeline_mgr *m, VkCommandBuffer cmd, u64 key);

/* @cullMode: the ISP's. With the cull mode dynamic (cullSetter) one
 * pipeline draws them all, the cull mode set with vk_pso_cull. */
static INLINE VkPipeline vk_oit_pipeline_mgr_modvol(vk_oit_pipeline_mgr *m, int mode, u32 cullMode)
{
	return vk_pso_get(&m->fixedSlots[VK_OIT_FIXED_MODVOL + (u32)mode * 3 + (PsoCullIndex(cullMode) & m->cullKept)]);
}
static INLINE VkPipeline vk_oit_pipeline_mgr_trmodvol(vk_oit_pipeline_mgr *m, int mode, u32 cullMode)
{
	return vk_pso_get(&m->fixedSlots[VK_OIT_FIXED_TRMODVOL + (u32)mode * 3 + (PsoCullIndex(cullMode) & m->cullKept)]);
}
/* @perSample: SSAA's for a render pass that is not the last; @sums: the
 * last one's, which also stores the channel sums, when there is one */
static INLINE VkPipeline vk_oit_pipeline_mgr_final(vk_oit_pipeline_mgr *m, bool perSample, bool sums)
{
	return vk_pso_get(&m->fixedSlots[perSample ? VK_OIT_FIXED_FINAL_PER_SAMPLE
			: sums && m->sums ? VK_OIT_FIXED_FINAL_SUMS : VK_OIT_FIXED_FINAL]);
}
static INLINE VkPipeline vk_oit_pipeline_mgr_clear(vk_oit_pipeline_mgr *m)
{
	return vk_pso_get(&m->fixedSlots[VK_OIT_FIXED_CLEAR]);
}

#ifdef __cplusplus
}
#endif

#endif
