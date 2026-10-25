/* The bindings the Direct3D 12 compute renderer's DXIL kernels declare,
 * from their containers' PSV0 parts (what the runtime takes them from),
 * held to what dx12/dx12_compute.cpp binds: each of the HDR output's
 * walks (dx12_compute_hdr_dxil.inc) binds what the walk it stands for
 * (dx12_compute_dxil.inc) does and the channel sums (u1 of space 2, a
 * typed UAV of a 2D texture) besides, and is signed; no other kernel binds
 * the sums. The temporal upscaler's walks (dx12_compute_upscaler_dxil.inc)
 * the same way: the walk for one sample's bindings and the depth (u2 of
 * space 2) besides - and the sums, in the two that have them; no other
 * kernel binds the depth. Runs anywhere: no Direct3D 12, no dxc.
 *
 *   cc -DPVRC_KERNELS=N -DPVRC_K_WALK=W -Icore/rend/dx12 tools/rend/dxil_bindings.c
 *
 * (kernels_dx12.sh bindings builds and runs it.) C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int u32;

#include "dx12_compute_dxil.inc"
#include "dx12_compute_hdr_dxil.inc"
#include "dx12_compute_upscaler_dxil.inc"

#define MAX_BINDINGS 64

/* PSV0's resource types (DxilPipelineStateValidation.h) */
enum { PSV_UAV_TYPED = 6 };
/* ...and kinds (DXIL::ResourceKind) */
enum { KIND_TEXTURE2D = 2 };

struct binding
{
   u32 type, space, lower, upper, kind;
};

