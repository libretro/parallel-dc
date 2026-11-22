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
/* GPU-side TA processing in Direct3D 12: see dx12_ta_decoder.h */
#include "dx12_ta_decoder.h"
#include <stdlib.h>
#include <string.h>
#include "dx12context_lr.h"
#include "rend/ta_gpu.h"

extern int screen_height;

/* The kernels, by TAG_K_*, as DXBC made from rend/ta_gpu.h when the core
 * was built: tools/rend/kernels_dx12.sh, which checks that they still are
 * its. Nothing is compiled when the core runs. */
#include "dx12_ta_dxbc.inc"

#define TA_CHECK(name, c) typedef char dx12_ta_check_##name[(c) ? 1 : -1]
TA_CHECK(kernels, sizeof(dx12_ta_dxbc) / sizeof(dx12_ta_dxbc[0]) == TAG_KERNELS + 1);
/* (the renderers draw from the records and commands as they are) */
TA_CHECK(record, TAG_C_WORDS * 4 == sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
TA_CHECK(vertex, TAG_V_WORDS * 4 == sizeof(Vertex));
TA_CHECK(args, TAG_O_ARGSCH == TAG_O_ARGS64 + 3 && TAG_O_ARGS1 == TAG_O_ARGS64 + 6);

#define MIN_U(a, b) ((a) < (b) ? (a) : (b))
#define MAX_U(a, b) ((a) > (b) ? (a) : (b))

static const IID iid_resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
static const IID iid_root_signature = { 0xc54a6b66, 0x72df, 0x4ee8, { 0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14 } };
static const IID iid_pipeline_state = { 0x765a30f3, 0xf624, 0x4c6f, { 0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45 } };
static const IID iid_command_signature = { 0xc36a797c, 0xec80, 0x4f0a, { 0x89, 0x85, 0xa7, 0xb2, 0x47, 0x50, 0x82, 0xd1 } };

typedef HRESULT (WINAPI *SerializeRootSignature_t)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);

/* The kernels' outputs, in the order of their unordered access views
 * (u0 to u8); then what the decoder has of its own */
enum { B_VTX, B_MOD, B_IDX, B_WORK, B_REC, B_CMD, B_FRM, B_TRD, B_TRP, B_OUTPUTS,
	B_ARGS = B_OUTPUTS,	/* the dispatches' arguments, copied out of the frame's words */
	B_FRM0,			/* the frame's words as each decode starts (fZ_max 1, the rest 0) */
	B_ZERO,			/* zeroes, copied where a decode wants them */
	B_INST,			/* 0, 1, 2...: the translucent strips' numbers (dx12_ta_draw::instances) */
	B_COUNT };

/* How many pieces the tables have: a frame's is written again once the GPU
 * is done with the frame that read it last */
#define TABLE_PIECES 8

typedef struct ta_resources
{
	ID3D12Resource *pool;		/* the TA contexts' data */
	u8 *poolMap;
	u32 slice, slices;
	ID3D12Resource *tables;		/* mapped: a piece for each frame */
	u32 *tablesMap;
	u32 tableWords;
	u64 pieceFence[TABLE_PIECES];	/* the list that last read each piece */
	ID3D12Resource *buffers[B_COUNT];
	D3D12_RESOURCE_STATES states[B_COUNT];
	u64 sizes[B_COUNT];
	TaGpuLayout layout;
	ID3D12RootSignature *root;
	ID3D12PipelineState *kernels[TAG_KERNELS + 1];
	ID3D12CommandSignature *dispatch;
	ID3D12CommandSignature *drawIndexed;
	u32 serial;
	/* (TA_GPU_SORT) room for the sorted translucent indices of a frame, and
	 * the prepass's checks after them, for each piece of the tables, in
	 * memory the CPU writes them to once (they are its sort's) and the GPU
	 * reads once, but for the prepass's triangles the prepass's kernel
	 * makes nothing of (TAG_K_PREPASS), which it writes in place */
	ID3D12Resource *sorted;
	D3D12_RESOURCE_STATES sortedState;
	u8 *sortedMap;
	u64 sortSlot;		/* bytes; 0: none */
} ta_resources;

static struct
{
	ta_resources *res;
	bool ready;
	bool failed;		/* could not be made on this device: not tried again */
	bool prepared;
	bool sortWanted;
	bool sortable;
	bool decoding;		/* the outputs are in the kernels' state (unordered access) */
	u32 index;			/* this frame's piece of the tables */
	u32 flags;
	TaGpuFrame frame;
	dx12_ta_draw draw;

