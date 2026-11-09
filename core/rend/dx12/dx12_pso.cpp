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
#include "dx12context_lr.h"
#include "dx12_shaders.h"
#include "dx12_nvapi.h"
#include "hw/pvr/ta_ctx.h"
#include <streams/file_stream.h>
#include <algorithm>
#include <cstdio>
#include <string>

extern std::string get_writable_data_path(const std::string& filename);

static const char *PipelineFileName = "d3d12_pipelines.bin";

/* The file: this header, the keys, then the library's data. The library
 * is only of the shaders it was made with: a pipeline's description names
 * its bytecode, and one kept for other bytecode is never loaded again, nor
 * can one of the same name be stored. */
struct DX12PsoFileHeader
{
	char magic[8];
	u32 version;	// of the keys' layout (DX12PsoKey)
	u32 keyCount;
	u64 libraryBytes;
	// (from version 3)
	u32 shaders;	// dx12_shaders_hash() of the library's pipelines
	u32 reserved;
};
static const char PsoFileMagic[8] = { 'F', 'C', 'D', '1', '2', 'P', 'S', 'O' };
/* 3: a polygon's blend folded into its key (DX12PsoKey::Pack()); 4: its
 * cull mode too; 5: its shading. A file of 2 to 4 gives its keys, folded,
 * and no library */
static const u32 PsoFileVersion = 5;
static const u32 MaxKeys = 8192;

/* What a build did (DX12Pipelines::BuildDesc()): counted by the thread
 * that renders */
enum
{
	DX12_BUILT_LIBRARY = 1,		// handed back by the library
	DX12_BUILT_STORED = 2,		// stored in it
	DX12_BUILT_POSTZ = 4,		// with NVAPI's post-depth coverage
	DX12_BUILT_OWNTEST = 8,		// SSAA's lists testing their samples themselves
	DX12_BUILT_NOPOSTZ = 16,	// NVAPI made none: it is not asked again
};

static const D3D12_COMPARISON_FUNC depthFuncs[] =
{
	D3D12_COMPARISON_FUNC_NEVER,			//0	Never
	D3D12_COMPARISON_FUNC_LESS,				//1	Less
	D3D12_COMPARISON_FUNC_EQUAL,			//2	Equal
	D3D12_COMPARISON_FUNC_LESS_EQUAL,		//3	Less Or Equal
	D3D12_COMPARISON_FUNC_GREATER,			//4	Greater
	D3D12_COMPARISON_FUNC_NOT_EQUAL,		//5	Not Equal
	D3D12_COMPARISON_FUNC_GREATER_EQUAL,	//6	Greater Or Equal
	D3D12_COMPARISON_FUNC_ALWAYS,			//7	Always
};

static const D3D12_INPUT_ELEMENT_DESC MainLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(Vertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, vtx_spc), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, (UINT)offsetof(Vertex, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};
/* (a position alone: the modifier volumes' triangles, and the polygons'
 * vertices read for their position only - the stride is the view's) */
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
	// (a translucent strip's number, an instance's: in the second vertex buffer)
	{ "POLYNUMBER", 0, DXGI_FORMAT_R32_UINT,      1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
};
/* (the motion vector pass's: the position, and where the vertex was, in
 * its own stream) */
static const D3D12_INPUT_ELEMENT_DESC MotionLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(Vertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "PREVPOS",  0, DXGI_FORMAT_R32G32_FLOAT,    1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};
static const D3D12_INPUT_ELEMENT_DESC PositionLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

