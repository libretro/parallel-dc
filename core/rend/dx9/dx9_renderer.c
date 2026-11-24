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
/* The Direct3D 9 renderer: translucent polygons sorted by triangle or by
 * strip (Direct3D 9 has nothing a per-pixel renderer could be made of).
 *
 * It is the Direct3D 11 renderer (dx11/dx11_renderer.h) step for step,
 * which is the Vulkan renderer's: the TA context and its render passes,
 * the shadows of rend/shadows.h, renders to textures that stay on the
 * card until the game touches them in video memory (rend/rtt_watch.h),
 * the picture kept across a new context or a new size
 * (rend/last_picture.h). The frame is a picture the frontend is handed as
 * it is (dx9c_set_frame()).
 *
 * Everything a frame needs is made when the renderer is set up or the
 * screen changes size: the shaders, all of them; the geometry buffers, as
 * large as the tile accelerator's lists can be; the depth buffer and the
 * screen's pictures. Textures come from the texture cache, and go back to
 * the context to be used again when the cache lets them go. */
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include "rend/d3d_system.h"
#include <math.h>
#include <string.h>
#include "dx9context_lr.h"
#include "dx9_texture.h"
#include "dx9_texdecoder.h"
#include "dx9_overlay.h"
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/sorter.h"
#include "rend/tileclip.h"
#include "rend/transform_matrix.h"
#include "rend/shadows.h"
#include "rend/depth_scale.h"
#include "rend/rtt_watch.h"
#include "rend/last_picture.h"
#include "rend/modvol_clip.h"
#include "rend/texconv.h"

extern int screen_width, screen_height;

#define PPW(w, f) TAW_GET(w, f)

/* (the vertices' colours are R G B A bytes; D3DCOLOR has them the other
 * way round, which the vertex shader undoes) */
