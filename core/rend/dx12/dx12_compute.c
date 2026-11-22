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
/* The compute renderer on Direct3D 12: see dx12_compute.h */
#include "dx12_compute.h"
#include <stdlib.h>
#include <string.h>
#include "dx12context_lr.h"
#include "dx12_texture.h"
#include "rend/ssaa_pattern.h"
#include "rend/ta_gpu.h"
#include "hw/pvr/Renderer_if.h"

#define MAX_U(a, b) ((a) > (b) ? (a) : (b))

/* The kernels, by PVRC_K_* (the walk's sorted variant for more than one
 * sample after the last, by SSAA_LOG2() of its samples, then its unsorted
 * variant for 1, 2, 4 and 8: pvr_compute.h's PVRC_V_*), as DXIL made from
 * rend/tile/pvr_compute.h when the source was: tools/rend/kernels_dx12.sh,
 * which checks they still are its. Nothing is compiled when the core runs. */
#include "dx12_compute_dxil.inc"
/* ...and the HDR output's walks, which store each pixel's channel sums as
 * well (PVRC_SUMS): the sorted variant by SSAA_LOG2() of its samples, then
 * the unsorted */
#include "dx12_compute_hdr_dxil.inc"
/* ...and the temporal upscaler's, which store each pixel's depth as well
 * (PVRC_DEPTH), for 1 sample a pixel: the sorted variant, the unsorted,
 * then both with the channel sums */
#include "dx12_compute_upscaler_dxil.inc"

#define COMPUTE_CHECK(name, c) typedef char dx12_compute_check_##name[(c) ? 1 : -1]
COMPUTE_CHECK(kernels, sizeof(dx12_compute_dxil) / sizeof(dx12_compute_dxil[0]) == PVRC_KERNELS + 8);
COMPUTE_CHECK(hdr, sizeof(dx12_compute_hdr_dxil) / sizeof(dx12_compute_hdr_dxil[0]) == 8);
COMPUTE_CHECK(upscaler, sizeof(dx12_compute_upscaler_dxil) / sizeof(dx12_compute_upscaler_dxil[0]) == 4);
/* (the decode's words, as the kernels read them) */
COMPUTE_CHECK(decode, PVRC_TAG_C_WORDS == TAG_C_WORDS && PVRC_TAG_C_COUNT == TAG_C_COUNT && PVRC_TAG_C_FIRST == TAG_C_FIRST
		&& PVRC_TAG_O_OVERRUN == TAG_O_OVERRUN && PVRC_TAG_O_ZSCALE == TAG_O_ZSCALE);
/* (a slot's word: the texture's place | its sampler's << 16) */
COMPUTE_CHECK(slot, PVRC_TEX_SLOTS <= 0x10000 && DX12_SRV_TABLE_SIZE <= 0x10000 && DX12_SAMPLER_TABLE_SIZE <= 0x10000);

static const IID iid_resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
static const IID iid_root_signature = { 0xc54a6b66, 0x72df, 0x4ee8, { 0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14 } };
static const IID iid_pipeline_state = { 0x765a30f3, 0xf624, 0x4c6f, { 0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45 } };
static const IID iid_command_signature = { 0xc36a797c, 0xec80, 0x4f0a, { 0x89, 0x85, 0xa7, 0xb2, 0x47, 0x50, 0x82, 0xd1 } };

const void *dx12_compute_kernel(u32 kernel, size_t *size)
{
	*size = dx12_compute_dxil_size[kernel];
	return dx12_compute_dxil[kernel];
}

/* (the walk's unsorted variant for @samples, in the table) */
#define WALK_UNSORTED_DXIL(samples) (PVRC_KERNELS + 4 + SSAA_LOG2(samples))

/* The bin pool's entries: a run for each tile a primitive touches, for a
 * frame's every primitive (4 MiB), as Vulkan's */
#define POOL_ENTRIES (2u << 20)
/* The pieces of the ring: frames in flight at once */
#define SLOTS DX12C_COMPUTE_TABLES
/* The kernels' views (the table's), in the order of their registers: the
 * buffers u0 to u9, then the picture (u0, space 2), the HDR output's
 * channel sums (u1, space 2: a null view but for its walks) and the
 * temporal upscaler's depth (u2, space 2: the same) */
enum { V_FRAME, V_REC, V_BIN, V_VTX, V_IDX, V_MOD, V_CMD, V_TRD, V_DFRM, V_SLOTS, V_PICTURE, V_SUMS, V_DEPTH, V_COUNT };
COMPUTE_CHECK(views, V_COUNT == DX12C_COMPUTE_TABLE_SIZE);
/* (the translucent fragments the walk keeps at a time: what the kernels
 * were made for, pvrc_fragments_kept() of what every Direct3D 12 device
 * gives a workgroup) */
#define SHARED_BYTES 32768

/* The textures a frame's polygons have, each given a slot: a table of a
 * fixed size, emptied by a new generation each frame (Vulkan's), each
 * slot's word the texture's place in the bindless table and its sampler's */
#define SLOT_TABLE_SIZE (2 * PVRC_TEX_SLOTS)
typedef struct slot_entry
{
	u64 texid;
	u32 sampler;
	u32 gen;
	u32 slot;
} slot_entry;

typedef struct slot_table
{
	slot_entry entries[SLOT_TABLE_SIZE];
	u32 *words;		/* this frame's piece of the ring: a word a slot */
	dx12_pooled_texture *images[PVRC_TEX_SLOTS];
	u32 gen;
	u32 used;
} slot_table;

static void slots_reset(slot_table *t, u32 *words)
{
	if (++t->gen == 0)
	{
		memset(t->entries, 0, sizeof(t->entries));
		t->gen = 1;
	}
	t->used = 0;
	t->words = words;
}

