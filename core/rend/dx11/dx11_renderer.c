#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
/*
	Copyright 2021 flyinghead

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
#include "dx11_renderer.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
#include <retro_miscellaneous.h>
#include "dx11_shaders.h"
#include "dx11_texture.h"
#include "dx11_overlay.h"
#include "dx11_texdecoder.h"
#include "dx11_ta_decoder.h"
#include "dx11_writeout.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/tile/compute_frame.h"
#include "rend/tileclip.h"
#include "rend/shadows.h"
#include "rend/depth_scale.h"
#include "rend/rtt_watch.h"
#include "rend/last_picture.h"
#include "rend/modvol_clip.h"
#include "rend/texconv.h"

#define PPW(w, f) TAW_GET(w, f)
#define MIN_U(a, b) ((a) < (b) ? (a) : (b))
#define MAX_U(a, b) ((a) > (b) ? (a) : (b))
#define MAX_L(a, b) ((a) > (b) ? (a) : (b))
#define MIN_L(a, b) ((a) < (b) ? (a) : (b))

dx11_renderer dx11r;

typedef struct VertexConstants
{
	float scale[4];
	float offset[4];
	float depthScale[4];	/* 1/fZ_max, 1 / the scale's z (rend/depth_scale.h) */
} VertexConstants;

typedef struct FragmentConstants
{
	float colorClampMin[4];
	float colorClampMax[4];
	float fogColRam[4];
	float fogColVert[4];
	float alphaTestValue;
	float fogDensity;
	float shadeScale;
	float padding;
} FragmentConstants;

typedef struct PolyConstants
{
	float clipTest[4];
	float trilinearAlpha;
	float paletteIndex;
	float prepassAlpha;	/* the depth prepass's least alpha (rend/sorter.h), 0 for the colour */
	u32 polyKey;		/* the general pixel shader's: the polygon's key (dx11_shaders.h) */
} PolyConstants;

/* (the whole buffers: the constants are a whole number of 16-byte registers) */
typedef char dx11_vtx_check[sizeof(VertexConstants) % 16 == 0 ? 1 : -1];
typedef char dx11_frag_check[sizeof(FragmentConstants) % 16 == 0 ? 1 : -1];
typedef char dx11_poly_check[sizeof(PolyConstants) % 16 == 0 ? 1 : -1];
/* (the depth's scale after the scale and the offset: copied in there for a frame decoded on the GPU) */
typedef char dx11_depth_scale_check[offsetof(VertexConstants, depthScale) == 32 ? 1 : -1];

/* What the constant buffers have, so that what is the same is not written again */
static struct
{
	VertexConstants vtx;
	FragmentConstants frag;
	PolyConstants poly;
	bool vtx_valid;
	bool frag_valid;
	bool poly_valid;
} uploaded;

static const D3D11_INPUT_ELEMENT_DESC MainLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(Vertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, col), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, vtx_spc), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, (UINT)offsetof(Vertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0 },
};
static const D3D11_INPUT_ELEMENT_DESC PositionLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};
static const D3D11_INPUT_ELEMENT_DESC ModVolLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

static void set_rect(D3D11_RECT *r, LONG left, LONG top, LONG right, LONG bottom)
{
	r->left = left;
	r->top = top;
	r->right = right;
	r->bottom = bottom;
}

static void release_screen_target(dx11_screen_target *t)
{
	DX11_RELEASE(t->uav);
	DX11_RELEASE(t->target);
	DX11_RELEASE(t->view);
	DX11_RELEASE(t->texture);
}

static ID3D11Buffer *make_constant_buffer(UINT size)
{
	ID3D11Buffer *buffer = NULL;
	D3D11_BUFFER_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = (size + 15) & ~15u;
	/* (written whole with UpdateSubresource(): one call where mapping it takes two) */
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(ID3D11Device_CreateBuffer(dx11r.device, &desc, NULL, &buffer)))
		buffer = NULL;
	return buffer;
}

/* The polygon's constants, when they are not what the buffer has already */
static void set_poly_constants(const PolyConstants *constants)
{
	if (uploaded.poly_valid && !memcmp(constants, &uploaded.poly, sizeof(*constants)))
		return;
	dx11r_set_constants(dx11r.poly_constants, constants);
	uploaded.poly = *constants;
	uploaded.poly_valid = true;
}

static void set_scissor(const D3D11_RECT *rect)
{
	dx11c_set_scissor(rect);
}

static ID3D11DepthStencilState *depth_state(bool depthTest, bool depthWrite, int depthFunc, DX11Stencil stencil)
{
	return dx11_depth_states_get(&dx11r.depth_states, depthTest, depthWrite, depthFunc, stencil);
}

/* ---- The per-triangle renderer's own ---- */

static u32 tri_ta_flags(void)
{
	/* sorted translucent triangles sorted from the frame's depths, as
	 * SortPassesTo() sorts them (TA_GPU_SORT); sorted by strip, the polygons
	 * are sorted from their vertices, which a frame decoded on the GPU
	 * has there alone: such a frame is parsed */
	return TA_GPU_DRAW | TA_GPU_PROVOKE | TA_GPU_SHADOWS
			| (!settings.pvr.Emulation.AlphaSortMode && dx11_ta_can_sort() ? TA_GPU_SORT : TA_GPU_NO_SORT);
}

static ID3D11InputLayout *tri_poly_input_layout(void)
{
	return dx11r.main_layout;
}

static ID3D11Buffer *tri_poly_constants(void)
{
	return dx11r.poly_constants;
}

static void draw_frame(void);

static const dx11_renderer_kind tri_kind = {
	draw_frame, true, true, tri_ta_flags, true, tri_poly_input_layout, tri_poly_constants
};

/* ---- The renderer ---- */

bool dx11r_init(const dx11_renderer_kind *kind)
{
	ID3D11Device *device;
	ID3DBlob *blob;
	bool success;
	int i;

	dx11r.kind = kind;
	/* (the passes' sorted draws: room before the first frame) */
	SortPassesReserve(&dx11r.sorted_passes);
	SortPassesReserve(&dx11r.prepass_passes);
	device = dx11r.device = dx11c.device;
	dx11r.ctx = dx11c.device_context;
	if (dx11r.device == NULL || dx11r.ctx == NULL)
	{
		WARN_LOG(RENDERER, "DX11: no device\n");
		return false;
	}
	transform_matrix_init(&dx11r.matrices, false);

	dx11c_lock();
	/* Everything a frame can want made now, before the first: no shader or
	 * state is made on the draw path (AGENTS.md) */
	success = dx11_shaders_start();
	success = dx11_depth_states_make_all(&dx11r.depth_states) && success;
	success = dx11_blend_make_all() && success;
	success = dx11_samplers_make_all() && success;
	/* what a render read back takes, made now (dx11_rtt_init()) */
	dx11_rtt_init();
	blob = dx11_shaders_vs_blob();
	success = success && blob != NULL
			&& SUCCEEDED(ID3D11Device_CreateInputLayout(device, MainLayout, ARRAY_SIZE(MainLayout),
					ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &dx11r.main_layout));
	blob = dx11_shaders_modvol_vs_blob();
	success = success && blob != NULL
			&& SUCCEEDED(ID3D11Device_CreateInputLayout(device, PositionLayout, ARRAY_SIZE(PositionLayout),
					ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &dx11r.position_layout))
			&& SUCCEEDED(ID3D11Device_CreateInputLayout(device, ModVolLayout, ARRAY_SIZE(ModVolLayout),
					ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &dx11r.modvol_layout));

	dx11r.vtx_constants = make_constant_buffer(sizeof(VertexConstants));
	dx11r.frag_constants = make_constant_buffer(sizeof(FragmentConstants));
	dx11r.poly_constants = make_constant_buffer(sizeof(PolyConstants));
	success = success && dx11r.vtx_constants != NULL && dx11r.frag_constants != NULL && dx11r.poly_constants != NULL;

	/* No culling, front faces culled, back faces: ISP's culling mode 0 or 1, 2, 3 */
	for (i = 0; i < 3; i++)
	{
		D3D11_RASTERIZER_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = i == 0 ? D3D11_CULL_NONE : i == 1 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
		desc.FrontCounterClockwise = TRUE;
		desc.ScissorEnable = TRUE;
		/* (the depth is the shaders' own, the clip space's is always 1) */
		desc.DepthClipEnable = FALSE;
		success = success && SUCCEEDED(ID3D11Device_CreateRasterizerState(device, &desc, &dx11r.rasterizers[i]));
	}

	/* The fog table, two rows of 128: see check_fog_texture() */
	success = success && dx11_make_texture(128, 2, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE,
			&dx11r.fog_texture, &dx11r.fog_view, NULL, 0);
	dx11r.fog_uploaded_valid = false;
	/* The palette, a row of 1024 colours */
	success = success && dx11_make_texture(1024, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
			&dx11r.palette_texture, &dx11r.palette_view, NULL, 0);
	palette_updated = true;

	dx11r.have_picture = false;
	dx11r.last_screen = -1;
	dx11r.last_in_extra = false;
	dx11r.screen_width = dx11r.screen_height = 0;
	uploaded.vtx_valid = uploaded.frag_valid = uploaded.poly_valid = false;
	dx11r.held_from_process = false;
	if (!success)
	{
		WARN_LOG(RENDERER, "DX11 renderer initialization failed\n");
		dx11c_unlock();
		return false;
	}
	/* textures made on the GPU, and the textures themselves, all of them now (rend/tex_pool.h) */
	dx11_texdec_init();
	dx11_pool_init();
	/* ...and the cache's room for them (rend/tex_map.h) */
	dx11_texcache_reserve_for(dx11_pool_made());
	/* the chip's write-out, and the pictures made for it (dx11_writeout.h) */
	dx11r.writeout_pictures = dx11_writeout_init();
	dx11_textures_attach(dx11_texdec_ready(), true);
	/* GPU-side TA processing, if it is on: here, not in a frame */
	dx11_ta_want_sort(kind->ta_sorts);
	dx11_ta_setup(kind->ta_flags() != 0 && ta_gpu_wanted());
	dx11c_unlock();
	return true;
}