	/* for measuring */
	u64 statFrames;		/* decoded */
	u64 statSkipped;	/* left to the CPU */
	u64 statBusy;		/* ...of them, because the GPU still had this frame's piece of the tables */
	u64 statWhy[TA_GPU_WHY_COUNT];
	u64 statShadowed;
	u64 statSorted;		/* decoded with a sorted pass, sorted on this side */
} ta;

/* @object let go: once the GPU is done with it (@later), or now */
static void let_go(IUnknown *object, bool later)
{
	if (object == NULL)
		return;
	if (later)
		dx12c_release(object);
	else
		IUnknown_Release(object);
}

static void free_resources(ta_resources *r, bool later)
{
	int i;
	if (r == NULL)
		return;
	let_go((IUnknown *)r->pool, later);
	let_go((IUnknown *)r->tables, later);
	for (i = 0; i < B_COUNT; i++)
		let_go((IUnknown *)r->buffers[i], later);
	let_go((IUnknown *)r->root, later);
	for (i = 0; i <= TAG_KERNELS; i++)
		let_go((IUnknown *)r->kernels[i], later);
	let_go((IUnknown *)r->dispatch, later);
	let_go((IUnknown *)r->sorted, later);
	let_go((IUnknown *)r->drawIndexed, later);
	free(r);
}

/* A committed buffer of @bytes in a heap of @props, in @state */
static bool make_buffer(ID3D12Device *device, const D3D12_HEAP_PROPERTIES *props, u64 bytes, D3D12_RESOURCE_FLAGS flags,
		D3D12_RESOURCE_STATES state, ID3D12Resource **out)
{
	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = MAX_U(bytes, 256);
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = flags;
	if (SUCCEEDED(ID3D12Device_CreateCommittedResource(device, props, D3D12_HEAP_FLAG_NONE, &desc, state, NULL,
			&iid_resource, (void **)out)))
		return true;
	*out = NULL;
	return false;
}

/* Not made after all: @r freed, and not tried again on this device */
static bool give_up(ta_resources *r, const char *why)
{
	if (why != NULL)
		INFO_LOG(RENDERER, "DX12: TA processing stays on the CPU (%s)\n", why);
	free_resources(r, false);
	ta.failed = true;
	return false;
}

static void transition(D3D12_RESOURCE_BARRIER *barrier, ID3D12Resource *resource, D3D12_RESOURCE_STATES *state, D3D12_RESOURCE_STATES to)
{
	memset(barrier, 0, sizeof(*barrier));
	barrier->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier->Transition.pResource = resource;
	barrier->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier->Transition.StateBefore = *state;
	barrier->Transition.StateAfter = to;
	*state = to;
}

static D3D12_GPU_VIRTUAL_ADDRESS gpu_address(ID3D12Resource *resource)
{
	return ID3D12Resource_GetGPUVirtualAddress(resource);
}