static void stencilDesc(DX12Stencil stencil, D3D12_DEPTH_STENCIL_DESC& desc)
{
	desc.StencilEnable = stencil != DX12Stencil::None;
	desc.StencilReadMask = 0xff;
	desc.StencilWriteMask = 0xff;
	D3D12_DEPTH_STENCILOP_DESC& op = desc.FrontFace;
	op.StencilFailOp = op.StencilDepthFailOp = op.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	op.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	// (the reference is set with the draw, OMSetStencilRef())
	switch (stencil)
	{
	case DX12Stencil::None:
	case DX12Stencil::Count:
		break;
	case DX12Stencil::Shadow:
		op.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		desc.StencilReadMask = 0;
		desc.StencilWriteMask = 0x80;
		break;
	case DX12Stencil::ShadowLaterWins:
		op.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		op.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc.StencilReadMask = 0x81;
		desc.StencilWriteMask = 0x40;
		break;
	case DX12Stencil::ShadowEarlierWins:
		op.StencilPassOp = D3D12_STENCIL_OP_INVERT;
		op.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc.StencilReadMask = 0xC1;
		desc.StencilWriteMask = 0x40;
		break;
	case DX12Stencil::Xor:
		op.StencilPassOp = D3D12_STENCIL_OP_INVERT;
		desc.StencilReadMask = 0;
		desc.StencilWriteMask = 2;
		break;
	case DX12Stencil::Or:
		op.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		desc.StencilReadMask = 2;
		desc.StencilWriteMask = 2;
		break;
	case DX12Stencil::Inclusion:
		op.StencilFailOp = op.StencilDepthFailOp = D3D12_STENCIL_OP_ZERO;
		op.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		op.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.StencilReadMask = 3;
		desc.StencilWriteMask = 3;
		break;
	case DX12Stencil::Exclusion:
		op.StencilFailOp = op.StencilDepthFailOp = D3D12_STENCIL_OP_ZERO;
		op.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc.StencilReadMask = 3;
		desc.StencilWriteMask = 3;
		break;
	case DX12Stencil::Final:
		// takes shadows, in a volume, and not drawn again for it (bit 6);
		// clears the volume bits and that mark
		op.StencilFailOp = op.StencilDepthFailOp = op.StencilPassOp = D3D12_STENCIL_OP_ZERO;
		op.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc.StencilReadMask = 0xC1;
		desc.StencilWriteMask = 0x43;
		break;
	}
	desc.BackFace = desc.FrontFace;
}

