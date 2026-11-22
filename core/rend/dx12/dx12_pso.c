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
#include "dx12_pso.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <streams/file_stream.h>
#include <retro_miscellaneous.h>
#include "dx12context_lr.h"
#include "dx12_shaders.h"
#include "dx12_nvapi.h"
#include "hw/pvr/ta_ctx.h"
#include "paths.h"

/* (mingw-w64's d3d12.h has ID3D12Device1, which makes them, and not the
 * interface itself: as the Windows SDK has it) */
#ifndef __ID3D12PipelineLibrary_INTERFACE_DEFINED__
typedef struct ID3D12PipelineLibraryVtbl
{
	HRESULT (STDMETHODCALLTYPE *QueryInterface)(struct ID3D12PipelineLibrary *This, REFIID riid, void **object);
	ULONG (STDMETHODCALLTYPE *AddRef)(struct ID3D12PipelineLibrary *This);
	ULONG (STDMETHODCALLTYPE *Release)(struct ID3D12PipelineLibrary *This);
	HRESULT (STDMETHODCALLTYPE *GetPrivateData)(struct ID3D12PipelineLibrary *This, REFGUID guid, UINT *size, void *data);
	HRESULT (STDMETHODCALLTYPE *SetPrivateData)(struct ID3D12PipelineLibrary *This, REFGUID guid, UINT size, const void *data);
	HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(struct ID3D12PipelineLibrary *This, REFGUID guid, const IUnknown *data);
	HRESULT (STDMETHODCALLTYPE *SetName)(struct ID3D12PipelineLibrary *This, LPCWSTR name);
	HRESULT (STDMETHODCALLTYPE *GetDevice)(struct ID3D12PipelineLibrary *This, REFIID riid, void **device);
	HRESULT (STDMETHODCALLTYPE *StorePipeline)(struct ID3D12PipelineLibrary *This, LPCWSTR name, ID3D12PipelineState *pipeline);
	HRESULT (STDMETHODCALLTYPE *LoadGraphicsPipeline)(struct ID3D12PipelineLibrary *This, LPCWSTR name,
			const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline);
	HRESULT (STDMETHODCALLTYPE *LoadComputePipeline)(struct ID3D12PipelineLibrary *This, LPCWSTR name,
			const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline);
	SIZE_T (STDMETHODCALLTYPE *GetSerializedSize)(struct ID3D12PipelineLibrary *This);
	HRESULT (STDMETHODCALLTYPE *Serialize)(struct ID3D12PipelineLibrary *This, void *data, SIZE_T size);
} ID3D12PipelineLibraryVtbl;
struct ID3D12PipelineLibrary
{
	const ID3D12PipelineLibraryVtbl *lpVtbl;
};
#endif

/* (the interfaces' ids, here: nothing linked for them) */
static const IID iid_pipeline_library = { 0xc64226a8, 0x9201, 0x46af, { 0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7, 0x41, 0x4f } };
static const IID iid_pipeline_state = { 0x765a30f3, 0xf624, 0x4c6f, { 0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45 } };
static const IID iid_device1 = { 0x77acce80, 0x638e, 0x4e65, { 0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e } };

dx12_pipelines dx12p;

static const char *PipelineFileName = "d3d12_pipelines.bin";

/* The file: this header, the keys, then the library's data. The library
 * is only of the shaders it was made with: a pipeline's description names
 * its bytecode, and one kept for other bytecode is never loaded again, nor
 * can one of the same name be stored. */
typedef struct DX12PsoFileHeader
{
	char magic[8];
	u32 version;	/* of the keys' layout (DX12PsoKey) */
	u32 keyCount;
	u64 libraryBytes;
	/* (from version 3) */
	u32 shaders;	/* dx12_shaders_hash() of the library's pipelines */
	u32 reserved;
} DX12PsoFileHeader;
static const char PsoFileMagic[8] = { 'F', 'C', 'D', '1', '2', 'P', 'S', 'O' };
/* 3: a polygon's blend folded into its key (dx12_pso_key_pack()); 4: its
 * cull mode too; 5: its shading; 6: the per-pixel renderer's blend. A file
 * of 2 to 5 gives its keys, folded, and no library */
#define PSO_FILE_VERSION 6
/* (room for the batches a start or SSAA hands over at once) */
#define BATCH_JOBS (DX12_PSO_SLOTS * 2)

/* What a build did (dx12_pso_build_desc()): counted by the thread that renders */
enum
{
	DX12_BUILT_LIBRARY = 1,		/* handed back by the library */
	DX12_BUILT_STORED = 2,		/* stored in it */
	DX12_BUILT_POSTZ = 4,		/* with NVAPI's post-depth coverage */
	DX12_BUILT_OWNTEST = 8,		/* SSAA's lists testing their samples themselves */
	DX12_BUILT_NOPOSTZ = 16		/* NVAPI made none: it is not asked again */
};

static const D3D12_COMPARISON_FUNC depthFuncs[] =
{
	D3D12_COMPARISON_FUNC_NEVER,			/* 0 Never */
	D3D12_COMPARISON_FUNC_LESS,				/* 1 Less */
	D3D12_COMPARISON_FUNC_EQUAL,			/* 2 Equal */
	D3D12_COMPARISON_FUNC_LESS_EQUAL,		/* 3 Less Or Equal */
	D3D12_COMPARISON_FUNC_GREATER,			/* 4 Greater */
	D3D12_COMPARISON_FUNC_NOT_EQUAL,		/* 5 Not Equal */
	D3D12_COMPARISON_FUNC_GREATER_EQUAL,	/* 6 Greater Or Equal */
	D3D12_COMPARISON_FUNC_ALWAYS,			/* 7 Always */
};

