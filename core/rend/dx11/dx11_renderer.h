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
#pragma once
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d11.h>
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/sorter.h"
#include "rend/transform_matrix.h"
#include "dx11context_lr.h"
#include "dx11_renderstate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Direct3D 11 renderers. This one sorts translucent polygons by
 * triangle or by strip; the per-pixel one (oit/) is made from it: it has
 * the frame drawn its own way (dx11_renderer_kind) and the rest this one's.
 *
 * Everything the game sees is what it sees with the Vulkan renderers,
 * which this follows step for step: the TA context and its render passes,
 * the shadows of rend/shadows.h, renders to textures that stay on the card
 * until the game touches them in video memory (rend/rtt_watch.h), the
 * picture kept across a new context or a new size (rend/last_picture.h).
 * The frame is a picture the frontend is handed (dx11c_set_frame()). */

/* What the renderer that draws the frame has its own */
typedef struct dx11_renderer_kind
{
	/* The frame's polygons: into what the render bound */
	void (*draw_frame)(void);
	/* What is bound by the time the frame is drawn: the per-pixel renderer
	 * puts the frame together in pictures of its own, and binds the
	 * screen's only at the end */
	bool binds_screen;
	/* Whether the sorted lists are drawn as sorted (and have a prepass) */
	bool draws_sorted;
	/* How a frame decoded on the GPU is drawn (ta_gpu_prepare()'s
	 * TA_GPU_*); 0: none is - every frame is parsed */
	u32 (*ta_flags)(void);
	/* Whether the decoder is to have room for sorted translucent triangles (TA_GPU_SORT) */
	bool ta_sorts;
	/* What the polygons' vertices are read with, and their constants are in */
	ID3D11InputLayout *(*poly_input_layout)(void);
	ID3D11Buffer *(*poly_constants)(void);
} dx11_renderer_kind;

/* The screen's pictures: one for each of the frontend's sync indexes (as
 * many as dx11c_present_count() can say) */
#define DX11_SCREEN_SLOTS 32

typedef struct dx11_screen_target
{
	ID3D11Texture2D *texture;
	ID3D11RenderTargetView *target;
	ID3D11ShaderResourceView *view;
	ID3D11UnorderedAccessView *uav;	/* for the chip's write-out (dx11_writeout.h) */
} dx11_screen_target;