bool DX12Pipelines::BuildDesc(u64 packed, bool postZCoverage, bool useLibrary, ID3D12PipelineState **out, u32 *flags) const
{
	const DX12PsoKey key = DX12PsoKey::Unpack(packed);
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = root;
	DX12Shader vs, ps;
	bool postZ = false;
	*flags = 0;
	/* SSAA's shaders: by rate (dx12_shaders.h) */
	const int rateShader = key.rate == DX12_RATE_PIXEL ? 1 : 0;
	switch (key.kind)
	{
	case DX12_PSO_POLY:
		vs = DX12Shader::PolyVS;
		ps = DX12Shader::PolyPS;
		if (key.rate != DX12_RATE_NONE)
			ps = (DX12Shader)((int)DX12Shader::SsaaPolyPS + rateShader);
		desc.InputLayout = { MainLayout, (UINT)ARRAY_SIZE(MainLayout) };
		break;
	case DX12_PSO_MODVOL:
		vs = DX12Shader::ModVolVS;
		ps = DX12Shader::ModVolPS;
		desc.InputLayout = { PositionLayout, (UINT)ARRAY_SIZE(PositionLayout) };
		break;
	case DX12_PSO_OIT:
		vs = DX12Shader::OITVS;
		ps = (DX12Shader)((int)DX12Shader::OITDepthPS + (int)std::min<u32>(key.pass, DX12_PASS_OIT));
		if (key.rate != DX12_RATE_NONE)
		{
			// (the lists' are once a pixel only)
			ps = key.pass >= DX12_PASS_OIT ? DX12Shader::SsaaOITListPS
					: (DX12Shader)((int)DX12Shader::SsaaOITDepthPS + 2 * (int)key.pass + rateShader);
			/* The lists' with the device's own depth test at each sample,
			 * where NVAPI makes the pipeline with post-depth coverage: the
			 * pass tests greater or equal against the opaque depth,
			 * read-only, and the shader takes the samples that passed
			 * instead of testing them itself, a few ulp off */
			postZ = postZCoverage && key.pass >= DX12_PASS_OIT && key.samples != 0 && key.depthBound && key.depthTest
					&& !key.depthWrite && key.depthFunc == 6;
			if (postZ)
				ps = DX12Shader::SsaaOITListPostZPS;
		}
		desc.InputLayout = { OITLayout, (UINT)ARRAY_SIZE(OITLayout) };
		break;
	case DX12_PSO_OIT_FINAL:
		vs = DX12Shader::OITFinalVS;
		ps = key.rate == DX12_RATE_PIXEL ? DX12Shader::SsaaOITFinalPS : key.rate == DX12_RATE_SAMPLE ? DX12Shader::SsaaOITFinalSamplePS
				: DX12Shader::OITFinalPS;
		break;
	case DX12_PSO_OIT_MODVOL:
		vs = DX12Shader::ModVolVS;
		ps = key.rate != DX12_RATE_NONE ? DX12Shader::SsaaOITModVolPS : DX12Shader::OITModVolPS;
		desc.InputLayout = { PositionLayout, (UINT)ARRAY_SIZE(PositionLayout) };
		break;
	case DX12_PSO_SSAA_RESOLVE:
		vs = DX12Shader::OITFinalVS;
		ps = DX12Shader::SsaaResolvePS;
		break;
	case DX12_PSO_MOTION:
		if (key.pass == 1)
		{
			vs = DX12Shader::QuadVS;
			ps = DX12Shader::DepthCopyPS;
			break;
		}
		vs = DX12Shader::MotionVS;
		// (with no depth bound, the compute renderer's frame: the shader tests the depth it stored)
		ps = key.depthBound ? DX12Shader::MotionPS : DX12Shader::MotionSampledPS;
		desc.InputLayout = { MotionLayout, (UINT)ARRAY_SIZE(MotionLayout) };
		break;
	default:
		vs = DX12Shader::QuadVS;
		ps = DX12Shader::QuadPS;
		break;
	}
	desc.VS = dx12_shader(vs);
	desc.PS = dx12_shader(ps);
	// (the HDR output's, of its own bytecode)
	if (key.kind == DX12_PSO_OIT_FINAL && key.rate == DX12_RATE_PIXEL_SUMS)
		desc.PS = dx12_hdr_shader(DX12HdrShader::SsaaOITFinalSumsPS);

	D3D12_RENDER_TARGET_BLEND_DESC& rt = desc.BlendState.RenderTarget[0];
	rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt.LogicOp = D3D12_LOGIC_OP_NOOP;
	rt.RenderTargetWriteMask = key.writeColor ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
	if (key.special == DX12_BLEND_ALPHA)
	{
		rt.BlendEnable = TRUE;
		rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
		rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	}
	else if (key.special == DX12_BLEND_FACTOR)
	{
		rt.BlendEnable = TRUE;
		rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
		rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
	}
	else
		dx12_blend_desc(key, rt);
	desc.SampleMask = UINT_MAX;

	D3D12_RASTERIZER_DESC& rs = desc.RasterizerState;
	rs.FillMode = D3D12_FILL_MODE_SOLID;
	rs.CullMode = key.cull == 1 ? D3D12_CULL_MODE_FRONT : key.cull == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
	rs.FrontCounterClockwise = TRUE;
	// (the depth is the shaders' own, the clip space's is always 1)
	rs.DepthClipEnable = key.kind == DX12_PSO_QUAD;

	D3D12_DEPTH_STENCIL_DESC& ds = desc.DepthStencilState;
	ds.DepthEnable = key.depthTest;
	ds.DepthWriteMask = key.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	ds.DepthFunc = depthFuncs[key.depthFunc & 7];
	stencilDesc(key.stencil, ds);
	if (!key.depthBound)
	{
		ds.DepthEnable = FALSE;
		ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		ds.StencilEnable = FALSE;
	}

	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = key.kind != DX12_PSO_MOTION ? DX12FrameFormat : key.pass == 1 ? DX12UpscalerDepthFormat : DX12MotionFormat;
	desc.DSVFormat = key.depthBound ? DX12DepthFormat : DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1u << key.samples;
	// (SSAA's samples where rend/ssaa_pattern.h has them)
	desc.SampleDesc.Quality = key.samples != 0 ? DX12StandardPattern : 0;

	if (postZ)
	{
		// (not through the library: NVAPI's pipelines have no cached blob)
		if (dx12_nvapi_create_post_z_pipeline(device, &desc, out))
		{
			*flags |= DX12_BUILT_POSTZ;
			return true;
		}
		*flags |= DX12_BUILT_NOPOSTZ;
		desc.PS = dx12_shader(DX12Shader::SsaaOITListPS);
	}
	if (key.kind == DX12_PSO_OIT && key.pass >= DX12_PASS_OIT && key.rate != DX12_RATE_NONE)
		*flags |= DX12_BUILT_OWNTEST;

	wchar_t name[24];
	swprintf(name, 24, L"fc%016llx", (unsigned long long)packed);
	if (useLibrary && library != nullptr
			&& SUCCEEDED(library->LoadGraphicsPipeline(name, &desc, __uuidof(ID3D12PipelineState), (void **)out)))
	{
		*flags |= DX12_BUILT_LIBRARY;
		return true;
	}
	if (FAILED(device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), (void **)out)))
	{
		*out = nullptr;
		return false;
	}
	if (useLibrary && library != nullptr && SUCCEEDED(library->StorePipeline(name, *out)))
		*flags |= DX12_BUILT_STORED;
	return true;
}

void DX12Pipelines::Count(u32 flags)
{
	if (flags & DX12_BUILT_LIBRARY)
		statFromLibrary++;
	if (flags & DX12_BUILT_STORED)
		storedSinceLoad++;
	if (flags & DX12_BUILT_POSTZ)
		statPostZ++;
	if (flags & DX12_BUILT_OWNTEST)
		statOwnTest++;
}

