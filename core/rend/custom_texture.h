/*
	 Copyright 2018 flyinghead

	 This file is part of reicast.

	 reicast is free software: you can redistribute it and/or modify
	 it under the terms of the GNU General Public License as published by
	 the Free Software Foundation, either version 2 of the License, or
	 (at your option) any later version.

	 reicast is distributed in the hope that it will be useful,
	 but WITHOUT ANY WARRANTY; without even the implied warranty of
	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	 GNU General Public License for more details.

	 You should have received a copy of the GNU General Public License
	 along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
/* Custom textures: pictures in the frontend's system directory, under
 * dc/textures/<game id>/<hash>.png (or .jpg), put in place of the game's
 * textures of that hash, and the game's textures dumped there by hash.
 *
 * A texture asks with custom_texture_request(); a thread of its own loads
 * the picture, and the texture takes it once custom_request_t's count is
 * back to zero. Nothing is allocated when a texture asks; the list of
 * pictures is made once, when the thread starts. */
#ifndef REND_CUSTOM_TEXTURE_H
#define REND_CUSTOM_TEXTURE_H
#include "types.h"
#include <retro_atomic.h>
#include <retro_common_api.h>
#include "custom_image.h"

RETRO_BEGIN_DECLS

/* A texture's part in it */
typedef struct custom_request
{
	/* The requests the loader has not answered. Raised from zero by the
	 * renderer only, brought back to zero by the loader only, last of all
	 * it does to the texture: while it is not zero the texture is the
	 * loader's (it may not be deleted). */
	retro_atomic_int_t in_progress;
	/* What the loader is asked to look for: the texture's three names as
	 * they were when the renderer asked, which is when it had the texture
	 * and video memory to itself. Written by the renderer before it counts
	 * the request in in_progress; read by the loader after it has read
	 * that count - so a request that changes them under the loader is one
	 * more for it to answer, and what it found is thrown away. */
	retro_atomic_int_t hash;
	retro_atomic_int_t old_hash;
	retro_atomic_int_t old_vqhash;
	struct custom_request *next;	/* link in the loader's work list */
	/* The picture loaded (malloc'd, RGBA), for the renderer once
	 * in_progress is zero; NULL if none */
	u8 *image_data;
	u32 width;
	u32 height;
} custom_request_t;

/* A texture made (or made again): nothing asked, nothing loaded */
void custom_request_init(custom_request_t *r);

/* Renderer: whether the game has custom textures (the first time, the
 * loader is started). Ask with custom_texture_request() only if so. */
bool custom_texture_available(void);
/* Renderer: ask for the picture named @hash, or else @old_vqhash (if not
 * 0), or else @old_hash, for @r. */
void custom_texture_request(custom_request_t *r, u32 hash, u32 old_vqhash, u32 old_hash);
/* Renderer: @w x @h texels of @type at @src, written as <hash>.png in
 * the texture dump's directory */
void custom_texture_dump(u32 hash, int w, int h, enum custom_image_texels type, const void *src);
/* The game goes: the loader stopped, what it did not get to dropped (the
 * counts back to zero), the list forgotten */
void custom_texture_terminate(void);

RETRO_END_DECLS

#endif
