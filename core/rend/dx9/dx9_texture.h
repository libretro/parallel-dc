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
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d9.h>
#include "rend/TexCache.h"
#include "rend/texdecode.h"
#include "rend/tex_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A texture of the texture pool's, and a surface of each of its levels
 * where it is drawn into */
typedef struct dx9_pooled_texture dx9_pooled_texture;
struct dx9_pooled_texture
{
	TexPoolEntry entry;
	IDirect3DTexture9 *texture;
	IDirect3DSurface9 *surfaces[TEXD_MAX_LEVELS];
	u32 levels;
	dx9_pooled_texture *all;		/* every one there is, for dx9_pool_term() */
};

/* The cache's textures (rend/tex_pool.h): made when the renderer starts,
 * and handed out to the textures that need one of their shape. Of three
 * kinds: the ones the processor writes (dynamic, as the renderer's own are:
 * the formats 0 to 4, by TEX_TYPE_*); the ones the pixel shader draws
 * into (render targets, DX9_POOL_DRAWN + TEX_TYPE_*), made only when it is
 * to; and the pictures rendered to (DX9_POOL_TARGET, A8R8G8B8). A texture
 * is given back as soon as it is done with: Direct3D sees to it that what
 * the GPU still has to do with it is done first. */
#define DX9_POOL_DRAWN 5
#define DX9_POOL_TARGET 10
#define DX9_POOL_FORMATS 11
/* (the render targets of the formats the texture decoder draws into, if
 * it is ready: dx9_texdecoder.h) */
void dx9_pool_init(void);
void dx9_pool_term(void);
/* A texture of the pool's format @f, @width by @height, with its levels
 * or not: a free one of that shape, or one made now if there is none.
 * Null if the shape or the format is not one of the pool's. */
dx9_pooled_texture *dx9_pool_take(u32 f, u32 width, u32 height, bool mips);
void dx9_pool_give(dx9_pooled_texture *texture);
/* how many it has made */
u32 dx9_pool_made(void);

/* A texture in video memory, written by the processor where it lies: a
 * dynamic texture (a managed one on a device without them), locked with
 * D3DLOCK_DISCARD, which the texture cache converts the console's texture
 * straight into when it can (upload_memory). */
typedef struct dx9_texture
{
	texcache_data base;
	IDirect3DTexture9 *texture;
	/* The texture holds R G B A bytes, which the card reads as B G R A:
	 * the shader swaps them */
	bool swap_rb;

	/* A render to this texture is drawn into this other picture, and the
	 * two change places when it is done (as the Direct3D 11 renderer's) */
	IDirect3DTexture9 *spare;
	IDirect3DSurface9 *spare_surface;

	/* Locked by upload_memory for the upload_to_gpu that follows */
	void *locked_bits;
	dx9_pooled_texture *pooled;
	dx9_pooled_texture *spare_pooled;
	u32 decode_job, decode_count, decode_serial;
} dx9_texture;

/* The renderer's texture cache, by the textures' words (texcache_map_*) */
dx9_texture *dx9_texcache_get(u32 tsp, u32 tcw, int area);
void dx9_texcache_reserve_for(u32 images);
void dx9_texcache_cleanup(void);
void dx9_texcache_clear(void);
/* ...and its room given back (after dx9_texcache_clear()) */
void dx9_texcache_free(void);
/* Whether the textures use the decoder and the pool: while the renderer
 * has them */
void dx9_textures_attach(bool decoder, bool pool);

/* The spare made the texture, and the texture given back */
void dx9_texture_swap_with_spare(dx9_texture *t);
/* The spare, from the pool if it has one of this shape */
bool dx9_texture_take_spare(dx9_texture *t, u32 width, u32 height);
void dx9_texture_release_spare(dx9_texture *t);

/* The renderer's side of rend/rtt_watch.h: a render to a texture stays in
 * the texture, and is brought to video memory when the game first touches
 * it there. */
void dx9_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx9_rtt_watch(dx9_texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx9_rtt_term(void);
/* width by height pixels from the top left of a render target texture,
 * R G B A, top line first. Waits for the card. */
bool dx9_read_picture(IDirect3DTexture9 *texture, u32 width, u32 height, u8 *rgba);

#ifdef __cplusplus
}
#endif
