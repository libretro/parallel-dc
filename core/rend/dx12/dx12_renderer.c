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
#include "dx12_renderer.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "dx12_texture.h"
#include "dx12_texdecoder.h"
#include "dx12_ta_decoder.h"
#include "dx12_overlay.h"
#include "dx12_writeout.h"
#include "dx12_shader_flags.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/tileclip.h"
#include "rend/shadows.h"
#include "rend/rtt_watch.h"
#include "rend/last_picture.h"
#include "rend/modvol_clip.h"
#include "rend/ssaa_pattern.h"
#include "rend/ssaa_rate.h"
#include "rend/upscaler.h"
#include "rend/texconv.h"

#define PPW(w, f) TAW_GET(w, f)
#define MIN_U(a, b) ((a) < (b) ? (a) : (b))
#define MAX_U(a, b) ((a) > (b) ? (a) : (b))
#define MAX_L(a, b) ((a) > (b) ? (a) : (b))
#define MIN_L(a, b) ((a) < (b) ? (a) : (b))

#define RENDERER_CHECK(name, c) typedef char dx12_renderer_check_##name[(c) ? 1 : -1]
/* (the scale's fourth word is the depth's) */
RENDERER_CHECK(vtx, offsetof(dx12_vertex_constants, scale) == 0 && sizeof(dx12_vertex_constants) == 32);
RENDERER_CHECK(poly, sizeof(dx12_poly_constants) == DX12C_POLY_CONSTANT_COUNT * 4);

extern int screen_width, screen_height;

static const IID iid_resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };

dx12_renderer dx12r;

/* The shadow passes of a polygon drawn again for its shadow (rend/shadows.h) */
enum { SHADOW_PASS_NONE, SHADOW_PASS_LATER_WINS, SHADOW_PASS_EARLIER_WINS };

/* An image of the renderer's own: a committed texture, with its place in
 * the bindless table (at @srv if given, one taken otherwise) and a render
 * target view if it is one; with @storage, unordered access views can be
 * made of it too, for the compute renderer to draw into */
static bool make_image(dx12_image *image, u32 width, u32 height, DXGI_FORMAT format, bool target, u32 srv, bool storage)
{
	D3D12_RESOURCE_FLAGS flags;
	dx12_image_reset(image);
	image->width = width;
	image->height = height;
	image->state = target ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	flags = target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
	if (storage)
		flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (!dx12_make_texture(dx12c.device, width, height, 1, format, flags, image->state, &image->resource, NULL))
		return false;
	image->srv = srv != ~0u ? srv : dx12_heap_alloc(&dx12c.srv_heap);
	if (image->srv == ~0u)
	{
		DX12_RELEASE(image->resource);
		return false;
	}
	dx12c_make_srv(image->resource, format, 1, image->srv);
	if (target)
	{
		image->rtv = dx12_heap_alloc(&dx12c.rtv_heap);
		if (image->rtv == ~0u)
		{
			if (srv == ~0u)
				dx12_heap_free(&dx12c.srv_heap, image->srv);
			dx12_image_reset(image);
			return false;
		}
		ID3D12Device_CreateRenderTargetView(dx12c.device, image->resource, NULL, dx12_heap_cpu(&dx12c.rtv_heap, image->rtv));
	}
	return true;
}

/* A picture of @samples samples a pixel in the standard pattern (SSAA's),
 * committed: of the frame's colour, with a place in the bindless table (a
 * view of its samples) and a render target view; or of its depth and
 * stencil, with no views (setup_ssaa() makes those) */
static bool make_samples(dx12_image *image, u32 width, u32 height, u32 samples, bool depthStencil)
{
	ID3D12Device *device = dx12c.device;
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC desc;
	D3D12_CLEAR_VALUE clear;
	D3D12_SHADER_RESOURCE_VIEW_DESC view;
	dx12_image_reset(image);
	image->width = width;
	image->height = height;
	image->state = depthStencil ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = depthStencil ? DX12DepthResourceFormat : DX12FrameFormat;
	desc.SampleDesc.Count = samples;
	desc.SampleDesc.Quality = DX12StandardPattern;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = depthStencil ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	memset(&clear, 0, sizeof(clear));
	if (depthStencil)
		clear.Format = DX12DepthFormat;
	else
	{
		clear.Format = DX12FrameFormat;
		clear.Color[3] = 1.f;
	}
	if (FAILED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, image->state, &clear,
			&iid_resource, (void **)&image->resource)))
	{
		WARN_LOG(RENDERER, "DX12: SSAA picture creation failed (%d x %d, %d samples)\n", width, height, samples);
		image->resource = NULL;
		dx12_image_reset(image);
		return false;
	}
	if (depthStencil)
		return true;
	image->srv = dx12_heap_alloc(&dx12c.srv_heap);
	image->rtv = dx12_heap_alloc(&dx12c.rtv_heap);
	if (image->srv == ~0u || image->rtv == ~0u)
		return false;
	memset(&view, 0, sizeof(view));
	view.Format = DX12FrameFormat;
	view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
	view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	ID3D12Device_CreateShaderResourceView(device, image->resource, &view, dx12_heap_cpu(&dx12c.srv_heap, image->srv));
	ID3D12Device_CreateRenderTargetView(device, image->resource, NULL, dx12_heap_cpu(&dx12c.rtv_heap, image->rtv));
	return true;
}

/* Room in the ring for an image's rows, at the pitch a copy to a texture wants */
static u8 *reserve_image(dx12_upload *upload, u32 width, u32 height, u32 bpp, u32 *pitch)
{
	*pitch = (width * bpp + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	if (!dx12c_upload(*pitch * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, upload))
		return NULL;
	return upload->cpu;
}

/* The rows written to @upload into @image, which is left to be sampled */
static void record_image(dx12_image *image, const dx12_upload *upload, u32 width, u32 height, u32 pitch)
{
	D3D12_TEXTURE_COPY_LOCATION dst, src;
	D3D12_RESOURCE_DESC desc;
	dx12c_transition(image, D3D12_RESOURCE_STATE_COPY_DEST);
	memset(&dst, 0, sizeof(dst));
	dst.pResource = image->resource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	memset(&src, 0, sizeof(src));
	src.pResource = upload->buffer;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = upload->offset;
	desc = dx12_resource_desc(image->resource);
	src.PlacedFootprint.Footprint.Format = desc.Format;
	src.PlacedFootprint.Footprint.Width = width;
	src.PlacedFootprint.Footprint.Height = height;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = pitch;
	ID3D12GraphicsCommandList_CopyTextureRegion(dx12c_cmd(), &dst, 0, 0, 0, &src, NULL);
	dx12c_transition(image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

/* Rows into the ring and then into @image, which is left to be sampled */
static bool upload_image(dx12_image *image, const void *pixels, u32 width, u32 height, u32 bpp)
{
	dx12_upload upload;
	u32 pitch, y;
	u8 *out;
	if (image->resource == NULL)
		return false;
	out = reserve_image(&upload, width, height, bpp, &pitch);
	if (out == NULL)
		return false;
	if (pitch == width * bpp)
		memcpy(out, pixels, (size_t)pitch * height);
	else
		for (y = 0; y < height; y++)
			memcpy(out + (size_t)y * pitch, (const u8 *)pixels + (size_t)y * width * bpp, (size_t)width * bpp);
	record_image(image, &upload, width, height, pitch);
	return true;
}

/* @resource let go of once the GPU is done with it */
static void let_go(ID3D12Resource **resource)
{
	if (*resource == NULL)
		return;
	dx12c_release((IUnknown *)*resource);
	*resource = NULL;
}

void dx12r_drop_image(dx12_image *image)
{
	let_go(&image->resource);
	/* (the fog's and the palette's places are theirs for good) */
	if (image->srv >= DX12_SRV_FIRST_FREE && image->srv != ~0u)
		dx12c_release_srv(image->srv);
	if (image->rtv != ~0u)
		dx12_heap_free(&dx12c.rtv_heap, image->rtv);
	dx12_image_reset(image);
}

static void drop_restored(void)
{
	dx12r_drop_image(&dx12r.restored_picture);
}

/* The renderer as a new one is: nothing made */
static void construct(const dx12_renderer_kind *kind, bool storage)
{
	u32 i;
	memset(&dx12r, 0, sizeof(dx12r));
	dx12r.kind = kind;
	dx12r.storage_pictures = storage;
	transform_matrix_init(&dx12r.matrices, false);
	dx12r.gpu_vtx_constants_state = D3D12_RESOURCE_STATE_COPY_DEST;
	dx12_image_init(&dx12r.fog_texture);
	dx12_image_init(&dx12r.palette_texture);
	dx12_image_init(&dx12r.depth);
	dx12_image_init(&dx12r.other_depth);
	dx12r.stencil_state = dx12r.other_stencil_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	dx12r.dsv_read_stencil = dx12r.dsv_read_only = ~0u;
	dx12r.other_dsv_read_stencil = dx12r.other_dsv_read_only = ~0u;
	for (i = 0; i < DX12_SCREEN_SLOTS; i++)
	{
		dx12_image_init(&dx12r.screens[i]);
		dx12_image_init(&dx12r.upscaler_slots[i].motion);
		dx12_image_init(&dx12r.upscaler_slots[i].depth);
	}
	dx12_image_init(&dx12r.extra);
	dx12_image_init(&dx12r.restored_picture);
	dx12_image_init(&dx12r.ssaa_color[0]);
	dx12_image_init(&dx12r.ssaa_color[1]);
	dx12_image_init(&dx12r.fb_texture);
	dx12r.last_screen = -1;
	dx12r.target_rtv = ~0u;
	dx12r.depth_copy_srv = ~0u;
	dx12r.ssaa_samples = 1;
}

static u32 ta_flags(void)
{
	return dx12r.kind->ta_flags();
}

/* (what the decoder has been asked for, recorded) */
static void flush_decoder(void *unused)
{
	(void)unused;
	dx12_texdec_flush();
}

static void setup_ssaa(u32 width, u32 height);
static void drop_ssaa(void);
static void setup_upscaler(void);
static void drop_upscaler(void);

bool dx12r_init(const dx12_renderer_kind *kind, bool storage)
{
	ID3D12Device *device;
	bool success = true;

	construct(kind, storage);
	/* (the passes' sorted draws: room before the first frame) */
	SortPassesReserve(&dx12r.sorted_passes);
	SortPassesReserve(&dx12r.prepass_passes);
	device = dx12r.device = dx12c.device;
	if (device == NULL)
	{
		WARN_LOG(RENDERER, "DX12: no device\n");
		return false;
	}
	/* The fog table, two rows of 128: see check_fog_texture() */
	success = success && make_image(&dx12r.fog_texture, 128, 2, DXGI_FORMAT_R8_UNORM, false, DX12_SRV_FOG, false);
	dx12r.fog_uploaded_valid = false;
	/* The palette, a row of 1024 colours */
	success = success && make_image(&dx12r.palette_texture, 1024, 1, DXGI_FORMAT_R8G8B8A8_UNORM, false, DX12_SRV_PALETTE, false);
	palette_updated = true;
	/* the depth, for the screen at least: made now and not in the first frame */
	success = success && dx12r_ensure_depth((u32)MAX_L(screen_width, 640), (u32)MAX_L(screen_height, 480));
	success = success && dx12_overlay_init();
	if (dx12r.batch == NULL)
		dx12r.batch = (dx12_draw_args *)malloc(DX12R_BATCH_MAX * sizeof(dx12_draw_args));
	success = success && dx12r.batch != NULL;
	dx12r.batch_count = 0;
	dx12r.stat_batches = dx12r.stat_batched = 0;

	dx12r.have_picture = false;
	dx12r.last_screen = -1;
	dx12r.last_in_extra = false;
	dx12r.screen_width = dx12r.screen_height = 0;
	dx12r.held_from_process = false;
	if (!success)
	{
		WARN_LOG(RENDERER, "DX12 renderer initialization failed\n");
		return false;
	}
	dx12_make_samplers();
	/* textures made on the GPU, and the textures themselves, all of them now (rend/tex_pool.h) */
	dx12_texdec_init();
	/* (what the decoder has been asked for goes into the list a readback submits early) */
	dx12c_set_before_submit(flush_decoder, NULL);
	/* (the pictures rendered to: drawn into by the compute renderer too, when it is wanted) */
	dx12_pool_set_target_storage(dx12r.storage_pictures);
	dx12_pool_init();
	/* ...and the cache's room for them (rend/tex_map.h) */
	dx12_texcache_reserve_for(dx12_pool_made());
	dx12_textures_attach(dx12_texdec_ready(), true);
	/* what a render read back takes, made now (dx12_rtt_init()) */
	dx12_rtt_init();
	/* the pipelines, and SSAA's pictures and pipelines made from them */
	kind->prepare_pipelines();
	/* the chip's write-out, and the pictures made for it (dx12_writeout.h) */
	dx12_writeout_init();
	dx12r.writeout_pictures = dx12_writeout_wanted();
	setup_ssaa((u32)screen_width, (u32)screen_height);
	/* where frames are parsed when the card can draw them from there: made once */
	if (!dx12r.parse_arena.ready)
	{
		size_t room[PARSE_ARENA_PARTS];
		tactx_arena_rooms(room);
		dx12r.arena_missed = false;
		dx12r.arena_here = dx12r.arena_busy = dx12r.arena_other = 0;
		if (parse_arena_d3d12_init(&dx12r.parse_arena, device, dx12c.fence, room, 4))
			NOTICE_LOG(RENDERER, "DX12: renders parsed where the card reads them, not copied there\n");
		else
			INFO_LOG(RENDERER, "DX12: renders copied to the card (its memory is not the processor's, cache-coherent)\n");
	}
	setup_upscaler();
	/* GPU-side TA processing, if it is on: here, not in a frame; and the
	 * buffer its frames' vertex constants go to (dx12r_upload_constants()) */
	if (dx12r.gpu_vtx_constants == NULL)
	{
		D3D12_HEAP_PROPERTIES heap;
		D3D12_RESOURCE_DESC desc;
		memset(&heap, 0, sizeof(heap));
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		memset(&desc, 0, sizeof(desc));
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		dx12r.gpu_vtx_constants_state = D3D12_RESOURCE_STATE_COPY_DEST;
		if (FAILED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, dx12r.gpu_vtx_constants_state, NULL,
				&iid_resource, (void **)&dx12r.gpu_vtx_constants)))
			dx12r.gpu_vtx_constants = NULL;
	}
	dx12_ta_want_sort(kind->ta_sorts);
	dx12_ta_setup(ta_flags() != 0 && dx12r.gpu_vtx_constants != NULL && ta_gpu_wanted());
	return true;
}

void dx12r_options_changed(void)
{
	if (dx12r.device != NULL)
		dx12_ta_setup(ta_flags() != 0 && dx12r.gpu_vtx_constants != NULL && ta_gpu_wanted());
}

bool dx12r_ready(void)
{
	/* (its pipelines made) */
	return dx12_pso_made();
}

static void release_later(void *c, IUnknown *object)
{
	(void)c;
	dx12c_release(object);
}

void dx12r_term(void)
{
	u32 i;
	SortPassesFree(&dx12r.sorted_passes);
	SortPassesFree(&dx12r.prepass_passes);
	if (dx12r.device == NULL)
		return;
	dx12r_flush_batch();
	drop_ssaa();
	dx12_writeout_term();
	ta_set_flat_strips(false);
	ta_set_modvol_clip(false);
	dx12c_set_frame(NULL);
	dx12c_set_before_submit(NULL, NULL);
	dx12_rtt_term();
	dx12_texcache_clear();
	/* (what the cache's textures had is back in the pool by now) */
	dx12_textures_attach(false, false);
	dx12_texcache_free();
	dx12_pool_term();
	dx12_texdec_term();
	dx12_ta_term();
	let_go(&dx12r.gpu_vtx_constants);
	dx12r.gpu_vtx_constants_state = D3D12_RESOURCE_STATE_COPY_DEST;
	if (dx12r.parse_arena.ready)
		NOTICE_LOG(RENDERER, "DX12: %u renders parsed where the card reads them, %u copied there with no slice free, %u copied otherwise\n",
				dx12r.arena_here, dx12r.arena_busy, dx12r.arena_other);
	/* (let go once the frames drawn from it are done) */
	parse_arena_d3d12_term(&dx12r.parse_arena, release_later, NULL);
	dx12_overlay_term();
	dx12r_drop_image(&dx12r.fb_texture);
	drop_restored();
	for (i = 0; i < dx12r.screen_count; i++)
		dx12r_drop_image(&dx12r.screens[i]);
	dx12r.screen_count = 0;
	dx12r_drop_image(&dx12r.extra);
	dx12r.last_in_extra = false;
	drop_upscaler();
	if (dx12r.depth.resource != NULL)
	{
		let_go(&dx12r.depth.resource);
		dx12_heap_free(&dx12c.dsv_heap, dx12r.depth.rtv);
		dx12_heap_free(&dx12c.dsv_heap, dx12r.dsv_read_stencil);
		dx12_heap_free(&dx12c.dsv_heap, dx12r.dsv_read_only);
	}
	dx12_image_reset(&dx12r.depth);
	dx12r.dsv_read_stencil = dx12r.dsv_read_only = ~0u;
	dx12r_drop_image(&dx12r.palette_texture);
	dx12r_drop_image(&dx12r.fog_texture);
	free(dx12r.batch);
	dx12r.batch = NULL;
	NOTICE_LOG(RENDERER, "DX12: %u draws in %u batches; %u submits, %u stalls\n", dx12r.stat_batched, dx12r.stat_batches, dx12c.stat_submits, dx12c.stat_stalls);
	dx12r.device = NULL;
}

/* The size rendered at has changed, and the context is the same: the
 * screen's pictures are made again at the new size when the next frame is
 * drawn, and the last picture goes with them - dx12r_ensure_screen().
 * SSAA's are made again now, or SSAA is taken up or left: a change of the
 * option is one of size. */
void dx12r_resize(int w, int h)
{
	if (dx12r.device != NULL)
	{
		setup_ssaa((u32)MAX_L(w, 0), (u32)MAX_L(h, 0));
		setup_upscaler();
	}
	/* (a change of the write-out option is one of size: the pictures are
	 * made again, for it to write or not) */
	if (dx12r.device != NULL && dx12r.writeout_pictures != dx12_writeout_wanted())
	{
		dx12_writeout_init();
		dx12r.writeout_pictures = dx12_writeout_wanted();
		dx12r.screen_width = dx12r.screen_height = 0;
	}
}

/* ---- SSAA ---- */

static void setup_ssaa(u32 width, u32 height)
{
	ID3D12Device *device = dx12r.device;
	const dx12_renderer_kind *kind = dx12r.kind;
	D3D12_DEPTH_STENCIL_VIEW_DESC view;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv;
	u32 samples = 1, i;
	bool ok = true;
	/* (the upscaler is the anti-aliasing then, and reads the depth, which SSAA's samples are not) */
	if (settings.rend.Ssaa > 1 && width != 0 && height != 0 && !upscaler_active())
		samples = dx12c_ssaa_samples(settings.rend.Ssaa, ssaa_per_sample_kib(width, height, 4 * kind->ssaa_color_images + 8),
				kind->ssaa_fixed_kib != NULL ? kind->ssaa_fixed_kib(width, height) : 0,
				kind->binds_screen ? "DX12 per-triangle renderer" : "DX12 per-pixel renderer");
	if (samples == dx12r.ssaa_samples && (samples == 1 || (width == dx12r.ssaa_width && height == dx12r.ssaa_height)))
		return;
	drop_ssaa();
	if (samples <= 1)
		return;
	for (i = 0; i < kind->ssaa_color_images && i < 2; i++)
		ok = ok && make_samples(&dx12r.ssaa_color[i], width, height, samples, false);
	ok = ok && make_samples(&dx12r.other_depth, width, height, samples, true);
	if (ok)
	{
		dx12r.other_stencil_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		dx12r.other_depth.rtv = dx12_heap_alloc(&dx12c.dsv_heap);
		dx12r.other_dsv_read_stencil = dx12_heap_alloc(&dx12c.dsv_heap);
		dx12r.other_dsv_read_only = dx12_heap_alloc(&dx12c.dsv_heap);
		ok = dx12r.other_depth.rtv != ~0u && dx12r.other_dsv_read_stencil != ~0u && dx12r.other_dsv_read_only != ~0u;
	}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX12: SSAA off: its pictures could not be made\n");
		drop_ssaa();
		return;
	}
	memset(&view, 0, sizeof(view));
	view.Format = DX12DepthFormat;
	view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
	ID3D12Device_CreateDepthStencilView(device, dx12r.other_depth.resource, &view, dx12_heap_cpu(&dx12c.dsv_heap, dx12r.other_depth.rtv));
	view.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
	ID3D12Device_CreateDepthStencilView(device, dx12r.other_depth.resource, &view, dx12_heap_cpu(&dx12c.dsv_heap, dx12r.other_dsv_read_stencil));
	view.Flags = (D3D12_DSV_FLAGS)(D3D12_DSV_FLAG_READ_ONLY_STENCIL | D3D12_DSV_FLAG_READ_ONLY_DEPTH);
	ID3D12Device_CreateDepthStencilView(device, dx12r.other_depth.resource, &view, dx12_heap_cpu(&dx12c.dsv_heap, dx12r.other_dsv_read_only));
	/* the per-pixel renderer's views of its samples' stencil and depth (its shaders read them) */
	memset(&srv, 0, sizeof(srv));
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
	ID3D12Device_CreateShaderResourceView(device, dx12r.other_depth.resource, &srv, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_SSAA_FIRST + 3));
	srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	ID3D12Device_CreateShaderResourceView(device, dx12r.other_depth.resource, &srv, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_SSAA_FIRST + 4));
	dx12r.ssaa_samples = samples;
	dx12r.ssaa_log2 = SSAA_LOG2(samples);
	dx12r.ssaa_width = width;
	dx12r.ssaa_height = height;
	dx12_pso_make_for_samples(dx12r.ssaa_log2, dx12c_hdr_active());
}