static const D3D12_INPUT_ELEMENT_DESC MainLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(Vertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, vtx_spc), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, (UINT)offsetof(Vertex, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};
/* (the per-pixel renderer's: with the second volume's colours and coordinates) */
static const D3D12_INPUT_ELEMENT_DESC OITLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(Vertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, vtx_spc), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, (UINT)offsetof(Vertex, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR",    2, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, col1), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR",    3, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, spc1), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT,    0, (UINT)offsetof(Vertex, u1), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	/* (a translucent strip's number, an instance's: in the second vertex buffer) */
	{ "POLYNUMBER", 0, DXGI_FORMAT_R32_UINT,      1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
};
/* (the motion vector pass's: the position, and where the vertex was, in
 * its own stream) */
static const D3D12_INPUT_ELEMENT_DESC MotionLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(Vertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "PREVPOS",  0, DXGI_FORMAT_R32G32_FLOAT,    1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};
/* (a position alone: the modifier volumes' triangles, and the polygons'
 * vertices read for their position only - the stride is the view's) */
static const D3D12_INPUT_ELEMENT_DESC PositionLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

static void stencil_desc(DX12Stencil stencil, D3D12_DEPTH_STENCIL_DESC *desc)
{
	D3D12_DEPTH_STENCILOP_DESC *op = &desc->FrontFace;
	desc->StencilEnable = stencil != DX12Stencil_None;
	desc->StencilReadMask = 0xff;
	desc->StencilWriteMask = 0xff;
	op->StencilFailOp = op->StencilDepthFailOp = op->StencilPassOp = D3D12_STENCIL_OP_KEEP;
	op->StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	/* (the reference is set with the draw, OMSetStencilRef()) */
	switch (stencil)
	{
	case DX12Stencil_None:
	case DX12Stencil_Count:
		break;
	case DX12Stencil_Shadow:
		op->StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		desc->StencilReadMask = 0;
		desc->StencilWriteMask = 0x80;
		break;
	case DX12Stencil_ShadowLaterWins:
		op->StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		op->StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc->StencilReadMask = 0x81;
		desc->StencilWriteMask = 0x40;
		break;
	case DX12Stencil_ShadowEarlierWins:
		op->StencilPassOp = D3D12_STENCIL_OP_INVERT;
		op->StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc->StencilReadMask = 0xC1;
		desc->StencilWriteMask = 0x40;
		break;
	case DX12Stencil_Xor:
		op->StencilPassOp = D3D12_STENCIL_OP_INVERT;
		desc->StencilReadMask = 0;
		desc->StencilWriteMask = 2;
		break;
	case DX12Stencil_Or:
		op->StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		desc->StencilReadMask = 2;
		desc->StencilWriteMask = 2;
		break;
	case DX12Stencil_Inclusion:
		op->StencilFailOp = op->StencilDepthFailOp = D3D12_STENCIL_OP_ZERO;
		op->StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		op->StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc->StencilReadMask = 3;
		desc->StencilWriteMask = 3;
		break;
	case DX12Stencil_Exclusion:
		op->StencilFailOp = op->StencilDepthFailOp = D3D12_STENCIL_OP_ZERO;
		op->StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc->StencilReadMask = 3;
		desc->StencilWriteMask = 3;
		break;
	case DX12Stencil_Final:
		/* takes shadows, in a volume, and not drawn again for it (bit 6);
		 * clears the volume bits and that mark */
		op->StencilFailOp = op->StencilDepthFailOp = op->StencilPassOp = D3D12_STENCIL_OP_ZERO;
		op->StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc->StencilReadMask = 0xC1;
		desc->StencilWriteMask = 0x43;
		break;
	}
	desc->BackFace = desc->FrontFace;
}

#define LAYOUT(d, l) ((d).pInputElementDescs = (l), (d).NumElements = (UINT)ARRAY_SIZE(l))