static u32 word(const unsigned char *p)
{
   return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* @code (@size bytes) a DXIL container: its bindings into @out, how many;
 * -1 when it has no PSV0 part to read them from. @signed_out: whether its
 * digest is not all zeros. */
static int bindings_of(const u32 *code, size_t size, struct binding *out, int *signed_out)
{
   const unsigned char *c = (const unsigned char *)code;
   u32 parts, i;
   int k;
   if (size < 32 || memcmp(c, "DXBC", 4) != 0)
      return -1;
   *signed_out = 0;
   for (k = 4; k < 20; k++)
      if (c[k] != 0)
         *signed_out = 1;
   parts = word(c + 28);
   for (i = 0; i < parts && 32 + 4 * i + 4 <= size; i++)
   {
      const u32 at = word(c + 32 + 4 * i);
      const unsigned char *part, *p;
      u32 part_size, info_size, count, entry_size, n;
      if (at + 8 > size)
         return -1;
      part = c + at;
      part_size = word(part + 4);
      if (memcmp(part, "PSV0", 4) != 0)
         continue;
      if (at + 8 + part_size > size)
         return -1;
      p = part + 8;
      info_size = word(p);
      p += 4 + info_size;
      count = word(p);
      p += 4;
      if (count == 0)
         return 0;
      entry_size = word(p);
      p += 4;
      if (count > MAX_BINDINGS || entry_size < 16 || p + count * entry_size > part + 8 + part_size)
         return -1;
      for (n = 0; n < count; n++, p += entry_size)
      {
         out[n].type  = word(p);
         out[n].space = word(p + 4);
         out[n].lower = word(p + 8);
         out[n].upper = word(p + 12);
         out[n].kind  = entry_size >= 24 ? word(p + 16) : 0;
      }
      return (int)count;
   }
   return -1;
}

static int is_sums(const struct binding *b)
{
   return b->space == 2 && b->lower == 1;
}

static int is_depth(const struct binding *b)
{
   return b->space == 2 && b->lower == 2;
}

static int has(const struct binding *set, int n, const struct binding *b)
{
   int i;
   for (i = 0; i < n; i++)
      if (set[i].type == b->type && set[i].space == b->space && set[i].lower == b->lower
            && set[i].upper == b->upper && set[i].kind == b->kind)
         return 1;
   return 0;
}

int main(void)
{
   const int kernels = (int)(sizeof(dx12_compute_dxil) / sizeof(dx12_compute_dxil[0]));
   struct binding mine[MAX_BINDINGS], theirs[MAX_BINDINGS];
   int failures = 0, k, i, sig;

   if (kernels != PVRC_KERNELS + 8 || sizeof(dx12_compute_hdr_dxil) / sizeof(dx12_compute_hdr_dxil[0]) != 8
         || sizeof(dx12_compute_upscaler_dxil) / sizeof(dx12_compute_upscaler_dxil[0]) != 4)
   {
      fprintf(stderr, "dxil_bindings: not the kernels it is made for (%d)\n", kernels);
      return 1;
   }
   /* (no kernel but the HDR output's walks binds the sums, none but the
    * upscaler's the depth) */
   for (k = 1; k < kernels; k++)
   {
      const int n = bindings_of(dx12_compute_dxil[k], dx12_compute_dxil_size[k], theirs, &sig);
      if (n < 0)
      {
         fprintf(stderr, "dxil_bindings: kernel %d: no bindings read\n", k);
         failures++;
         continue;
      }
      for (i = 0; i < n; i++)
         if (is_sums(&theirs[i]) || is_depth(&theirs[i]))
         {
            fprintf(stderr, "dxil_bindings: kernel %d binds the HDR output's sums or the upscaler's depth\n", k);
            failures++;
         }
   }
   for (k = 0; k < 8; k++)
   {
      const int n = bindings_of(dx12_compute_hdr_dxil[k], dx12_compute_hdr_dxil_size[k], theirs, &sig);
      for (i = 0; i < n; i++)
         if (is_depth(&theirs[i]))
         {
            fprintf(stderr, "dxil_bindings: HDR walk %d binds the upscaler's depth\n", k);
            failures++;
         }
   }
   /* each HDR walk: the walk it stands for (sorted by SSAA_LOG2() of its
    * samples, then unsorted), and the sums */
   for (k = 0; k < 8; k++)
   {
      const int walk = k == 0 ? PVRC_K_WALK : PVRC_KERNELS + k;
      const int m = bindings_of(dx12_compute_hdr_dxil[k], dx12_compute_hdr_dxil_size[k], mine, &sig);
      const int n = bindings_of(dx12_compute_dxil[walk], dx12_compute_dxil_size[walk], theirs, &i);
      int sums = 0, picture = 0, j;
      if (m < 0 || n < 0)
      {
         fprintf(stderr, "dxil_bindings: HDR walk %d: no bindings read\n", k);
         failures++;
         continue;
      }
      if (!sig)
      {
         fprintf(stderr, "dxil_bindings: HDR walk %d is not signed\n", k);
         failures++;
      }
      for (j = 0; j < n; j++)
         if (!has(mine, m, &theirs[j]))
         {
            fprintf(stderr, "dxil_bindings: HDR walk %d lacks walk %d's binding (type %u, space %u, %u..%u)\n",
                  k, walk, theirs[j].type, theirs[j].space, theirs[j].lower, theirs[j].upper);
            failures++;
         }
      for (j = 0; j < m; j++)
      {
         if (is_sums(&mine[j]))
         {
            sums++;
            if (mine[j].type != PSV_UAV_TYPED || mine[j].upper != 1 || (mine[j].kind != 0 && mine[j].kind != KIND_TEXTURE2D))
            {
               fprintf(stderr, "dxil_bindings: HDR walk %d: the sums are not a typed UAV of a 2D texture at u1 of space 2\n", k);
               failures++;
            }
         }
         else if (mine[j].space == 2 && mine[j].lower == 0)
            picture++;
         else if (!has(theirs, n, &mine[j]))
         {
            fprintf(stderr, "dxil_bindings: HDR walk %d binds what walk %d does not (type %u, space %u, %u..%u)\n",
                  k, walk, mine[j].type, mine[j].space, mine[j].lower, mine[j].upper);
            failures++;
         }
      }
      if (sums != 1 || picture != 1)
      {
         fprintf(stderr, "dxil_bindings: HDR walk %d: %d bindings of the sums, %d of the picture\n", k, sums, picture);
         failures++;
      }
      printf("dxil_bindings: HDR walk %d (walk %d): %d bindings, %d of them the walk's, %d the sums\n", k, walk, m, n, sums);
   }
   /* each upscaler walk: the walk for one sample it stands for (sorted,
    * unsorted, then both with the sums), the depth, and the sums where it
    * has them */
   for (k = 0; k < 4; k++)
   {
      const int walk = (k & 1) == 0 ? PVRC_K_WALK : PVRC_KERNELS + 4;
      const int withSums = k >= 2;
      const int m = bindings_of(dx12_compute_upscaler_dxil[k], dx12_compute_upscaler_dxil_size[k], mine, &sig);
      const int n = bindings_of(dx12_compute_dxil[walk], dx12_compute_dxil_size[walk], theirs, &i);
      int sums = 0, depth = 0, picture = 0, j;
      if (m < 0 || n < 0)
      {
         fprintf(stderr, "dxil_bindings: upscaler walk %d: no bindings read\n", k);
         failures++;
         continue;
      }
      if (!sig)
      {
         fprintf(stderr, "dxil_bindings: upscaler walk %d is not signed\n", k);
         failures++;
      }
      for (j = 0; j < n; j++)
         if (!has(mine, m, &theirs[j]))
         {
            fprintf(stderr, "dxil_bindings: upscaler walk %d lacks walk %d's binding (type %u, space %u, %u..%u)\n",
                  k, walk, theirs[j].type, theirs[j].space, theirs[j].lower, theirs[j].upper);
            failures++;
         }
      for (j = 0; j < m; j++)
      {
         if (is_sums(&mine[j]) || is_depth(&mine[j]))
         {
            if (is_sums(&mine[j]))
               sums++;
            else
               depth++;
            if (mine[j].type != PSV_UAV_TYPED || mine[j].upper != mine[j].lower || (mine[j].kind != 0 && mine[j].kind != KIND_TEXTURE2D))
            {
               fprintf(stderr, "dxil_bindings: upscaler walk %d: the sums or the depth are not a typed UAV of a 2D texture at u1 or u2 of space 2\n", k);
               failures++;
            }
         }
         else if (mine[j].space == 2 && mine[j].lower == 0)
            picture++;
         else if (!has(theirs, n, &mine[j]))
         {
            fprintf(stderr, "dxil_bindings: upscaler walk %d binds what walk %d does not (type %u, space %u, %u..%u)\n",
                  k, walk, mine[j].type, mine[j].space, mine[j].lower, mine[j].upper);
            failures++;
         }
      }
      if (sums != withSums || depth != 1 || picture != 1)
      {
         fprintf(stderr, "dxil_bindings: upscaler walk %d: %d bindings of the sums, %d of the depth, %d of the picture\n", k, sums, depth, picture);
         failures++;
      }
      printf("dxil_bindings: upscaler walk %d (walk %d): %d bindings, %d of them the walk's, %d the sums, %d the depth\n", k, walk, m, n, sums, depth);
   }
   printf("dxil_bindings: %s\n", failures ? "failed" : "passed");
   return failures ? 1 : 0;
}