static void drop_ssaa(void)
{
	u32 i;
	if (dx12r.device == NULL)
		return;
	dx12r_ssaa_depth(false);
	for (i = 0; i < 2; i++)
		dx12r_drop_image(&dx12r.ssaa_color[i]);
	let_go(&dx12r.other_depth.resource);
	if (dx12r.other_depth.rtv != ~0u)
		dx12_heap_free(&dx12c.dsv_heap, dx12r.other_depth.rtv);
	if (dx12r.other_dsv_read_stencil != ~0u)
		dx12_heap_free(&dx12c.dsv_heap, dx12r.other_dsv_read_stencil);
	if (dx12r.other_dsv_read_only != ~0u)
		dx12_heap_free(&dx12c.dsv_heap, dx12r.other_dsv_read_only);
	dx12_image_reset(&dx12r.other_depth);
	dx12r.other_dsv_read_stencil = dx12r.other_dsv_read_only = ~0u;
	dx12r.ssaa_samples = 1;
	dx12r.ssaa_log2 = 0;
	dx12r.ssaa_width = dx12r.ssaa_height = 0;
}

void dx12r_ssaa_depth(bool in)
{
	dx12_image image;
	D3D12_RESOURCE_STATES state;
	u32 dsv;
	if (in == dx12r.ssaa_depth_in || (in && (dx12r.ssaa_samples <= 1 || dx12r.other_depth.resource == NULL)))
		return;
	image = dx12r.depth;
	dx12r.depth = dx12r.other_depth;
	dx12r.other_depth = image;
	state = dx12r.stencil_state;
	dx12r.stencil_state = dx12r.other_stencil_state;
	dx12r.other_stencil_state = state;
	dsv = dx12r.dsv_read_stencil;
	dx12r.dsv_read_stencil = dx12r.other_dsv_read_stencil;
	dx12r.other_dsv_read_stencil = dsv;
	dsv = dx12r.dsv_read_only;
	dx12r.dsv_read_only = dx12r.other_dsv_read_only;
	dx12r.other_dsv_read_only = dsv;
	dx12r.ssaa_depth_in = in;
}

/* The picture of samples at @srv of the table resolved into what is bound */
static void ssaa_resolve(u32 srv)
{
	DX12PsoKey key;
	ID3D12PipelineState *pso;
	u32 constants[2];
	memset(&key, 0, sizeof(key));
	key.kind = DX12_PSO_SSAA_RESOLVE;
	key.writeColor = true;
	key.depthFunc = 7;
	pso = dx12_pso_get(&key);
	if (pso == NULL)
		return;
	dx12c_set_pipeline(pso);
	dx12c_set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	constants[0] = srv;
	constants[1] = dx12r.ssaa_log2;
	dx12c_set_root_constants(constants, 2);
	ID3D12GraphicsCommandList_DrawInstanced(dx12c_cmd(), 4, 1, 0, 0);
}

u64 dx12r_get_texture(u32 tsp, u32 tcw, int area)
{
	dx12_texture *tf = dx12_texcache_get(tsp, tcw, area);

	if (tf->pooled == NULL)
		texcache_create(&tf->base);
	/* update if needed */
	if (texcache_needs_update(&tf->base))
		texcache_update(&tf->base);
	else if (texcache_custom_available(&tf->base))
		texcache_check_custom(&tf->base);

	return (u64)(uintptr_t)tf;
}

/* A picture kept from the context before: the first render to the screen
 * starts from it */
static void check_kept_picture(void)
{
	int width, height;
	const u8 *pixels = last_picture(&width, &height);
	if (pixels == NULL)
		return;
	drop_restored();
	if (!make_image(&dx12r.restored_picture, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, false, ~0u, false)
			|| !upload_image(&dx12r.restored_picture, pixels, width, height, 4))
		drop_restored();
}

/* Uploaded when it differs from what the card has: see UpdateFogTexture() in gles/gles.c */
static void check_fog_texture(void)
{
	u8 texData[256];
	MakeFogTexture(texData);
	if (dx12r.fog_uploaded_valid && !memcmp(dx12r.fog_uploaded, texData, sizeof(dx12r.fog_uploaded)))
		return;
	memcpy(dx12r.fog_uploaded, texData, sizeof(dx12r.fog_uploaded));
	dx12r.fog_uploaded_valid = true;
	upload_image(&dx12r.fog_texture, texData, 128, 2, 1);
}

static void check_palette_texture(void)
{
	if (!palette_updated)
		return;
	palette_updated = false;
	upload_image(&dx12r.palette_texture, palette32_ram, 1024, 1, 4);
}

static bool upload_framebuffer(void);

static bool process_locked(TA_context *ctx)
{
	bool result;
	dx12_ta_begin_frame();
	if (!ctx->rend.isRTT)
		dx12_overlay_prepare();
	if (!ctx->rend.isRTT && !ctx->rend.isRenderFramebuffer)
		check_kept_picture();

	if (ctx->rend.isRenderFramebuffer)
		return upload_framebuffer();

	if (KillTex)
	{
		/* what was rendered to a texture and is not in video memory yet goes there before its picture does */
		rtt_watch_flush();
		dx12_texcache_clear();
	}

	/* SSAA draws flat strips at a single rate: the parse looks for them
	 * then, and only then */
	ta_set_flat_strips(dx12r.ssaa_samples > 1);
	/* the modifier volumes' triangles far off the screen cut before the GPU clips them (rend/modvol_clip.h) */
	ta_set_modvol_clip(true);
	/* GPU-side TA processing: the header pass makes the frame ready to be
	 * drawn from the GPU's decode, in the full parse's place, which is
	 * recorded now; or it is parsed as ever */
	if (!dx12_ta_ready() || !dx12_ta_prepare(ctx, ta_flags(), &result))
	{
		/* into a slice of the memory the card reads, where there is one
		 * free: the draws read it there, uncopied (with the background
		 * polygon's four vertices FillBGP() wrote, at the head of the list
		 * the parse empties and takes them back in) */
		ta_list_t *lists[PARSE_ARENA_PARTS];
		int attached;
		tactx_arena_lists(ctx, lists);
		attached = parse_arena_d3d12_attach(&dx12r.parse_arena, lists, 4 * sizeof(Vertex));
		if (attached > 0)
			dx12r.arena_here++;
		else if (attached < 0)
		{
			dx12r.arena_busy++;
			if (!dx12r.arena_missed)
			{
				dx12r.arena_missed = true;
				NOTICE_LOG(RENDERER, "DX12: a render copied to the card, no slice of the parse's free\n");
			}
		}
		else if (dx12r.parse_arena.ready && dx12r.arena_other++ == 0)
			NOTICE_LOG(RENDERER, "DX12: a render copied to the card, its lists not of the parse's rooms\n");
		result = ta_parse_vdrc(ctx);
		if (!result)
			parse_arena_d3d12_cancel(&dx12r.parse_arena);
	}
	else if (result)
		dx12_ta_record(ctx);
	/* the textures the frame's polygons asked for that are made on the GPU, before the draws */
	dx12_texdec_flush();

	dx12_texcache_cleanup();

	if (result)
	{
		check_fog_texture();
		check_palette_texture();
	}
	return result;
}

