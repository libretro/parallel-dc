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
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string.h>
#include <retro_inline.h>
#include "types.h"
#include <libretro.h>
#include <libretro_d3d12.h>
#include "rend/vram_budget.h"
#include "rend/TexCache.h"
#include "dx12_pso.h"
#include "rend/hdr/hdr_encode_d3d12.h"

/* The frontend's Direct3D 12 device and queue, from RETRO_HW_CONTEXT_D3D12's
 * interface, and what the renderers share of them.
 *
 * Everything is recorded into one command list at a time, opened when
 * something is first recorded (dx12c_cmd()) and executed on the frontend's queue
 * when a render is done (dx12c_submit()), which signals this context's fence.
 * The fence is only ever polled - nothing waits for it but a readback the
 * game asked for, and the frontend's own wait_sync_index (version 2) - and
 * what the GPU still reads (the command allocators, the upload ring, a
 * resource let go of) is used again once the fence says it is done with it.
 *
 * Version 1 of the interface hands the frame over with set_texture, and the
 * frontend copies it inside video_refresh, on the same queue: one picture.
 * Version 2 (asked for through the negotiation interface, as Direct3D 11's
 * version 2 is) hands it over with set_texture_fenced, the fence and the
 * value its render signals, and the frontend reads it where it is, with no
 * copy: a picture for each of its sync indexes. */