/* (the thread that renders: the library is its own when the thread ahead
 * is not building) */
bool DX12Pipelines::Build(u64 packed, ID3D12PipelineState **out)
{
	u32 flags;
	const bool ok = BuildDesc(packed, postZCoverage, !d3d_warm_busy(warm), out, &flags);
	if (flags & DX12_BUILT_NOPOSTZ)
	{
		WARN_LOG(RENDERER, "DX12: NVAPI made no pipeline with post-depth coverage: SSAA's lists test their samples themselves");
		postZCoverage = false;
	}
	Count(flags);
	return ok;
}

/* The lists' pipelines NVAPI would make: never the thread's to make */
bool DX12Pipelines::WantsPostZ(const DX12PsoKey& key) const
{
	return postZCoverage && key.kind == DX12_PSO_OIT && key.rate != DX12_RATE_NONE && key.pass >= DX12_PASS_OIT
			&& key.samples != 0 && key.depthBound && key.depthTest && !key.depthWrite && key.depthFunc == 6;
}

ID3D12PipelineState *DX12Pipelines::Make(u64 key, u32 slot, bool inFrame)
{
	ID3D12PipelineState *pso = nullptr;
	if (used >= SLOTS * 3 / 4 || !Build(key, &pso))
	{
		WARN_LOG(RENDERER, "DX12: pipeline %016llx could not be made", (unsigned long long)key);
		return nullptr;
	}
	slots[slot].key = key;
	slots[slot].pso = pso;
	used++;
	statMade++;
	if (inFrame && started)
	{
		statMadeInFrame++;
		NOTICE_LOG(RENDERER, "DX12: pipeline %016llx made in a frame", (unsigned long long)key);
	}
	if (keys.size() < MaxKeys)
		keys.push_back(key);
	return pso;
}

/* A frame wants @key, not in the table: what the thread made, then
 * made here if it still is not there */
ID3D12PipelineState *DX12Pipelines::Missed(u64 key)
{
	TakeBuilt();
	u32 i = Hash(key);
	for (;;)
	{
		Slot& s = slots[i];
		if (s.key == key)
			return s.pso;
		if (s.key == 0)
			break;
		i = (i + 1) & (SLOTS - 1);
	}
	Request(key);
	return Make(key, i, true);
}

void DX12Pipelines::TakeBuilt()
{
	d3d_warm_result_t r;
	while (d3d_warm_take(warm, &r))
		Insert(r.key, (ID3D12PipelineState *)r.obj, r.aux);
}

void DX12Pipelines::Insert(u64 key, ID3D12PipelineState *pso, u32 flags)
{
	u32 i = Hash(key);
	for (;;)
	{
		Slot& s = slots[i];
		if (s.key == key)
		{
			// (made here meanwhile)
			pso->Release();
			return;
		}
		if (s.key == 0)
			break;
		i = (i + 1) & (SLOTS - 1);
	}
	if (used >= SLOTS * 3 / 4)
	{
		pso->Release();
		return;
	}
	slots[i].key = key;
	slots[i].pso = pso;
	used++;
	statMade++;
	statAhead++;
	Count(flags);
	if (keys.size() < MaxKeys)
		keys.push_back(key);
}

bool DX12Pipelines::Requested(u64 key)
{
	u32 i = Hash(key) * 2;
	for (;;)
	{
		const u64 k = requested[i];
		if (k == key)
			return true;
		if (k == 0)
			return false;
		i = (i + 1) & (SLOTS * 2 - 1);
	}
}

void DX12Pipelines::Request(u64 key)
{
	u32 i = Hash(key) * 2;
	for (;;)
	{
		const u64 k = requested[i];
		if (k == key)
			return;
		if (k == 0)
			break;
		i = (i + 1) & (SLOTS * 2 - 1);
	}
	// (kept from filling: one not recorded is at worst queued twice)
	if (requestedUsed >= SLOTS * 2 * 3 / 4)
		return;
	requested[i] = key;
	requestedUsed++;
}

/* @key to be made: queued for the thread, or made here when there is none
 * (and those NVAPI makes, always here) */
void DX12Pipelines::Want(u64 key)
{
	if (Requested(key))
		return;
	if (warm == nullptr || WantsPostZ(DX12PsoKey::Unpack(key)))
	{
		Add(key);
		return;
	}
	Request(key);
	d3d_warm_job_t job;
	job.key = key;
	job.kind = 0;
	job.aux = 0;
	batch.push_back(job);
}

/* What is queued goes to the thread; what it cannot take is made here, as
 * it was before there was one */