static u32 slots_slot(slot_table *t, u64 texid, u32 tsp)
{
	const u32 sampler = dx12_sampler_index(tsp);
	const u32 h = (u32)(texid >> 4) * 2654435761u ^ sampler * 40503u;
	u32 i;
	for (i = 0; i < SLOT_TABLE_SIZE; i++)
	{
		slot_entry *e = &t->entries[(h + i) & (SLOT_TABLE_SIZE - 1)];
		if (e->gen != t->gen)
		{
			const dx12_texture *texture;
			if (t->used == PVRC_TEX_SLOTS)
				return PVRC_NONE;
			e->gen = t->gen;
			e->texid = texid;
			e->sampler = sampler;
			e->slot = t->used;
			texture = texid != 0 ? (const dx12_texture *)(uintptr_t)texid : NULL;
			t->images[t->used] = texture != NULL ? texture->pooled : NULL;
			t->words[t->used] = (texture != NULL ? dx12_texture_srv(texture) : DX12_SRV_NONE) | (sampler << 16);
			return t->used++;
		}
		if (e->texid == texid && e->sampler == sampler)
			return e->slot;
	}
	return PVRC_NONE;
}

typedef struct compute_resources
{
	ID3D12RootSignature *root;
	ID3D12PipelineState *pipelines[PVRC_KERNELS + 1];
	/* the walk's unsorted variant (pvr_compute.h: PVRC_V_*; the sorted is pipelines[PVRC_K_WALK]) */
	ID3D12PipelineState *walkUnsorted;
	/* with SSAA, the walk for renders to a texture, at one sample a pixel, both variants */
	ID3D12PipelineState *walk1, *walk1Unsorted;
	/* with the HDR output, the screen's walk that stores the channel sums, both variants */
	ID3D12PipelineState *walkSums, *walkSumsUnsorted;
	bool sumsStored;	/* by the frame just recorded */
	/* for the temporal upscaler, the screen's walk that stores each pixel's
	 * depth, both variants, and both with the sums */
	ID3D12PipelineState *walkDepth, *walkDepthUnsorted, *walkDepthSums, *walkDepthSumsUnsorted;
	bool depthStored;	/* by the frame just recorded */
	D3D12_RESOURCE_STATES ringState;
	/* whether the device loads R8G8B8A8_UNORM from an unordered access view (the Y filter, PVRC_K_YFILTER) */
	bool yfilter;
	ID3D12CommandSignature *dispatch;
	/* the ring: a piece a slot, each the frame's words then the slots' */
	ID3D12Resource *ring;
	u32 *ringMap;
	u32 frameWords, pieceWords;
	u64 pieceFence[SLOTS];
	u32 slot;		/* this frame's piece */
	ID3D12Resource *records;
	ID3D12Resource *bins;
	ID3D12Resource *zero;		/* zeros, for the bins' counts and starts each frame */
	ID3D12Resource *args;		/* the dispatches' arguments: the bins' counts, copied */
	ID3D12Resource *across;		/* the walk's dispatches across, pvrc_walk_x() for each tile size and count of samples, copied to them */
	ID3D12Resource *stats;		/* the counts for the log, a piece a slot, read when it comes round */
	/* the picture a frame starts from when the one before cannot be read
	 * where it is (dx12_compute_copy_start()): the screen's size, a place in the table */
	ID3D12Resource *start;
	D3D12_RESOURCE_STATES startState;
	u32 startSrv;
	const u32 *statsMap;
	bool statsWritten[SLOTS];
	u32 binWords;
	u32 runs, chunks, tileLog2;
	u32 keep;
	u64 recordBytes;
	slot_table slots;
	bool gpu;	/* this frame's is the decode's */
	bool rtt;	/* ...a render to a texture */
	u32 last;		/* the picture to start from, in the table */
	bool lastScaled;
	/* this frame's, between dx12_compute_prepare() and dx12_compute_record() */
	u32 words, prims;
	u64 skipped, dropped, hiz, clipped;
	u32 peels;
} compute_resources;

static struct
{
	compute_resources *res;
	bool ready;
	bool failed;
	u32 samples;
	u32 width, height;
	bool gpuDraw;
	u64 statFrames;
	u64 statGpuFrames;
	u64 statRttFrames;
	u64 statFallback;
	u64 statBusy;
	u64 statWhy[CF_WHY_COUNT];
} cd;

const char *dx12_compute_device_lacks(void)
{
	ID3D12Device *device = dx12c.device;
	D3D12_FEATURE_DATA_SHADER_MODEL model;
	D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1;
	D3D12_FEATURE_DATA_D3D12_OPTIONS options;
	if (device == NULL)
		return "no device";
	memset(&model, 0, sizeof(model));
	model.HighestShaderModel = D3D_SHADER_MODEL_6_0;
	if (FAILED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)))
			|| model.HighestShaderModel < D3D_SHADER_MODEL_6_0)
		return "no shader model 6.0";
	memset(&options1, 0, sizeof(options1));
	if (FAILED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))) || !options1.Int64ShaderOps)
		return "no 64-bit integers in shaders";
	/* (the bindless table and the samplers are unbounded ranges of the root signature) */
	memset(&options, 0, sizeof(options));
	if (FAILED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))
			|| options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
		return "resource binding tier 1";
	return NULL;
}

static bool make_buffer(ID3D12Device *device, D3D12_HEAP_TYPE type, u64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
		ID3D12Resource **out)
{
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC desc;
	memset(&heap, 0, sizeof(heap));
	heap.Type = type;
	if (type == D3D12_HEAP_TYPE_CUSTOM)
	{
		/* written by the CPU, read in place by the GPU, and the decode's
		 * frame written by PVRC_K_GPUSRC: write-combined memory of the host's
		 * that unordered access views can have */
		heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
		heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
	}
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = MAX_U(bytes, 256);
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = flags;
	if (SUCCEEDED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, state, NULL,
			&iid_resource, (void **)out)))
		return true;
	*out = NULL;
	return false;
}

static bool make_kernel(const void *code, size_t size, ID3D12PipelineState **out)
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = cd.res->root;
	desc.CS.pShaderBytecode = code;
	desc.CS.BytecodeLength = size;
	if (SUCCEEDED(ID3D12Device_CreateComputePipelineState(dx12c.device, &desc, &iid_pipeline_state, (void **)out)))
		return true;
	*out = NULL;
	return false;
}

