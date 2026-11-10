/* What the texture pool makes at the start (rend/tex_pool.h,
 * TexPoolBook::Plan()): the images renders to a texture and textures
 * scaled up are drawn into, so that none is made while a game runs.
 *
 *   sh tools/rend/tex_pool_test.sh */
#include "rend/tex_pool.h"
#include <stdio.h>

static int fails;

static void expect(const char *what, u32 got, u32 want)
{
   if (got != want)
   {
      printf("FAIL: %s: %u, not %u\n", what, got, want);
      fails++;
   }
}

static void at_least(const char *what, u32 got, u32 least)
{
   if (got < least)
   {
      printf("FAIL: %s: %u, fewer than %u\n", what, got, least);
      fails++;
   }
}

/* 565, 5551, 4444, 8888, 8-bit, and the pictures rendered to */
enum { F565 = 0, F8888 = 3, TARGET = 5, FORMATS = 6 };
static const u32 texel[FORMATS] = { 2, 2, 2, 4, 1, 4 };
static const u32 use[FORMATS] = { 6, 8, 6, 4, 2, 0 };
static const u8 kind[FORMATS] = { 0, 0, 0, 0, 0, TexPoolBook::TARGETS };
static u32 counts[TexPoolBook::CLASSES];

static void plan(u32 budget_mib, u32 upscale, u32 upMax, u32 rtt)
{
   VramBudget budget;
   budget.budget_kib = budget_mib << 10;
   budget.usage_kib = 0;
   budget.exact = true;
   TexPoolBook::Plan(budget, texel, use, kind, FORMATS, counts, upscale, upMax, rtt);
}

static u32 target(u32 w, u32 h)
{
   return counts[TexPoolBook::Class(TARGET, w, h, false)];
}

int main(void)
{
   u32 base64, base128, base512;

   /* A card with much memory: the shapes renders come in */
   plan(8192, 1, 0, 1);
   base64 = target(64, 64);
   base128 = target(128, 128);
   base512 = target(1024, 512);
   expect("64x64 targets", base64, 8);
   expect("128x128 targets", base128, 2);
   expect("1024x512 targets", base512, 4);

   /* A card with little memory (a sixteenth of it under 64 MB) has half as
    * many of the large ones, and all the small ones a game renders many of
    * at once (a state loaded every frame keeps five of 64 in use) */
   plan(512, 1, 0, 1);
   expect("64x64 targets, little memory", target(64, 64), 8);
   expect("128x128 targets, little memory", target(128, 128), 2);
   expect("1024x512 targets, little memory", target(1024, 512), 2);

   /* Renders to a texture twice as fine: those as large as they are drawn
    * too (the console's size is read back), up to 1024 */
   plan(8192, 1, 0, 2);
   expect("64x64 targets, renders 2x", target(64, 64), base64);
   expect("128x128 targets, renders 2x", target(128, 128), base128 + base64);
   expect("1024x1024 targets, renders 2x", target(1024, 1024), 1 + 2);
   plan(8192, 1, 0, 4);
   expect("256x256 targets, renders 4x", target(256, 256), 2 + base64);
   /* (three times: sizes no image of the pool's has) */
   plan(8192, 1, 0, 3);
   expect("128x128 targets, renders 3x", target(128, 128), base128);

   /* Textures scaled up twice: 32-bit, a size up, with their mipmaps (as
    * many as the same share has room for) */
   plan(8192, 1, 0, 1);
   {
      const u32 plain256 = counts[TexPoolBook::Class(F8888, 256, 256, true)];
      plan(8192, 2, 256, 1);
      expect("565 128x128 mipmapped, scaled 2x", counts[TexPoolBook::Class(F565, 128, 128, true)], 0);
      at_least("8888 256x256 mipmapped, scaled 2x", counts[TexPoolBook::Class(F8888, 256, 256, true)], plain256 + 1);
   }

   if (fails)
   {
      printf("tex_pool_test: %d failed\n", fails);
      return 1;
   }
   printf("tex_pool_test: the pool's plan is as it should be\n");
   return 0;
}
