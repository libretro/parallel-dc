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
#endif

#endif