#define MAKE(index, out) make_kernel(dx12_compute_dxil[index], dx12_compute_dxil_size[index], out)
#define MAKE_HDR(index, out) make_kernel(dx12_compute_hdr_dxil[index], dx12_compute_hdr_dxil_size[index], out)
#define MAKE_DEPTH(index, out) make_kernel(dx12_compute_upscaler_dxil[index], dx12_compute_upscaler_dxil_size[index], out)

/* @object let go: once the GPU is done with it */
static void let_go(IUnknown *object)
{
	if (object != NULL)
		dx12c_release(object);
}

static void free_resources(compute_resources *r)
{
	u32 i;
	/* (what a frame still on the GPU has goes once it is done with it) */
	if (r->ring != NULL && r->ringMap != NULL)
		ID3D12Resource_Unmap(r->ring, 0, NULL);
	if (r->stats != NULL && r->statsMap != NULL)
		ID3D12Resource_Unmap(r->stats, 0, NULL);
	let_go((IUnknown *)r->ring);
	let_go((IUnknown *)r->records);
	let_go((IUnknown *)r->bins);
	let_go((IUnknown *)r->zero);
	let_go((IUnknown *)r->args);
	let_go((IUnknown *)r->across);
	let_go((IUnknown *)r->stats);
	let_go((IUnknown *)r->start);
	if (r->startSrv != ~0u)
		dx12c_release_srv(r->startSrv);
	for (i = 0; i <= PVRC_KERNELS; i++)
		let_go((IUnknown *)r->pipelines[i]);
	let_go((IUnknown *)r->walkUnsorted);
	let_go((IUnknown *)r->walk1);
	let_go((IUnknown *)r->walk1Unsorted);
	let_go((IUnknown *)r->walkSums);
	let_go((IUnknown *)r->walkSumsUnsorted);
	let_go((IUnknown *)r->walkDepth);
	let_go((IUnknown *)r->walkDepthUnsorted);
	let_go((IUnknown *)r->walkDepthSums);
	let_go((IUnknown *)r->walkDepthSumsUnsorted);
	let_go((IUnknown *)r->root);
	let_go((IUnknown *)r->dispatch);
	free(r);
}

static bool fail(const char *why)
{
	INFO_LOG(RENDERER, "Direct3D 12 compute renderer: not on this device (%s); the per-pixel renderer draws\n", why);
	cd.failed = true;
	if (cd.res != NULL)
		free_resources(cd.res);
	cd.res = NULL;
	return false;
}

/* A view of @words words of @buffer from @firstWord, at @place of the heap */
static void buffer_view(u32 place, ID3D12Resource *buffer, u64 firstWord, u64 words)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	desc.Buffer.FirstElement = firstWord;
	desc.Buffer.NumElements = (UINT)words;
	desc.Buffer.StructureByteStride = 4;
	ID3D12Device_CreateUnorderedAccessView(dx12c.device, buffer, NULL, &desc, dx12_heap_cpu(&dx12c.srv_heap, place));
}

