/* ACES 2.0 (Full)'s tables and cube made off the frame (hdr_transform.c):
 * with threads, hdr_dt_fill() hands them to a worker and never waits for
 * it. The tables:
 *
 *  - the first fill with none draws the tone scale alone, and the fills
 *    after a change of settings go on with the tables there were, not
 *    the new ones made there and then (as they were before: a frame of
 *    several milliseconds);
 *  - what the fills draw once the worker's are back is what the tables
 *    made in place (hdr_dt_finish()) give, to the byte.
 *
 * The cube it comes back with is held to the one baked in place
 * (hdr_dt_aces2_cube_finish()), byte for byte:
 *
 *  - baked through, with the frame thread polling as frames do;
 *  - superseded half way by another paper white, and by other tables (the
 *    gamut), the stale cube never handed out and the right one in the end;
 *  - freed with a bake in flight (hdr_dt_free() stops and joins it).
 *
 *   hdr_cube [--dump file]   (built twice by hdr_cube.sh: with threads,
 *                             and with TARGET_NO_THREADS for the bake in
 *                             place a fill at a time)
 *
 * C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/rend/hdr/hdr_transform.h"

#define CUBE_BYTES (sizeof(uint16_t) * 4 * HDR_DT_ACES2_CUBE * HDR_DT_ACES2_CUBE * HDR_DT_ACES2_CUBE)

static const uint16_t xy[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static unsigned failures;

static void settings(struct hdr_dt_settings *s, int gamut)
{
   memset(s, 0, sizeof(*s));
   s->curve           = HDR_DT_ACES2FULL;
   s->aces2_surround  = 1;
   s->aces2_gamut     = gamut;
   s->peak_mode       = 0;
   s->peak_nits       = 1000.0f;
   s->gt_toe          = 0.22f;
   s->gt_linear       = 0.40f;
   s->filmic_contrast = 3;
   s->filmic_max_ev   = 4.026068811f;
}

/* Fills as frames do until the block says the cube is in use; how many
 * fills that took. A cube handed out before then is a failure. */
static unsigned fill_until_baked(float paper)
{
   struct hdr_dt_block b;
   unsigned fills = 0;
   clock_t start = clock();
   for (;;)
   {
      hdr_dt_fill(&b, paper, 1000.0f, 1000.0f, xy);
      fills++;
      if (b.gm[2] != 0.0f)
      {
         if (!hdr_dt_aces2_cube())
         {
            printf("hdr_cube: the block reads a cube that is not there\n");
            failures++;
         }
         return fills;
      }
      if (hdr_dt_aces2_cube())
      {
         printf("hdr_cube: a cube handed out before the block reads it\n");
         failures++;
      }
      if ((double)(clock() - start) / CLOCKS_PER_SEC > 60.0)
      {
         printf("hdr_cube: no cube after %u fills\n", fills);
         failures++;
         return fills;
      }
   }
}

/* The cube for @gamut at @paper, baked in place */
static uint16_t *reference(int gamut, float paper)
{
   struct hdr_dt_settings s;
   uint16_t *ref = (uint16_t*)malloc(CUBE_BYTES);
   hdr_dt_free();
   settings(&s, gamut);
   hdr_dt_configure(&s);
   hdr_dt_finish(paper, 1000.0f, 1000.0f, xy, true);
   if (!ref || !hdr_dt_aces2_cube())
   {
      printf("hdr_cube: no reference cube\n");
      exit(1);
   }
   memcpy(ref, hdr_dt_aces2_cube(), CUBE_BYTES);
   hdr_dt_free();
   return ref;
}

/* The block the fills draw for @gamut at @paper with the tables made in
 * place, the cube flag aside */
static void reference_block(int gamut, float paper, struct hdr_dt_block *b)
{
   struct hdr_dt_settings s;
   hdr_dt_free();
   settings(&s, gamut);
   hdr_dt_configure(&s);
   hdr_dt_finish(paper, 1000.0f, 1000.0f, xy, false);
   hdr_dt_fill(b, paper, 1000.0f, 1000.0f, xy);
   b->gm[2] = 0.0f;
   hdr_dt_free();
}

#ifndef TARGET_NO_THREADS
/* Fills until the block is @want, the cube flag aside: the tables the
 * worker makes come to it. Each block before is @before's (NULL: one of
 * no tables, the tone scale alone), never one of two tables: a fill
 * whose cube bake took new tables back half way through gave that,
 * reading the ones it had freed. */
static void fill_until(float paper, const struct hdr_dt_block *before, const struct hdr_dt_block *want,
      struct hdr_dt_block *b)
{
   clock_t start = clock();
   for (;;)
   {
      hdr_dt_fill(b, paper, 1000.0f, 1000.0f, xy);
      b->gm[2] = 0.0f;
      if (!memcmp(b, want, sizeof(*b)))
         return;
      if (before ? memcmp(b, before, sizeof(*b)) != 0 : (int)b->mode[0] != HDR_DT_ACES2)
      {
         printf("hdr_cube: a block neither of the tables before nor of the new ones\n");
         failures++;
         return;
      }
      if ((double)(clock() - start) / CLOCKS_PER_SEC > 60.0)
      {
         printf("hdr_cube: not the tables made in place after a minute\n");
         failures++;
         return;
      }
   }
}

