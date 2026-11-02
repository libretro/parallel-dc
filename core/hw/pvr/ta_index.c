/* See ta_index.h. C89. */
#include <string.h>
#include "ta_index.h"

int ta_strip_ok_ignored = 0;

void ta_strip_ok_bind(ta_strip_ok_t *s, uint32_t *words, uint32_t polys)
{
   s->bits = words;
   s->polys = polys;
   s->hi = (polys + 31) / 32;
   ta_strip_ok_clear(s);
}

void ta_strip_ok_clear(ta_strip_ok_t *s)
{
   if (s->hi != 0)
      memset(s->bits, 0, s->hi * sizeof(uint32_t));
   s->hi = 0;
}

/* Whether the vertex at @v is left out (ta_vertex_bad()) */
static INLINE uint32_t vertex_bad(const unsigned char *v)
{
   uint32_t x, y, z;
   memcpy(&x, v, 4);
   memcpy(&y, v + 4, 4);
   memcpy(&z, v + 8, 4);
   return ta_vertex_bad(x, y, z);
}

void ta_index_strip(ta_index_out_t *o, const void *vtx, size_t stride, uint32_t first, uint32_t count,
      int dupe, uint32_t first_index, int known_good)
{
   const unsigned char *v = (const unsigned char *)vtx;
   uint32_t bad = 0, i;
   long last_good;

   if (count == 0)
      return;
   if (!known_good)
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