static const D3DVERTEXELEMENT9 MainElements[] =
{
	{ 0, (WORD)offsetof(Vertex, x),       D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
	{ 0, (WORD)offsetof(Vertex, col),     D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
	{ 0, (WORD)offsetof(Vertex, vtx_spc), D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    1 },
	{ 0, (WORD)offsetof(Vertex, u),       D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
	D3DDECL_END()
};
static const D3DVERTEXELEMENT9 PositionElements[] =
{
	{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
	D3DDECL_END()
};

/* The screen: a picture for each of the frontend's sync indexes, the
 * size rendered at - and, for each, one to draw a second render to the
 * screen in, in a frame whose first one has been handed over already:
 * the two change places, and the one handed over stays the frontend's
 * until that sync index comes round again */
typedef struct dx9_screen_target
{
	IDirect3DTexture9 *texture;
	IDirect3DSurface9 *surface;
	u32 width;
	u32 height;
} dx9_screen_target;
typedef struct dx9_screen_slot
{
	dx9_screen_target target;
	dx9_screen_target extra;
} dx9_screen_slot;
/* (as many as dx9c_present_count() can say) */
#define DX9_SCREEN_SLOTS 32

/* The constants, uploaded when they differ from what the device has */
typedef struct dx9_poly_constants
{
	float clip_test[4];
	float params[4];		/* trilinear alpha, palette index, the depth prepass's least alpha (rend/sorter.h; 0 for the colour) */
} dx9_poly_constants;

#define SHADOW_PASS_NONE 0
#define SHADOW_PASS_LATER_WINS 1
#define SHADOW_PASS_EARLIER_WINS 2

static struct
{
	IDirect3DDevice9 *device;
	transform_matrix_t matrices;

	IDirect3DVertexDeclaration9 *main_decl;
	IDirect3DVertexDeclaration9 *position_decl;	/* the positions alone, of either kind of vertex */
	/* A render's vertices and modifier volumes in one buffer, and its
	 * indexes in another, each render's after the last one's, and the
	 * buffers started again (discarded) only when they are full: where
	 * this render's are in them */
	IDirect3DVertexBuffer9 *vertex_buffer;
	IDirect3DIndexBuffer9 *index_buffer;
	u32 vertex_buffer_size;
	u32 index_buffer_size;
	u32 vertex_buffer_used;
	u32 index_buffer_used;
	INT vertex_base;		/* (in vertices) */
	UINT vertex_count;
	UINT index_base;		/* (in indexes) */
	UINT mod_vol_base;		/* (in modifier volume vertices) */
	bool have_mod_vols;
	IDirect3DTexture9 *fog_texture;
	u8 fog_uploaded[256];
	bool fog_uploaded_valid;
	IDirect3DTexture9 *palette_texture;

	/* The depth and stencil, for the screen and renders to textures: as
	 * large as the largest of them so far */
	IDirect3DSurface9 *depth_surface;
	u32 depth_width;
	u32 depth_height;

	dx9_screen_slot screen_slots[DX9_SCREEN_SLOTS];
	u32 screen_slot_count;
	u32 screen_width;
	u32 screen_height;
	int screen_index;			/* the one being drawn */
	/* The last picture drawn to the screen, or the one kept from the
	 * context before (rend/last_picture.h) or the size before (Resize()),
	 * whose bytes are R G B A when it was kept */
	IDirect3DTexture9 *last_picture;
	IDirect3DTexture9 *restored_picture;
	bool restored_swap;

	/* A render to a texture: the texture, where in video memory */
	dx9_texture *rtt_texture;
	u32 rtt_addr;

	RECT base_scissor;

	dx9_poly_constants poly_uploaded;
	u32 poly_epoch;			/* the context's epoch they were uploaded in, or not (0) */

	/* The device taken by process() and kept for the render() after it */
	bool held_from_process;

	/* Per-triangle sort results */
	grow_buf_t sorted_passes;	/* SortPasses(): a SortList a pass */
	u32 sorted_index_count;		/* (their indices: after the frame's, in the index buffer, sorted there) */
	/* ...and the depth prepasses of the sorted lists (rend/sorter.h), their indexes after each pass's sorted ones */
	grow_buf_t prepass_passes;
	bool prepass_draw;	/* draw_poly() draws for the depth prepass */

	/* The framebuffer the game wrote itself, for a frame that is that */
	IDirect3DTexture9 *fb_texture;
	int fb_width;
	int fb_height;
} dx9;

/* Whether the render starts from the last picture: it covers only part
 * of the screen, and the game has drawn to this framebuffer lately */
static bool keeps_last_picture(void)
{
	return !pvrrc.clearFramebuffer && transform_matrix_clipped(&dx9.matrices)
			&& (dx9.last_picture != NULL || dx9.restored_picture != NULL);
}

static void draw(D3DPRIMITIVETYPE type, UINT first, UINT count)
{
	IDirect3DDevice9_DrawIndexedPrimitive(dx9.device, type, dx9.vertex_base, 0, dx9.vertex_count, dx9.index_base + first,
			type == D3DPT_TRIANGLESTRIP ? count - 2 : count / 3);
}

/* As much as the tile accelerator's lists can hold of a frame (ta_ctx.h),
 * and the sorted triangles' indexes, three for each index at most */
static void maxGeometry(u32 *vertexBytes, u32 *indexBytes)
{
	const u32 ratio = ta_data_size() / TA_DATA_SIZE;
	const u32 scale = ratio > 1 ? ratio : 1;
	*vertexBytes = 4 * 1024 * 1024 * scale + 16384 * scale * (u32)sizeof(ModTriangle) + 4096;
	*indexBytes = 120 * 1024 * scale * 4 * (u32)sizeof(u32) + 4096;
}

static bool make_buffers(u32 vertexBytes, u32 indexBytes)
{
	DX9_RELEASE(dx9.vertex_buffer);
	DX9_RELEASE(dx9.index_buffer);
	dx9.vertex_buffer_size = dx9.index_buffer_size = 0;
	dx9.vertex_buffer_used = dx9.index_buffer_used = 0;
	if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dx9.device, vertexBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
				D3DPOOL_DEFAULT, &dx9.vertex_buffer, NULL)))
		dx9.vertex_buffer = NULL;
	else if (FAILED(IDirect3DDevice9_CreateIndexBuffer(dx9.device, indexBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
				D3DFMT_INDEX32, D3DPOOL_DEFAULT, &dx9.index_buffer, NULL)))
		dx9.index_buffer = NULL;
	if (dx9.vertex_buffer == NULL || dx9.index_buffer == NULL)
	{
		WARN_LOG(RENDERER, "DX9: buffer creation failed (%d and %d bytes)\n", vertexBytes, indexBytes);
		DX9_RELEASE(dx9.vertex_buffer);
		DX9_RELEASE(dx9.index_buffer);
		return false;
	}
	dx9.vertex_buffer_size = vertexBytes;
	dx9.index_buffer_size = indexBytes;
	return true;
}

static void screen_target_free(dx9_screen_target *target)
{
	DX9_RELEASE(target->surface);
	DX9_RELEASE(target->texture);
	target->width = target->height = 0;
}

static bool ensure_depth(u32 width, u32 height);
static bool make_screen_target(dx9_screen_target *target, u32 width, u32 height);

static bool dx9_init(void)
{
	u32 vertexBytes, indexBytes, i;
	bool success;
	/* (the passes' sorted draws: room before the first frame) */
	SortPassesReserve(&dx9.sorted_passes);
	SortPassesReserve(&dx9.prepass_passes);
	transform_matrix_init(&dx9.matrices, false);
	dx9.device = dx9c.device;
	if (dx9.device == NULL)
	{
		WARN_LOG(RENDERER, "DX9: no device\n");
		return false;
	}

	dx9c_lock();
	success = SUCCEEDED(IDirect3DDevice9_CreateVertexDeclaration(dx9.device, MainElements, &dx9.main_decl))
			&& SUCCEEDED(IDirect3DDevice9_CreateVertexDeclaration(dx9.device, PositionElements, &dx9.position_decl));
	maxGeometry(&vertexBytes, &indexBytes);
	success = success && make_buffers(vertexBytes, indexBytes);

	/* The fog table, two rows of 128: see check_fog_texture() */
	dx9.fog_texture = dx9c_make_texture(128, 2, 1, dx9c_write_usage(), D3DFMT_A8);
	dx9.fog_uploaded_valid = false;
	/* The palette, a row of 1024 colours */
	dx9.palette_texture = dx9c_make_texture(1024, 1, 1, dx9c_write_usage(), D3DFMT_A8R8G8B8);
	palette_updated = true;
	success = success && dx9.fog_texture != NULL && dx9.palette_texture != NULL;

	DX9_RELEASE(dx9.last_picture);
	DX9_RELEASE(dx9.restored_picture);
	dx9.screen_width = dx9.screen_height = 0;
	dx9.poly_epoch = 0;
	dx9.held_from_process = false;
	/* textures drawn on the GPU (with the core option for it), and the
	 * textures themselves, all of them now (rend/tex_pool.h) */
	dx9_texdec_init();
	dx9_pool_init();
	/* ...and the cache's room for them (rend/tex_map.h) */
	dx9_texcache_reserve_for(dx9_pool_made());
	dx9_textures_attach(dx9_texdec_ready(), true);
	/* The depth buffer and the screen's pictures now, at the size the
	 * screen is, and not when the first frame comes */
	if (success && screen_width > 0 && screen_height > 0)
	{
		success = ensure_depth(screen_width, screen_height);
		dx9.screen_width = screen_width;
		dx9.screen_height = screen_height;
		dx9.screen_slot_count = dx9c_present_count();
		for (i = 0; i < dx9.screen_slot_count; i++)
			success = success && make_screen_target(&dx9.screen_slots[i].target, dx9.screen_width, dx9.screen_height);
	}
	dx9c_unlock();
	if (!success)
	{
		WARN_LOG(RENDERER, "DX9 renderer initialization failed\n");
		return false;
	}
	return true;
}

static void dx9_term(void)
{
	u32 i;
	SortPassesFree(&dx9.sorted_passes);
	SortPassesFree(&dx9.prepass_passes);
	dx9c_lock();
	if (dx9.device != NULL)
		dx9c_set_frame(NULL);
	dx9_rtt_term();
	ta_set_modvol_clip(false);
	dx9_texcache_clear();
	/* (what the cache's textures had is back in the pool by now) */
	dx9_textures_attach(false, false);
	dx9_texcache_free();
	dx9_pool_term();
	dx9_texdec_term();
	dx9_overlay_term();
	DX9_RELEASE(dx9.fb_texture);
	DX9_RELEASE(dx9.restored_picture);
	DX9_RELEASE(dx9.last_picture);
	for (i = 0; i < DX9_SCREEN_SLOTS; i++)
	{
		screen_target_free(&dx9.screen_slots[i].target);
		screen_target_free(&dx9.screen_slots[i].extra);
	}
	dx9.screen_slot_count = 0;
	DX9_RELEASE(dx9.depth_surface);
	dx9.depth_width = dx9.depth_height = 0;
	DX9_RELEASE(dx9.palette_texture);
	DX9_RELEASE(dx9.fog_texture);
	DX9_RELEASE(dx9.vertex_buffer);
	DX9_RELEASE(dx9.index_buffer);
	dx9.vertex_buffer_size = dx9.index_buffer_size = 0;
	DX9_RELEASE(dx9.position_decl);
	DX9_RELEASE(dx9.main_decl);
	dx9.device = NULL;
	dx9c_unlock();
}

/* The size rendered at has changed, and the device is the same: the
 * screen's pictures are made again at the new size when they are next
 * drawn into, and the last picture goes with them - ensure_screen() */
static void dx9_resize(int w, int h)
{
	(void)w;
	(void)h;
}

static u64 dx9_get_texture(u32 tsp, u32 tcw, int area)
{
	dx9_texture *tf = dx9_texcache_get(tsp, tcw, area);

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
	int width, height, y;
	D3DLOCKED_RECT rect;
	const u8 *pixels = last_picture(&width, &height);
	if (pixels == NULL)
		return;
	dx9c_recycle(&dx9.restored_picture);
	dx9.restored_picture = dx9c_make_texture(width, height, 1, dx9c_write_usage(), D3DFMT_A8R8G8B8);
	if (dx9.restored_picture == NULL
			|| FAILED(IDirect3DTexture9_LockRect(dx9.restored_picture, 0, &rect, NULL, dx9c_write_lock_flags())))
	{
		DX9_RELEASE(dx9.restored_picture);
		return;
	}
	for (y = 0; y < height; y++)
		memcpy((u8 *)rect.pBits + (size_t)y * rect.Pitch, pixels + (size_t)y * width * 4, (size_t)width * 4);
	IDirect3DTexture9_UnlockRect(dx9.restored_picture, 0);
	dx9.restored_swap = true;
}

/* Uploaded when it differs from what the card has: see UpdateFogTexture() in gles/gles.c */
static void check_fog_texture(void)
{
	u8 texData[256];
	D3DLOCKED_RECT rect;
	MakeFogTexture(texData);
	if (dx9.fog_uploaded_valid && !memcmp(dx9.fog_uploaded, texData, sizeof(dx9.fog_uploaded)))
		return;
	if (FAILED(IDirect3DTexture9_LockRect(dx9.fog_texture, 0, &rect, NULL, dx9c_write_lock_flags())))
		return;
	memcpy(rect.pBits, texData, 128);
	memcpy((u8 *)rect.pBits + rect.Pitch, texData + 128, 128);
	IDirect3DTexture9_UnlockRect(dx9.fog_texture, 0);
	memcpy(dx9.fog_uploaded, texData, sizeof(dx9.fog_uploaded));
	dx9.fog_uploaded_valid = true;
}

static void check_palette_texture(void)
{
	D3DLOCKED_RECT rect;
	if (!palette_updated)
		return;
	if (FAILED(IDirect3DTexture9_LockRect(dx9.palette_texture, 0, &rect, NULL, dx9c_write_lock_flags())))
		return;
	/* (R G B A bytes, which the shader reads the other way round) */
	memcpy(rect.pBits, palette32_ram, 1024 * 4);
	IDirect3DTexture9_UnlockRect(dx9.palette_texture, 0);
	palette_updated = false;
}

/* ---- A frame the game wrote to the framebuffer itself ---- */

/* Converted straight into the texture, where it is locked */
static bool upload_framebuffer(void)
{
	FramebufferRead fb;
	D3DLOCKED_RECT rect;
	if (FB_R_SIZE.fb_x_size == 0 || FB_R_SIZE.fb_y_size == 0)
		return false;

	ReadFramebufferGeometry(&fb);
	if (dx9.fb_texture == NULL || fb.width != dx9.fb_width || fb.height != dx9.fb_height)
	{
		/* (written by the processor every frame: one the card reads where it
		 * is written, with no copy of it made on the way) */
		dx9c_recycle(&dx9.fb_texture);
		dx9.fb_texture = dx9c_make_texture(fb.width, fb.height, 1, dx9c_write_usage(), D3DFMT_A8R8G8B8);
		if (dx9.fb_texture == NULL)
			return false;
		dx9.fb_width = fb.width;
		dx9.fb_height = fb.height;
	}
	if (FAILED(IDirect3DTexture9_LockRect(dx9.fb_texture, 0, &rect, NULL, dx9c_write_lock_flags())))
		return false;
	ReadFramebufferTo(&fb, (u8 *)rect.pBits, (size_t)rect.Pitch);
	IDirect3DTexture9_UnlockRect(dx9.fb_texture, 0);
	return true;
}

static bool process_locked(TA_context *ctx)
{
	bool result;
	if (!ctx->rend.isRTT)
		dx9_overlay_prepare();
	if (!ctx->rend.isRTT && !ctx->rend.isRenderFramebuffer)
		check_kept_picture();

	if (ctx->rend.isRenderFramebuffer)
		return upload_framebuffer();

	if (KillTex)
	{
		/* what was rendered to a texture and is not in video memory yet goes there before its picture does */
		rtt_watch_flush();
		dx9_texcache_clear();
	}

	/* the modifier volumes' triangles far off the screen cut before the GPU clips them (rend/modvol_clip.h) */
	ta_set_modvol_clip(true);
	result = ta_parse_vdrc(ctx);
	/* the textures the frame's polygons asked for that are drawn on the GPU, before its scene */
	dx9_texdec_flush();

	dx9_texcache_cleanup();

	if (result)
	{
		check_fog_texture();
		check_palette_texture();
	}
	return result;
}

static bool dx9_process(TA_context *ctx)
{
	bool keep, result;
	if (dx9.device == NULL)
		return false;
	/* The device is taken here and kept for the render() that follows,
	 * which is then one lock with this. Except when a render to the screen
	 * is handed over to the other thread when this is done (threaded
	 * rendering): the frontend is waited for before the screen's picture
	 * is drawn into, and never with the device taken - and not here,
	 * which would keep the emulation waiting too. */
	keep = ctx->rend.isRTT || !settings.rend.ThreadedRendering;
	if (keep && !ctx->rend.isRTT)
		dx9c_wait_present_index();
	dx9c_lock();
	result = process_locked(ctx);
	dx9.held_from_process = keep && result;
	if (!dx9.held_from_process)
		dx9c_unlock();
	return result;
}

/* ---- What is set ---- */

/* ISP's culling mode 0 or 1: none; 2: counter-clockwise faces; 3: clockwise ones */
static void set_cull_mode(int mode)
{
	dx9c_set_render_state(D3DRS_CULLMODE, mode == 3 ? D3DCULL_CW : mode == 2 ? D3DCULL_CCW : D3DCULL_NONE);
}

static void set_poly_constants(const dx9_poly_constants *constants)
{
	if (dx9.poly_epoch == dx9c.epoch && !memcmp(constants, &dx9.poly_uploaded, sizeof(*constants)))
		return;
	IDirect3DDevice9_SetPixelShaderConstantF(dx9.device, DX9PC_ClipTest, constants->clip_test, 2);
	dx9.poly_uploaded = *constants;
	dx9.poly_epoch = dx9c.epoch;
}

/* ---- Clipping ---- */

static int set_tile_clip(u32 val, RECT *clipRect)
{
	int rect[4] = { 0, 0, 0, 0 };
	const int clipmode = tile_clip(val, &dx9.matrices.viewport, rect);
	if (clipmode == TILECLIP_OUTSIDE)
		intersect_clip_rect(rect, dx9.base_scissor.left, dx9.base_scissor.top,
				dx9.base_scissor.right - dx9.base_scissor.left, dx9.base_scissor.bottom - dx9.base_scissor.top);
	clipRect->left = rect[0];
	clipRect->top = rect[1];
	clipRect->right = rect[0] + rect[2];
	clipRect->bottom = rect[1] + rect[3];

	return clipmode;
}

static void set_rect(RECT *r, LONG left, LONG top, LONG right, LONG bottom)
{
	r->left = left;
	r->top = top;
	r->right = right;
	r->bottom = bottom;
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
			&& !transform_matrix_clipped(&dx9.matrices);
	if (!wide_screen_on)
	{
		if (pvrrc.isRenderFramebuffer)
			set_rect(&dx9.base_scissor, 0, 0, 640, 480);
		else
		{
			const xform *scissor = &dx9.matrices.scissor;
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
			set_rect(&dx9.base_scissor, x, y, x + round_at_least_0(width), y + round_at_least_0(height));
		}
	}
	else
		set_rect(&dx9.base_scissor, 0, 0, (LONG)screen_width, (LONG)screen_height);
}

/* Direct3D takes a flat shaded triangle's colours from its first vertex.
 * On Dreamcast the last vertex is the provoking one so it is copied onto the
 * first (as in the Vulkan renderers). */
static void set_provoking_vertex(const PolyParam *list, int used)
{
	Vertex *verts = TA_LIST_HEAD(pvrrc.verts, Vertex);
	const u32 *idx = TA_LIST_HEAD(pvrrc.idx, u32);
	int i;
	u32 n;
	for (i = 0; i < used; i++)
	{
		const PolyParam *pp = &list[i];
		if (!PPW(pp->pcw, PCW_Gouraud) && pp->count > 2)
		{
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
	set_provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_op, PolyParam), TA_LIST_USED(pvrrc.global_param_op, PolyParam));
	set_provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam), TA_LIST_USED(pvrrc.global_param_pt, PolyParam));
	set_provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam), TA_LIST_USED(pvrrc.global_param_tr, PolyParam));
}

