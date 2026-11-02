/* See parse_arena.h. C89. */
#include <string.h>
#include "parse_arena.h"

static size_t round_up(size_t n, size_t align)
{
   return (n + align - 1) / align * align;
}

size_t parse_arena_layout(parse_arena_t *a, const size_t *room, unsigned slices, size_t align)
{
   size_t at = 0;
   unsigned p;

   memset(a, 0, sizeof(*a));
   a->slot = -1;
   if (slices > PARSE_ARENA_SLOTS)
      return 0;
   if (align == 0)
      align = 1;
   /* (the sorted passes': as much again as the indices, which is room for
    * more translucent triangles than a frame has; more, and that frame is
    * drawn from a copy) */
   a->sort_room = room[PARSE_ARENA_IDX];
   for (p = 0; p < PARSE_ARENA_PARTS; p++)
   {
      a->room[p] = room[p];
      a->space[p] = room[p] + (p == PARSE_ARENA_IDX ? a->sort_room : 0);
      a->at[p] = at;
      at = round_up(at + a->space[p], align);
   }
   a->slice = at;
   a->slices = slices;
   return a->slice * slices;
}

void parse_arena_bind(parse_arena_t *a, void *mem)
{
   unsigned i, p;
   a->mem = (unsigned char *)mem;
   for (i = 0; i < a->slices; i++)
      for (p = 0; p < PARSE_ARENA_PARTS; p++)
         a->part_mem[i][p] = mem != NULL ? a->mem + a->slice * i + a->at[p] : NULL;
}

void parse_arena_bind_slot(parse_arena_t *a, unsigned slot, unsigned part, void *mem)
{
   if (slot < a->slices && part < PARSE_ARENA_PARTS)
      a->part_mem[slot][part] = (unsigned char *)mem;
}

/* @l pointed at @room bytes at @at, with what it holds - and at least its
 * first @head bytes - copied there */
static void move_into(ta_list_t *l, unsigned char *at, size_t room, size_t head)
{
   const size_t used = (size_t)(l->cur - l->base);
   memcpy(at, l->base, used > head ? used : head);
   l->base = at;
   l->cur = at + used;
   l->end = at + room;
}

int parse_arena_attach(parse_arena_t *a, unsigned slot, ta_list_t *const *lists, size_t vert_head)
{
   unsigned p;

   if (slot >= a->slices || a->slot >= 0 || vert_head > a->room[PARSE_ARENA_VERTS])
      return 0;
   for (p = 0; p < PARSE_ARENA_PARTS; p++)
      if (a->part_mem[slot][p] == NULL || (size_t)(lists[p]->end - lists[p]->base) != a->room[p])
         return 0;
   a->slot = (int)slot;
   for (p = 0; p < PARSE_ARENA_PARTS; p++)
   {
      a->own[p] = *lists[p];
      a->lists[p] = lists[p];
      move_into(lists[p], a->part_mem[slot][p], a->room[p], p == PARSE_ARENA_VERTS ? vert_head : 0);
   }
   return 1;
}

void parse_arena_detach(parse_arena_t *a)
{
   unsigned p;

   if (a->slot < 0)
      return;
   for (p = 0; p < PARSE_ARENA_PARTS; p++)
   {
      *a->lists[p] = a->own[p];
      a->lists[p] = NULL;
   }
   a->slot = -1;
}

size_t parse_arena_offset(const parse_arena_t *a, unsigned slot, unsigned part)
{
   return a->slice * slot + a->at[part];
}