void dx11r_options_changed(void)
{
	if (dx11r.device == NULL)
		return;
	dx11c_lock();
	dx11_ta_want_sort(dx11r.kind->ta_sorts);
	dx11_ta_setup(dx11r.kind->ta_flags() != 0 && ta_gpu_wanted());
	dx11c_unlock();
}

void dx11r_term(void)
{
	u32 i;
	SortPassesFree(&dx11r.sorted_passes);
	SortPassesFree(&dx11r.prepass_passes);
	dx11c_lock();
	if (dx11r.ctx != NULL)
	{
		dx11c_set_targets(NULL, NULL);
		dx11c_unbind_ps_resources(4);
	}
	dx11c_set_frame(NULL, NULL);
	dx11_ta_term();
	ta_set_modvol_clip(false);
	dx11_rtt_term();
	dx11_texcache_clear();
	/* (what the cache's textures had is back in the pool by now) */
	dx11_textures_attach(false, false);
	dx11_texcache_free();
	dx11_pool_term();
	dx11_texdec_term();
	dx11_overlay_term();
	dx11_writeout_term();
	if (dx11r.device != NULL)
		NOTICE_LOG(RENDERER, "DX11: %u depth-stencil, %u blend and %u sampler states made on the draw path\n",
				dx11r.depth_states.draw_path, dx11_blends.draw_path, dx11_samplers.draw_path);
	dx11_depth_states_term(&dx11r.depth_states);
	DX11_RELEASE(dx11r.fb_view);
	DX11_RELEASE(dx11r.fb_texture);
	dx11r.fb_width = dx11r.fb_height = 0;
	DX11_RELEASE(dx11r.restored_picture);
	for (i = 0; i < DX11_SCREEN_SLOTS; i++)
		release_screen_target(&dx11r.screens[i]);
	dx11r.screen_count = 0;
	release_screen_target(&dx11r.extra);
	dx11r.last_in_extra = false;
	DX11_RELEASE(dx11r.stencil_srv);
	DX11_RELEASE(dx11r.depth_srv);
	DX11_RELEASE(dx11r.depth_view_read_only);
	DX11_RELEASE(dx11r.depth_view_read_stencil);
	DX11_RELEASE(dx11r.depth_view);
	DX11_RELEASE(dx11r.depth_tex);
	dx11r.depth_width = dx11r.depth_height = 0;
	DX11_RELEASE(dx11r.palette_view);
	DX11_RELEASE(dx11r.palette_texture);
	DX11_RELEASE(dx11r.fog_view);
	DX11_RELEASE(dx11r.fog_texture);
	for (i = 0; i < 3; i++)
		DX11_RELEASE(dx11r.rasterizers[i]);
	DX11_RELEASE(dx11r.poly_constants);
	DX11_RELEASE(dx11r.frag_constants);
	DX11_RELEASE(dx11r.vtx_constants);
	DX11_RELEASE(dx11r.geometry);
	dx11r.geometry_size = dx11r.geometry_used = 0;
	DX11_RELEASE(dx11r.modvol_layout);
	DX11_RELEASE(dx11r.position_layout);
	DX11_RELEASE(dx11r.main_layout);
	dx11r.device = NULL;
	dx11r.ctx = NULL;
	dx11c_unlock();
}

/* The size rendered at has changed, and the context is the same: the
 * screen's pictures are made again at the new size when the next frame is
 * drawn, and the last picture goes with them - ensure_screen() */
void dx11r_resize(int w, int h)
{
	(void)w;
	(void)h;
	/* (a change of the write-out option is one of size: the pictures are
	 * made again, for it to write or not) */
	if (dx11r.device != NULL && dx11r.writeout_pictures != !compute_frame_scalers_off)
	{
		dx11c_lock();
		dx11r.writeout_pictures = dx11_writeout_init();
		dx11r.screen_width = dx11r.screen_height = 0;
		dx11c_unlock();
	}
}

u64 dx11r_get_texture(u32 tsp, u32 tcw, int area)
{
	dx11_texture *tf = dx11_texcache_get(tsp, tcw, area);

	if (tf->texture == NULL)
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
	ID3D11Texture2D *texture = NULL;
	const u8 *pixels = last_picture(&width, &height);
	if (pixels == NULL)
		return;
	if (dx11_make_texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &texture,
			&dx11r.restored_picture, pixels, width * 4))
		/* (the view holds the texture) */
		DX11_RELEASE(texture);
	else
		DX11_RELEASE(dx11r.restored_picture);
}

/* Uploaded when it differs from what the card has: see UpdateFogTexture() in gles/gles.c */
static void check_fog_texture(void)
{
	u8 texData[256];
	MakeFogTexture(texData);
	if (dx11r.fog_uploaded_valid && !memcmp(dx11r.fog_uploaded, texData, sizeof(dx11r.fog_uploaded)))
		return;
	memcpy(dx11r.fog_uploaded, texData, sizeof(dx11r.fog_uploaded));
	dx11r.fog_uploaded_valid = true;
	ID3D11DeviceContext_UpdateSubresource(dx11r.ctx, (ID3D11Resource *)dx11r.fog_texture, 0, NULL, texData, 128, 256);
}

static void check_palette_texture(void)
{
	if (!palette_updated)
		return;
	palette_updated = false;
	ID3D11DeviceContext_UpdateSubresource(dx11r.ctx, (ID3D11Resource *)dx11r.palette_texture, 0, NULL, palette32_ram,
			1024 * 4, 1024 * 4);
}

static bool upload_framebuffer(void);

static bool process_locked(TA_context *ctx)
{
	bool result;
	if (!ctx->rend.isRTT)
		dx11_overlay_prepare();
	if (!ctx->rend.isRTT && !ctx->rend.isRenderFramebuffer)
		check_kept_picture();

	if (ctx->rend.isRenderFramebuffer)
		return upload_framebuffer();

	if (KillTex)
	{
		/* what was rendered to a texture and is not in video memory yet goes there before its picture does */
		rtt_watch_flush();
		dx11_texcache_clear();
	}

	/* the modifier volumes' triangles far off the screen cut before the GPU clips them (rend/modvol_clip.h) */
	ta_set_modvol_clip(true);
	/* GPU-side TA processing: the header pass makes the frame ready to be
	 * drawn from the GPU's decode, in the full parse's place, which is run
	 * now; or it is parsed as ever */
	if (!dx11_ta_ready() || !dx11_ta_prepare(ctx, dx11r.kind->ta_flags(), &result))
		result = ta_parse_vdrc(ctx);
	else if (result)
		dx11_ta_record(ctx);
	/* the textures the frame's polygons asked for that are made on the GPU, before the draws */
	dx11_texdec_flush();

	dx11_texcache_cleanup();

	if (result)
	{
		check_fog_texture();
		check_palette_texture();
	}
	return result;
}

bool dx11r_process(TA_context *ctx)
{
	/* The context is taken here and kept for the render() that follows,
	 * which is then one lock with this. Except when a render to the screen
	 * is handed over to the other thread when this is done (threaded
	 * rendering): the frontend is waited for before the screen's picture
	 * is drawn into, and never with the context taken - and not here,
	 * which would keep the emulation waiting too. */
	const bool keep = ctx->rend.isRTT || !settings.rend.ThreadedRendering;
	bool result;
	if (dx11r.device == NULL)
		return false;
	if (keep && !ctx->rend.isRTT)
		dx11c_wait_present_index();
	dx11c_lock();
	result = process_locked(ctx);
	dx11r.held_from_process = keep && result;
	if (!dx11r.held_from_process)
		dx11c_unlock();
	return result;
}

/* ---- What is set ---- */