/* ---- Buffers ---- */

/* Each at a multiple of its size, as the draws count them; after what
 * the renders before this one have put there, and not over any of it -
 * the buffer started again (discarded) when it is full */
static u32 lay(u32 start, u32 vertexBytes, u32 modVolBytes, u32 *vertices, u32 *modVols)
{
	const u32 modVolSize = sizeof(float) * 3;
	*vertices = (start + sizeof(Vertex) - 1) / sizeof(Vertex) * sizeof(Vertex);
	*modVols = (*vertices + vertexBytes + modVolSize - 1) / modVolSize * modVolSize;
	return *modVols + modVolBytes;
}

static bool upload_geometry(void)
{
	const u32 vertexBytes = TA_LIST_BYTES(pvrrc.verts);
	const u32 idxBytes = TA_LIST_BYTES(pvrrc.idx);
	/* (room for the sorted passes' triangles, sorted into it: SortPassesTo()) */
	const u32 indexBytes = idxBytes + SortPassesRoom() * sizeof(u32);
	/* (the volumes' triangles with those far off the screen cut, when the frame has them: rend/modvol_clip.h) */
	const bool clipped = TA_LIST_USED(pvrrc.mv_clip, ModTriangle) != 0;
	const ModTriangle *modVolList = clipped ? TA_LIST_HEAD(pvrrc.mv_clip, ModTriangle) : TA_LIST_HEAD(pvrrc.modtrig, ModTriangle);
	const u32 modVolBytes = clipped ? TA_LIST_BYTES(pvrrc.mv_clip) : TA_LIST_BYTES(pvrrc.modtrig);
	const u32 modVolSize = sizeof(float) * 3;
	u32 vertices, modVols, vertexEnd, indexStart;
	DWORD vertexFlags = D3DLOCK_NOOVERWRITE, indexFlags = D3DLOCK_NOOVERWRITE;
	void *data;
	u32 used;

	vertexEnd = lay(dx9.vertex_buffer_used, vertexBytes, modVolBytes, &vertices, &modVols);
	if (vertexEnd > dx9.vertex_buffer_size)
	{
		vertexFlags = D3DLOCK_DISCARD;
		vertexEnd = lay(0, vertexBytes, modVolBytes, &vertices, &modVols);
	}
	indexStart = dx9.index_buffer_used;
	if (indexStart + indexBytes > dx9.index_buffer_size)
	{
		indexFlags = D3DLOCK_DISCARD;
		indexStart = 0;
	}
	if (vertexEnd > dx9.vertex_buffer_size || indexBytes > dx9.index_buffer_size)
	{
		/* (more than the tile accelerator can make of a frame: it cannot
		 * happen, but if it does, larger buffers once) */
		WARN_LOG(RENDERER, "DX9: a frame of %d bytes of vertices and %d of indexes: buffers made larger\n", vertexEnd, indexBytes);
		if (!make_buffers((vertexEnd > dx9.vertex_buffer_size ? vertexEnd : dx9.vertex_buffer_size) * 2,
				(indexBytes > dx9.index_buffer_size ? indexBytes : dx9.index_buffer_size) * 2))
			return false;
		vertexFlags = indexFlags = D3DLOCK_DISCARD;
		vertexEnd = lay(0, vertexBytes, modVolBytes, &vertices, &modVols);
		indexStart = 0;
	}

	if (vertexEnd > vertices)
	{
		if (FAILED(IDirect3DVertexBuffer9_Lock(dx9.vertex_buffer, vertices, vertexEnd - vertices, &data, vertexFlags)))
			return false;
		memcpy(data, TA_LIST_HEAD(pvrrc.verts, Vertex), vertexBytes);
		if (modVolBytes != 0)
			memcpy((u8 *)data + (modVols - vertices), modVolList, modVolBytes);
		IDirect3DVertexBuffer9_Unlock(dx9.vertex_buffer);
	}
	if (indexBytes != 0)
	{
		if (FAILED(IDirect3DIndexBuffer9_Lock(dx9.index_buffer, indexStart, indexBytes, &data, indexFlags)))
			return false;
		memcpy(data, TA_LIST_HEAD(pvrrc.idx, u32), idxBytes);
		dx9.sorted_index_count = SortPassesTo(&dx9.sorted_passes, &dx9.prepass_passes,
				(u32 *)((u8 *)data + idxBytes), true, true);
		IDirect3DIndexBuffer9_Unlock(dx9.index_buffer);
	}
	else
		dx9.sorted_index_count = SortPassesTo(&dx9.sorted_passes, &dx9.prepass_passes, NULL, true, true);
	dx9.vertex_buffer_used = vertexEnd;
	dx9.index_buffer_used = indexStart + idxBytes + dx9.sorted_index_count * sizeof(u32);

	dx9.vertex_base = (INT)(vertices / sizeof(Vertex));
	used = TA_LIST_USED(pvrrc.verts, Vertex);
	dx9.vertex_count = used > 1 ? used : 1;
	dx9.index_base = indexStart / sizeof(u32);
	dx9.mod_vol_base = modVols / modVolSize;
	dx9.have_mod_vols = modVolBytes != 0;
	return true;
}

