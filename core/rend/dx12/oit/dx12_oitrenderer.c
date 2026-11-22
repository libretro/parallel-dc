#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
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
/* The per-pixel renderer: the translucent polygons' pixels are put in
 * lists, sorted by depth and blended at the end, as the Direct3D 11
 * per-pixel renderer does it (dx11/oit/dx11_oitrenderer.c), step for
 * step:
 *
 *  - the opaque and punch-through polygons' depth, and the modifier
 *    volumes into the stencil;
 *  - their colour, where their depth is the one left, darkened where the
 *    stencil says they are in a volume;
 *  - the translucent polygons' pixels into the lists (or, in a list that
 *    is not to be sorted, drawn as they come);
 *  - the translucent modifier volumes, marking the pixels in the lists;
 *  - the lists put together over the colour, into the picture - or, with
 *    a render pass after this one, into a picture the next pass draws on.
 *
 * In bands when a frame has more translucent pixels than the lists hold
 * (choose_bands()), the counts read back without waiting through a ring of
 * them in a readback buffer, a frame's counts read once the fence says its
 * list is done.
 *
 * Direct3D 12's own: the lists, their heads and the counter are a table of
 * views the root signature has for them (DX12C_OIT_FIRST); the depth
 * and the stencil are read through views of the depth buffer's planes in
 * that table, each plane in the state its pass needs; what the shaders do
 * to the lists is ordered by barriers between the passes, which Direct3D
 * 11 did by itself; the translucent polygons' parameters go to the GPU in
 * the upload ring, as a root descriptor. The rest is the per-triangle
 * renderer's: the textures, renders to textures, the screen's pictures,
 * the last picture, the batching of the draws.
 *
 * SSAA (dx12_renderer.h), as the Vulkan per-pixel renderer has it: the
 * screen's opaque and punch-through polygons are drawn into two pictures of
 * samples (the base renderer's ssaa_color), with the samples' depth and
 * stencil, which the shaders read through the table of views for them
 * (DX12C_OIT_SSAA_FIRST); a translucent fragment of a sorted list is
 * put in the lists once a pixel, with the samples it covers and is in
 * front of the opaque depth at; the last render pass's lists are sorted
 * once a pixel, blended at each sample onto that sample's opaque colour,
 * and the samples resolved by the shared rule into the screen's picture,
 * in one pass; one that is not the last puts each sample together into the
 * other picture of samples, for the next to draw over. Translucent
 * modifier volumes count where they cover a pixel's first sample. */
#include "../dx12_renderer.h"
#include <string.h>
#include "../dx12_compute.h"
#include "../dx12_texture.h"
#include "../dx12_ta_decoder.h"
#include "../dx12_shader_flags.h"
#include "rend/oit_size.h"
#include "rend/rend_method.h"
#include "rend/ssaa_rate.h"
#include "rend/tileclip.h"
#include "rend/modvol_clip.h"
#include "rend/upscaler.h"
#include "rend/texconv.h"
#include "hw/pvr/ta.h"

#define PPW(w, f) TAW_GET(w, f)
#define MIN_U(a, b) ((a) < (b) ? (a) : (b))
#define MAX_U(a, b) ((a) > (b) ? (a) : (b))
#define MAX_L(a, b) ((a) > (b) ? (a) : (b))
#define MIN_L(a, b) ((a) < (b) ? (a) : (b))

/* (the most a resource can be: a quarter of the card's memory, but at
 * least 128 MB and at most 2 GB; mingw-w64's header has not the names) */
#ifndef D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM
#define D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM 128
#endif
#ifndef D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM
#define D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM 2048
#endif

extern int screen_width, screen_height;

static const IID iid_resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };

#define COUNT_SLOTS 4
#define COUNT_WORDS 256
#define MAX_BANDS 64

typedef struct oit_buffer
{
	ID3D12Resource *resource;
	D3D12_RESOURCE_STATES state;
} oit_buffer;

static struct
{
	/* the lists (OIT_FRAGMENT_BYTES each), their heads (R32_UINT, a picture's size), the counter (a word) */
	oit_buffer pixels;
	oit_buffer pointers;
	u32 pointersWidth, pointersHeight;
	oit_buffer counter;
	u64 bufferSize;
	u32 room;
	/* the counts read back: COUNT_SLOTS of COUNT_WORDS, mapped for good */
	oit_buffer counts;
	const u32 *countsMapped;
	u32 countWords[COUNT_SLOTS];
	u64 countFence[COUNT_SLOTS];	/* the fence value of the list that copied them */
	/* What the opaque and punch-through polygons are drawn into, two of
	 * them: with several render passes, each draws on what the last one
	 * put together */
	dx12_image pictures[2];
	u32 picturesWidth;
	u32 picturesHeight;
	/* the depth buffer the table's depth and stencil views are of */
	ID3D12Resource *depthViewed;
	/* A view of the lists' heads where the CPU has it, for them to be
	 * cleared with (SSAA, a render pass that is not the last: resolve()) */
	dx12_descriptor_heap clearHeap;
	oit_buffer noInstance;		/* a zero, the instances' buffer of a frame parsed on the CPU */
	/* A frame drawn in horizontal bands (draw_frame()): the band being
	 * drawn, in pixels of the target - from its top to its bottom, which is
	 * 0 when the frame is drawn whole */
	LONG bandTop, bandBottom;
	/* The counts of the frames in flight: the slot the frame being drawn
	 * copies into, and the bands each slot's frame was drawn in and whether
	 * it was a render to a texture */
	u32 countHead;
	u32 countBands[COUNT_SLOTS];
	bool countRtt[COUNT_SLOTS];
	/* how many bands frames are drawn in: the screen's [0], renders to a texture [1] */
	u32 oitBands[2];
	u32 oitRoomyFrames[2];
	/* With the HDR output and SSAA: each pixel's channel sums, which the
	 * screen's resolve stores (DX12_RATE_PIXEL_SUMS) for the encode
	 * (dx12c_encode_hdr()) */
	dx12_image sums;
	bool sumsStored;	/* by the last screen frame */
} oit;

/* The per-pixel renderer as a new one is: nothing made */
static void oit_construct(void)
{
	memset(&oit, 0, sizeof(oit));
	dx12_image_init(&oit.pictures[0]);
	dx12_image_init(&oit.pictures[1]);
	dx12_image_init(&oit.sums);
	oit.oitBands[0] = oit.oitBands[1] = 1;
}

/* ---- Rendering Method: Compute (dx12_compute.h) ----
 * The frames it can draw are drawn into the screen's pictures by it, and
 * the others below; made with SSAA's samples, when the renderer starts and
 * when the screen changes size */
static void init_compute(void)
{
	if (rend_compute_wanted && dx12r.device != NULL)
	{
		dx12_compute_set_gpu_draw(true);
		/* (for the temporal upscaler, with the walks that store the depth) */
		dx12_compute_init((u32)MAX_L(screen_width, 1), (u32)MAX_L(screen_height, 1), dx12r.ssaa_samples,
				upscaler_active() && dx12r.ssaa_samples == 1);
	}
	else
		dx12_compute_term();
}

/* The frame's words by the per-pixel renderer's own rules */
static const xform *compute_begin(void *self)
{
	(void)self;
	transform_matrix_calc(&dx12r.matrices, &pvrrc);
	return &dx12r.matrices.viewport;
}

static void compute_base_scissor(void *self, int *x, int *y, int *width, int *height)
{
	const D3D12_RECT *r = &dx12r.base_scissor;
	(void)self;
	dx12r_set_base_scissor();
	*x = r->left;
	*y = r->top;
	*width = r->right - r->left;
	*height = r->bottom - r->top;
}

static void compute_provoking_vertices(void *self)
{
	(void)self;
	dx12r_set_provoking_vertices();
}

static const compute_frame_renderer compute_rules =
{
	NULL,
	NULL,	/* (dx12_compute_prepare()'s own) */
	compute_begin,
	compute_base_scissor,
	compute_provoking_vertices
};

/* For a screen of @width x @height: rend/oit_size.h, and no more than a
 * resource can be */
static u64 planned_size(int width, int height)
{
	return oit_buffer_bytes((u32)MAX_L(width, 0), (u32)MAX_L(height, 0),
			vram_budget_available_kib(&dx12c.vram_budget), (u64)D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM * 1024u * 1024u);
}

/* ---- The buffers ---- */

static void drop_buffer(oit_buffer *buffer)
{
	if (buffer->resource != NULL)
	{
		dx12c_release((IUnknown *)buffer->resource);
		buffer->resource = NULL;
	}
	buffer->state = D3D12_RESOURCE_STATE_COMMON;
}