void DX12Pipelines::SubmitBatch()
{
	const size_t taken = batch.empty() ? 0 : d3d_warm_submit(warm, batch.data(), batch.size());
	for (size_t i = taken; i < batch.size(); i++)
		Add(batch[i].key);
	batch.clear();
}

void DX12Pipelines::Add(u64 key)
{
	// (made here: wanted, whether or not the thread has it queued)
	Request(key);
	u32 i = Hash(key);
	for (;;)
	{
		Slot& s = slots[i];
		if (s.key == key)
			return;
		if (s.key == 0)
		{
			Make(key, i, false);
			return;
		}
		i = (i + 1) & (SLOTS - 1);
	}
}

/* The thread's: a pipeline made from its key alone (never NVAPI's: those
 * are not queued), through the library, which is the thread's alone while
 * it builds */
static bool dx12WarmBuild(void *ctx, const d3d_warm_job_t *job, d3d_warm_result_t *res)
{
	const DX12Pipelines *p = (const DX12Pipelines *)ctx;
	ID3D12PipelineState *pso = nullptr;
	u32 flags = 0;
	if (!p->BuildDesc(job->key, false, true, &pso, &flags) || pso == nullptr)
		return false;
	res->obj = pso;
	res->aux = flags;
	return true;
}

static void dx12WarmRelease(void *ctx, d3d_warm_result_t *res)
{
	(void)ctx;
	if (res->obj != nullptr)
		((ID3D12PipelineState *)res->obj)->Release();
	res->obj = nullptr;
}

bool DX12Pipelines::Init(ID3D12Device *device, ID3D12RootSignature *root, UINT vendor)
{
	Term();
	this->device = device;
	this->root = root;
	slots = new Slot[SLOTS]();
	requested = new u64[SLOTS * 2]();
	requestedUsed = 0;
	used = 0;
	statAhead = 0;
	statMade = statMadeInFrame = statFromLibrary = 0;
	statPostZ = statOwnTest = 0;
	// (NVIDIA's PCI vendor id)
	postZCoverage = vendor == 0x10de && dx12_nvapi_post_z_available();
	storedSinceLoad = 0;
	fileKeys = 0;
	keys.clear();
	keys.reserve(MaxKeys);
	if (FAILED(device->QueryInterface(__uuidof(ID3D12Device1), (void **)&device1)))
		device1 = nullptr;
	// (before the file: the library kept is of the shaders chosen)
	NOTICE_LOG(RENDERER, "DX12: the shaders as %s", dx12_shaders_choose(device) ? "DXIL (shader model 6.0)" : "DXBC (shader model 5.1)");
	LoadFile();
	// (after the library: the thread uses it)
	warm = d3d_warm_new(dx12WarmBuild, dx12WarmRelease, this, MaxKeys * 2);
	batch.reserve(1024);
	return true;
}

