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
#include "rend/TexCache.h"
#include "rend/texdecode.h"
#include "rend/tex_pool.h"
#include "dx12_shader_flags.h"
#include "dx12context_lr.h"
#include "dx12_texdecoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A texture of the texture pool's (rend/tex_pool.h): its place in the
 * bindless table, which is its for good, and the state the GPU last left
 * it in. The decoder writes its levels through unordered access views
 * (32-bit and 8-bit texels, into a typeless texture) or draws them through
 * render target views (16-bit texels), made when they are needed in the
 * context's rings of them: a view is a few words in a heap, not an
 * object. */
typedef struct dx12_pooled_texture dx12_pooled_texture;
struct dx12_pooled_texture
{
	TexPoolEntry entry;
	ID3D12Resource *resource;
	D3D12_RESOURCE_STATES state;
	u32 srv;
	u32 rtv;			/* a picture rendered to: its render target view, for good */
	u32 width;
	u32 height;
	u32 levels;
	DXGI_FORMAT format;	/* the view's */
	bool writable;
	bool own;			/* not one of the pool's shapes: made for one texture, and goes with it */
	dx12_pooled_texture *all;		/* every one there is, for dx12_pool_term() */
};

/* The cache's textures: made when the renderer starts, and handed out to
 * the textures that need one of their shape. The pool's formats are the
 * texture types, by their numbers, and DX12_POOL_TARGET: the pictures
 * rendered to, RGBA8 with a render target view. A texture is given back
 * as soon as it is done with: everything is recorded into one command
 * list, in order, so what the GPU still has to draw with it is drawn
 * before it is written again. */
#define DX12_POOL_FORMATS 6
#define DX12_POOL_TARGET 5
/* (the decoder's, dx12_texdec_init(), made first) */
void dx12_pool_init(void);
void dx12_pool_term(void);
u32 dx12_pool_made(void);
/* The pictures rendered to made for unordered access views too (the
 * compute renderer's renders to a texture): before dx12_pool_init() */
void dx12_pool_set_target_storage(bool on);
bool dx12_pool_targets_storable(void);
/* A picture to render to, @width by @height; null if the pool has none
 * of that shape */
dx12_pooled_texture *dx12_pool_take_target(u32 width, u32 height);
/* One of a shape the pool has not, for one texture: @levels levels of
 * @type (DX12_POOL_TARGET for a picture to render to). Given back like
 * any other, and goes then. */
dx12_pooled_texture *dx12_pool_make_own(u32 type, u32 width, u32 height, u32 levels);
void dx12_pool_give(dx12_pooled_texture *texture);

typedef struct dx12_texture
{
	texcache_data base;
	dx12_pooled_texture *pooled;
	/* A render to this texture is drawn into this other picture, and the
	 * two change places when it is done: the polygons of the render that
	 * are drawn with the texture itself see the picture it had before, as
	 * the other renderers' do, and no picture is made anew for every
	 * render. */
	dx12_pooled_texture *spare;
	u32 decode_job, decode_count, decode_serial;
	/* tex_upload_memory() handed out a piece of the upload ring for the
	 * next upload */
	u8 *upload_cpu;
	dx12_upload upload_piece;
	u32 upload_pitch;
} dx12_texture;

/* Its place in the bindless table (DX12_SRV_NONE when it has none:
 * sampled as zero) */
static INLINE u32 dx12_texture_srv(const dx12_texture *t)
{
	return t->pooled != NULL ? t->pooled->srv : DX12_SRV_NONE;
}

dx12_texture *dx12_texcache_get(u32 tsp, u32 tcw, int area);
void dx12_texcache_reserve_for(u32 images);
void dx12_texcache_cleanup(void);
void dx12_texcache_clear(void);
void dx12_texcache_free(void);
/* The decoder and the pool the textures use, while the renderer has them */
void dx12_textures_attach(bool decoder, bool pool);

/* (the texture it had goes back to the pool: the spare is that now) */
void dx12_texture_swap_with_spare(dx12_texture *t);
/* The spare, from the pool if it has one of this shape */
bool dx12_texture_take_spare(dx12_texture *t, u32 width, u32 height);
/* ...or this one, made for it */
void dx12_texture_set_spare(dx12_texture *t, dx12_pooled_texture *spare);
void dx12_texture_release_spare(dx12_texture *t);

/* The polygons' samplers: all of them in the sampler table when the
 * renderer starts, by the TSP's MipMapD (four bits), FilterMode (two),
 * ClampV, ClampU, FlipV, FlipU - ten bits, DX12_SAMPLER_TABLE_SIZE of
 * them. The fog table's and the palette's are the root signature's own. */
static INLINE u32 dx12_sampler_index(u32 tsp)
{
	return ((tsp >> 8) & 0xF) | (((tsp >> 13) & 0x3F) << 4);
}
void dx12_make_samplers(void);

/* The Direct3D 12 renderers' side of rend/rtt_watch.h, as vulkan/rtt_read.h
 * is the Vulkan renderers': a render to a texture stays in the texture,
 * and is brought to video memory when the game first touches it there. */
void dx12_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx12_rtt_watch(dx12_texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx12_rtt_init(void);
void dx12_rtt_term(void);
/* width by height pixels of a picture of that size (left in @state, and
 * left as it was), R G B A, top line first. Submits what is recorded and
 * waits for the card: a readback. */
bool dx12_read_picture(ID3D12Resource *texture, D3D12_RESOURCE_STATES *state, u32 width, u32 height, u8 *rgba);

#ifdef __cplusplus
}
#endif