void dx11r_set_cull_mode(int mode)
{
	dx11c_set_rasterizer(dx11r.rasterizers[mode == 3 ? 2 : mode == 2 ? 1 : 0]);
}

/* ---- Clipping ---- */

int dx11r_set_tile_clip(u32 val, D3D11_RECT *clipRect)
{
	int rect[4] = { 0, 0, 0, 0 };
	const int clipmode = tile_clip(val, &dx11r.matrices.viewport, rect);
	if (clipmode == TILECLIP_OUTSIDE)
		intersect_clip_rect(rect, dx11r.base_scissor.left, dx11r.base_scissor.top,
				dx11r.base_scissor.right - dx11r.base_scissor.left, dx11r.base_scissor.bottom - dx11r.base_scissor.top);
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

static void set_base_scissor(void)
{
	const bool wide_screen_on = settings.rend.WideScreen && !pvrrc.isRenderFramebuffer
			&& !transform_matrix_clipped(&dx11r.matrices);
	if (!wide_screen_on)
	{
		if (pvrrc.isRenderFramebuffer)
			set_rect(&dx11r.base_scissor, 0, 0, 640, 480);
		else
		{
			const xform *scissor = &dx11r.matrices.scissor;
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
			set_rect(&dx11r.base_scissor, x, y, x + round_at_least_0(width), y + round_at_least_0(height));
		}
	}
	else
		set_rect(&dx11r.base_scissor, 0, 0, (LONG)screen_width, (LONG)screen_height);
}

/* Direct3D takes a flat shaded triangle's colours from its first vertex.
 * On Dreamcast the last vertex is the provoking one so it is copied onto the
 * first (as in the Vulkan renderers). */
static void set_provoking_vertex(const PolyParam *list, int count)
{
	Vertex *verts = TA_LIST_HEAD(pvrrc.verts, Vertex);
	const u32 *idx = TA_LIST_HEAD(pvrrc.idx, u32);
	int i;
	for (i = 0; i < count; i++)
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

static void set_provoking_vertices(void)
{
	/* (a frame decoded on the GPU has had it done there: TA_GPU_PROVOKE) */
	if (pvrrc.gpu)
		return;
	set_provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_op, PolyParam), TA_LIST_USED(pvrrc.global_param_op, PolyParam));
	set_provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam), TA_LIST_USED(pvrrc.global_param_pt, PolyParam));
	set_provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam), TA_LIST_USED(pvrrc.global_param_tr, PolyParam));
}

/* ---- Buffers ---- */

/* Where a render's vertices, indexes and modifier volume triangles go in
 * the geometry buffer if they start at @start: each at a multiple of its
 * size there, as the draws count them. Returns where they end. */
static u32 lay_geometry(u32 start, u32 vertexBytes, u32 indexBytes, u32 modVolBytes, u32 *vertices, u32 *indexes, u32 *modVols)
{
	const u32 modVolSize = sizeof(float) * 3;
	*vertices = (start + sizeof(Vertex) - 1) / sizeof(Vertex) * sizeof(Vertex);
	*indexes = *vertices + vertexBytes;
	*modVols = (*indexes + indexBytes + modVolSize - 1) / modVolSize * modVolSize;
	return *modVols + modVolBytes;
}

/* The polygons, the volumes and the indexes, and the sorted triangles'
 * indexes after those, sorted there, and the vertices' colours made for
 * the card (set_provoking_vertices()) */
static bool upload_geometry(void)
{
	const bool gpu = pvrrc.gpu;
	const bool clipped = TA_LIST_USED(pvrrc.mv_clip, ModTriangle) != 0;
	const void *modVolHead;
	u32 vertexBytes, idxBytes, indexBytes, modVolBytes, vertices, indexes, modVols, end;
	D3D11_MAP mode;
	D3D11_MAPPED_SUBRESOURCE mapped;
	u8 *base;

	if (gpu && !clipped)
	{
		/* (the decode's, on the GPU, each from its start, its sorted passes sorted there: TA_GPU_SORT) */
		dx11r.sorted_index_count = 0;
		dx11r.vertex_base = 0;
		dx11r.index_base = 0;
		dx11r.modvol_base = 0;
		dx11r.have_modvols = pvrrc.gpu_modtrig != 0;
		return true;
	}
	/* (decoded on the GPU but with volume triangles cut: rend/modvol_clip.h)
	 * the cut ones, here, and the rest the decode's */
	modVolHead = clipped ? (const void *)TA_LIST_HEAD(pvrrc.mv_clip, ModTriangle) : (const void *)TA_LIST_HEAD(pvrrc.modtrig, ModTriangle);
	modVolBytes = clipped ? TA_LIST_BYTES(pvrrc.mv_clip) : TA_LIST_BYTES(pvrrc.modtrig);
	vertexBytes = gpu ? 0 : TA_LIST_BYTES(pvrrc.verts);
	idxBytes = TA_LIST_BYTES(pvrrc.idx);
	/* (with room for the sorted passes' indices, which are sorted into it) */
	if (gpu)
		dx11r.sorted_index_count = 0;
	indexBytes = gpu ? 0 : idxBytes + SortPassesRoom() * sizeof(u32);
	end = lay_geometry(dx11r.geometry_used, vertexBytes, indexBytes, modVolBytes, &vertices, &indexes, &modVols);
	/* After what the renders before this one have put there, and not over
	 * any of it; started again (discarded) when it is full - and always on
	 * a deferred context, whose commands are each a list of their own */
	mode = D3D11_MAP_WRITE_NO_OVERWRITE;
	if (dx11r.geometry == NULL || end > dx11r.geometry_size || !dx11c.immediate)
	{
		mode = D3D11_MAP_WRITE_DISCARD;
		end = lay_geometry(0, vertexBytes, indexBytes, modVolBytes, &vertices, &indexes, &modVols);
		if (dx11r.geometry == NULL || end > dx11r.geometry_size)
		{
			/* (larger only, never smaller again: a growing scene does not make it over and over) */
			u32 newSize = MAX_U(dx11r.geometry_size, 256u * 1024u);
			D3D11_BUFFER_DESC desc;
			while (newSize < end)
				newSize *= 2;
			DX11_RELEASE(dx11r.geometry);
			dx11r.geometry_size = 0;
			memset(&desc, 0, sizeof(desc));
			desc.ByteWidth = newSize;
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(ID3D11Device_CreateBuffer(dx11r.device, &desc, NULL, &dx11r.geometry)))
			{
				WARN_LOG(RENDERER, "DX11: buffer creation failed (%d bytes)\n", newSize);
				dx11r.geometry = NULL;
				/* (as it was before: the vertices made for the card, once) */
				set_provoking_vertices();
				return false;
			}
			dx11r.geometry_size = newSize;
		}
	}
	if (FAILED(ID3D11DeviceContext_Map(dx11r.ctx, (ID3D11Resource *)dx11r.geometry, 0, mode, 0, &mapped)))
	{
		set_provoking_vertices();
		return false;
	}
	base = (u8 *)mapped.pData;
	if (!gpu)
	{
		/* sorted from the vertices as the game sent them, then those made for the card */
		dx11r.sorted_index_count = SortPassesTo(&dx11r.sorted_passes, &dx11r.prepass_passes,
				(u32 *)(base + indexes + idxBytes), dx11r.kind->draws_sorted, true);
		set_provoking_vertices();
		memcpy(base + vertices, TA_LIST_HEAD(pvrrc.verts, Vertex), vertexBytes);
		memcpy(base + indexes, TA_LIST_HEAD(pvrrc.idx, u32), idxBytes);
	}
	if (modVolBytes != 0)
		memcpy(base + modVols, modVolHead, modVolBytes);
	ID3D11DeviceContext_Unmap(dx11r.ctx, (ID3D11Resource *)dx11r.geometry, 0);
	dx11r.geometry_used = end;

	dx11r.vertex_base = gpu ? 0 : (INT)(vertices / sizeof(Vertex));
	dx11r.index_base = gpu ? 0 : indexes / sizeof(u32);
	dx11r.modvol_base = modVols / (sizeof(float) * 3);
	dx11r.have_modvols = modVolBytes != 0;
	return true;
}

static void make_fragment_constants(FragmentConstants *constants)
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
}

void dx11r_bind_geometry(void)
{
	if (pvrrc.gpu)
	{
		const dx11_ta_draw *gpu = dx11_ta_get_draw();
		/* (the volumes' triangles cut on the CPU, when the frame has them: in the geometry buffer) */
		dx11c_set_geometry2(gpu->vertices, sizeof(Vertex),
				TA_LIST_USED(pvrrc.mv_clip, ModTriangle) != 0 ? dx11r.geometry : gpu->modtrig,
				sizeof(float) * 3, gpu->indices);
	}
	else
		dx11c_set_geometry(dx11r.geometry, sizeof(Vertex), sizeof(float) * 3, dx11r.geometry);
}