void DX12Pipelines::MakeAtStart()
{
	started = false;
	const size_t fromFile = keys.size();
	DX12PsoKey k{};
	/* What earlier runs made, first: what this game draws with, before what
	 * games mostly do (SSAA's, when it is taken up: MakeForSamples()). The
	 * thread makes them in this order, and a frame that wants one not made
	 * yet makes it itself. */
	fileKeys = fromFile;
	std::vector<u64> kept(keys.begin(), keys.begin() + fromFile);
	for (u64 key : kept)
		if (DX12PsoKey::Valid(key) && DX12PsoKey::Unpack(key).samples == 0)
			Want(key);
	// the quads
	k = DX12PsoKey{};
	k.kind = DX12_PSO_QUAD;
	k.writeColor = true;
	for (u32 d = 0; d < 2; d++)
		for (u32 s = 0; s < 3; s++)
		{
			k.depthBound = d != 0;
			k.special = s;
			Want(k.Pack());
		}
	// the modifier volumes: the volumes themselves, the area summed, what is left darkened
	k = DX12PsoKey{};
	k.depthBound = true;
	k.kind = DX12_PSO_MODVOL;
	for (u32 cull = 0; cull < 3; cull++)
	{
		k.cull = cull;
		k.blend = false;
		k.writeColor = false;
		k.depthTest = true;
		k.depthWrite = false;
		k.depthFunc = 4;
		k.stencil = DX12Stencil::Or;
		Want(k.Pack());
		k.stencil = DX12Stencil::Xor;
		Want(k.Pack());
		k.depthTest = false;
		k.depthFunc = 7;
		k.stencil = DX12Stencil::Inclusion;
		Want(k.Pack());
		k.stencil = DX12Stencil::Exclusion;
		Want(k.Pack());
		// the depth of sorted polygons for a later render pass
		k.depthTest = true;
		k.depthWrite = true;
		k.depthFunc = 6;
		k.stencil = DX12Stencil::None;
		Want(k.Pack());
	}
	k = DX12PsoKey{};
	k.kind = DX12_PSO_MODVOL;
	k.depthBound = true;
	k.special = DX12_BLEND_ALPHA;
	k.writeColor = true;
	k.depthFunc = 7;
	k.stencil = DX12Stencil::Final;
	Want(k.Pack());
	// the depth prepass of sorted polygons
	k = DX12PsoKey{};
	k.kind = DX12_PSO_POLY;
	k.depthBound = true;
	k.depthTest = true;
	k.depthWrite = true;
	k.depthFunc = 6;
	for (u32 g = 0; g < 2; g++)
		for (u32 cull = 0; cull < 3; cull++)
		{
			k.gouraud = g != 0;
			k.cull = cull;
			Want(k.Pack());
		}
	/* The polygons' pipelines games mostly draw with (DX12Renderer::DrawPoly()),
	 * the commonest first: opaque ones with every depth mode, punch-through
	 * and translucent ones with the blends that read nothing of the frame
	 * (one pipeline, dx12_src_folds()), translucent ones with every depth
	 * mode for those, and the shadow passes. Then the other 14 blends a
	 * pipeline can have, one pair of instructions standing for each. A
	 * game's others (a blended opaque polygon, an unusual depth mode with a
	 * blend that reads the frame) are made as it first draws with them, and
	 * are in the key log for its next run. */
	static const u8 srcs[] = { 1, 2, 3, 6, 7 };
	static const u8 dsts[] = { 0, 6, 7 };
	for (u32 rare = 0; rare < 2; rare++)
		for (u32 g = 0; g < 2; g++)
			for (u32 cull = 0; cull < 3; cull++)
			{
				k = DX12PsoKey{};
				k.kind = DX12_PSO_POLY;
				k.gouraud = g != 0;
				k.cull = cull;
				k.depthBound = true;
				k.depthTest = true;
				k.writeColor = true;
				if (!rare)
				{
					// opaque: no blend, the shadow stencil
					k.stencil = DX12Stencil::Shadow;
					for (u32 func = 1; func < 8; func++)
						for (u32 w = 0; w < 2; w++)
						{
							k.depthFunc = func;
							k.depthWrite = w != 0;
							Want(k.Pack());
						}
					// the shadow passes: where the polygon is (equal), nothing written but the colour
					k.depthWrite = false;
					k.depthFunc = 2;
					k.stencil = DX12Stencil::ShadowLaterWins;
					Want(k.Pack());
					k.stencil = DX12Stencil::ShadowEarlierWins;
					Want(k.Pack());
				}
				k.blend = true;
				for (const u8 s : srcs)
					for (const u8 d : dsts)
					{
						// (the blends that read nothing of the frame first, the others after)
						if ((s == 1 && d == 0) == (rare != 0))
							continue;
						k.src = s;
						k.dst = d;
						// punch-through: blended, depth written, greater or equal
						k.stencil = DX12Stencil::Shadow;
						k.depthFunc = 6;
						k.depthWrite = true;
						Want(k.Pack());
						// translucent, sorted: no depth write; and unsorted with the depth modes seen most
						k.stencil = DX12Stencil::None;
						k.depthWrite = false;
						Want(k.Pack());
						k.depthFunc = 7;
						Want(k.Pack());
						k.depthWrite = true;
						Want(k.Pack());
						k.depthFunc = 6;
						Want(k.Pack());
					}
				// translucent, unsorted, with every depth mode: the blends that read nothing of the frame
				if (!rare)
				{
					k.src = 1;
					k.dst = 0;
					k.stencil = DX12Stencil::None;
					for (u32 func = 1; func < 8; func++)
						for (u32 w = 0; w < 2; w++)
						{
							k.depthFunc = func;
							k.depthWrite = w != 0;
							Want(k.Pack());
						}
				}
			}
	const u32 queued = (u32)batch.size();
	SubmitBatch();
	started = true;
	NOTICE_LOG(RENDERER, "DX12: %u pipelines made at the start, %u to be made ahead (%u from the library, %u keys from earlier runs)",
			statMade, queued, statFromLibrary, (u32)fromFile);
}