#ifdef __cplusplus
extern "C" {
#endif

/* A COM object given back, and the pointer to it NULL */
#define DX12_RELEASE(p) do { if ((p) != NULL) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

/* A texture or picture of the renderer's, and the state it was left in */
typedef struct dx12_image
{
	ID3D12Resource *resource;
	D3D12_RESOURCE_STATES state;
	u32 srv;		/* its place in the bindless table */
	u32 rtv;		/* in the render target heap, if it has one */
	u32 width;
	u32 height;
} dx12_image;

/* An image with nothing: no resource, no places */
static INLINE void dx12_image_init(dx12_image *image)
{
	image->resource = NULL;
	image->state = D3D12_RESOURCE_STATE_COMMON;
	image->srv = image->rtv = ~0u;
	image->width = image->height = 0;
}

/* ...and one that had something: its resource given back now */
static INLINE void dx12_image_reset(dx12_image *image)
{
	DX12_RELEASE(image->resource);
	dx12_image_init(image);
}

/* A piece of the upload ring: where the CPU writes, where the GPU reads */
typedef struct dx12_upload
{
	u8 *cpu;
	D3D12_GPU_VIRTUAL_ADDRESS gpu;
	ID3D12Resource *buffer;
	u64 offset;		/* in the buffer */
} dx12_upload;

/* A heap of descriptors with places handed out and back: a stack of the
 * free ones, made with the heap (nothing allocated after) */
typedef struct dx12_descriptor_heap
{
	ID3D12DescriptorHeap *heap;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu_start;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu_start;
	u32 increment;
	u32 *free_list;
	u32 free_count;
	u32 capacity;
} dx12_descriptor_heap;

bool dx12_heap_init(dx12_descriptor_heap *h, ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 count,
		bool shaderVisible, u32 reserved);
void dx12_heap_term(dx12_descriptor_heap *h);
/* The places from @first on handed out from here, those before kept */
void dx12_heap_range(dx12_descriptor_heap *h, u32 first, u32 end);
/* ~0u when there is none left */
static INLINE u32 dx12_heap_alloc(dx12_descriptor_heap *h)
{
	return h->free_count != 0 ? h->free_list[--h->free_count] : ~0u;
}
static INLINE void dx12_heap_free(dx12_descriptor_heap *h, u32 index)
{
	if (index < h->capacity && h->free_count < h->capacity)
		h->free_list[h->free_count++] = index;
}
static INLINE D3D12_CPU_DESCRIPTOR_HANDLE dx12_heap_cpu(const dx12_descriptor_heap *h, u32 index)
{
	D3D12_CPU_DESCRIPTOR_HANDLE c = h->cpu_start;
	c.ptr += (SIZE_T)index * h->increment;
	return c;
}
static INLINE D3D12_GPU_DESCRIPTOR_HANDLE dx12_heap_gpu(const dx12_descriptor_heap *h, u32 index)
{
	D3D12_GPU_DESCRIPTOR_HANDLE g = h->gpu_start;
	g.ptr += (UINT64)index * h->increment;
	return g;
}

/* The command allocators, used in turn */
#define DX12C_ALLOCATORS 8
#define DX12C_RING_MARKS 64
#define DX12C_RELEASES 1024
/* A place in the render target heap for a view read when a command is
 * recorded and not after: the next of a ring of them (dx12c_transient_rtv()) */
#define DX12C_TRANSIENT_RTVS 64
/* The bindless table starts here; the UAVs after it; then the per-pixel
 * renderer's table, and its table for SSAA's samples after that (the
 * same lists, the depth and stencil views of the samples') */
#define DX12C_UAV_FIRST DX12_SRV_TABLE_SIZE
#define DX12C_UAV_COUNT 4096
#define DX12C_OIT_FIRST (DX12C_UAV_FIRST + DX12C_UAV_COUNT)
#define DX12C_OIT_SSAA_FIRST (DX12C_OIT_FIRST + DX12_OIT_TABLE_SIZE)
/* ...then the compute renderer's tables of views (dx12_compute.h), one
 * for each piece of its ring; and the end of the heap */
#define DX12C_COMPUTE_FIRST (DX12C_OIT_SSAA_FIRST + DX12_OIT_TABLE_SIZE)
#define DX12C_COMPUTE_TABLE_SIZE 13
#define DX12C_COMPUTE_TABLES 4
/* ...then the raster renderers' write-out pass's (dx12_writeout.h): a
 * table of two views, the words and the picture, for each of its slots */
#define DX12C_WRITEOUT_FIRST (DX12C_COMPUTE_FIRST + DX12C_COMPUTE_TABLES * DX12C_COMPUTE_TABLE_SIZE)
#define DX12C_WRITEOUT_SLOTS 16
/* ...then the per-pixel renderer's UAV of SSAA's channel sums (u4: the
 * HDR output's, DX12_RATE_PIXEL_SUMS) */
#define DX12C_OIT_SUMS_SLOT (DX12C_WRITEOUT_FIRST + 2 * DX12C_WRITEOUT_SLOTS)
#define DX12C_SRV_HEAP_SIZE (DX12C_OIT_SUMS_SLOT + 1)
/* How many root constants a polygon has (shaders/dx12_raster.hlsl, PolyConstants) */
#define DX12C_POLY_CONSTANT_COUNT 14

typedef struct dx12_ring_mark
{
	u64 fence;
	u64 head;
} dx12_ring_mark;

typedef struct dx12_released
{
	IUnknown *object;
	u32 srv;
	u64 fence;
} dx12_released;

/* What is bound, for the list being recorded: set only when it differs */
typedef struct dx12_bound
{
	ID3D12PipelineState *pso;
	D3D12_PRIMITIVE_TOPOLOGY topology;
	D3D12_RECT scissor;
	bool scissorSet;
	float viewport[2];
	bool viewportSet;
	UINT stencilRef;
	bool stencilRefSet;
	u32 rootConstants[DX12C_POLY_CONSTANT_COUNT];
	bool rootConstantsSet;
	u32 rtv, dsv;
	D3D12_VERTEX_BUFFER_VIEW vertexBuffer;
	D3D12_INDEX_BUFFER_VIEW indexBuffer;
	float blendFactor[4];
	bool blendFactorSet;
	bool rasterBound;
	bool oitSsaa;
	u32 samples;
} dx12_bound;

typedef struct dx12_context
{
	ID3D12Device *device;
	ID3D12CommandQueue *queue;
	/* what video memory there was to use when the context was set up */
	VramBudget vram_budget;
	bool format_supported[5];
	HMODULE d3d12_module;

	/* the fence, and the event a readback waits on */
	ID3D12Fence *fence;
	HANDLE fence_event;
	u64 fence_value;		/* the last submitted */
	u64 completed;

	/* the command allocators, in turn, each with the value its last list signalled */
	ID3D12CommandAllocator *allocators[DX12C_ALLOCATORS];
	u64 allocator_fence[DX12C_ALLOCATORS];
	u32 allocator_next;
	ID3D12GraphicsCommandList *cmd_list;
	bool recording;
	void (*before_submit)(void *);
	void *before_submit_arg;

	/* The upload ring: positions only ever grow, the place in the buffer is
	 * the position modulo its size. What the GPU may still read starts at
	 * the position the oldest list not done yet was submitted after. */
	ID3D12Resource *ring;
	u8 *ring_cpu;
	D3D12_GPU_VIRTUAL_ADDRESS ring_gpu;
	u64 ring_size;
	u64 ring_head;
	dx12_ring_mark ring_marks[DX12C_RING_MARKS];
	u32 ring_mark_first, ring_mark_count;
	u64 ring_tail;
	u64 ring_submit_head;	/* the head when the last list was submitted */

	/* released once the GPU is done: an object, or a place in the bindless table */
	dx12_released released[DX12C_RELEASES];
	u32 released_first, released_count;

	/* shader visible: textures (the bindless table), the kernels' UAVs;
	 * the polygons' samplers; render targets; depths */
	dx12_descriptor_heap srv_heap, sampler_heap, rtv_heap, dsv_heap;
	ID3D12RootSignature *raster_root;
	ID3D12RootSignature *decode_root;
	/* ExecuteIndirect of draws that change the polygon's root constants
	 * (null if the device cannot) */
	ID3D12CommandSignature *draw_signature;

	dx12_bound bound;
	u32 transient_rtv;
	bool oit_table_ssaa;	/* the per-pixel renderer's table wanted: SSAA's or not (dx12c_bind_oit_table()) */

	/* a picture read back, through this buffer, mapped */
	ID3D12Resource *readback;
	u64 readback_size;

	/* the frame: a reference to its picture, which stays whatever the renderer does with its own */
	ID3D12Resource *frame_resource;
	DXGI_FORMAT frame_format;
	/* the HDR output, and the picture it encoded for the next dx12c_set_frame() */
	hdr_d3d12_t *hdr;
	DXGI_FORMAT hdr_format;
	ID3D12Resource *hdr_frame;
	u64 frame_fence;
	bool handed_over;
	D3D12_RESOURCE_STATES required_state;

	/* the interface */
	bool v2;
	void *handle;
	void (*set_texture)(void *, ID3D12Resource *, DXGI_FORMAT);
	unsigned (*get_sync_index)(void *);
	unsigned (*get_sync_index_mask)(void *);
	void (*wait_sync_index)(void *);
	void (*set_texture_fenced)(void *, ID3D12Resource *, DXGI_FORMAT, ID3D12Fence *, UINT64);

	/* For measuring: the waits there were that the protocol does not ask
	 * for (a full ring, every allocator still in use) */
	u32 stat_stalls;
	u32 stat_submits;
} dx12_context;

extern dx12_context dx12c;

/* What a render's draw calls and the format of every picture are */
#define DX12FrameFormat DXGI_FORMAT_R8G8B8A8_UNORM
/* SSAA's pictures' quality: the standard multisample pattern
 * (D3D12_STANDARD_MULTISAMPLE_PATTERN, which mingw-w64's d3d12.h lacks),
 * the positions rend/ssaa_pattern.h has */
#define DX12StandardPattern 0xffffffffu
#define DX12DepthFormat DXGI_FORMAT_D32_FLOAT_S8X24_UINT
/* The temporal upscaler's: the motion vectors, two halves a pixel, and the
 * depth the frame left, copied out of the depth's first plane */
#define DX12MotionFormat DXGI_FORMAT_R16G16_FLOAT
#define DX12UpscalerDepthFormat DXGI_FORMAT_R32_FLOAT
/* The depth and stencil: a float for the depth, as the Vulkan renderers have it */
#define DX12DepthResourceFormat DXGI_FORMAT_R32G8X24_TYPELESS

/* The frontend has made its device (context_reset) */
bool dx12c_init(const struct retro_hw_render_interface_d3d12 *iface);
/* ...and is about to destroy it, or the game is going */
void dx12c_term(void);

/* Whether the 16-bit formats can be sampled */
static INLINE bool dx12c_format_supported(int type)
{
	return (unsigned)type < 5 && dx12c.format_supported[type];
}
bool dx12c_can_render_to(DXGI_FORMAT format);
bool dx12c_can_write_uav(DXGI_FORMAT format);
/* SSAA's samples a pixel (rend/ssaa_pattern.h): the most of @requested
 * (1, 2, 4 or 8) or fewer that the device draws the frame's colour and
 * depth with in the standard pattern, and then that the video memory
 * budget read at the start has room for: @perSampleKib for each sample,
 * after @fixedKib. 1 is SSAA off. Said in the log, by @what. */
u32 dx12c_ssaa_samples(u32 requested, u32 perSampleKib, u32 fixedKib, const char *what);

/* ---- Recording and submitting ---- */

/* The command list things are recorded into, opened if none is */
ID3D12GraphicsCommandList *dx12c_cmd_open(void);
static INLINE ID3D12GraphicsCommandList *dx12c_cmd(void)
{
	return dx12c.recording ? dx12c.cmd_list : dx12c_cmd_open();
}
/* What is recorded, executed on the frontend's queue; the fence then
 * signalled with the value returned (the last one, if nothing was
 * recorded). What is waiting to be recorded (the before-submit call) goes first. */
u64 dx12c_submit(void);
/* Called before a list is submitted: the uploads and the texture kernels
 * that are waiting (the renderer's) */
static INLINE void dx12c_set_before_submit(void (*fn)(void *), void *arg)
{
	dx12c.before_submit = fn;
	dx12c.before_submit_arg = arg;
}
/* What the GPU has done, polled */
u64 dx12c_completed(void);
/* Waits until the GPU has done @value: for a readback only */
void dx12c_wait_for(u64 value);

/* Bytes of the upload ring, mapped for good, for this list's commands
 * to read: at a multiple of @align (a power of two). The GPU reads it
 * until the list it is for has been executed; the ring takes it back
 * then. False if it can never be had (larger than the ring). */
bool dx12c_upload(u32 bytes, u32 align, dx12_upload *out);

/* An object the GPU may still use, released once it is done with it (the
 * reference handed over) */
void dx12c_release(IUnknown *object);
/* A place in the bindless table the GPU may still read, free again once it is done with it */
void dx12c_release_srv(u32 index);
/* A readback buffer of at least @bytes (made larger if it is not;
 * waits for the GPU then), mapped; null if it cannot be had */
ID3D12Resource *dx12c_readback_buffer(u64 bytes);

/* A transition, if the resource is not in that state already */
void dx12c_transition_resource(ID3D12Resource *resource, D3D12_RESOURCE_STATES *current, D3D12_RESOURCE_STATES state,
		UINT subresource);
static INLINE void dx12c_transition(dx12_image *image, D3D12_RESOURCE_STATES state)
{
	dx12c_transition_resource(image->resource, &image->state, state, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}
/* What a shader wrote through unordered access views is done before
 * what comes next reads or writes them */
void dx12c_uav_barrier(ID3D12Resource *resource);

/* ---- Descriptors ---- */

/* A view of a 2D texture into the table, at @index */
void dx12c_make_srv(ID3D12Resource *resource, DXGI_FORMAT format, u32 levels, u32 index);
/* The polygons' root signature, the table and the samplers bound, if
 * they are not; dx12c_forget_raster() when another root signature was set */
void dx12c_bind_raster(void);
static INLINE void dx12c_forget_raster(void)
{
	dx12c.bound.rasterBound = false;
}
static INLINE u32 dx12c_transient_rtv(void)
{
	return dx12c.transient_rtv = (dx12c.transient_rtv + 1) % DX12C_TRANSIENT_RTVS;
}
/* The per-pixel renderer's table the shaders read: SSAA's or not */
static INLINE void dx12c_bind_oit_table(bool ssaa)
{
	/* (kept for a list started after this: dx12c_bind_raster() binds it again) */
	dx12c.oit_table_ssaa = ssaa;
	dx12c_bind_raster();
	if (ssaa != dx12c.bound.oitSsaa)
	{
		ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(dx12c.cmd_list, 5,
				dx12_heap_gpu(&dx12c.srv_heap, ssaa ? DX12C_OIT_SSAA_FIRST : DX12C_OIT_FIRST));
		dx12c.bound.oitSsaa = ssaa;
	}
}

/* ---- What is bound ---- */

static INLINE void dx12c_set_pipeline(ID3D12PipelineState *pso)
{
	if (pso != dx12c.bound.pso)
	{
		ID3D12GraphicsCommandList_SetPipelineState(dx12c.cmd_list, pso);
		dx12c.bound.pso = pso;
	}
}
/* (a pipeline was set that is not one of the table's) */
static INLINE void dx12c_forget_pipeline(void)
{
	dx12c.bound.pso = NULL;
}
static INLINE void dx12c_set_topology(D3D12_PRIMITIVE_TOPOLOGY topology)
{
	if (topology != dx12c.bound.topology)
	{
		ID3D12GraphicsCommandList_IASetPrimitiveTopology(dx12c.cmd_list, topology);
		dx12c.bound.topology = topology;
	}
}
static INLINE void dx12c_set_scissor(const D3D12_RECT *rect)
{
	if (!dx12c.bound.scissorSet || memcmp(rect, &dx12c.bound.scissor, sizeof(*rect)))
	{
		ID3D12GraphicsCommandList_RSSetScissorRects(dx12c.cmd_list, 1, rect);
		dx12c.bound.scissor = *rect;
		dx12c.bound.scissorSet = true;
	}
}
static INLINE void dx12c_set_viewport(float width, float height)
{
	if (!dx12c.bound.viewportSet || width != dx12c.bound.viewport[0] || height != dx12c.bound.viewport[1])
	{
		D3D12_VIEWPORT vp;
		vp.TopLeftX = 0.f;
		vp.TopLeftY = 0.f;
		vp.Width = width;
		vp.Height = height;
		vp.MinDepth = 0.f;
		vp.MaxDepth = 1.f;
		ID3D12GraphicsCommandList_RSSetViewports(dx12c.cmd_list, 1, &vp);
		dx12c.bound.viewport[0] = width;
		dx12c.bound.viewport[1] = height;
		dx12c.bound.viewportSet = true;
	}
}
static INLINE void dx12c_set_stencil_ref(UINT ref)
{
	if (!dx12c.bound.stencilRefSet || ref != dx12c.bound.stencilRef)
	{
		ID3D12GraphicsCommandList_OMSetStencilRef(dx12c.cmd_list, ref);
		dx12c.bound.stencilRef = ref;
		dx12c.bound.stencilRefSet = true;
	}
}
static INLINE void dx12c_set_vertex_buffer(const D3D12_VERTEX_BUFFER_VIEW *view)
{
	if (memcmp(view, &dx12c.bound.vertexBuffer, sizeof(*view)))
	{
		ID3D12GraphicsCommandList_IASetVertexBuffers(dx12c.cmd_list, 0, 1, view);
		dx12c.bound.vertexBuffer = *view;
	}
}
static INLINE void dx12c_set_index_buffer(const D3D12_INDEX_BUFFER_VIEW *view)
{
	if (memcmp(view, &dx12c.bound.indexBuffer, sizeof(*view)))
	{
		ID3D12GraphicsCommandList_IASetIndexBuffer(dx12c.cmd_list, view);
		dx12c.bound.indexBuffer = *view;
	}
}
static INLINE void dx12c_set_blend_factor(const float factor[4])
{
	if (!dx12c.bound.blendFactorSet || memcmp(factor, dx12c.bound.blendFactor, sizeof(dx12c.bound.blendFactor)))
	{
		ID3D12GraphicsCommandList_OMSetBlendFactor(dx12c.cmd_list, factor);
		memcpy(dx12c.bound.blendFactor, factor, sizeof(dx12c.bound.blendFactor));
		dx12c.bound.blendFactorSet = true;
	}
}
/* (what an ExecuteIndirect changed is not known after it) */
static INLINE void dx12c_forget_root_constants(void)
{
	dx12c.bound.rootConstantsSet = false;
}
static INLINE void dx12c_set_root_constants(const u32 *values, u32 count)
{
	if (!dx12c.bound.rootConstantsSet || memcmp(values, dx12c.bound.rootConstants, count * 4))
	{
		ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(dx12c.cmd_list, 0, count, values, 0);
		memcpy(dx12c.bound.rootConstants, values, count * 4);
		dx12c.bound.rootConstantsSet = true;
	}
}
/* The render target and depth (or none), each with a view in its heap
 * (a transient render target view is set whatever was bound), of 2^@samples
 * samples a pixel (SSAA's) */
void dx12c_set_targets(u32 rtv, u32 dsv, u32 samples);
static INLINE void dx12c_forget_targets(void)
{
	dx12c.bound.rtv = dx12c.bound.dsv = ~0u - 1;
}
static INLINE bool dx12c_has_depth_bound(void)
{
	return dx12c.bound.dsv != ~0u && dx12c.bound.dsv != ~0u - 1;
}
/* log2 of the samples a pixel of what is bound: what a pipeline drawing into it is made for */
static INLINE u32 dx12c_target_samples(void)
{
	return dx12c.bound.samples;
}

/* A picture drawn onto what is bound, as a rectangle: the last frame's
 * into the next, the VMUs' screens and the crosshairs over the picture,
 * the framebuffer the game wrote itself, a render to a texture brought
 * down to the console's size. No vertex buffer: the rectangle is in the
 * root constants (shaders/dx12_raster.hlsl, DX12_QUAD_VS). The picture at
 * @srv of the bindless table, sampled with the linear sampler or the
 * nearest one; @x, @y, @w, @h in clip space (y going up); its coordinates
 * from (@u0, @v0) to (@u1, @v1); blended by @special (DX12_BLEND_KEY: not
 * blended; DX12_BLEND_ALPHA; DX12_BLEND_FACTOR with @factor) */
void dx12c_draw_quad_at(u32 srv, bool linear, float x, float y, float w, float h,
		float u0, float v0, float u1, float v1, u32 special, const float *factor);
#define dx12c_draw_quad(srv, linear, x, y, w, h) \
	dx12c_draw_quad_at(srv, linear, x, y, w, h, 0.f, 0.f, 1.f, 1.f, DX12_BLEND_KEY, NULL)

/* ---- The frame ---- */

/* The picture the next video_refresh is to show, drawn and left in
 * required_state by a render that has been submitted: with version 2 it
 * is handed over here, and is the frontend's (dx12c_is_handed_over())
 * until the frame has been shown */
void dx12c_set_frame(const dx12_image *image);
/* With the HDR output (Color Format): the picture @image, @width x
 * @height, encoded into the command list for the frontend's frame and
 * left in the pixel shader resource state; the next dx12c_set_frame()
 * hands the encoded one over. False without the HDR output: the picture
 * is then to be left in required_state, as ever. With SSAA's samples the
 * picture was resolved from (@samples, 2^@samplesLog2 a pixel), or their
 * channel sums (@sums: the per-pixel renderer's, which resolves them
 * itself), where the picture is their resolve their exact average is
 * encoded. */
bool dx12c_encode_hdr(dx12_image *image, u32 width, u32 height, dx12_image *samples, u32 samplesLog2, dx12_image *sums);
static INLINE bool dx12c_hdr_active(void)
{
	return dx12c.hdr != NULL;
}
static INLINE bool dx12c_is_handed_over(const dx12_image *image)
{
	return dx12c.v2 && dx12c.handed_over && image != NULL && image->resource == dx12c.frame_resource;
}
/* Just before video_refresh(RETRO_HW_FRAME_BUFFER_VALID) */
void dx12c_present(void);
/* How many pictures the screen is drawn in turn into, and which of them
 * the next frame is; before drawing into it, the frontend is waited for
 * to be done with it (version 2's wait_sync_index) */
unsigned dx12c_present_count(void);
unsigned dx12c_present_index(void);
void dx12c_wait_present_index(void);

/* A committed 2D texture in the default heap */
bool dx12_make_texture(ID3D12Device *device, u32 width, u32 height, u32 levels, DXGI_FORMAT format,
		D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ID3D12Resource **out, const D3D12_CLEAR_VALUE *clear);
/* (the struct returns, called as Microsoft's ABI has them: an out pointer) */
D3D12_RESOURCE_DESC dx12_resource_desc(ID3D12Resource *resource);

#ifdef __cplusplus
}
#endif