/* What every polygon of the frame is drawn with */
static void setup_frame(void)
{
	/* The normal matrix, made for a picture whose first line is the top
	 * one and whose clip space has y going up: the other way up from the
	 * Vulkan renderers', whose matrices these are, and nothing else */
	const xform *normal = &dx11r.matrices.normal;
	VertexConstants vtx;
	FragmentConstants frag;
	ID3D11Buffer *buffers[2];
	ID3D11ShaderResourceView *views[2];
	memset(&vtx, 0, sizeof(vtx));
	vtx.scale[0] = normal->sx;
	vtx.scale[1] = -normal->sy;
	vtx.scale[2] = normal->sz;
	vtx.offset[0] = normal->tx;
	vtx.offset[1] = -normal->ty;
	vtx.offset[2] = normal->tz;
	vtx.depthScale[0] = pvr_depth_scale(pvrrc.fZ_max);
	vtx.depthScale[1] = 1.f / normal->sz;
	if (!uploaded.vtx_valid || memcmp(&vtx, &uploaded.vtx, sizeof(vtx)))
	{
		dx11r_set_constants(dx11r.vtx_constants, &vtx);
		uploaded.vtx = vtx;
		uploaded.vtx_valid = true;
	}
	if (pvrrc.gpu)
	{
		/* A frame decoded on the GPU: its fZ_max is the GPU's, and so is
		 * the depth's scale, copied in from the decode's words over what
		 * the CPU put there - which is then not what the buffer has */
		dx11_ta_copy_depth_scale(dx11r.vtx_constants, offsetof(VertexConstants, depthScale));
		uploaded.vtx_valid = false;
	}
	make_fragment_constants(&frag);
	if (!uploaded.frag_valid || memcmp(&frag, &uploaded.frag, sizeof(frag)))
	{
		dx11r_set_constants(dx11r.frag_constants, &frag);
		uploaded.frag = frag;
		uploaded.frag_valid = true;
	}

	dx11c_set_vs_constants(dx11r.vtx_constants);
	buffers[0] = dx11r.frag_constants;
	buffers[1] = dx11r.kind->poly_constants();
	dx11c_set_ps_constants_n(0, 2, buffers);
	views[0] = dx11r.palette_view;
	views[1] = dx11r.fog_view;
	dx11c_set_ps_resources(1, 2, views);
	dx11c_set_ps_sampler(1, dx11_sampler_point());
	dx11c_set_input_layout(dx11r.kind->poly_input_layout());
	dx11r_bind_geometry();
	/* (what was pushed, for a polygon that pushes nothing: anything at all) */
	if (!uploaded.poly_valid && dx11r.kind->poly_constants() == dx11r.poly_constants)
	{
		PolyConstants zero;
		memset(&zero, 0, sizeof(zero));
		set_poly_constants(&zero);
	}
}

/* As large as a render of a screen to a texture is, 640x480 times Render
 * To Texture Upscaling: what the screen's depth and pictures are made at
 * least as large as, from the first, so that no such render has them made
 * again while the game runs (a larger one, which few games make, does) */
void dx11r_common_render(u32 *width, u32 *height)
{
	const u32 scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	*width = MAX_U(*width, 640u * scale);
	*height = MAX_U(*height, 480u * scale);
}

bool dx11r_ensure_depth(u32 width, u32 height)
{
	ID3D11Device *device = dx11r.device;
	D3D11_TEXTURE2D_DESC desc;
	D3D11_DEPTH_STENCIL_VIEW_DESC dsv;
	D3D11_SHADER_RESOURCE_VIEW_DESC srv;
	bool success;

	dx11r_common_render(&width, &height);
	if (dx11r.depth_tex != NULL && dx11r.depth_width >= width && dx11r.depth_height >= height)
		return true;
	width = MAX_U(width, dx11r.depth_width);
	height = MAX_U(height, dx11r.depth_height);
	DX11_RELEASE(dx11r.stencil_srv);
	DX11_RELEASE(dx11r.depth_srv);
	DX11_RELEASE(dx11r.depth_view_read_only);
	DX11_RELEASE(dx11r.depth_view_read_stencil);
	DX11_RELEASE(dx11r.depth_view);
	DX11_RELEASE(dx11r.depth_tex);
	dx11r.depth_width = dx11r.depth_height = 0;

	memset(&desc, 0, sizeof(desc));
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	/* (a float for the depth, as the Vulkan renderers have it where they can) */
	desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &dx11r.depth_tex)))
	{
		WARN_LOG(RENDERER, "DX11: depth buffer creation failed (%d x %d)\n", width, height);
		dx11r.depth_tex = NULL;
		return false;
	}
	memset(&dsv, 0, sizeof(dsv));
	dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	success = SUCCEEDED(ID3D11Device_CreateDepthStencilView(device, (ID3D11Resource *)dx11r.depth_tex, &dsv, &dx11r.depth_view));
	dsv.Flags = D3D11_DSV_READ_ONLY_STENCIL;
	success = SUCCEEDED(ID3D11Device_CreateDepthStencilView(device, (ID3D11Resource *)dx11r.depth_tex, &dsv,
			&dx11r.depth_view_read_stencil)) && success;
	dsv.Flags = D3D11_DSV_READ_ONLY_STENCIL | D3D11_DSV_READ_ONLY_DEPTH;
	success = SUCCEEDED(ID3D11Device_CreateDepthStencilView(device, (ID3D11Resource *)dx11r.depth_tex, &dsv,
			&dx11r.depth_view_read_only)) && success;
	memset(&srv, 0, sizeof(srv));
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	success = SUCCEEDED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)dx11r.depth_tex, &srv,
			&dx11r.depth_srv)) && success;
	srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
	success = SUCCEEDED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)dx11r.depth_tex, &srv,
			&dx11r.stencil_srv)) && success;
	if (!success)
		WARN_LOG(RENDERER, "DX11: depth buffer view creation failed\n");
	dx11r.depth_width = width;
	dx11r.depth_height = height;
	return true;
}

/* ---- The screen ---- */

static bool make_screen_target(u32 width, u32 height, dx11_screen_target *t, bool storage)
{
	ID3D11Device *device = dx11r.device;
	static const float black[4] = { 0.f, 0.f, 0.f, 1.f };
	if (!dx11_make_texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET
				| (storage ? D3D11_BIND_UNORDERED_ACCESS : 0), &t->texture, &t->view, NULL, 0)
			|| FAILED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)t->texture, NULL, &t->target))
			|| (storage && FAILED(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource *)t->texture, NULL, &t->uav))))
	{
		release_screen_target(t);
		return false;
	}
	ID3D11DeviceContext_ClearRenderTargetView(dx11r.ctx, t->target, black);
	return true;
}

/* (the view of what the last picture is in: held, as the picture the next render starts from) */
static void restore_from(ID3D11ShaderResourceView *view)
{
	DX11_ASSIGN(dx11r.restored_picture, view);
}

static bool ensure_screen(u32 width, u32 height)
{
	const unsigned count = MIN_U(dx11c_present_count(), DX11_SCREEN_SLOTS);
	dx11_screen_target *target;
	u32 i;

	if (width != dx11r.screen_width || height != dx11r.screen_height)
	{
		/* Another size. The picture with the last frame goes on as the
		 * picture the next render starts from, at whatever size the screen
		 * now is (Vulkan's carried picture): nothing is read back or
		 * loaded again. */
		if (dx11r.have_picture && dx11r.last_in_extra)
			restore_from(dx11r.extra.view);
		else if (dx11r.have_picture && dx11r.last_screen >= 0 && (u32)dx11r.last_screen < dx11r.screen_count)
			restore_from(dx11r.screens[dx11r.last_screen].view);
		for (i = 0; i < dx11r.screen_count; i++)
			release_screen_target(&dx11r.screens[i]);
		dx11r.screen_count = 0;
		release_screen_target(&dx11r.extra);
		dx11r.have_picture = false;
		dx11r.last_in_extra = false;
		dx11r.last_screen = -1;
		dx11r.screen_width = width;
		dx11r.screen_height = height;
	}
	if (dx11r.screen_count > count)
	{
		if (dx11r.last_screen >= (int)count && !dx11r.last_in_extra)
		{
			if (dx11r.have_picture)
				restore_from(dx11r.screens[dx11r.last_screen].view);
			dx11r.have_picture = false;
			dx11r.last_screen = -1;
		}
		for (i = count; i < dx11r.screen_count; i++)
			release_screen_target(&dx11r.screens[i]);
	}
	dx11r.screen_count = count;
	if ((u32)dx11r.screen_index >= count)
		return false;
	target = &dx11r.screens[dx11r.screen_index];
	if (target->texture == NULL && !make_screen_target(width, height, target, dx11r.writeout_pictures))
		return false;
	if (dx11c_is_handed_over(target->texture))
	{
		/* A render to the screen after one this frame has handed over
		 * already: this one goes to the extra picture, which takes the
		 * place of the one handed over, and that stays as it is until the
		 * frontend has shown it. (The last picture goes where its picture
		 * goes.) */
		dx11_screen_target swap;
		if (dx11r.extra.texture == NULL && !make_screen_target(width, height, &dx11r.extra, dx11r.writeout_pictures))
			return false;
		swap = *target;
		*target = dx11r.extra;
		dx11r.extra = swap;
		if (dx11r.last_in_extra)
		{
			dx11r.last_in_extra = false;
			dx11r.last_screen = dx11r.screen_index;
		}
		else if (dx11r.last_screen == dx11r.screen_index)
			dx11r.last_in_extra = true;
	}
	return dx11r_ensure_depth(width, height);
}