void DX12Pipelines::MakeForSamples(u32 log2n, bool hdr)
{
	if (log2n == 0 || log2n > 3 || slots == nullptr)
		return;
	// (what is made already is found in the table: only what is not gets made)
	started = false;
	const u32 before = statMade;
	const u32 listsBefore = statPostZ + statOwnTest;
	// (the keys of one sample made or queued now: what is wanted below is added to them too)
	std::vector<u64> single;
	single.reserve(used);
	for (u32 i = 0; i < SLOTS * 2; i++)
		if (requested[i] != 0 && DX12PsoKey::Unpack(requested[i]).samples == 0)
			single.push_back(requested[i]);
	std::sort(single.begin(), single.end());
	for (u64 packed : single)
	{
		DX12PsoKey k = DX12PsoKey::Unpack(packed);
		k.samples = log2n;
		switch (k.kind)
		{
		case DX12_PSO_POLY:
			k.rate = DX12_RATE_SAMPLE;
			Want(k.Pack());
			k.rate = DX12_RATE_PIXEL;
			Want(k.Pack());
			break;
		case DX12_PSO_OIT:
			if (k.pass != DX12_PASS_OIT)
			{
				k.rate = DX12_RATE_SAMPLE;
				Want(k.Pack());
			}
			k.rate = DX12_RATE_PIXEL;
			Want(k.Pack());
			break;
		case DX12_PSO_OIT_FINAL:
			// into the next render pass's samples; resolved into the picture, of one sample
			k.rate = DX12_RATE_SAMPLE;
			Want(k.Pack());
			k.samples = 0;
			k.rate = DX12_RATE_PIXEL;
			Want(k.Pack());
			if (hdr)
			{
				k.rate = DX12_RATE_PIXEL_SUMS;
				Want(k.Pack());
			}
			break;
		case DX12_PSO_OIT_MODVOL:
			k.rate = DX12_RATE_PIXEL;
			Want(k.Pack());
			break;
		case DX12_PSO_SSAA_RESOLVE:
		case DX12_PSO_MOTION:	// (never with SSAA: the upscaler is the anti-aliasing then)
			break;
		default:
			// the volumes, the quads: shaded once a pixel, written to every sample covered
			Want(k.Pack());
			break;
		}
	}
	// the resolve, into the screen's picture
	DX12PsoKey r{};
	r.kind = DX12_PSO_SSAA_RESOLVE;
	r.writeColor = true;
	r.depthFunc = 7;
	Want(r.Pack());
	// what earlier runs made with this many
	for (size_t i = 0; i < fileKeys && i < keys.size(); i++)
		if (DX12PsoKey::Valid(keys[i]) && DX12PsoKey::Unpack(keys[i]).samples == log2n)
			Want(keys[i]);
	const u32 queued = (u32)batch.size();
	SubmitBatch();
	started = true;
	NOTICE_LOG(RENDERER, "DX12: %u pipelines made for SSAA %u, %u to be made ahead (%u from library)", statMade - before, 1u << log2n, queued, statFromLibrary);
	if (statPostZ + statOwnTest != listsBefore)
		NOTICE_LOG(RENDERER, "DX12 per-pixel renderer: with SSAA, a translucent fragment takes the samples %s",
				statPostZ != 0 && postZCoverage ? "the depth test passed (NVAPI post-depth coverage)" : "its own depth test passes, at each sample");
}

void DX12Pipelines::LoadFile()
{
	const std::string path = get_writable_data_path(PipelineFileName);
	void *file = nullptr;
	int64_t size = 0;
	const size_t oldHeader = offsetof(DX12PsoFileHeader, shaders);
	if (filestream_read_file(path.c_str(), &file, &size) && file != nullptr && (size_t)size >= oldHeader)
	{
		DX12PsoFileHeader header{};
		memcpy(&header, file, oldHeader);
		const bool old = header.version == 2;
		// (keys of the layout before this one: taken, their library not)
		const bool keysOnly = old || header.version == 3 || header.version == 4;
		const size_t headerBytes = old ? oldHeader : sizeof(header);
		if (!old && (size_t)size >= sizeof(header))
			memcpy(&header, file, sizeof(header));
		const u64 keyBytes = (u64)header.keyCount * sizeof(u64);
		if (!memcmp(header.magic, PsoFileMagic, sizeof(PsoFileMagic))
				&& (old || (header.version >= 3 && header.version <= PsoFileVersion && (size_t)size >= sizeof(header)))
				&& header.keyCount <= MaxKeys && headerBytes + keyBytes <= (u64)size
				&& header.libraryBytes <= (u64)size - headerBytes - keyBytes)
		{
			const u64 *k = (const u64 *)((const u8 *)file + headerBytes);
			// (a key of an earlier layout as this one has it: its blend folded)
			for (u32 i = 0; i < header.keyCount; i++)
				keys.push_back(DX12PsoKey::Valid(k[i]) ? DX12PsoKey::Unpack(k[i]).Pack() : k[i]);
			if (!keysOnly && header.shaders == dx12_shaders_hash())
			{
				const u8 *lib = (const u8 *)file + headerBytes + keyBytes;
				libraryData.assign(lib, lib + header.libraryBytes);
			}
			else if (header.libraryBytes != 0)
				INFO_LOG(RENDERER, "DX12: the pipeline library kept is of other shaders: started again");
		}
	}
	free(file);
	if (device1 != nullptr)
	{
		HRESULT hr = E_FAIL;
		if (!libraryData.empty())
			hr = device1->CreatePipelineLibrary(libraryData.data(), libraryData.size(), __uuidof(ID3D12PipelineLibrary), (void **)&library);
		if (FAILED(hr))
		{
			// (a new driver, or none kept: an empty one)
			if (!libraryData.empty())
				INFO_LOG(RENDERER, "DX12: the pipeline library kept is not this driver's (%x): started again", (unsigned)hr);
			library = nullptr;
			libraryData.clear();
			if (FAILED(device1->CreatePipelineLibrary(nullptr, 0, __uuidof(ID3D12PipelineLibrary), (void **)&library)))
				library = nullptr;
		}
	}
	NOTICE_LOG(RENDERER, "DX12: pipeline file %s: %u keys, library of %u KB%s", path.c_str(), (u32)keys.size(),
			(u32)(libraryData.size() / 1024), library == nullptr ? " (no pipeline library on this device)" : "");
}