bool dx12_pso_build_desc(u64 packed, bool postZCoverage, bool useLibrary, ID3D12PipelineState **out, u32 *flags)
{
	const DX12PsoKey key = dx12_pso_key_unpack(packed);
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	D3D12_RENDER_TARGET_BLEND_DESC *rt = &desc.BlendState.RenderTarget[0];
	D3D12_RASTERIZER_DESC *rs = &desc.RasterizerState;
	D3D12_DEPTH_STENCIL_DESC *ds = &desc.DepthStencilState;
	/* SSAA's shaders: by rate (dx12_shaders.h) */
	const int rateShader = key.rate == DX12_RATE_PIXEL ? 1 : 0;
	int vs, ps;
	bool postZ = false;
	wchar_t name[24];
	struct ID3D12PipelineLibrary *library = dx12p.library;

	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = dx12p.root;
	*flags = 0;
	switch (key.kind)
	{
	case DX12_PSO_POLY:
		vs = DX12S_PolyVS;
		ps = DX12S_PolyPS;
		if (key.rate != DX12_RATE_NONE)
			ps = DX12S_SsaaPolyPS + rateShader;
		LAYOUT(desc.InputLayout, MainLayout);
		break;
	case DX12_PSO_MODVOL:
		vs = DX12S_ModVolVS;
		ps = DX12S_ModVolPS;
		LAYOUT(desc.InputLayout, PositionLayout);
		break;
	case DX12_PSO_OIT:
		vs = DX12S_OITVS;
		ps = DX12S_OITDepthPS + (int)(key.pass < DX12_PASS_OIT ? key.pass : DX12_PASS_OIT);
		if (key.rate != DX12_RATE_NONE)
		{
			/* (the lists' are once a pixel only) */
			ps = key.pass >= DX12_PASS_OIT ? DX12S_SsaaOITListPS
					: DX12S_SsaaOITDepthPS + 2 * (int)key.pass + rateShader;
			/* The lists' with the device's own depth test at each sample,
			 * where NVAPI makes the pipeline with post-depth coverage: the
			 * pass tests greater or equal against the opaque depth,
			 * read-only, and the shader takes the samples that passed
			 * instead of testing them itself, a few ulp off */
			postZ = postZCoverage && key.pass >= DX12_PASS_OIT && key.samples != 0 && key.depthBound && key.depthTest
					&& !key.depthWrite && key.depthFunc == 6;
			if (postZ)
				ps = DX12S_SsaaOITListPostZPS;
		}
		LAYOUT(desc.InputLayout, OITLayout);
		break;
	case DX12_PSO_OIT_FINAL:
		vs = DX12S_OITFinalVS;
		ps = key.rate == DX12_RATE_PIXEL ? DX12S_SsaaOITFinalPS : key.rate == DX12_RATE_SAMPLE ? DX12S_SsaaOITFinalSamplePS
				: DX12S_OITFinalPS;
		break;
	case DX12_PSO_OIT_MODVOL:
		vs = DX12S_ModVolVS;
		ps = key.rate != DX12_RATE_NONE ? DX12S_SsaaOITModVolPS : DX12S_OITModVolPS;
		LAYOUT(desc.InputLayout, PositionLayout);
		break;
	case DX12_PSO_SSAA_RESOLVE:
		vs = DX12S_OITFinalVS;
		ps = DX12S_SsaaResolvePS;
		break;
	case DX12_PSO_MOTION:
		if (key.pass == 1)
		{
			vs = DX12S_QuadVS;
			ps = DX12S_DepthCopyPS;
			break;
		}
		vs = DX12S_MotionVS;
		/* (with no depth bound, the compute renderer's frame: the shader tests the depth it stored) */
		ps = key.depthBound ? DX12S_MotionPS : DX12S_MotionSampledPS;
		LAYOUT(desc.InputLayout, MotionLayout);
		break;
	default:
		vs = DX12S_QuadVS;
		ps = DX12S_QuadPS;
		break;
	}
	desc.VS = dx12_shader(vs);
	desc.PS = dx12_shader(ps);
	/* (the HDR output's, of its own bytecode) */
	if (key.kind == DX12_PSO_OIT_FINAL && key.rate == DX12_RATE_PIXEL_SUMS)
		desc.PS = dx12_hdr_shader(DX12HS_SsaaOITFinalSumsPS);

	rt->BlendOp = rt->BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt->LogicOp = D3D12_LOGIC_OP_NOOP;
	rt->RenderTargetWriteMask = key.writeColor ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
	if (key.special == DX12_BLEND_ALPHA)
	{
		rt->BlendEnable = TRUE;
		rt->SrcBlend = rt->SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
		rt->DestBlend = rt->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	}
	else if (key.special == DX12_BLEND_FACTOR)
	{
		rt->BlendEnable = TRUE;
		rt->SrcBlend = rt->SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
		rt->DestBlend = rt->DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
	}
	else
		dx12_blend_desc(&key, rt);
	desc.SampleMask = UINT_MAX;

	rs->FillMode = D3D12_FILL_MODE_SOLID;
	rs->CullMode = key.cull == 1 ? D3D12_CULL_MODE_FRONT : key.cull == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
	rs->FrontCounterClockwise = TRUE;
	/* (the depth is the shaders' own, the clip space's is always 1) */
	rs->DepthClipEnable = key.kind == DX12_PSO_QUAD;

	ds->DepthEnable = key.depthTest;
	ds->DepthWriteMask = key.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	ds->DepthFunc = depthFuncs[key.depthFunc & 7];
	stencil_desc(key.stencil, ds);
	if (!key.depthBound)
	{
		ds->DepthEnable = FALSE;
		ds->DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		ds->StencilEnable = FALSE;
	}

	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = key.kind != DX12_PSO_MOTION ? DX12FrameFormat : key.pass == 1 ? DX12UpscalerDepthFormat : DX12MotionFormat;
	desc.DSVFormat = key.depthBound ? DX12DepthFormat : DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1u << key.samples;
	/* (SSAA's samples where rend/ssaa_pattern.h has them) */
	desc.SampleDesc.Quality = key.samples != 0 ? DX12StandardPattern : 0;

	if (postZ)
	{
		/* (not through the library: NVAPI's pipelines have no cached blob) */
		if (dx12_nvapi_create_post_z_pipeline(dx12p.device, &desc, out))
		{
			*flags |= DX12_BUILT_POSTZ;
			return true;
		}
		*flags |= DX12_BUILT_NOPOSTZ;
		desc.PS = dx12_shader(DX12S_SsaaOITListPS);
	}
	if (key.kind == DX12_PSO_OIT && key.pass >= DX12_PASS_OIT && key.rate != DX12_RATE_NONE)
		*flags |= DX12_BUILT_OWNTEST;

	swprintf(name, 24, L"fc%016llx", (unsigned long long)packed);
	if (useLibrary && library != NULL
			&& SUCCEEDED(library->lpVtbl->LoadGraphicsPipeline(library, name, &desc, &iid_pipeline_state, (void **)out)))
	{
		*flags |= DX12_BUILT_LIBRARY;
		return true;
	}
	if (FAILED(ID3D12Device_CreateGraphicsPipelineState(dx12p.device, &desc, &iid_pipeline_state, (void **)out)))
	{
		*out = NULL;
		return false;
	}
	if (useLibrary && library != NULL && SUCCEEDED(library->lpVtbl->StorePipeline(library, name, *out)))
		*flags |= DX12_BUILT_STORED;
	return true;
}