typedef struct dx11_renderer
{
	const dx11_renderer_kind *kind;
	ID3D11Device *device;
	ID3D11DeviceContext *ctx;
	dx11_depth_states depth_states;
	transform_matrix_t matrices;

	ID3D11InputLayout *main_layout;
	ID3D11InputLayout *position_layout;	/* the main vertices' positions alone */
	ID3D11InputLayout *modvol_layout;	/* the modifier volumes', from vertex slot 1 */
	/* A render's vertices, indexes and modifier volumes, one after the
	 * other in one buffer, bound once (vertex slots 0 and 1, and the
	 * indexes), each render's after the last one's, and the buffer started
	 * again only when it is full: where this render's are in it */
	ID3D11Buffer *geometry;
	u32 geometry_size;
	u32 geometry_used;
	INT vertex_base;		/* (in vertices) */
	UINT index_base;		/* (in indexes) */
	UINT modvol_base;		/* (in modifier volume vertices) */
	bool have_modvols;
	ID3D11Buffer *vtx_constants;
	ID3D11Buffer *frag_constants;
	ID3D11Buffer *poly_constants;
	ID3D11RasterizerState *rasterizers[3];	/* no culling, front, back */
	ID3D11Texture2D *fog_texture;
	ID3D11ShaderResourceView *fog_view;
	u8 fog_uploaded[256];
	bool fog_uploaded_valid;
	ID3D11Texture2D *palette_texture;
	ID3D11ShaderResourceView *palette_view;

	/* The depth and stencil, for the screen and renders to textures: as
	 * large as the largest of them so far. A view to draw with and, for
	 * the per-pixel renderer, views that leave the stencil or both to be
	 * read by the shaders as well. */
	ID3D11Texture2D *depth_tex;
	ID3D11DepthStencilView *depth_view;
	ID3D11DepthStencilView *depth_view_read_stencil;
	ID3D11DepthStencilView *depth_view_read_only;
	ID3D11ShaderResourceView *depth_srv;
	ID3D11ShaderResourceView *stencil_srv;
	u32 depth_width;
	u32 depth_height;

	/* The screen: a picture for each of the frontend's sync indexes (one
	 * with version 1), the size rendered at */
	dx11_screen_target screens[DX11_SCREEN_SLOTS];
	u32 screen_count;
	/* ...and one to draw a second render to the screen in, in a frame whose
	 * first one has been handed over (version 2) already: it takes the
	 * place of the one handed over, which stays the frontend's */
	dx11_screen_target extra;
	bool last_in_extra;		/* the last picture is in it */
	/* the chip's write-out (dx11_writeout.h), and whether the pictures were made for it */
	bool writeout_pictures;
	u32 screen_width;
	u32 screen_height;
	int screen_index;		/* the one being drawn */
	int last_screen;		/* the one the last picture is in */
	bool have_picture;
	/* ...or the picture kept from the context before (rend/last_picture.h),
	 * or from the size before (dx11_screen_ensure()) */
	ID3D11ShaderResourceView *restored_picture;

	/* A render to a texture: the texture, where in video memory */
	struct dx11_texture *rtt_texture;
	u32 rtt_addr;
	/* What the frame is drawn into, and the size of what is drawn of it */
	ID3D11RenderTargetView *target_view;
	u32 target_width;
	u32 target_height;
	D3D11_RECT base_scissor;

	/* The context taken by process() and kept for the render() after it */
	bool held_from_process;
	/* Per-triangle sort results: a SortList a pass, their indices in the
	 * geometry buffer (sorted there by the upload); and the depth prepasses
	 * of the sorted lists (rend/sorter.h), their indexes after each pass's
	 * sorted ones */
	grow_buf_t sorted_passes;
	u32 sorted_index_count;
	grow_buf_t prepass_passes;
	/* (a frame decoded on the GPU) the draws are of the sorted indices,
	 * whose counts are the CPU's */
	bool gpu_sorted;
	bool prepass_draw;	/* the polygons are drawn for the depth prepass */

	/* The framebuffer the game wrote itself, for a frame that is that */
	ID3D11Texture2D *fb_texture;
	ID3D11ShaderResourceView *fb_view;
	int fb_width;
	int fb_height;
} dx11_renderer;

extern dx11_renderer dx11r;

/* The base's: what the per-pixel renderer has of this one */
bool dx11r_init(const dx11_renderer_kind *kind);
void dx11r_term(void);
void dx11r_resize(int w, int h);
bool dx11r_process(TA_context *ctx);
bool dx11r_render(void);
void dx11r_keep_picture(void);
u64 dx11r_get_texture(u32 tsp, u32 tcw, int area);
void dx11r_options_changed(void);

/* At least what a render of a screen to a texture is: 640x480 times
 * Render To Texture Upscaling */
void dx11r_common_render(u32 *width, u32 *height);
bool dx11r_ensure_depth(u32 width, u32 height);
/* TILECLIP_*: @rect the polygon's clip */
int dx11r_set_tile_clip(u32 val, D3D11_RECT *rect);
void dx11r_set_cull_mode(int mode);
/* Whether the render starts from the last picture: it covers only part
 * of the screen, and the game has drawn to this framebuffer lately */
bool dx11r_keeps_last_picture(void);
/* The last picture drawn whole onto what is bound */
void dx11r_draw_last_picture(u32 width, u32 height);
/* The geometry buffer, bound; or a frame decoded on the GPU's buffers */
void dx11r_bind_geometry(void);
/* A draw group of a frame decoded on the GPU (pvrrc.gpu), drawn by its
 * arguments there: an opaque or punch-through one by its command (or,
 * @shadow, its second one, for shadows), a translucent one by its
 * records, @count of them at most */
void dx11r_draw_gpu(u32 list_type, bool shadow, u32 first, u32 count);
/* A constant buffer written whole (a whole number of 16-byte registers) */
static INLINE void dx11r_set_constants(ID3D11Buffer *buffer, const void *constants)
{
	ID3D11DeviceContext_UpdateSubresource(dx11r.ctx, (ID3D11Resource *)buffer, 0, NULL, constants, 0, 0);
}
static INLINE void dx11r_set_texture(UINT slot, ID3D11ShaderResourceView *view, ID3D11SamplerState *sampler)
{
	dx11c_set_ps_resource(slot, view);
	dx11c_set_ps_sampler(slot, sampler);
}

#ifdef __cplusplus
}
#endif
