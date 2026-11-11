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
#include <stdlib.h>
#include <string.h>

#include <formats/image.h>
#include <formats/rpng.h>
#include <formats/rjpeg.h>

#include "custom_image.h"

/* (the largest side taken: a texture is at most 1024, scaled up 8 times) */
#define CUSTOM_IMAGE_SIDE 16384

static uint8_t *custom_image_png(const uint8_t *data, size_t len,
      unsigned *width, unsigned *height)
{
   rpng_t *rpng = rpng_alloc();
   uint32_t *px = NULL;
   unsigned w   = 0;
   unsigned h   = 0;
   unsigned y, x;
   int ret      = IMAGE_PROCESS_ERROR;

   if (!rpng)
      return NULL;
   if (rpng_set_buf_ptr(rpng, (void *)data, len) && rpng_start(rpng))
   {
      while (rpng_iterate_image(rpng))
         ;
      if (rpng_is_valid(rpng))
         do
            ret = rpng_process_image(rpng, (void **)&px, len, &w, &h, true);
         while (ret == IMAGE_PROCESS_NEXT);
   }
   rpng_free(rpng);
   if (ret != IMAGE_PROCESS_END || !px || !w || !h)
   {
      free(px);
      return NULL;
   }
   /* The other way up, swapped in place */
   for (y = 0; y < h / 2; y++)
   {
      uint32_t *a = px + (size_t)y * w;
      uint32_t *b = px + (size_t)(h - 1 - y) * w;
      for (x = 0; x < w; x++)
      {
         uint32_t t = a[x];
         a[x]       = b[x];
         b[x]       = t;
      }
   }
   *width  = w;
   *height = h;
   return (uint8_t *)px;
}

struct custom_image_rows
{
   uint8_t *px;
   unsigned width;
   unsigned height;
};

static bool custom_image_jpeg_size(void *user, unsigned width, unsigned height)
{
   struct custom_image_rows *r = (struct custom_image_rows *)user;
   if (!width || !height || width > CUSTOM_IMAGE_SIDE || height > CUSTOM_IMAGE_SIDE)
      return false;
   r->px     = (uint8_t *)malloc((size_t)width * height * 4);
   r->width  = width;
   r->height = height;
   return r->px != NULL;
}

/* Decoded the other way up, straight into place */
static uint8_t *custom_image_jpeg_row(void *user, unsigned y)
{
   struct custom_image_rows *r = (struct custom_image_rows *)user;
   return r->px + (size_t)(r->height - 1 - y) * r->width * 4;
}

static uint8_t *custom_image_jpeg(const uint8_t *data, size_t len,
      unsigned *width, unsigned *height)
{
   struct custom_image_rows r;
   struct rjpeg_rows rows;
   rjpeg_t *rjpeg = rjpeg_alloc();
   unsigned w     = 0;
   unsigned h     = 0;
   bool ok;

   if (!rjpeg)
      return NULL;
   r.px      = NULL;
   r.width   = 0;
   r.height  = 0;
   rows.size = custom_image_jpeg_size;
   rows.row  = custom_image_jpeg_row;
   rows.done = NULL;
   rows.user = &r;
   ok        = rjpeg_decode_rows(rjpeg, data, len, true, &rows, &w, &h);
   rjpeg_free(rjpeg);
   if (!ok || !r.px)
   {
      free(r.px);
      return NULL;
   }
   *width  = r.width;
   *height = r.height;
   return r.px;
}

uint8_t *custom_image_decode(const uint8_t *data, size_t len,
      unsigned *width, unsigned *height)
{
   if (!data)
      return NULL;
   if (len >= 8 && rpng_header_ready(data, len))
      return custom_image_png(data, len, width, height);
   if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
      return custom_image_jpeg(data, len, width, height);
   return NULL;
}

bool custom_image_dump(const char *path, const void *texels,
      unsigned width, unsigned height, enum custom_image_texels type)
{
   const uint16_t *src = (const uint16_t *)texels;
   const uint32_t *src32 = (const uint32_t *)texels;
   uint8_t *px;
   unsigned y, x;
   bool ok;

   if (!width || !height || type > CUSTOM_IMAGE_8888)
      return false;
   if (!(px = (uint8_t *)malloc((size_t)width * height * 4)))
      return false;
   /* Full is 255, as the texture converters make it for a texture drawn
    * from 32 bits: with the low bits left clear an opaque texel would be
    * dumped with an alpha of 240, and white as 248 or 240, and that is the
    * picture a replacement is painted over and loaded back from. */
   for (y = 0; y < height; y++)
   {
      /* The file's first row is the texture's last */
      uint8_t *dst = px + (size_t)(height - 1 - y) * width * 4;
      for (x = 0; x < width; x++, dst += 4)
      {
         unsigned t;
         switch (type)
         {
            case CUSTOM_IMAGE_4444:
               t      = *src++;
               dst[0] = (uint8_t)(((t >> 12) & 0xF) * 17);
               dst[1] = (uint8_t)(((t >> 8) & 0xF) * 17);
               dst[2] = (uint8_t)(((t >> 4) & 0xF) * 17);
               dst[3] = (uint8_t)((t & 0xF) * 17);
               break;
            case CUSTOM_IMAGE_565:
               t      = *src++;
               dst[0] = (uint8_t)((((t >> 11) & 0x1F) << 3) | ((t >> 13) & 7));
               dst[1] = (uint8_t)((((t >> 5) & 0x3F) << 2) | ((t >> 9) & 3));
               dst[2] = (uint8_t)(((t & 0x1F) << 3) | ((t >> 2) & 7));
               dst[3] = 255;
               break;
            case CUSTOM_IMAGE_5551:
               t      = *src++;
               dst[0] = (uint8_t)((((t >> 11) & 0x1F) << 3) | ((t >> 13) & 7));
               dst[1] = (uint8_t)((((t >> 6) & 0x1F) << 3) | ((t >> 8) & 7));
               dst[2] = (uint8_t)((((t >> 1) & 0x1F) << 3) | ((t >> 3) & 7));
               dst[3] = (uint8_t)((t & 1) ? 255 : 0);
               break;
            default:
               memcpy(dst, src32++, 4);
               break;
         }
      }
   }
   ok = rpng_save_image_rgba(path, px, width, height, width * 4);
   free(px);
   return ok;
}