static void setup_frame(void)
{
	/* The normal matrix, made for a picture whose first line is the top
	 * one and whose clip space has y going up: the other way up from the
	 * Vulkan renderers', whose matrices these are, and nothing else */
	const xform *normal = &dx9.matrices.normal;
	float vtx[2][4];
	float depthScale[4];
	float frag[5][4];
	const u8 *fog_colvert_bgra = (const u8 *)&FOG_COL_VERT;
	const u8 *fog_colram_bgra = (const u8 *)&FOG_COL_RAM;
	const u8 *fog_density = (const u8 *)&FOG_DENSITY;
	float fog_den_mant;
	s32 fog_den_exp;
	int i;

	vtx[0][0] = normal->sx;
	vtx[0][1] = -normal->sy;
	vtx[0][2] = normal->sz;
	vtx[0][3] = 0.f;
	vtx[1][0] = normal->tx;
	vtx[1][1] = -normal->ty;
	vtx[1][2] = normal->tz;
	vtx[1][3] = 0.f;
	IDirect3DDevice9_SetVertexShaderConstantF(dx9.device, DX9VC_NormalScale, &vtx[0][0], 2);
	depthScale[0] = pvr_depth_scale(pvrrc.fZ_max);
	depthScale[1] = 1.f / normal->sz;
	depthScale[2] = 0.f;
	depthScale[3] = 0.f;
	IDirect3DDevice9_SetVertexShaderConstantF(dx9.device, DX9VC_DepthScale, depthScale, 1);

	memset(frag, 0, sizeof(frag));
	/* the clamping limits */
	for (i = 0; i < 3; i++)
	{
		frag[DX9PC_ColorClampMin][i] = ((pvrrc.fog_clamp_min >> (16 - 8 * i)) & 0xFF) / 255.0f;
		frag[DX9PC_ColorClampMax][i] = ((pvrrc.fog_clamp_max >> (16 - 8 * i)) & 0xFF) / 255.0f;
	}
	frag[DX9PC_ColorClampMin][3] = ((pvrrc.fog_clamp_min >> 24) & 0xFF) / 255.0f;
	frag[DX9PC_ColorClampMax][3] = ((pvrrc.fog_clamp_max >> 24) & 0xFF) / 255.0f;
	/* VERT and RAM fog color constants */
	for (i = 0; i < 3; i++)
	{
		frag[DX9PC_FogColVert][i] = fog_colvert_bgra[2 - i] / 255.0f;
		frag[DX9PC_FogColRam][i] = fog_colram_bgra[2 - i] / 255.0f;
	}
	/* half a step under the reference: see the same in gles/gles.c */
	frag[DX9PC_FrameParams][0] = ((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f;
	/* Fog density constant */
	fog_den_mant = fog_density[1] / 128.0f;  /* bit 7 -> x. bit, so [6:0] -> fraction -> /128 */
	fog_den_exp = (s8)fog_density[0];
	frag[DX9PC_FrameParams][1] = fog_den_mant * (float)pow(2.0f, (float)fog_den_exp);
	frag[DX9PC_FrameParams][2] = FPU_SHAD_SCALE.scale_factor / 256.f;
	IDirect3DDevice9_SetPixelShaderConstantF(dx9.device, DX9PC_ColorClampMin, &frag[0][0], 5);

	dx9c_set_decl(dx9.main_decl);
	dx9c_set_stream(dx9.vertex_buffer, sizeof(Vertex));
	dx9c_set_indices(dx9.index_buffer);
	dx9c_set_texture(DX9S_Palette, DX9_BASE(dx9.palette_texture));
	dx9c_set_texture(DX9S_Fog, DX9_BASE(dx9.fog_texture));
	dx9c_set_vs(dx9_shaders_vs(DX9VS_Main));
}

static bool ensure_depth(u32 width, u32 height)
{
	HRESULT hr;
	if (dx9.depth_surface != NULL && dx9.depth_width >= width && dx9.depth_height >= height)
		return true;
	if (width < dx9.depth_width)
		width = dx9.depth_width;
	if (height < dx9.depth_height)
		height = dx9.depth_height;
	DX9_RELEASE(dx9.depth_surface);
	dx9.depth_width = dx9.depth_height = 0;
	hr = IDirect3DDevice9_CreateDepthStencilSurface(dx9.device, width, height, dx9c.depth_format, D3DMULTISAMPLE_NONE, 0,
			TRUE, &dx9.depth_surface, NULL);
	if (FAILED(hr))
	{
		WARN_LOG(RENDERER, "DX9: depth buffer creation failed (%x, %d x %d)\n", (unsigned)hr, width, height);
		dx9.depth_surface = NULL;
		return false;
	}
	dx9.depth_width = width;
	dx9.depth_height = height;
	return true;
}

/* ---- The screen ---- */

static bool make_screen_target(dx9_screen_target *target, u32 width, u32 height)
{
	screen_target_free(target);
	target->texture = dx9c_make_texture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8);
	if (target->texture == NULL || FAILED(IDirect3DTexture9_GetSurfaceLevel(target->texture, 0, &target->surface)))
	{
		target->surface = NULL;
		screen_target_free(target);
		return false;
	}
	target->width = width;
	target->height = height;
	IDirect3DDevice9_ColorFill(dx9.device, target->surface, NULL, D3DCOLOR_ARGB(255, 0, 0, 0));
	return true;
}

static bool ensure_screen(u32 width, u32 height)
{
	dx9_screen_slot *slot;
	unsigned count;
	if (width != dx9.screen_width || height != dx9.screen_height)
	{
		/* Another size. The picture with the last frame goes on as the
		 * picture the next render starts from, at whatever size the screen
		 * now is: nothing is read back or loaded again. The pictures of the
		 * old size are made again each when it is next drawn into, after
		 * the frontend is done with it. */
		if (dx9.last_picture != NULL)
		{
			DX9_ASSIGN(dx9.restored_picture, dx9.last_picture);
			dx9.restored_swap = false;
		}
		DX9_RELEASE(dx9.last_picture);
		dx9.screen_width = width;
		dx9.screen_height = height;
	}
	count = dx9c_present_count();
	if (dx9.screen_slot_count < count)
		dx9.screen_slot_count = count;
	slot = &dx9.screen_slots[dx9.screen_index];
	if (slot->target.texture != NULL && (slot->target.width != width || slot->target.height != height))
		screen_target_free(&slot->target);
	if (slot->extra.texture != NULL && (slot->extra.width != width || slot->extra.height != height))
		screen_target_free(&slot->extra);
	if (slot->target.texture == NULL)
	{
		dx9c_end_scene();
		if (!make_screen_target(&slot->target, width, height))
			return false;
	}
	if (dx9c_is_handed_over(slot->target.texture))
	{
		/* A render to the screen after one this frame has handed over
		 * already: this one goes to the slot's other picture, and the one
		 * handed over stays as it is until the frontend has shown it */
		dx9_screen_target swap;
		if (slot->extra.texture == NULL)
		{
			dx9c_end_scene();
			if (!make_screen_target(&slot->extra, width, height))
				return false;
		}
		swap = slot->target;
		slot->target = slot->extra;
		slot->extra = swap;
	}
	return ensure_depth(width, height);
}

static void draw_last_picture(void)
{
	IDirect3DTexture9 *picture = dx9.last_picture != NULL ? dx9.last_picture : dx9.restored_picture;
	if (picture == NULL)
		return;
	/* (the picture kept from another context can be of another size) */
	dx9c_draw_quad(picture, dx9.last_picture == NULL, dx9.last_picture != NULL ? false : dx9.restored_swap);
}

static bool begin_screen(void)
{
	dx9_screen_target *target;
	dx9.screen_index = (int)dx9c_present_index();
	if (!ensure_screen(screen_width, screen_height))
		return false;
	target = &dx9.screen_slots[dx9.screen_index].target;

	transform_matrix_calc(&dx9.matrices, &pvrrc);
	dx9c_set_target(target->surface, dx9.depth_surface, dx9.screen_width, dx9.screen_height);
	if (!dx9c_begin_scene())
		return false;

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour; any other starts black - unless the render covers only part
	 * of the screen, which leaves the rest as it was: the last picture,
	 * which is in this picture already if the last frame went to it.
	 * (Only for such a render: one that covers the screen pays nothing.) */
	if (keeps_last_picture())
	{
		dx9c_clear(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0);
		if (dx9.last_picture != target->texture)
		{
			if (dx9.last_picture == NULL)
				dx9c_clear(D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0));
			draw_last_picture();
		}
	}
	else
	{
		D3DCOLOR color = D3DCOLOR_ARGB(255, 0, 0, 0);
		if (pvrrc.clearFramebuffer)
			color = D3DCOLOR_ARGB(255, VO_BORDER_COL.Red, VO_BORDER_COL.Green, VO_BORDER_COL.Blue);
		dx9c_clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, color);
	}
	set_base_scissor();
	return true;
}

