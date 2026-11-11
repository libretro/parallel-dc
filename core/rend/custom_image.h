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
/* Custom textures' pictures, read (PNG, JPEG) and dumped (PNG) with
 * libretro-common's rpng and rjpeg. C89.
 *
 * A picture is held as the renderers take a texture: RGBA bytes, the
 * bottom row first. Files hold the top row first. */
#ifndef FLYCAST_CUSTOM_IMAGE_H
#define FLYCAST_CUSTOM_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* A texture's texels, as a dumped one is given */
enum custom_image_texels
{
   CUSTOM_IMAGE_4444 = 0,
   CUSTOM_IMAGE_565,
   CUSTOM_IMAGE_5551,
   CUSTOM_IMAGE_8888
};

/* The picture in a PNG or JPEG file's bytes (by what they are, not the
 * file's name), or NULL. Free it with free(). */
uint8_t *custom_image_decode(const uint8_t *data, size_t len,
      unsigned *width, unsigned *height);

/* Writes @texels (a texture: bottom row first; 16 bits a texel, or 32 for
 * CUSTOM_IMAGE_8888) to a PNG file. Four, five and six bits are made eight
 * by repeating their top bits. */
bool custom_image_dump(const char *path, const void *texels,
      unsigned width, unsigned height, enum custom_image_texels type);

RETRO_END_DECLS

#endif
