/* The textures' hashes (TexCache.cpp: whole, and a VQ texture's codebook
 * then its indices; seed 7) and the others the core takes (seeds 0 and 1),
 * with libretro-common's xxh32, against the values xxHash's own XXH32 gave
 * for them when it was the core's: custom textures are found by these
 * names, and dumped under them. C89.
 *
 *   cc -std=c89 -Icore/libretro-common/include tools/rend/texhash_test.c \
 *      core/libretro-common/encodings/encoding_xxh32.c && ./a.out
 */
#include <stdio.h>
#include <stdint.h>

#include <encodings/xxh32.h>

static unsigned char vram[1 << 16];

struct one
{
   unsigned at;
   unsigned len;
   uint32_t hash;
};

static const struct one ones[] = {
   {   0,     0, 0xd7adaff8 }, {  13,    1, 0x63711f7a }, {  26,    3, 0x1918c976 },
   {  39,     4, 0x790bb426 }, {  52,   15, 0x8f465801 }, {  65,   16, 0x6d1a6e1b },
   {  78,    17, 0x6f9d2118 }, {  91,   31, 0x5bff26c3 }, { 104,   32, 0x510363c4 },
   { 117,    33, 0xb877a9e6 }, { 130,   64, 0x63617393 }, { 143, 1024, 0xa29e14ae },
   { 156,  2048, 0x40a42932 }, { 169,  128, 0x3b137796 }, { 182, 4096, 0x276df442 },
   {   0, 65536, 0xac1e3670 }
};

/* A VQ texture: the codebook (2048 bytes), then that many bytes of indices */
static const uint32_t vqs[5] = {
   0x258f3fdb, 0xc947bb5f, 0xa92abfec, 0x3440761f, 0x4bcd67a4
};

int main(void)
{
   unsigned i, fails = 0;
   uint32_t s = 0x12345678;

   for (i = 0; i < sizeof(vram); i++)
   {
      s       = s * 1664525u + 1013904223u;
      vram[i] = (unsigned char)(s >> 24);
   }
   for (i = 0; i < sizeof(ones) / sizeof(ones[0]); i++)
   {
      uint32_t h = xxh32(vram + ones[i].at, ones[i].len, 7);
      if (h != ones[i].hash)
      {
         printf("FAIL: %u bytes at %u: %08x, not %08x\n", ones[i].len,
               ones[i].at, (unsigned)h, (unsigned)ones[i].hash);
         fails++;
      }
   }
   for (i = 0; i < 5; i++)
   {
      xxh32_state_t st;
      unsigned n = 64u << (i * 2);
      uint32_t h;
      xxh32_reset(&st, 7);
      xxh32_update(&st, vram + 100, 2048);
      xxh32_update(&st, vram + 100 + 2048, n);
      h = xxh32_digest(&st);
      if (h != vqs[i])
      {
         printf("FAIL: VQ texture, %u bytes of indices: %08x, not %08x\n",
               n, (unsigned)h, (unsigned)vqs[i]);
         fails++;
      }
   }
   if (xxh32(vram, 65536, 0) != 0x00c3350e || xxh32(vram, 4097, 1) != 0x89513dcd)
   {
      printf("FAIL: seeds 0 and 1\n");
      fails++;
   }
   printf("texhash_test: %s\n", fails ? "FAIL" : "the textures' hashes as XXH32 made them");
   return fails ? 1 : 0;
}