#endif

static void same_block(const char *what, const struct hdr_dt_block *b, const struct hdr_dt_block *ref)
{
   if (memcmp(b, ref, sizeof(*b)))
   {
      printf("hdr_cube: %s: the block is not the one the tables made in place give\n", what);
      failures++;
   }
   else
      printf("hdr_cube: %s: same\n", what);
}

/* The tables: made off the frame, and the same as made in place */
static void tables(void)
{
   struct hdr_dt_settings s;
   struct hdr_dt_block b, ref1, ref2, old;
   reference_block(1, 200.0f, &ref1);
   reference_block(2, 200.0f, &ref2);

   settings(&s, 1);
   hdr_dt_configure(&s);
   hdr_dt_fill(&b, 200.0f, 1000.0f, 1000.0f, xy);
#ifndef TARGET_NO_THREADS
   /* none yet: the tone scale alone, nothing solved in the fill */
   if ((int)b.mode[0] != HDR_DT_ACES2 || hdr_dt_aces2_lut())
   {
      printf("hdr_cube: the first fill made the tables itself\n");
      failures++;
   }
   fill_until(200.0f, NULL, &ref1, &b);
#endif
   b.gm[2] = 0.0f;
   same_block("tables made off the frame", &b, &ref1);

   /* another gamut (Rec.2020): the fills go on with these until the new ones are back */
   old = b;
   settings(&s, 2);
   hdr_dt_configure(&s);
   hdr_dt_fill(&b, 200.0f, 1000.0f, 1000.0f, xy);
   b.gm[2] = 0.0f;
#ifndef TARGET_NO_THREADS
   if (memcmp(&b, &old, sizeof(b)))
   {
      printf("hdr_cube: the fill after a change made the new tables itself\n");
      failures++;
   }
   fill_until(200.0f, &old, &ref2, &b);
#endif
   same_block("another gamut's tables", &b, &ref2);
   (void)old;
   hdr_dt_free();
}

static void same(const char *what, const uint16_t *ref)
{
   const uint16_t *cube = hdr_dt_aces2_cube();
   if (!cube || memcmp(cube, ref, CUBE_BYTES) != 0)
   {
      printf("hdr_cube: %s: the cube is not the one baked in place\n", what);
      failures++;
   }
   else
      printf("hdr_cube: %s: same\n", what);
}

int main(int argc, char **argv)
{
   struct hdr_dt_settings s;
   struct hdr_dt_block b;
   uint16_t *ref200, *ref300, *refp3;
   unsigned fills, i;

   if (argc == 3 && !strcmp(argv[1], "--dump"))
   {
      FILE *f;
      uint16_t *ref = reference(1, 200.0f);
      f = fopen(argv[2], "wb");
      if (!f || fwrite(ref, 1, CUBE_BYTES, f) != CUBE_BYTES)
         return 1;
      fclose(f);
      free(ref);
      return 0;
   }

   tables();
   ref200 = reference(1, 200.0f);
   ref300 = reference(1, 300.0f);
   refp3  = reference(2, 300.0f);

   /* baked through */
   settings(&s, 1);
   hdr_dt_configure(&s);
   fills = fill_until_baked(200.0f);
   printf("hdr_cube: baked in %u fills\n", fills);
   same("baked through", ref200);

   /* the paper white changed with a complete cube: it goes stale at once */
   hdr_dt_fill(&b, 300.0f, 1000.0f, 1000.0f, xy);
   if (b.gm[2] != 0.0f || hdr_dt_aces2_cube())
   {
      printf("hdr_cube: a stale cube still in use\n");
      failures++;
   }
   fill_until_baked(300.0f);
   same("another paper white", ref300);

   /* superseded half way, twice: by paper white, then by the tables */
   hdr_dt_fill(&b, 200.0f, 1000.0f, 1000.0f, xy);
   for (i = 0; i < 3; i++)
      hdr_dt_fill(&b, 250.0f, 1000.0f, 1000.0f, xy);
   settings(&s, 2);
   hdr_dt_configure(&s);
   hdr_dt_fill(&b, 300.0f, 1000.0f, 1000.0f, xy);
   fill_until_baked(300.0f);
   same("superseded half way", refp3);

   /* freed with a bake in flight, then again from nothing */
   settings(&s, 1);
   hdr_dt_configure(&s);
   hdr_dt_fill(&b, 200.0f, 1000.0f, 1000.0f, xy);
   hdr_dt_free();
   hdr_dt_configure(&s);
   fill_until_baked(200.0f);
   same("after a free in flight", ref200);
   hdr_dt_free();

   free(ref200);
   free(ref300);
   free(refp3);
   if (failures)
   {
      printf("hdr_cube: %u failures\n", failures);
      return 1;
   }
   printf("hdr_cube: passed\n");
   return 0;
}