static void end_screen(void)
{
	dx9_screen_target *target = &dx9.screen_slots[dx9.screen_index].target;
	/* what goes over the picture, onto it (with the depth test off) */
	if (!dx9_overlay_empty())
		dx9_overlay_draw(dx9.screen_width, dx9.screen_height);

	DX9_ASSIGN(dx9.last_picture, target->texture);
	DX9_RELEASE(dx9.restored_picture);
	dx9c_set_frame(target->texture);
}

static void dx9_keep_picture(void)
{
	u8 *pixels;
	if (dx9.last_picture == NULL || dx9.screen_width == 0 || dx9.screen_height == 0)
		return;
	pixels = last_picture_keep(dx9.screen_width, dx9.screen_height);
	if (pixels != NULL && !dx9_read_picture(dx9.last_picture, dx9.screen_width, dx9.screen_height, pixels))
		last_picture_drop();
}

/* ---- Renders to textures ---- */

static bool begin_rtt(void)
{
	/* the frame from its first pixel, as far as the pixel clip goes (see transform_matrix.h) */
	const u32 origWidth = pvrrc.fb_X_CLIP.max + 1;
	const u32 origHeight = pvrrc.fb_Y_CLIP.max + 1;
	const u32 clipX = pvrrc.fb_X_CLIP.min, clipY = pvrrc.fb_Y_CLIP.min;
	const u32 clipWidth = origWidth - clipX, clipHeight = origHeight - clipY;
	const u32 scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	u32 upscaledWidth = origWidth;
	u32 upscaledHeight = origHeight;
	u32 heightPow2 = 8, widthPow2 = 8;
	u32 tcw, tsp = 0, u, v;
	dx9_texture *rtt;

	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d @ %08x\n", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max, FB_W_SOF1 & VRAM_MASK);
	transform_matrix_calc(&dx9.matrices, &pvrrc);

	dx9.rtt_addr = FB_W_SOF1 & VRAM_MASK;
	while (heightPow2 < upscaledHeight)
		heightPow2 *= 2;
	while (widthPow2 < upscaledWidth)
		widthPow2 *= 2;
	upscaledWidth *= scale;
	upscaledHeight *= scale;
	widthPow2 *= scale;
	heightPow2 *= scale;

	dx9c_end_scene();
	if (!ensure_depth(widthPow2, heightPow2))
		return false;

	/* what an earlier render left waiting in this memory, while its picture is still what it was: see rtt_watch.h */
	dx9_rtt_supersede(dx9.rtt_addr, clipX, clipY, clipWidth, clipHeight);
	/* TexAddr, ScanOrder 1 */
	tcw = TAW_SET(TAW_SET(0, TCW_TexAddr, dx9.rtt_addr >> 3), TCW_ScanOrder, 1);
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

	rtt = dx9.rtt_texture = dx9_texcache_get(tsp, tcw, 0);
	if (rtt->texture == NULL)
		texcache_create(&rtt->base);

	if (rtt->spare != NULL)
	{
		D3DSURFACE_DESC desc;
		if (FAILED(IDirect3DTexture9_GetLevelDesc(rtt->spare, 0, &desc)) || desc.Width != widthPow2 || desc.Height != heightPow2
				|| desc.Format != D3DFMT_A8R8G8B8 || (desc.Usage & D3DUSAGE_RENDERTARGET) == 0
				|| IDirect3DTexture9_GetLevelCount(rtt->spare) != 1)
		{
			dx9_texture_release_spare(rtt);
			DX9_RELEASE(rtt->spare_surface);
			dx9c_recycle(&rtt->spare);
		}
	}
	/* (one of the pool's, made at the start, where it has one of this shape) */
	if (rtt->spare == NULL && !dx9_texture_take_spare(rtt, widthPow2, heightPow2))
		rtt->spare = dx9c_make_texture(widthPow2, heightPow2, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8);
	if (rtt->spare == NULL)
		return false;
	if (rtt->spare_surface == NULL && FAILED(IDirect3DTexture9_GetSurfaceLevel(rtt->spare, 0, &rtt->spare_surface)))
	{
		rtt->spare_surface = NULL;
		WARN_LOG(RENDERER, "DX9: render to texture target creation failed\n");
		return false;
	}

	/* (a texture this render draws with is not to be bound while it is
	 * drawn into) */
	dx9c_set_texture(DX9S_Texture, NULL);
	dx9c_set_target(rtt->spare_surface, dx9.depth_surface, upscaledWidth, upscaledHeight);
	if (!dx9c_begin_scene())
		return false;
	dx9c_clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, D3DCOLOR_ARGB(255, 0, 0, 0));
	/* only the pixel clip is written */
	set_rect(&dx9.base_scissor, (LONG)(clipX * scale), (LONG)(clipY * scale),
			(LONG)((clipX + clipWidth) * scale), (LONG)((clipY + clipHeight) * scale));
	return true;
}

