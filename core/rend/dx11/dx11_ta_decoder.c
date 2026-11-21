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
/* GPU-side TA processing in Direct3D 11: see dx11_ta_decoder.h */
#include "dx11_ta_decoder.h"
#include <stdlib.h>
#include <string.h>
#include "dx11context_lr.h"
#include "dx11_nvapi.h"
#include "rend/ta_gpu.h"

extern int screen_height;

/* The kernels, by TAG_K_*, as shader model 5.0 DXBC made from rend/ta_gpu.h
 * when the core was built: tools/rend/kernels_dx12.sh, which checks that
 * they still are its. Nothing is compiled when the core runs. */
#include "dx11_ta_dxbc.inc"

#define TA_CHECK(name, c) typedef char dx11_ta_check_##name[(c) ? 1 : -1]
TA_CHECK(kernels, sizeof(dx11_ta_dxbc) / sizeof(dx11_ta_dxbc[0]) == TAG_KERNELS + 1);
/* (the renderers draw from the records and commands as they are) */
TA_CHECK(record, TAG_C_WORDS * 4 == sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS));
TA_CHECK(vertex, TAG_V_WORDS * 4 == sizeof(Vertex));
TA_CHECK(pair, TAG_P_WORDS == 2);

#define MIN_U(a, b) ((a) < (b) ? (a) : (b))
#define MAX_U(a, b) ((a) > (b) ? (a) : (b))

/* The kernels' outputs, in the order of their unordered access views
 * (u0 to u8, as rend/ta_gpu.h has them); then what the decoder has of its
 * own. A compute shader of feature level 11.0 has eight views, u0 to u7:
 * the polygon words (u8) and the sorted indices (u9) are each written by
 * one kernel alone, which has them at u1 in place of the modifier volume
 * triangles that it does not touch (tools/rend/kernels_dx12.sh,
 * ta11_source_of()) and has them bound there (dx11_ta_record()) */
enum { B_VTX, B_MOD, B_IDX, B_WORK, B_REC, B_CMD, B_FRM, B_TRD, B_TRP, B_OUTPUTS,
	B_ARGS = B_OUTPUTS,	/* the dispatches' arguments, copied out of the frame's words */
	B_FRM0,			/* the frame's words as each decode starts (fZ_max 1, the rest 0) */
	B_ZERO,			/* zeroes, copied where a decode wants them */
	B_INST,			/* 0, 1, 2...: the translucent strips' numbers (dx11_ta_draw::instances) */
	B_SORT,			/* (TA_GPU_SORT) the sorted translucent indices, and the prepass's checks after them */
	B_COUNT };

/* The views bound for the kernels: u0 to u7 */
enum { UAV_SLOTS = 8 };
TA_CHECK(slots, (int)B_TRD + 1 == (int)UAV_SLOTS && (int)B_TRP == (int)UAV_SLOTS);

/* How many frames' tables there are room for: a frame's piece is written
 * again once the GPU is done with the decode that read it last */
enum { PIECES = 8 };

typedef struct ta_resources
{
	/* The frames' input, a dynamic buffer: PIECES pieces of tables, then a
	 * ring the frames' data are copied into, each where the last one's
	 * ended (or from the start, where it does not fit), over nothing the
	 * GPU may still read. Mapped without discarding where the device lets
	 * a buffer read through a view be (no_overwrite); discarded every frame
	 * otherwise, when the driver gives it new memory. */
	ID3D11Buffer *input;
	ID3D11ShaderResourceView *table_views[PIECES];
	ID3D11ShaderResourceView *ring_view;
	u32 table_bytes;		/* a piece's */
	u32 ring_base, ring_bytes;
	u32 ring_head;
	bool no_overwrite;
	bool mapped;			/* (the first map discards: nothing has been written yet) */
	/* What each piece's last decode read, and the event after it, polled
	 * (never waited for) to know when it is done */
	ID3D11Query *piece_done[PIECES];
	bool piece_busy[PIECES];
	u32 piece_ring[PIECES][2];	/* the ring's bytes it read: from, to */
	u32 serial;
	u32 piece;			/* this frame's */

	ID3D11Buffer *buffers[B_COUNT];
	ID3D11UnorderedAccessView *uavs[B_OUTPUTS];
	ID3D11UnorderedAccessView *sort_uav;
	ID3D11ShaderResourceView *tr_params;
	u64 sizes[B_COUNT];
	TaGpuLayout layout;
	ID3D11ComputeShader *kernels[TAG_KERNELS + 1];
	/* how many workgroups the kernels that go round were dispatched (b0),
	 * which HLSL does not tell them */
	ID3D11Buffer *groups;
	u32 groups_set;
	/* (TA_GPU_SORT) the CPU's sort's words, written where this maps it
	 * (discarded each frame), and copied by the GPU to the buffer the
	 * prepass's kernel changes in place */
	ID3D11Buffer *sort_upload;
	u32 sort_words;
	bool overlap;			/* NVAPI's UAV overlap for the last kernels */
} ta_resources;

