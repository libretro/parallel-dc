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
#include "rend/TexCache.h"
#include "rend/texdecode.h"
#include "rend/tex_pool.h"
#include "dx11_texdecoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A texture of the texture pool's: a view of it to sample, and of each
 * level for the decoder to write through - an unsigned integer view of
 * the 32-bit and 8-bit ones (made typeless for that), a render target view
 * of the 16-bit ones - where the decoder makes textures of its format */
typedef struct dx11_pooled_texture dx11_pooled_texture;
struct dx11_pooled_texture
{
	TexPoolEntry entry;
	ID3D11Texture2D *texture;
	ID3D11ShaderResourceView *view;
	ID3D11UnorderedAccessView *uavs[TEXD_MAX_LEVELS];
	ID3D11RenderTargetView *targets[TEXD_MAX_LEVELS];
	u32 levels;
	bool writable;
	dx11_pooled_texture *all;		/* every one there is, for dx11_pool_term() */
};

/* The cache's textures (rend/tex_pool.h): made when the renderer starts,
 * and handed out to the textures that need one of their shape. The pool's
 * formats are the texture types, by their numbers, and DX11_POOL_TARGET:
 * the pictures rendered to, RGBA8 with a render target view (targets[0]).
 * A texture is given back as soon as it is done with: Direct3D sees to it
 * that what the GPU still has to do with it is done first. The renderer's,
 * one at a time. */
#define DX11_POOL_FORMATS 6
#define DX11_POOL_TARGET 5
/* (the decoder's first, if it is to be used: dx11_texdecoder.h) */
void dx11_pool_init(void);
void dx11_pool_term(void);
/* how many it has made */
u32 dx11_pool_made(void);

/* A texture in the cache (rend/TexCache.h) */
typedef struct dx11_texture
{
	texcache_data base;
	ID3D11Texture2D *texture;
	ID3D11ShaderResourceView *texture_view;

	/* A render to this texture is drawn into this other picture, and the
	 * two change places when it is done: the polygons of the render that
	 * are drawn with the texture itself see the picture it had before, as
	 * the other renderers' do, and no picture is made anew for every
	 * render. */
	ID3D11Texture2D *spare;
	ID3D11ShaderResourceView *spare_view;
	ID3D11RenderTargetView *spare_target;

	dx11_pooled_texture *pooled;
	dx11_pooled_texture *spare_pooled;
	u32 decode_job, decode_count, decode_serial;
} dx11_texture;

/* The renderer's texture cache, by the textures' words (texcache_map_*) */
dx11_texture *dx11_texcache_get(u32 tsp, u32 tcw, int area);
void dx11_texcache_reserve_for(u32 images);
void dx11_texcache_cleanup(void);
void dx11_texcache_clear(void);
/* ...and its room given back (after dx11_texcache_clear()) */
void dx11_texcache_free(void);
/* Whether the textures use the decoder and the pool: while the renderer
 * has them */
void dx11_textures_attach(bool decoder, bool pool);

/* The spare made the texture, and the texture given back */
void dx11_texture_swap_with_spare(dx11_texture *t);
/* The spare, from the pool if it has one of this shape */
bool dx11_texture_take_spare(dx11_texture *t, u32 width, u32 height);
void dx11_texture_release_spare(dx11_texture *t);

/* A texture of one level and a view of it, with @data (rows @pitch bytes
 * apart) or none: false, and both NULL, if they cannot be made */
bool dx11_make_texture(u32 width, u32 height, DXGI_FORMAT format, UINT bind,
		ID3D11Texture2D **texture, ID3D11ShaderResourceView **view, const void *data, UINT pitch);

/* The Direct3D 11 renderers' side of rend/rtt_watch.h, as vulkan/rtt_read.h
 * is the Vulkan renderers': a render to a texture stays in the texture,
 * and is brought to video memory when the game first touches it there. */
void dx11_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx11_rtt_watch(dx11_texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx11_rtt_init(void);
void dx11_rtt_term(void);
/* width by height pixels of a picture of that size, R G B A, top line
 * first. Waits for the card. False if the frontend's context cannot be
 * read from. */
bool dx11_read_picture(ID3D11Texture2D *texture, u32 width, u32 height, u8 *rgba);

#ifdef __cplusplus
}
#endif