bool dx12r_process(TA_context *ctx)
{
	/* A render to the screen with threaded rendering is handed over to the
	 * other thread when this is done: the frontend is waited for before the
	 * screen's picture is drawn into, and not here, which would keep the
	 * emulation waiting too. */
	const bool keep = ctx->rend.isRTT || !settings.rend.ThreadedRendering;
	bool result;
	if (dx12r.device == NULL)
		return false;
	if (keep && !ctx->rend.isRTT)
		dx12c_wait_present_index();
	result = process_locked(ctx);
	dx12r.held_from_process = keep && result;
	return result;
}

/* ---- Clipping ---- */

int dx12r_set_tile_clip(u32 val, D3D12_RECT *clipRect)
{
	int rect[4] = { 0, 0, 0, 0 };
	const int clipmode = tile_clip(val, &dx12r.matrices.viewport, rect);
	if (clipmode == TILECLIP_OUTSIDE)
		intersect_clip_rect(rect, dx12r.base_scissor.left, dx12r.base_scissor.top,
				dx12r.base_scissor.right - dx12r.base_scissor.left, dx12r.base_scissor.bottom - dx12r.base_scissor.top);
	clipRect->left = rect[0];
	clipRect->top = rect[1];
	clipRect->right = rect[0] + rect[2];
	clipRect->bottom = rect[1] + rect[3];

	return clipmode;
}

/* (lroundf()'s: half away from zero, and no less than 0) */
static LONG round_at_least_0(float v)
{
	const long r = tileclip_round(v);
	return (LONG)(r > 0 ? r : 0);
}

static void set_rect(D3D12_RECT *r, LONG left, LONG top, LONG right, LONG bottom)
{
	r->left = left;
	r->top = top;
	r->right = right;
	r->bottom = bottom;
}

void dx12r_set_base_scissor(void)
{
	const bool wide_screen_on = settings.rend.WideScreen && !pvrrc.isRenderFramebuffer
			&& !transform_matrix_clipped(&dx12r.matrices);
	if (!wide_screen_on)
	{
		if (pvrrc.isRenderFramebuffer)
			set_rect(&dx12r.base_scissor, 0, 0, 640, 480);
		else
		{
			const xform *scissor = &dx12r.matrices.scissor;
			float min_x = xform_x(scissor, pvrrc.fb_X_CLIP.min);
			float min_y = xform_y(scissor, pvrrc.fb_Y_CLIP.min);
			float width = xform_w(scissor, pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1);
			float height = xform_h(scissor, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
			LONG x, y;
			if (width < 0)
			{
				min_x += width;
				width = -width;
			}
			if (height < 0)
			{
				min_y += height;
				height = -height;
			}
			x = round_at_least_0(min_x);
			y = round_at_least_0(min_y);
			set_rect(&dx12r.base_scissor, x, y, x + round_at_least_0(width), y + round_at_least_0(height));
		}
	}
	else
		set_rect(&dx12r.base_scissor, 0, 0, (LONG)screen_width, (LONG)screen_height);
}

/* Direct3D takes a flat shaded triangle's colours from its first vertex.
 * On Dreamcast the last vertex is the provoking one so it is copied onto the
 * first (as in the Vulkan renderers). */
static void set_provoking_vertex(const PolyParam *list, int used)
{
	Vertex *verts = TA_LIST_HEAD(pvrrc.verts, Vertex);
	const u32 *idx = TA_LIST_HEAD(pvrrc.idx, u32);
	int i;
	for (i = 0; i < used; i++)
	{
		const PolyParam *pp = &list[i];
		if (!PPW(pp->pcw, PCW_Gouraud) && pp->count > 2)
		{
			u32 n;
			for (n = 0; n < pp->count - 2; n++)
			{
				Vertex *vertex = &verts[idx[pp->first + n]];
				const Vertex *lastVertex = &verts[idx[pp->first + n + 2]];
				memcpy(vertex->col, lastVertex->col, 4);
				memcpy(vertex->vtx_spc, lastVertex->vtx_spc, 4);
				memcpy(vertex->col1, lastVertex->col1, 4);
				memcpy(vertex->spc1, lastVertex->spc1, 4);
			}
		}
	}
}

void dx12r_set_provoking_vertices(void)
{
	/* (a frame decoded on the GPU has had it done there: TA_GPU_PROVOKE) */
	if (pvrrc.gpu)
		return;
	set_provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_op, PolyParam), TA_LIST_USED(pvrrc.global_param_op, PolyParam));
	set_provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam), TA_LIST_USED(pvrrc.global_param_pt, PolyParam));
	set_provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam), TA_LIST_USED(pvrrc.global_param_tr, PolyParam));
}

/* The sorted passes' triangles and their prepasses', their indices written
 * to @out (SortPassesRoom() of them): the ring, which the card reads them
 * from */
static void sort_triangles(u32 *out)
{
	dx12r.sorted_index_count = SortPassesTo(&dx12r.sorted_passes, &dx12r.prepass_passes, out, true, true);
}

D3D12_RECT dx12r_cut_scissor(const D3D12_RECT *rect)
{
	D3D12_RECT cut = *rect;
	if (dx12r.kind->cut_scissor != NULL)
		dx12r.kind->cut_scissor(&cut);
	return cut;
}

/* ---- Buffers ---- */