static struct
{
	ta_resources *res;
	bool ready;
	bool failed;			/* could not be made on this device: not tried again */
	bool prepared;
	bool sort_wanted;
	bool sortable;
	u32 flags;
	TaGpuFrame frame;
	dx11_ta_draw draw;
	/* for measuring */
	u64 stat_frames;		/* decoded */
	u64 stat_skipped;		/* left to the CPU */
	u64 stat_busy;			/* ...of them, because the GPU still had the room for this frame's input */
	u64 stat_why[TA_GPU_WHY_COUNT];
	u64 stat_shadowed;
	u64 stat_sorted;		/* decoded with a sorted pass, sorted on this side */
	u64 stat_draw_failed;		/* multi-draws the driver said no to */
} ta;

static void free_resources(ta_resources *r)
{
	int i;
	if (r == NULL)
		return;
	DX11_RELEASE(r->input);
	for (i = 0; i < PIECES; i++)
	{
		DX11_RELEASE(r->table_views[i]);
		DX11_RELEASE(r->piece_done[i]);
	}
	DX11_RELEASE(r->ring_view);
	for (i = 0; i < B_COUNT; i++)
		DX11_RELEASE(r->buffers[i]);
	for (i = 0; i < B_OUTPUTS; i++)
		DX11_RELEASE(r->uavs[i]);
	DX11_RELEASE(r->sort_uav);
	DX11_RELEASE(r->tr_params);
	for (i = 0; i <= TAG_KERNELS; i++)
		DX11_RELEASE(r->kernels[i]);
	DX11_RELEASE(r->groups);
	DX11_RELEASE(r->sort_upload);
	free(r);
}

/* A buffer of @bytes: @bind, @misc; with @data, made with it */
static bool make_buffer(ID3D11Device *device, u64 bytes, UINT bind, UINT misc, D3D11_USAGE usage, const void *data,
		ID3D11Buffer **out)
{
	D3D11_BUFFER_DESC desc;
	D3D11_SUBRESOURCE_DATA initial;
	u64 rounded = (bytes + 15) & ~(u64)15;
	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = (UINT)MAX_U(rounded, 16);
	desc.Usage = usage;
	desc.BindFlags = bind;
	desc.CPUAccessFlags = usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
	desc.MiscFlags = misc;
	memset(&initial, 0, sizeof(initial));
	initial.pSysMem = data;
	if (FAILED(ID3D11Device_CreateBuffer(device, &desc, data != NULL ? &initial : NULL, out)))
	{
		*out = NULL;
		return false;
	}
	return true;
}

/* A view of @buffer as @count elements of @format from element @first */
static bool make_srv(ID3D11Device *device, ID3D11Buffer *buffer, DXGI_FORMAT format, u32 first, u32 count,
		ID3D11ShaderResourceView **out)
{
	D3D11_SHADER_RESOURCE_VIEW_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Format = format;
	desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	desc.Buffer.FirstElement = first;
	desc.Buffer.NumElements = MAX_U(count, 1);
	if (FAILED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)buffer, &desc, out)))
	{
		*out = NULL;
		return false;
	}
	return true;
}

static bool make_uav(ID3D11Device *device, ID3D11Buffer *buffer, u64 bytes, ID3D11UnorderedAccessView **out)
{
	D3D11_UNORDERED_ACCESS_VIEW_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Format = DXGI_FORMAT_R32_UINT;
	desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	desc.Buffer.NumElements = (UINT)MAX_U(bytes / 4, 1);
	if (FAILED(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource *)buffer, &desc, out)))
	{
		*out = NULL;
		return false;
	}
	return true;
}

static bool no(const char *why)
{
	NOTICE_LOG(RENDERER, "DX11: TA processing stays on the CPU (%s)\n", why);
	free_resources(ta.res);
	ta.res = NULL;
	ta.failed = true;
	return false;
}

