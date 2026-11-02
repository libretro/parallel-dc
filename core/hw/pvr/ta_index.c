/* See ta_index.h. C89. */
#include <string.h>
#include "ta_index.h"

/* 1e25 and 3.4e37, as their bits: x and y are dropped from 1e25 up (Manic
 * Panic Ghosts), z is from a NaN of either sign or a huge positive value (a
 * negative one is drawn: the base runner views in World Series Baseball
 * 2K1) */
#define HUGE_XY 0x69045951u
#define HUGE_Z  0x7DCCA14Cu

/* Whether the vertex at @v is left out: 1 or 0, without a branch. Asked of
 * the bits, not the values: a float's bits with the sign taken off go up as
 * its size does, with infinity and the NaNs above everything, so one integer
 * comparison says "huge, infinite or not a number". And it has to be the
 * bits: built with fast-math the compiler is told there are no NaNs, and
 * tests of the values for them are dropped. */
static INLINE uint32_t vertex_bad(const unsigned char *v)
{
   uint32_t x, y, z;
   memcpy(&x, v, 4);
   memcpy(&y, v + 4, 4);
   memcpy(&z, v + 8, 4);
   return (uint32_t)((x & 0x7FFFFFFFu) > HUGE_XY) | (uint32_t)((y & 0x7FFFFFFFu) > HUGE_XY)
         | (uint32_t)((z & 0x7FFFFFFFu) > 0x7F800000u) | (uint32_t)(z < 0x80000000u && z > HUGE_Z);
}

void ta_index_strip(ta_index_out_t *o, const void *vtx, size_t stride, uint32_t first, uint32_t count,
      int dupe, uint32_t first_index)
{
   const unsigned char *v = (const unsigned char *)vtx;
   uint32_t bad = 0, i;
   long last_good;

   if (count == 0)
      return;
   for (i = 0; i < count; i++)
      bad |= vertex_bad(v + i * stride);

   /* As a strip nearly always is: every vertex drawn, in order. The first
    * may be put twice - to join this strip to the one before, or to keep
    * the triangles the right way round - and then never again: one index
    * a vertex keeps the count's evenness where the first left it. */
   if (!bad && (size_t)(o->end - o->out) >= (size_t)count + 2)
   {
      uint32_t *out = o->out;
      if (dupe)
         *out++ = first;
      if (((uint32_t)(out - o->base) - first_index) & 1)
         *out++ = first;
      for (i = 0; i < count; i++)
         out[i] = first + i;
      o->out = out + count;
      return;
   }

   /* A vertex left out, or the list nearly full: one at a time, joining
    * the strip across those left out by repeating the last vertex drawn
    * and the next */
   last_good = -1;
   for (i = 0; i < count; i++)
   {
      if (vertex_bad(v + i * stride))
      {
         while (i < count - 1)
         {
            if (!vertex_bad(v + (i + 1) * stride))
            {
               if (last_good >= 0)
               {
                  ta_index_put(o, (uint32_t)last_good);
                  dupe = 1;
               }
               break;
            }
            i++;
         }
      }
      else
      {
         last_good = (long)(first + i);
         if (dupe)
         {
            ta_index_put(o, (uint32_t)last_good);
            dupe = 0;
         }
         if ((i ^ ((uint32_t)(o->out - o->base) - first_index)) & 1)
            ta_index_put(o, (uint32_t)last_good);
         ta_index_put(o, (uint32_t)last_good);
      }
   }
}