static void count_built(u32 flags)
{
	if (flags & DX12_BUILT_LIBRARY)
		dx12p.statFromLibrary++;
	if (flags & DX12_BUILT_STORED)
		dx12p.storedSinceLoad++;
	if (flags & DX12_BUILT_POSTZ)
		dx12p.statPostZ++;
	if (flags & DX12_BUILT_OWNTEST)
		dx12p.statOwnTest++;
}

/* (the thread that renders: the library is its own when the thread ahead
 * is not building) */
static bool build(u64 packed, ID3D12PipelineState **out)
{
	u32 flags;
	const bool ok = dx12_pso_build_desc(packed, dx12p.postZCoverage, !d3d_warm_busy(dx12p.warm), out, &flags);
	if (flags & DX12_BUILT_NOPOSTZ)
	{
		WARN_LOG(RENDERER, "DX12: NVAPI made no pipeline with post-depth coverage: SSAA's lists test their samples themselves\n");
		dx12p.postZCoverage = false;
	}
	count_built(flags);
	return ok;
}

/* The lists' pipelines NVAPI would make: never the thread's to make */
static bool wants_post_z(const DX12PsoKey *key)
{
	return dx12p.postZCoverage && key->kind == DX12_PSO_OIT && key->rate != DX12_RATE_NONE && key->pass >= DX12_PASS_OIT
			&& key->samples != 0 && key->depthBound && key->depthTest && !key->depthWrite && key->depthFunc == 6;
}

static void keep_key(u64 key)
{
	if (dx12p.keyCount < DX12_PSO_MAX_KEYS)
		dx12p.keys[dx12p.keyCount++] = key;
}

static ID3D12PipelineState *make(u64 key, u32 slot, bool inFrame)
{
	ID3D12PipelineState *pso = NULL;
	if (dx12p.used >= DX12_PSO_SLOTS * 3 / 4 || !build(key, &pso))
	{
		WARN_LOG(RENDERER, "DX12: pipeline %016llx could not be made\n", (unsigned long long)key);
		return NULL;
	}
	dx12p.slots[slot].key = key;
	dx12p.slots[slot].pso = pso;
	dx12p.used++;
	dx12p.statMade++;
	if (inFrame && dx12p.started)
	{
		dx12p.statMadeInFrame++;
		NOTICE_LOG(RENDERER, "DX12: pipeline %016llx made in a frame\n", (unsigned long long)key);
	}
	keep_key(key);
	return pso;
}

static void insert(u64 key, ID3D12PipelineState *pso, u32 flags)
{
	u32 i = dx12_pso_hash(key);
	for (;;)
	{
		const dx12_pso_slot *s = &dx12p.slots[i];
		if (s->key == key)
		{
			/* (made here meanwhile) */
			ID3D12PipelineState_Release(pso);
			return;
		}
		if (s->key == 0)
			break;
		i = (i + 1) & (DX12_PSO_SLOTS - 1);
	}
	if (dx12p.used >= DX12_PSO_SLOTS * 3 / 4)
	{
		ID3D12PipelineState_Release(pso);
		return;
	}
	dx12p.slots[i].key = key;
	dx12p.slots[i].pso = pso;
	dx12p.used++;
	dx12p.statMade++;
	dx12p.statAhead++;
	count_built(flags);
	keep_key(key);
}

/* What the thread handed back, into the table */
static void take_built(void)
{
	d3d_warm_result_t r;
	while (d3d_warm_take(dx12p.warm, &r))
		insert(r.key, (ID3D12PipelineState *)r.obj, r.aux);
}

static bool requested(u64 key)
{
	u32 i = dx12_pso_hash(key) * 2;
	for (;;)
	{
		const u64 k = dx12p.requested[i];
		if (k == key)
			return true;
		if (k == 0)
			return false;
		i = (i + 1) & (DX12_PSO_SLOTS * 2 - 1);
	}
}

static void request(u64 key)
{
	u32 i = dx12_pso_hash(key) * 2;
	for (;;)
	{
		const u64 k = dx12p.requested[i];
		if (k == key)
			return;
		if (k == 0)
			break;
		i = (i + 1) & (DX12_PSO_SLOTS * 2 - 1);
	}
	/* (kept from filling: one not recorded is at worst queued twice) */
	if (dx12p.requestedUsed >= DX12_PSO_SLOTS * 2 * 3 / 4)
		return;
	dx12p.requested[i] = key;
	dx12p.requestedUsed++;
}

/* A frame wants @key, not in the table: what the thread made, then
 * made here if it still is not there */
