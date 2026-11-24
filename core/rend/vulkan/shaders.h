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
#ifndef VK_SHADERS_H
#define VK_SHADERS_H

#include "vulkan.h"
#include "pso_key.h"
#include "raster_spv.inc"

#ifdef __cplusplus
extern "C" {
#endif

/* The raster renderers' shaders are SPIR-V made when the core is built
 * (raster_spv.inc, from vulkan/shaders/ by tools/rend/kernels_spv.sh):
 * nothing is compiled when it runs. A module's words, by enum RasterSpv;
 * and the module made of them on the context's device, VK_NULL_HANDLE if
 * it could not be. */
const u32 *RasterSpvWords(int id, size_t *size);
VkShaderModule vk_raster_module(int id);

/* The modules a renderer draws with, all made by vk_raster_modules_make():
 * they are only read after, from any thread */
typedef struct vk_raster_modules
{
	VkShaderModule m[RSPV_COUNT];
} vk_raster_modules;

/* those of [first, last] not made yet; false if one could not be */
bool vk_raster_modules_make(vk_raster_modules *r, int first, int last);
void vk_raster_modules_term(vk_raster_modules *r);

/* std140 alignment required; shaders/position.glsl, and rend/depth_scale.h
 * for depth_scale: 1/fZ_max, which for a frame decoded on the GPU is the
 * decode's own word, copied over this one before the render pass */
typedef struct VertexShaderUniforms
{
	float normal_matrix[16];
	float depth_scale;
	float depth_mul;	/* 1 / what the matrix scales z by */
	float pad[2];
} VertexShaderUniforms;

/* std140 alignment required */
typedef struct FragmentShaderUniforms
{
	float colorClampMin[4];
	float colorClampMax[4];
	float sp_FOG_COL_RAM[4];	/* Only using 3 elements but easier for std140 */
	float sp_FOG_COL_VERT[4];	/* same comment */
	float cp_AlphaTestValue;
	float sp_FOG_DENSITY;
	float shade_scale_factor;
} FragmentShaderUniforms;

#ifdef __cplusplus
}

#include "vk_context.h"
#include "rend/sorter.h"

/* (both cast: on 32-bit x86, whose x87 keeps floats wider, the literal is
 * otherwise compared at the wider precision) */
static_assert((float)PREPASS_ALPHA_MIN == (float)PREPASS_ALPHA_MIN_GLSL, "the prepass's alpha, in shaders/prepass.frag (pso_key.h)");

static inline vk::UniqueShaderModule MakeRasterModule(RasterSpv id)
{
	const VkShaderModule module = vk_raster_module(id);
	if (module == VK_NULL_HANDLE)
		throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateShaderModule");
	return vk::UniqueShaderModule(vk::ShaderModule(module),
			vk::ObjectDestroy<vk::Device, vk::DispatchLoaderStatic>(vk::Device(vkc.device)));
}

/* The C++ renderers' view of a vk_raster_modules */
class RasterModules
{
public:
	RasterModules() { memset(&r, 0, sizeof(r)); }
	~RasterModules() { vk_raster_modules_term(&r); }
	RasterModules(const RasterModules&) = delete;
	RasterModules& operator=(const RasterModules&) = delete;
	/* [first, last] */
	void Make(RasterSpv first, RasterSpv last)
	{
		if (!vk_raster_modules_make(&r, first, last))
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateShaderModule");
	}
	vk::ShaderModule Get(RasterSpv id) const { return vk::ShaderModule(r.m[id]); }
	void Term() { vk_raster_modules_term(&r); }
	vk_raster_modules *Raw() { return &r; }

private:
	vk_raster_modules r;
};

/* A pipeline's specialization constants (pso_key.h): the key, -1 for
 * the one read from the push constants, and SSAA's samples a pixel */
struct PsoSpecialization
{
	s32 values[3];
	vk::SpecializationMapEntry entries[3];
	vk::SpecializationInfo info;

	PsoSpecialization(s32 key, u32 samples)
	{
		values[0] = key;
		values[1] = (s32)samples;
		u32 log2n = 0;
		while ((2u << log2n) <= samples)
			log2n++;
		values[2] = (s32)log2n;
		entries[0] = vk::SpecializationMapEntry(PSO_SC_KEY, 0, sizeof(s32));
		entries[1] = vk::SpecializationMapEntry(PSO_SC_SSAA_SAMPLES, sizeof(s32), sizeof(s32));
		entries[2] = vk::SpecializationMapEntry(PSO_SC_SSAA_LOG2N, 2 * sizeof(s32), sizeof(s32));
		info = vk::SpecializationInfo(3, entries, sizeof(values), values);
	}
	PsoSpecialization(const PsoSpecialization&) = delete;
	PsoSpecialization& operator=(const PsoSpecialization&) = delete;
};