static bool ta_init(void)
{
	ID3D11Device *device = dx11c.device;
	TaGpuSizes sz;
	ta_resources *r;
	u32 strips, work[9], work_words, trd_slots;
	u64 sort_words, most;
	/* the buffers the decode writes and the draws read: each a typed view of words, for both */
	static const UINT bind[B_OUTPUTS] = {
		D3D11_BIND_VERTEX_BUFFER, D3D11_BIND_VERTEX_BUFFER, D3D11_BIND_INDEX_BUFFER, 0, 0, 0, 0, 0, D3D11_BIND_SHADER_RESOURCE
	};
	bool ok = true;
	int b, p, k, i;
	u32 *words;
	D3D11_BUFFER_DESC cb;
	D3D11_SUBRESOURCE_DATA cb_initial;
	D3D11_FEATURE_DATA_D3D11_OPTIONS options;
	static const u32 zero[4] = { 0, 0, 0, 0 };

	if (device == NULL)
		return false;
	/* NVIDIA's multi-draw indirect, for the translucent groups' records;
	 * compute shaders with unordered access views of typed buffers, which
	 * feature level 11.0 has; the device's own context, which runs what it
	 * is given as it is given and maps a buffer without discarding it */
	if (dx11c.vendor != 0x10de)
		return no("GPU TA processing in Direct3D 11 needs an NVIDIA GPU");
	if (!dx11_nvapi_multi_draw_available())
		return no("no NVAPI multi-draw indirect: GPU TA processing in Direct3D 11 needs NVIDIA's driver with NVAPI");
	if (dx11c.feature_level < D3D_FEATURE_LEVEL_11_0)
		return no("feature level 11.0 needed");
	if (!dx11c.immediate)
		return no("the frontend's context is a deferred one");

	ta_gpu_sizes(&sz);
	r = ta.res = (ta_resources *)calloc(1, sizeof(ta_resources));
	if (r == NULL)
		return no("no memory");
	strips = sz.list[0] + sz.list[1] + sz.list[2];
	work_words = tag_work_layout(sz.verts, strips, sz.headers, work);
	/* the translucent draw records: each group's count and room for a draw a vertex */
	trd_slots = sz.verts + sz.headers;

	r->sizes[B_VTX] = (u64)sz.verts * TAG_V_WORDS * 4;
	r->sizes[B_MOD] = (u64)sz.modtrig * 9 * 4 + 4;
	r->sizes[B_IDX] = (u64)sz.idx * 4;
	r->sizes[B_WORK] = (u64)work_words * 4;
	r->sizes[B_REC] = (u64)strips * TAG_C_WORDS * 4;
	/* (and a second command for each, for shadows: TAG_T_SHD_CMD) */
	r->sizes[B_CMD] = (u64)sz.headers * 2 * TAG_C_WORDS * 4;
	r->sizes[B_FRM] = TAG_O_WORDS * 4;
	r->sizes[B_TRD] = (u64)trd_slots * TAG_C_WORDS * 4;
	r->sizes[B_TRP] = (u64)sz.list[2] * TAG_P_WORDS * 4;
	r->sizes[B_ARGS] = 12 * 4;
	r->sizes[B_FRM0] = TAG_O_WORDS * 4;
	r->sizes[B_ZERO] = MAX_U((u64)trd_slots * TAG_C_WORDS * 4, (u64)sz.headers * 4);
	r->sizes[B_INST] = (u64)sz.list[2] * 4;
	/* the sorted indices: three a triangle, for as many triangles as a
	 * frame can have indices, and as many again for the depth prepass
	 * (rend/sorter.h), whose triangles are some of those; and the
	 * prepass's checks after them (TAG_T_PREP), a word and as many checks
	 * as there are translucent strips at most */
	sort_words = (u64)sz.idx * 2 * 3 + 1 + (u64)sz.list[2] * TAG_PC_WORDS;
	r->sizes[B_SORT] = ta.sort_wanted ? sort_words * 4 : 0;

	for (b = 0; ok && b < B_OUTPUTS; b++)
	{
		const UINT misc = b == B_CMD || b == B_TRD ? D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS : 0;
		ok = make_buffer(device, r->sizes[b], bind[b] | D3D11_BIND_UNORDERED_ACCESS, misc, D3D11_USAGE_DEFAULT, NULL, &r->buffers[b])
				&& make_uav(device, r->buffers[b], r->sizes[b], &r->uavs[b]);
	}
	ok = ok && make_srv(device, r->buffers[B_TRP], DXGI_FORMAT_R32G32_UINT, 0, sz.list[2], &r->tr_params);
	/* (a buffer with no binding at all is not one every driver makes: the arguments' can be read as well) */
	ok = ok && make_buffer(device, r->sizes[B_ARGS], D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS,
			D3D11_USAGE_DEFAULT, NULL, &r->buffers[B_ARGS]);
	/* What is copied in at the start of every decode, and the strips'
	 * numbers: made with what they hold, now */
	most = MAX_U(r->sizes[B_ZERO], MAX_U(r->sizes[B_INST], r->sizes[B_FRM0]));
	words = (u32 *)calloc((size_t)(most / 4 + 4), sizeof(u32));
	ok = ok && words != NULL;
	ok = ok && make_buffer(device, r->sizes[B_ZERO], D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_IMMUTABLE, words, &r->buffers[B_ZERO]);
	if (words != NULL)
		words[TAG_O_ZMAX] = 0x3F800000u;	/* fZ_max starts at 1 */
	ok = ok && make_buffer(device, r->sizes[B_FRM0], D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_IMMUTABLE, words, &r->buffers[B_FRM0]);
	if (ok)
		for (i = 0; i < (int)sz.list[2]; i++)
			words[i] = (u32)i;
	ok = ok && make_buffer(device, r->sizes[B_INST], D3D11_BIND_VERTEX_BUFFER, 0, D3D11_USAGE_IMMUTABLE, words, &r->buffers[B_INST]);
	free(words);
	memset(&cb, 0, sizeof(cb));
	cb.ByteWidth = 16;
	cb.Usage = D3D11_USAGE_DEFAULT;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	memset(&cb_initial, 0, sizeof(cb_initial));
	cb_initial.pSysMem = zero;
	if (ok && FAILED(ID3D11Device_CreateBuffer(device, &cb, &cb_initial, &r->groups)))
	{
		r->groups = NULL;
		ok = false;
	}
	r->groups_set = 0;
	if (!ok)
		return no("its buffers could not be made");

	/* The input: the tables' pieces, then the ring, at least the largest
	 * frame's data and half as much again */
	r->table_bytes = ((sz.table_words + 63) & ~63u) * 4;
	r->ring_base = r->table_bytes * PIECES;
	r->ring_bytes = ta_data_size() + ta_data_size() / 2;
	memset(&options, 0, sizeof(options));
	r->no_overwrite = SUCCEEDED(ID3D11Device_CheckFeatureSupport(device, D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)))
			&& options.MapNoOverwriteOnDynamicBufferSRV;
	ok = make_buffer(device, (u64)r->ring_base + r->ring_bytes, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_DYNAMIC, NULL, &r->input)
			&& make_srv(device, r->input, DXGI_FORMAT_R32_UINT, r->ring_base / 4, r->ring_bytes / 4, &r->ring_view);
	for (p = 0; ok && p < PIECES; p++)
	{
		D3D11_QUERY_DESC q;
		memset(&q, 0, sizeof(q));
		q.Query = D3D11_QUERY_EVENT;
		ok = make_srv(device, r->input, DXGI_FORMAT_R32_UINT, p * r->table_bytes / 4, r->table_bytes / 4, &r->table_views[p]);
		if (ok && FAILED(ID3D11Device_CreateQuery(device, &q, &r->piece_done[p])))
		{
			r->piece_done[p] = NULL;
			ok = false;
		}
	}
	if (!ok)
		return no("its input buffer could not be made");

	/* every kernel, now: none is made while a game runs */
	for (k = 1; ok && k <= TAG_KERNELS; k++)
		if (FAILED(ID3D11Device_CreateComputeShader(device, dx11_ta_dxbc[k], dx11_ta_dxbc_size[k], NULL, &r->kernels[k])))
		{
			r->kernels[k] = NULL;
			ok = false;
		}
	if (!ok)
		return no("its kernels could not be made");

	/* The sorted indices, in video memory the prepass's kernel changes in
	 * place, copied there from the dynamic buffer the CPU writes them in */
	if (ta.sort_wanted)
	{
		r->sort_words = (u32)sort_words;
		/* (a dynamic buffer has to be bound to something: never bound) */
		if (!make_buffer(device, r->sizes[B_SORT], D3D11_BIND_INDEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_USAGE_DEFAULT,
					NULL, &r->buffers[B_SORT])
				|| !make_uav(device, r->buffers[B_SORT], r->sizes[B_SORT], &r->sort_uav)
				|| !make_buffer(device, r->sizes[B_SORT], D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_DYNAMIC, NULL, &r->sort_upload))
		{
			DX11_RELEASE(r->buffers[B_SORT]);
			DX11_RELEASE(r->sort_uav);
			DX11_RELEASE(r->sort_upload);
			r->sort_words = 0;
		}
	}

	r->layout.words[0] = 0;		/* (each frame's: where its data are in the ring) */
	for (i = 0; i < 9; i++)
		r->layout.words[1 + i] = work[i];
	r->layout.words[10] = 0;
	r->layout.words[11] = sz.list[0] * TAG_C_WORDS;
	r->layout.words[12] = (sz.list[0] + sz.list[1]) * TAG_C_WORDS;

	ta_set_gpu_processing(true);
	if (!ta_gpu_processing())
		return no("the header pass's memory could not be made");
	/* (without the CPU's room to sort in, a frame with a sorted pass is parsed and sorted as ever) */
	ta.sortable = r->buffers[B_SORT] != NULL && ta_gpu_sort_reserve(true);
	r->overlap = dx11_nvapi_uav_overlap_available();

	ta.draw.vertices = r->buffers[B_VTX];
	ta.draw.indices = r->buffers[B_IDX];
	ta.draw.modtrig = r->buffers[B_MOD];
	ta.draw.commands = r->buffers[B_CMD];
	ta.draw.shadowCommands = sz.headers * TAG_C_WORDS * 4;
	ta.draw.trDraws = r->buffers[B_TRD];
	ta.draw.trParams = r->tr_params;
	ta.draw.instances = r->buffers[B_INST];
	ta.draw.sorted = ta.sortable ? r->buffers[B_SORT] : NULL;
	ta.ready = true;
	NOTICE_LOG(RENDERER, "DX11: TA processing on the GPU (NVIDIA's multi-draw indirect; %u KB of tables a frame, %u pieces, "
			"a ring of %u MB for the frames' data%s; the last kernels %s%s)\n",
			r->table_bytes / 1024, (u32)PIECES, r->ring_bytes >> 20, r->no_overwrite ? "" : ", discarded every frame",
			r->overlap ? "overlapped through NVAPI" : "one after another",
			ta.sortable ? "; translucent triangles sorted from the frame's depths" : "");
	return true;
}