bool dx12_compute_init(u32 width, u32 height, u32 samples, bool depth)
{
	ID3D12Device *device = dx12c.device;
	const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	compute_resources *r;
	const char *lacks;
	u32 tiles, k, s;
	u64 deviceBytes;
	D3D12_RANGE none;
	void *p = NULL;
	bool ok;

	dx12_compute_term();
	if (cd.failed)
		return false;
	cd.width = width;
	cd.height = height;
	cd.samples = samples > 4 ? 8 : samples > 2 ? 4 : samples > 1 ? 2 : 1;

	lacks = dx12_compute_device_lacks();
	if (lacks != NULL)
		return fail(lacks);
	r = (compute_resources *)calloc(1, sizeof(compute_resources));
	if (r == NULL)
		return fail("no memory");
	cd.res = r;
	r->ringState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	r->startState = D3D12_RESOURCE_STATE_COPY_DEST;
	r->startSrv = ~0u;
	r->last = ~0u;
	r->keep = pvrc_fragments_kept(SHARED_BYTES);
	r->tileLog2 = compute_frame_tile_log2(width);
	tiles = ((width + (1u << r->tileLog2) - 1) >> r->tileLog2) * ((height + (1u << r->tileLog2) - 1) >> r->tileLog2);
	r->chunks = (compute_frame_max_prims() + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2;
	/* (room for the screen's tiles, and a texture rendered to's, as Vulkan's) */
	r->runs = r->chunks * MAX_U(tiles, compute_frame_max_rtt_tiles());
	r->binWords = pvrc_bin_words(r->runs, POOL_ENTRIES);
	r->frameWords = (compute_frame_max_words() + 63) & ~63u;
	r->pieceWords = r->frameWords + PVRC_TEX_SLOTS;
	r->recordBytes = (u64)compute_frame_max_prims() * PVRC_REC_WORDS * 4;
	deviceBytes = r->recordBytes + (u64)r->binWords * 4 + (u64)PVRC_B_POOL(r->runs) * 4 + (u64)width * height * 4;
	if ((deviceBytes >> 10) > vram_budget_available_kib(&dx12c.vram_budget))
		return fail("not enough video memory");

	/* The root signature: the pictures' places in the table (b0); the table
	 * of views (u0 to u9, then the picture, the sums and the depth, u0 to u2
	 * of space 2); the bindless table
	 * (t0 on, space 1) and the samplers (s0 on, space 1); the linear
	 * sampler for a picture of another size to start from (s0) */
	{
		typedef HRESULT (WINAPI *serialize_fn)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
		D3D12_DESCRIPTOR_RANGE ranges[4];
		D3D12_ROOT_PARAMETER params[4];
		D3D12_STATIC_SAMPLER_DESC linear;
		D3D12_ROOT_SIGNATURE_DESC desc;
		ID3DBlob *blob = NULL, *errors = NULL;
		HMODULE lib;
		serialize_fn serialize;
		int i;
		memset(ranges, 0, sizeof(ranges));
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[0].NumDescriptors = V_PICTURE;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 3;
		ranges[1].RegisterSpace = 2;
		ranges[1].OffsetInDescriptorsFromTableStart = V_PICTURE;
		ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[2].NumDescriptors = UINT_MAX;
		ranges[2].RegisterSpace = 1;
		ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		ranges[3].NumDescriptors = UINT_MAX;
		ranges[3].RegisterSpace = 1;
		memset(params, 0, sizeof(params));
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.Num32BitValues = 2;
		for (i = 0; i < 3; i++)
		{
			params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[1 + i].DescriptorTable.NumDescriptorRanges = i == 0 ? 2 : 1;
			params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i == 0 ? 0 : i + 1];
		}
		memset(&linear, 0, sizeof(linear));
		linear.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		linear.AddressU = linear.AddressV = linear.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		linear.MaxLOD = D3D12_FLOAT32_MAX;
		linear.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		memset(&desc, 0, sizeof(desc));
		desc.NumParameters = 4;
		desc.pParameters = params;
		desc.NumStaticSamplers = 1;
		desc.pStaticSamplers = &linear;
		lib = GetModuleHandleA("d3d12.dll");
		serialize = lib != NULL ? (serialize_fn)GetProcAddress(lib, "D3D12SerializeRootSignature") : NULL;
		ok = serialize != NULL && SUCCEEDED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))
				&& SUCCEEDED(ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
						&iid_root_signature, (void **)&r->root));
		if (blob != NULL)
			ID3D10Blob_Release(blob);
		if (errors != NULL)
			ID3D10Blob_Release(errors);
		if (!ok)
		{
			r->root = NULL;
			return fail("no root signature");
		}
	}
	/* every kernel, now, for this many samples, and the walk for one when
	 * that is not one (renders to a texture): none is made while a game runs */
	for (k = 1; k <= PVRC_KERNELS; k++)
		if (!MAKE(k == PVRC_K_WALK && cd.samples > 1 ? PVRC_KERNELS + SSAA_LOG2(cd.samples) : k, &r->pipelines[k]))
			return fail("a kernel's pipeline could not be made");
	if (!MAKE(WALK_UNSORTED_DXIL(cd.samples), &r->walkUnsorted))
		return fail("a kernel's pipeline could not be made");
	if (cd.samples > 1 && (!MAKE(PVRC_K_WALK, &r->walk1) || !MAKE(WALK_UNSORTED_DXIL(1), &r->walk1Unsorted)))
		return fail("a kernel's pipeline could not be made");
	/* (the HDR output's: the walks that store the channel sums; with them
	 * not made, the screen's walks are the others and the picture is
	 * encoded as it is) */
	if (dx12c_hdr_active())
	{
		if (!MAKE_HDR(SSAA_LOG2(cd.samples), &r->walkSums) || !MAKE_HDR(4 + SSAA_LOG2(cd.samples), &r->walkSumsUnsorted))
		{
			WARN_LOG(RENDERER, "Direct3D 12 compute renderer: the HDR output's walks could not be made; the picture is encoded as it is\n");
			DX12_RELEASE(r->walkSums);
			DX12_RELEASE(r->walkSumsUnsorted);
		}
	}
	/* (the temporal upscaler's: the walks that store the depth, at one
	 * sample a pixel - the upscaler is the anti-aliasing then; with them not
	 * made, the frames are drawn by the others and shown as they are) */
	if (depth && cd.samples == 1)
	{
		ok = MAKE_DEPTH(0, &r->walkDepth) && MAKE_DEPTH(1, &r->walkDepthUnsorted);
		if (ok && r->walkSums != NULL)
			ok = MAKE_DEPTH(2, &r->walkDepthSums) && MAKE_DEPTH(3, &r->walkDepthSumsUnsorted);
		if (!ok)
		{
			WARN_LOG(RENDERER, "Direct3D 12 compute renderer: the temporal upscaler's walks could not be made; its frames are shown as they are\n");
			DX12_RELEASE(r->walkDepth);
			DX12_RELEASE(r->walkDepthUnsorted);
			DX12_RELEASE(r->walkDepthSums);
			DX12_RELEASE(r->walkDepthSumsUnsorted);
		}
	}
	{
		D3D12_INDIRECT_ARGUMENT_DESC arg;
		D3D12_COMMAND_SIGNATURE_DESC desc;
		memset(&arg, 0, sizeof(arg));
		arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
		memset(&desc, 0, sizeof(desc));
		desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
		desc.NumArgumentDescs = 1;
		desc.pArgumentDescs = &arg;
		if (FAILED(ID3D12Device_CreateCommandSignature(device, &desc, NULL, &iid_command_signature, (void **)&r->dispatch)))
		{
			r->dispatch = NULL;
			return fail("no command signature");
		}
	}
	{
		/* (typed loads of R8G8B8A8_UNORM: every device of feature level
		 * 12_0 and up has them, but they are an option of 11's) */
		D3D12_FEATURE_DATA_FORMAT_SUPPORT fs;
		memset(&fs, 0, sizeof(fs));
		fs.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		r->yfilter = SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))
				&& (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
	}

	ok = make_buffer(device, D3D12_HEAP_TYPE_CUSTOM, (u64)r->pieceWords * 4 * SLOTS, uav, ua, &r->ring)
			&& make_buffer(device, D3D12_HEAP_TYPE_DEFAULT, r->recordBytes, uav, ua, &r->records)
			/* (made zero: a committed resource's memory is) */
			&& make_buffer(device, D3D12_HEAP_TYPE_DEFAULT, (u64)r->binWords * 4, uav, ua, &r->bins)
			&& make_buffer(device, D3D12_HEAP_TYPE_DEFAULT, (u64)PVRC_B_POOL(r->runs) * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_SOURCE, &r->zero)
			&& make_buffer(device, D3D12_HEAP_TYPE_DEFAULT, PVRC_ST_ARGS * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, &r->args)
			&& make_buffer(device, D3D12_HEAP_TYPE_UPLOAD, 8 * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &r->across)
			&& make_buffer(device, D3D12_HEAP_TYPE_READBACK, (u64)PVRC_STATS * 4 * SLOTS, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &r->stats);
	none.Begin = 0;
	none.End = 0;
	if (ok)
	{
		/* (tiles of 2^5 or 2^6 pixels, compute_frame_tile_log2(); 1, 2, 4 or 8 samples) */
		u32 *across = NULL;
		ok = SUCCEEDED(ID3D12Resource_Map(r->across, 0, &none, (void **)&across));
		if (ok)
		{
			u32 i;
			for (i = 0; i < 8; i++)
				across[i] = pvrc_walk_x(5 + i / 4, 1u << (i % 4));
			ID3D12Resource_Unmap(r->across, 0, NULL);
		}
	}
	if (ok)
	{
		ok = dx12_make_texture(device, width, height, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, r->startState, &r->start, NULL);
		r->startSrv = ok ? dx12_heap_alloc(&dx12c.srv_heap) : ~0u;
		ok = ok && r->startSrv != ~0u;
		if (ok)
			dx12c_make_srv(r->start, DXGI_FORMAT_R8G8B8A8_UNORM, 1, r->startSrv);
	}
	if (!ok)
		return fail("its buffers could not be made");
	if (FAILED(ID3D12Resource_Map(r->ring, 0, &none, &p)))
		return fail("its ring could not be mapped");
	r->ringMap = (u32 *)p;
	if (FAILED(ID3D12Resource_Map(r->stats, 0, NULL, &p)))
		return fail("its counts could not be mapped");
	r->statsMap = (const u32 *)p;

	/* Each piece's table of views of the buffers (the decode's are written
	 * when a frame of the decode's has the piece) */
	for (s = 0; s < SLOTS; s++)
	{
		const u32 table = DX12C_COMPUTE_FIRST + s * DX12C_COMPUTE_TABLE_SIZE;
		D3D12_UNORDERED_ACCESS_VIEW_DESC sums;
		u32 v;
		buffer_view(table + V_FRAME, r->ring, (u64)s * r->pieceWords, r->frameWords);
		buffer_view(table + V_REC, r->records, 0, r->recordBytes / 4);
		buffer_view(table + V_BIN, r->bins, 0, r->binWords);
		for (v = V_VTX; v <= V_DFRM; v++)
			buffer_view(table + v, r->ring, (u64)s * r->pieceWords, r->frameWords);
		buffer_view(table + V_SLOTS, r->ring, (u64)s * r->pieceWords + r->frameWords, PVRC_TEX_SLOTS);
		memset(&sums, 0, sizeof(sums));
		sums.Format = DXGI_FORMAT_R16G16B16A16_UINT;
		sums.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		ID3D12Device_CreateUnorderedAccessView(device, NULL, NULL, &sums, dx12_heap_cpu(&dx12c.srv_heap, table + V_SUMS));
		sums.Format = DX12UpscalerDepthFormat;
		ID3D12Device_CreateUnorderedAccessView(device, NULL, NULL, &sums, dx12_heap_cpu(&dx12c.srv_heap, table + V_DEPTH));
	}

	cd.ready = true;
	NOTICE_LOG(RENDERER, "Direct3D 12 compute renderer: %u x %u, %u sample%s a pixel, tiles of %u, %u translucent fragments a sample at a time; %u KiB of video memory, %u KiB of host memory\n",
			width, height, cd.samples, cd.samples > 1 ? "s" : "", 1u << r->tileLog2, r->keep,
			(u32)(deviceBytes >> 10), (u32)(((u64)r->pieceWords * 4 * SLOTS) >> 10));
	return true;
}

