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
#include "dx12_nvapi.h"
#include <gfx/nvidia/nvapi_loader.h>
#include "types.h"
#include <stddef.h>
#include <string.h>

/* From the NVAPI SDK (github.com/NVIDIA/nvapi), which NVIDIA publishes
 * under the MIT licence:
 *   SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION &
 *   AFFILIATES. All rights reserved. SPDX-License-Identifier: MIT
 * the interface ids (nvapi_interface.h) and the layout of the two
 * structures below (nvapi.h: NVAPI_D3D12_PSO_EXTENSION_DESC_V1 and
 * NVAPI_D3D12_PSO_RASTERIZER_STATE_DESC_V1, packed to 8 bytes as the
 * header packs them). A structure's version word is its size with the
 * revision in the high half, as MAKE_NVAPI_VERSION makes it. */
#define NvId_D3D12_CreateGraphicsPipelineState 0x2fc28856u
#define NvPsoRasterExtension 0		/* NV_PSO_RASTER_EXTENSION */

#pragma pack(push, 8)
typedef struct NvPsoExtensionDesc
{
	u32 baseVersion;
	int psoExtension;			/* NV_PSO_EXTENSION */
} NvPsoExtensionDesc;

/* (the header's derives from the other; the same layout, as its first member;
 * its bools a byte each, as C++'s) */
typedef struct NvPsoRasterizerStateDesc
{
	NvPsoExtensionDesc base;
	u32 version;
	u8 ProgrammableSamplePositionsEnable;
	u8 InterleavedSamplingEnable;
	u8 SampleCount;
	u8 SamplePositionsX[16];
	u8 SamplePositionsY[16];
	int QuadFillMode;			/* NVAPI_QUAD_FILLMODE, 0: disabled */
	u8 PostZCoverageEnable;
	u8 CoverageToColorEnable;
	u8 CoverageToColorRTIndex;
	u8 TargetIndepentRasterWithDepth;	/* (revision 2) */
	u8 ForcedSampleCount;		/* (revision 2) */
	u8 reserved[62];
} NvPsoRasterizerStateDesc;
#pragma pack(pop)

#define NV_CHECK(name, c) typedef char nv_check_##name[(c) ? 1 : -1]
NV_CHECK(ext, sizeof(NvPsoExtensionDesc) == 8);
NV_CHECK(version, offsetof(NvPsoRasterizerStateDesc, version) == 8);
NV_CHECK(positions, offsetof(NvPsoRasterizerStateDesc, SamplePositionsY) == 31);
NV_CHECK(quad, offsetof(NvPsoRasterizerStateDesc, QuadFillMode) == 48);
NV_CHECK(postz, offsetof(NvPsoRasterizerStateDesc, PostZCoverageEnable) == 52);
NV_CHECK(size, sizeof(NvPsoRasterizerStateDesc) == 120);

/* NV_PSO_EXTENSION_DESC_VER; NV_RASTERIZER_PSO_EXTENSION_DESC_VER_1, which every driver with the function takes */
#define NvPsoExtensionDescVer ((u32)sizeof(NvPsoExtensionDesc) | (1u << 16))
#define NvRasterizerDescVer1 ((u32)sizeof(NvPsoRasterizerStateDesc) | (1u << 16))

typedef int (__cdecl *NvCreateGraphicsPipelineState)(ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
		u32 extensionCount, const NvPsoExtensionDesc **extensions, ID3D12PipelineState **pso);

static NvCreateGraphicsPipelineState nvCreateGraphicsPipelineState;
static bool probed;

bool dx12_nvapi_post_z_available(void)
{
	if (!probed)
	{
		probed = true;
		nvCreateGraphicsPipelineState = (NvCreateGraphicsPipelineState)nvapi_query(NvId_D3D12_CreateGraphicsPipelineState);
	}
	return nvCreateGraphicsPipelineState != NULL;
}

bool dx12_nvapi_create_post_z_pipeline(ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
		ID3D12PipelineState **out)
{
	NvPsoRasterizerStateDesc raster;
	const NvPsoExtensionDesc *extensions[1];
	*out = NULL;
	if (!dx12_nvapi_post_z_available())
		return false;
	memset(&raster, 0, sizeof(raster));
	raster.base.baseVersion = NvPsoExtensionDescVer;
	raster.base.psoExtension = NvPsoRasterExtension;
	raster.version = NvRasterizerDescVer1;
	/* (the render target's: what the header asks for without target-independent rasterization) */
	raster.SampleCount = (u8)desc->SampleDesc.Count;
	raster.PostZCoverageEnable = 1;
	extensions[0] = &raster.base;
	if (nvCreateGraphicsPipelineState(device, desc, 1, extensions, out) != NVAPI_OK || *out == NULL)
	{
		*out = NULL;
		return false;
	}
	return true;
}