static bool ta_init(void)
{
	ID3D12Device *device = dx12c.device;
	TaGpuSizes sz;
	ta_resources *r;
	u32 strips, workWords, spares, trdSlots, frm0Bytes, instBytes, i;
	u32 work[9];
	u64 budget;
	D3D12_HEAP_PROPERTIES local, upload, cached;
	D3D12_RANGE none;
	HMODULE lib;
	SerializeRootSignature_t serialize;
	D3D12_ROOT_PARAMETER params[13];
	D3D12_ROOT_SIGNATURE_DESC desc;
	ID3DBlob *blob = NULL, *errors = NULL;
	D3D12_INDIRECT_ARGUMENT_DESC arg;
	D3D12_COMMAND_SIGNATURE_DESC sig;
	D3D12_RESOURCE_BARRIER barriers[3];
	dx12_upload up;
	ID3D12GraphicsCommandList *cmd;
	u32 *words;
	bool ok;
	int b, k;

	if (device == NULL)
		return false;
#ifdef MSB_FIRST
	/* (the kernels read the blocks little-endian) */
	ta.failed = true;
	return false;
#endif
	ta_gpu_sizes(&sz);
	r = (ta_resources *)calloc(1, sizeof(ta_resources));
	if (r == NULL)
		return give_up(NULL, "no memory");
	strips = sz.list[0] + sz.list[1] + sz.list[2];
	workWords = tag_work_layout(sz.verts, strips, sz.headers, work);
	/* The contexts: every list the GPU may still be behind with, one being
	 * drawn, and the few the game writes to at a time and renders to
	 * textures with; and spares, for a game that writes to more at once, as
	 * many as a thirty-second of the memory budget left allows, up to four
	 * more: a frame that finds them all taken is drawn from the CPU's parse,
	 * in a context of its own memory (tactx_gpu_pool_attach() makes one) */
	r->slice = ta_data_size();
	budget = (u64)vram_budget_available_kib(&dx12c.vram_budget) * 1024 / 32;
	spares = (u32)MIN_U(budget / r->slice, 4);
	r->slices = MIN_U(TABLE_PIECES + 4 + spares, 16);
	r->tableWords = (sz.table_words + 63) & ~63u;
	/* the translucent draw records: each group's count and room for a draw a vertex */
	trdSlots = sz.verts + sz.headers;

	r->sizes[B_VTX] = (u64)sz.verts * TAG_V_WORDS * 4;
	r->sizes[B_MOD] = (u64)sz.modtrig * 9 * 4 + 4;
	r->sizes[B_IDX] = (u64)sz.idx * 4;
	r->sizes[B_WORK] = (u64)workWords * 4;
	r->sizes[B_REC] = (u64)strips * TAG_C_WORDS * 4;
	/* (and a second command for each, for shadows: TAG_T_SHD_CMD) */
	r->sizes[B_CMD] = (u64)sz.headers * 2 * TAG_C_WORDS * 4;
	r->sizes[B_FRM] = TAG_O_WORDS * 4;
	r->sizes[B_TRD] = (u64)trdSlots * TAG_C_WORDS * 4;
	r->sizes[B_TRP] = (u64)sz.list[2] * TAG_P_WORDS * 4;
	r->sizes[B_ARGS] = 12 * 4;
	r->sizes[B_FRM0] = TAG_O_WORDS * 4;
	r->sizes[B_ZERO] = MAX_U((u64)trdSlots * TAG_C_WORDS * 4, (u64)sz.headers * 4);
	r->sizes[B_INST] = (u64)sz.list[2] * 4;

	memset(&local, 0, sizeof(local));
	local.Type = D3D12_HEAP_TYPE_DEFAULT;
	memset(&upload, 0, sizeof(upload));
	upload.Type = D3D12_HEAP_TYPE_UPLOAD;
	/* The SH4 reads the control words back and states copy the data:
	 * memory without the host's cache in front of it is too slow for that */
	memset(&cached, 0, sizeof(cached));
	cached.Type = D3D12_HEAP_TYPE_CUSTOM;
	cached.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
	cached.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;

	ok = make_buffer(device, &cached, (u64)r->slice * r->slices, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, &r->pool)
			&& SUCCEEDED(ID3D12Resource_Map(r->pool, 0, NULL, (void **)&r->poolMap));
	if (!ok)
		return give_up(r, "no host-cached memory");
	none.Begin = 0;
	none.End = 0;
	ok = make_buffer(device, &upload, (u64)r->tableWords * 4 * TABLE_PIECES, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &r->tables)
			&& SUCCEEDED(ID3D12Resource_Map(r->tables, 0, &none, (void **)&r->tablesMap));
	for (b = 0; ok && b < B_COUNT; b++)
	{
		r->states[b] = b < B_OUTPUTS ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST;
		ok = make_buffer(device, &local, r->sizes[b], b < B_OUTPUTS ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE,
				r->states[b], &r->buffers[b]);
	}
	if (!ok)
		return give_up(r, "its buffers could not be made");

	r->layout.words[0] = 0;		/* (each slice is bound from its start) */
	for (i = 0; i < 9; i++)
		r->layout.words[1 + i] = work[i];
	r->layout.words[10] = 0;
	r->layout.words[11] = sz.list[0] * TAG_C_WORDS;
	r->layout.words[12] = (sz.list[0] + sz.list[1]) * TAG_C_WORDS;

	/* The kernels' root signature: how many workgroups were dispatched
	 * (b0), the frame's data and its tables (t0, t1), the outputs (u0 to
	 * u8) and the sorted indices (u9), all root descriptors */
	lib = GetModuleHandleA("d3d12.dll");
	serialize = lib != NULL ? (SerializeRootSignature_t)GetProcAddress(lib, "D3D12SerializeRootSignature") : NULL;
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.RegisterSpace = 0;
	params[0].Constants.Num32BitValues = 1;
	for (i = 0; i < 2; i++)
	{
		params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[1 + i].Descriptor.ShaderRegister = i;
	}
	for (i = 0; i < 10; i++)
	{
		params[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[3 + i].Descriptor.ShaderRegister = i;
	}
	for (i = 0; i < 13; i++)
		params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	memset(&desc, 0, sizeof(desc));
	desc.NumParameters = 13;
	desc.pParameters = params;
	ok = serialize != NULL && SUCCEEDED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))
			&& SUCCEEDED(ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
					&iid_root_signature, (void **)&r->root));
	if (blob != NULL)
		ID3D10Blob_Release(blob);
	if (errors != NULL)
		ID3D10Blob_Release(errors);
	/* every kernel, now: none is made while a game runs */
	for (k = 1; ok && k <= TAG_KERNELS; k++)
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC cs;
		memset(&cs, 0, sizeof(cs));
		cs.pRootSignature = r->root;
		cs.CS.pShaderBytecode = dx12_ta_dxbc[k];
		cs.CS.BytecodeLength = dx12_ta_dxbc_size[k];
		ok = SUCCEEDED(ID3D12Device_CreateComputePipelineState(device, &cs, &iid_pipeline_state, (void **)&r->kernels[k]));
	}
	memset(&arg, 0, sizeof(arg));
	memset(&sig, 0, sizeof(sig));
	sig.NumArgumentDescs = 1;
	sig.pArgumentDescs = &arg;
	arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
	sig.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
	ok = ok && SUCCEEDED(ID3D12Device_CreateCommandSignature(device, &sig, NULL, &iid_command_signature, (void **)&r->dispatch));
	arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
	sig.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	ok = ok && SUCCEEDED(ID3D12Device_CreateCommandSignature(device, &sig, NULL, &iid_command_signature, (void **)&r->drawIndexed));
	if (!ok)
		return give_up(r, "its kernels could not be made");

	/* The sorted indices: three a triangle, for as many triangles as a frame
	 * can have indices, and as many again for the depth prepass
	 * (rend/sorter.h), whose triangles are some of those, and its checks
	 * after them (TAG_T_PREP); in the host's memory, uncached, which the
	 * GPU reads and the prepass's kernel writes as it is */
	if (ta.sortWanted)
	{
		D3D12_HEAP_PROPERTIES combined;
		const u64 slot = ((u64)sz.idx * 2 * 3 * 4 + 255) & ~(u64)255;
		memset(&combined, 0, sizeof(combined));
		combined.Type = D3D12_HEAP_TYPE_CUSTOM;
		combined.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
		combined.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
		if (make_buffer(device, &combined, slot * TABLE_PIECES, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_COMMON, &r->sorted)
				&& SUCCEEDED(ID3D12Resource_Map(r->sorted, 0, &none, (void **)&r->sortedMap)))
			r->sortSlot = slot;
		else
			DX12_RELEASE(r->sorted);
	}

	if (!tactx_gpu_pool_attach(r->poolMap, r->slice, r->slices))
		return give_up(r, NULL);
	ta_set_gpu_processing(true);
	if (!ta_gpu_processing())
	{
		tactx_gpu_pool_detach();
		return give_up(r, NULL);
	}

	/* What is copied in at the start of every decode, and the strips'
	 * numbers: through the ring once, now */
	frm0Bytes = TAG_O_WORDS * 4;
	instBytes = (u32)r->sizes[B_INST];
	if (!dx12c_upload(frm0Bytes + instBytes, 16, &up))
	{
		ta_set_gpu_processing(false);
		tactx_gpu_pool_detach();
		return give_up(r, "the upload ring is too small");
	}
	words = (u32 *)up.cpu;
	memset(words, 0, frm0Bytes);
	words[TAG_O_ZMAX] = 0x3F800000u;	/* fZ_max starts at 1 */
	for (i = 0; i < sz.list[2]; i++)
		words[TAG_O_WORDS + i] = i;
	cmd = dx12c_cmd();
	ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->buffers[B_FRM0], 0, up.buffer, up.offset, frm0Bytes);
	ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->buffers[B_INST], 0, up.buffer, up.offset + frm0Bytes, instBytes);
	transition(&barriers[0], r->buffers[B_FRM0], &r->states[B_FRM0], D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(&barriers[1], r->buffers[B_INST], &r->states[B_INST], D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	transition(&barriers[2], r->buffers[B_ARGS], &r->states[B_ARGS], D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	ID3D12GraphicsCommandList_ResourceBarrier(cmd, 3, barriers);
	r->states[B_ZERO] = D3D12_RESOURCE_STATE_COPY_DEST;
	dx12c_transition_resource(r->buffers[B_ZERO], &r->states[B_ZERO], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

	/* (without the CPU's room to sort in, a frame with a sorted pass is parsed and sorted as ever) */
	if (r->sortSlot != 0 && !ta_gpu_sort_reserve(true))
		r->sortSlot = 0;
	ta.sortable = r->sortSlot != 0;
	memset(&ta.draw, 0, sizeof(ta.draw));
	ta.draw.vertices.BufferLocation = gpu_address(r->buffers[B_VTX]);
	ta.draw.vertices.SizeInBytes = (UINT)r->sizes[B_VTX];
	ta.draw.vertices.StrideInBytes = sizeof(Vertex);
	ta.draw.indices.BufferLocation = gpu_address(r->buffers[B_IDX]);
	ta.draw.indices.SizeInBytes = (UINT)r->sizes[B_IDX];
	ta.draw.indices.Format = DXGI_FORMAT_R32_UINT;
	ta.draw.modtrig.BufferLocation = gpu_address(r->buffers[B_MOD]);
	ta.draw.modtrig.SizeInBytes = (UINT)r->sizes[B_MOD];
	ta.draw.modtrig.StrideInBytes = sizeof(float) * 3;
	ta.draw.vertexResource = r->buffers[B_VTX];
	ta.draw.indexResource = r->buffers[B_IDX];
	ta.draw.modtrigResource = r->buffers[B_MOD];
	ta.draw.commands = r->buffers[B_CMD];
	ta.draw.shadowCommands = (u64)sz.headers * TAG_C_WORDS * 4;
	ta.draw.trDraws = r->buffers[B_TRD];
	ta.draw.trParams = gpu_address(r->buffers[B_TRP]);
	ta.draw.instances.BufferLocation = gpu_address(r->buffers[B_INST]);
	ta.draw.instances.SizeInBytes = (UINT)r->sizes[B_INST];
	ta.draw.instances.StrideInBytes = 4;
	ta.draw.frame = r->buffers[B_FRM];
	ta.draw.drawIndexed = r->drawIndexed;
	ta.res = r;
	ta.decoding = true;
	ta.ready = true;
	NOTICE_LOG(RENDERER, "DX12: TA processing on the GPU (%u frames of %u MB in host-cached memory, %u KB of tables a frame%s)\n",
			r->slices, r->slice >> 20, r->tableWords / 256, ta.sortable ? "; translucent triangles sorted from the frame's depths" : "");
	return true;
}

static void ta_log(const char *when)
{
	u32 shortFrames, allocated;
	tactx_gpu_pool_stats(&shortFrames, &allocated);
	NOTICE_LOG(RENDERER, "DX12: TA processing%s: %llu frames decoded and drawn on the GPU, %llu parsed on the CPU "
			"(%llu without a table, %llu frame skipping, %llu sorted on the CPU, %llu the header pass leaves to the parse, "
			"%llu might run out of room, %llu whose tables the GPU still had); pool: %u frames found it full, %u contexts allocated; "
			"%llu with shadows the GPU decides, %llu with a sorted pass\n",
			when, (unsigned long long)ta.statFrames, (unsigned long long)ta.statSkipped,
			(unsigned long long)ta.statWhy[TA_GPU_WHY_TABLE], (unsigned long long)ta.statWhy[TA_GPU_WHY_PARSE],
			(unsigned long long)ta.statWhy[TA_GPU_WHY_SORT], (unsigned long long)ta.statWhy[TA_GPU_WHY_WALK],
			(unsigned long long)ta.statWhy[TA_GPU_WHY_ROOM], (unsigned long long)ta.statBusy, shortFrames, allocated,
			(unsigned long long)ta.statShadowed, (unsigned long long)ta.statSorted);
}

void dx12_ta_term(void)
{
	if (!ta.ready)
		return;
	ta_log("");
	/* (the renderer is done with the GPU, or the context goes: the frames in the pool go on in memory of their own) */
	ta_set_gpu_processing(false);
	tactx_gpu_pool_detach();
	/* the buffers go once the GPU is done with them */
	free_resources(ta.res, dx12c.device != NULL);
	ta.res = NULL;
	ta.ready = false;
	ta.prepared = false;
	ta.sortable = false;
}

void dx12_ta_setup(bool wanted)
{
	if (ta.ready && wanted)
		return;
	if (ta.ready)
		dx12_ta_term();
	if (!wanted)
	{
		ta.failed = false;
		return;
	}
	if (ta.failed)
		return;
	ta_init();
}

bool dx12_ta_ready(void)
{
	return ta.ready;
}

void dx12_ta_want_sort(bool on)
{
	ta.sortWanted = on;
}

bool dx12_ta_can_sort(void)
{
	return ta.sortable;
}

const dx12_ta_draw *dx12_ta_get_draw(void)
{
	return &ta.draw;
}

void dx12_ta_begin_frame(void)
{
	if (!ta.ready)
		return;
	ta.prepared = false;
	/* every list up to the one the fence has reached is done, and the frames it read */
	tactx_gpu_done((u32)dx12c_completed());
}

bool dx12_ta_prepare(TA_context *ctx, u32 want, bool *rv)
{
	ta_resources *r = ta.res;
	ta.prepared = false;
	if (ctx->pool < 0 || ctx->rend.isRenderFramebuffer)
	{
		/* (a frame that found the pool full is in memory the GPU cannot read) */
		ta.statSkipped++;
		ta.statWhy[TA_GPU_WHY_TABLE]++;
		return false;
	}
	/* This frame's piece of the tables, if the GPU is done with the frame
	 * that last read it; if not, the frame is parsed (never waited for) */
	ta.index = r->serial % TABLE_PIECES;
	if (r->pieceFence[ta.index] > dx12c_completed())
	{
		ta.statSkipped++;
		ta.statBusy++;
		return false;
	}
	ta.flags = want;
	ta.prepared = ta_gpu_prepare(ctx, &r->layout, r->tablesMap + (size_t)ta.index * r->tableWords, r->tableWords, &ta.frame, ta.flags);
	if (!ta.prepared)
	{
		ta.statSkipped++;
		ta.statWhy[ta.frame.why < TA_GPU_WHY_COUNT ? ta.frame.why : TA_GPU_WHY_WALK]++;
		return false;
	}
	r->serial++;
	*rv = ta.frame.rv;
	if (ta_gpu_sorted_indices() != 0)
		ta.statSorted++;
	if (ta.sortable)
	{
		ta.draw.sorted.BufferLocation = gpu_address(r->sorted) + ta.index * r->sortSlot;
		ta.draw.sorted.SizeInBytes = (UINT)r->sortSlot;
		ta.draw.sorted.Format = DXGI_FORMAT_R32_UINT;
	}
	if (ta.frame.shadows != 0)
		ta.statShadowed++;
	/* (nothing to draw: nothing to decode) */
	if (!ta.frame.rv)
		ta.prepared = false;
	return true;
}

/* The outputs into the kernels' state (@decoding), or into the states the
 * draws read them in: transitions all at once */
static void ta_states(bool decoding)
{
	static const D3D12_RESOURCE_STATES read[B_OUTPUTS] = {
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,	/* vertices */
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,	/* modifier volume triangles */
		D3D12_RESOURCE_STATE_INDEX_BUFFER,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,			/* the work buffer: the kernels' own */
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,			/* the records: as well */
		D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,			/* the commands */
		D3D12_RESOURCE_STATE_COPY_SOURCE,			/* the frame's words: 1/fZ_max copied out */
		D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,			/* the translucent draw records */
		(D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
	};
	ta_resources *r = ta.res;
	D3D12_RESOURCE_BARRIER barriers[B_OUTPUTS];
	D3D12_RESOURCE_STATES sortedTo;
	UINT n = 0;
	int b;
	if (decoding == ta.decoding)
		return;
	ta.decoding = decoding;
	for (b = 0; b < B_OUTPUTS; b++)
	{
		const D3D12_RESOURCE_STATES to = decoding ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : read[b];
		if (r->states[b] == to)
			continue;
		transition(&barriers[n++], r->buffers[b], &r->states[b], to);
	}
	sortedTo = decoding ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_INDEX_BUFFER;
	if (r->sorted != NULL && r->sortedState != sortedTo)
	{
		D3D12_RESOURCE_BARRIER sortedBarrier;
		transition(&sortedBarrier, r->sorted, &r->sortedState, sortedTo);
		ID3D12GraphicsCommandList_ResourceBarrier(dx12c_cmd(), 1, &sortedBarrier);
	}
	if (n != 0)
		ID3D12GraphicsCommandList_ResourceBarrier(dx12c_cmd(), n, barriers);
}

void dx12_ta_for_kernels(void)
{
	ta_states(true);
}

/* A kernel's dispatch, and what it wrote seen by the next */
static void dispatch(ID3D12GraphicsCommandList *cmd, int k, u32 groups)
{
	D3D12_RESOURCE_BARRIER step;
	memset(&step, 0, sizeof(step));
	step.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	ID3D12GraphicsCommandList_SetPipelineState(cmd, ta.res->kernels[k]);
	if (groups != 0)
		ID3D12GraphicsCommandList_Dispatch(cmd, groups, 1, 1);
	ID3D12GraphicsCommandList_ResourceBarrier(cmd, 1, &step);
}

/* (the dispatches' arguments, a copy of the frame's: TAG_O_ARGS64 on) */
static void indirect(ID3D12GraphicsCommandList *cmd, int k, u32 at)
{
	D3D12_RESOURCE_BARRIER step;
	memset(&step, 0, sizeof(step));
	step.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	ID3D12GraphicsCommandList_SetPipelineState(cmd, ta.res->kernels[k]);
	ID3D12GraphicsCommandList_ExecuteIndirect(cmd, ta.res->dispatch, 1, ta.res->buffers[B_ARGS], (u64)(at - TAG_O_ARGS64) * 4, NULL, 0);
	ID3D12GraphicsCommandList_ResourceBarrier(cmd, 1, &step);
}

/* (a dispatch of so many workgroups at most, each then taking every so many: told how many there are) */
static void loop(ID3D12GraphicsCommandList *cmd, int k, u32 count)
{
	const u32 groups = MIN_U(count, 65535u);
	ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(cmd, 0, groups, 0);
	dispatch(cmd, k, groups);
}

void dx12_ta_record(TA_context *ctx)
{
	ta_resources *r = ta.res;
	ID3D12GraphicsCommandList *cmd;
	D3D12_RESOURCE_BARRIER barriers[3];
	const TaGpuFrame *frame = &ta.frame;
	bool sorted;
	u32 checks, scanGroups;
	UINT n = 0;
	u64 list;
	int b;

	if (!ta.prepared)
		return;
	ta.prepared = false;
	cmd = dx12c_cmd();
	/* The outputs are one copy: whatever read the last frame's (its draws)
	 * is done before they are written again - the transitions say so */
	ta_states(true);

	/* The frame's words as a decode starts them, the slots' heads and the
	 * translucent draw records zeroed: copies, the targets in COPY_DEST */
	transition(&barriers[n++], r->buffers[B_FRM], &r->states[B_FRM], D3D12_RESOURCE_STATE_COPY_DEST);
	if (frame->slots != 0)
		transition(&barriers[n++], r->buffers[B_WORK], &r->states[B_WORK], D3D12_RESOURCE_STATE_COPY_DEST);
	if (frame->trd != 0)
		transition(&barriers[n++], r->buffers[B_TRD], &r->states[B_TRD], D3D12_RESOURCE_STATE_COPY_DEST);
	ID3D12GraphicsCommandList_ResourceBarrier(cmd, n, barriers);
	ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->buffers[B_FRM], 0, r->buffers[B_FRM0], 0, TAG_O_WORDS * 4);
	if (frame->slots != 0)
		ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->buffers[B_WORK], (u64)r->layout.words[9] * 4, r->buffers[B_ZERO], 0, (u64)frame->slots * 4);
	if (frame->trd != 0)
		ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->buffers[B_TRD], 0, r->buffers[B_ZERO], 0, (u64)frame->trd * TAG_C_WORDS * 4);
	n = 0;
	transition(&barriers[n++], r->buffers[B_FRM], &r->states[B_FRM], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (frame->slots != 0)
		transition(&barriers[n++], r->buffers[B_WORK], &r->states[B_WORK], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (frame->trd != 0)
		transition(&barriers[n++], r->buffers[B_TRD], &r->states[B_TRD], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ID3D12GraphicsCommandList_ResourceBarrier(cmd, n, barriers);

	ID3D12GraphicsCommandList_SetComputeRootSignature(cmd, r->root);
	ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(cmd, 0, 0, 0);
	ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cmd, 1, gpu_address(r->pool) + (u64)ctx->pool * r->slice);
	ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cmd, 2, gpu_address(r->tables) + (u64)ta.index * r->tableWords * 4);
	for (b = 0; b < B_OUTPUTS; b++)
		ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cmd, 3 + b, gpu_address(r->buffers[b]));
	/* The sorted translucent indices (TA_GPU_SORT), and the prepass's checks
	 * after them, are the CPU's: written now, in this frame's piece, before
	 * the kernel that reads them is (the host's writes before the list is
	 * executed are the GPU's); u9 for the prepass's kernel */
	sorted = ta.sortable && ta_gpu_sorted_indices() != 0;
	checks = sorted ? ta_gpu_prepass_checks() : 0;
	if (sorted)
	{
		ta_gpu_sorted_write((u32 *)(r->sortedMap + ta.index * r->sortSlot));
		ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cmd, 12, gpu_address(r->sorted) + ta.index * r->sortSlot);
	}

	scanGroups = (frame->verts + TAG_SCAN_N - 1) / TAG_SCAN_N;
	dispatch(cmd, TAG_K_UNPACK, (frame->verts + frame->modtrig + 63) / 64);
	/* (the vertices as unpacked, before TAG_K_PROVOKE moves colours) */
	if (checks != 0)
		loop(cmd, TAG_K_PREPASS, checks);
	dispatch(cmd, TAG_K_COUNT, scanGroups);
	dispatch(cmd, TAG_K_COUNTSUM, 1);
	/* The arguments of the dispatches that go by strips, out of the frame's
	 * words: an indirect argument buffer is not one the kernels write */
	n = 0;
	transition(&barriers[n++], r->buffers[B_FRM], &r->states[B_FRM], D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(&barriers[n++], r->buffers[B_ARGS], &r->states[B_ARGS], D3D12_RESOURCE_STATE_COPY_DEST);
	ID3D12GraphicsCommandList_ResourceBarrier(cmd, n, barriers);
	ID3D12GraphicsCommandList_CopyBufferRegion(cmd, r->buffers[B_ARGS], 0, r->buffers[B_FRM], TAG_O_ARGS64 * 4, 9 * 4);
	n = 0;
	transition(&barriers[n++], r->buffers[B_FRM], &r->states[B_FRM], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	transition(&barriers[n++], r->buffers[B_ARGS], &r->states[B_ARGS], D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	ID3D12GraphicsCommandList_ResourceBarrier(cmd, n, barriers);

	dispatch(cmd, TAG_K_ENDS, scanGroups);
	indirect(cmd, TAG_K_SUMMARY, TAG_O_ARGS64);
	indirect(cmd, TAG_K_GROUPS, TAG_O_ARGSCH);
	dispatch(cmd, TAG_K_GROUPSUM, 1);
	indirect(cmd, TAG_K_STATES, TAG_O_ARGSCH);
	indirect(cmd, TAG_K_EMIT, TAG_O_ARGS64);
	dispatch(cmd, TAG_K_FINISH, (frame->slots + 63) / 64);
	if (frame->shadows != 0)
		loop(cmd, TAG_K_SHADOW, frame->shadows);
	if (frame->trd != 0)
		indirect(cmd, TAG_K_TRDRAW, TAG_O_ARGS64);
	if (screen_height > 480)
		indirect(cmd, TAG_K_BLEED, TAG_O_ARGS1);
	if (ta.flags & TA_GPU_PROVOKE)
		indirect(cmd, TAG_K_PROVOKE, TAG_O_ARGS1);

	/* for whatever draws from them */
	ta_states(false);
	/* (the pipeline bound is not one the context knows) */
	dx12c_forget_pipeline();
	/* the context's slice and this piece of the tables are read until this list is done */
	list = dx12c.fence_value + 1;
	ctx->gpu_serial = (u32)list;
	r->pieceFence[ta.index] = list;
	if (++ta.statFrames % 600 == 0)
		ta_log(" so far");
}

void dx12_ta_copy_depth_scale(ID3D12Resource *dst, u64 offset)
{
	ID3D12GraphicsCommandList_CopyBufferRegion(dx12c_cmd(), dst, offset, ta.res->buffers[B_FRM], TAG_O_ZSCALE * 4, 4);
}