ID3D12PipelineState *dx12_pso_missed(u64 key)
{
	u32 i;
	take_built();
	i = dx12_pso_hash(key);
	for (;;)
	{
		const dx12_pso_slot *s = &dx12p.slots[i];
		if (s->key == key)
			return s->pso;
		if (s->key == 0)
			break;
		i = (i + 1) & (DX12_PSO_SLOTS - 1);
	}
	request(key);
	return make(key, i, true);
}

bool dx12_pso_made(void)
{
	if (d3d_warm_busy(dx12p.warm))
		return false;
	take_built();
	return true;
}

void dx12_pso_wait_made(void)
{
	d3d_warm_drain_wait(dx12p.warm);
	take_built();
}

static void add(u64 key)
{
	u32 i;
	/* (made here: wanted, whether or not the thread has it queued) */
	request(key);
	i = dx12_pso_hash(key);
	for (;;)
	{
		const dx12_pso_slot *s = &dx12p.slots[i];
		if (s->key == key)
			return;
		if (s->key == 0)
		{
			make(key, i, false);
			return;
		}
		i = (i + 1) & (DX12_PSO_SLOTS - 1);
	}
}

/* @key to be made: queued for the thread, or made here when there is none
 * (and those NVAPI makes, always here) */
static void want(u64 key)
{
	DX12PsoKey k;
	if (requested(key))
		return;
	k = dx12_pso_key_unpack(key);
	if (dx12p.warm == NULL || wants_post_z(&k) || dx12p.batchCount >= BATCH_JOBS)
	{
		add(key);
		return;
	}
	request(key);
	dx12p.batch[dx12p.batchCount].key = key;
	dx12p.batch[dx12p.batchCount].kind = 0;
	dx12p.batch[dx12p.batchCount].aux = 0;
	dx12p.batchCount++;
}

/* What is queued goes to the thread; what it cannot take is made here, as
 * it was before there was one */
static void submit_batch(void)
{
	const size_t taken = dx12p.batchCount == 0 ? 0 : d3d_warm_submit(dx12p.warm, dx12p.batch, dx12p.batchCount);
	size_t i;
	for (i = taken; i < dx12p.batchCount; i++)
		add(dx12p.batch[i].key);
	dx12p.batchCount = 0;
}

void dx12_pso_prepare(const DX12PsoKey *key)
{
	want(dx12_pso_key_pack(key));
	submit_batch();
}

void dx12_pso_prepare_many(const u64 *packed, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
		want(packed[i]);
	submit_batch();
}

/* The thread's: a pipeline made from its key alone (never NVAPI's: those
 * are not queued), through the library, which is the thread's alone while
 * it builds */
static bool dx12_warm_build(void *ctx, const d3d_warm_job_t *job, d3d_warm_result_t *res)
{
	ID3D12PipelineState *pso = NULL;
	u32 flags = 0;
	(void)ctx;
	if (!dx12_pso_build_desc(job->key, false, true, &pso, &flags) || pso == NULL)
		return false;
	res->obj = pso;
	res->aux = flags;
	return true;
}

static void dx12_warm_release(void *ctx, d3d_warm_result_t *res)
{
	(void)ctx;
	if (res->obj != NULL)
		ID3D12PipelineState_Release((ID3D12PipelineState *)res->obj);
	res->obj = NULL;
}

/* The path of @name in the writable data directory (made at the start and
 * the end only) */
static char *data_path(const char *name, const char *suffix)
{
	const size_t n = writable_data_path(NULL, 0, name), s = strlen(suffix);
	char *path = (char *)malloc(n + s + 1);
	if (path == NULL)
		return NULL;
	writable_data_path(path, n + 1, name);
	memcpy(path + n, suffix, s + 1);
	return path;
}