static void ta_log(const char *when)
{
	NOTICE_LOG(RENDERER, "DX11: TA processing%s: %llu frames decoded and drawn on the GPU, %llu parsed on the CPU "
			"(%llu without a table, %llu frame skipping, %llu sorted on the CPU, %llu the header pass leaves to the parse, "
			"%llu might run out of room, %llu whose input the GPU still had); %llu with shadows the GPU decides, "
			"%llu with a sorted pass, %llu multi-draws refused\n",
			when, (unsigned long long)ta.stat_frames, (unsigned long long)ta.stat_skipped,
			(unsigned long long)ta.stat_why[TA_GPU_WHY_TABLE], (unsigned long long)ta.stat_why[TA_GPU_WHY_PARSE],
			(unsigned long long)ta.stat_why[TA_GPU_WHY_SORT], (unsigned long long)ta.stat_why[TA_GPU_WHY_WALK],
			(unsigned long long)ta.stat_why[TA_GPU_WHY_ROOM], (unsigned long long)ta.stat_busy,
			(unsigned long long)ta.stat_shadowed, (unsigned long long)ta.stat_sorted, (unsigned long long)ta.stat_draw_failed);
}

void dx11_ta_term(void)
{
	if (ta.res == NULL)
		return;
	if (ta.ready)
		ta_log("");
	/* (the renderer is done with the GPU, or the context goes) */
	ta_set_gpu_processing(false);
	/* Nothing of it is left bound: the context's record of what is bound
	 * must not name a buffer that goes (dx11context_lr.h) */
	if (dx11c.device_context != NULL)
	{
		dx11c_lock();
		dx11c_set_geometry(NULL, 0, 0, NULL);
		dx11c_set_vertex_buffer(NULL, 0, 2);
		dx11c_set_ps_resource(5, NULL);
		dx11c_unlock();
	}
	free_resources(ta.res);
	ta.res = NULL;
	ta.ready = false;
	ta.prepared = false;
	ta.sortable = false;
	memset(&ta.draw, 0, sizeof(ta.draw));
}