static void end_rtt(void)
{
	dx9_texture *rtt = dx9.rtt_texture;
	/* (left bound: whatever binds the texture to be read from binds
	 * another target first) */
	dx9_texture_swap_with_spare(rtt);

	rtt->base.dirty = 0;
	rtt->base.stride_read = 0;	/* (rendered, not read) */
	rtt->base.made_type = 0;
	libCore_vramlock_Lock(rtt->base.sa_tex, rtt->base.sa + rtt->base.size - 1, &rtt->base);
	/* video memory gets it when something first touches it there */
	dx9_rtt_watch(rtt, dx9.rtt_addr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
	dx9.rtt_texture = NULL;
}

/* Into the screen's picture, where the game's 640x480 is, as the OpenGL
 * renderers draw it */
static void draw_framebuffer(void)
{
	dx9_screen_target *target;
	const xform *viewport;
	float x0, x1, y0, y1;
	dx9.screen_index = (int)dx9c_present_index();
	if (!ensure_screen(screen_width, screen_height))
		return;
	target = &dx9.screen_slots[dx9.screen_index].target;
	transform_matrix_calc(&dx9.matrices, &pvrrc);
	dx9c_set_target(target->surface, NULL, dx9.screen_width, dx9.screen_height);
	if (!dx9c_begin_scene())
		return;
	dx9c_clear(D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0));

	viewport = &dx9.matrices.viewport;
	x0 = xform_x(viewport, 0.f);
	x1 = xform_x(viewport, 640.f);
	y0 = xform_y(viewport, 0.f);
	y1 = xform_y(viewport, 480.f);
	/* (R G B A bytes) */
	dx9c_draw_quad_at(dx9.fb_texture, true, true,
			x0 * 2.f / dx9.screen_width - 1.f, 1.f - y1 * 2.f / dx9.screen_height,
			(x1 - x0) * 2.f / dx9.screen_width, (y1 - y0) * 2.f / dx9.screen_height,
			0.f, 0.f, 1.f, 1.f, false, 1.f);
	end_screen();
}

/* ---- The polygons, as the Vulkan renderer's Drawer draws them ---- */

