/* The parse's arena (core/rend/parse_arena.c): the slices and their parts
 * laid out apart and aligned, the indices' part with the sorted passes'
 * room after; a frame's lists moved into a slice with what they hold - and
 * the head the parse keeps though the list is empty (the background
 * polygon's vertices: FillBGP()) - their own back after; lists of another
 * size, a second attach, a slot out of range or with a part unbound
 * refused; a frame filling its slice's room overruns into its own slice,
 * not the next; parts bound apart (a buffer each, as OpenGL's). C89.
 *
 *   sh tools/rend/parse_arena_test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parse_arena.h"

static int fails;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define SLICES    3
#define ALIGN     256
#define HEAD      64      /* "four vertices" */

static const size_t room[PARSE_ARENA_PARTS] = { 4096, 1024, 360, 540 };
static const size_t elem[PARSE_ARENA_PARTS] = { 16, 4, 36, 36 };

/* the lists, each with its own memory, and how many bytes each holds */
static int make_lists(ta_list_t *l, ta_list_t **lp, unsigned char *overrun)
{
   unsigned p;
   for (p = 0; p < PARSE_ARENA_PARTS; p++)
   {
      if (!ta_list_init(&l[p], room[p], elem[p], overrun, "list"))
         return 0;
      lp[p] = &l[p];
   }
   return 1;
}