static bool make_buffer(oit_buffer *buffer, u64 bytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC desc;
	drop_buffer(buffer);
	memset(&heap, 0, sizeof(heap));
	heap.Type = heapType;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = bytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = flags;
	if (FAILED(ID3D12Device_CreateCommittedResource(dx12r.device, &heap, D3D12_HEAP_FLAG_NONE, &desc, state, NULL,
			&iid_resource, (void **)&buffer->resource)))
	{
		buffer->resource = NULL;
		return false;
	}
	buffer->state = state;
	return true;
}

/* The lists: one large buffer (@wanted bytes, or as large as the card will
 * make it), the counter, the counts' readback ring; their views into the
 * table */
static bool make_buffers(u64 wanted)
{
	ID3D12Device *device = dx12r.device;
	/* (the most a resource can be is a quarter of the card's memory, but
	 * at least 128 MB and at most 2 GB: it is asked for no larger than
	 * that, and halved until the card makes it, a whole number of
	 * pixels each time) */
	const u64 largest = (u64)D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM * 1024u * 1024u;
	const u64 smallest = (u64)D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM * 1024u * 1024u;
	u64 bytes = (wanted < largest ? wanted : largest) & ~(u64)(OIT_FRAGMENT_BYTES - 1);
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav, counterUav;
	bool ok = make_buffer(&oit.pixels, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	while (!ok && bytes > smallest)
	{
		const u64 half = (bytes / 2) & ~(u64)(OIT_FRAGMENT_BYTES - 1);
		bytes = half > smallest ? half : smallest;
		ok = make_buffer(&oit.pixels, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX12: pixel buffer creation failed\n");
		return false;
	}
	oit.bufferSize = wanted;
	oit.room = dx12r.pixel_buffer_room = (u32)(bytes / OIT_FRAGMENT_BYTES);
	if (bytes < wanted)
		NOTICE_LOG(RENDERER, "DX12: pixel buffer of %u MB\n", (u32)(bytes / 1024 / 1024));
	memset(&uav, 0, sizeof(uav));
	uav.Format = DXGI_FORMAT_UNKNOWN;
	uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = oit.room;
	uav.Buffer.StructureByteStride = OIT_FRAGMENT_BYTES;
	ID3D12Device_CreateUnorderedAccessView(device, oit.pixels.resource, NULL, &uav, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_FIRST + 0));
	ID3D12Device_CreateUnorderedAccessView(device, oit.pixels.resource, NULL, &uav, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_SSAA_FIRST + 0));

	/* the counter: a word, counted up by the shaders */
	if (!make_buffer(&oit.counter, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
	{
		WARN_LOG(RENDERER, "DX12: pixel counter creation failed\n");
		return false;
	}
	memset(&counterUav, 0, sizeof(counterUav));
	counterUav.Format = DXGI_FORMAT_R32_TYPELESS;
	counterUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	counterUav.Buffer.NumElements = 4;
	counterUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	ID3D12Device_CreateUnorderedAccessView(device, oit.counter.resource, NULL, &counterUav, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_FIRST + 2));
	ID3D12Device_CreateUnorderedAccessView(device, oit.counter.resource, NULL, &counterUav, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_SSAA_FIRST + 2));

	/* the counts read back, mapped once */
	if (oit.counts.resource == NULL)
	{
		D3D12_RANGE whole;
		void *p = NULL;
		if (!make_buffer(&oit.counts, COUNT_SLOTS * COUNT_WORDS * sizeof(u32), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST))
		{
			WARN_LOG(RENDERER, "DX12: pixel count readback buffer creation failed\n");
			return false;
		}
		whole.Begin = 0;
		whole.End = COUNT_SLOTS * COUNT_WORDS * sizeof(u32);
		if (FAILED(ID3D12Resource_Map(oit.counts.resource, 0, &whole, &p)))
			return false;
		oit.countsMapped = (const u32 *)p;
	}
	return true;
}

static bool make_pictures(u32 width, u32 height)
{
	ID3D12Device *device = dx12r.device;
	/* (made again for another size of screen; a render to a texture is not
	 * one, and a frame that wants more is drawn in bands) */
	const u64 wanted = planned_size(screen_width, screen_height);
	if (wanted != oit.bufferSize && !make_buffers(wanted))
		return false;
	/* (as large as a render of a screen to a texture, at the upscale renders
	 * to textures have, from the first: made while the game runs, for the
	 * first such render, otherwise) */
	{
		const u32 scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
		width = MAX_U(width, 640u * scale);
		height = MAX_U(height, 480u * scale);
	}

	/* the lists' heads, a picture of at least this size: none of them a list (EOL) */
	if (oit.pointers.resource == NULL || width > oit.pointersWidth || height > oit.pointersHeight)
	{
		static const UINT eol[4] = { ~0u, ~0u, ~0u, ~0u };
		D3D12_HEAP_PROPERTIES heap;
		D3D12_RESOURCE_DESC desc;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
		width = MAX_U(width, oit.pointersWidth);
		height = MAX_U(height, oit.pointersHeight);
		drop_buffer(&oit.pointers);
		oit.pointersWidth = oit.pointersHeight = 0;
		memset(&heap, 0, sizeof(heap));
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		memset(&desc, 0, sizeof(desc));
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R32_UINT;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		oit.pointers.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		if (FAILED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, oit.pointers.state, NULL,
				&iid_resource, (void **)&oit.pointers.resource))
				|| (oit.clearHeap.heap == NULL && !dx12_heap_init(&oit.clearHeap, device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, false, 0)))
		{
			WARN_LOG(RENDERER, "DX12: per-pixel list heads creation failed (%d x %d)\n", width, height);
			DX12_RELEASE(oit.pointers.resource);
			return false;
		}
		memset(&uav, 0, sizeof(uav));
		uav.Format = DXGI_FORMAT_R32_UINT;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		ID3D12Device_CreateUnorderedAccessView(device, oit.pointers.resource, NULL, &uav, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_FIRST + 1));
		ID3D12Device_CreateUnorderedAccessView(device, oit.pointers.resource, NULL, &uav, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_SSAA_FIRST + 1));
		ID3D12Device_CreateUnorderedAccessView(device, oit.pointers.resource, NULL, &uav, dx12_heap_cpu(&oit.clearHeap, 0));
		/* filled with EOL on the card (they are made as large as a render of
		 * a screen to a texture, which the upload ring may have no room for) */
		ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(dx12c_cmd(), dx12_heap_gpu(&dx12c.srv_heap, DX12C_OIT_FIRST + 1), dx12_heap_cpu(&oit.clearHeap, 0),
				oit.pointers.resource, eol, 0, NULL);
		dx12c_uav_barrier(oit.pointers.resource);
		oit.pointersWidth = width;
		oit.pointersHeight = height;
	}

	/* (with SSAA the screen is drawn into the pictures of samples: these
	 * are for the renders to textures) */
	if (!dx12r_ssaa_active() && (oit.pictures[0].resource == NULL || width > oit.picturesWidth || height > oit.picturesHeight))
	{
		int i;
		width = MAX_U(width, oit.picturesWidth);
		height = MAX_U(height, oit.picturesHeight);
		oit.picturesWidth = oit.picturesHeight = 0;
		for (i = 0; i < 2; i++)
		{
			dx12_image *picture = &oit.pictures[i];
			dx12r_drop_image(picture);
			picture->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
			picture->width = width;
			picture->height = height;
			if (!dx12_make_texture(device, width, height, 1, DX12FrameFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, picture->state, &picture->resource, NULL))
			{
				WARN_LOG(RENDERER, "DX12: per-pixel renderer picture creation failed (%d x %d)\n", width, height);
				return false;
			}
			picture->srv = dx12_heap_alloc(&dx12c.srv_heap);
			picture->rtv = dx12_heap_alloc(&dx12c.rtv_heap);
			if (picture->srv == ~0u || picture->rtv == ~0u)
			{
				dx12r_drop_image(picture);
				return false;
			}
			dx12c_make_srv(picture->resource, DX12FrameFormat, 1, picture->srv);
			ID3D12Device_CreateRenderTargetView(device, picture->resource, NULL, dx12_heap_cpu(&dx12c.rtv_heap, picture->rtv));
		}
		oit.picturesWidth = width;
		oit.picturesHeight = height;
	}
	if (!dx12r_ensure_depth(dx12r_ssaa_active() ? dx12r.target_width : width, dx12r_ssaa_active() ? dx12r.target_height : height))
		return false;
	/* the shaders' views of the depth buffer's planes, when it is another (SSAA's have their own) */
	if (!dx12r_ssaa_active() && oit.depthViewed != dx12r.depth.resource)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC view;
		memset(&view, 0, sizeof(view));
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Texture2D.MipLevels = 1;
		view.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
		view.Texture2D.PlaneSlice = 1;
		ID3D12Device_CreateShaderResourceView(device, dx12r.depth.resource, &view, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_FIRST + 3));
		view.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		view.Texture2D.PlaneSlice = 0;
		ID3D12Device_CreateShaderResourceView(device, dx12r.depth.resource, &view, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_FIRST + 4));
		oit.depthViewed = dx12r.depth.resource;
	}
	return true;
}