bool dx11r_keeps_last_picture(void)
{
	return !pvrrc.clearFramebuffer && transform_matrix_clipped(&dx11r.matrices)
			&& (dx11r.have_picture || dx11r.restored_picture != NULL);
}

/* What the last picture is drawn from */
static ID3D11ShaderResourceView *last_picture_view(void)
{
	if (!dx11r.have_picture)
		return dx11r.restored_picture;
	return dx11r.last_in_extra ? dx11r.extra.view : dx11r.screens[dx11r.last_screen].view;
}

static ID3D11Texture2D *last_picture_texture(void)
{
	return dx11r.last_in_extra ? dx11r.extra.texture : dx11r.screens[dx11r.last_screen].texture;
}

void dx11r_draw_last_picture(u32 width, u32 height)
{
	ID3D11ShaderResourceView *view = last_picture_view();
	if (view == NULL)
		return;
	dx11c_set_viewport((float)width, (float)height);
	/* (the picture kept from another context can be of another size) */
	dx11c_draw_quad(view, dx11r.have_picture ? dx11_sampler_point() : dx11_sampler_linear());
	dx11c_set_ps_resource(0, NULL);
}

static bool begin_screen(void)
{
	dx11_screen_target *target;
	float color[4] = { 0.f, 0.f, 0.f, 1.f };
	dx11r.screen_index = (int)dx11c_present_index();
	if (!ensure_screen(screen_width, screen_height))
		return false;
	target = &dx11r.screens[dx11r.screen_index];
	dx11r.target_view = target->target;
	dx11r.target_width = dx11r.screen_width;
	dx11r.target_height = dx11r.screen_height;

	transform_matrix_calc(&dx11r.matrices, &pvrrc);
	if (!dx11r.kind->binds_screen)
	{
		set_base_scissor();
		return true;
	}
	dx11c_set_targets(target->target, dx11r.depth_view);
	ID3D11DeviceContext_ClearDepthStencilView(dx11r.ctx, dx11r.depth_view, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.f, 0);

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour; any other starts black - unless the render covers only part
	 * of the screen, which leaves the rest as it was: the last picture,
	 * which is in this picture already if the last frame went to it.
	 * (Only for such a render: one that covers the screen pays nothing.) */
	if (dx11r_keeps_last_picture())
	{
		if (dx11r.have_picture && last_picture_texture() == target->texture)
			;
		else if (dx11r.have_picture)
			ID3D11DeviceContext_CopyResource(dx11r.ctx, (ID3D11Resource *)target->texture, (ID3D11Resource *)last_picture_texture());
		else
		{
			ID3D11DeviceContext_ClearRenderTargetView(dx11r.ctx, target->target, color);
			dx11r_draw_last_picture(dx11r.screen_width, dx11r.screen_height);
		}
	}
	else
	{
		if (pvrrc.clearFramebuffer)
		{
			color[0] = VO_BORDER_COL.Red / 255.f;
			color[1] = VO_BORDER_COL.Green / 255.f;
			color[2] = VO_BORDER_COL.Blue / 255.f;
		}
		ID3D11DeviceContext_ClearRenderTargetView(dx11r.ctx, target->target, color);
	}
	dx11c_set_viewport((float)dx11r.screen_width, (float)dx11r.screen_height);
	set_base_scissor();
	return true;
}

/* (@writeOut: the chip's write-out over the picture first, a frame the
 * renderer drew: dx11_writeout.h) */
static void end_screen(bool writeOutPass)
{
	dx11_screen_target *target = &dx11r.screens[dx11r.screen_index];
	if (writeOutPass && target->uav != NULL)
	{
		dx11c_set_targets(NULL, NULL);
		dx11_writeout_record(target->uav, dx11r.screen_width, dx11r.screen_height);
	}
	/* what goes over the picture, onto it (with the depth test off: whatever depth is bound is not looked at) */
	if (!dx11_overlay_empty())
	{
		dx11c_set_targets(target->target, dx11r.kind->binds_screen ? dx11r.depth_view : NULL);
		dx11c_set_viewport((float)dx11r.screen_width, (float)dx11r.screen_height);
		dx11_overlay_draw(dx11r.screen_width, dx11r.screen_height);
	}
	dx11c_set_targets(NULL, NULL);

	dx11r.have_picture = true;
	dx11r.last_screen = dx11r.screen_index;
	dx11r.last_in_extra = false;
	DX11_RELEASE(dx11r.restored_picture);
	dx11c_set_frame(target->texture, target->view);
}

void dx11r_keep_picture(void)
{
	u8 *pixels;
	if (!dx11r.have_picture || dx11r.last_screen < 0 || (u32)dx11r.last_screen >= dx11r.screen_count)
		return;
	pixels = last_picture_keep(dx11r.screen_width, dx11r.screen_height);
	if (pixels != NULL && !dx11_read_picture(last_picture_texture(), dx11r.screen_width, dx11r.screen_height, pixels))
		last_picture_drop();
}

/* ---- Renders to textures ---- */

