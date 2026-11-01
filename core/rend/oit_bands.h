/* How many horizontal bands the Vulkan per-pixel renderer draws a frame in
 * (OITDrawer::Draw()), so that the translucent fragments of one band fit in
 * the fragment buffer (rend/oit_size.h): from the counts of the frames drawn
 * before, which the CPU reads once a frame slot's fence has passed.
 *
 * The screen's frames and renders to a texture take turns at the frame
 * slots, as many of each as the game draws, and each kind has bands of its
 * own: what a slot counted goes to the kind of frame that was drawn in it,
 * whichever kind comes to the slot next. More bands at once when it had
 * fragments that found no room; fewer when a second of frames in a row
 * would have fitted in fewer. Each band is taken to need what the fullest
 * one did, with a quarter more room. C89. */
#ifndef OIT_BANDS_H
#define OIT_BANDS_H

#include <string.h>
#include <retro_inline.h>

#define OIT_BANDS_MAX 64
/* frame slots: the command pool's, which are the swap chain's */
#define OIT_BANDS_SLOTS 8
/* frames in a row that would have fitted in fewer bands before there are */
#define OIT_BANDS_ROOMY 60

typedef struct oit_bands
{
   unsigned bands[2];                     /* the screen's [0], renders to a texture [1] */
   unsigned roomy[2];                     /* frames in a row that would have fitted in fewer */
   unsigned drawn_in[OIT_BANDS_SLOTS];    /* the bands each slot's frame was drawn in */
   unsigned char rtt[OIT_BANDS_SLOTS];    /* each slot's frame was a render to a texture */
} oit_bands_t;

static INLINE void oit_bands_init(oit_bands_t *b)
{
   memset(b, 0, sizeof(*b));
   b->bands[0] = 1;
   b->bands[1] = 1;
}

/* What the frame last drawn in @slot counted: @fragments, the most one of
 * its render passes asked for, with room for @room. Its kind's bands; 1 if
 * they changed. */
static INLINE int oit_bands_count(oit_bands_t *b, unsigned slot, unsigned fragments, unsigned room)
{
   const unsigned kind = b->rtt[slot] ? 1 : 0;
   const unsigned drawn_in = b->drawn_in[slot] != 0 ? b->drawn_in[slot] : 1;
   const unsigned was = b->bands[kind];
   unsigned long long frame, wanted;

   if (room == 0)
      room = 1;
   frame = (unsigned long long)fragments * drawn_in;
   wanted = (frame + frame / 4 + room - 1) / room;
   if (wanted > OIT_BANDS_MAX)
      wanted = OIT_BANDS_MAX;
   if (wanted < 1)
      wanted = 1;
   if (fragments > room)
   {
      unsigned bands = b->bands[kind];
      if (bands < (unsigned)wanted)
         bands = (unsigned)wanted;
      if (bands < drawn_in + 1)
         bands = drawn_in + 1;
      b->bands[kind] = bands < OIT_BANDS_MAX ? bands : OIT_BANDS_MAX;
      b->roomy[kind] = 0;
   }
   else if ((unsigned)wanted < b->bands[kind])
   {
      if (++b->roomy[kind] >= OIT_BANDS_ROOMY)
      {
         b->bands[kind] = (unsigned)wanted;
         b->roomy[kind] = 0;
      }
   }
   else
      b->roomy[kind] = 0;
   return b->bands[kind] != was;
}

/* A frame about to be drawn in @slot, a render to a texture if @rtt: the
 * bands it is drawn in, which the slot keeps for its count */
static INLINE unsigned oit_bands_begin(oit_bands_t *b, unsigned slot, int rtt)
{
   const unsigned kind = rtt ? 1 : 0;
   b->rtt[slot] = (unsigned char)kind;
   b->drawn_in[slot] = b->bands[kind];
   return b->bands[kind];
}

#endif