/* What the shaders that put the lists together need to know of each
 * translucent polygon: its TSP, depth function and mask, whether it takes
 * shadows, and its second volume's TSP */
static bool upload_tr_poly_params(void)
{
	D3D12_VERTEX_BUFFER_VIEW none;
	dx12_upload upload;
	const PolyParam *pp;
	u32 *words;
	int count, i;
	u32 size;
	/* (a frame decoded on the GPU: its words are the decode's, and its
	 * strips' numbers the instances' buffer's) */
	if (pvrrc.gpu)
	{
		const dx12_ta_draw *gpu = dx12_ta_get_draw();
		ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(dx12c_cmd(), 6, gpu->trParams);
		ID3D12GraphicsCommandList_IASetVertexBuffers(dx12c_cmd(), 1, 1, &gpu->instances);
		return true;
	}
	none.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(oit.noInstance.resource);
	none.SizeInBytes = 256;
	none.StrideInBytes = 4;
	ID3D12GraphicsCommandList_IASetVertexBuffers(dx12c_cmd(), 1, 1, &none);
	count = TA_LIST_USED(pvrrc.global_param_tr, PolyParam);
	size = (u32)((count > 1 ? count : 1) * 2 * sizeof(u32));
	if (!dx12c_upload(size, 16, &upload))
		return false;
	/* (written into the ring, in order: a pair of words a polygon, or one
	 * of zeros for none) */
	words = (u32 *)upload.cpu;
	if (count == 0)
		words[0] = words[1] = 0;
	pp = TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam);
	for (i = 0; i < count; i++, pp++, words += 2)
	{
		words[0] = (pp->tsp & 0xffff00c0) | ((pp->isp >> 16) & 0xe400) | ((pp->pcw >> 7) & 1);
		words[1] = pp->tsp1;
	}
	ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(dx12c_cmd(), 6, upload.gpu);
	return true;
}

/* ---- The counts ---- */