static void draw_poly(u32 listType, bool sortTriangles, const PolyParam *poly, u32 first, u32 count,
		int shadowPass, const RECT *within)
{
	RECT scissorRect;
	const int tileClip = set_tile_clip(poly->tileclip, &scissorRect);
	float trilinearAlpha = 1.f;
	bool palette, textured, swapRB = false, shadowedPass, depthWrite, shadowed;
	float palette_index = 0.f;
	u32 shadInstr, fog, bools;
	int depthFunc;

	if (within != NULL)
	{
		/* only the part of it inside this rectangle, and nothing if there is none */
		const RECT *clip = tileClip == TILECLIP_OUTSIDE ? &scissorRect : &dx9.base_scissor;
		RECT r;
		set_rect(&r, within->left > clip->left ? within->left : clip->left,
				within->top > clip->top ? within->top : clip->top,
				within->right < clip->right ? within->right : clip->right,
				within->bottom < clip->bottom ? within->bottom : clip->bottom);
		if (r.right <= r.left || r.bottom <= r.top || !dx9c_set_scissor(&r))
			return;
	}
	else if (!dx9c_set_scissor(tileClip == TILECLIP_OUTSIDE ? &scissorRect : &dx9.base_scissor))
		return;

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
	if (tileClip == TILECLIP_INSIDE || trilinearAlpha != 1.f || palette || dx9.prepass_draw)
	{
		dx9_poly_constants constants;
		constants.clip_test[0] = (float)scissorRect.left;
		constants.clip_test[1] = (float)scissorRect.top;
		constants.clip_test[2] = (float)scissorRect.right;
		constants.clip_test[3] = (float)scissorRect.bottom;
		constants.params[0] = trilinearAlpha;
		constants.params[1] = palette_index;
		constants.params[2] = dx9.prepass_draw ? (PrepassAlwaysHides(poly) ? -1.f : PREPASS_ALPHA_MIN) : 0.f;
		constants.params[3] = 0.f;
		set_poly_constants(&constants);
	}

	textured = PPW(poly->pcw, PCW_Texture) && poly->texid != 0;
	if (textured)
	{
		const dx9_texture *texture = (const dx9_texture *)(uintptr_t)poly->texid;
		const D3DTEXTUREADDRESS u = PPW(poly->tsp, TSP_ClampU) ? D3DTADDRESS_CLAMP
				: PPW(poly->tsp, TSP_FlipU) ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
		const D3DTEXTUREADDRESS v = PPW(poly->tsp, TSP_ClampV) ? D3DTADDRESS_CLAMP
				: PPW(poly->tsp, TSP_FlipV) ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
		dx9c_set_texture(DX9S_Texture, DX9_BASE(texture->texture));
		swapRB = texture->swap_rb;
		dx9c_set_sampler(DX9S_Texture, dx9c_sampler_key(PPW(poly->tsp, TSP_FilterMode) != 0,
				PPW(poly->tsp, TSP_FilterMode) != 0 ? (u32)settings.rend.AnisotropicFiltering : 1, u, v,
				PPW(poly->tsp, TSP_MipMapD)));
	}

	/* What the other renderers make a shader for, as the shader's
	 * flags. By the polygon alone, as the Vulkan renderer has it: the
	 * clamping limits change, and a shader is kept by the polygon's bit only */
	shadowedPass = shadowPass != SHADOW_PASS_NONE;
	shadInstr = PPW(poly->tsp, TSP_ShadInstr) & 3;
	fog = PPW(poly->tsp, TSP_FogCtrl) & 3;
	bools = (PPW(poly->tsp, TSP_UseAlpha) << DX9B_UseAlpha)
			| ((u32)(PPW(poly->tsp, TSP_IgnoreTexA) || PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_565) << DX9B_IgnoreTexA)
			| (PPW(poly->pcw, PCW_Offset) << DX9B_Offset)
			| ((u32)(PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_BUMPMAP) << DX9B_BumpMap)
			| (PPW(poly->tsp, TSP_ColorClamp) << DX9B_ColorClamp)
			| ((u32)(PPW(poly->pcw, PCW_Texture) && PPW(poly->tsp, TSP_FilterMode) > 1 && listType != ListType_Punch_Through
				&& PPW(poly->tcw, TCW_MipMapped) == 1) << DX9B_TriLinear)
			| ((u32)((poly->tileclip >> 28) == 3) << DX9B_ClipInside)
			| ((u32)(listType == ListType_Punch_Through) << DX9B_AlphaTest)
			| ((u32)shadowedPass << DX9B_Shadowed)
			| ((shadInstr & 1) << DX9B_ShadInstr0) | ((shadInstr >> 1) << DX9B_ShadInstr1)
			| ((fog & 1) << DX9B_Fog0) | ((fog >> 1) << DX9B_Fog1)
			| ((u32)swapRB << DX9B_SwapRB);
	dx9c_set_bools(bools);
	dx9c_set_ps(dx9_shaders_ps(!PPW(poly->pcw, PCW_Texture) ? DX9PS_Plain : palette ? DX9PS_Palette : DX9PS_Texture));
	dx9c_set_render_state(D3DRS_SHADEMODE, PPW(poly->pcw, PCW_Gouraud) ? D3DSHADE_GOURAUD : D3DSHADE_FLAT);

	/* Depth and stencil */
	if (listType == ListType_Punch_Through || sortTriangles)
		depthFunc = 6;	/* greater or equal */
	else
		depthFunc = (int)PPW(poly->isp, ISP_TSP_DepthMode);
	/* Sorted translucent polygons, per triangle or per strip, do not write
	 * depth: a strip drawn in sorted order would hide parts of the ones after it.
	 * Their depth prepass does. */
	if (dx9.prepass_draw)
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
			dx9c_set_depth_stencil(true, false, 2, DX9Stencil_ShadowLaterWins, 0xC1);
		else
			dx9c_set_depth_stencil(true, false, 2, DX9Stencil_ShadowEarlierWins, 0x81);
	}
	else if (shadowed)
		dx9c_set_depth_stencil(true, depthWrite, depthFunc, DX9Stencil_Shadow, PPW(poly->pcw, PCW_Shadow) != 0 ? 0x80 : 0);
	else
		dx9c_set_depth_stencil(true, depthWrite, depthFunc, DX9Stencil_None, 0);

	/* Apparently punch-through polys support blending, or at least some combinations
	 * An opaque polygon is blended by its instructions too when it is in a
	 * list continuation; the ones of the first render pass have had theirs
	 * set to source x 1 and destination x 0 when the list was read. */
	if (dx9.prepass_draw)
		dx9c_set_blend(false, 1, 0, false);
	else if (shadowPass == SHADOW_PASS_NONE
			&& (listType == ListType_Translucent || listType == ListType_Punch_Through
				|| PPW(poly->tsp, TSP_SrcInstr) != 1 || PPW(poly->tsp, TSP_DstInstr) != 0))
		dx9c_set_blend(true, PPW(poly->tsp, TSP_SrcInstr), PPW(poly->tsp, TSP_DstInstr), true);
	else
		dx9c_set_no_blend();

	set_cull_mode((int)PPW(poly->isp, ISP_TSP_CullMode));
	draw(dx9.prepass_draw || (sortTriangles && !settings.pvr.Emulation.AlphaSortMode) ? D3DPT_TRIANGLELIST : D3DPT_TRIANGLESTRIP,
			first, count);
}

/* The polygons of a list that take shadows, drawn again where they are in a
 * modifier volume: see rend/shadows.h. */
static void draw_shadowed(u32 listType, const PolyParam *polys, u32 first, u32 last,
		const ScreenBounds *area, const RECT *scissor)
{
	const PolyParam *pp_end = polys + last;
	const PolyParam *pp;
	for (pp = polys + first; pp != pp_end; pp++)
		if (CanDrawShadowed(listType, pp) && poly_overlaps(pp, area))
			draw_poly(listType, false, pp, pp->first, pp->count,
					shadow_later_wins(listType, pp) ? SHADOW_PASS_LATER_WINS : SHADOW_PASS_EARLIER_WINS,
					scissor);
}

static void draw_sorted(const SortTrigDrawParam *polys, u32 count, bool multipass)
{
	const u32 idxUsed = TA_LIST_USED(pvrrc.idx, u32);
	u32 i;
	for (i = 0; i < count; i++)
		draw_poly(ListType_Translucent, true, polys[i].ppid, idxUsed + polys[i].first, polys[i].count, SHADOW_PASS_NONE, NULL);

	if (multipass && settings.rend.TranslucentPolygonDepthMask)
	{
		/* Sorted translucent polygons are drawn without writing depth. Where
		 * a later render pass needs their depth (Cosmic Smash), it is written
		 * now, for those that do not have Z write disabled. */
		dx9c_set_decl(dx9.position_decl);
		dx9c_set_vs(dx9_shaders_vs(DX9VS_ModVol));
		dx9c_set_ps(dx9_shaders_ps(DX9PS_ModVol));
		dx9c_set_blend(false, 1, 0, false);
		dx9c_set_depth_stencil(true, true, 6, DX9Stencil_None, 0);
		for (i = 0; i < count; i++)
		{
			const SortTrigDrawParam *param = &polys[i];
			RECT scissorRect;
			if (param->count <= 2 || PPW(param->ppid->isp, ISP_TSP_ZWriteDis))
				continue;
			if (!dx9c_set_scissor(set_tile_clip(param->ppid->tileclip, &scissorRect) == TILECLIP_OUTSIDE
					? &scissorRect : &dx9.base_scissor))
				continue;
			set_cull_mode((int)PPW(param->ppid->isp, ISP_TSP_CullMode));
			draw(D3DPT_TRIANGLELIST, idxUsed + param->first, param->count);
		}
		dx9c_set_decl(dx9.main_decl);
		dx9c_set_vs(dx9_shaders_vs(DX9VS_Main));
	}
}

