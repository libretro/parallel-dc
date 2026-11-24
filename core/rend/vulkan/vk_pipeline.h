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
/* The per-triangle renderer's pipelines (pso.h), its descriptor sets, and
 * the render pass of SSAA's frames */
#ifndef VK_PIPELINE_H
#define VK_PIPELINE_H
#include "vk_context.h"
#include "vk_image.h"
#include "pso.h"
#include "desc_cache.h"
#include "shaders.h"
#include "hw/pvr/ta_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The sets a frame slot draws with: the frame's (the uniforms, the fog and
 * palette textures) and, by texture, the polygons' (desc_cache.h) */
typedef struct vk_desc_sets
{
	vk_samplers *samplers;
	VkPipelineLayout pipelineLayout;
	VkDescriptorSetLayout perFrameLayout;
	VkDescriptorSetLayout perPolyLayout;
	VkDescriptorSet perFrameDescSet;
	VkDescriptorSet noTextureDescSet;
	VkImageView noTextureView;
	vk_desc_cache perPoly;
} vk_desc_sets;

void vk_desc_sets_construct(vk_desc_sets *d);
/* (the sets made now, with the drawer: one that first draws while the game
 * runs - a render to a texture - makes none then) */
void vk_desc_sets_init(vk_desc_sets *d, vk_samplers *samplers, VkPipelineLayout pipelineLayout,
		VkDescriptorSetLayout perFrameLayout, VkDescriptorSetLayout perPolyLayout);
/* (the vertex uniforms are a buffer of their own: BaseDrawer::WriteVertexUniforms()) */
void vk_desc_sets_update_uniforms(vk_desc_sets *d, VkBuffer vertexUniformBuffer, VkBuffer buffer, u32 fragmentUniformOffset,
		VkImageView fogImageView, VkImageView paletteImageView);
/* The set a polygon with this texture is drawn with: written once, and
 * kept while it is drawn with (desc_cache.h) */
VkDescriptorSet vk_desc_sets_set_texture(vk_desc_sets *d, u64 textureId, u32 tsp);
void vk_desc_sets_bind_per_frame(const vk_desc_sets *d, VkCommandBuffer cmd);
void vk_desc_sets_bind_per_poly(const vk_desc_sets *d, VkCommandBuffer cmd, VkDescriptorSet set);
/* the slot used again */
void vk_desc_sets_reset(vk_desc_sets *d);
void vk_desc_sets_term(vk_desc_sets *d);

/* What a draw is made with: its pipeline, and whether that is the general
 * one (pso.h), which is then told the key's bits with the push constants
 * and its dynamic states with vk_pipeline_mgr_set_dynamic_state() */
typedef struct PsoChoice
{
	VkPipeline pipeline;
	u64 key;
	bool general;
} PsoChoice;

/* What a polygon is being drawn for: itself, or a second time for the part
 * of it that is in a modifier volume (see rend/shadows.h), in which case
 * which of two polygons at the same depth is left showing matters. */
enum { VK_SHADOW_NONE, VK_SHADOW_LATER_WINS, VK_SHADOW_EARLIER_WINS };

/* The modifier volumes' pipelines. Depth: not a volume. The depth of sorted
 * translucent polygons, written after they are drawn, with no colour (main
 * vertices, triangle list). */
enum { VK_MODVOL_XOR, VK_MODVOL_OR, VK_MODVOL_INCLUSION, VK_MODVOL_EXCLUSION, VK_MODVOL_FINAL, VK_MODVOL_DEPTH };

/* The key's fields. Bits 0 to 14 are the shader's (pso_key.h); 56 to 59
 * say what kind of pipeline it is, 60 to 63 are the key log's. */
#define VK_KEY_GOURAUD 15
#define VK_KEY_RATE 16		/* 2 bits: 0 without SSAA, 1 at each sample, 2 once a pixel */
#define VK_KEY_CULL 18		/* 2 bits: none, front, back */
#define VK_KEY_LIST 20		/* a triangle list, not strips */
#define VK_KEY_ZWRITE 21
#define VK_KEY_DEPTH_OP 22	/* 3 bits: VkCompareOp */
#define VK_KEY_STENCIL 25	/* 3 bits: VK_STENCIL_* */
#define VK_KEY_BLEND 28
#define VK_KEY_SRC 29		/* 3 bits: the PVR's */
#define VK_KEY_DST 32		/* 3 bits */
#define VK_KEY_KIND 56		/* 4 bits: VK_KIND_* */
enum { VK_STENCIL_NONE, VK_STENCIL_SHADOW_SET, VK_STENCIL_SHADOW_CLEAR, VK_STENCIL_LATER_WINS, VK_STENCIL_EARLIER_WINS };
enum { VK_KIND_POLY, VK_KIND_GENERAL, VK_KIND_MODVOL, VK_KIND_PREPASS };

/* The per-triangle renderer's pipelines (pso.h). A polygon's is kept under
 * a key of everything it is made of: its shader's bits (pso_key.h), the
 * module it takes, and its depth, stencil, cull, topology and blend states
 * as they are made, so two polygons that differ in nothing a pipeline has
 * share one. */