void dx11_ta_setup(bool wanted)
{
	if (ta.ready && wanted)
		return;
	if (ta.ready)
		dx11_ta_term();
	if (!wanted)
	{
		ta.failed = false;
		return;
	}
	if (ta.failed)
		return;
	ta_init();
}

bool dx11_ta_ready(void)
{
	return ta.ready;
}

void dx11_ta_want_sort(bool on)
{
	ta.sort_wanted = on;
}

bool dx11_ta_can_sort(void)
{
	return ta.sortable;
}

const dx11_ta_draw *dx11_ta_get_draw(void)
{
	return &ta.draw;
}

bool dx11_ta_prepare(TA_context *ctx, u32 want, bool *rv)
{
	ta_resources *r = ta.res;
	ID3D11DeviceContext *dc = dx11c.device_context;
	u32 piece, bytes, at = 0;
	size_t wrote;
	D3D11_MAPPED_SUBRESOURCE mapped;
	D3D11_MAP how;
	u8 *input;
	int p;

	ta.prepared = false;
	if (ctx->rend.isRenderFramebuffer)
	{
		ta.stat_skipped++;
		ta.stat_why[TA_GPU_WHY_TABLE]++;
		return false;
	}
	/* This frame's piece of the tables, and room in the ring for its data,
	 * where the GPU is done with the decodes that read them (polled, never
	 * waited for); if not, the frame is parsed. (Discarding the input every
	 * frame, the driver sees to that.) */
	piece = r->no_overwrite ? r->serial % PIECES : 0;
	wrote = (size_t)(tad_end(&ctx->tad) - ctx->tad.thd_root);
	bytes = (u32)MIN_U(wrote, (size_t)ta_data_size());
	if (r->no_overwrite)
	{
		const u32 room = (bytes + 255) & ~255u;
		bool busy;
		for (p = 0; p < PIECES; p++)
			if (r->piece_busy[p] && ID3D11DeviceContext_GetData(dc, (ID3D11Asynchronous *)r->piece_done[p], NULL, 0,
						D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
				r->piece_busy[p] = false;
		at = r->ring_head + room <= r->ring_bytes ? r->ring_head : 0;
		busy = r->piece_busy[piece];
		for (p = 0; p < PIECES && !busy; p++)
			busy = r->piece_busy[p] && at < r->piece_ring[p][1] && r->piece_ring[p][0] < at + room;
		if (busy)
		{
			ta.stat_skipped++;
			ta.stat_busy++;
			return false;
		}
	}
	how = r->no_overwrite && r->mapped ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
	if (FAILED(ID3D11DeviceContext_Map(dc, (ID3D11Resource *)r->input, 0, how, 0, &mapped)))
	{
		ta.stat_skipped++;
		ta.stat_why[TA_GPU_WHY_TABLE]++;
		return false;
	}
	input = (u8 *)mapped.pData;
	r->mapped = true;
	ta.flags = want;
	r->layout.words[0] = at / 4;
	ta.prepared = ta_gpu_prepare(ctx, &r->layout, (u32 *)(input + piece * r->table_bytes), r->table_bytes / 4, &ta.frame, ta.flags);
	/* (the data as far as the game wrote them: all the decode reads) */
	if (ta.prepared && ta.frame.rv)
		memcpy(input + r->ring_base + at, ctx->tad.thd_root, bytes);
	ID3D11DeviceContext_Unmap(dc, (ID3D11Resource *)r->input, 0);
	if (!ta.prepared)
	{
		ta.stat_skipped++;
		ta.stat_why[ta.frame.why < TA_GPU_WHY_COUNT ? ta.frame.why : TA_GPU_WHY_WALK]++;
		return false;
	}
	*rv = ta.frame.rv;
	if (ta_gpu_sorted_indices() != 0)
		ta.stat_sorted++;
	if (ta.frame.shadows != 0)
		ta.stat_shadowed++;
	/* (nothing to draw: nothing to decode) */
	if (!ta.frame.rv)
	{
		ta.prepared = false;
		return true;
	}
	r->piece = piece;
	r->piece_ring[piece][0] = at;
	r->piece_ring[piece][1] = at + ((bytes + 255) & ~255u);
	r->ring_head = r->piece_ring[piece][1];
	r->serial++;
	return true;
}

static void set_groups(u32 groups)
{
	u32 words[4];
	if (groups == ta.res->groups_set)
		return;
	words[0] = groups;
	words[1] = words[2] = words[3] = 0;
	ID3D11DeviceContext_UpdateSubresource(dx11c.device_context, (ID3D11Resource *)ta.res->groups, 0, NULL, words, 0, 0);
	ta.res->groups_set = groups;
}

static void set_box(D3D11_BOX *box, UINT left, UINT right)
{
	box->left = left;
	box->top = 0;
	box->front = 0;
	box->right = right;
	box->bottom = 1;
	box->back = 1;
}

/* Each dispatch after the one before: Direct3D 11 has a dispatch see what
 * the ones before it wrote to the views bound */
static void dispatch(int k, u32 groups)
{
	ID3D11DeviceContext *dc = dx11c.device_context;
	if (groups == 0)
		return;
	ID3D11DeviceContext_CSSetShader(dc, ta.res->kernels[k], NULL, 0);
	ID3D11DeviceContext_Dispatch(dc, groups, 1, 1);
}

/* (the dispatches' arguments, a copy of the frame's: TAG_O_ARGS64 on) */
static void indirect(int k, u32 at)
{
	ID3D11DeviceContext *dc = dx11c.device_context;
	ID3D11DeviceContext_CSSetShader(dc, ta.res->kernels[k], NULL, 0);
	ID3D11DeviceContext_DispatchIndirect(dc, ta.res->buffers[B_ARGS], (at - TAG_O_ARGS64) * 4);
}

/* (a dispatch of so many workgroups at most, each then taking every so many: told how many there are) */
static void loop(int k, u32 count)
{
	const u32 groups = MIN_U(count, 65535u);
	set_groups(groups);
	dispatch(k, groups);
}

/* (the two kernels that have another buffer at u1 than the modifier volume triangles: bound for each) */
static void at_u1(ID3D11UnorderedAccessView *view)
{
	ID3D11DeviceContext_CSSetUnorderedAccessViews(dx11c.device_context, 1, 1, &view, NULL);
}

TA_CHECK(args, TAG_O_ARGSCH == TAG_O_ARGS64 + 3 && TAG_O_ARGS1 == TAG_O_ARGS64 + 6);

void dx11_ta_record(TA_context *ctx)
{
	ta_resources *r = ta.res;
	ID3D11DeviceContext *dc = dx11c.device_context;
	D3D11_BOX box;
	bool sorted, overlapped;
	u32 checks, scan_groups, tail;
	ID3D11ShaderResourceView *inputs[2];
	ID3D11ShaderResourceView * const no_inputs[2] = { NULL, NULL };
	ID3D11UnorderedAccessView *uavs[UAV_SLOTS];
	ID3D11UnorderedAccessView *none[UAV_SLOTS];
	int b;

	(void)ctx;
	if (!ta.prepared)
		return;
	ta.prepared = false;

	/* The outputs are bound to be written: Direct3D 11 unbinds them from
	 * wherever they are bound to be read, behind the context's back, so
	 * the context unbinds them first (the vertices, the volumes' triangles
	 * and the indices; the per-pixel renderer's polygon words) */
	dx11c_set_geometry(NULL, 0, 0, NULL);
	dx11c_set_ps_resource(5, NULL);

	/* The frame's words as a decode starts them, the slots' heads and the
	 * translucent draw records zeroed: copies */
	set_box(&box, 0, TAG_O_WORDS * 4);
	ID3D11DeviceContext_CopySubresourceRegion(dc, (ID3D11Resource *)r->buffers[B_FRM], 0, 0, 0, 0,
			(ID3D11Resource *)r->buffers[B_FRM0], 0, &box);
	if (ta.frame.slots != 0)
	{
		box.right = ta.frame.slots * 4;
		ID3D11DeviceContext_CopySubresourceRegion(dc, (ID3D11Resource *)r->buffers[B_WORK], 0, r->layout.words[9] * 4, 0, 0,
				(ID3D11Resource *)r->buffers[B_ZERO], 0, &box);
	}
	if (ta.frame.trd != 0)
	{
		box.right = ta.frame.trd * TAG_C_WORDS * 4;
		ID3D11DeviceContext_CopySubresourceRegion(dc, (ID3D11Resource *)r->buffers[B_TRD], 0, 0, 0, 0,
				(ID3D11Resource *)r->buffers[B_ZERO], 0, &box);
	}
	/* The sorted translucent indices (TA_GPU_SORT), and the prepass's checks
	 * after them, are the CPU's: given to their buffer now, before the
	 * kernel that reads them runs */
	sorted = ta.sortable && ta_gpu_sorted_indices() != 0;
	checks = sorted ? ta_gpu_prepass_checks() : 0;
	if (sorted)
	{
		const u32 words = MIN_U(ta_gpu_sorted_words(), r->sort_words);
		D3D11_MAPPED_SUBRESOURCE mapped;
		D3D11_BOX part;
		set_box(&part, 0, words * 4);
		if (SUCCEEDED(ID3D11DeviceContext_Map(dc, (ID3D11Resource *)r->sort_upload, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		{
			ta_gpu_sorted_write((u32 *)mapped.pData);
			ID3D11DeviceContext_Unmap(dc, (ID3D11Resource *)r->sort_upload, 0);
			ID3D11DeviceContext_CopySubresourceRegion(dc, (ID3D11Resource *)r->buffers[B_SORT], 0, 0, 0, 0,
					(ID3D11Resource *)r->sort_upload, 0, &part);
		}
	}

	inputs[0] = r->ring_view;
	inputs[1] = r->table_views[r->piece];
	ID3D11DeviceContext_CSSetShaderResources(dc, 0, 2, inputs);
	for (b = 0; b < UAV_SLOTS; b++)
	{
		uavs[b] = r->uavs[b];
		none[b] = NULL;
	}
	ID3D11DeviceContext_CSSetUnorderedAccessViews(dc, 0, UAV_SLOTS, uavs, NULL);
	set_groups(0);
	ID3D11DeviceContext_CSSetConstantBuffers(dc, 0, 1, &r->groups);

	scan_groups = (ta.frame.verts + TAG_SCAN_N - 1) / TAG_SCAN_N;
	dispatch(TAG_K_UNPACK, (ta.frame.verts + ta.frame.modtrig + 63) / 64);
	/* (the vertices as unpacked, before TAG_K_PROVOKE moves colours) */
	if (checks != 0)
	{
		at_u1(r->sort_uav);
		loop(TAG_K_PREPASS, checks);
	}
	dispatch(TAG_K_COUNT, scan_groups);
	dispatch(TAG_K_COUNTSUM, 1);
	/* The arguments of the dispatches that go by strips, out of the frame's
	 * words, as they are once TAG_K_COUNTSUM has written them */
	set_box(&box, TAG_O_ARGS64 * 4, (TAG_O_ARGS64 + 9) * 4);
	ID3D11DeviceContext_CopySubresourceRegion(dc, (ID3D11Resource *)r->buffers[B_ARGS], 0, 0, 0, 0,
			(ID3D11Resource *)r->buffers[B_FRM], 0, &box);

	dispatch(TAG_K_ENDS, scan_groups);
	indirect(TAG_K_SUMMARY, TAG_O_ARGS64);
	indirect(TAG_K_GROUPS, TAG_O_ARGSCH);
	dispatch(TAG_K_GROUPSUM, 1);
	indirect(TAG_K_STATES, TAG_O_ARGSCH);
	indirect(TAG_K_EMIT, TAG_O_ARGS64);
	dispatch(TAG_K_FINISH, (ta.frame.slots + 63) / 64);
	/* The last four, after TAG_K_FINISH, do not depend on one another: each
	 * reads what the kernels before them wrote and writes words none of the
	 * others reads or writes -
	 *   TAG_K_SHADOW  reads the commands' first half (TAG_K_FINISH's), the
	 *                 indices and the vertices' x and y; writes the second
	 *                 half of the commands (TAG_T_SHD_CMD on)
	 *   TAG_K_TRDRAW  reads the records; writes the translucent draw
	 *                 records and the polygon words
	 *   TAG_K_BLEED   reads the records, the indices, the vertices' u, v
	 *                 and z; writes their u and v
	 *   TAG_K_PROVOKE reads the records, the indices and the vertices'
	 *                 four colours; writes those colours
	 * (and all of them the tables, the work buffer and the frame's words,
	 * which none of them writes). So where NVAPI has UAV overlap they run
	 * without the driver waiting between them; it ends before anything
	 * reads their outputs - the views unbound below, then the draws. */
	tail = (ta.frame.shadows != 0) + (ta.frame.trd != 0) + (screen_height > 480) + ((ta.flags & TA_GPU_PROVOKE) != 0);
	overlapped = r->overlap && tail > 1;
	if (overlapped)
		dx11_nvapi_begin_uav_overlap(dc);
	if (ta.frame.shadows != 0)
		loop(TAG_K_SHADOW, ta.frame.shadows);
	if (ta.frame.trd != 0)
	{
		at_u1(r->uavs[B_TRP]);
		indirect(TAG_K_TRDRAW, TAG_O_ARGS64);
	}
	if (screen_height > 480)
		indirect(TAG_K_BLEED, TAG_O_ARGS1);
	if (ta.flags & TA_GPU_PROVOKE)
		indirect(TAG_K_PROVOKE, TAG_O_ARGS1);
	if (overlapped)
		dx11_nvapi_end_uav_overlap(dc);

	/* for whatever draws from them: unbound from the kernels */
	ID3D11DeviceContext_CSSetUnorderedAccessViews(dc, 0, UAV_SLOTS, none, NULL);
	ID3D11DeviceContext_CSSetShaderResources(dc, 0, 2, no_inputs);
	/* the piece and the ring's bytes are read until the event after the decode */
	ID3D11DeviceContext_End(dc, (ID3D11Asynchronous *)r->piece_done[r->piece]);
	r->piece_busy[r->piece] = true;
	if (++ta.stat_frames % 600 == 0)
		ta_log(" so far");
}

void dx11_ta_copy_depth_scale(ID3D11Buffer *dst, u32 offset)
{
	D3D11_BOX box;
	set_box(&box, TAG_O_ZSCALE * 4, TAG_O_ZSCALE * 4 + 4);
	ID3D11DeviceContext_CopySubresourceRegion(dx11c.device_context, (ID3D11Resource *)dst, 0, offset, 0, 0,
			(ID3D11Resource *)ta.res->buffers[B_FRM], 0, &box);
}

void dx11_ta_draw_records(u32 slot, u32 count)
{
	const u32 stride = TAG_C_WORDS * 4;
	if (count == 0)
		return;
	if (!dx11_nvapi_multi_draw_indexed(dx11c.device_context, count, ta.res->buffers[B_TRD], slot * stride, stride)
			&& ta.stat_draw_failed++ == 0)
		WARN_LOG(RENDERER, "DX11: NVAPI's multi-draw indirect failed: a translucent draw group is not drawn\n");
}