static void load_file(void)
{
	char *path = data_path(PipelineFileName, "");
	void *file = NULL;
	int64_t size = 0;
	const size_t oldHeader = offsetof(DX12PsoFileHeader, shaders);
	if (path != NULL && filestream_read_file(path, &file, &size) && file != NULL && (size_t)size >= oldHeader)
	{
		DX12PsoFileHeader header;
		bool old, keysOnly;
		size_t headerBytes;
		u64 keyBytes;
		memset(&header, 0, sizeof(header));
		memcpy(&header, file, oldHeader);
		old = header.version == 2;
		/* (keys of the layout before this one: taken, their library not) */
		keysOnly = old || header.version < PSO_FILE_VERSION;
		headerBytes = old ? oldHeader : sizeof(header);
		if (!old && (size_t)size >= sizeof(header))
			memcpy(&header, file, sizeof(header));
		keyBytes = (u64)header.keyCount * sizeof(u64);
		if (!memcmp(header.magic, PsoFileMagic, sizeof(PsoFileMagic))
				&& (old || (header.version >= 3 && header.version <= PSO_FILE_VERSION && (size_t)size >= sizeof(header)))
				&& header.keyCount <= DX12_PSO_MAX_KEYS && headerBytes + keyBytes <= (u64)size
				&& header.libraryBytes <= (u64)size - headerBytes - keyBytes)
		{
			const u8 *k = (const u8 *)file + headerBytes;
			u32 i;
			/* (a key of an earlier layout as this one has it: its blend folded) */
			for (i = 0; i < header.keyCount; i++)
			{
				u64 key;
				DX12PsoKey unpacked;
				memcpy(&key, k + (size_t)i * sizeof(u64), sizeof(key));
				if (dx12_pso_key_valid(key))
				{
					unpacked = dx12_pso_key_unpack(key);
					key = dx12_pso_key_pack(&unpacked);
				}
				keep_key(key);
			}
			if (!keysOnly && header.shaders == dx12_shaders_hash())
			{
				const u8 *lib = (const u8 *)file + headerBytes + keyBytes;
				dx12p.libraryBytes = (size_t)header.libraryBytes;
				dx12p.libraryData = (u8 *)malloc(dx12p.libraryBytes + 1);
				if (dx12p.libraryData != NULL)
					memcpy(dx12p.libraryData, lib, dx12p.libraryBytes);
				else
					dx12p.libraryBytes = 0;
			}
			else if (header.libraryBytes != 0)
				INFO_LOG(RENDERER, "DX12: the pipeline library kept is of other shaders: started again\n");
		}
	}
	free(file);
	if (dx12p.device1 != NULL)
	{
		HRESULT hr = E_FAIL;
		if (dx12p.libraryBytes != 0)
			hr = ID3D12Device1_CreatePipelineLibrary(dx12p.device1, dx12p.libraryData, dx12p.libraryBytes, &iid_pipeline_library,
					(void **)&dx12p.library);
		if (FAILED(hr))
		{
			/* (a new driver, or none kept: an empty one) */
			if (dx12p.libraryBytes != 0)
				INFO_LOG(RENDERER, "DX12: the pipeline library kept is not this driver's (%x): started again\n", (unsigned)hr);
			dx12p.library = NULL;
			free(dx12p.libraryData);
			dx12p.libraryData = NULL;
			dx12p.libraryBytes = 0;
			if (FAILED(ID3D12Device1_CreatePipelineLibrary(dx12p.device1, NULL, 0, &iid_pipeline_library, (void **)&dx12p.library)))
				dx12p.library = NULL;
		}
	}
	NOTICE_LOG(RENDERER, "DX12: pipeline file %s: %u keys, library of %u KB%s\n", path != NULL ? path : PipelineFileName,
			(u32)dx12p.keyCount, (u32)(dx12p.libraryBytes / 1024), dx12p.library == NULL ? " (no pipeline library on this device)" : "");
	free(path);
}

