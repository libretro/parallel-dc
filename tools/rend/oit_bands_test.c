/* The Vulkan per-pixel renderer's bands (core/rend/oit_bands.h), with the
 * frame slots of the command pool and the counts the GPU hands back
 * simulated: each render takes the next slot, reads what the render drawn
 * there before counted, draws its translucent fragments in the bands it is
 * given (a band holding its share of them, the fullest one counted) and
 * leaves its own count in the slot.
 *
 * Renders to a texture and the screen's frames take turns at the slots,
 * any number of renders to a texture a frame, over every count of slots
 * the command pool has. Checked, for each:
 *  - a kind whose fragments do not fit is drawn in enough bands within a
 *    few rounds of the slots, and none of its renders drops any after
 *    that, whatever the other kind needs;
 *  - a kind that fits is never drawn in more than one band because of the
 *    other;
 *  - when a kind needs less again, it goes back to fewer bands after
 *    OIT_BANDS_ROOMY of its renders would have fitted, and not before.
 *
 *   sh tools/rend/oit_bands_test.sh
 *
 * C89. */
#include <stdio.h>
#include <stdlib.h>
#include "../../core/rend/oit_bands.h"

#define ROOM 100000u

typedef struct sim
{
   oit_bands_t b;
   unsigned count[OIT_BANDS_SLOTS];      /* the fullest band's fragments */
   unsigned written[OIT_BANDS_SLOTS];    /* the slot has a count */
   unsigned slot;
   unsigned slots;
} sim_t;

static int failures;

static void sim_init(sim_t *s, unsigned slots)
{
   unsigned i;
   oit_bands_init(&s->b);
   for (i = 0; i < OIT_BANDS_SLOTS; i++)
   {
      s->count[i] = 0;
      s->written[i] = 0;
   }
   s->slot = 0;
   s->slots = slots;
}

/* A render of @need translucent fragments: the bands it was drawn in, and
 * in @dropped whether a band had more than there is room for */
static unsigned sim_render(sim_t *s, int rtt, unsigned need, int *dropped)
{
   unsigned bands, per;

   s->slot = (s->slot + 1) % s->slots;
   if (s->written[s->slot])
      oit_bands_count(&s->b, s->slot, s->count[s->slot], ROOM);
   s->written[s->slot] = 0;
   bands = oit_bands_begin(&s->b, s->slot, rtt);
   per = (need + bands - 1) / bands;
   s->count[s->slot] = per;
   s->written[s->slot] = 1;
   s->b.drawn_in[s->slot] = bands;
   *dropped = per > ROOM;
   return bands;
}

/* What the game draws each frame: @rtts renders to a texture of @rtt_need
 * fragments, then the screen's of @screen_need; @frames of them. Fails if
 * a render drops fragments after @settle frames, or, with @fit, a render
 * to a texture that fits is drawn in bands then. The bands each kind was
 * last drawn in, in @last. */
static void run(sim_t *s, const char *what, unsigned frames, unsigned settle, int fit, unsigned rtts,
      unsigned rtt_need, unsigned screen_need, unsigned last[2])
{
   unsigned f, r;
   int dropped;

   for (f = 0; f < frames; f++)
   {
      for (r = 0; r < rtts; r++)
      {
         last[1] = sim_render(s, 1, rtt_need, &dropped);
         if (dropped && f >= settle)
         {
            printf("oit_bands_test: FAIL: %s, %u slots, %u renders to a texture a frame: frame %u, a render to a texture of %u fragments dropped some in %u band(s)\n",
                  what, s->slots, rtts, f, rtt_need, last[1]);
            failures++;
            return;
         }
         if (fit && rtt_need <= ROOM && last[1] != 1 && f >= settle)
         {
            printf("oit_bands_test: FAIL: %s, %u slots: a render to a texture that fits drawn in %u bands\n",
                  what, s->slots, last[1]);
            failures++;
            return;
         }
      }
      last[0] = sim_render(s, 0, screen_need, &dropped);
      if (dropped && f >= settle)
      {
         printf("oit_bands_test: FAIL: %s, %u slots, %u renders to a texture a frame: frame %u, the screen's %u fragments dropped some in %u band(s)\n",
               what, s->slots, rtts, f, screen_need, last[0]);
         failures++;
         return;
      }
   }
}

int main(void)
{
   static const unsigned needs[][2] = {
      /* render to a texture, screen */
      { ROOM / 5, ROOM * 7 / 2 },
      { ROOM * 5 / 2, ROOM / 5 },
      { ROOM * 3, ROOM * 9 / 2 },
      { ROOM / 2, ROOM / 2 },
   };
   unsigned slots, rtts, n, cases = 0;

   for (slots = 1; slots <= OIT_BANDS_SLOTS; slots++)
      for (rtts = 0; rtts <= 3; rtts++)
         for (n = 0; n < sizeof(needs) / sizeof(needs[0]); n++)
         {
            sim_t s;
            unsigned last[2] = { 1, 1 };
            /* a few rounds of the slots, renders of either kind */
            const unsigned settle = 4 * OIT_BANDS_SLOTS;
            const int before = failures;

            if (rtts == 0 && needs[n][1] <= ROOM)
               continue;
            sim_init(&s, slots);
            run(&s, "overflow", 300, settle, 1, rtts, needs[n][0], needs[n][1], last);
            if (failures != before)
               continue;
            if (needs[n][1] <= ROOM && last[0] != 1)
            {
               printf("oit_bands_test: FAIL: %u slots, %u renders to a texture: the screen fits and is drawn in %u bands\n",
                     slots, rtts, last[0]);
               failures++;
               continue;
            }
            if (rtts != 0 && needs[n][0] <= ROOM && last[1] != 1)
            {
               printf("oit_bands_test: FAIL: %u slots: renders to a texture fit and are drawn in %u bands\n",
                     slots, last[1]);
               failures++;
               continue;
            }
            /* the same frames needing little: fewer bands, not at once */
            if (needs[n][1] > ROOM)
            {
               const unsigned was = last[0];
               run(&s, "after", OIT_BANDS_ROOMY / 2, 0, 0, rtts, ROOM / 5, ROOM / 5, last);
               if (failures != before)
                  continue;
               if (last[0] != was)
               {
                  printf("oit_bands_test: FAIL: %u slots, %u renders to a texture: %u bands down to %u after %u frames that fit, fewer than %u\n",
                        slots, rtts, was, last[0], OIT_BANDS_ROOMY / 2, OIT_BANDS_ROOMY);
                  failures++;
                  continue;
               }
               run(&s, "after", 2 * OIT_BANDS_ROOMY, 0, 0, rtts, ROOM / 5, ROOM / 5, last);
               if (failures != before)
                  continue;
               if (last[0] != 1)
               {
                  printf("oit_bands_test: FAIL: %u slots, %u renders to a texture: still %u bands after %u frames that fit\n",
                        slots, rtts, last[0], 3 * OIT_BANDS_ROOMY / 2);
                  failures++;
                  continue;
               }
            }
            cases++;
         }
   if (failures != 0)
   {
      printf("oit_bands_test: FAIL (%d)\n", failures);
      return 1;
   }
   printf("oit_bands_test: PASS (%u cases)\n", cases);
   return 0;
}