static bool begin_rtt(void)
{
	static const float black[4] = { 0.f, 0.f, 0.f, 1.f };
	/* the frame from its first pixel, as far as the pixel clip goes (see transform_matrix.h) */
	const u32 origWidth = pvrrc.fb_X_CLIP.max + 1;
	const u32 origHeight = pvrrc.fb_Y_CLIP.max + 1;
	const u32 clipX = pvrrc.fb_X_CLIP.min, clipY = pvrrc.fb_Y_CLIP.min;
	const u32 clipWidth = origWidth - clipX, clipHeight = origHeight - clipY;
	const u32 scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	u32 upscaledWidth = origWidth;
	u32 upscaledHeight = origHeight;
	u32 heightPow2 = 8, widthPow2 = 8, tcw, tsp = 0, u, v;
	dx11_texture *rtt;

	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d @ %08x\n", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max, FB_W_SOF1 & VRAM_MASK);
	transform_matrix_calc(&dx11r.matrices, &pvrrc);

	dx11r.rtt_addr = FB_W_SOF1 & VRAM_MASK;
	while (heightPow2 < upscaledHeight)
		heightPow2 *= 2;
	while (widthPow2 < upscaledWidth)
		widthPow2 *= 2;
	upscaledWidth *= scale;
	upscaledHeight *= scale;
	widthPow2 *= scale;
	heightPow2 *= scale;

	if (!dx11r_ensure_depth(widthPow2, heightPow2))
		return false;

	/* what an earlier render left waiting in this memory, while its picture is still what it was: see rtt_watch.h */
	dx11_rtt_supersede(dx11r.rtt_addr, clipX, clipY, clipWidth, clipHeight);
	/* TexAddr : fb_rtt.TexAddr, Reserved : 0, StrideSel : 0, ScanOrder : 1 */
	tcw = TAW_SET(TAW_SET(0, TCW_TexAddr, dx11r.rtt_addr >> 3), TCW_ScanOrder, 1);
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

	rtt = dx11r.rtt_texture = dx11_texcache_get(tsp, tcw, 0);
	if (rtt->texture == NULL)
		texcache_create(&rtt->base);

	if (rtt->spare != NULL)
	{
		D3D11_TEXTURE2D_DESC desc;
		ID3D11Texture2D_GetDesc(rtt->spare, &desc);
		if (desc.Width != widthPow2 || desc.Height != heightPow2 || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM
				|| (desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 || desc.MipLevels != 1)
		{
			dx11_texture_release_spare(rtt);
			DX11_RELEASE(rtt->spare_target);
			DX11_RELEASE(rtt->spare_view);
			DX11_RELEASE(rtt->spare);
		}
	}
	/* (one of the pool's, made at the start, where it has one of this shape) */
	if (rtt->spare == NULL && !dx11_texture_take_spare(rtt, widthPow2, heightPow2)
			&& !dx11_make_texture(widthPow2, heightPow2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
					&rtt->spare, &rtt->spare_view, NULL, 0))
		return false;
	if (rtt->spare_target == NULL
			&& FAILED(ID3D11Device_CreateRenderTargetView(dx11r.device, (ID3D11Resource *)rtt->spare, NULL, &rtt->spare_target)))
	{
		rtt->spare_target = NULL;
		WARN_LOG(RENDERER, "DX11: render to texture target creation failed\n");
		return false;
	}

	dx11r.target_view = rtt->spare_target;
	dx11r.target_width = upscaledWidth;
	dx11r.target_height = upscaledHeight;

	/* (a texture this render draws with is not to be bound while it is
	 * drawn into: the textures' slots, not the palette's and the fog's) */
	dx11c_set_ps_resource(0, NULL);
	dx11c_set_ps_resource(3, NULL);
	if (dx11r.kind->binds_screen)
	{
		dx11c_set_targets(rtt->spare_target, dx11r.depth_view);
		ID3D11DeviceContext_ClearRenderTargetView(dx11r.ctx, rtt->spare_target, black);
		ID3D11DeviceContext_ClearDepthStencilView(dx11r.ctx, dx11r.depth_view, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.f, 0);
		dx11c_set_viewport((float)upscaledWidth, (float)upscaledHeight);
	}
	/* only the pixel clip is written */
	set_rect(&dx11r.base_scissor, (LONG)(clipX * scale), (LONG)(clipY * scale),
			(LONG)((clipX + clipWidth) * scale), (LONG)((clipY + clipHeight) * scale));
	return true;
}

static void end_rtt(void)
{
	dx11_texture *rtt = dx11r.rtt_texture;
	/* (left bound: whatever binds the texture to be read from binds
	 * another target first) */
	dx11_texture_swap_with_spare(rtt);

	rtt->base.dirty = 0;
	rtt->base.stride_read = 0;	/* (rendered, not read) */
	rtt->base.made_type = 0;
	libCore_vramlock_Lock(rtt->base.sa_tex, rtt->base.sa + rtt->base.size - 1, &rtt->base);
	/* video memory gets it when something first touches it there */
	dx11_rtt_watch(rtt, dx11r.rtt_addr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
	dx11r.rtt_texture = NULL;
}

/* ---- A frame the game wrote to the framebuffer itself ---- */

static bool upload_framebuffer(void)
{
	FramebufferRead fb;
	int width, height;
	D3D11_MAPPED_SUBRESOURCE mapped;

	if (FB_R_SIZE.fb_x_size == 0 || FB_R_SIZE.fb_y_size == 0)
		return false;

	ReadFramebufferGeometry(&fb);
	width = fb.width;
	height = fb.height;

	if (dx11r.fb_texture == NULL || width != dx11r.fb_width || height != dx11r.fb_height)
	{
		/* (written by the processor every frame: one the card reads where it
		 * is written, with no copy of it made on the way) */
		D3D11_TEXTURE2D_DESC desc;
		DX11_RELEASE(dx11r.fb_view);
		DX11_RELEASE(dx11r.fb_texture);
		memset(&desc, 0, sizeof(desc));
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(ID3D11Device_CreateTexture2D(dx11r.device, &desc, NULL, &dx11r.fb_texture)))
			dx11r.fb_texture = NULL;
		else if (FAILED(ID3D11Device_CreateShaderResourceView(dx11r.device, (ID3D11Resource *)dx11r.fb_texture, NULL, &dx11r.fb_view)))
			dx11r.fb_view = NULL;
		if (dx11r.fb_view == NULL)
		{
			WARN_LOG(RENDERER, "DX11: texture creation failed (%d x %d, format %d)\n", width, height, (int)desc.Format);
			DX11_RELEASE(dx11r.fb_texture);
			return false;
		}
		dx11r.fb_width = width;
		dx11r.fb_height = height;
	}
	if (FAILED(ID3D11DeviceContext_Map(dx11r.ctx, (ID3D11Resource *)dx11r.fb_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	/* converted straight into the texture the card reads */
	ReadFramebufferTo(&fb, (u8 *)mapped.pData, mapped.RowPitch);
	ID3D11DeviceContext_Unmap(dx11r.ctx, (ID3D11Resource *)dx11r.fb_texture, 0);
	return true;
}

/* Into the screen's picture, where the game's 640x480 is, as the OpenGL
 * renderers draw it */
static void draw_framebuffer(void)
{
	static const float black[4] = { 0.f, 0.f, 0.f, 1.f };
	dx11_screen_target *target;
	const xform *viewport;
	float x0, x1, y0, y1;
	dx11r.screen_index = (int)dx11c_present_index();
	if (!ensure_screen(screen_width, screen_height))
		return;
	target = &dx11r.screens[dx11r.screen_index];
	transform_matrix_calc(&dx11r.matrices, &pvrrc);
	dx11c_set_targets(target->target, NULL);
	ID3D11DeviceContext_ClearRenderTargetView(dx11r.ctx, target->target, black);
	dx11c_set_viewport((float)dx11r.screen_width, (float)dx11r.screen_height);

	viewport = &dx11r.matrices.viewport;
	x0 = xform_x(viewport, 0.f);
	x1 = xform_x(viewport, 640.f);
	y0 = xform_y(viewport, 0.f);
	y1 = xform_y(viewport, 480.f);
	dx11c_draw_quad_at(dx11r.fb_view, dx11_sampler_linear(),
			x0 * 2.f / (float)dx11r.screen_width - 1.f, 1.f - y1 * 2.f / (float)dx11r.screen_height,
			(x1 - x0) * 2.f / (float)dx11r.screen_width, (y1 - y0) * 2.f / (float)dx11r.screen_height,
			0.f, 0.f, 1.f, 1.f, NULL, NULL);
	end_screen(false);
}

/* ---- The frame ---- */

static bool render_locked(void)
{
	if (pvrrc.isRenderFramebuffer)
	{
		draw_framebuffer();
		return true;
	}

	if (pvrrc.isRTT ? !begin_rtt() : !begin_screen())
	{
		dx11r.rtt_texture = NULL;
		return false;
	}
	/* (sorted into the geometry buffer, and the provoking vertices set: upload_geometry()) */
	if (!upload_geometry())
	{
		dx11c_set_targets(NULL, NULL);
		dx11r.rtt_texture = NULL;
		return false;
	}
	setup_frame();

	dx11r.kind->draw_frame();

	if (pvrrc.isRTT)
		end_rtt();
	else
		end_screen(true);

	return !pvrrc.isRTT;
}

bool dx11r_render(void)
{
	/* the context, taken by process() and kept for this, or taken here -
	 * and given back on the way out */
	const bool held = dx11r.held_from_process;
	bool result;
	dx11r.held_from_process = false;
	if (dx11r.device == NULL)
	{
		if (held)
			dx11c_unlock();
		return false;
	}
	if (pvrrc.isRTT && (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min))
	{
		/* A pixel clip that ends before it starts holds no pixel: the
		 * chip writes nothing. (Its size would be negative here.) */
		WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d\n",
				pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
		if (held)
			dx11c_unlock();
		return false;
	}
	if (!held)
	{
		/* the frontend may still be showing what is in the screen's next
		 * picture: it is waited for, before the context is taken */
		if (!pvrrc.isRTT)
			dx11c_wait_present_index();
		dx11c_lock();
	}
	result = render_locked();
	dx11c_unlock();
	return result;
}

/* ---- The polygons, as the Vulkan renderer's Drawer draws them ---- */

enum { SHADOW_PASS_NONE, SHADOW_PASS_LATER_WINS, SHADOW_PASS_EARLIER_WINS };

static void draw_poly(u32 listType, bool sortTriangles, const PolyParam *poly, u32 first, u32 count,
		int shadowPass, const D3D11_RECT *within)
{
	D3D11_RECT scissorRect;
	const int tileClip = dx11r_set_tile_clip(poly->tileclip, &scissorRect);
	float trilinearAlpha = 1.f, palette_index = 0.f;
	bool palette, general, depthWrite, shadowed;
	DX11FragmentShaderParams params;
	int depthFunc;

	if (within != NULL)
	{
		/* only the part of it inside this rectangle, and nothing if there is none */
		const D3D11_RECT *clip = tileClip == TILECLIP_OUTSIDE ? &scissorRect : &dx11r.base_scissor;
		D3D11_RECT r;
		set_rect(&r, MAX_L(within->left, clip->left), MAX_L(within->top, clip->top),
				MIN_L(within->right, clip->right), MIN_L(within->bottom, clip->bottom));
		if (r.right <= r.left || r.bottom <= r.top)
			return;
		set_scissor(&r);
	}
	else if (tileClip == TILECLIP_OUTSIDE)
		set_scissor(&scissorRect);
	else
		set_scissor(&dx11r.base_scissor);

	if (PPW(poly->tsp, TSP_FilterMode) > 1 && PPW(poly->pcw, PCW_Texture) && listType != ListType_Punch_Through
			&& PPW(poly->tcw, TCW_MipMapped) == 1)
	{
		trilinearAlpha = 0.25f * (PPW(poly->tsp, TSP_MipMapD) & 0x3);
		if (PPW(poly->tsp, TSP_FilterMode) == 2)
			/* Trilinear pass A */
			trilinearAlpha = 1.f - trilinearAlpha;
	}
	palette = tex_gpu_paletted(poly->tsp, poly->tcw, 0);
	if (palette)
	{
		if (PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_4BPP)
			palette_index = ((float)(PPW(poly->tcw, TCW_PalSelect) << 4) + 0.5f) / 1024.f;
		else
			palette_index = ((float)((PPW(poly->tcw, TCW_PalSelect) >> 4) << 8) + 0.5f) / 1024.f;
	}
	memset(&params, 0, sizeof(params));
	params.alphaTest = listType == ListType_Punch_Through;
	params.bumpmap = PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_BUMPMAP;
	/* By the polygon alone, as the Vulkan renderer has it: the clamping
	 * limits change, and a shader is kept by the polygon's bit only */
	params.clamping = PPW(poly->tsp, TSP_ColorClamp) != 0;
	params.insideClipTest = (poly->tileclip >> 28) == 3;
	params.fog = (int)PPW(poly->tsp, TSP_FogCtrl);
	params.gouraud = PPW(poly->pcw, PCW_Gouraud) != 0;
	params.ignoreTexAlpha = PPW(poly->tsp, TSP_IgnoreTexA) || PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_565;
	params.offset = PPW(poly->pcw, PCW_Offset) != 0;
	params.shaderInstr = (int)PPW(poly->tsp, TSP_ShadInstr);
	params.texture = PPW(poly->pcw, PCW_Texture) != 0;
	params.trilinear = PPW(poly->pcw, PCW_Texture) && PPW(poly->tsp, TSP_FilterMode) > 1 && listType != ListType_Punch_Through
			&& PPW(poly->tcw, TCW_MipMapped) == 1;
	params.useAlpha = PPW(poly->tsp, TSP_UseAlpha) != 0;
	params.palette = palette;
	params.shadowed = shadowPass != SHADOW_PASS_NONE;
	dx11c_set_vs(dx11_shaders_vs(params.gouraud));
	dx11c_set_ps(dx11_shaders_ps(&params, &general));

	/* (the general shader's key is the polygon's: in its constants) */
	if (tileClip == TILECLIP_INSIDE || trilinearAlpha != 1.f || palette || dx11r.prepass_draw || general)
	{
		PolyConstants constants;
		constants.clipTest[0] = (float)scissorRect.left;
		constants.clipTest[1] = (float)scissorRect.top;
		constants.clipTest[2] = (float)scissorRect.right;
		constants.clipTest[3] = (float)scissorRect.bottom;
		constants.trilinearAlpha = trilinearAlpha;
		constants.paletteIndex = palette_index;
		constants.prepassAlpha = dx11r.prepass_draw ? (PrepassAlwaysHides(poly) ? -1.f : PREPASS_ALPHA_MIN) : 0.f;
		constants.polyKey = general ? dx11_shaders_key(&params) : 0u;
		set_poly_constants(&constants);
	}

	if (PPW(poly->pcw, PCW_Texture) && poly->texid != 0)
	{
		const dx11_texture *texture = (const dx11_texture *)(uintptr_t)poly->texid;
		dx11r_set_texture(0, texture->texture_view, dx11_sampler_get(poly->tsp));
	}

	/* Depth and stencil */
	if (listType == ListType_Punch_Through || sortTriangles)
		depthFunc = 6;	/* greater or equal */
	else
		depthFunc = (int)PPW(poly->isp, ISP_TSP_DepthMode);
	/* Sorted translucent polygons, per triangle or per strip, do not write
	 * depth: a strip drawn in sorted order would hide parts of the ones after it.
	 * Their depth prepass does. */
	if (dx11r.prepass_draw)
		depthWrite = true;
	else if (sortTriangles)
		depthWrite = false;
	/* Z Write Disable seems to be ignored for punch-through.
	 * Fixes Worms World Party, Bust-a-Move 4 and Re-Volt */
	else if (listType == ListType_Punch_Through)
		depthWrite = true;
	else
		depthWrite = !PPW(poly->isp, ISP_TSP_ZWriteDis);

	shadowed = listType == ListType_Opaque || listType == ListType_Punch_Through;
	if (shadowPass != SHADOW_PASS_NONE)
	{
		/* Drawn again for its shadow: where this polygon is the one showing
		 * (same depth), takes shadows (stencil bit 7) and is in a volume
		 * (bit 0). Bit 6 marks the pixels drawn this way; whether one already
		 * marked may be drawn over is what tells the two kinds apart. */
		if (shadowPass == SHADOW_PASS_LATER_WINS)
			dx11c_set_depth_stencil(depth_state(true, false, 2, DX11Stencil_ShadowLaterWins), 0xC1);
		else
			dx11c_set_depth_stencil(depth_state(true, false, 2, DX11Stencil_ShadowEarlierWins), 0x81);
	}
	else if (shadowed)
		dx11c_set_depth_stencil(depth_state(true, depthWrite, depthFunc, DX11Stencil_Shadow),
				PPW(poly->pcw, PCW_Shadow) != 0 ? 0x80 : 0);
	else
		dx11c_set_depth_stencil(depth_state(true, depthWrite, depthFunc, DX11Stencil_None), 0);

	/* Apparently punch-through polys support blending, or at least some combinations.
	 * An opaque polygon is blended by its instructions too when it is in a
	 * list continuation; the ones of the first render pass have had theirs
	 * set to source x 1 and destination x 0 when the list was read. */
	if (dx11r.prepass_draw)
		dx11c_set_blend(dx11_blend_get(false, 1, 0, false), NULL);
	else if (shadowPass == SHADOW_PASS_NONE
			&& (listType == ListType_Translucent || listType == ListType_Punch_Through
				|| PPW(poly->tsp, TSP_SrcInstr) != 1 || PPW(poly->tsp, TSP_DstInstr) != 0))
		dx11c_set_blend(dx11_blend_get(true, PPW(poly->tsp, TSP_SrcInstr), PPW(poly->tsp, TSP_DstInstr), true), NULL);
	else
		dx11c_set_blend(dx11_blend_get(false, 1, 0, true), NULL);

	dx11r_set_cull_mode((int)PPW(poly->isp, ISP_TSP_CullMode));
	dx11c_set_topology(dx11r.prepass_draw || (sortTriangles && !settings.pvr.Emulation.AlphaSortMode)
			? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	/* (a frame decoded on the GPU: a draw group by its arguments there) */
	if (pvrrc.gpu && !dx11r.gpu_sorted)
		dx11r_draw_gpu(listType, shadowPass != SHADOW_PASS_NONE, first, count);
	else
		ID3D11DeviceContext_DrawIndexed(dx11r.ctx, count, dx11r.index_base + first, dx11r.vertex_base);
}

void dx11r_draw_gpu(u32 listType, bool shadow, u32 first, u32 count)
{
	const dx11_ta_draw *gpu = dx11_ta_get_draw();
	const u32 stride = sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS);
	if (listType == ListType_Translucent)
		/* its records after its count, as many as it has vertices at most: those past the count draw nothing */
		dx11_ta_draw_records(first + 1, count);
	else
		ID3D11DeviceContext_DrawIndexedInstancedIndirect(dx11r.ctx, gpu->commands, (shadow ? gpu->shadowCommands : 0) + first * stride);
}

/* The polygons of a list that take shadows, drawn again where they are in a
 * modifier volume: see rend/shadows.h. */
static void draw_shadowed(u32 listType, const PolyParam *polys, u32 first, u32 last,
		const ScreenBounds *area, const D3D11_RECT *scissor)
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

static void draw_sorted(const SortTrigDrawParam *polys, u32 count, bool multipass)
{
	/* (a frame decoded on the GPU: its sorted indices are the decoder's
	 * buffer for them, and name the GPU's vertices) */
	const u32 base = pvrrc.gpu ? 0 : (u32)TA_LIST_USED(pvrrc.idx, u32);
	u32 i;
	if (pvrrc.gpu)
	{
		if (count == 0)
			return;
		dx11c_set_index_buffer(dx11_ta_get_draw()->sorted);
		dx11r.gpu_sorted = true;
	}
	for (i = 0; i < count; i++)
		draw_poly(ListType_Translucent, true, polys[i].ppid, base + polys[i].first, polys[i].count, SHADOW_PASS_NONE, NULL);

	if (multipass && settings.rend.TranslucentPolygonDepthMask)
	{
		/* Sorted translucent polygons are drawn without writing depth. Where
		 * a later render pass needs their depth (Cosmic Smash), it is written
		 * now, for those that do not have Z write disabled. */
		dx11c_set_input_layout(dx11r.position_layout);
		dx11c_set_vs(dx11_shaders_modvol_vs());
		dx11c_set_ps(dx11_shaders_modvol_ps());
		dx11c_set_blend(dx11_blend_get(false, 1, 0, false), NULL);
		dx11c_set_depth_stencil(depth_state(true, true, 6, DX11Stencil_None), 0);
		dx11c_set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		for (i = 0; i < count; i++)
		{
			const SortTrigDrawParam *param = &polys[i];
			D3D11_RECT scissorRect;
			if (param->count <= 2 || PPW(param->ppid->isp, ISP_TSP_ZWriteDis))
				continue;
			set_scissor(dx11r_set_tile_clip(param->ppid->tileclip, &scissorRect) == TILECLIP_OUTSIDE
					? &scissorRect : &dx11r.base_scissor);
			dx11r_set_cull_mode((int)PPW(param->ppid->isp, ISP_TSP_CullMode));
			ID3D11DeviceContext_DrawIndexed(dx11r.ctx, param->count, dx11r.index_base + base + param->first, dx11r.vertex_base);
		}
		dx11c_set_input_layout(dx11r.main_layout);
	}
	if (pvrrc.gpu)
	{
		dx11c_set_index_buffer(dx11_ta_get_draw()->indices);
		dx11r.gpu_sorted = false;
	}
}

/* The depth prepass of a sorted list: rend/sorter.h */
static void draw_prepass(const SortTrigDrawParam *polys, u32 count)
{
	/* (the colour pass's polygons that do not set their constants find what they found before) */
	PolyConstants saved;
	bool savedValid;
	/* (as draw_sorted(): of a frame decoded on the GPU, the indices are in the decoder's buffer for them) */
	const u32 base = pvrrc.gpu ? 0 : (u32)TA_LIST_USED(pvrrc.idx, u32);
	u32 i;
	if (count == 0)
		return;
	saved = uploaded.poly;
	savedValid = uploaded.poly_valid;
	if (pvrrc.gpu)
	{
		dx11c_set_index_buffer(dx11_ta_get_draw()->sorted);
		dx11r.gpu_sorted = true;
	}
	dx11r.prepass_draw = true;
	for (i = 0; i < count; i++)
		draw_poly(ListType_Translucent, true, polys[i].ppid, base + polys[i].first, polys[i].count, SHADOW_PASS_NONE, NULL);
	dx11r.prepass_draw = false;
	if (pvrrc.gpu)
	{
		dx11c_set_index_buffer(dx11_ta_get_draw()->indices);
		dx11r.gpu_sorted = false;
	}
	if (!savedValid)
		memset(&saved, 0, sizeof(saved));
	set_poly_constants(&saved);
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

static void draw_mod_vols(int first, int count, const RenderPass *previous_pass, const RenderPass *current_pass, int render_pass)
{
	const ModifierVolumeParam *mvo = TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam);
	const ModifierVolumeParam *params;
	const f32 *ga;
	ScreenBounds area;
	int mod_base = -1, cmv;

	if (count == 0 || !dx11r.have_modvols)
		return;

	/* (the modifier volumes' triangles are at vertex slot 1) */
	dx11c_set_input_layout(dx11r.modvol_layout);
	dx11c_set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	dx11c_set_vs(dx11_shaders_modvol_vs());
	dx11c_set_ps(dx11_shaders_modvol_ps());
	dx11c_set_blend(dx11_blend_get(false, 1, 0, false), NULL);
	set_scissor(&dx11r.base_scissor);

	params = &mvo[first];

	for (cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam *param = &params[cmv];
		u32 mvFirst, mvCount, mv_mode;
		D3D11_RECT clipRect;
		mv_clip_range_get(&pvrrc, mvo, param, &mvFirst, &mvCount);

		if (mvCount == 0)
			continue;

		mv_mode = PPW(param->isp, ISP_Modvol_DepthMode);

		/* clipped like a polygon, to the rectangle it was sent under if it asked to be */
		if (dx11r_set_tile_clip(param->tileclip, &clipRect) == TILECLIP_OUTSIDE)
			set_scissor(&clipRect);
		else
			set_scissor(&dx11r.base_scissor);

		if (mod_base == -1)
			mod_base = (int)mvFirst;

		dx11r_set_cull_mode((int)PPW(param->isp, ISP_Modvol_CullMode));
		if (!PPW(param->isp, ISP_Modvol_VolumeLast) && mv_mode > 0)
			dx11c_set_depth_stencil(depth_state(true, false, 4, DX11Stencil_Or), 2);	/* OR'ing (open volume or quad) */
		else
			dx11c_set_depth_stencil(depth_state(true, false, 4, DX11Stencil_Xor), 2);	/* XOR'ing (closed volume) */
		ID3D11DeviceContext_Draw(dx11r.ctx, mvCount * 3, dx11r.modvol_base + mvFirst * 3);

		if (mv_mode == 1 || mv_mode == 2)
		{
			/* Sum the area */
			dx11c_set_depth_stencil(depth_state(false, false, 7, mv_mode == 1 ? DX11Stencil_Inclusion : DX11Stencil_Exclusion), 1);
			ID3D11DeviceContext_Draw(dx11r.ctx, (mvFirst + mvCount - mod_base) * 3, dx11r.modvol_base + mod_base * 3);
			mod_base = -1;
		}
	}
	set_scissor(&dx11r.base_scissor);
	dx11c_set_input_layout(dx11r.main_layout);

	/* The polygons that take shadows, again, where they are in a volume
	 * (whose bounds the header pass has worked out for a frame decoded on
	 * the GPU, which has the volumes' triangles: TA_GPU_SHADOWS) */
	ga = pvrrc.gpu_shadow_area[render_pass < 10 ? render_pass : 0];
	if (pvrrc.gpu)
	{
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
		D3D11_RECT scissor;
		bounds_to_scissor(&area, &dx11r.matrices.viewport, rect);
		set_rect(&scissor, rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]);
		draw_shadowed(ListType_Opaque, TA_LIST_HEAD(pvrrc.global_param_op, PolyParam),
				previous_pass->op_count, current_pass->op_count, &area, &scissor);
		draw_shadowed(ListType_Punch_Through, TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam),
				previous_pass->pt_count, current_pass->pt_count, &area, &scissor);
		set_scissor(&dx11r.base_scissor);
	}

	/* What is left: darkened as it is. The background polygon is the
	 * rectangle drawn: the first four vertices' positions. */
	dx11c_set_input_layout(dx11r.position_layout);
	dx11c_set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx11c_set_vs(dx11_shaders_modvol_vs());
	dx11c_set_ps(dx11_shaders_modvol_ps());
	dx11c_set_blend(dx11_blend_alpha(), NULL);
	dx11c_set_depth_stencil(depth_state(false, false, 7, DX11Stencil_Final), 0x81);
	dx11r_set_cull_mode(0);
	ID3D11DeviceContext_DrawIndexed(dx11r.ctx, 4, dx11r.index_base, dx11r.vertex_base);
	dx11c_set_input_layout(dx11r.main_layout);
}

static void draw_frame(void)
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
		draw_mod_vols(previous_pass.mvo_count, current_pass->mvo_count - previous_pass.mvo_count, &previous_pass, current_pass, render_pass);
		if (current_pass->autosort)
		{
			u32 n;
			/* (a frame decoded on the GPU with a sorted pass was sorted by triangle: TA_GPU_SORT) */
			if (pvrrc.gpu)
			{
				const SortTrigDrawParam *prepass = ta_gpu_prepass(render_pass, &n);
				const SortTrigDrawParam *sorted;
				draw_prepass(prepass, n);
				sorted = ta_gpu_sorted(render_pass, &n);
				draw_sorted(sorted, n, render_pass < npasses - 1);
			}
			else if (!settings.pvr.Emulation.AlphaSortMode)
			{
				const SortTrigDrawParam *draws = SortPassDraws(&dx11r.prepass_passes, render_pass, &n);
				draw_prepass(draws, n);
				draws = SortPassDraws(&dx11r.sorted_passes, render_pass, &n);
				draw_sorted(draws, n, render_pass < npasses - 1);
			}
			else
			{
				const SortTrigDrawParam *draws = SortPassDraws(&dx11r.prepass_passes, render_pass, &n);
				draw_prepass(draws, n);
				SortPParams(previous_pass.tr_count, current_pass->tr_count - previous_pass.tr_count);
				draw_list(ListType_Translucent, true, tr, previous_pass.tr_count, current_pass->tr_count);
			}
		}
		else
			draw_list(ListType_Translucent, false, tr, previous_pass.tr_count, current_pass->tr_count);
		previous_pass = *current_pass;
	}
}

/* ---- The per-triangle renderer ---- */

static bool tri_init(void)
{
	return dx11r_init(&tri_kind);
}

static const renderer_ops dx11_renderer_ops =
{
	tri_init,
	dx11r_resize,
	dx11r_term,
	dx11r_process,
	dx11r_render,
	dx11r_keep_picture,
	dx11r_get_texture,
	dx11r_options_changed,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

const renderer_ops *rend_D3D11(void)
{
	return &dx11_renderer_ops;
}