static int compare_keys(const void *a, const void *b)
{
	const u64 x = *(const u64 *)a, y = *(const u64 *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

static void save_file(void)
{
	const bool newKeys = dx12p.statMade != dx12p.statFromLibrary || dx12p.storedSinceLoad != 0;
	size_t libBytes, n = 0, i, bytes;
	DX12PsoFileHeader header;
	u8 *file;
	char *path, *tmpPath;
	RFILE *f;
	bool written;
	if (!newKeys && dx12p.statMadeInFrame == 0)
		return;
	qsort(dx12p.keys, dx12p.keyCount, sizeof(u64), compare_keys);
	for (i = 0; i < dx12p.keyCount; i++)
		if (n == 0 || dx12p.keys[i] != dx12p.keys[n - 1])
			dx12p.keys[n++] = dx12p.keys[i];
	dx12p.keyCount = n;
	libBytes = dx12p.library != NULL ? dx12p.library->lpVtbl->GetSerializedSize(dx12p.library) : 0;
	memcpy(header.magic, PsoFileMagic, sizeof(PsoFileMagic));
	header.version = PSO_FILE_VERSION;
	header.keyCount = (u32)n;
	header.shaders = dx12_shaders_hash();
	header.reserved = 0;
	file = (u8 *)malloc(sizeof(header) + n * sizeof(u64) + libBytes + 1);
	if (file == NULL)
		return;
	if (libBytes != 0 && FAILED(dx12p.library->lpVtbl->Serialize(dx12p.library, file + sizeof(header) + n * sizeof(u64), libBytes)))
		libBytes = 0;
	header.libraryBytes = libBytes;
	bytes = sizeof(header) + n * sizeof(u64) + libBytes;
	memcpy(file, &header, sizeof(header));
	if (n != 0)
		memcpy(file + sizeof(header), dx12p.keys, n * sizeof(u64));
	/* (written beside it, then put in its place: a write cut short leaves the one there was) */
	path = data_path(PipelineFileName, "");
	tmpPath = data_path(PipelineFileName, ".tmp");
	f = path != NULL && tmpPath != NULL
			? filestream_open(tmpPath, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE) : NULL;
	if (f != NULL)
	{
		written = filestream_write(f, file, (int64_t)bytes) == (int64_t)bytes;
		filestream_close(f);
		if (written)
		{
			filestream_delete(path);
			written = filestream_rename(tmpPath, path) == 0;
		}
		if (!written)
			filestream_delete(tmpPath);
		else
			NOTICE_LOG(RENDERER, "DX12: pipeline file saved: %u keys, library of %u KB\n", header.keyCount, (u32)(libBytes / 1024));
	}
	free(file);
	free(path);
	free(tmpPath);
}

bool dx12_pso_init(ID3D12Device *device, ID3D12RootSignature *root, UINT vendor)
{
	dx12_pso_term();
	dx12p.device = device;
	dx12p.root = root;
	dx12p.slots = (dx12_pso_slot *)calloc(DX12_PSO_SLOTS, sizeof(dx12_pso_slot));
	dx12p.requested = (u64 *)calloc(DX12_PSO_SLOTS * 2, sizeof(u64));
	dx12p.keys = (u64 *)malloc(DX12_PSO_MAX_KEYS * sizeof(u64));
	dx12p.batch = (d3d_warm_job_t *)malloc(BATCH_JOBS * sizeof(d3d_warm_job_t));
	if (dx12p.slots == NULL || dx12p.requested == NULL || dx12p.keys == NULL || dx12p.batch == NULL)
	{
		dx12_pso_term();
		return false;
	}
	dx12p.requestedUsed = 0;
	dx12p.used = 0;
	dx12p.statAhead = 0;
	dx12p.statMade = dx12p.statMadeInFrame = dx12p.statFromLibrary = 0;
	dx12p.statPostZ = dx12p.statOwnTest = 0;
	/* (NVIDIA's PCI vendor id) */
	dx12p.postZCoverage = vendor == 0x10de && dx12_nvapi_post_z_available();
	dx12p.storedSinceLoad = 0;
	dx12p.fileKeys = 0;
	dx12p.keyCount = 0;
	dx12p.batchCount = 0;
	if (FAILED(ID3D12Device_QueryInterface(device, &iid_device1, (void **)&dx12p.device1)))
		dx12p.device1 = NULL;
	/* (before the file: the library kept is of the shaders chosen) */
	NOTICE_LOG(RENDERER, "DX12: the shaders as %s\n", dx12_shaders_choose(device) ? "DXIL (shader model 6.0)" : "DXBC (shader model 5.1)");
	load_file();
	/* (after the library: the thread uses it) */
	dx12p.warm = d3d_warm_new(dx12_warm_build, dx12_warm_release, NULL, DX12_PSO_MAX_KEYS * 2);
	return true;
}

void dx12_pso_make_at_start(void)
{
	const size_t fromFile = dx12p.keyCount;
	DX12PsoKey k;
	u64 *kept;
	size_t i, queued;
	u32 d, s, cull;
	dx12p.started = false;
	/* What earlier runs made, first: what this game draws with, before what
	 * games mostly do (SSAA's, when it is taken up: dx12_pso_make_for_samples()).
	 * The thread makes them in this order, and a frame that wants one not
	 * made yet makes it itself. */
	dx12p.fileKeys = fromFile;
	kept = (u64 *)malloc((fromFile + 1) * sizeof(u64));
	if (kept != NULL)
	{
		memcpy(kept, dx12p.keys, fromFile * sizeof(u64));
		for (i = 0; i < fromFile; i++)
			if (dx12_pso_key_valid(kept[i]) && dx12_pso_key_unpack(kept[i]).samples == 0)
				want(kept[i]);
		free(kept);
	}
	/* the quads */
	memset(&k, 0, sizeof(k));
	k.kind = DX12_PSO_QUAD;
	k.writeColor = true;
	for (d = 0; d < 2; d++)
		for (s = 0; s < 3; s++)
		{
			k.depthBound = d != 0;
			k.special = s;
			want(dx12_pso_key_pack(&k));
		}
	/* the modifier volumes: the volumes themselves, the area summed, what is left darkened */
	memset(&k, 0, sizeof(k));
	k.depthBound = true;
	k.kind = DX12_PSO_MODVOL;
	for (cull = 0; cull < 3; cull++)
	{
		k.cull = cull;
		k.blend = false;
		k.writeColor = false;
		k.depthTest = true;
		k.depthWrite = false;
		k.depthFunc = 4;
		k.stencil = DX12Stencil_Or;
		want(dx12_pso_key_pack(&k));
		k.stencil = DX12Stencil_Xor;
		want(dx12_pso_key_pack(&k));
		k.depthTest = false;
		k.depthFunc = 7;
		k.stencil = DX12Stencil_Inclusion;
		want(dx12_pso_key_pack(&k));
		k.stencil = DX12Stencil_Exclusion;
		want(dx12_pso_key_pack(&k));
		/* the depth of sorted polygons for a later render pass */
		k.depthTest = true;
		k.depthWrite = true;
		k.depthFunc = 6;
		k.stencil = DX12Stencil_None;
		want(dx12_pso_key_pack(&k));
	}
	memset(&k, 0, sizeof(k));
	k.kind = DX12_PSO_MODVOL;
	k.depthBound = true;
	k.special = DX12_BLEND_ALPHA;
	k.writeColor = true;
	k.depthFunc = 7;
	k.stencil = DX12Stencil_Final;
	want(dx12_pso_key_pack(&k));
	/* (the polygons': every one each renderer can draw with, made by it,
	 * the renderers' PreparePipelines()) */
	queued = dx12p.batchCount;
	submit_batch();
	dx12p.started = true;
	NOTICE_LOG(RENDERER, "DX12: %u pipelines made at the start, %u to be made ahead (%u from the library, %u keys from earlier runs)\n",
			dx12p.statMade, (u32)queued, dx12p.statFromLibrary, (u32)fromFile);
}

void dx12_pso_make_for_samples(u32 log2n, bool hdr)
{
	u32 before, listsBefore, i;
	u64 *single;
	size_t singleCount = 0, j, queued;
	DX12PsoKey r;
	if (log2n == 0 || log2n > 3 || dx12p.slots == NULL)
		return;
	/* (what is made already is found in the table: only what is not gets made) */
	dx12p.started = false;
	before = dx12p.statMade;
	listsBefore = dx12p.statPostZ + dx12p.statOwnTest;
	/* (the keys of one sample made or queued now: what is wanted below is added to them too) */
	single = (u64 *)malloc((dx12p.requestedUsed + 1) * sizeof(u64));
	if (single != NULL)
	{
		for (i = 0; i < DX12_PSO_SLOTS * 2; i++)
			if (dx12p.requested[i] != 0 && dx12_pso_key_unpack(dx12p.requested[i]).samples == 0
					&& singleCount <= dx12p.requestedUsed)
				single[singleCount++] = dx12p.requested[i];
		qsort(single, singleCount, sizeof(u64), compare_keys);
	}
	for (j = 0; j < singleCount; j++)
	{
		DX12PsoKey k = dx12_pso_key_unpack(single[j]);
		k.samples = log2n;
		switch (k.kind)
		{
		case DX12_PSO_POLY:
			k.rate = DX12_RATE_SAMPLE;
			want(dx12_pso_key_pack(&k));
			k.rate = DX12_RATE_PIXEL;
			want(dx12_pso_key_pack(&k));
			break;
		case DX12_PSO_OIT:
			if (k.pass != DX12_PASS_OIT)
			{
				k.rate = DX12_RATE_SAMPLE;
				want(dx12_pso_key_pack(&k));
			}
			k.rate = DX12_RATE_PIXEL;
			want(dx12_pso_key_pack(&k));
			break;
		case DX12_PSO_OIT_FINAL:
			/* into the next render pass's samples; resolved into the picture, of one sample */
			k.rate = DX12_RATE_SAMPLE;
			want(dx12_pso_key_pack(&k));
			k.samples = 0;
			k.rate = DX12_RATE_PIXEL;
			want(dx12_pso_key_pack(&k));
			if (hdr)
			{
				k.rate = DX12_RATE_PIXEL_SUMS;
				want(dx12_pso_key_pack(&k));
			}
			break;
		case DX12_PSO_OIT_MODVOL:
			k.rate = DX12_RATE_PIXEL;
			want(dx12_pso_key_pack(&k));
			break;
		case DX12_PSO_SSAA_RESOLVE:
		case DX12_PSO_MOTION:	/* (never with SSAA: the upscaler is the anti-aliasing then) */
			break;
		default:
			/* the volumes, the quads: shaded once a pixel, written to every sample covered */
			want(dx12_pso_key_pack(&k));
			break;
		}
	}
	free(single);
	/* the resolve, into the screen's picture */
	memset(&r, 0, sizeof(r));
	r.kind = DX12_PSO_SSAA_RESOLVE;
	r.writeColor = true;
	r.depthFunc = 7;
	want(dx12_pso_key_pack(&r));
	/* what earlier runs made with this many */
	for (j = 0; j < dx12p.fileKeys && j < dx12p.keyCount; j++)
		if (dx12_pso_key_valid(dx12p.keys[j]) && dx12_pso_key_unpack(dx12p.keys[j]).samples == log2n)
			want(dx12p.keys[j]);
	queued = dx12p.batchCount;
	submit_batch();
	dx12p.started = true;
	NOTICE_LOG(RENDERER, "DX12: %u pipelines made for SSAA %u, %u to be made ahead (%u from library)\n",
			dx12p.statMade - before, 1u << log2n, (u32)queued, dx12p.statFromLibrary);
	if (dx12p.statPostZ + dx12p.statOwnTest != listsBefore)
		NOTICE_LOG(RENDERER, "DX12 per-pixel renderer: with SSAA, a translucent fragment takes the samples %s\n",
				dx12p.statPostZ != 0 && dx12p.postZCoverage ? "the depth test passed (NVAPI post-depth coverage)"
				: "its own depth test passes, at each sample");
}

void dx12_pso_term(void)
{
	u32 i;
	/* (the thread stopped first: what it made is kept, and the library is ours again) */
	if (dx12p.warm != NULL)
	{
		d3d_warm_stop(dx12p.warm);
		if (dx12p.slots != NULL)
			take_built();
		d3d_warm_free(dx12p.warm);
		dx12p.warm = NULL;
	}
	dx12p.batchCount = 0;
	if (dx12p.slots != NULL)
	{
		NOTICE_LOG(RENDERER, "DX12: %u pipelines made, %u of them in a frame, %u ahead, %u from the library\n",
				dx12p.statMade, dx12p.statMadeInFrame, dx12p.statAhead, dx12p.statFromLibrary);
		if (dx12p.keys != NULL)
			save_file();
		for (i = 0; i < DX12_PSO_SLOTS; i++)
			if (dx12p.slots[i].pso != NULL)
				ID3D12PipelineState_Release(dx12p.slots[i].pso);
		free(dx12p.slots);
		dx12p.slots = NULL;
	}
	free(dx12p.requested);
	dx12p.requested = NULL;
	free(dx12p.batch);
	dx12p.batch = NULL;
	if (dx12p.library != NULL)
		dx12p.library->lpVtbl->Release(dx12p.library);
	dx12p.library = NULL;
	free(dx12p.libraryData);
	dx12p.libraryData = NULL;
	dx12p.libraryBytes = 0;
	if (dx12p.device1 != NULL)
		ID3D12Device1_Release(dx12p.device1);
	dx12p.device1 = NULL;
	free(dx12p.keys);
	dx12p.keys = NULL;
	dx12p.keyCount = 0;
	dx12p.used = 0;
	dx12p.device = NULL;
	dx12p.root = NULL;
	dx12p.started = false;
}