static void make_fragment_constants(dx12_fragment_constants *constants)
{
	/* VERT and RAM fog color constants */
	const u8 *fog_colvert_bgra = (const u8 *)&FOG_COL_VERT;
	const u8 *fog_colram_bgra = (const u8 *)&FOG_COL_RAM;
	/* Fog density constant */
	const u8 *fog_density = (const u8 *)&FOG_DENSITY;
	const float fog_den_mant = fog_density[1] / 128.0f;	/* bit 7 -> x. bit, so [6:0] -> fraction -> /128 */
	const s32 fog_den_exp = (s8)fog_density[0];
	memset(constants, 0, sizeof(*constants));
	constants->fogColVert[0] = fog_colvert_bgra[2] / 255.0f;
	constants->fogColVert[1] = fog_colvert_bgra[1] / 255.0f;
	constants->fogColVert[2] = fog_colvert_bgra[0] / 255.0f;

	constants->fogColRam[0] = fog_colram_bgra[2] / 255.0f;
	constants->fogColRam[1] = fog_colram_bgra[1] / 255.0f;
	constants->fogColRam[2] = fog_colram_bgra[0] / 255.0f;

	constants->fogDensity = fog_den_mant * (float)pow(2.0, (double)fog_den_exp);

	constants->colorClampMin[0] = ((pvrrc.fog_clamp_min >> 16) & 0xFF) / 255.0f;
	constants->colorClampMin[1] = ((pvrrc.fog_clamp_min >> 8) & 0xFF) / 255.0f;
	constants->colorClampMin[2] = ((pvrrc.fog_clamp_min >> 0) & 0xFF) / 255.0f;
	constants->colorClampMin[3] = ((pvrrc.fog_clamp_min >> 24) & 0xFF) / 255.0f;

	constants->colorClampMax[0] = ((pvrrc.fog_clamp_max >> 16) & 0xFF) / 255.0f;
	constants->colorClampMax[1] = ((pvrrc.fog_clamp_max >> 8) & 0xFF) / 255.0f;
	constants->colorClampMax[2] = ((pvrrc.fog_clamp_max >> 0) & 0xFF) / 255.0f;
	constants->colorClampMax[3] = ((pvrrc.fog_clamp_max >> 24) & 0xFF) / 255.0f;

	/* half a step under the reference: see the same in gles/gles.c */
	constants->alphaTestValue = ((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f;
	constants->shadeScale = FPU_SHAD_SCALE.scale_factor / 256.f;
	constants->pixelBufferCount = dx12r.pixel_buffer_room;
	constants->ssaaMask = dx12r_ssaa_active() ? (1u << dx12r.ssaa_samples) - 1 : 1;
	constants->ssaaLog2 = dx12r_ssaa_active() ? dx12r.ssaa_log2 : 0;
}

bool dx12r_upload_constants(void)
{
	/* The constants: the normal matrix, made for a picture whose first line
	 * is the top one and whose clip space has y going up: the other way up
	 * from the Vulkan renderers', whose matrices these are, and nothing
	 * else. A few words in the ring every render, read by the GPU where
	 * they are. */
	const xform *normal = &dx12r.matrices.normal;
	dx12_vertex_constants vtx;
	dx12_fragment_constants frag;
	dx12_upload c;
	/* (and the depth's scale: one over the frame's largest 1/w) */
	vtx.scale[0] = normal->sx;
	vtx.scale[1] = -normal->sy;
	vtx.scale[2] = normal->sz;
	vtx.scale[3] = 1.f / pvrrc.fZ_max;
	vtx.offset[0] = normal->tx;
	vtx.offset[1] = -normal->ty;
	vtx.offset[2] = normal->tz;
	vtx.offset[3] = 0.f;
	make_fragment_constants(&frag);
	/* (two whole 256-byte registers: a constant buffer is read by the register) */
	if (!dx12c_upload(2 * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, &c))
		return false;
	memcpy(c.cpu, &vtx, sizeof(vtx));
	memcpy(c.cpu + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, &frag, sizeof(frag));
	dx12r.vtx_constants_gpu = c.gpu;
	dx12r.frag_constants_gpu = c.gpu + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
	if (pvrrc.gpu && dx12r.gpu_vtx_constants != NULL)
	{
		/* A frame decoded on the GPU: its fZ_max is the GPU's. The vertex
		 * constants go to the buffer of the default heap, and the depth's
		 * scale (scale[3]) from the decode's words: three copies that do not
		 * overlap, which the transitions put after the last frame's draws
		 * and before this one's. */
		ID3D12GraphicsCommandList *cmd = dx12c_cmd();
		ID3D12Resource *dst = dx12r.gpu_vtx_constants;
		dx12c_transition_resource(dst, &dx12r.gpu_vtx_constants_state, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
		ID3D12GraphicsCommandList_CopyBufferRegion(cmd, dst, 0, c.buffer, c.offset, 12);
		dx12_ta_copy_depth_scale(dst, 12);
		ID3D12GraphicsCommandList_CopyBufferRegion(cmd, dst, 16, c.buffer, c.offset + 16, 16);
		dx12c_transition_resource(dst, &dx12r.gpu_vtx_constants_state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
		dx12r.vtx_constants_gpu = ID3D12Resource_GetGPUVirtualAddress(dst);
	}
	return true;
}

/* The polygons, the volumes and the indexes, and the sorted triangles'
 * indexes after those, sorted there, and the constants, in the ring */
static bool upload_geometry(void)
{
	u32 vertexBytes, idxBytes, indexBytes, modVolBytes, indexes, modVols, total;
	const void *modVolHead;
	D3D12_GPU_VIRTUAL_ADDRESS here;
	dx12_upload upload;
	const bool clipped = TA_LIST_USED(pvrrc.mv_clip, ModTriangle) != 0;
	if (pvrrc.gpu)
	{
		/* (the decode's, on the GPU, its sorted passes sorted already and
		 * their indices in the decoder's room for them: see TA_GPU_SORT) */
		const dx12_ta_draw *gpu = dx12_ta_get_draw();
		dx12r.sorted_index_count = 0;
		dx12r.vertex_view = gpu->vertices;
		dx12r.index_view = gpu->indices;
		dx12r.modvol_view = gpu->modtrig;
		dx12r.have_modvols = pvrrc.gpu_modtrig != 0;
		/* ...but for the volumes' triangles with those far off the screen
		 * cut (rend/modvol_clip.h), which are the CPU's */
		if (clipped)
		{
			const u32 bytes = TA_LIST_BYTES(pvrrc.mv_clip);
			if (!dx12c_upload(bytes, 64, &upload))
			{
				WARN_LOG(RENDERER, "DX12: the render's volumes do not fit the upload ring (%u bytes)\n", bytes);
				return false;
			}
			memcpy(upload.cpu, TA_LIST_HEAD(pvrrc.mv_clip, ModTriangle), bytes);
			dx12r.modvol_view.BufferLocation = upload.gpu;
			dx12r.modvol_view.SizeInBytes = bytes;
			dx12r.modvol_view.StrideInBytes = sizeof(float) * 3;
			dx12r.have_modvols = true;
		}
		return dx12r_upload_constants();
	}
	vertexBytes = TA_LIST_BYTES(pvrrc.verts);
	idxBytes = TA_LIST_BYTES(pvrrc.idx);
	/* (room for the sorted passes' indices, which are sorted into it) */
	indexBytes = idxBytes + SortPassesRoom() * sizeof(u32);
	modVolHead = clipped ? (const void *)TA_LIST_HEAD(pvrrc.mv_clip, ModTriangle) : (const void *)TA_LIST_HEAD(pvrrc.modtrig, ModTriangle);
	modVolBytes = clipped ? TA_LIST_BYTES(pvrrc.mv_clip) : TA_LIST_BYTES(pvrrc.modtrig);
	/* Parsed where the card reads it (the parse arena): drawn from there,
	 * the sorted passes' indices after the frame's, in the room they have
	 * there - or, with more of them, copied as below */
	here = parse_arena_d3d12_gpu(&dx12r.parse_arena, PARSE_ARENA_VERTS);
	if (here != 0 && indexBytes <= dx12r.parse_arena.arena.space[PARSE_ARENA_IDX])
	{
		/* sorted from the vertices as the game sent them, then those made for the card, in place */
		sort_triangles(TA_LIST_HEAD(pvrrc.idx, u32) + TA_LIST_USED(pvrrc.idx, u32));
		dx12r_set_provoking_vertices();
		dx12r.vertex_view.BufferLocation = here;
		dx12r.vertex_view.SizeInBytes = vertexBytes;
		dx12r.vertex_view.StrideInBytes = sizeof(Vertex);
		dx12r.index_view.BufferLocation = parse_arena_d3d12_gpu(&dx12r.parse_arena, PARSE_ARENA_IDX);
		dx12r.index_view.SizeInBytes = idxBytes + dx12r.sorted_index_count * sizeof(u32);
		dx12r.index_view.Format = DXGI_FORMAT_R32_UINT;
		dx12r.modvol_view.BufferLocation = parse_arena_d3d12_gpu(&dx12r.parse_arena, clipped ? PARSE_ARENA_MV_CLIP : PARSE_ARENA_MODTRIG);
		dx12r.modvol_view.SizeInBytes = modVolBytes;
		dx12r.modvol_view.StrideInBytes = sizeof(float) * 3;
		dx12r.have_modvols = modVolBytes != 0;
		return dx12r_upload_constants();
	}
	/* the vertices, the indexes, the volumes' triangles, each at a multiple
	 * of its size, in one piece of the ring */
	indexes = (vertexBytes + 3) & ~3u;
	modVols = (indexes + indexBytes + 11) / 12 * 12;
	total = modVols + modVolBytes;
	if (!dx12c_upload(total, 64, &upload))
	{
		WARN_LOG(RENDERER, "DX12: the render's geometry does not fit the upload ring (%u bytes)\n", total);
		/* (as it was before: the vertices made for the card, once) */
		dx12r_set_provoking_vertices();
		return false;
	}
	/* sorted from the vertices as the game sent them, then those made for the card */
	sort_triangles((u32 *)(upload.cpu + indexes + idxBytes));
	dx12r_set_provoking_vertices();
	memcpy(upload.cpu, TA_LIST_HEAD(pvrrc.verts, Vertex), vertexBytes);
	memcpy(upload.cpu + indexes, TA_LIST_HEAD(pvrrc.idx, u32), idxBytes);
	if (modVolBytes != 0)
		memcpy(upload.cpu + modVols, modVolHead, modVolBytes);

	dx12r.vertex_view.BufferLocation = upload.gpu;
	dx12r.vertex_view.SizeInBytes = vertexBytes;
	dx12r.vertex_view.StrideInBytes = sizeof(Vertex);
	dx12r.index_view.BufferLocation = upload.gpu + indexes;
	dx12r.index_view.SizeInBytes = idxBytes + dx12r.sorted_index_count * sizeof(u32);
	dx12r.index_view.Format = DXGI_FORMAT_R32_UINT;
	dx12r.modvol_view.BufferLocation = upload.gpu + modVols;
	dx12r.modvol_view.SizeInBytes = modVolBytes;
	dx12r.modvol_view.StrideInBytes = sizeof(float) * 3;
	dx12r.have_modvols = modVolBytes != 0;
	return dx12r_upload_constants();
}

/* What every polygon of the frame is drawn with */
static void setup_frame(void)
{
	ID3D12GraphicsCommandList *cmd;
	dx12c_bind_raster();
	cmd = dx12c_cmd();
	ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cmd, 1, dx12r.vtx_constants_gpu);
	ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cmd, 2, dx12r.frag_constants_gpu);
	dx12c_set_vertex_buffer(&dx12r.vertex_view);
	dx12c_set_index_buffer(&dx12r.index_view);
}

bool dx12r_ensure_depth(u32 width, u32 height)
{
	ID3D12Device *device = dx12r.device;
	D3D12_CLEAR_VALUE clear;
	D3D12_DEPTH_STENCIL_VIEW_DESC view;
	u32 dsv;
	if (dx12r.depth.resource != NULL && dx12r.depth.width >= width && dx12r.depth.height >= height)
		return true;
	/* (SSAA's is made at the screen's size, and with it: setup_ssaa()) */
	if (dx12r.ssaa_depth_in)
		return false;
	width = MAX_U(width, dx12r.depth.width);
	height = MAX_U(height, dx12r.depth.height);
	/* (the one there was goes once the GPU is done with it; its views' places are used again) */
	dsv = dx12r.depth.rtv;
	let_go(&dx12r.depth.resource);
	dx12_image_reset(&dx12r.depth);
	memset(&clear, 0, sizeof(clear));
	clear.Format = DX12DepthFormat;
	clear.DepthStencil.Depth = 0.f;
	clear.DepthStencil.Stencil = 0;
	dx12r.depth.state = dx12r.stencil_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	if (!dx12_make_texture(device, width, height, 1, DX12DepthResourceFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, dx12r.depth.state,
			&dx12r.depth.resource, &clear))
	{
		WARN_LOG(RENDERER, "DX12: depth buffer creation failed (%d x %d)\n", width, height);
		return false;
	}
	if (dsv == ~0u)
	{
		dsv = dx12_heap_alloc(&dx12c.dsv_heap);
		dx12r.dsv_read_stencil = dx12_heap_alloc(&dx12c.dsv_heap);
		dx12r.dsv_read_only = dx12_heap_alloc(&dx12c.dsv_heap);
	}
	if (dsv == ~0u || dx12r.dsv_read_stencil == ~0u || dx12r.dsv_read_only == ~0u)
	{
		dx12_image_reset(&dx12r.depth);
		return false;
	}
	memset(&view, 0, sizeof(view));
	view.Format = DX12DepthFormat;
	view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	ID3D12Device_CreateDepthStencilView(device, dx12r.depth.resource, &view, dx12_heap_cpu(&dx12c.dsv_heap, dsv));
	view.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
	ID3D12Device_CreateDepthStencilView(device, dx12r.depth.resource, &view, dx12_heap_cpu(&dx12c.dsv_heap, dx12r.dsv_read_stencil));
	view.Flags = (D3D12_DSV_FLAGS)(D3D12_DSV_FLAG_READ_ONLY_STENCIL | D3D12_DSV_FLAG_READ_ONLY_DEPTH);
	ID3D12Device_CreateDepthStencilView(device, dx12r.depth.resource, &view, dx12_heap_cpu(&dx12c.dsv_heap, dx12r.dsv_read_only));
	dx12r.depth.rtv = dsv;
	dx12r.depth.width = width;
	dx12r.depth.height = height;
	/* (the view is read when a command is recorded: what is bound is bound to the old one) */
	dx12c_forget_targets();
	return true;
}

void dx12r_depth_states(D3D12_RESOURCE_STATES depthPlane, D3D12_RESOURCE_STATES stencilPlane)
{
	/* (the planes are the subresources: the depth 0, the stencil 1) */
	dx12c_transition_resource(dx12r.depth.resource, &dx12r.depth.state, depthPlane, 0);
	dx12c_transition_resource(dx12r.depth.resource, &dx12r.stencil_state, stencilPlane, 1);
}

/* ---- The screen ---- */

static void clear_target(const dx12_image *image, const float color[4])
{
	ID3D12GraphicsCommandList_ClearRenderTargetView(dx12c_cmd(), dx12_heap_cpu(&dx12c.rtv_heap, image->rtv), color, 0, NULL);
}

static void clear_depth(void)
{
	ID3D12GraphicsCommandList_ClearDepthStencilView(dx12c_cmd(), dx12_heap_cpu(&dx12c.dsv_heap, dx12r.depth.rtv),
			(D3D12_CLEAR_FLAGS)(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL), 0.f, 0, 0, NULL);
}

static void set_whole_scissor(u32 width, u32 height)
{
	D3D12_RECT whole;
	set_rect(&whole, 0, 0, (LONG)width, (LONG)height);
	dx12c_set_scissor(&whole);
}

static bool make_screen_target(dx12_image *target, u32 width, u32 height)
{
	static const float black[4] = { 0.f, 0.f, 0.f, 1.f };
	if (!make_image(target, width, height, DX12FrameFormat, true, ~0u, dx12r.storage_pictures || dx12r.writeout_pictures))
	{
		dx12_image_reset(target);
		return false;
	}
	clear_target(target, black);
	return true;
}

bool dx12r_ensure_screen(u32 width, u32 height)
{
	const unsigned count = MIN_U(dx12c_present_count(), DX12_SCREEN_SLOTS);
	dx12_image *target;
	u32 i;

	if (width != dx12r.screen_width || height != dx12r.screen_height)
	{
		/* Another size. The picture with the last frame goes on as the
		 * picture the next render starts from, at whatever size the screen
		 * now is (Vulkan's carried picture): nothing is read back or
		 * loaded again. */
		drop_restored();
		if (dx12r.have_picture && dx12r.last_in_extra)
		{
			dx12r.restored_picture = dx12r.extra;
			dx12_image_init(&dx12r.extra);
		}
		else if (dx12r.have_picture && dx12r.last_screen >= 0 && (u32)dx12r.last_screen < dx12r.screen_count)
		{
			dx12r.restored_picture = dx12r.screens[dx12r.last_screen];
			dx12_image_init(&dx12r.screens[dx12r.last_screen]);
		}
		for (i = 0; i < dx12r.screen_count; i++)
			dx12r_drop_image(&dx12r.screens[i]);
		dx12r.screen_count = 0;
		dx12r_drop_image(&dx12r.extra);
		dx12r.have_picture = false;
		dx12r.last_in_extra = false;
		dx12r.last_screen = -1;
		dx12r.screen_width = width;
		dx12r.screen_height = height;
		/* (the upscaler's are the screen's size: made again, and it starts over) */
		if (dx12r.upscaler_slot_count != 0)
		{
			drop_upscaler();
			upscaler_reset();
		}
	}
	if (dx12r.screen_count > count)
	{
		if (dx12r.last_screen >= (int)count && !dx12r.last_in_extra)
		{
			drop_restored();
			if (dx12r.have_picture)
			{
				dx12r.restored_picture = dx12r.screens[dx12r.last_screen];
				dx12_image_init(&dx12r.screens[dx12r.last_screen]);
			}
			dx12r.have_picture = false;
			dx12r.last_screen = -1;
		}
		for (i = count; i < dx12r.screen_count; i++)
			dx12r_drop_image(&dx12r.screens[i]);
		dx12r.screen_count = count;
	}
	while (dx12r.screen_count < count)
		dx12_image_init(&dx12r.screens[dx12r.screen_count++]);
	target = &dx12r.screens[dx12r.screen_index];
	if (target->resource == NULL && !make_screen_target(target, width, height))
		return false;
	if (dx12c_is_handed_over(target))
	{
		/* A render to the screen after one this frame has handed over
		 * already: this one goes to the extra picture, which takes the
		 * place of the one handed over, and that stays as it is until the
		 * frontend has shown it. (The last picture goes where its picture
		 * goes.) */
		dx12_image t;
		if (dx12r.extra.resource == NULL && !make_screen_target(&dx12r.extra, width, height))
			return false;
		t = *target;
		*target = dx12r.extra;
		dx12r.extra = t;
		if (dx12r.last_in_extra)
		{
			dx12r.last_in_extra = false;
			dx12r.last_screen = dx12r.screen_index;
		}
		else if (dx12r.last_screen == dx12r.screen_index)
			dx12r.last_in_extra = true;
	}
	return dx12r_ensure_depth(width, height);
}

bool dx12r_keeps_last_picture(void)
{
	return !pvrrc.clearFramebuffer && transform_matrix_clipped(&dx12r.matrices)
			&& (dx12r.have_picture || dx12r.restored_picture.resource != NULL);
}

dx12_image *dx12r_last_picture_image(void)
{
	return dx12r.have_picture ? (dx12r.last_in_extra ? &dx12r.extra : &dx12r.screens[dx12r.last_screen])
			: dx12r.restored_picture.resource != NULL ? &dx12r.restored_picture : NULL;
}

/* The last picture drawn whole onto what is bound */
static void draw_last_picture(u32 width, u32 height)
{
	dx12_image *image = dx12r_last_picture_image();
	if (image == NULL)
		return;
	dx12c_set_viewport((float)width, (float)height);
	set_whole_scissor(width, height);
	/* (the picture kept from another context can be of another size) */
	dx12c_draw_quad_at(image->srv, !dx12r.have_picture, -1.f, -1.f, 2.f, 2.f, 0.f, 0.f, 1.f, 1.f, DX12_BLEND_KEY, NULL);
}

static void border_colour(float color[4])
{
	color[0] = VO_BORDER_COL.Red / 255.f;
	color[1] = VO_BORDER_COL.Green / 255.f;
	color[2] = VO_BORDER_COL.Blue / 255.f;
}

/* The screen's frame with SSAA: its samples cleared, or, under a render
 * that covers only part of the screen, the last picture drawn into them
 * (each of a pixel's samples its pixel's colour, which resolves to it) */
static void begin_screen_ssaa(dx12_image *target)
{
	dx12_image *samples = &dx12r.ssaa_color[0];
	float color[4] = { 0.f, 0.f, 0.f, 1.f };
	const bool keep = dx12r_keeps_last_picture();
	dx12_image *last;
	dx12c_transition(samples, D3D12_RESOURCE_STATE_RENDER_TARGET);
	dx12c_set_targets(samples->rtv, dx12r.depth.rtv, dx12r.ssaa_log2);
	if (!keep && pvrrc.clearFramebuffer)
		border_colour(color);
	clear_target(samples, color);
	last = keep ? dx12r_last_picture_image() : NULL;
	if (last != NULL)
	{
		/* Drawn from where it is when it can be read there; a picture the
		 * frontend has (version 2), left in the state it was handed over
		 * in, is copied into this frame's picture first, which the resolve
		 * writes all of later */
		const bool handedOver = dx12c_is_handed_over(last);
		dx12_image *from = NULL;
		if (!handedOver || last->state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
			from = last;
		else if (dx12r.have_picture && last->state == D3D12_RESOURCE_STATE_COPY_SOURCE && last->width == target->width
				&& last->height == target->height)
		{
			dx12c_transition(target, D3D12_RESOURCE_STATE_COPY_DEST);
			ID3D12GraphicsCommandList_CopyResource(dx12c_cmd(), target->resource, last->resource);
			from = target;
		}
		if (from != NULL)
		{
			dx12c_transition(from, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dx12c_set_viewport((float)dx12r.screen_width, (float)dx12r.screen_height);
			set_whole_scissor(dx12r.screen_width, dx12r.screen_height);
			dx12c_draw_quad_at(from->srv, !dx12r.have_picture, -1.f, -1.f, 2.f, 2.f, 0.f, 0.f, 1.f, 1.f, DX12_BLEND_KEY, NULL);
		}
	}
	clear_depth();
	dx12c_set_viewport((float)dx12r.screen_width, (float)dx12r.screen_height);
	dx12r_set_base_scissor();
}

static bool begin_upscaler(void)
{
	return dx12r_upscaler_jitter() && dx12r_upscaler_match();
}

static bool begin_screen(void)
{
	dx12_image *target;
	dx12r.screen_index = (int)dx12c_present_index();
	/* (SSAA: drawn into the samples, with their depth, when they are the screen's size) */
	dx12r_ssaa_depth(dx12r.ssaa_samples > 1 && dx12r.ssaa_width == (u32)screen_width && dx12r.ssaa_height == (u32)screen_height);
	if (!dx12r_ensure_screen((u32)screen_width, (u32)screen_height))
	{
		dx12r_ssaa_depth(false);
		return false;
	}
	target = &dx12r.screens[dx12r.screen_index];
	dx12r.target_rtv = target->rtv;
	dx12r.target_width = dx12r.screen_width;
	dx12r.target_height = dx12r.screen_height;

	transform_matrix_calc(&dx12r.matrices, &pvrrc);
	/* the frame drawn under this frame's jitter, its strips matched for the motion vector pass */
	begin_upscaler();
	if (!dx12r.kind->binds_screen)
	{
		/* (the per-pixel renderer clears and binds its own pictures, and this one at the end) */
		dx12c_transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
		dx12r_set_base_scissor();
		return true;
	}
	if (dx12r_ssaa_active())
	{
		begin_screen_ssaa(target);
		return true;
	}

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour; any other starts black - unless the render covers only part
	 * of the screen, which leaves the rest as it was: the last picture,
	 * which is in this picture already if the last frame went to it.
	 * (Only for such a render: one that covers the screen pays nothing.) */
	if (dx12r_keeps_last_picture() && dx12r_last_picture_image() != target)
	{
		dx12_image *last = dx12r_last_picture_image();
		/* Copied, where it is the same size and can be read: a picture the
		 * frontend has (version 2) is left in the state it was handed over
		 * in, which is one to copy from with the frontends there are;
		 * drawn otherwise. */
		const bool handedOver = dx12c_is_handed_over(last);
		if (dx12r.have_picture && (!handedOver || last->state == D3D12_RESOURCE_STATE_COPY_SOURCE))
		{
			dx12c_transition(last, D3D12_RESOURCE_STATE_COPY_SOURCE);
			dx12c_transition(target, D3D12_RESOURCE_STATE_COPY_DEST);
			ID3D12GraphicsCommandList_CopyResource(dx12c_cmd(), target->resource, last->resource);
			dx12c_transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
			dx12c_set_targets(target->rtv, dx12r.depth.rtv, 0);
		}
		else
		{
			static const float black[4] = { 0.f, 0.f, 0.f, 1.f };
			dx12c_transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
			dx12c_set_targets(target->rtv, dx12r.depth.rtv, 0);
			clear_target(target, black);
			if (!handedOver || last->state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
			{
				dx12c_transition(last, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				draw_last_picture(dx12r.screen_width, dx12r.screen_height);
			}
		}
	}
	else
	{
		dx12c_transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
		dx12c_set_targets(target->rtv, dx12r.depth.rtv, 0);
		if (!dx12r_keeps_last_picture())
		{
			float color[4] = { 0.f, 0.f, 0.f, 1.f };
			if (pvrrc.clearFramebuffer)
				border_colour(color);
			clear_target(target, color);
		}
	}
	clear_depth();
	dx12c_set_viewport((float)dx12r.screen_width, (float)dx12r.screen_height);
	dx12r_set_base_scissor();
	return true;
}

static void record_motion_pass(void);
static void submit_upscaler(bool hdr);

void dx12r_end_screen(bool writeOutPass)
{
	dx12_image *target = &dx12r.screens[dx12r.screen_index];
	const bool ssaa = dx12r_ssaa_active();
	const bool writes = writeOutPass && dx12r.writeout_pictures;
	/* the HDR output takes the samples' exact average where the picture is
	 * their resolve (not on a frame the chip's write-out changes after,
	 * nor where the per-pixel renderer resolved them itself): kept till then */
	const bool hdrSamples = ssaa && dx12r.kind->binds_screen && dx12c_hdr_active() && !writes;
	bool hdr;
	u32 i;
	dx12r_flush_batch();
	if (ssaa)
	{
		/* The samples resolved into the picture, all of it. Their contents
		 * are not wanted after that: discarded, which a tiler need not
		 * store (the next frame clears them). */
		dx12r_depth_states(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		ID3D12GraphicsCommandList_DiscardResource(dx12c_cmd(), dx12r.depth.resource, NULL);
		dx12r_ssaa_depth(false);
		/* (the per-pixel renderer resolves them itself, as it puts its lists together) */
		if (dx12r.kind->binds_screen)
		{
			dx12c_transition(&dx12r.ssaa_color[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			dx12c_transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
			dx12c_set_targets(target->rtv, ~0u, 0);
			dx12c_set_viewport((float)dx12r.screen_width, (float)dx12r.screen_height);
			set_whole_scissor(dx12r.screen_width, dx12r.screen_height);
			ssaa_resolve(dx12r.ssaa_color[0].srv);
		}
		for (i = hdrSamples ? 1 : 0; i < dx12r.kind->ssaa_color_images && i < 2; i++)
		{
			dx12c_transition(&dx12r.ssaa_color[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
			ID3D12GraphicsCommandList_DiscardResource(dx12c_cmd(), dx12r.ssaa_color[i].resource, NULL);
		}
	}
	if (writes)
	{
		dx12c_set_targets(~0u, ~0u, 0);
		dx12_writeout_record(target);
		if (!dx12_overlay_empty())
			dx12c_transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}
	/* what goes over the picture, onto it (with the depth test off: whatever depth is bound is not looked at) */
	if (!dx12_overlay_empty())
	{
		/* (with SSAA, none: the depth of one sample may be smaller than the screen) */
		dx12c_set_targets(target->rtv, ssaa ? ~0u : dx12r.depth.rtv, 0);
		dx12c_set_viewport((float)dx12r.screen_width, (float)dx12r.screen_height);
		set_whole_scissor(dx12r.screen_width, dx12r.screen_height);
		dx12_overlay_draw(dx12r.screen_width, dx12r.screen_height);
	}
	/* the motion vectors over the depth the frame left, and the depth, for the upscaler */
	if (dx12r.upscaled)
		record_motion_pass();
	dx12c_set_targets(~0u, ~0u, 0);
	/* as the frontend wants it - or encoded for its HDR output, which it is handed instead - and off to the GPU */
	hdr = dx12c_encode_hdr(target, target->width, target->height, hdrSamples ? &dx12r.ssaa_color[0] : NULL, dx12r.ssaa_log2,
			!writes && dx12r.kind->hdr_sums != NULL ? dx12r.kind->hdr_sums() : NULL);
	if (!hdr)
		dx12c_transition(target, dx12c.required_state);
	if (hdrSamples)
	{
		dx12c_transition(&dx12r.ssaa_color[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
		ID3D12GraphicsCommandList_DiscardResource(dx12c_cmd(), dx12r.ssaa_color[0].resource, NULL);
	}
	dx12c_submit();
	/* (before the picture: the frontend takes them with it) */
	if (dx12r.upscaled)
		submit_upscaler(hdr);

	dx12r.have_picture = true;
	dx12r.last_screen = dx12r.screen_index;
	dx12r.last_in_extra = false;
	drop_restored();
	dx12c_set_frame(target);
}

void dx12r_keep_picture(void)
{
	dx12_image *image;
	u8 *pixels;
	if (!dx12r.have_picture || dx12r.last_screen < 0 || (u32)dx12r.last_screen >= dx12r.screen_count || dx12r.device == NULL)
		return;
	image = dx12r_last_picture_image();
	pixels = last_picture_keep(dx12r.screen_width, dx12r.screen_height);
	if (pixels != NULL && (image == NULL || !dx12_read_picture(image->resource, &image->state, dx12r.screen_width, dx12r.screen_height, pixels)))
		last_picture_drop();
}

/* ---- The temporal upscaler ---- */

/* The pass's pipelines made while the upscaler has the frames: at init and
 * resize, not in a frame; and its slots let go of when it has not */
static void setup_upscaler(void)
{
	DX12PsoKey key;
	u32 cull;
	if (!upscaler_active() || dx12r.ssaa_samples > 1)
	{
		drop_upscaler();
		return;
	}
	/* the pass's pipelines, for each cull mode, over the depth buffer and
	 * over the compute renderer's depth, and the depth's copy: now, and not
	 * when a frame first wants one */
	memset(&key, 0, sizeof(key));
	key.kind = DX12_PSO_MOTION;
	key.writeColor = true;
	key.pass = 1;
	key.depthFunc = 7;
	dx12_pso_prepare(&key);
	key.pass = 0;
	key.depthFunc = 0;
	for (cull = 0; cull < 3; cull++)
	{
		key.cull = cull;
		key.depthBound = true;
		key.depthTest = true;
		key.depthFunc = 6;
		dx12_pso_prepare(&key);
		key.depthBound = false;
		key.depthTest = false;
		key.depthFunc = 0;
		dx12_pso_prepare(&key);
	}
}

static void drop_upscaler(void)
{
	u32 i;
	for (i = 0; i < dx12r.upscaler_slot_count; i++)
	{
		dx12r_drop_image(&dx12r.upscaler_slots[i].motion);
		dx12r_drop_image(&dx12r.upscaler_slots[i].depth);
	}
	dx12r.upscaler_slot_count = 0;
	if (dx12r.depth_copy_srv != ~0u)
		dx12c_release_srv(dx12r.depth_copy_srv);
	dx12r.depth_copy_srv = ~0u;
	dx12r.depth_copy_viewed = NULL;
	dx12r.upscaled = false;
}

bool dx12r_upscaler_jitter(void)
{
	dx12_upscaler_slot *slot;
	float jx, jy;
	dx12r.upscaled = false;
	dx12r.motion_sampled = false;
	/* (a frame decoded on the GPU: nothing of it to match) */
	if (!upscaler_active() || dx12r_ssaa_active() || pvrrc.gpu || dx12r.screen_width == 0 || dx12r.screen_height == 0
			|| dx12r.screen_index >= DX12_SCREEN_SLOTS)
		return false;
	while (dx12r.upscaler_slot_count <= (u32)dx12r.screen_index)
	{
		dx12_image_init(&dx12r.upscaler_slots[dx12r.upscaler_slot_count].motion);
		dx12_image_init(&dx12r.upscaler_slots[dx12r.upscaler_slot_count].depth);
		dx12r.upscaler_slot_count++;
	}
	slot = &dx12r.upscaler_slots[dx12r.screen_index];
	/* (the depth: drawn into, or stored into by the compute renderer's walk) */
	if (slot->motion.resource == NULL && (!make_image(&slot->motion, dx12r.screen_width, dx12r.screen_height, DX12MotionFormat, true, ~0u, false)
			|| !make_image(&slot->depth, dx12r.screen_width, dx12r.screen_height, DX12UpscalerDepthFormat, true, ~0u, true)))
	{
		WARN_LOG(RENDERER, "DX12: the upscaler's pictures could not be made (%d x %d)\n", dx12r.screen_width, dx12r.screen_height);
		dx12r_drop_image(&slot->motion);
		dx12r_drop_image(&slot->depth);
		return false;
	}
	upscaler_jitter(&jx, &jy);
	transform_matrix_jitter(&dx12r.matrices, jx, jy);
	return true;
}

bool dx12r_upscaler_match(void)
{
	/* the frame's strips matched with the frame before's: where each vertex was, in the ring */
	const size_t used = (size_t)TA_LIST_USED(pvrrc.verts, Vertex) * 2 * sizeof(float);
	const u32 bytes = (u32)(used > 2 * sizeof(float) ? used : 2 * sizeof(float));
	dx12_upload upload;
	if (!dx12c_upload(bytes, 16, &upload))
		return false;
	/* (written where the GPU reads them: the upload ring) */
	upscaler_match(&pvrrc, (float *)upload.cpu);
	dx12r.prev_xy_view.BufferLocation = upload.gpu;
	dx12r.prev_xy_view.SizeInBytes = bytes;
	dx12r.prev_xy_view.StrideInBytes = 2 * sizeof(float);
	dx12r.upscaled = true;
	return true;
}

static int cull_of(u32 isp)
{
	const u32 mode = PPW(isp, ISP_TSP_CullMode);
	return mode == 3 ? 2 : mode == 2 ? 1 : 0;
}

static void record_motion_pass(void)
{
	static const float still[4] = { 0.f, 0.f, 0.f, 0.f };
	ID3D12GraphicsCommandList *cmd = dx12c_cmd();
	dx12_upscaler_slot *slot = &dx12r.upscaler_slots[dx12r.screen_index];
	const PolyParam *lists[2];
	int counts[2];
	float size[2];
	u32 constants[3];
	DX12PsoKey key;
	ID3D12PipelineState *pso;
	int bound = -1, l, i;
	dx12c_transition(&slot->motion, D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (dx12r.motion_sampled)
	{
		/* the vectors over the depth the walk stored, a texture the shader tests */
		dx12c_transition(&slot->depth, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		dx12c_set_targets(slot->motion.rtv, ~0u, 0);
	}
	else
	{
		/* the vectors drawn over the depth the frame left: tested, not written */
		dx12r_depth_states(D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_READ);
		dx12c_set_targets(slot->motion.rtv, dx12r.dsv_read_only, 0);
	}
	ID3D12GraphicsCommandList_ClearRenderTargetView(cmd, dx12_heap_cpu(&dx12c.rtv_heap, slot->motion.rtv), still, 0, NULL);
	dx12c_set_viewport((float)dx12r.screen_width, (float)dx12r.screen_height);
	dx12c_set_scissor(&dx12r.base_scissor);
	dx12c_bind_raster();
	ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cmd, 1, dx12r.vtx_constants_gpu);
	/* (the vectors in pixels: the size, as the shader's constants; and the depth's place, where the shader tests it) */
	size[0] = (float)dx12r.screen_width;
	size[1] = (float)dx12r.screen_height;
	memcpy(constants, size, sizeof(size));
	constants[2] = slot->depth.srv;
	dx12c_set_root_constants(constants, 3);
	dx12c_set_vertex_buffer(dx12r.motion_sampled ? &dx12r.motion_vertex_view : &dx12r.vertex_view);
	ID3D12GraphicsCommandList_IASetVertexBuffers(cmd, 1, 1, &dx12r.prev_xy_view);
	dx12c_set_index_buffer(dx12r.motion_sampled ? &dx12r.motion_index_view : &dx12r.index_view);
	dx12c_set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	memset(&key, 0, sizeof(key));
	key.kind = DX12_PSO_MOTION;
	key.writeColor = true;
	key.depthBound = !dx12r.motion_sampled;
	key.depthTest = !dx12r.motion_sampled;
	key.depthFunc = dx12r.motion_sampled ? 0 : 6;
	lists[0] = TA_LIST_HEAD(pvrrc.global_param_op, PolyParam);
	counts[0] = TA_LIST_USED(pvrrc.global_param_op, PolyParam);
	lists[1] = TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam);
	counts[1] = TA_LIST_USED(pvrrc.global_param_pt, PolyParam);
	for (l = 0; l < 2; l++)
		for (i = 0; i < counts[l]; i++)
		{
			const PolyParam *pp = &lists[l][i];
			int cull;
			if (pp->count < 3)
				continue;
			cull = cull_of(pp->isp);
			if (cull != bound)
			{
				key.cull = (u32)cull;
				pso = dx12_pso_get(&key);
				if (pso == NULL)
					break;
				dx12c_set_pipeline(pso);
				bound = cull;
			}
			ID3D12GraphicsCommandList_DrawIndexedInstanced(cmd, pp->count, 1, pp->first, 0, 0);
		}
	/* (the next frame's draws bind the one stream they have: the pass's second is theirs to ignore) */
	if (dx12r.motion_sampled)
	{
		dx12c_transition(&slot->motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		dx12c_set_targets(~0u, ~0u, 0);
		return;
	}
	/* the depth buffer's first plane drawn whole into the slot's picture of
	 * floats, for the upscaler to read as it likes (a copy between the two
	 * formats is not one every Direct3D 12 there is takes) */
	if (dx12r.depth_copy_viewed != dx12r.depth.resource)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC view;
		if (dx12r.depth_copy_srv != ~0u)
			dx12c_release_srv(dx12r.depth_copy_srv);
		dx12r.depth_copy_srv = dx12_heap_alloc(&dx12c.srv_heap);
		if (dx12r.depth_copy_srv == ~0u)
		{
			dx12r.upscaled = false;
			dx12c_transition(&slot->motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			dx12r_depth_states(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			dx12c_set_targets(~0u, ~0u, 0);
			return;
		}
		memset(&view, 0, sizeof(view));
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Texture2D.MipLevels = 1;
		view.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		ID3D12Device_CreateShaderResourceView(dx12r.device, dx12r.depth.resource, &view, dx12_heap_cpu(&dx12c.srv_heap, dx12r.depth_copy_srv));
		dx12r.depth_copy_viewed = dx12r.depth.resource;
	}
	dx12r_depth_states((D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), D3D12_RESOURCE_STATE_DEPTH_READ);
	dx12c_transition(&slot->depth, D3D12_RESOURCE_STATE_RENDER_TARGET);
	dx12c_set_targets(slot->depth.rtv, ~0u, 0);
	set_whole_scissor(dx12r.screen_width, dx12r.screen_height);
	memset(&key, 0, sizeof(key));
	key.kind = DX12_PSO_MOTION;
	key.pass = 1;
	key.writeColor = true;
	key.depthFunc = 7;
	pso = dx12_pso_get(&key);
	if (pso != NULL)
	{
		/* (the quad's constants: the whole target, the plane's place in the table) */
		static const float rect[8] = { -1.f, -1.f, 2.f, 2.f, 0.f, 0.f, 1.f, 1.f };
		u32 quad[10];
		dx12c_set_pipeline(pso);
		memcpy(quad, rect, sizeof(rect));
		quad[8] = dx12r.depth_copy_srv;
		quad[9] = 0;
		dx12c_set_root_constants(quad, 10);
		ID3D12GraphicsCommandList_DrawInstanced(cmd, 4, 1, 0, 0);
	}
	dx12c_transition(&slot->depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	dx12c_transition(&slot->motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	dx12r_depth_states(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	dx12c_set_targets(~0u, ~0u, 0);
}

static void submit_upscaler(bool hdr)
{
	const dx12_upscaler_slot *slot = &dx12r.upscaler_slots[dx12r.screen_index];
	struct retro_hw_upscaler_image motion, depth;
	motion.image  = (uint64_t)(uintptr_t)slot->motion.resource;
	motion.view   = 0;
	motion.format = (unsigned)DX12MotionFormat;
	motion.state  = (unsigned)slot->motion.state;
	motion.width  = dx12r.screen_width;
	motion.height = dx12r.screen_height;
	depth.image   = (uint64_t)(uintptr_t)slot->depth.resource;
	depth.view    = 0;
	depth.format  = (unsigned)DX12UpscalerDepthFormat;
	depth.state   = (unsigned)slot->depth.state;
	depth.width   = dx12r.screen_width;
	depth.height  = dx12r.screen_height;
	/* (the depth is 1/w: nearer is greater) */
	upscaler_submit(&motion, &depth, RETRO_HW_UPSCALER_DEPTH_INVERTED | (hdr ? RETRO_HW_UPSCALER_HDR : 0));
	dx12r.upscaled = false;
}

/* ---- Renders to textures ---- */

void dx12r_rtt_size(u32 *widthPow2, u32 *heightPow2)
{
	const u32 origWidth = pvrrc.fb_X_CLIP.max + 1;
	const u32 origHeight = pvrrc.fb_Y_CLIP.max + 1;
	const u32 scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	*heightPow2 = 8;
	while (*heightPow2 < origHeight)
		*heightPow2 *= 2;
	*widthPow2 = 8;
	while (*widthPow2 < origWidth)
		*widthPow2 *= 2;
	*widthPow2 *= scale;
	*heightPow2 *= scale;
}

bool dx12r_begin_rtt(void)
{
	u32 origWidth, origHeight, clipX, clipY, clipWidth, clipHeight, scale, upscaledWidth, upscaledHeight;
	u32 widthPow2, heightPow2, tcw, tsp = 0, u, v;
	dx12_pooled_texture *spare;
	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d @ %08x\n", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max, FB_W_SOF1 & VRAM_MASK);
	transform_matrix_calc(&dx12r.matrices, &pvrrc);
	/* (one sample a pixel, SSAA or not) */
	dx12r_ssaa_depth(false);

	dx12r.rtt_addr = FB_W_SOF1 & VRAM_MASK;
	/* the frame from its first pixel, as far as the pixel clip goes (see rend/transform_matrix.h) */
	origWidth = pvrrc.fb_X_CLIP.max + 1;
	origHeight = pvrrc.fb_Y_CLIP.max + 1;
	clipX = pvrrc.fb_X_CLIP.min;
	clipY = pvrrc.fb_Y_CLIP.min;
	clipWidth = origWidth - clipX;
	clipHeight = origHeight - clipY;
	scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	upscaledWidth = origWidth * scale;
	upscaledHeight = origHeight * scale;
	dx12r_rtt_size(&widthPow2, &heightPow2);

	if (!dx12r_ensure_depth(widthPow2, heightPow2))
		return false;

	/* what an earlier render left waiting in this memory, while its picture is still what it was: see rtt_watch.h */
	dx12_rtt_supersede(dx12r.rtt_addr, clipX, clipY, clipWidth, clipHeight);
	/* TexAddr : fb_rtt.TexAddr, Reserved : 0, StrideSel : 0, ScanOrder : 1 */
	tcw = TAW_SET(TAW_SET(0, TCW_TexAddr, dx12r.rtt_addr >> 3), TCW_ScanOrder, 1);
	switch (FB_W_CTRL.fb_packmode)
	{
	case 0:
	case 3:
		tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_1555);
		break;
	case 1:
		tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_565);
		break;
	case 2:
		tcw = TAW_SET(tcw, TCW_PixelFmt, TA_PIXEL_4444);
		break;
	}
	for (u = 0; u <= 7 && (8u << u) < origWidth; u++)
		;
	for (v = 0; v <= 7 && (8u << v) < origHeight; v++)
		;
	tsp = TAW_SET(TAW_SET(tsp, TSP_TexU, u), TSP_TexV, v);

	dx12r.rtt_texture = dx12_texcache_get(tsp, tcw, 0);
	if (dx12r.rtt_texture->pooled == NULL)
		texcache_create(&dx12r.rtt_texture->base);

	spare = dx12r.rtt_texture->spare;
	if (spare != NULL && (spare->width != widthPow2 || spare->height != heightPow2 || spare->rtv == ~0u))
	{
		dx12_texture_release_spare(dx12r.rtt_texture);
		spare = NULL;
	}
	/* (one of the pool's, made at the start, where it has one of this shape) */
	if (spare == NULL && !dx12_texture_take_spare(dx12r.rtt_texture, widthPow2, heightPow2))
	{
		dx12_pooled_texture *own = dx12_pool_make_own(DX12_POOL_TARGET, widthPow2, heightPow2, 1);
		if (own == NULL)
			return false;
		dx12_texture_set_spare(dx12r.rtt_texture, own);
	}
	spare = dx12r.rtt_texture->spare;

	dx12r.target_rtv = spare->rtv;
	dx12r.target_width = upscaledWidth;
	dx12r.target_height = upscaledHeight;

	dx12c_transition_resource(spare->resource, &spare->state, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	if (dx12r.kind->binds_screen)
	{
		static const float black[4] = { 0.f, 0.f, 0.f, 1.f };
		dx12c_set_targets(spare->rtv, dx12r.depth.rtv, 0);
		ID3D12GraphicsCommandList_ClearRenderTargetView(dx12c_cmd(), dx12_heap_cpu(&dx12c.rtv_heap, spare->rtv), black, 0, NULL);
		clear_depth();
	}
	dx12c_set_viewport((float)upscaledWidth, (float)upscaledHeight);
	/* only the pixel clip is written */
	set_rect(&dx12r.base_scissor, (LONG)(clipX * scale), (LONG)(clipY * scale),
			(LONG)((clipX + clipWidth) * scale), (LONG)((clipY + clipHeight) * scale));
	return true;
}

void dx12r_end_rtt(void)
{
	dx12_texture *rtt = dx12r.rtt_texture;
	dx12_pooled_texture *spare;
	dx12r_flush_batch();
	dx12c_set_targets(~0u, ~0u, 0);
	spare = rtt->spare;
	/* to be drawn with, and off to the GPU */
	dx12c_transition_resource(spare->resource, &spare->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	dx12c_submit();
	dx12_texture_swap_with_spare(rtt);

	rtt->base.dirty = 0;
	rtt->base.stride_read = 0;	/* (rendered, not read) */
	rtt->base.made_type = 0;
	libCore_vramlock_Lock(rtt->base.sa_tex, rtt->base.sa + rtt->base.size - 1, &rtt->base);
	/* video memory gets it when something first touches it there */
	dx12_rtt_watch(rtt, dx12r.rtt_addr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
	dx12r.rtt_texture = NULL;
}

/* ---- A frame the game wrote to the framebuffer itself ---- */

static bool upload_framebuffer(void)
{
	FramebufferRead fb;
	dx12_upload upload;
	u32 pitch;
	int width, height;
	u8 *out;
	if (FB_R_SIZE.fb_x_size == 0 || FB_R_SIZE.fb_y_size == 0)
		return false;

	ReadFramebufferGeometry(&fb);
	width = fb.width;
	height = fb.height;

	if (dx12r.fb_texture.resource == NULL || (u32)width != dx12r.fb_texture.width || (u32)height != dx12r.fb_texture.height)
	{
		/* (another size: seldom, and between frames) */
		dx12r_drop_image(&dx12r.fb_texture);
		if (!make_image(&dx12r.fb_texture, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, false, ~0u, false))
			return false;
	}
	/* converted where the copy to the texture reads it: the upload ring */
	out = reserve_image(&upload, width, height, 4, &pitch);
	if (out == NULL)
		return false;
	ReadFramebufferTo(&fb, out, pitch);
	record_image(&dx12r.fb_texture, &upload, width, height, pitch);
	return true;
}

/* Into the screen's picture, where the game's 640x480 is, as the OpenGL
 * renderers draw it */
static void draw_framebuffer(void)
{
	static const float black[4] = { 0.f, 0.f, 0.f, 1.f };
	dx12_image *target;
	const xform *viewport;
	float x0, x1, y0, y1;
	dx12r.screen_index = (int)dx12c_present_index();
	if (!dx12r_ensure_screen((u32)screen_width, (u32)screen_height))
		return;
	target = &dx12r.screens[dx12r.screen_index];
	transform_matrix_calc(&dx12r.matrices, &pvrrc);
	dx12c_transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
	dx12c_set_targets(target->rtv, ~0u, 0);
	clear_target(target, black);
	dx12c_set_viewport((float)dx12r.screen_width, (float)dx12r.screen_height);
	set_whole_scissor(dx12r.screen_width, dx12r.screen_height);

	viewport = &dx12r.matrices.viewport;
	x0 = xform_x(viewport, 0.f);
	x1 = xform_x(viewport, 640.f);
	y0 = xform_y(viewport, 0.f);
	y1 = xform_y(viewport, 480.f);
	dx12c_draw_quad_at(dx12r.fb_texture.srv, true, x0 * 2.f / dx12r.screen_width - 1.f, 1.f - y1 * 2.f / dx12r.screen_height,
			(x1 - x0) * 2.f / dx12r.screen_width, (y1 - y0) * 2.f / dx12r.screen_height, 0.f, 0.f, 1.f, 1.f, DX12_BLEND_KEY, NULL);
	dx12r_end_screen(false);
}

/* ---- The frame ---- */

/* The frame drawn, and its lists its own again: what the parse wrote where
 * the card reads it (the parse arena) the card's till the commands that
 * draw it are done */
bool dx12r_render(void)
{
	const bool rv = dx12r.kind->render_frame != NULL ? dx12r.kind->render_frame() : dx12r_render_frame();
	/* (the commands that draw it: submitted already, or in the list the next
	 * submission executes) */
	parse_arena_d3d12_drawn(&dx12r.parse_arena, dx12c.recording ? dx12c.fence_value + 1 : dx12c.fence_value);
	return rv;
}

bool dx12r_render_frame(void)
{
	const bool held = dx12r.held_from_process;
	dx12r.held_from_process = false;
	if (dx12r.device == NULL)
		return false;
	if (pvrrc.isRTT && (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min))
	{
		/* A pixel clip that ends before it starts holds no pixel: the
		 * chip writes nothing. (Its size would be negative here.) */
		WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d\n",
				pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
		return false;
	}
	/* the frontend may still be showing what is in the screen's next
	 * picture: it is waited for, if process() has not */
	if (!held && !pvrrc.isRTT)
		dx12c_wait_present_index();

	dx12r_ssaa_depth(false);
	if (pvrrc.isRenderFramebuffer)
	{
		draw_framebuffer();
		return true;
	}

	if (pvrrc.isRTT ? !dx12r_begin_rtt() : !begin_screen())
	{
		dx12r.rtt_texture = NULL;
		return false;
	}
	/* (sorted into the ring, and the provoking vertices set: upload_geometry()) */
	if (!upload_geometry())
	{
		dx12c_set_targets(~0u, ~0u, 0);
		dx12r_ssaa_depth(false);
		dx12r.rtt_texture = NULL;
		return false;
	}
	setup_frame();

	dx12r.kind->draw_frame();

	if (pvrrc.isRTT)
		dx12r_end_rtt();
	else
		dx12r_end_screen(true);

	return !pvrrc.isRTT;
}

/* ---- The draws, batched ---- */

void dx12r_draw(const dx12_draw_state *state, const u32 *constants, u32 indexCount, u32 firstIndex)
{
	dx12_draw_args *args;
	if (dx12r.batch_count != 0 && (dx12r.batch_count == DX12R_BATCH_MAX || state->pso != dx12r.batch_state.pso
			|| state->stencilRef != dx12r.batch_state.stencilRef || state->topology != dx12r.batch_state.topology
			|| memcmp(&state->scissor, &dx12r.batch_state.scissor, sizeof(state->scissor))))
		dx12r_flush_batch();
	if (dx12r.batch_count == 0)
		dx12r.batch_state = *state;
	args = &dx12r.batch[dx12r.batch_count++];
	memcpy(args->constants, constants, sizeof(args->constants));
	args->draw.IndexCountPerInstance = indexCount;
	args->draw.InstanceCount = 1;
	args->draw.StartIndexLocation = firstIndex;
	args->draw.BaseVertexLocation = 0;
	args->draw.StartInstanceLocation = 0;
}

void dx12r_flush_batch(void)
{
	const u32 count = dx12r.batch_count;
	ID3D12GraphicsCommandList *cmd;
	ID3D12CommandSignature *signature;
	dx12_upload upload;
	u32 i;
	if (count == 0)
		return;
	dx12r.batch_count = 0;
	if (dx12r.batch_state.pso == NULL)
		return;
	dx12c_set_pipeline(dx12r.batch_state.pso);
	dx12c_set_scissor(&dx12r.batch_state.scissor);
	dx12c_set_stencil_ref(dx12r.batch_state.stencilRef);
	dx12c_set_topology(dx12r.batch_state.topology);
	cmd = dx12c_cmd();
	dx12r.stat_batches++;
	dx12r.stat_batched += count;
	signature = dx12c.draw_signature;
	if (count > 1 && signature != NULL && dx12c_upload(count * sizeof(dx12_draw_args), 16, &upload))
	{
		memcpy(upload.cpu, dx12r.batch, (size_t)count * sizeof(dx12_draw_args));
		ID3D12GraphicsCommandList_ExecuteIndirect(cmd, signature, count, upload.buffer, upload.offset, NULL, 0);
		dx12c_forget_root_constants();
		return;
	}
	for (i = 0; i < count; i++)
	{
		const dx12_draw_args *args = &dx12r.batch[i];
		dx12c_set_root_constants(args->constants, DX12C_POLY_CONSTANT_COUNT);
		ID3D12GraphicsCommandList_DrawIndexedInstanced(cmd, args->draw.IndexCountPerInstance, 1, args->draw.StartIndexLocation, 0, 0);
	}
}

void dx12r_draw_gpu(const dx12_draw_state *state, const u32 *constants, u32 listType, bool shadow, u32 first, u32 count)
{
	const dx12_ta_draw *gpu;
	const u64 stride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	ID3D12GraphicsCommandList *cmd;
	dx12r_flush_batch();
	if (state->pso == NULL)
		return;
	dx12c_set_pipeline(state->pso);
	dx12c_set_scissor(&state->scissor);
	dx12c_set_stencil_ref(state->stencilRef);
	dx12c_set_topology(state->topology);
	dx12c_set_root_constants(constants, DX12C_POLY_CONSTANT_COUNT);
	gpu = dx12_ta_get_draw();
	cmd = dx12c_cmd();
	dx12r.stat_batches++;
	dx12r.stat_batched++;
	if (listType == ListType_Translucent)
		ID3D12GraphicsCommandList_ExecuteIndirect(cmd, gpu->drawIndexed, count, gpu->trDraws, (first + 1) * stride, gpu->trDraws, first * stride);
	else
		ID3D12GraphicsCommandList_ExecuteIndirect(cmd, gpu->drawIndexed, 1, gpu->commands, (shadow ? gpu->shadowCommands : 0) + first * stride, NULL, 0);
}

/* ---- The polygons, as the Direct3D 11 renderer draws them ---- */

/* A polygon's pipeline (draw_poly()), but for its cull mode and shading,
 * which its shaders apply, and SSAA's: by its list, whether it is sorted,
 * drawn for the depth prepass or again for its shadow, the ISP's depth
 * mode and Z Write Disable, the TSP's blend instructions */
static DX12PsoKey poly_key(u32 listType, bool sortTriangles, bool prepass, int shadowPass,
		u32 depthMode, bool zWriteDis, u32 src, u32 dst)
{
	DX12PsoKey key;
	memset(&key, 0, sizeof(key));
	key.kind = DX12_PSO_POLY;
	key.depthBound = true;
	key.depthTest = true;
	if (listType == ListType_Punch_Through || sortTriangles)
		key.depthFunc = 6;	/* greater or equal */
	else
		key.depthFunc = depthMode;
	/* Sorted translucent polygons, per triangle or per strip, do not write
	 * depth: a strip drawn in sorted order would hide parts of the ones after it.
	 * Their depth prepass does. */
	if (prepass)
		key.depthWrite = true;
	else if (sortTriangles)
		key.depthWrite = false;
	/* Z Write Disable seems to be ignored for punch-through.
	 * Fixes Worms World Party, Bust-a-Move 4 and Re-Volt */
	else if (listType == ListType_Punch_Through)
		key.depthWrite = true;
	else
		key.depthWrite = !zWriteDis;

	if (shadowPass != SHADOW_PASS_NONE)
	{
		/* Drawn again for its shadow: where this polygon is the one showing
		 * (same depth), takes shadows (stencil bit 7) and is in a volume
		 * (bit 0). Bit 6 marks the pixels drawn this way; whether one already
		 * marked may be drawn over is what tells the two kinds apart. */
		key.depthWrite = false;
		key.depthFunc = 2;
		key.stencil = shadowPass == SHADOW_PASS_LATER_WINS ? DX12Stencil_ShadowLaterWins : DX12Stencil_ShadowEarlierWins;
	}
	else if (listType == ListType_Opaque || listType == ListType_Punch_Through)
		key.stencil = DX12Stencil_Shadow;
	else
		key.stencil = DX12Stencil_None;

	/* Apparently punch-through polys support blending, or at least some combinations
	 * An opaque polygon is blended by its instructions too when it is in a
	 * list continuation; the ones of the first render pass have had theirs
	 * set to source x 1 and destination x 0 when the list was read. */
	key.writeColor = !prepass;
	if (!prepass && shadowPass == SHADOW_PASS_NONE
			&& (listType == ListType_Translucent || listType == ListType_Punch_Through || src != 1 || dst != 0))
	{
		key.blend = true;
		key.src = src;
		key.dst = dst;
	}
	return key;
}

/* Every pipeline draw_poly() can draw with (poly_key()), for every list
 * and pass the render draws it in, every depth mode and blend. The thread
 * makes them in this order: the blends that read nothing of the frame
 * first (one pipeline for them all, dx12_pso_key_pack()) with the depth
 * modes games mostly use, then the rest. */
static u64 prepared_keys[2 * 8 * 9 * 2 * 64];

static void tri_prepare_pipelines(void)
{
	static const u8 modes[] = { 6, 7, 3, 1, 2, 4, 5, 0 };
	static const struct { u32 list; bool sorted; bool prepass; int shadow; } draws[] = {
		{ ListType_Opaque, false, false, SHADOW_PASS_NONE }, { ListType_Punch_Through, false, false, SHADOW_PASS_NONE },
		{ ListType_Translucent, true, false, SHADOW_PASS_NONE }, { ListType_Translucent, true, true, SHADOW_PASS_NONE },
		{ ListType_Translucent, false, false, SHADOW_PASS_NONE },
		{ ListType_Opaque, false, false, SHADOW_PASS_LATER_WINS }, { ListType_Opaque, false, false, SHADOW_PASS_EARLIER_WINS },
		{ ListType_Punch_Through, false, false, SHADOW_PASS_LATER_WINS },
		{ ListType_Punch_Through, false, false, SHADOW_PASS_EARLIER_WINS },
	};
	size_t n = 0;
	u32 rare, m, d, zwd, src, dst;
	for (rare = 0; rare < 2; rare++)
		for (m = 0; m < sizeof(modes); m++)
			for (d = 0; d < sizeof(draws) / sizeof(draws[0]); d++)
				for (zwd = 0; zwd < 2; zwd++)
					for (src = 0; src < 8; src++)
						for (dst = 0; dst < 8; dst++)
							if ((dx12_src_folds(src) && dx12_dst_folds(dst)) != (rare != 0))
							{
								const DX12PsoKey key = poly_key(draws[d].list, draws[d].sorted, draws[d].prepass, draws[d].shadow,
										modes[m], zwd != 0, src, dst);
								prepared_keys[n++] = dx12_pso_key_pack(&key);
							}
	/* (each made once, where it first comes) */
	dx12_pso_prepare_many(prepared_keys, n);
}

static void draw_poly(u32 listType, bool sortTriangles, const PolyParam *poly, u32 first, u32 count,
		int shadowPass, const D3D12_RECT *within)
{
	D3D12_RECT scissorRect;
	const int tileClip = dx12r_set_tile_clip(poly->tileclip, &scissorRect);
	dx12_draw_state state;
	float trilinearAlpha = 1.f;
	const bool trilinear = PPW(poly->pcw, PCW_Texture) && PPW(poly->tsp, TSP_FilterMode) > 1 && listType != ListType_Punch_Through
			&& PPW(poly->tcw, TCW_MipMapped) == 1;
	const bool palette = tex_gpu_paletted(poly->tsp, poly->tcw, 0);
	float palette_index = 0.f;
	dx12_poly_constants constants;
	u32 flags = 0;
	u32 words[DX12C_POLY_CONSTANT_COUNT];
	DX12PsoKey key;
	if (within != NULL)
	{
		/* only the part of it inside this rectangle, and nothing if there is none */
		const D3D12_RECT *clip = tileClip == TILECLIP_OUTSIDE ? &scissorRect : &dx12r.base_scissor;
		D3D12_RECT r;
		set_rect(&r, MAX_L(within->left, clip->left), MAX_L(within->top, clip->top),
				MIN_L(within->right, clip->right), MIN_L(within->bottom, clip->bottom));
		if (r.right <= r.left || r.bottom <= r.top)
			return;
		state.scissor = r;
	}
	else if (tileClip == TILECLIP_OUTSIDE)
		state.scissor = scissorRect;
	else
		state.scissor = dx12r.base_scissor;
	state.scissor = dx12r_cut_scissor(&state.scissor);

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

	/* The polygon's root constants: its bits, what the Direct3D 11
	 * renderer made a shader for */
	memset(&constants, 0, sizeof(constants));
	constants.clipTest[0] = (float)scissorRect.left;
	constants.clipTest[1] = (float)scissorRect.top;
	constants.clipTest[2] = (float)scissorRect.right;
	constants.clipTest[3] = (float)scissorRect.bottom;
	constants.trilinearAlpha = trilinearAlpha;
	constants.paletteIndex = palette_index;
	constants.prepassAlpha = dx12r.prepass_draw ? (PrepassAlwaysHides(poly) ? -1.f : PREPASS_ALPHA_MIN) : 0.f;
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
	/* By the polygon alone, as the Vulkan renderer has it: the clamping
	 * limits change, and a shader is kept by the polygon's bit only */
	if (PPW(poly->tsp, TSP_ColorClamp))
		flags |= DX12_PF_CLAMP;
	if (trilinear)
		flags |= DX12_PF_TRILINEAR;
	if (palette)
		flags |= DX12_PF_PALETTE;
	if (shadowPass != SHADOW_PASS_NONE)
		flags |= DX12_PF_SHADOWED;
	constants.flags = flags;
	constants.texture = DX12_SRV_NONE;
	if (PPW(poly->pcw, PCW_Texture) && poly->texid != 0)
		constants.texture = dx12_texture_srv((const dx12_texture *)(uintptr_t)poly->texid);
	constants.sampler = dx12_sampler_index(poly->tsp);

	/* Depth, stencil and blend */
	key = poly_key(listType, sortTriangles, dx12r.prepass_draw, shadowPass,
			PPW(poly->isp, ISP_TSP_DepthMode), PPW(poly->isp, ISP_TSP_ZWriteDis) != 0, PPW(poly->tsp, TSP_SrcInstr), PPW(poly->tsp, TSP_DstInstr));
	key.gouraud = PPW(poly->pcw, PCW_Gouraud) != 0;
	state.stencilRef = key.stencil == DX12Stencil_ShadowLaterWins ? 0xC1
			: key.stencil == DX12Stencil_ShadowEarlierWins ? 0x81
			: key.stencil == DX12Stencil_Shadow && PPW(poly->pcw, PCW_Shadow) != 0 ? 0x80 : 0;
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
	/* SSAA: shaded at each sample, or once a pixel */
	key.samples = dx12c_target_samples();
	if (key.samples != 0)
		key.rate = ssaa_single_rate(poly) ? DX12_RATE_PIXEL : DX12_RATE_SAMPLE;
	state.pso = dx12_pso_get(&key);
	state.topology = dx12r.prepass_draw || (sortTriangles && !settings.pvr.Emulation.AlphaSortMode)
			? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;

	memcpy(words, &constants, sizeof(words));
	/* (a frame decoded on the GPU: a draw group by its arguments there) */
	if (pvrrc.gpu && !dx12r.gpu_sorted)
		dx12r_draw_gpu(&state, words, listType, shadowPass != SHADOW_PASS_NONE, first, count);
	else
		dx12r_draw(&state, words, count, first);
}

/* The polygons of a list that take shadows, drawn again where they are in a
 * modifier volume: see rend/shadows.h. */
static void draw_shadowed(u32 listType, const PolyParam *polys, u32 first, u32 last,
		const ScreenBounds *area, const D3D12_RECT *scissor)
{
	const PolyParam *pp_end = polys + last;
	const PolyParam *pp;
	for (pp = polys + first; pp != pp_end; pp++)
		/* (of a frame decoded on the GPU, the GPU has said which groups reach
		 * into the volumes: the others' second commands draw nothing) */
		if (CanDrawShadowed(listType, pp) && (pvrrc.gpu || poly_overlaps(pp, area)))
			draw_poly(listType, false, pp, pp->first, pp->count,
					shadow_later_wins(listType, pp) ? SHADOW_PASS_LATER_WINS : SHADOW_PASS_EARLIER_WINS,
					scissor);
}

ID3D12PipelineState *dx12r_modvol_pipeline(bool depthTest, bool depthWrite, u32 depthFunc, int stencil, u32 cull, bool writeColor, u32 special)
{
	DX12PsoKey key;
	memset(&key, 0, sizeof(key));
	key.kind = DX12_PSO_MODVOL;
	key.depthBound = true;
	key.depthTest = depthTest;
	key.depthWrite = depthWrite;
	key.depthFunc = depthFunc;
	key.stencil = (DX12Stencil)stencil;
	key.cull = cull;
	key.writeColor = writeColor;
	key.special = special;
	key.samples = dx12c_target_samples();
	return dx12_pso_get(&key);
}

static void draw_sorted(const SortTrigDrawParam *polys, u32 count, bool multipass)
{
	/* (a frame decoded on the GPU: its sorted indices are this frame's in
	 * the decoder's room for them, and name the GPU's vertices) */
	const u32 base = pvrrc.gpu ? 0 : (u32)TA_LIST_USED(pvrrc.idx, u32);
	u32 i;
	if (pvrrc.gpu)
	{
		if (count == 0)
			return;
		dx12r_flush_batch();
		dx12c_set_index_buffer(&dx12_ta_get_draw()->sorted);
		dx12r.gpu_sorted = true;
	}
	for (i = 0; i < count; i++)
		draw_poly(ListType_Translucent, true, polys[i].ppid, base + polys[i].first, polys[i].count, SHADOW_PASS_NONE, NULL);

	if (multipass && settings.rend.TranslucentPolygonDepthMask)
	{
		/* Sorted translucent polygons are drawn without writing depth. Where
		 * a later render pass needs their depth (Cosmic Smash), it is written
		 * now, for those that do not have Z write disabled. */
		ID3D12GraphicsCommandList *cmd;
		dx12r_flush_batch();
		cmd = dx12c_cmd();
		dx12c_set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		for (i = 0; i < count; i++)
		{
			const SortTrigDrawParam *param = &polys[i];
			D3D12_RECT scissorRect;
			ID3D12PipelineState *pso;
			if (param->count <= 2 || PPW(param->ppid->isp, ISP_TSP_ZWriteDis))
				continue;
			dx12c_set_scissor(dx12r_set_tile_clip(param->ppid->tileclip, &scissorRect) == TILECLIP_OUTSIDE ? &scissorRect : &dx12r.base_scissor);
			pso = dx12r_modvol_pipeline(true, true, 6, DX12Stencil_None, (u32)cull_of(param->ppid->isp), false, DX12_BLEND_KEY);
			if (pso == NULL)
				continue;
			dx12c_set_pipeline(pso);
			ID3D12GraphicsCommandList_DrawIndexedInstanced(cmd, param->count, 1, base + param->first, 0, 0);
		}
	}
	if (pvrrc.gpu)
	{
		dx12r_flush_batch();
		dx12c_set_index_buffer(&dx12r.index_view);
		dx12r.gpu_sorted = false;
	}
}

/* The depth prepass of a sorted list: rend/sorter.h */
static void draw_prepass(const SortTrigDrawParam *polys, u32 count)
{
	/* (as draw_sorted(): of a frame decoded on the GPU, the indices are in the decoder's room) */
	const u32 base = pvrrc.gpu ? 0 : (u32)TA_LIST_USED(pvrrc.idx, u32);
	u32 i;
	if (count == 0)
		return;
	if (pvrrc.gpu)
	{
		dx12r_flush_batch();
		dx12c_set_index_buffer(&dx12_ta_get_draw()->sorted);
		dx12r.gpu_sorted = true;
	}
	dx12r.prepass_draw = true;
	for (i = 0; i < count; i++)
		draw_poly(ListType_Translucent, true, polys[i].ppid, base + polys[i].first, polys[i].count, SHADOW_PASS_NONE, NULL);
	dx12r.prepass_draw = false;
	if (pvrrc.gpu)
	{
		dx12r_flush_batch();
		dx12c_set_index_buffer(&dx12r.index_view);
		dx12r.gpu_sorted = false;
	}
}

static void draw_list(u32 listType, bool sortTriangles, const PolyParam *polys, u32 first, u32 last)
{
	const PolyParam *pp_end = polys + last;
	const PolyParam *pp;
	for (pp = polys + first; pp != pp_end; pp++)
	{
		if (pp->count <= 2)
			continue;
		/* Depth mode "never" with nothing to force it to another: nothing of
		 * it is drawn, and no stencil is written where its depth test fails.
		 * Not drawn at all. */
		if (PPW(pp->isp, ISP_TSP_DepthMode) == 0 && listType != ListType_Punch_Through && !sortTriangles)
			continue;
		draw_poly(listType, sortTriangles, pp, pp->first, pp->count, SHADOW_PASS_NONE, NULL);
	}
}

static void draw_modvols(int first, int count, const RenderPass *previous_pass, const RenderPass *current_pass, int render_pass)
{
	const ModifierVolumeParam *mvo = TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam);
	const ModifierVolumeParam *params;
	ID3D12GraphicsCommandList *cmd;
	ID3D12PipelineState *pso;
	ScreenBounds area;
	int mod_base = -1, cmv;
	if (count == 0 || !dx12r.have_modvols)
		return;
	dx12r_flush_batch();
	cmd = dx12c_cmd();

	/* the modifier volumes' triangles, a position each */
	dx12c_set_vertex_buffer(&dx12r.modvol_view);
	dx12c_set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	dx12c_set_scissor(&dx12r.base_scissor);

	params = &mvo[first];

	for (cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam *param = &params[cmv];
		u32 mvFirst, mvCount, mv_mode, cull, mode;
		D3D12_RECT clipRect;
		mv_clip_range_get(&pvrrc, mvo, param, &mvFirst, &mvCount);

		if (mvCount == 0)
			continue;

		mv_mode = PPW(param->isp, ISP_Modvol_DepthMode);

		/* clipped like a polygon, to the rectangle it was sent under if it asked to be */
		if (dx12r_set_tile_clip(param->tileclip, &clipRect) == TILECLIP_OUTSIDE)
			dx12c_set_scissor(&clipRect);
		else
			dx12c_set_scissor(&dx12r.base_scissor);

		if (mod_base == -1)
			mod_base = (int)mvFirst;

		mode = PPW(param->isp, ISP_Modvol_CullMode);
		cull = mode == 3 ? 2 : mode == 2 ? 1 : 0;
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

		if (mv_mode == 1 || mv_mode == 2)
		{
			/* Sum the area */
			pso = dx12r_modvol_pipeline(false, false, 7, mv_mode == 1 ? DX12Stencil_Inclusion : DX12Stencil_Exclusion, cull, false, DX12_BLEND_KEY);
			if (pso != NULL)
			{
				dx12c_set_pipeline(pso);
				dx12c_set_stencil_ref(1);
				ID3D12GraphicsCommandList_DrawInstanced(cmd, (mvFirst + mvCount - mod_base) * 3, 1, mod_base * 3, 0);
			}
			mod_base = -1;
		}
	}
	dx12c_set_scissor(&dx12r.base_scissor);
	dx12c_set_vertex_buffer(&dx12r.vertex_view);

	/* The polygons that take shadows, again, where they are in a volume
	 * (whose bounds the header pass has worked out for a frame decoded on
	 * the GPU, which has the volumes' triangles: TA_GPU_SHADOWS) */
	if (pvrrc.gpu)
	{
		const f32 *ga = pvrrc.gpu_shadow_area[render_pass < 10 ? render_pass : 0];
		area.min_x = ga[0];
		area.min_y = ga[1];
		area.max_x = ga[2];
		area.max_y = ga[3];
	}
	else
		area = shadow_modvol_bounds(first, count);
	if (!screen_bounds_empty(&area))
	{
		int rect[4];
		D3D12_RECT scissor;
		bounds_to_scissor(&area, &dx12r.matrices.viewport, rect);
		set_rect(&scissor, rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]);
		draw_shadowed(ListType_Opaque, TA_LIST_HEAD(pvrrc.global_param_op, PolyParam), previous_pass->op_count, current_pass->op_count, &area, &scissor);
		draw_shadowed(ListType_Punch_Through, TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam), previous_pass->pt_count, current_pass->pt_count, &area, &scissor);
		dx12r_flush_batch();
		dx12c_set_scissor(&dx12r.base_scissor);
	}

	/* What is left: darkened as it is. The background polygon is the
	 * rectangle drawn: the first four vertices' positions. */
	pso = dx12r_modvol_pipeline(false, false, 7, DX12Stencil_Final, 0, true, DX12_BLEND_ALPHA);
	if (pso != NULL)
	{
		dx12c_set_pipeline(pso);
		dx12c_set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		dx12c_set_stencil_ref(0x81);
		ID3D12GraphicsCommandList_DrawIndexedInstanced(cmd, 4, 1, 0, 0, 0);
	}
}

static void tri_draw_frame(void)
{
	const RenderPass *passes = TA_LIST_HEAD(pvrrc.render_passes, RenderPass);
	const int npasses = TA_LIST_USED(pvrrc.render_passes, RenderPass);
	const PolyParam *op = TA_LIST_HEAD(pvrrc.global_param_op, PolyParam);
	const PolyParam *pt = TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam);
	const PolyParam *tr = TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam);
	RenderPass previous_pass;
	int render_pass;
	memset(&previous_pass, 0, sizeof(previous_pass));
	for (render_pass = 0; render_pass < npasses; render_pass++)
	{
		const RenderPass *current_pass = &passes[render_pass];

		DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d autosort %d\n", render_pass + 1,
				current_pass->op_count - previous_pass.op_count,
				current_pass->pt_count - previous_pass.pt_count,
				current_pass->tr_count - previous_pass.tr_count,
				current_pass->mvo_count - previous_pass.mvo_count, current_pass->autosort);
		draw_list(ListType_Opaque, false, op, previous_pass.op_count, current_pass->op_count);
		draw_list(ListType_Punch_Through, false, pt, previous_pass.pt_count, current_pass->pt_count);
		draw_modvols(previous_pass.mvo_count, current_pass->mvo_count - previous_pass.mvo_count, &previous_pass, current_pass, render_pass);
		if (current_pass->autosort)
		{
			u32 n;
			/* (a frame decoded on the GPU with a sorted pass was sorted by triangle: TA_GPU_SORT) */
			if (pvrrc.gpu)
			{
				const SortTrigDrawParam *draws = ta_gpu_prepass(render_pass, &n);
				draw_prepass(draws, n);
				draws = ta_gpu_sorted(render_pass, &n);
				draw_sorted(draws, n, render_pass < npasses - 1);
			}
			else if (!settings.pvr.Emulation.AlphaSortMode)
			{
				const SortTrigDrawParam *draws = SortPassDraws(&dx12r.prepass_passes, render_pass, &n);
				draw_prepass(draws, n);
				draws = SortPassDraws(&dx12r.sorted_passes, render_pass, &n);
				draw_sorted(draws, n, render_pass < npasses - 1);
			}
			else
			{
				const SortTrigDrawParam *draws = SortPassDraws(&dx12r.prepass_passes, render_pass, &n);
				draw_prepass(draws, n);
				SortPParams(previous_pass.tr_count, current_pass->tr_count - previous_pass.tr_count);
				draw_list(ListType_Translucent, true, tr, previous_pass.tr_count, current_pass->tr_count);
			}
		}
		else
			draw_list(ListType_Translucent, false, tr, previous_pass.tr_count, current_pass->tr_count);
		previous_pass = *current_pass;
	}
	dx12r_flush_batch();
}

/* ---- The per-triangle renderer ---- */

/* sorted translucent triangles sorted from the frame's depths, as
 * SortPassesTo() sorts them (TA_GPU_SORT); sorted by strip, the polygons
 * are sorted from their vertices, which a frame decoded on the GPU has
 * there alone: such a frame is parsed */
static u32 tri_ta_flags(void)
{
	return TA_GPU_DRAW | TA_GPU_PROVOKE | TA_GPU_SHADOWS
			| (!settings.pvr.Emulation.AlphaSortMode && dx12_ta_can_sort() ? TA_GPU_SORT : TA_GPU_NO_SORT);
}

static const dx12_renderer_kind tri_kind =
{
	NULL,					/* render_frame */
	tri_draw_frame,
	true,					/* binds_screen */
	tri_ta_flags,
	true,					/* ta_sorts */
	1,						/* ssaa_color_images */
	NULL,					/* ssaa_fixed_kib */
	tri_prepare_pipelines,
	NULL,					/* cut_scissor */
	NULL					/* hdr_sums */
};

static bool tri_init(void)
{
	return dx12r_init(&tri_kind, false);
}

static const renderer_ops dx12_renderer_ops =
{
	tri_init,
	dx12r_resize,
	dx12r_term,
	dx12r_process,
	dx12r_render,
	dx12r_keep_picture,
	dx12r_get_texture,
	dx12r_options_changed,
	NULL,
	NULL,
	NULL,
	NULL,
	dx12r_ready
};

const renderer_ops *rend_D3D12(void)
{
	return &dx12_renderer_ops;
}
