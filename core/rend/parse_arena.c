/* See parse_arena.h. C89. */
#include <string.h>
#include "parse_arena.h"

static size_t round_up(size_t n, size_t align)
{
   return (n + align - 1) / align * align;
}

size_t parse_arena_layout(parse_arena_t *a, size_t vert_room, size_t idx_room, unsigned slices, size_t align)
{
   memset(a, 0, sizeof(*a));
   a->slot = -1;
   if (slices > PARSE_ARENA_SLOTS)
      return 0;
   if (align == 0)
      align = 1;
   a->vert_room = vert_room;
   a->idx_room = idx_room;
   /* (the sorted passes': as much again as the indices, which is room for
    * more translucent triangles than a frame has; more, and that frame is
    * drawn from a copy) */
   a->sort_room = idx_room;
   a->idx_at = round_up(vert_room, align);
   a->slice = round_up(a->idx_at + idx_room + a->sort_room, align);
   a->slices = slices;
   a->slot = -1;
   return a->slice * slices;
}

void parse_arena_bind(parse_arena_t *a, void *mem)
{
   unsigned i;
   a->mem = (unsigned char *)mem;
   for (i = 0; i < a->slices; i++)
   {
      a->slot_verts[i] = mem != NULL ? a->mem + a->slice * i : NULL;
      a->slot_idx[i] = mem != NULL ? a->mem + a->slice * i + a->idx_at : NULL;
   }
}

void parse_arena_bind_slot(parse_arena_t *a, unsigned slot, void *verts, void *idx)
{
   if (slot >= a->slices)
      return;
   a->slot_verts[slot] = (unsigned char *)verts;
   a->slot_idx[slot] = (unsigned char *)idx;
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

int parse_arena_attach(parse_arena_t *a, unsigned slot, ta_list_t *verts, size_t vert_head, ta_list_t *idx)
{
   if (slot >= a->slices || a->slot_verts[slot] == NULL || a->slot_idx[slot] == NULL || a->slot >= 0
         || (size_t)(verts->end - verts->base) != a->vert_room || (size_t)(idx->end - idx->base) != a->idx_room
         || vert_head > a->vert_room)
      return 0;
   a->own_verts = *verts;
   a->own_idx = *idx;
   a->verts = verts;
   a->idx = idx;
   a->slot = (int)slot;
   move_into(verts, a->slot_verts[slot], a->vert_room, vert_head);
   move_into(idx, a->slot_idx[slot], a->idx_room, 0);
   return 1;
}

void parse_arena_detach(parse_arena_t *a)
{
   if (a->slot < 0)
      return;
   *a->verts = a->own_verts;
   *a->idx = a->own_idx;
   a->verts = a->idx = NULL;
   a->slot = -1;
}

size_t parse_arena_vert_offset(const parse_arena_t *a, unsigned slot)
{
   return a->slice * slot;
}

size_t parse_arena_idx_offset(const parse_arena_t *a, unsigned slot)
{
   return a->slice * slot + a->idx_at;
}
