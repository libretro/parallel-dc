/* Custom textures' pictures (core/rend/custom_image.h). See
 * custom_image_test.sh.
 *
 *   custom_image_test decode FILE...   each picture's name, size and the
 *                                      XXH32 of its RGBA bytes, bottom row
 *                                      first, a line each
 *   custom_image_test cut FILE...      the files cut short and spoilt:
 *                                      a picture or none, never a fault
 *   custom_image_test dump DIR         textures of each kind dumped to
 *                                      DIR, read back the same
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <encodings/xxh32.h>

#include "custom_image.h"

static int fails;

static uint8_t *slurp(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   uint8_t *data = NULL;
   long n;

   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0
         && fseek(f, 0, SEEK_SET) == 0
         && (data = (uint8_t *)malloc((size_t)n)) != NULL)
   {
      if (fread(data, 1, (size_t)n, f) != (size_t)n)
      {
         free(data);
         data = NULL;
      }
      else
         *len = (size_t)n;
   }
   fclose(f);
   return data;
}

static const char *base_name(const char *path)
{
   const char *s = strrchr(path, '/');
   const char *b = strrchr(path, '\\');
   if (b > s)
      s = b;
   return s ? s + 1 : path;
}

static void decode(const char *path)
{
   size_t len = 0;
   uint8_t *data = slurp(path, &len);
   uint8_t *px;
   unsigned w = 0, h = 0;

   if (!data)
   {
      printf("%s: not read\n", path);
      fails++;
      return;
   }
   px = custom_image_decode(data, len, &w, &h);
   if (px)
      printf("%s %u %u %08x\n", base_name(path), w, h,
            (unsigned)xxh32(px, (size_t)w * h * 4, 0));
   else
      printf("%s none\n", base_name(path));
   free(px);
   free(data);
}

/* Every cut, and some bytes spoilt, of one file */
static void cut(const char *path)
{
   size_t len = 0, n, i;
   uint8_t *data = slurp(path, &len);
   unsigned seed = 1;

   if (!data)
   {
      printf("%s: not read\n", path);
      fails++;
      return;
   }
   for (n = 0; n < len; n++)
   {
      /* (a copy of its own, so that a read past the end is seen) */
      uint8_t *part = (uint8_t *)malloc(n ? n : 1);
      unsigned w, h;
      memcpy(part, data, n);
      free(custom_image_decode(part, n, &w, &h));
      free(part);
   }
   for (i = 0; i < 200; i++)
   {
      uint8_t *bad = (uint8_t *)malloc(len);
      unsigned w, h, k;
      memcpy(bad, data, len);
      for (k = 0; k < 4; k++)
      {
         seed = seed * 1103515245u + 12345u;
         /* (past the signature, or nothing gets as far as the decoder) */
         bad[8 + (seed >> 8) % (len - 8)] ^= (uint8_t)(1u << ((seed >> 4) & 7));
      }
      free(custom_image_decode(bad, len, &w, &h));
      free(bad);
   }
   printf("%s: cut and spoilt\n", base_name(path));
   free(data);
}

/* (five and six bits to eight, their top bits repeated below them) */
#define FIVE(v) ((uint8_t)(((v) << 3) | ((v) >> 2)))
#define SIX(v)  ((uint8_t)(((v) << 2) | ((v) >> 4)))

/* A texel made RGBA as a dumped texture is meant to be */
static void expect(enum custom_image_texels type, const void *texels,
      unsigned i, uint8_t *o)
{
   unsigned t;
   if (type == CUSTOM_IMAGE_8888)
   {
      memcpy(o, (const uint8_t *)texels + i * 4, 4);
      return;
   }
   t = ((const uint16_t *)texels)[i];
   switch (type)
   {
      case CUSTOM_IMAGE_4444:
         o[0] = (uint8_t)((t >> 12) * 0x11);
         o[1] = (uint8_t)(((t >> 8) & 15) * 0x11);
         o[2] = (uint8_t)(((t >> 4) & 15) * 0x11);
         o[3] = (uint8_t)((t & 15) * 0x11);
         break;
      case CUSTOM_IMAGE_565:
         o[0] = FIVE(t >> 11);
         o[1] = SIX((t >> 5) & 63);
         o[2] = FIVE(t & 31);
         o[3] = 255;
         break;
      default:
         o[0] = FIVE(t >> 11);
         o[1] = FIVE((t >> 6) & 31);
         o[2] = FIVE((t >> 1) & 31);
         o[3] = (uint8_t)((t & 1) ? 255 : 0);
         break;
   }
}

static void dump(const char *dir)
{
   static const char *const names[4] = { "4444", "565", "5551", "8888" };
   enum { W = 13, H = 7 };
   uint8_t texels[W * H * 4];
   unsigned k, i;

   for (i = 0; i < sizeof(texels); i++)
      texels[i] = (uint8_t)(i * 37 + (i >> 3) * 11);
   /* (the ends of each range too) */
   texels[0] = texels[1] = 0;
   texels[2] = texels[3] = 255;
   for (k = 0; k < 4; k++)
   {
      enum custom_image_texels type = (enum custom_image_texels)k;
      char path[1024];
      size_t len = 0;
      uint8_t *data, *px;
      unsigned w = 0, h = 0, bad = 0;
      FILE *f;

      sprintf(path, "%.900s/%s.png", dir, names[k]);
      if (!custom_image_dump(path, texels, W, H, type)
            || !(data = slurp(path, &len)))
      {
         printf("dump %s: not written\n", names[k]);
         fails++;
         continue;
      }
      px = custom_image_decode(data, len, &w, &h);
      if (!px || w != W || h != H)
      {
         printf("dump %s: not read back\n", names[k]);
         fails++;
      }
      else
      {
         for (i = 0; i < W * H; i++)
         {
            uint8_t o[4];
            expect(type, texels, i, o);
            if (memcmp(o, px + i * 4, 4))
               bad++;
         }
         /* What the file holds, top row first, for another reader */
         sprintf(path, "%.900s/%s.rgba", dir, names[k]);
         if ((f = fopen(path, "wb")) != NULL)
         {
            unsigned y;
            for (y = 0; y < H; y++)
               for (i = 0; i < W; i++)
               {
                  uint8_t o[4];
                  expect(type, texels, (H - 1 - y) * W + i, o);
                  fwrite(o, 1, 4, f);
               }
            fclose(f);
         }
         if (bad)
         {
            printf("dump %s: %u of %u texels not as dumped\n", names[k],
                  bad, (unsigned)(W * H));
            fails++;
         }
         else
            printf("dump %s: %ux%u read back the same\n", names[k], w, h);
      }
      free(px);
      free(data);
   }
   if (custom_image_dump(dir, texels, 0, H, CUSTOM_IMAGE_565))
   {
      printf("dump: a texture of no width written\n");
      fails++;
   }
}

int main(int argc, char **argv)
{
   int i;
   if (argc < 3)
   {
      fprintf(stderr, "custom_image_test decode|cut FILE... | dump DIR\n");
      return 2;
   }
   if (!strcmp(argv[1], "decode"))
      for (i = 2; i < argc; i++)
         decode(argv[i]);
   else if (!strcmp(argv[1], "cut"))
      for (i = 2; i < argc; i++)
         cut(argv[i]);
   else if (!strcmp(argv[1], "dump"))
      dump(argv[2]);
   else
      return 2;
   return fails ? 1 : 0;
}