/* The depth prepass of a sorted list: rend/sorter.h */
static void draw_prepass(const SortTrigDrawParam *polys, u32 count)
{
	const u32 idxUsed = TA_LIST_USED(pvrrc.idx, u32);
	dx9_poly_constants saved;
	bool savedValid;
	u32 i;
	if (count == 0)
		return;
	/* (the colour pass's polygons that do not set their constants find what they found before) */
	saved = dx9.poly_uploaded;
	savedValid = dx9.poly_epoch == dx9c.epoch;
	dx9.prepass_draw = true;
	for (i = 0; i < count; i++)
		draw_poly(ListType_Translucent, true, polys[i].ppid, idxUsed + polys[i].first, polys[i].count, SHADOW_PASS_NONE, NULL);
	dx9.prepass_draw = false;
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

static void draw_mod_vols(int first, int count, const RenderPass *previous_pass, const RenderPass *current_pass)
{
	const ModifierVolumeParam *mvo = TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam);
	const ModifierVolumeParam *params;
	ScreenBounds area;
	int mod_base = -1;
	int cmv;

	/* (nothing of the frame on the target: nothing to do) */
	if (count == 0 || !dx9.have_mod_vols || !dx9c_set_scissor(&dx9.base_scissor))
		return;

	/* (the modifier volumes' triangles, after the vertices in the buffer) */
	dx9c_set_decl(dx9.position_decl);
	dx9c_set_stream(dx9.vertex_buffer, sizeof(float) * 3);
	dx9c_set_vs(dx9_shaders_vs(DX9VS_ModVol));
	dx9c_set_ps(dx9_shaders_ps(DX9PS_ModVol));
	dx9c_set_blend(false, 1, 0, false);

	params = &mvo[first];

	for (cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam *param = &params[cmv];
		u32 mvFirst, mvCount, mv_mode;
		RECT clipRect;
		bool visible;
		mv_clip_range_get(&pvrrc, mvo, param, &mvFirst, &mvCount);

		if (mvCount == 0)
			continue;

		mv_mode = PPW(param->isp, ISP_Modvol_DepthMode);

		/* clipped like a polygon, to the rectangle it was sent under if it asked to be
		 * (and with nothing of it left, nothing of it drawn) */
		visible = dx9c_set_scissor(set_tile_clip(param->tileclip, &clipRect) == TILECLIP_OUTSIDE ? &clipRect : &dx9.base_scissor);

		if (mod_base == -1)
			mod_base = (int)mvFirst;

		set_cull_mode((int)PPW(param->isp, ISP_Modvol_CullMode));
		if (!PPW(param->isp, ISP_Modvol_VolumeLast) && mv_mode > 0)
			dx9c_set_depth_stencil(true, false, 4, DX9Stencil_Or, 2);	/* OR'ing (open volume or quad) */
		else
			dx9c_set_depth_stencil(true, false, 4, DX9Stencil_Xor, 2);	/* XOR'ing (closed volume) */
		if (visible)
			IDirect3DDevice9_DrawPrimitive(dx9.device, D3DPT_TRIANGLELIST, dx9.mod_vol_base + mvFirst * 3, mvCount);

		if (mv_mode == 1 || mv_mode == 2)
		{
			/* Sum the area */
			dx9c_set_depth_stencil(false, false, 7, mv_mode == 1 ? DX9Stencil_Inclusion : DX9Stencil_Exclusion, 1);
			if (visible)
				IDirect3DDevice9_DrawPrimitive(dx9.device, D3DPT_TRIANGLELIST, dx9.mod_vol_base + mod_base * 3,
						mvFirst + mvCount - mod_base);
			mod_base = -1;
		}
	}
	dx9c_set_scissor(&dx9.base_scissor);
	dx9c_set_decl(dx9.main_decl);
	dx9c_set_stream(dx9.vertex_buffer, sizeof(Vertex));
	dx9c_set_vs(dx9_shaders_vs(DX9VS_Main));

	/* The polygons that take shadows, again, where they are in a volume */
	area = shadow_modvol_bounds(first, count);
	if (!screen_bounds_empty(&area))
	{
		int rect[4];
		RECT scissor;
		bounds_to_scissor(&area, &dx9.matrices.viewport, rect);
		set_rect(&scissor, rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]);
		draw_shadowed(ListType_Opaque, TA_LIST_HEAD(pvrrc.global_param_op, PolyParam),
				previous_pass->op_count, current_pass->op_count, &area, &scissor);
		draw_shadowed(ListType_Punch_Through, TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam),
				previous_pass->pt_count, current_pass->pt_count, &area, &scissor);
		dx9c_set_scissor(&dx9.base_scissor);
	}

	/* What is left: darkened as it is. The background polygon is the
	 * rectangle drawn: the first four vertices' positions. */
	dx9c_set_decl(dx9.position_decl);
	dx9c_set_vs(dx9_shaders_vs(DX9VS_ModVol));
	dx9c_set_ps(dx9_shaders_ps(DX9PS_ModVol));
	dx9c_set_alpha_blend();
	dx9c_set_depth_stencil(false, false, 7, DX9Stencil_Final, 0x81);
	set_cull_mode(0);
	draw(D3DPT_TRIANGLESTRIP, 0, 4);
	dx9c_set_decl(dx9.main_decl);
	dx9c_set_vs(dx9_shaders_vs(DX9VS_Main));
}

static void draw_frame(void)
{
	const RenderPass *passes = TA_LIST_HEAD(pvrrc.render_passes, RenderPass);
	const int npasses = TA_LIST_USED(pvrrc.render_passes, RenderPass);
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
		draw_list(ListType_Opaque, false, TA_LIST_HEAD(pvrrc.global_param_op, PolyParam), previous_pass.op_count, current_pass->op_count);
		draw_list(ListType_Punch_Through, false, TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam), previous_pass.pt_count, current_pass->pt_count);
		draw_mod_vols(previous_pass.mvo_count, current_pass->mvo_count - previous_pass.mvo_count, &previous_pass, current_pass);
		if (current_pass->autosort)
		{
			u32 n;
			const SortTrigDrawParam *draws = SortPassDraws(&dx9.prepass_passes, render_pass, &n);
			draw_prepass(draws, n);
			draws = SortPassDraws(&dx9.sorted_passes, render_pass, &n);
			if (!settings.pvr.Emulation.AlphaSortMode)
				draw_sorted(draws, n, render_pass < npasses - 1);
			else
			{
				SortPParams(previous_pass.tr_count, current_pass->tr_count - previous_pass.tr_count);
				draw_list(ListType_Translucent, true, TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam),
						previous_pass.tr_count, current_pass->tr_count);
			}
		}
		else
			draw_list(ListType_Translucent, false, TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam),
					previous_pass.tr_count, current_pass->tr_count);
		previous_pass = *current_pass;
	}
}

/* ---- The frame ---- */

static bool render_locked(void)
{
	if (pvrrc.isRenderFramebuffer)
	{
		draw_framebuffer();
		return true;
	}

	set_provoking_vertices();
	/* (the buffers are written before the scene begins) */
	if (!upload_geometry())
		return false;
	if (pvrrc.isRTT ? !begin_rtt() : !begin_screen())
	{
		dx9.rtt_texture = NULL;
		return false;
	}
	setup_frame();

	draw_frame();

	if (pvrrc.isRTT)
		end_rtt();
	else
		end_screen();

	return !pvrrc.isRTT;
}

static bool dx9_render(void)
{
	/* the device, taken by process() and kept for this, or taken here -
	 * and given back on the way out */
	const bool held = dx9.held_from_process;
	bool result;
	dx9.held_from_process = false;
	if (dx9.device == NULL)
	{
		if (held)
			dx9c_unlock();
		return false;
	}
	if (pvrrc.isRTT && (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min))
	{
		/* A pixel clip that ends before it starts holds no pixel: the
		 * chip writes nothing. (Its size would be negative here.) */
		WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d\n",
				pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
		if (held)
			dx9c_unlock();
		return false;
	}
	if (!held)
	{
		/* the frontend may still be showing what is in the screen's next
		 * picture: it is waited for, before the device is taken */
		if (!pvrrc.isRTT)
			dx9c_wait_present_index();
		dx9c_lock();
	}
	result = render_locked();
	dx9c_unlock();
	return result;
}

static const renderer_ops dx9_renderer_ops =
{
	dx9_init,
	dx9_resize,
	dx9_term,
	dx9_process,
	dx9_render,
	dx9_keep_picture,
	dx9_get_texture,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

const renderer_ops *rend_D3D9(void)
{
	return &dx9_renderer_ops;
}