void DX12Pipelines::SaveFile()
{
	const bool newKeys = statMade != statFromLibrary || storedSinceLoad != 0;
	if (!newKeys && statMadeInFrame == 0)
		return;
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	size_t libBytes = library != nullptr ? library->GetSerializedSize() : 0;
	DX12PsoFileHeader header;
	memcpy(header.magic, PsoFileMagic, sizeof(PsoFileMagic));
	header.version = PsoFileVersion;
	header.keyCount = (u32)keys.size();
	header.shaders = dx12_shaders_hash();
	header.reserved = 0;
	std::vector<u8> file(sizeof(header) + keys.size() * sizeof(u64) + libBytes);
	u8 *lib = &file[sizeof(header) + keys.size() * sizeof(u64)];
	if (libBytes != 0 && FAILED(library->Serialize(lib, libBytes)))
		libBytes = 0;
	header.libraryBytes = libBytes;
	file.resize(sizeof(header) + keys.size() * sizeof(u64) + libBytes);
	memcpy(&file[0], &header, sizeof(header));
	if (!keys.empty())
		memcpy(&file[sizeof(header)], keys.data(), keys.size() * sizeof(u64));
	// (written beside it, then put in its place: a write cut short leaves the one there was)
	const std::string path = get_writable_data_path(PipelineFileName);
	const std::string tmpPath = path + ".tmp";
	RFILE *f = filestream_open(tmpPath.c_str(), RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f == nullptr)
		return;
	bool written = filestream_write(f, &file[0], (int64_t)file.size()) == (int64_t)file.size();
	filestream_close(f);
	if (written)
	{
		filestream_delete(path.c_str());
		written = filestream_rename(tmpPath.c_str(), path.c_str()) == 0;
	}
	if (!written)
		filestream_delete(tmpPath.c_str());
	else
		NOTICE_LOG(RENDERER, "DX12: pipeline file saved: %u keys, library of %u KB", header.keyCount, (u32)(libBytes / 1024));
}

void DX12Pipelines::Term()
{
	// (the thread stopped first: what it made is kept, and the library is ours again)
	if (warm != nullptr)
	{
		d3d_warm_stop(warm);
		if (slots != nullptr)
			TakeBuilt();
		d3d_warm_free(warm);
		warm = nullptr;
	}
	batch.clear();
	if (slots != nullptr)
	{
		NOTICE_LOG(RENDERER, "DX12: %u pipelines made, %u of them in a frame, %u ahead, %u from the library", statMade, statMadeInFrame, statAhead, statFromLibrary);
		SaveFile();
		for (u32 i = 0; i < SLOTS; i++)
			if (slots[i].pso != nullptr)
				slots[i].pso->Release();
		delete [] slots;
		slots = nullptr;
	}
	delete [] requested;
	requested = nullptr;
	if (library != nullptr)
		library->Release();
	library = nullptr;
	libraryData.clear();
	libraryData.shrink_to_fit();
	if (device1 != nullptr)
		device1->Release();
	device1 = nullptr;
	keys.clear();
	used = 0;
	device = nullptr;
	root = nullptr;
	started = false;
}