void dx12_compute_term(void)
{
	compute_resources *r = cd.res;
	if (r != NULL)
	{
		if (cd.statFrames + cd.statFallback != 0)
		{
			NOTICE_LOG(RENDERER, "Direct3D 12 compute renderer: %llu frames drawn (%llu of them the GPU's TA decode's, %llu renders to a texture), %llu left to the per-pixel renderer; %llu runs walked over their chunk, %llu triangles at no depth left out, %llu with a vertex behind the eye clipped, at most %u rounds of translucent fragments, %llu primitives passed over by the tiles' depths\n",
					(unsigned long long)cd.statFrames, (unsigned long long)cd.statGpuFrames, (unsigned long long)cd.statRttFrames, (unsigned long long)cd.statFallback, (unsigned long long)r->skipped,
					(unsigned long long)r->dropped, (unsigned long long)r->clipped, r->peels, (unsigned long long)r->hiz);
			NOTICE_LOG(RENDERER, "Direct3D 12 compute renderer: fallbacks: %llu renders to a texture it cannot write, %llu starting from a picture it cannot take, %llu with more render passes than it takes, %llu with more textures than its slots, %llu too large, %llu the decode's without its buffers, %llu with its piece of the ring still the GPU's\n",
					(unsigned long long)cd.statWhy[CF_WHY_RTT], (unsigned long long)cd.statWhy[CF_WHY_PICTURE],
					(unsigned long long)cd.statWhy[CF_WHY_PASSES], (unsigned long long)cd.statWhy[CF_WHY_TEXTURES],
					(unsigned long long)cd.statWhy[CF_WHY_ROOM], (unsigned long long)cd.statWhy[CF_WHY_DECODE],
					(unsigned long long)cd.statBusy);
		}
		free_resources(r);
		cd.res = NULL;
	}
	cd.ready = false;
	cd.statFrames = cd.statFallback = cd.statGpuFrames = cd.statRttFrames = cd.statBusy = 0;
	memset(cd.statWhy, 0, sizeof(cd.statWhy));
}

void dx12_compute_reset(void)
{
	dx12_compute_term();
	cd.failed = false;
	cd.gpuDraw = false;
}

bool dx12_compute_ready(void)
{
	return cd.ready;
}

void dx12_compute_set_gpu_draw(bool on)
{
	cd.gpuDraw = on;
}

u32 dx12_compute_width(void)
{
	return cd.width;
}

u32 dx12_compute_height(void)
{
	return cd.height;
}

void dx12_compute_fallback(u32 why)
{
	cd.statFallback++;
	cd.statWhy[why < CF_WHY_COUNT ? why : 0]++;
}

bool dx12_compute_sums_on(void)
{
	return cd.res != NULL && cd.res->walkSums != NULL;
}

bool dx12_compute_depth_on(void)
{
	return cd.res != NULL && cd.res->walkDepth != NULL;
}

