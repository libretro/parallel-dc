/* core/rend/dx11/dx11_dxbc.inc from the shader files a run of the core
 * left (rend/d3d_shader_cache.h): the shaders of the families given, by
 * hash, sorted. See kernels_dx11.sh.
 *
 *   dx11_dxbc_inc OUT FILE:FAMILY,FAMILY... [FILE:FAMILY...]
 *
 * A shader both files have (the same hash) is written once. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d_shader_cache.h"

typedef struct
{
   uint64_t       hash;
   const uint8_t *bytes;
   uint32_t       size;
} shader_t;

static uint8_t *slurp(const char *path, size_t *len)
{
   FILE    *f = fopen(path, "rb");
   uint8_t *data;
   long     n;
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   data = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
   if (!data || n <= 0 || fread(data, 1, (size_t)n, f) != (size_t)n)
   {
      free(data);
      fclose(f);
      return NULL;
   }
   fclose(f);
   *len = (size_t)n;
   return data;
}

static int by_hash(const void *a, const void *b)
{
   const shader_t *x = (const shader_t *)a;
   const shader_t *y = (const shader_t *)b;
   return x->hash < y->hash ? -1 : x->hash > y->hash;
}

int main(int argc, char **argv)
{
   shader_t *all  = NULL;
   size_t    n    = 0, room = 0, i, at;
   FILE     *out;
   int       a;

   if (argc < 3)
   {
      fprintf(stderr, "dx11_dxbc_inc OUT FILE:FAMILY,... [...]\n");
      return 2;
   }
   for (a = 2; a < argc; a++)
   {
      char                 path[1024];
      const char          *colon = strrchr(argv[a], ':');
      d3d_shcache_entry_t *e     = NULL;
      size_t               count = 0, blobs = 0, len = 0;
      uint8_t             *data;
      if (!colon || (size_t)(colon - argv[a]) >= sizeof(path))
         return 2;
      memcpy(path, argv[a], (size_t)(colon - argv[a]));
      path[colon - argv[a]] = '\0';
      if (!(data = slurp(path, &len)) || !d3d_shcache_parse(data, len, &e, &count, &blobs))
      {
         fprintf(stderr, "dx11_dxbc_inc: %s is not a shader file\n", path);
         return 1;
      }
      for (i = 0; i < count; i++)
      {
         char        fam[16];
         const char *f = colon + 1;
         int         wanted = 0;
         sprintf(fam, "%u", (unsigned)e[i].family);
         while (*f)
         {
            size_t l = strcspn(f, ",");
            if (l == strlen(fam) && !strncmp(f, fam, l))
               wanted = 1;
            f += l + (f[l] == ',');
         }
         if (!wanted)
            continue;
         if (n == room)
         {
            room = room ? room * 2 : 64;
            all  = (shader_t *)realloc(all, room * sizeof(*all));
         }
         all[n].hash  = e[i].hash;
         all[n].bytes = data + blobs + e[i].offset;
         all[n].size  = e[i].size;
         n++;
      }
      free(e);
      /* (data kept: the shaders point into it) */
   }
   qsort(all, n, sizeof(*all), by_hash);
   /* (the same shader from both files, once) */
   for (i = at = 0; i < n; i++)
      if (!at || all[at - 1].hash != all[i].hash)
         all[at++] = all[i];
   n = at;

   if (!(out = fopen(argv[1], "w")))
      return 1;
   fprintf(out,
      "/* The bytecode of the shaders the Direct3D 11 renderers' start makes,\n"
      " * by the hash of what each is compiled from (DX11ShaderCompiler::HashOf):\n"
      " * made by tools/rend/kernels_dx11.sh from the core's own run. Not to be\n"
      " * edited. */\n");
   {
      size_t total = 0;
      for (i = 0; i < n; i++)
         total += all[i].size;
      fprintf(out, "static const u8 dx11_dxbc_bytes[%u] = {\n", (unsigned)(total ? total : 1));
      if (!total)
         fprintf(out, "0\n");
      for (i = 0; i < n; i++)
      {
         uint32_t k;
         for (k = 0; k < all[i].size; k++)
            fprintf(out, "0x%02x,%s", all[i].bytes[k], (k % 16 == 15 || k + 1 == all[i].size) ? "\n" : "");
      }
      fprintf(out, "};\n");
   }
   fprintf(out, "static const struct { u64 hash; u32 offset; u32 size; } dx11_dxbc_index[%u] = {\n",
         (unsigned)(n ? n : 1));
   if (!n)
      fprintf(out, "{ 0, 0, 0 }\n");
   for (i = at = 0; i < n; i++)
   {
      fprintf(out, "{ 0x%016llxull, %u, %u },\n", (unsigned long long)all[i].hash,
            (unsigned)at, (unsigned)all[i].size);
      at += all[i].size;
   }
   fprintf(out, "};\nstatic const size_t dx11_dxbc_count = %u;\n", (unsigned)n);
   fclose(out);
   printf("dx11_dxbc_inc: %u shaders\n", (unsigned)n);
   return 0;
}
