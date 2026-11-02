/* The parse's arena (core/rend/parse_arena.c): the slices laid out apart
 * and aligned; a frame's lists moved into a slice with what they hold -
 * and the head the parse keeps though the list is empty (the background
 * polygon's vertices: FillBGP()) - their own back after; lists of another
 * size, a second attach, a slot out of range refused; a frame filling its
 * slice's room overruns into its own slice, not the next. C89.
 *
 *   sh tools/rend/parse_arena_test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parse_arena.h"

static int fails;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define VERT_ROOM 4096
#define IDX_ROOM  1024
#define SLICES    3
#define ALIGN     256
#define HEAD      64      /* "four vertices" */

int main(void)
{
   parse_arena_t a;
   ta_list_t verts, idx, other;
   unsigned char overrun = 0;
   unsigned char *mem;
   unsigned char *own_verts, *own_idx;
   size_t bytes, i;
   unsigned slot;

   bytes = parse_arena_layout(&a, VERT_ROOM, IDX_ROOM, SLICES, ALIGN);
   CHECK(bytes == a.slice * SLICES);
   CHECK(a.slice % ALIGN == 0 && a.idx_at % ALIGN == 0);
   CHECK(a.idx_at >= VERT_ROOM && a.slice >= a.idx_at + IDX_ROOM + a.sort_room);
   CHECK(a.sort_room >= IDX_ROOM);
   mem = (unsigned char *)malloc(bytes);
   CHECK(mem != NULL);
   if (mem == NULL)
      return 1;
   memset(mem, 0xEE, bytes);

   /* nothing to attach to before there is memory */
   CHECK(ta_list_init(&verts, VERT_ROOM, 16, &overrun, "verts"));
   CHECK(ta_list_init(&idx, IDX_ROOM, 4, &overrun, "idx"));
   CHECK(!parse_arena_attach(&a, 0, &verts, HEAD, &idx));
   parse_arena_bind(&a, mem);
   own_verts = verts.base;
   own_idx = idx.base;

   for (slot = 0; slot < SLICES; slot++)
   {
      /* the head written, the list emptied after (as the TA's is) */
      for (i = 0; i < HEAD; i++)
         own_verts[i] = (unsigned char)(slot * 16 + i);
      verts.cur = verts.base;
      /* one index already there */
      own_idx[0] = (unsigned char)(0x40 + slot);
      own_idx[1] = own_idx[2] = own_idx[3] = 0;
      idx.cur = idx.base + 4;

      CHECK(parse_arena_attach(&a, slot, &verts, HEAD, &idx));
      CHECK(verts.base == mem + parse_arena_vert_offset(&a, slot));
      CHECK(idx.base == mem + parse_arena_idx_offset(&a, slot));
      CHECK(verts.cur == verts.base && (size_t)(verts.end - verts.base) == VERT_ROOM);
      CHECK(idx.cur == idx.base + 4 && (size_t)(idx.end - idx.base) == IDX_ROOM);
      for (i = 0; i < HEAD; i++)
         CHECK(verts.base[i] == (unsigned char)(slot * 16 + i));
      CHECK(idx.base[0] == 0x40 + slot);
      /* (one frame at a time) */
      CHECK(!parse_arena_attach(&a, slot, &verts, HEAD, &idx));

      /* the parse fills its room: its slice's only, the others untouched */
      memset(verts.base, 0x11 * (slot + 1), VERT_ROOM);
      memset(idx.base, 0x22 * (slot + 1), IDX_ROOM + a.sort_room);
      parse_arena_detach(&a);
      CHECK(verts.base == own_verts && idx.base == own_idx);
      CHECK((size_t)(verts.end - verts.base) == VERT_ROOM);
      parse_arena_detach(&a);   /* (twice: nothing) */
      CHECK(verts.base == own_verts);
   }
   for (slot = 0; slot < SLICES; slot++)
   {
      const unsigned char *v = mem + parse_arena_vert_offset(&a, slot);
      const unsigned char *x = mem + parse_arena_idx_offset(&a, slot);
      CHECK(v[0] == 0x11 * (slot + 1) && v[VERT_ROOM - 1] == 0x11 * (slot + 1));
      CHECK(x[0] == 0x22 * (slot + 1) && x[IDX_ROOM + a.sort_room - 1] == 0x22 * (slot + 1));
   }

   /* refused: a slot out of range, lists of another room, a head past it */
   CHECK(!parse_arena_attach(&a, SLICES, &verts, HEAD, &idx));
   CHECK(ta_list_init(&other, VERT_ROOM / 2, 16, &overrun, "verts"));
   CHECK(!parse_arena_attach(&a, 0, &other, HEAD, &idx));
   CHECK(!parse_arena_attach(&a, 0, &verts, VERT_ROOM + 1, &idx));
   CHECK(verts.base == own_verts && idx.base == own_idx && other.base != NULL);
   ta_list_free(&other);

   /* an overrun in the slice: the list's own flag, and its own slice */
   CHECK(parse_arena_attach(&a, 1, &verts, 0, &idx));
   overrun = 0;
   verts.cur = verts.end;
   CHECK(ta_list_append(&verts, 16) == mem + parse_arena_vert_offset(&a, 1));
   CHECK(overrun == 1);
   parse_arena_detach(&a);

   /* slices bound apart (a buffer of their own each, as OpenGL's): their
    * memory, wherever it is */
   {
      static unsigned char vmem[2][VERT_ROOM], imem[2][IDX_ROOM];
      parse_arena_t b;
      CHECK(parse_arena_layout(&b, VERT_ROOM, IDX_ROOM, 2, 1) != 0);
      CHECK(!parse_arena_attach(&b, 0, &verts, HEAD, &idx));
      parse_arena_bind_slot(&b, 0, vmem[0], imem[0]);
      parse_arena_bind_slot(&b, 1, vmem[1], imem[1]);
      parse_arena_bind_slot(&b, 2, vmem[0], imem[0]);   /* (no slot 2: nothing) */
      for (i = 0; i < HEAD; i++)
         own_verts[i] = (unsigned char)(0x80 + i);
      verts.cur = verts.base;
      idx.cur = idx.base;
      CHECK(parse_arena_attach(&b, 1, &verts, HEAD, &idx));
      CHECK(verts.base == vmem[1] && idx.base == imem[1]);
      CHECK(memcmp(vmem[1], own_verts, HEAD) == 0);
      parse_arena_detach(&b);
      CHECK(verts.base == own_verts && idx.base == own_idx);
      CHECK(!parse_arena_attach(&b, 2, &verts, HEAD, &idx));
      /* more slices than there is room for: none */
      CHECK(parse_arena_layout(&b, VERT_ROOM, IDX_ROOM, PARSE_ARENA_SLOTS + 1, 1) == 0);
      CHECK(b.slot == -1);
   }

   ta_list_free(&verts);
   ta_list_free(&idx);
   free(mem);
   if (fails)
   {
      printf("parse_arena_test: %d failed\n", fails);
      return 1;
   }
   printf("parse_arena_test: all passed\n");
   return 0;
}
