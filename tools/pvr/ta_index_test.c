/* ta_index_strip() (core/hw/pvr/ta_index.c) against make_index()'s loop as
 * it was, vertex by vertex, on random strips: vertices at infinity, NaNs
 * and huge ones of each coordinate among them or not, joined to the strip
 * before or not, at every evenness, with lists of every room down to too
 * little - which then mark the frame and start again at their beginning.
 * Every index, where the next goes, and the frame's mark have to be the
 * same. C89.
 *
 *   sh tools/pvr/ta_index_test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ta_index.h"

typedef struct vtx { uint32_t x, y, z, rest[8]; } vtx_t;	/* the core's Vertex: 44 bytes */

#define ROOM 600

/* make_index()'s strip, as it was */
static int ref_inf(const vtx_t *v)
{
   const uint32_t huge_xy = 0x69045951u, huge_z = 0x7DCCA14Cu;
   return (v->x & 0x7fffffff) > huge_xy || (v->y & 0x7fffffff) > huge_xy
         || (v->z & 0x7fffffff) > 0x7f800000 || (v->z < 0x80000000 && v->z > huge_z);
}

typedef struct ref { uint32_t *base, *out, *end; int overrun; } ref_t;

static void ref_put(ref_t *r, uint32_t v)
{
   if (r->out == r->end)
   {
      r->overrun = 1;
      r->out = r->base;
   }
   *r->out++ = v;
}

static void ref_strip(ref_t *r, const vtx_t *vtx, uint32_t first, uint32_t count, int dupe, uint32_t first_index)
{
   long last_good = -1;
   uint32_t i;
   for (i = 0; i < count; i++)
   {
      if (ref_inf(&vtx[i]))
      {
         while (i < count - 1)
         {
            if (!ref_inf(&vtx[i + 1]))
            {
               if (last_good >= 0)
               {
                  ref_put(r, (uint32_t)last_good);
                  dupe = 1;
               }
               break;
            }
            i++;
         }
      }
      else
      {
         uint32_t n;
         last_good = (long)(first + i);
         if (dupe)
         {
            ref_put(r, (uint32_t)last_good);
            dupe = 0;
         }
         n = (uint32_t)(r->out - r->base) - first_index;
         if ((i ^ n) & 1)
            ref_put(r, (uint32_t)last_good);
         ref_put(r, (uint32_t)last_good);
      }
   }
}

static uint32_t rnd_state = 12345;
static uint32_t rnd(void)
{
   rnd_state = rnd_state * 1103515245u + 12345u;
   return rnd_state >> 8;
}

static uint32_t coord(int bad_odds)
{
   static const uint32_t bad[] = { 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00001u, 0x69045952u, 0xE9045952u, 0x7DCCA14Du };
   if (bad_odds != 0 && rnd() % (uint32_t)bad_odds == 0)
      return bad[rnd() % 7];
   return 0x3F800000u + (rnd() & 0xFFFFFu);
}

int main(void)
{
   static vtx_t v[300];
   static uint32_t a[ROOM], b[ROOM];
   unsigned long cases = 0, fast = 0, failed = 0;
   int t;

   for (t = 0; t < 1000000 && failed < 5; t++)
   {
      const uint32_t count = rnd() % 64 + (rnd() % 16 == 0 ? 200 : 0);
      const int bad_odds = rnd() % 3 == 0 ? 0 : (int)(rnd() % 40 + 1);
      const uint32_t room = rnd() % 4 == 0 ? rnd() % (count + 8) + 1 : ROOM;
      const uint32_t used = rnd() % (room < 40 ? room : 40);
      const uint32_t first = rnd() % 1000;
      const int dupe = (int)(rnd() & 1);
      const uint32_t first_index = rnd() % (used + 1);
      unsigned char overrun = 0;
      ta_list_t list;
      ta_index_out_t o;
      ref_t r;
      uint32_t i;

      for (i = 0; i < count; i++)
      {
         v[i].x = coord(bad_odds);
         v[i].y = coord(bad_odds);
         v[i].z = coord(bad_odds);
         /* (a negative z is drawn: not bad) */
         if (rnd() % 8 == 0)
            v[i].z |= 0x80000000u;
      }
      memset(a, 0xEE, sizeof(a));
      memset(b, 0xEE, sizeof(b));

      r.base = a;
      r.out = a + used;
      r.end = a + room;
      r.overrun = 0;
      ref_strip(&r, v, first, count, dupe, first_index);

      list.base = (unsigned char *)b;
      list.cur = (unsigned char *)(b + used);
      list.end = (unsigned char *)(b + room);
      list.overrun = &overrun;
      list.name = NULL;
      o.base = b;
      o.out = b + used;
      o.end = b + room;
      o.list = &list;
      ta_index_strip(&o, v, sizeof(vtx_t), first, count, dupe, first_index);

      cases++;
      if (r.overrun == 0)
         fast++;
      if (memcmp(a, b, sizeof(a)) != 0 || r.out - a != o.out - b || r.overrun != (int)overrun)
      {
         printf("ta_index_test: case %d different: %u vertices, room %u, used %u, dupe %d, first index %u; "
               "ends %ld and %ld, marks %d and %d\n", t, (unsigned)count, (unsigned)room, (unsigned)used, dupe,
               (unsigned)first_index, (long)(r.out - a), (long)(o.out - b), r.overrun, (int)overrun);
         failed++;
      }
   }
   printf("ta_index_test: %lu strips, %lu of them with room, %lu different\n", cases, fast, failed);
   return failed != 0;
}