typedef struct vk_pipeline_mgr
{
	vk_pso_builder builder;	/* (first: the worker's) */
	vk_pso_table pipelines;	/* the polygons' own */
	vk_pso_table general;	/* the general ones */
	/* the modifier volumes': by mode and cull mode (mode * 3 + PsoCullIndex()), made with the general ones */
	PsoSlot modVolSlots[VK_MODVOL_DEPTH * 3 + 3];
	/* the depth prepass's: [textured][Gouraud][no culling, front, back][SSAA: shaded at each sample] */
	PsoSlot prepassSlots[2][2][3][2];
	/* prepare()'s, for the device: with VK_EXT_extended_dynamic_state the
	 * cull mode is not in those pipelines (only their none slots are made)
	 * but set with the draws (cullSetter); otherwise a pipeline each */
	u32 cullKept;
	void (VKAPI_PTR *cullSetter)(VkCommandBuffer, VkCullModeFlags);
	u64 lastKey;
	PsoChoice lastChoice;

	VkPipelineLayout pipelineLayout;
	VkDescriptorSetLayout perFrameLayout;
	VkDescriptorSetLayout perPolyLayout;
	VkRenderPass renderPass;
	u32 samples;
	vk_raster_modules *modules;
	/* the key log's: the screen's 1, the textures' 2 */
	u32 who;
	/* the textures' own render pass (vk_pipeline_mgr_rtt_init()) */
	VkRenderPass rttRenderPass;
} vk_pipeline_mgr;

void vk_pipeline_mgr_construct(vk_pipeline_mgr *m, u32 who);
/* @samples: of the render pass's polygon subpass (SSAA; 1 without). The
 * general pipelines and the modifier volumes' are made here. False if the
 * layouts or the shaders could not be made. */
bool vk_pipeline_mgr_init(vk_pipeline_mgr *m, vk_raster_modules *modules, VkRenderPass renderPass, u32 samples);
/* The renders to a texture's, with a render pass of their own made here.
 * @samples: SSAA's. Then the polygons are drawn into attachments of that
 * many samples, and a second subpass resolves them into the texture: see
 * vk_make_ssaa_render_pass(). */
bool vk_pipeline_mgr_rtt_init(vk_pipeline_mgr *m, vk_raster_modules *modules, u32 samples);
/* everything made dropped, the layouts and render pass too */
void vk_pipeline_mgr_term(vk_pipeline_mgr *m);
/* Everything made dropped: what the worker has of it first. (A new
 * render pass or count of samples.) */
void vk_pipeline_mgr_reset(vk_pipeline_mgr *m);
/* @shadow: VK_SHADOW_* */
PsoChoice vk_pipeline_mgr_get(vk_pipeline_mgr *m, u32 listType, bool sortTriangles, const PolyParam *pp, int shadow);
/* the dynamic states of a general pipeline's draw, after it is bound */
void vk_pipeline_mgr_set_dynamic_state(const vk_pipeline_mgr *m, VkCommandBuffer cmd, u64 key);

/* @cullMode: the ISP's. With the cull mode dynamic (cullSetter) one
 * pipeline draws them all, the cull mode set with vk_pso_cull. */
static INLINE VkPipeline vk_pipeline_mgr_modvol(vk_pipeline_mgr *m, int mode, u32 cullMode)
{
	return vk_pso_get(&m->modVolSlots[(u32)mode * 3 + (PsoCullIndex(cullMode) & m->cullKept)]);
}

/* The depth prepass of a sorted translucent list (rend/sorter.h): the
 * pipeline a polygon is drawn with there. They are all made with the
 * render pass, none in the middle of a frame: textured or not, flat or
 * Gouraud, each culling, and each SSAA rate. */
VkPipeline vk_pipeline_mgr_prepass(vk_pipeline_mgr *m, const PolyParam *pp);

/* The render pass of SSAA's frames, the screen's and the textures':
 * attachment 0 the picture (@colorFormat, a single sample), 1 and 2 the
 * colour and depth attachments of @samples the polygons are drawn into.
 * Subpass 0 draws, subpass 1 resolves 1 into 0 (vk_ssaa_resolve). Only the
 * picture is stored; it is all written by the resolve, so not loaded.
 * With @hdrFormat (not VK_FORMAT_UNDEFINED), a subpass after the resolve
 * encodes the picture, its input, into attachment 3, the HDR output's
 * image (hdr_encode_vk.h). VK_NULL_HANDLE if it could not be made. */
VkRenderPass vk_make_ssaa_render_pass(VkFormat colorFormat, u32 samples, VkImageLayout pictureLayout, VkFormat hdrFormat);

/* ---- what the renderers' pipelines are made of (the per-pixel one's too) ---- */

typedef struct vk_pipe_states
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
} vk_pipe_states;

/* No vertex input (the caller's), @topology, @cull (none, front, back),
 * @samples a pixel shaded at each sample (@perSample) or once a pixel; no
 * depth, stencil or blending; the viewport and scissor dynamic */
void vk_pipe_states_init(vk_pipe_states *s, VkPrimitiveTopology topology, u32 cull, u32 samples, bool perSample);
void vk_pipe_states_dynamic(vk_pipe_states *s, VkDynamicState state);
/* the general pipelines' dynamic states: what the device can set with the draws */
void vk_pipe_states_general(vk_pipe_states *s);
/* the stages: @specialized, with the pipeline's specialization constants
 * (pso_key.h): the key, -1 for the one read from the push constants, and
 * SSAA's samples a pixel */
void vk_pipe_states_stages(vk_pipe_states *s, VkShaderModule vertex, VkShaderModule fragment, bool specialized, s32 key, u32 samples);
VkPipeline vk_pipe_states_make(vk_pipe_states *s, VkPipelineLayout layout, VkRenderPass renderPass, u32 subpass, VkPipelineCache cache);
VkCullModeFlags vk_pipe_cull_mode(u32 cull);
void vk_pipe_stencil_op(VkStencilOpState *s, VkStencilOp fail, VkStencilOp pass, VkStencilOp depthFail, VkCompareOp compare,
		u32 compareMask, u32 writeMask, u32 reference);
/* the PVR's blend instruction's (@src: the source's) */
VkBlendFactor vk_pipe_blend_factor(u32 instr, bool src);

#ifdef __cplusplus
}
#endif

#endif