bool dx12_compute_depth_stored(void)
{
	return cd.res != NULL && cd.res->depthStored;
}

bool dx12_compute_sums_stored(void)
{
	return cd.res != NULL && cd.res->sumsStored;
}

u32 dx12_compute_start_srv(void)
{
	return cd.res != NULL ? cd.res->startSrv : ~0u;
}

void dx12_compute_ring_for_draw(D3D12_VERTEX_BUFFER_VIEW *vertices, D3D12_INDEX_BUFFER_VIEW *indices)
{
	compute_resources *r = cd.res;
	const u32 *words = r->ringMap + (size_t)r->slot * r->pieceWords;
	const D3D12_GPU_VIRTUAL_ADDRESS piece = ID3D12Resource_GetGPUVirtualAddress(r->ring) + (u64)r->slot * r->pieceWords * 4;
	vertices->BufferLocation = piece + (u64)words[PVRC_F_O_VTX] * 4;
	vertices->SizeInBytes = (words[PVRC_F_O_IDX] - words[PVRC_F_O_VTX]) * 4;
	vertices->StrideInBytes = sizeof(Vertex);
	indices->BufferLocation = piece + (u64)words[PVRC_F_O_IDX] * 4;
	indices->SizeInBytes = (r->frameWords - words[PVRC_F_O_IDX]) * 4;
	indices->Format = DXGI_FORMAT_R32_UINT;
	dx12c_transition_resource(r->ring, &r->ringState, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}

void dx12_compute_copy_start(ID3D12Resource *from)
{
	compute_resources *r = cd.res;
	dx12c_transition_resource(r->start, &r->startState, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	ID3D12GraphicsCommandList_CopyResource(dx12c_cmd(), r->start, from);
	dx12c_transition_resource(r->start, &r->startState, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}

bool dx12_compute_can_draw(void)
{
	if (!cd.ready || pvrrc.isRenderFramebuffer || (pvrrc.gpu && (!cd.gpuDraw || !dx12_ta_ready())))
		return false;
	return true;
}

/* compute_frame_pack()'s renderer: @frame's, but the slots this module's */
typedef struct bound_frame
{
	const compute_frame_renderer *frame;
	slot_table *slots;
} bound_frame;

static u32 bound_slot(void *self, u64 texid, u32 tsp)
{
	return slots_slot(((bound_frame *)self)->slots, texid, tsp);
}

static const xform *bound_begin(void *self)
{
	const compute_frame_renderer *f = ((bound_frame *)self)->frame;
	return f->begin(f->self);
}

static void bound_base_scissor(void *self, int *x, int *y, int *width, int *height)
{
	const compute_frame_renderer *f = ((bound_frame *)self)->frame;
	f->base_scissor(f->self, x, y, width, height);
}

static void bound_provoking_vertices(void *self)
{
	const compute_frame_renderer *f = ((bound_frame *)self)->frame;
	f->provoking_vertices(f->self);
}

bool dx12_compute_prepare(const compute_frame_renderer *frame, u32 last, bool scaled, u32 rttWidth, u32 rttHeight)
{
	compute_resources *r = cd.res;
	/* The next piece of the ring, if the GPU is done with the frame that
	 * had it (and the counts it left, read); else the per-pixel renderer
	 * draws this one */
	const u32 slot = (r->slot + 1) % SLOTS;
	const bool rtt = pvrrc.isRTT;
	compute_frame_layout layout;
	compute_frame_renderer bound;
	bound_frame b;
	u32 tiles, why = CF_WHY_NONE;
	u32 *words;

	if (r->pieceFence[slot] > dx12c_completed())
	{
		if (cd.statBusy++ == 0)
			NOTICE_LOG(RENDERER, "Direct3D 12 compute renderer: the GPU still has the piece of the ring this frame would take (an earlier frame's): the per-pixel renderer draws it (and others like it, not logged)\n");
		dx12_compute_fallback(CF_WHY_NONE);
		return false;
	}
	if (r->statsWritten[slot])
	{
		const u32 *st = r->statsMap + slot * PVRC_STATS;
		r->skipped += st[PVRC_ST_SKIPPED];
		r->dropped += st[PVRC_ST_DROPPED];
		r->clipped += st[PVRC_ST_CLIPPED];
		r->hiz += st[PVRC_ST_HIZ];
		r->peels = MAX_U(r->peels, st[PVRC_ST_PEELS]);
		r->statsWritten[slot] = false;
	}

	memset(&layout, 0, sizeof(layout));
	layout.width = rtt ? rttWidth : cd.width;
	layout.height = rtt ? rttHeight : cd.height;
	layout.tileLog2 = rtt ? compute_frame_tile_log2(layout.width) : r->tileLog2;
	layout.pool = POOL_ENTRIES;
	layout.runs = r->runs;
	layout.chunks = r->chunks;
	layout.lastPicture = last != ~0u && !rtt;
	layout.lastScaled = layout.lastPicture && scaled;
	layout.rttScale = rtt ? compute_frame_rtt_scale() : 0;
	/* (a texture rendered to larger than the bins are made for: none the PowerVR's registers ask for) */
	tiles = ((layout.width + (1u << layout.tileLog2) - 1) >> layout.tileLog2) * ((layout.height + (1u << layout.tileLog2) - 1) >> layout.tileLog2);
	if (layout.width == 0 || layout.height == 0 || (u64)tiles * r->chunks > r->runs)
	{
		dx12_compute_fallback(CF_WHY_ROOM);
		return false;
	}
	words = r->ringMap + (size_t)slot * r->pieceWords;
	slots_reset(&r->slots, words + r->frameWords);
	b.frame = frame;
	b.slots = &r->slots;
	bound.self = &b;
	bound.slot = bound_slot;
	bound.begin = bound_begin;
	bound.base_scissor = bound_base_scissor;
	bound.provoking_vertices = bound_provoking_vertices;
	r->words = compute_frame_pack(&layout, &bound, words, r->frameWords, &r->prims, &why);
	if (r->words == 0)
	{
		dx12_compute_fallback(why);
		return false;
	}
	r->slot = slot;
	r->gpu = pvrrc.gpu;
	r->rtt = rtt;
	r->last = layout.lastPicture ? last : ~0u;
	r->lastScaled = layout.lastScaled;
	return true;
}

static void uav_barrier(ID3D12GraphicsCommandList *cmd)
{
	D3D12_RESOURCE_BARRIER b;
	memset(&b, 0, sizeof(b));
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	ID3D12GraphicsCommandList_ResourceBarrier(cmd, 1, &b);
}

/* the bins' counts, as the GPU made them, to the dispatches' arguments */
static void arguments(ID3D12GraphicsCommandList *cmd, const u32 *words, D3D12_RESOURCE_STATES *binState, D3D12_RESOURCE_STATES *argState)
{
	compute_resources *r = cd.res;
	const u64 across = ((words[PVRC_F_TILE_LOG2] - 5) * 4 + SSAA_LOG2(r->rtt ? 1u : cd.samples)) * 4;
	dx12c_transition_resource(r->bins, binState, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	dx12c_transition_resource(r->args, argState, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->args, 0, r->bins, (u64)PVRC_B_STATS(r->runs) * 4, PVRC_ST_ARGS * 4);
	ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->args, PVRC_ST_WALK_U_X * 4, r->across, across, 4);
	ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->args, PVRC_ST_WALK_S_X * 4, r->across, across, 4);
	dx12c_transition_resource(r->args, argState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	dx12c_transition_resource(r->bins, binState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}

/* @kernel dispatched @x by @y (the walk at one sample a pixel where @n is) */
static void step(ID3D12GraphicsCommandList *cmd, u32 n, int kernel, u32 x, u32 y)
{
	compute_resources *r = cd.res;
	ID3D12GraphicsCommandList_SetPipelineState(cmd, kernel == PVRC_K_WALK && n != cd.samples ? r->walk1 : r->pipelines[kernel]);
	if (x != 0 && y != 0)
		ID3D12GraphicsCommandList_Dispatch(cmd, x, y, 1);
	uav_barrier(cmd);
}

static void indirect(ID3D12GraphicsCommandList *cmd, int kernel, u32 at)
{
	compute_resources *r = cd.res;
	ID3D12GraphicsCommandList_SetPipelineState(cmd, r->pipelines[kernel]);
	ID3D12GraphicsCommandList_ExecuteIndirect(cmd, r->dispatch, 1, r->args, (u64)at * 4, NULL, 0);
	uav_barrier(cmd);
}

static u64 buffer_words(ID3D12Resource *buffer)
{
	const D3D12_RESOURCE_DESC desc = dx12_resource_desc(buffer);
	return desc.Width / 4;
}

void dx12_compute_record(ID3D12Resource *picture, ID3D12Resource *sums, ID3D12Resource *depth)
{
	compute_resources *r = cd.res;
	ID3D12Device *device = dx12c.device;
	ID3D12GraphicsCommandList *cmd = dx12c_cmd();
	const u32 slot = r->slot;
	const u32 *words = r->ringMap + (size_t)slot * r->pieceWords;
	const u32 table = DX12C_COMPUTE_FIRST + slot * DX12C_COMPUTE_TABLE_SIZE;
	/* (a render to a texture is drawn at one sample a pixel) */
	const u32 n = r->rtt ? 1 : cd.samples;
	const D3D12_RESOURCE_STATES read = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	D3D12_RESOURCE_STATES binState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	D3D12_RESOURCE_STATES argState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
	ID3D12DescriptorHeap *heaps[2];
	u32 places[2];
	u32 used, tiles, groups, yx, yy, i;

	/* (the ring as the kernels read it, after a frame's motion vector pass read it as its buffers) */
	dx12c_transition_resource(r->ring, &r->ringState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	/* The piece's views: the picture, and what the frame's vertices,
	 * indices, volumes and the decode's words are read from - the ring's
	 * piece, or the decode's buffers (views are a few words in the heap,
	 * written where they change; the GPU is done with this piece's) */
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		ID3D12Device_CreateUnorderedAccessView(device, picture, NULL, &desc, dx12_heap_cpu(&dx12c.srv_heap, table + V_PICTURE));
		/* (the HDR output's channel sums, a frame of the screen's: where its walks store them) */
		if (r->walkSums == NULL || r->rtt)
			sums = NULL;
		desc.Format = DXGI_FORMAT_R16G16B16A16_UINT;
		ID3D12Device_CreateUnorderedAccessView(device, sums, NULL, &desc, dx12_heap_cpu(&dx12c.srv_heap, table + V_SUMS));
		/* (the temporal upscaler's depth, a frame of the screen's at one sample: where its walks store it) */
		if (r->walkDepth == NULL || r->rtt || (sums != NULL && r->walkDepthSums == NULL))
			depth = NULL;
		desc.Format = DX12UpscalerDepthFormat;
		ID3D12Device_CreateUnorderedAccessView(device, depth, NULL, &desc, dx12_heap_cpu(&dx12c.srv_heap, table + V_DEPTH));
		if (r->gpu)
		{
			const dx12_ta_draw *draw = dx12_ta_get_draw();
			buffer_view(table + V_VTX, draw->vertexResource, 0, draw->vertices.SizeInBytes / 4);
			buffer_view(table + V_IDX, draw->indexResource, 0, draw->indices.SizeInBytes / 4);
			buffer_view(table + V_MOD, draw->modtrigResource, 0, draw->modtrig.SizeInBytes / 4);
			buffer_view(table + V_CMD, draw->commands, 0, buffer_words(draw->commands));
			buffer_view(table + V_TRD, draw->trDraws, 0, buffer_words(draw->trDraws));
			buffer_view(table + V_DFRM, draw->frame, 0, buffer_words(draw->frame));
			/* the decode's outputs where the kernels read them */
			dx12_ta_for_kernels();
		}
		else
		{
			u32 v;
			for (v = V_VTX; v <= V_DFRM; v++)
				buffer_view(table + v, r->ring, (u64)slot * r->pieceWords, r->frameWords);
		}
	}
	/* The frame's textures, the palette and the picture to start from, for
	 * compute shaders too (left in the states the pixel shaders read them
	 * in as well) */
	for (i = 0; i < r->slots.used; i++)
	{
		dx12_pooled_texture *t = r->slots.images[i];
		if (t != NULL && t->state != read)
			dx12c_transition_resource(t->resource, &t->state, read, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	}

	/* The bins' counts and starts of this frame's runs, and the counts for
	 * the log, 0; the pool is 0 after the frame before's clear */
	used = (r->gpu ? r->chunks : words[PVRC_F_CHUNKS]) * words[PVRC_F_TILES_X] * words[PVRC_F_TILES_Y];
	dx12c_transition_resource(r->bins, &binState, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	if (used >= r->runs)
		ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->bins, 0, r->zero, 0, (u64)PVRC_B_POOL(r->runs) * 4);
	else
	{
		if (used != 0)
		{
			ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->bins, (u64)PVRC_B_COUNT(r->runs) * 4, r->zero, 0, (u64)used * 4);
			ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->bins, (u64)PVRC_B_START(r->runs) * 4, r->zero, 0, (u64)used * 4);
			ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->bins, (u64)PVRC_B_CURSOR(r->runs) * 4, r->zero, 0, (u64)used * 4);
		}
		ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->bins, (u64)PVRC_B_STATS(r->runs) * 4, r->zero, 0, (u64)(PVRC_B_POOL(r->runs) - PVRC_B_STATS(r->runs)) * 4);
	}
	dx12c_transition_resource(r->bins, &binState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

	heaps[0] = dx12c.srv_heap.heap;
	heaps[1] = dx12c.sampler_heap.heap;
	ID3D12GraphicsCommandList_SetDescriptorHeaps(cmd, 2, heaps);
	ID3D12GraphicsCommandList_SetComputeRootSignature(cmd, r->root);
	places[0] = r->last != ~0u ? r->last : DX12_SRV_NONE;
	places[1] = DX12_SRV_PALETTE;
	ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cmd, 0, 2, places, 0);
	ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cmd, 1, dx12_heap_gpu(&dx12c.srv_heap, table));
	ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cmd, 2, dx12_heap_gpu(&dx12c.srv_heap, 0));
	ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cmd, 3, dx12_heap_gpu(&dx12c.sampler_heap, 0));

	tiles = words[PVRC_F_TILES_X] * words[PVRC_F_TILES_Y];
	groups = (r->prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP;
	if (r->gpu)
	{
		/* A frame of the decode's: its numbering, and the dispatches of
		 * the kernels whose sizes are the GPU's, from it */
		step(cmd, n, PVRC_K_GPUSRC, 1, 1);
		arguments(cmd, words, &binState, &argState);
		indirect(cmd, PVRC_K_SETUP, PVRC_ST_SETUP_X);
		step(cmd, n, PVRC_K_SCAN, 1, 1);
		indirect(cmd, PVRC_K_SCATTER, PVRC_ST_SETUP_X);
		indirect(cmd, PVRC_K_SORT, PVRC_ST_SORT_X);
	}
	else
	{
		step(cmd, n, PVRC_K_SETUP, groups, 1);
		step(cmd, n, PVRC_K_SCAN, 1, 1);
		step(cmd, n, PVRC_K_SCATTER, groups, 1);
		step(cmd, n, PVRC_K_SORT, tiles, words[PVRC_F_CHUNKS]);
	}
	/* (the walk's two variants, each over the coarse tiles of its list,
	 * the sort's: nothing between them; the clear's size is the scan's) */
	arguments(cmd, words, &binState, &argState);
	ID3D12GraphicsCommandList_SetPipelineState(cmd, n != cd.samples ? r->walk1Unsorted
			: depth != NULL ? (sums != NULL ? r->walkDepthSumsUnsorted : r->walkDepthUnsorted)
			: sums != NULL ? r->walkSumsUnsorted : r->walkUnsorted);
	ID3D12GraphicsCommandList_ExecuteIndirect(cmd, r->dispatch, 1, r->args, (u64)PVRC_ST_WALK_U_X * 4, NULL, 0);
	ID3D12GraphicsCommandList_SetPipelineState(cmd, n != cd.samples ? r->walk1
			: depth != NULL ? (sums != NULL ? r->walkDepthSums : r->walkDepth)
			: sums != NULL ? r->walkSums : r->pipelines[PVRC_K_WALK]);
	ID3D12GraphicsCommandList_ExecuteIndirect(cmd, r->dispatch, 1, r->args, (u64)PVRC_ST_WALK_S_X * 4, NULL, 0);
	uav_barrier(cmd);
	/* the Y filter, in place over the picture, where the frame has it
	 * (and the device loads the picture's format from a view of it that
	 * is written: dx12_compute_init()) */
	pvrc_yfilter_groups(words, &yx, &yy);
	if (yy != 0 && r->yfilter)
		step(cmd, n, PVRC_K_YFILTER, yx, yy);
	/* (the sums are the picture's where the walk alone wrote it, and it
	 * resolved more than one sample a pixel - SSAA's, or the X scaler's
	 * pairs: Vulkan's rule) */
	r->sumsStored = sums != NULL && !(yy != 0 && r->yfilter)
			&& (n > 1 || (words[PVRC_F_FLAGS] & PVRC_FF_HSCALE) != 0);
	r->depthStored = depth != NULL;
	indirect(cmd, PVRC_K_CLEAR, PVRC_ST_CLEAR_X);
	/* the counts, for when this piece comes round */
	dx12c_transition_resource(r->bins, &binState, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->stats, (u64)slot * PVRC_STATS * 4, r->bins, (u64)PVRC_B_STATS(r->runs) * 4, PVRC_STATS * 4);
	dx12c_transition_resource(r->bins, &binState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	r->statsWritten[slot] = true;
	/* (the list this is in signals the next value when it is submitted) */
	r->pieceFence[slot] = dx12c.fence_value + 1;
	/* (another root signature and table were bound for compute: the raster's are bound again when they are drawn with) */
	dx12c_forget_raster();
	cd.statFrames++;
	if (r->gpu)
		cd.statGpuFrames++;
	if (r->rtt)
		cd.statRttFrames++;
}