int main(void)
{
   parse_arena_t a;
   ta_list_t l[PARSE_ARENA_PARTS], other;
   ta_list_t *lp[PARSE_ARENA_PARTS];
   unsigned char *own[PARSE_ARENA_PARTS];
   unsigned char overrun = 0;
   unsigned char *mem;
   size_t bytes, i;
   unsigned slot, p;

   bytes = parse_arena_layout(&a, room, SLICES, ALIGN);
   CHECK(bytes == a.slice * SLICES);
   CHECK(a.slice % ALIGN == 0);
   for (p = 0; p < PARSE_ARENA_PARTS; p++)
   {
      CHECK(a.at[p] % ALIGN == 0);
      CHECK(a.room[p] == room[p]);
      CHECK(a.space[p] == room[p] + (p == PARSE_ARENA_IDX ? a.sort_room : 0));
      if (p > 0)
         CHECK(a.at[p] >= a.at[p - 1] + a.space[p - 1]);
   }
   CHECK(a.slice >= a.at[PARSE_ARENA_PARTS - 1] + a.space[PARSE_ARENA_PARTS - 1]);
   CHECK(a.sort_room >= room[PARSE_ARENA_IDX]);
   mem = (unsigned char *)malloc(bytes);
   CHECK(mem != NULL);
   if (mem == NULL || !make_lists(l, lp, &overrun))
      return 1;
   memset(mem, 0xEE, bytes);
   for (p = 0; p < PARSE_ARENA_PARTS; p++)
      own[p] = l[p].base;

   /* nothing to attach to before there is memory */
   CHECK(!parse_arena_attach(&a, 0, lp, HEAD));
   parse_arena_bind(&a, mem);

   for (slot = 0; slot < SLICES; slot++)
   {
      /* the vertices' head written, the list emptied after (as the TA's
       * is); the other lists with one element each */
      for (i = 0; i < HEAD; i++)
         own[PARSE_ARENA_VERTS][i] = (unsigned char)(slot * 16 + i);
      l[PARSE_ARENA_VERTS].cur = l[PARSE_ARENA_VERTS].base;
      for (p = 1; p < PARSE_ARENA_PARTS; p++)
      {
         memset(own[p], (int)(0x40 + slot * 4 + p), elem[p]);
         l[p].cur = l[p].base + elem[p];
      }

      CHECK(parse_arena_attach(&a, slot, lp, HEAD));
      for (p = 0; p < PARSE_ARENA_PARTS; p++)
      {
         CHECK(l[p].base == mem + parse_arena_offset(&a, slot, p));
         CHECK((size_t)(l[p].end - l[p].base) == room[p]);
      }
      CHECK(l[PARSE_ARENA_VERTS].cur == l[PARSE_ARENA_VERTS].base);
      for (i = 0; i < HEAD; i++)
         CHECK(l[PARSE_ARENA_VERTS].base[i] == (unsigned char)(slot * 16 + i));
      for (p = 1; p < PARSE_ARENA_PARTS; p++)
      {
         CHECK(l[p].cur == l[p].base + elem[p]);
         CHECK(l[p].base[0] == (unsigned char)(0x40 + slot * 4 + p) && l[p].base[elem[p] - 1] == l[p].base[0]);
      }
      /* (one frame at a time) */
      CHECK(!parse_arena_attach(&a, slot, lp, HEAD));

      /* the parse fills its parts' space: its slice's only, the others untouched */
      for (p = 0; p < PARSE_ARENA_PARTS; p++)
         memset(l[p].base, (int)(0x11 * (slot + 1) + p), a.space[p]);
      parse_arena_detach(&a);
      for (p = 0; p < PARSE_ARENA_PARTS; p++)
      {
         CHECK(l[p].base == own[p]);
         CHECK((size_t)(l[p].end - l[p].base) == room[p]);
      }
      parse_arena_detach(&a);   /* (twice: nothing) */
      CHECK(l[PARSE_ARENA_VERTS].base == own[PARSE_ARENA_VERTS]);
   }
   for (slot = 0; slot < SLICES; slot++)
      for (p = 0; p < PARSE_ARENA_PARTS; p++)
      {
         const unsigned char *m = mem + parse_arena_offset(&a, slot, p);
         CHECK(m[0] == (unsigned char)(0x11 * (slot + 1) + p) && m[a.space[p] - 1] == m[0]);
      }

   /* refused: a slot out of range, a list of another room, a head past it */
   CHECK(!parse_arena_attach(&a, SLICES, lp, HEAD));
   CHECK(ta_list_init(&other, room[PARSE_ARENA_MV_CLIP] / 2, 36, &overrun, "other"));
   lp[PARSE_ARENA_MV_CLIP] = &other;
   CHECK(!parse_arena_attach(&a, 0, lp, HEAD));
   lp[PARSE_ARENA_MV_CLIP] = &l[PARSE_ARENA_MV_CLIP];
   CHECK(!parse_arena_attach(&a, 0, lp, room[PARSE_ARENA_VERTS] + 1));
   for (p = 0; p < PARSE_ARENA_PARTS; p++)
      CHECK(l[p].base == own[p]);
   ta_list_free(&other);

   /* an overrun in the slice: the list's own flag, and its own slice */
   CHECK(parse_arena_attach(&a, 1, lp, 0));
   overrun = 0;
   l[PARSE_ARENA_VERTS].cur = l[PARSE_ARENA_VERTS].end;
   CHECK(ta_list_append(&l[PARSE_ARENA_VERTS], 16) == mem + parse_arena_offset(&a, 1, PARSE_ARENA_VERTS));
   CHECK(overrun == 1);
   parse_arena_detach(&a);

   /* parts bound apart: their memory, wherever it is; a part unbound,
    * refused */
   {
      static unsigned char part[2][PARSE_ARENA_PARTS][8192];
      parse_arena_t b;
      CHECK(parse_arena_layout(&b, room, 2, 1) != 0);
      CHECK(!parse_arena_attach(&b, 0, lp, HEAD));
      for (p = 0; p < PARSE_ARENA_PARTS; p++)
      {
         parse_arena_bind_slot(&b, 0, p, part[0][p]);
         if (p != PARSE_ARENA_MV_CLIP)
            parse_arena_bind_slot(&b, 1, p, part[1][p]);
      }
      parse_arena_bind_slot(&b, 2, 0, part[0][0]);              /* (no slot 2: nothing) */
      parse_arena_bind_slot(&b, 0, PARSE_ARENA_PARTS, part[0][0]); /* (no such part: nothing) */
      CHECK(!parse_arena_attach(&b, 1, lp, HEAD));
      for (i = 0; i < HEAD; i++)
         own[PARSE_ARENA_VERTS][i] = (unsigned char)(0x80 + i);
      l[PARSE_ARENA_VERTS].cur = l[PARSE_ARENA_VERTS].base;
      CHECK(parse_arena_attach(&b, 0, lp, HEAD));
      for (p = 0; p < PARSE_ARENA_PARTS; p++)
         CHECK(l[p].base == part[0][p]);
      CHECK(memcmp(part[0][PARSE_ARENA_VERTS], own[PARSE_ARENA_VERTS], HEAD) == 0);
      parse_arena_detach(&b);
      for (p = 0; p < PARSE_ARENA_PARTS; p++)
         CHECK(l[p].base == own[p]);
      CHECK(!parse_arena_attach(&b, 2, lp, HEAD));
      /* more slices than there is room for: none */
      CHECK(parse_arena_layout(&b, room, PARSE_ARENA_SLOTS + 1, 1) == 0);
      CHECK(b.slot == -1);
   }

   for (p = 0; p < PARSE_ARENA_PARTS; p++)
      ta_list_free(&l[p]);
   free(mem);
   if (fails)
   {
      printf("parse_arena_test: %d failed\n", fails);
      return 1;
   }
   printf("parse_arena_test: all passed\n");
   return 0;
}
