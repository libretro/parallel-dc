/*
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
#pragma once
/* The Direct3D 12 renderer's shaders: DXBC made when the core is built
 * (dx12_dxbc.inc, from shaders/dx12_raster.hlsl, by
 * tools/rend/kernels_dx12.sh), the same as DXIL for a device of shader
 * model 6.0 (dx12_dxil.inc), and the texture kernels the Direct3D 11
 * renderer has (rend/texdecode_dxbc.inc). Nothing is compiled when the
 * core runs, and it needs no compiler. */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include "types.h"

/* By their number in dx12_dxbc.inc. Smooth and flat shading are one
 * shader (DX12_PF_GOURAUD). */
enum class DX12Shader
{
	PolyVS = 1,
	PolyPS = 2,
	ModVolVS = 3,
	ModVolPS = 4,
	QuadVS = 5,
	QuadPS = 6,
	// the per-pixel renderer's
	OITVS = 7,
	OITDepthPS = 8,		// DX12_PASS_DEPTH; then the colour pass's, then the lists'
	OITColorPS = 9,
	OITListPS = 10,
	OITFinalVS = 11,
	OITFinalPS = 12,
	OITModVolPS = 13,
	/* SSAA's (DX12_SSAA): each of these three at each sample, then once a
	 * pixel */
	SsaaPolyPS = 14,
	SsaaOITDepthPS = 16,
	SsaaOITColorPS = 18,
	SsaaOITListPS = 20,			// once a pixel
	SsaaOITFinalPS = 21,		// the lists put together and the samples resolved
	SsaaOITFinalSamplePS = 22,	// ...put together into each sample
	SsaaOITModVolPS = 23,
	SsaaResolvePS = 24,
	/* the lists' once a pixel, taking SV_Coverage for the samples the depth
	 * test passed: for pipelines made with NVIDIA's post-depth coverage
	 * (dx12_nvapi.h) */
	SsaaOITListPostZPS = 25,
	/* the temporal upscaler's motion vector pass (dx12_renderer.h) */
	MotionVS = 26,
	MotionPS = 27,
	MotionSampledPS = 28,	// testing the depth the compute renderer stored itself
	DepthCopyPS = 29,		// the depth buffer copied into the upscaler's picture of it
};

D3D12_SHADER_BYTECODE dx12_shader(DX12Shader shader);

/* The HDR output's, by their number in dx12_hdr_dxbc.inc (made by
 * tools/rend/kernels_dx12.sh update-hdr) */
enum class DX12HdrShader
{
	/* SsaaOITFinalPS that stores the samples' channel sums as well, for
	 * the encode (DX12_OIT_SUMS) */
	SsaaOITFinalSumsPS = 1,
};

D3D12_SHADER_BYTECODE dx12_hdr_shader(DX12HdrShader shader);
/* rend/texdecode.h's, by TEXD_KERNEL_D3D (1 to 4) */
D3D12_SHADER_BYTECODE dx12_texdecode_kernel(int kernel);
/* Whether the raster and HDR shaders above are DXIL (shader model 6.0,
 * dx12_dxil.inc) or DXBC (5.1): DXIL where @device takes it. Before a
 * pipeline is made with them. */
bool dx12_shaders_choose(ID3D12Device *device);
/* A sum of all of the bytecode above, as chosen: a pipeline kept on disk was
 * made with it only if it is the same (dx12_pso.cpp) */
u32 dx12_shaders_hash();