/* The counter back to 0: the lists are empty, the buffer filled from its start */
static void reset_count(void)
{
	dx12_upload zero;
	if (!dx12c_upload(4, 4, &zero))
		return;
	memset(zero.cpu, 0, 4);
	dx12c_transition_resource(oit.counter.resource, &oit.counter.state, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	ID3D12GraphicsCommandList_CopyBufferRegion(dx12c_cmd(), oit.counter.resource, 0, zero.buffer, zero.offset, 4);
	dx12c_transition_resource(oit.counter.resource, &oit.counter.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}

/* The counter, as the draws before left it, copied into @word of @slot of
 * the counts: nothing waits for it */
static void copy_count(u32 slot, u32 word)
{
	if (slot >= COUNT_SLOTS || word >= COUNT_WORDS || oit.counts.resource == NULL)
		return;
	dx12c_transition_resource(oit.counter.resource, &oit.counter.state, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	ID3D12GraphicsCommandList_CopyBufferRegion(dx12c_cmd(), oit.counts.resource, ((u64)slot * COUNT_WORDS + word) * sizeof(u32), oit.counter.resource, 0, 4);
	dx12c_transition_resource(oit.counter.resource, &oit.counter.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	if (word >= oit.countWords[slot])
		oit.countWords[slot] = word + 1;
	/* (the list being recorded is the next one submitted) */
	oit.countFence[slot] = dx12c.fence_value + 1;
}

/* The counts @slot holds, read if the GPU is done with the list that
 * copied them, and the largest of them in @largest: false when it is not
 * done, or when nothing was copied there. Never waits. */
static bool read_count(u32 slot, u32 *largest)
{
	const u32 *words;
	u32 i;
	if (slot >= COUNT_SLOTS || oit.countWords[slot] == 0 || oit.countsMapped == NULL)
		return false;
	if (dx12c_completed() < oit.countFence[slot])
		return false;
	words = oit.countsMapped + (size_t)slot * COUNT_WORDS;
	*largest = 0;
	for (i = 0; i < oit.countWords[slot]; i++)
		*largest = MAX_U(*largest, words[i]);
	oit.countWords[slot] = 0;
	return true;
}

/* The bands this frame is drawn in, from the counts of the oldest frame
 * whose list the GPU is done with: never this frame's, which nothing waits
 * for. More at once when that one had pixels that found no room; fewer
 * when a second of frames in a row would have fitted in fewer. Each band is
 * taken to need what the fullest one did, with a quarter more room. The
 * screen's frames and renders to a texture are counted apart. */
static u32 choose_bands(bool rtt)
{
	const u32 oldest = (oit.countHead + 1) % COUNT_SLOTS;	/* when all are in flight */
	u32 k;
	for (k = 0; k < COUNT_SLOTS; k++)
	{
		const u32 slot = (oldest + k) % COUNT_SLOTS;
		u32 fragments, drawnIn, wanted, was;
		u64 roomNow, frame, w;
		u32 *bands, *roomy;
		if (oit.countWords[slot] == 0)
			continue;
		if (!read_count(slot, &fragments))
			/* (still being drawn: the ones after it are younger) */
			break;
		drawnIn = MAX_U(MIN_U(oit.countBands[slot], MAX_BANDS), 1u);
		roomNow = oit.room > 1 ? oit.room : 1;
		frame = (u64)fragments * drawnIn;
		w = (frame + frame / 4 + roomNow - 1) / roomNow;
		wanted = (u32)(w < MAX_BANDS ? (w > 1 ? w : 1) : MAX_BANDS);
		bands = &oit.oitBands[oit.countRtt[slot] ? 1 : 0];
		roomy = &oit.oitRoomyFrames[oit.countRtt[slot] ? 1 : 0];
		was = *bands;
		if (fragments > roomNow)
		{
			u32 most = MAX_U(*bands, wanted);
			most = MAX_U(most, drawnIn + 1);
			*bands = MIN_U(most, MAX_BANDS);
			*roomy = 0;
		}
		else if (wanted < *bands)
		{
			if (++*roomy >= 60)
			{
				*bands = wanted;
				*roomy = 0;
			}
		}
		else
			*roomy = 0;
		if (*bands != was)
			NOTICE_LOG(RENDERER, "DX12 per-pixel renderer: %u translucent pixels in a band of %u%s, room for %u: %u band%s\n",
					fragments, drawnIn, oit.countRtt[slot] ? " (a render to a texture)" : "", (u32)roomNow, *bands, *bands > 1 ? "s" : "");
		break;
	}
	return oit.oitBands[rtt ? 1 : 0];
}

/* What the shaders did to the lists, done before what comes next */
static void lists_barrier(void)
{
	dx12c_uav_barrier(oit.pixels.resource);
	dx12c_uav_barrier(oit.pointers.resource);
	dx12c_uav_barrier(oit.counter.resource);
}

/* ---- The pipelines ---- */

/* A polygon's pipeline in a pass (oit_draw_poly()), but for its cull mode
 * and shading, which its shaders apply, and SSAA's: by its list, whether
 * the list is sorted, the ISP's depth mode and Z Write Disable, the TSP's
 * blend instructions */
static DX12PsoKey oit_poly_key(u32 listType, bool autosort, u32 pass, u32 depthMode, bool zWriteDis, u32 src, u32 dst)
{
	DX12PsoKey key;
	/* Depth is written in the depth pass. In the colour pass opaque and
	 * punch-through polygons are drawn where their depth is the one the
	 * depth pass left, and translucent ones that are sorted only into the
	 * lists; the translucent polygons that are not sorted are drawn there,
	 * in order, each tested against the depth the ones before it wrote, so
	 * they still write. */
	const bool colourPassWrites = pass == DX12_PASS_COLOR && !autosort && listType == ListType_Translucent;
	memset(&key, 0, sizeof(key));
	key.kind = DX12_PSO_OIT;
	key.pass = pass;
	key.depthBound = true;
	key.depthTest = true;
	if (pass == DX12_PASS_COLOR && !zWriteDis && listType != ListType_Translucent)
		key.depthFunc = 2;	/* equal */
	else if (listType == ListType_Punch_Through || autosort)
		key.depthFunc = 6;	/* greater or equal */
	else
		key.depthFunc = depthMode;
	if (pass != DX12_PASS_DEPTH && !colourPassWrites)
		key.depthWrite = false;
	/* Z Write Disable seems to be ignored for punch-through.
	 * Fixes Worms World Party, Bust-a-Move 4 and Re-Volt */
	else if (listType == ListType_Punch_Through)
		key.depthWrite = true;
	else
		key.depthWrite = !zWriteDis;
	key.stencil = pass == DX12_PASS_DEPTH && (listType == ListType_Opaque || listType == ListType_Punch_Through)
			? DX12Stencil_Shadow : DX12Stencil_None;
	/* Apparently punch-through polys support blending, or at least some combinations */
	if (listType == ListType_Punch_Through || pass == DX12_PASS_COLOR)
	{
		key.blend = true;
		key.writeColor = true;
		key.src = src;
		key.dst = dst;
	}
	return key;
}

/* Every pipeline the passes can draw with, made before a game draws (the
 * per-triangle renderer's are made by the context): each list in the
 * passes the render draws it in, with every depth mode and blend; the
 * lists put together, the translucent volumes. The thread makes them in
 * this order: the blends that read nothing of the frame first (one
 * pipeline for them all, dx12_pso_key_pack()) with the depth modes games
 * mostly use, then the rest. */
static u64 prepared_keys[8 * 8 * 2 * 64 + 4];

static void oit_prepare_pipelines(void)
{
	static const u8 modes[] = { 6, 7, 3, 1, 2, 4, 5, 0 };
	static const struct { u32 list; bool autosort; u32 pass; } draws[] = {
		{ ListType_Opaque, false, DX12_PASS_DEPTH }, { ListType_Punch_Through, false, DX12_PASS_DEPTH },
		{ ListType_Opaque, false, DX12_PASS_COLOR }, { ListType_Punch_Through, false, DX12_PASS_COLOR },
		{ ListType_Translucent, true, DX12_PASS_OIT }, { ListType_Translucent, false, DX12_PASS_COLOR },
		{ ListType_Translucent, false, DX12_PASS_DEPTH }, { ListType_Translucent, true, DX12_PASS_DEPTH },
	};
	size_t n = 0;
	u32 rare, m, d, zwd, src, dst, cull;
	DX12PsoKey k;
	for (rare = 0; rare < 2; rare++)
		for (m = 0; m < sizeof(modes); m++)
			for (d = 0; d < sizeof(draws) / sizeof(draws[0]); d++)
				for (zwd = 0; zwd < 2; zwd++)
					for (src = 0; src < 8; src++)
						for (dst = 0; dst < 8; dst++)
							if ((dx12_src_folds(src) && dx12_dst_folds(dst)) != (rare != 0))
							{
								const DX12PsoKey key = oit_poly_key(draws[d].list, draws[d].autosort, draws[d].pass, modes[m], zwd != 0, src, dst);
								prepared_keys[n++] = dx12_pso_key_pack(&key);
							}
	memset(&k, 0, sizeof(k));
	k.kind = DX12_PSO_OIT_FINAL;
	k.writeColor = true;
	k.depthFunc = 7;
	prepared_keys[n++] = dx12_pso_key_pack(&k);
	memset(&k, 0, sizeof(k));
	k.kind = DX12_PSO_OIT_MODVOL;
	k.depthFunc = 7;
	for (cull = 0; cull < 3; cull++)
	{
		k.cull = cull;
		prepared_keys[n++] = dx12_pso_key_pack(&k);
	}
	/* (each made once, where it first comes) */
	dx12_pso_prepare_many(prepared_keys, n);
}

/* ---- The draws ---- */

static void oit_cut_scissor(D3D12_RECT *rect)
{
	if (oit.bandBottom == 0)
		return;
	rect->top = MAX_L(rect->top, oit.bandTop);
	rect->bottom = MAX_L(MIN_L(rect->bottom, oit.bandBottom), rect->top);
}

static int cull_of(u32 isp)
{
	const u32 mode = PPW(isp, ISP_TSP_CullMode);
	return mode == 3 ? 2 : mode == 2 ? 1 : 0;
}

static void oit_draw_poly(u32 listType, bool autosort, u32 pass, const PolyParam *poly, u32 first, u32 count)
{
	D3D12_RECT scissorRect;
	const int tileClip = dx12r_set_tile_clip(poly->tileclip, &scissorRect);
	dx12_draw_state state;
	float trilinearAlpha = 1.f;
	const bool trilinear = PPW(poly->tsp, TSP_FilterMode) > 1 && PPW(poly->pcw, PCW_Texture) && listType != ListType_Punch_Through
			&& PPW(poly->tcw, TCW_MipMapped) == 1;
	const bool twoVolumes = poly->tsp1 != (u32)-1 || poly->tcw1 != (u32)-1;
	const bool keepAll = twoVolumes || PPW(poly->tsp, TSP_SrcSelect);
	const bool palette = tex_gpu_paletted(poly->tsp, poly->tcw, 0);
	float palette_index = 0.f;
	dx12_poly_constants constants;
	u32 flags = 0, oitFlags;
	u32 words[DX12C_POLY_CONSTANT_COUNT];
	DX12PsoKey key;
	state.scissor = dx12r_cut_scissor(tileClip == TILECLIP_OUTSIDE ? &scissorRect : &dx12r.base_scissor);

	if (trilinear)
	{
		trilinearAlpha = 0.25f * (PPW(poly->tsp, TSP_MipMapD) & 0x3);
		if (PPW(poly->tsp, TSP_FilterMode) == 2)
			/* Trilinear pass A */
			trilinearAlpha = 1.f - trilinearAlpha;
	}
	if (palette)
	{
		if (PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_4BPP)
			palette_index = ((float)(PPW(poly->tcw, TCW_PalSelect) << 4) + 0.5f) / 1024.f;
		else
			palette_index = ((float)((PPW(poly->tcw, TCW_PalSelect) >> 4) << 8) + 0.5f) / 1024.f;
	}

	memset(&constants, 0, sizeof(constants));
	constants.clipTest[0] = (float)scissorRect.left;
	constants.clipTest[1] = (float)scissorRect.top;
	constants.clipTest[2] = (float)scissorRect.right;
	constants.clipTest[3] = (float)scissorRect.bottom;
	constants.trilinearAlpha = trilinearAlpha;
	constants.paletteIndex = palette_index;
	if (listType == ListType_Punch_Through)
		flags |= DX12_PF_ALPHA_TEST;
	if ((poly->tileclip >> 28) == 3)
		flags |= DX12_PF_CLIP_INSIDE;
	if (PPW(poly->tsp, TSP_UseAlpha))
		flags |= DX12_PF_USE_ALPHA;
	if (PPW(poly->pcw, PCW_Texture))
		flags |= DX12_PF_TEXTURE;
	if (PPW(poly->tsp, TSP_IgnoreTexA) || PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_565)
		flags |= DX12_PF_IGNORE_TEX_A;
	flags |= (PPW(poly->tsp, TSP_ShadInstr) & 3) << DX12_PF_SHAD_INSTR_SHIFT;
	if (PPW(poly->pcw, PCW_Offset))
		flags |= DX12_PF_OFFSET;
	flags |= (PPW(poly->tsp, TSP_FogCtrl) & 3) << DX12_PF_FOG_SHIFT;
	if (PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_BUMPMAP)
		flags |= DX12_PF_BUMPMAP;
	if (PPW(poly->tsp, TSP_ColorClamp))
		flags |= DX12_PF_CLAMP;
	if (palette)
		flags |= DX12_PF_PALETTE;
	constants.flags = flags;
	/* The per-pixel pass drops a fragment whose blend would leave the
	 * pixel as it is. It cannot tell that when the source is the second
	 * accumulation buffer, or when the polygon has a second volume and
	 * which blend applies is known only when the lists are put
	 * together: those get a blend (8) that none of its cases take. */
	oitFlags = ((keepAll ? 8u : PPW(poly->tsp, TSP_SrcInstr)) << DX12_OF_SRC_SHIFT) | ((keepAll ? 8u : PPW(poly->tsp, TSP_DstInstr)) << DX12_OF_DST_SHIFT);
	if (twoVolumes)
	{
		oitFlags |= DX12_OF_TWO_VOLUMES | (PPW(poly->tsp1, TSP_SrcInstr) << DX12_OF_SRC1_SHIFT) | (PPW(poly->tsp1, TSP_DstInstr) << DX12_OF_DST1_SHIFT)
				| (PPW(poly->tsp1, TSP_ShadInstr) << DX12_OF_SHAD_INSTR1_SHIFT) | (PPW(poly->tsp1, TSP_FogCtrl) << DX12_OF_FOG1_SHIFT);
		if (PPW(poly->tsp1, TSP_UseAlpha))
			oitFlags |= DX12_OF_USE_ALPHA1;
		if (PPW(poly->tsp1, TSP_IgnoreTexA))
			oitFlags |= DX12_OF_IGNORE_TEX_A1;
	}
	constants.oitFlags = oitFlags;
	constants.texture = DX12_SRV_NONE;
	constants.texture1 = DX12_SRV_NONE;
	if (PPW(poly->pcw, PCW_Texture))
	{
		if (poly->texid != 0)
			constants.texture = dx12_texture_srv((const dx12_texture *)(uintptr_t)poly->texid);
		if (twoVolumes && poly->texid1 != 0)
			constants.texture1 = dx12_texture_srv((const dx12_texture *)(uintptr_t)poly->texid1);
	}
	constants.sampler = dx12_sampler_index(poly->tsp);
	constants.sampler1 = dx12_sampler_index(poly->tsp1);
	/* (of a frame decoded on the GPU, the strip's number is its draw record's: the instance's) */
	constants.polyNumber = listType == ListType_Translucent && !pvrrc.gpu
			? (u32)(poly - TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam)) : 0;

	/* Depth, stencil and blend */
	key = oit_poly_key(listType, autosort, pass, PPW(poly->isp, ISP_TSP_DepthMode), PPW(poly->isp, ISP_TSP_ZWriteDis) != 0,
			PPW(poly->tsp, TSP_SrcInstr), PPW(poly->tsp, TSP_DstInstr));
	key.gouraud = PPW(poly->pcw, PCW_Gouraud) != 0;
	state.stencilRef = key.stencil == DX12Stencil_Shadow && PPW(poly->pcw, PCW_Shadow) != 0 ? 0x80 : 0;
	if (key.blend)
	{
		/* (the factors the shader applies, and the pipeline has as one and its second colour) */
		if (dx12_src_folds(key.src))
			constants.flags |= DX12_PF_SRC_FOLD | (key.src << DX12_PF_SRC_FOLD_SHIFT);
		if (dx12_dst_folds(key.dst))
			constants.flags |= DX12_PF_DST_FOLD | (key.dst << DX12_PF_DST_FOLD_SHIFT);
	}
	key.cull = (u32)cull_of(poly->isp);
	/* (culled by the pixel shader: the pipeline culls nothing, dx12_pso_key_pack()) */
	constants.flags |= key.cull << DX12_PF_CULL_SHIFT;
	if (key.gouraud)
		constants.flags |= DX12_PF_GOURAUD;
	/* SSAA: shaded at each sample, or once a pixel - into the lists always */
	key.samples = dx12c_target_samples();
	if (key.samples != 0)
		key.rate = pass == DX12_PASS_OIT || ssaa_single_rate(poly) ? DX12_RATE_PIXEL : DX12_RATE_SAMPLE;
	state.pso = dx12_pso_get(&key);
	state.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;

	memcpy(words, &constants, sizeof(words));
	/* (a frame decoded on the GPU: a draw group by its arguments there) */
	if (pvrrc.gpu)
		dx12r_draw_gpu(&state, words, listType, false, first, count);
	else
		dx12r_draw(&state, words, count, first);
}

static void oit_draw_list(u32 listType, bool autosort, u32 pass, const PolyParam *polys, u32 first, u32 last)
{
	const PolyParam *pp_end = polys + last;
	const PolyParam *pp;
	for (pp = polys + first; pp != pp_end; pp++)
		if (pp->count > 2)
			oit_draw_poly(listType, autosort, pass, pp, pp->first, pp->count);
	dx12r_flush_batch();
}

/* The translucent modifier volumes' draws change the lists from their
 * shaders, and nothing orders what the shaders of two draws do to the same
 * fragment but a UAV barrier between them: a GPU runs draws at once, in any
 * order, where a software device runs them one after the other. They go
 * through here. Built with RACE_TEST (tools/rend/speckle_test.sh; never in
 * a release) the draws between two barriers are recorded a triangle at a
 * time, the draws' triangles taken in turn, the last draw's first: one
 * draw's shaders run while another's are half done, as on a GPU, and a
 * picture that changes then depends on an order nothing keeps. */
#ifdef RACE_TEST
typedef struct list_draw_held
{
	ID3D12PipelineState *pso;
	D3D12_RECT scissor;
	u32 constants[DX12C_POLY_CONSTANT_COUNT];
	u32 vertices, first;
} list_draw_held;
static list_draw_held held[256];
static u32 held_count;
#endif

static void list_draws_record(ID3D12PipelineState *pso, const D3D12_RECT *scissor, const u32 *constants, u32 vertices, u32 first)
{
	dx12c_set_scissor(scissor);
	dx12c_set_pipeline(pso);
	dx12c_set_root_constants(constants, DX12C_POLY_CONSTANT_COUNT);
	ID3D12GraphicsCommandList_DrawInstanced(dx12c_cmd(), vertices, 1, first, 0);
}

static void list_draws_flush(void)
{
#ifdef RACE_TEST
	u32 v, more, i;
	for (v = 0, more = held_count; more != 0; v += 3)
	{
		more = 0;
		for (i = held_count; i-- > 0; )
			if (v < held[i].vertices)
			{
				list_draws_record(held[i].pso, &held[i].scissor, held[i].constants, 3, held[i].first + v);
				more++;
			}
	}
	held_count = 0;
#endif
}

static void list_draws_draw(ID3D12PipelineState *pso, const D3D12_RECT *scissor, const u32 *constants, u32 vertices, u32 first)
{
#ifdef RACE_TEST
	list_draw_held *h;
	if (held_count == sizeof(held) / sizeof(held[0]))
		list_draws_flush();
	h = &held[held_count++];
	h->pso = pso;
	h->scissor = *scissor;
	memcpy(h->constants, constants, sizeof(h->constants));
	h->vertices = vertices;
	h->first = first;
#else
	list_draws_record(pso, scissor, constants, vertices, first);
#endif
}

/* what the draws before did to the lists is done before the draws after start */
static void list_draws_barrier(void)
{
	list_draws_flush();
	dx12c_uav_barrier(oit.pixels.resource);
}

static void set_cut_scissor(const D3D12_RECT *rect)
{
	const D3D12_RECT cut = dx12r_cut_scissor(rect);
	dx12c_set_scissor(&cut);
}

static void draw_modifier_volumes(bool translucent, int first, int count, const ModifierVolumeParam *modVolParams)
{
	const ModifierVolumeParam *params;
	ID3D12GraphicsCommandList *cmd;
	int mod_base = -1, cmv;
	if (count == 0 || !dx12r.have_modvols)
		return;
	dx12r_flush_batch();
	cmd = dx12c_cmd();

	/* the modifier volumes' triangles, a position each */
	dx12c_set_vertex_buffer(&dx12r.modvol_view);
	dx12c_set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	set_cut_scissor(&dx12r.base_scissor);

	params = &modVolParams[first];

	for (cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam *param = &params[cmv];
		u32 mvFirst, mvCount, mv_mode, mode, cull;
		u32 constants[DX12C_POLY_CONSTANT_COUNT];
		D3D12_RECT scissor, clipRect;
		ID3D12PipelineState *pso;
		mv_clip_range_get(&pvrrc, modVolParams, param, &mvFirst, &mvCount);

		if (mvCount == 0)
			continue;

		mv_mode = PPW(param->isp, ISP_Modvol_DepthMode);

		/* clipped like a polygon, to the rectangle it was sent under if it asked to be */
		if (dx12r_set_tile_clip(param->tileclip, &clipRect) == TILECLIP_OUTSIDE)
			scissor = dx12r_cut_scissor(&clipRect);
		else
			scissor = dx12r_cut_scissor(&dx12r.base_scissor);
		dx12c_set_scissor(&scissor);

		if (mod_base == -1)
			mod_base = (int)mvFirst;

		mode = PPW(param->isp, ISP_Modvol_CullMode);
		cull = mode == 3 ? 2 : mode == 2 ? 1 : 0;
		memset(constants, 0, sizeof(constants));
		if (translucent)
		{
			/* (the lists are marked by the shaders, and nothing else is tested or written) */
			DX12PsoKey key;
			memset(&key, 0, sizeof(key));
			key.kind = DX12_PSO_OIT_MODVOL;
			key.depthFunc = 7;
			key.cull = cull;
			key.samples = dx12c_target_samples();
			key.rate = key.samples != 0 ? DX12_RATE_PIXEL : DX12_RATE_NONE;
			pso = dx12_pso_get(&key);
			constants[13] = !PPW(param->isp, ISP_Modvol_VolumeLast) && mv_mode > 0 ? DX12_OF_MV_OR : DX12_OF_MV_XOR;
			if (pso != NULL)
				list_draws_draw(pso, &scissor, constants, mvCount * 3, mvFirst * 3);
		}
		else
		{
			if (!PPW(param->isp, ISP_Modvol_VolumeLast) && mv_mode > 0)
				pso = dx12r_modvol_pipeline(true, false, 4, DX12Stencil_Or, cull, false, DX12_BLEND_KEY);	/* OR'ing (open volume or quad) */
			else
				pso = dx12r_modvol_pipeline(true, false, 4, DX12Stencil_Xor, cull, false, DX12_BLEND_KEY);	/* XOR'ing (closed volume) */
			if (pso != NULL)
			{
				dx12c_set_pipeline(pso);
				dx12c_set_stencil_ref(2);
				ID3D12GraphicsCommandList_DrawInstanced(cmd, mvCount * 3, 1, mvFirst * 3, 0);
			}
		}

		if (mv_mode == 1 || mv_mode == 2)
		{
			/* Sum the area */
			if (translucent)
			{
				/* (what the volumes before did to the lists is done) */
				list_draws_barrier();
				constants[13] = mv_mode == 1 ? DX12_OF_MV_INCLUSION : DX12_OF_MV_EXCLUSION;
				if (pso != NULL)
				{
					list_draws_draw(pso, &scissor, constants, (mvFirst + mvCount - mod_base) * 3, mod_base * 3);
					list_draws_barrier();
				}
			}
			else
			{
				pso = dx12r_modvol_pipeline(false, false, 7, mv_mode == 1 ? DX12Stencil_Inclusion : DX12Stencil_Exclusion, cull, false, DX12_BLEND_KEY);
				if (pso != NULL)
				{
					dx12c_set_pipeline(pso);
					dx12c_set_stencil_ref(1);
					ID3D12GraphicsCommandList_DrawInstanced(cmd, (mvFirst + mvCount - mod_base) * 3, 1, mod_base * 3, 0);
				}
			}
			mod_base = -1;
		}
	}
	list_draws_flush();
	set_cut_scissor(&dx12r.base_scissor);
	dx12c_set_vertex_buffer(&dx12r.vertex_view);
}

/* With the HDR output and SSAA: the channel sums made for @width x
 * @height when they are not. False when they cannot be. */
static void drop_sums(void)
{
	oit.sumsStored = false;
	if (oit.sums.resource == NULL)
		return;
	/* (frames still on the GPU may store into it: it goes when they are done) */
	dx12c_release((IUnknown *)oit.sums.resource);
	oit.sums.resource = NULL;
	dx12_image_reset(&oit.sums);
	/* (its view stays in its place, unread: only the resolve that stores
	 * the sums reads it, and that one is drawn after a new one is made) */
}

static bool make_sums(u32 width, u32 height)
{
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC desc;
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
	/* (grown only, as the lists' heads are: its place is the one for good) */
	if (oit.sums.resource != NULL && width <= oit.sums.width && height <= oit.sums.height)
		return true;
	width = MAX_U(width, oit.sums.width);
	height = MAX_U(height, oit.sums.height);
	drop_sums();
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_UINT;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	oit.sums.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	if (FAILED(ID3D12Device_CreateCommittedResource(dx12r.device, &heap, D3D12_HEAP_FLAG_NONE, &desc, oit.sums.state, NULL,
			&iid_resource, (void **)&oit.sums.resource)))
	{
		WARN_LOG(RENDERER, "DX12: HDR channel sums creation failed (%d x %d)\n", width, height);
		oit.sums.resource = NULL;
		dx12_image_reset(&oit.sums);
		return false;
	}
	oit.sums.width = width;
	oit.sums.height = height;
	memset(&uav, 0, sizeof(uav));
	uav.Format = DXGI_FORMAT_R16G16B16A16_UINT;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	ID3D12Device_CreateUnorderedAccessView(dx12r.device, oit.sums.resource, NULL, &uav, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_SUMS_SLOT));
	return true;
}

static dx12_image *oit_hdr_sums(void)
{
	return oit.sumsStored ? &oit.sums : NULL;
}

/* The lists put together over the opaque polygons' picture (at @opaqueSrv
 * of the table), into @target: the whole picture, not the part the game
 * clips its drawing to - what is outside that has to be written too. With
 * SSAA, from the samples: resolved into @target, of one sample, when
 * @intoSamples is not set; else into the samples of @target, and the lists
 * emptied after. (@withSums: and the samples' channel sums stored, into
 * the sums.) */
static void resolve(u32 target, u32 opaqueSrv, bool intoSamples, bool withSums)
{
	DX12PsoKey key;
	ID3D12PipelineState *pso;
	D3D12_RECT whole, area;
	u32 constants[DX12C_POLY_CONSTANT_COUNT];
	dx12r_flush_batch();
	memset(&key, 0, sizeof(key));
	key.kind = DX12_PSO_OIT_FINAL;
	key.writeColor = true;
	key.depthFunc = 7;
	if (dx12r_ssaa_active())
	{
		key.rate = intoSamples ? DX12_RATE_SAMPLE : withSums ? DX12_RATE_PIXEL_SUMS : DX12_RATE_PIXEL;
		key.samples = intoSamples ? dx12r.ssaa_log2 : 0;
	}
	dx12c_set_targets(target, ~0u, key.samples);
	pso = dx12_pso_get(&key);
	if (pso == NULL)
		return;
	dx12c_set_pipeline(pso);
	whole.left = 0;
	whole.top = 0;
	whole.right = (LONG)dx12r.target_width;
	whole.bottom = (LONG)dx12r.target_height;
	area = dx12r_cut_scissor(&whole);
	dx12c_set_scissor(&area);
	dx12c_set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	memset(constants, 0, sizeof(constants));
	constants[8] = opaqueSrv;
	dx12c_set_root_constants(constants, DX12C_POLY_CONSTANT_COUNT);
	ID3D12GraphicsCommandList_DrawInstanced(dx12c_cmd(), 4, 1, 0, 0);
	if (key.rate == DX12_RATE_SAMPLE && oit.clearHeap.heap != NULL && area.right > area.left && area.bottom > area.top)
	{
		/* (the samples of a pixel all read its list: it is emptied once they
		 * are done, for the next render pass) */
		static const UINT eol[4] = { ~0u, ~0u, ~0u, ~0u };
		lists_barrier();
		ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(dx12c_cmd(), dx12_heap_gpu(&dx12c.srv_heap, DX12C_OIT_SSAA_FIRST + 1), dx12_heap_cpu(&oit.clearHeap, 0),
				oit.pointers.resource, eol, 1, &area);
	}
}

static void clear_stencil(void)
{
	dx12r_depth_states(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	ID3D12GraphicsCommandList_ClearDepthStencilView(dx12c_cmd(), dx12_heap_cpu(&dx12c.dsv_heap, dx12r.depth.rtv), D3D12_CLEAR_FLAG_STENCIL, 0.f, 0, 0, NULL);
}

static void oit_draw_frame(void)
{
	ID3D12GraphicsCommandList *cmd;
	/* SSAA: the screen drawn into the pictures of samples, with the views of
	 * their depth and stencil in the table the shaders read */
	const bool ssaa = dx12r_ssaa_active();
	dx12_image *pictures = ssaa ? dx12r.ssaa_color : oit.pictures;
	const u32 samples = ssaa ? dx12r.ssaa_log2 : 0;
	const RenderPass *passes = TA_LIST_HEAD(pvrrc.render_passes, RenderPass);
	const int render_pass_count = TA_LIST_USED(pvrrc.render_passes, RenderPass);
	const PolyParam *op = TA_LIST_HEAD(pvrrc.global_param_op, PolyParam);
	const PolyParam *pt = TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam);
	const PolyParam *tr = TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam);
	float clear[4] = { 0.f, 0.f, 0.f, 1.f };
	bool withSums;
	u32 bands, bandHeight, bandCount, bandIndex, countWord = 0, t;
	D3D12_RECT whole;
	int i;
	if (!make_pictures(dx12r.target_width, dx12r.target_height) || !upload_tr_poly_params() || oit.pixels.resource == NULL)
		return;
	dx12c_set_viewport((float)dx12r.target_width, (float)dx12r.target_height);
	cmd = dx12c_cmd();
	dx12c_bind_oit_table(ssaa);
	/* (the HDR output's: the screen's resolve stores the samples' channel
	 * sums as well, where they can be had) */
	withSums = ssaa && !pvrrc.isRTT && dx12c_hdr_active() && make_sums(dx12r.target_width, dx12r.target_height);
	if (!pvrrc.isRTT)
		oit.sumsStored = false;

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour, which is what the OpenGL renderers clear it to (never a
	 * render to a texture: that is not such a framebuffer); any other
	 * starts black. */
	if (pvrrc.clearFramebuffer)
	{
		clear[0] = VO_BORDER_COL.Red / 255.f;
		clear[1] = VO_BORDER_COL.Green / 255.f;
		clear[2] = VO_BORDER_COL.Blue / 255.f;
	}
	for (i = 0; i < 2; i++)
		dx12c_transition(&pictures[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
	ID3D12GraphicsCommandList_ClearRenderTargetView(cmd, dx12_heap_cpu(&dx12c.rtv_heap, pictures[0].rtv), clear, 0, NULL);
	if (pvrrc.isRTT)
		/* (what is outside the pixel clip, as the per-triangle renderer leaves it) */
		ID3D12GraphicsCommandList_ClearRenderTargetView(cmd, dx12_heap_cpu(&dx12c.rtv_heap, dx12r.target_rtv), clear, 0, NULL);
	dx12r_depth_states(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	ID3D12GraphicsCommandList_ClearDepthStencilView(cmd, dx12_heap_cpu(&dx12c.dsv_heap, dx12r.depth.rtv),
			(D3D12_CLEAR_FLAGS)(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL), 0.f, 0, 0, NULL);

	/* In bands, when the frames before had more translucent pixels than the
	 * lists have room for (choose_bands()): all of the frame's render passes
	 * once a band, every draw cut to the band by the scissor
	 * (oit_cut_scissor()), each band's lists starting empty and put on the
	 * picture before the next band's. Nothing is dropped and nothing is
	 * allocated; the opaque polygons are drawn once a band. (The clears
	 * above are of the whole target: a band's draws touch its own pixels
	 * alone, so the others' depth is still clear when their band comes.) */
	t = MAX_U(dx12r.target_height, 1u);
	bands = choose_bands(pvrrc.isRTT);
	bands = MAX_U(MIN_U(bands, t), 1u);
	bandHeight = (dx12r.target_height + bands - 1) / bands;
	bandCount = MAX_U((dx12r.target_height + bandHeight - 1) / bandHeight, 1u);
	oit.countWords[oit.countHead] = 0;
	whole.left = 0;
	whole.top = 0;
	whole.right = (LONG)dx12r.target_width;
	whole.bottom = (LONG)dx12r.target_height;

	for (bandIndex = 0; bandIndex < bandCount; bandIndex++)
	{
		int cur = 0, render_pass;
		RenderPass previous_pass;
		oit.bandTop = (LONG)(bandIndex * bandHeight);
		oit.bandBottom = bandCount > 1 ? (LONG)MIN_U((bandIndex + 1) * bandHeight, dx12r.target_height) : 0;
		memset(&previous_pass, 0, sizeof(previous_pass));
		for (render_pass = 0; render_pass < render_pass_count; render_pass++)
		{
			const RenderPass *current_pass = &passes[render_pass];
			const bool finalPass = render_pass == render_pass_count - 1;
			u32 target;

			DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d TrMV %d autosort %d\n", render_pass + 1,
					current_pass->op_count - previous_pass.op_count,
					current_pass->pt_count - previous_pass.pt_count,
					current_pass->tr_count - previous_pass.tr_count,
					current_pass->mvo_count - previous_pass.mvo_count,
					current_pass->mvo_tr_count - previous_pass.mvo_tr_count, current_pass->autosort);

			if (render_pass != 0)
				/* (each pass has its own volumes) */
				clear_stencil();

			/* Depth and stencil. The pixel counter is reset: the lists are
			 * empty, and the buffer is filled from its start. */
			reset_count();
			dx12r_depth_states(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			dx12c_transition(&pictures[cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
			dx12c_set_targets(pictures[cur].rtv, dx12r.depth.rtv, samples);
			oit_draw_list(ListType_Opaque, false, DX12_PASS_DEPTH, op, previous_pass.op_count, current_pass->op_count);
			oit_draw_list(ListType_Punch_Through, false, DX12_PASS_DEPTH, pt, previous_pass.pt_count, current_pass->pt_count);
			draw_modifier_volumes(false, previous_pass.mvo_count, current_pass->mvo_count - previous_pass.mvo_count,
					TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam));

			/* Colour, the stencil read by the shaders */
			dx12r_depth_states(D3D12_RESOURCE_STATE_DEPTH_WRITE, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
			dx12c_set_targets(pictures[cur].rtv, dx12r.dsv_read_stencil, samples);

			/* A render that covers only part of the screen leaves the rest as
			 * it was: the last picture goes in first, under everything */
			if (render_pass == 0 && !pvrrc.isRTT && dx12r_keeps_last_picture())
			{
				dx12_image *last = dx12r_last_picture_image();
				if (last != NULL && (!dx12c_is_handed_over(last) || last->state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
				{
					dx12c_transition(last, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
					/* (the quad is drawn with the scissor that is set: the band's) */
					set_cut_scissor(&whole);
					dx12c_draw_quad_at(last->srv, !dx12r.have_picture, -1.f, -1.f, 2.f, 2.f, 0.f, 0.f, 1.f, 1.f, DX12_BLEND_KEY, NULL);
				}
			}

			/* OP + PT */
			oit_draw_list(ListType_Opaque, false, DX12_PASS_COLOR, op, previous_pass.op_count, current_pass->op_count);
			oit_draw_list(ListType_Punch_Through, false, DX12_PASS_COLOR, pt, previous_pass.pt_count, current_pass->pt_count);

			/* TR */
			/* (In the first frame after the per-pixel lists are made, too: they
			 * start empty, and that frame's translucent polygons are drawn like
			 * any other's.) */
			if (current_pass->autosort)
			{
				/* into the lists, tested against the depth by the shaders as well */
				dx12r_depth_states((D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), D3D12_RESOURCE_STATE_DEPTH_READ);
				dx12c_set_targets(pictures[cur].rtv, dx12r.dsv_read_only, samples);
				oit_draw_list(ListType_Translucent, true, DX12_PASS_OIT, tr, previous_pass.tr_count, current_pass->tr_count);
				lists_barrier();
				/* how many pixels this band's pass asked for, copied out */
				copy_count(oit.countHead, countWord++);
			}
			else
				oit_draw_list(ListType_Translucent, false, DX12_PASS_COLOR, tr, previous_pass.tr_count, current_pass->tr_count);

			/* The translucent modifier volumes, and the lists put together */
			target = finalPass ? dx12r.target_rtv : pictures[cur ^ 1].rtv;
			/* (nothing is written to it; with SSAA the volumes are drawn with the
			 * samples, for where they cover a pixel's first sample) */
			dx12c_set_targets(ssaa ? pictures[cur].rtv : target, ~0u, samples);
			if (current_pass->mv_op_tr_shared)
				draw_modifier_volumes(true, previous_pass.mvo_count, current_pass->mvo_count - previous_pass.mvo_count,
						TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam));
			else
				draw_modifier_volumes(true, previous_pass.mvo_tr_count, current_pass->mvo_tr_count - previous_pass.mvo_tr_count,
						TA_LIST_HEAD(pvrrc.global_param_mvo_tr, ModifierVolumeParam));
			lists_barrier();
			dx12c_transition(&pictures[cur], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			if (finalPass && withSums)
				dx12c_transition(&oit.sums, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			resolve(target, pictures[cur].srv, !finalPass, finalPass && withSums);
			if (finalPass && withSums)
				oit.sumsStored = true;
			lists_barrier();

			if (!finalPass)
			{
				/* The translucent polygons' depth, for the passes after this one */
				dx12r_depth_states(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				dx12c_transition(&pictures[cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
				dx12c_set_targets(pictures[cur].rtv, dx12r.depth.rtv, samples);
				oit_draw_list(ListType_Translucent, current_pass->autosort != 0, DX12_PASS_DEPTH, tr, previous_pass.tr_count, current_pass->tr_count);
				/* the next pass draws on what this one put together */
				cur ^= 1;
			}

			previous_pass = *current_pass;
		}
	}
	oit.bandTop = oit.bandBottom = 0;
	/* (what is drawn on the picture after this is not cut to the last band; the depth as the base renderer has it) */
	dx12c_set_scissor(&whole);
	dx12r_depth_states(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	/* the frame's counts are in its slot: the next frame's go in the next */
	oit.countBands[oit.countHead] = bandCount;
	oit.countRtt[oit.countHead] = pvrrc.isRTT;
	oit.countHead = (oit.countHead + 1) % COUNT_SLOTS;
	/* (the target is left bound: the overlay goes onto it, and the end of the render takes it; with SSAA, its depth is of samples) */
	dx12c_set_targets(dx12r.target_rtv, ssaa ? ~0u : dx12r.depth.rtv, 0);
}

/* ---- Rendering Method: Compute ---- */

/* A render to a texture, drawn by the compute renderer into its texture;
 * false if it is not (nothing recorded then) */
static bool render_compute_rtt(void)
{
	dx12_pooled_texture *spare;
	u32 width, height;
	dx12r_rtt_size(&width, &height);
	if (!dx12_compute_prepare(&compute_rules, ~0u, false, width, height))
		return false;
	/* (the frame's textures were looked at; the texture drawn into is taken now, which can be one of them) */
	if (!dx12r_begin_rtt())
	{
		dx12r.rtt_texture = NULL;
		return false;
	}
	spare = dx12r.rtt_texture->spare;
	dx12c_transition(&dx12r.palette_texture, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	dx12c_transition_resource(spare->resource, &spare->state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	dx12_compute_record(spare->resource, NULL, NULL);
	dx12r_end_rtt();
	return true;
}

/* The frame in pvrrc drawn by the compute renderer into the screen's
 * picture, and shown; false if it is not (nothing recorded then) */
static bool render_compute(void)
{
	dx12_image *target, *image;
	dx12_upscaler_slot *slot;
	bool forUpscaler, scaled = false, copy = false, withSums;
	u32 last = ~0u;
	/* the frontend may still be showing what is in the screen's next
	 * picture: it is waited for, if process() has not (and not again by the
	 * per-pixel renderer, if it draws the frame after all) */
	if (!dx12r.held_from_process)
		dx12c_wait_present_index();
	dx12r.held_from_process = true;
	dx12r_ssaa_depth(false);
	dx12r.screen_index = (int)dx12c_present_index();
	if (!dx12r_ensure_screen((u32)screen_width, (u32)screen_height))
		return false;
	target = &dx12r.screens[dx12r.screen_index];
	/* (made for the screen's size: resize makes it again) */
	if (dx12_compute_width() != dx12r.screen_width || dx12_compute_height() != dx12r.screen_height)
	{
		dx12_compute_fallback(CF_WHY_ROOM);
		return false;
	}
	transform_matrix_calc(&dx12r.matrices, &pvrrc);
	/* the frame drawn under this frame's jitter, where the walks store the depth for the upscaler */
	forUpscaler = dx12_compute_depth_on() && dx12r_upscaler_jitter();
	/* The picture to start from, where the render covers only part of the
	 * screen: read where it is; or copied first where it cannot be - this
	 * frame's own picture (the frontend's one picture, version 1), or one
	 * the frontend has (version 2), which it leaves in COPY_SOURCE with the
	 * frontends there are, as the raster path copies it */
	image = dx12r_keeps_last_picture() ? dx12r_last_picture_image() : NULL;
	if (image != NULL)
	{
		const bool handedOver = dx12c_is_handed_over(image);
		copy = image == target || handedOver;
		if (copy && (!dx12r.have_picture || image->width != dx12_compute_width() || image->height != dx12_compute_height()
				|| (handedOver && image->state != D3D12_RESOURCE_STATE_COPY_SOURCE)))
		{
			dx12_compute_fallback(CF_WHY_PICTURE);
			return false;
		}
		last = copy ? dx12_compute_start_srv() : image->srv;
		/* (the picture kept from another context or size: filtered over this one, as the raster path draws it) */
		scaled = !dx12r.have_picture;
	}
	if (!dx12_compute_prepare(&compute_rules, last, scaled, 0, 0))
		return false;
	/* (the upscaler's strips matched once the frame is this renderer's: the raster path matches its own) */
	slot = forUpscaler && dx12r_upscaler_match() ? &dx12r.upscaler_slots[dx12r.screen_index] : NULL;
	if (image != NULL && copy)
	{
		dx12c_transition(image, D3D12_RESOURCE_STATE_COPY_SOURCE);
		dx12_compute_copy_start(image->resource);
	}
	else if (image != NULL)
		dx12c_transition(image, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	dx12c_transition(&dx12r.palette_texture, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	dx12c_transition(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	/* (the HDR output's channel sums, stored by the walk: the per-pixel renderer's image of them) */
	withSums = dx12_compute_sums_on() && make_sums(dx12r.screen_width, dx12r.screen_height);
	if (withSums)
		dx12c_transition(&oit.sums, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (slot != NULL)
		dx12c_transition(&slot->depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	dx12_compute_record(target->resource, withSums ? oit.sums.resource : NULL, slot != NULL ? slot->depth.resource : NULL);
	oit.sumsStored = withSums && dx12_compute_sums_stored();
	/* the motion vector pass at the end of the frame, over the depth the
	 * walk stored, from the frame's vertices where the walk read them (with
	 * the constants and scissor the pass draws with: the raster path's) */
	if (slot != NULL && dx12_compute_depth_stored() && dx12r_upload_constants())
	{
		dx12r_set_base_scissor();
		dx12_compute_ring_for_draw(&dx12r.motion_vertex_view, &dx12r.motion_index_view);
		dx12r.motion_sampled = true;
	}
	else
		dx12r.upscaled = false;
	/* (what goes over it is drawn into it, and it is handed over, as any other frame of the screen) */
	dx12c_transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
	dx12r_end_screen(false);
	dx12r.held_from_process = false;
	return true;
}

static bool oit_render_frame(void)
{
	/* (a render to a texture with an empty pixel clip writes nothing: dx12r_render_frame()'s) */
	if (dx12_compute_ready() && dx12r.device != NULL && !pvrrc.isRenderFramebuffer
			&& !(pvrrc.isRTT && (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min)))
	{
		if (pvrrc.isRTT && !dx12_pool_targets_storable())
			dx12_compute_fallback(CF_WHY_RTT);
		else if (!dx12_compute_can_draw())
			dx12_compute_fallback(CF_WHY_DECODE);
		else if (pvrrc.isRTT ? render_compute_rtt() : render_compute())
			return !pvrrc.isRTT;
	}
	return dx12r_render_frame();
}

/* ---- The per-pixel renderer ---- */

/* GPU-side TA processing: the translucent draw records number each strip
 * (TA_GPU_INSTANCE), which picks its number from the instances' buffer; no
 * sorting here */
static u32 oit_ta_flags(void)
{
	return TA_GPU_DRAW | TA_GPU_PROVOKE | TA_GPU_INSTANCE;
}

/* SSAA: two pictures of samples, and the lists first */
static u32 oit_ssaa_fixed_kib(u32 width, u32 height)
{
	const u64 kib = planned_size((int)width, (int)height) >> 10;
	return (u32)(kib < 0xffffffffu ? kib : 0xffffffffu);
}

static const dx12_renderer_kind oit_kind =
{
	oit_render_frame,
	oit_draw_frame,
	false,					/* binds_screen */
	oit_ta_flags,
	false,					/* ta_sorts */
	2,						/* ssaa_color_images */
	oit_ssaa_fixed_kib,
	oit_prepare_pipelines,
	oit_cut_scissor,
	oit_hdr_sums
};

static void oit_term(void)
{
	int i;
	dx12_compute_term();
	if (dx12r.device != NULL)
	{
		for (i = 0; i < 2; i++)
			dx12r_drop_image(&oit.pictures[i]);
		oit.picturesWidth = oit.picturesHeight = 0;
		drop_buffer(&oit.pointers);
		oit.pointersWidth = oit.pointersHeight = 0;
		drop_buffer(&oit.pixels);
		drop_buffer(&oit.counter);
		drop_buffer(&oit.noInstance);
		if (oit.counts.resource != NULL && oit.countsMapped != NULL)
			ID3D12Resource_Unmap(oit.counts.resource, 0, NULL);
		oit.countsMapped = NULL;
		drop_buffer(&oit.counts);
		oit.bufferSize = oit.room = dx12r.pixel_buffer_room = 0;
		oit.depthViewed = NULL;
		dx12_heap_term(&oit.clearHeap);
		drop_sums();
	}
	dx12r_term();
}

static bool oit_init(void)
{
	u32 *zero = NULL;
	u32 i;
	oit_construct();
	/* (Rendering Method: Compute draws into the screen's pictures, which
	 * it writes through unordered access views) */
	if (!dx12r_init(&oit_kind, rend_compute_wanted))
		return false;
	if (!make_buffers(planned_size(screen_width, screen_height)))
	{
		WARN_LOG(RENDERER, "DX12 per-pixel renderer initialization failed\n");
		oit_term();
		return false;
	}
	/* The translucent polygons' numbers past the root constant's, an
	 * instance's: 0 for every draw but a frame decoded on the GPU's
	 * translucent ones (dx12_ta_draw::instances) */
	if (!make_buffer(&oit.noInstance, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ)
			|| FAILED(ID3D12Resource_Map(oit.noInstance.resource, 0, NULL, (void **)&zero)))
	{
		WARN_LOG(RENDERER, "DX12 per-pixel renderer initialization failed\n");
		oit_term();
		return false;
	}
	memset(zero, 0, 256);
	ID3D12Resource_Unmap(oit.noInstance.resource, 0, NULL);
	oit.countHead = 0;
	for (i = 0; i < COUNT_SLOTS; i++)
	{
		oit.countWords[i] = 0;
		oit.countFence[i] = 0;
	}
	oit.oitBands[0] = oit.oitBands[1] = 1;
	oit.oitRoomyFrames[0] = oit.oitRoomyFrames[1] = 0;
	oit.bandTop = oit.bandBottom = 0;
	init_compute();
	return true;
}

static void oit_resize(int w, int h)
{
	dx12r_resize(w, h);
	init_compute();
}

/* (the compute renderer's, which is this renderer's: tried again by the next one) */
static void oit_release(void)
{
	dx12_compute_reset();
}

static const renderer_ops dx12_oit_renderer_ops =
{
	oit_init,
	oit_resize,
	oit_term,
	dx12r_process,
	dx12r_render,
	dx12r_keep_picture,
	dx12r_get_texture,
	dx12r_options_changed,
	NULL,
	oit_release,
	NULL,
	NULL,
	dx12r_ready
};

const renderer_ops *rend_OITD3D12(void)
{
	return &dx12_oit_renderer_ops;
}