struct VertexShaderParams
{
	bool gouraud;
};

/* alpha test, clip test, use alpha, texture, ignore alpha, shader instr, offset, fog, gouraud, bump, clamp, trilinear */
struct FragmentShaderParams
{
	bool alphaTest;
	bool insideClipTest;
	bool useAlpha;
	bool texture;
	bool ignoreTexAlpha;
	int shaderInstr;
	bool offset;
	int fog;
	bool gouraud;
	bool bumpmap;
	bool clamping;
	bool trilinear;
	bool palette;
	bool shadowed;	/* base and offset colours scaled: a pixel in a modifier volume */
	int sampleRate;	/* SSAA: shaded at each sample (1), or once a pixel at its centre (2); 0 without SSAA */
	int samples;	/* SSAA's (with sampleRate 2), 1 without */

	/* what the shader is specialized for (pso_key.h): all but gouraud, sampleRate and samples */
	s32 key() const
	{
		return ((s32)alphaTest << TRI_SC_ALPHA_TEST) | ((s32)insideClipTest << TRI_SC_CLIP_INSIDE)
			| ((s32)useAlpha << TRI_SC_USE_ALPHA) | ((s32)texture << TRI_SC_TEXTURE)
			| ((s32)ignoreTexAlpha << TRI_SC_IGNORE_TEX_A) | (shaderInstr << TRI_SC_SHAD_INSTR)
			| ((s32)offset << TRI_SC_OFFSET) | (fog << TRI_SC_FOG_CTRL) | ((s32)bumpmap << TRI_SC_BUMP_MAP)
			| ((s32)clamping << TRI_SC_CLAMPING) | ((s32)trilinear << TRI_SC_TRILINEAR)
			| ((s32)palette << TRI_SC_PALETTE) | ((s32)shadowed << TRI_SC_SHADOWED);
	}
};

/* The depth prepass's shader (rend/sorter.h): what its module is made for,
 * the rest being the push constants' flags (PREPASS_*, pso_key.h) */
struct PrepassShaderParams
{
	bool texture;
	bool gouraud;
	int sampleRate;	/* as FragmentShaderParams */
	int samples;
};

/* The per-triangle renderer's shaders, and the quads': made in Init(),
 * before any pipeline is */
class ShaderManager
{
public:
	void Init()
	{
		modules.Make(RSPV_TRI_VERT_G0, RSPV_PREPASS_FRAG_T1_G1_R2);
	}
	vk::ShaderModule GetVertexShader(const VertexShaderParams& params) const
	{
		return modules.Get(params.gouraud ? RSPV_TRI_VERT_G1 : RSPV_TRI_VERT_G0);
	}
	/* the module of @params' interpolation and sample rate: the rest is its key() */
	vk::ShaderModule GetFragmentShader(const FragmentShaderParams& params) const
	{
		return modules.Get((RasterSpv)(RSPV_TRI_FRAG_G0_R0 + (params.gouraud ? 3 : 0) + params.sampleRate));
	}
	vk::ShaderModule GetPrepassShader(const PrepassShaderParams& params) const
	{
		return modules.Get((RasterSpv)(RSPV_PREPASS_FRAG_T0_G0_R0 + (params.texture ? 6 : 0) + (params.gouraud ? 3 : 0) + params.sampleRate));
	}
	vk::ShaderModule GetModVolVertexShader() const { return modules.Get(RSPV_MODVOL_VERT); }
	vk::ShaderModule GetModVolShader() const { return modules.Get(RSPV_MODVOL_FRAG); }
	vk::ShaderModule GetMotionVertexShader() const { return modules.Get(RSPV_MV_VERT); }
	vk::ShaderModule GetMotionShader() const { return modules.Get(RSPV_MV_FRAG); }
	vk::ShaderModule GetQuadVertexShader() const { return modules.Get(RSPV_QUAD_VERT); }
	vk::ShaderModule GetQuadFragmentShader() const { return modules.Get(RSPV_QUAD_FRAG); }
	vk::ShaderModule GetSsaaResolveVertexShader() const { return modules.Get(RSPV_SSAA_RESOLVE_VERT); }
	vk::ShaderModule GetSsaaResolveShader() const { return modules.Get(RSPV_SSAA_RESOLVE_FRAG); }
	void Term()
	{
		modules.Term();
	}
	vk_raster_modules *Raw() { return modules.Raw(); }

private:
	RasterModules modules;
};
#endif

#endif
