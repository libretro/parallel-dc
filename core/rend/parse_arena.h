#ifndef PARSE_ARENA_H
#define PARSE_ARENA_H
/* Where a frame's parse writes its vertices and indices when the card
 * draws them from there: memory of the renderer's, which the card reads
 * and the processor has in its cache (a device whose memory is the
 * host's), in slices, one for each of the renderer's frames in flight.
 *
 * A render's frame is parsed into the slice of its frame in flight (the
 * renderer's command pool's slot, which is free when the frame before in
 * that slot is done on the card: nothing here waits), and drawn from there.
 * The vertices are not copied into a buffer for the card: they are in one.
 * A slice is the room a frame's lists have (ta_ctx.h), so a frame never
 * runs out of it here where it would not have in its own; and the sorted
 * passes' indices go after the frame's, in room of their own.
 *
 * The frame's own lists are put back when it has been drawn: they are the
 * frame's, and go with it. C89. */
#include <stddef.h>
#include "../hw/pvr/ta_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* (most slices an arena has) */
#define PARSE_ARENA_SLOTS 8

typedef struct parse_arena
{
   unsigned char *mem;        /* mapped: the slices one after another */
   /* ...or a slice's vertices and indices each where a buffer of their
    * own is mapped (parse_arena_bind_slot()) */
   unsigned char *slot_verts[PARSE_ARENA_SLOTS];
   unsigned char *slot_idx[PARSE_ARENA_SLOTS];
   size_t vert_room;          /* a slice's vertices' bytes */
   size_t idx_room;           /* ...its indices' */
   size_t sort_room;          /* ...and the sorted passes', after them */
   size_t idx_at;             /* where in a slice the indices start */
   size_t slice;              /* a slice's bytes */
   unsigned slices;
   /* while a frame's lists are in a slice: which, and the lists as they
    * were, to be put back */
   int slot;
   ta_list_t *verts, *idx;
   ta_list_t own_verts, own_idx;
} parse_arena_t;

/* The layout for @slices (PARSE_ARENA_SLOTS at most) of rooms of
 * @vert_room and @idx_room bytes (a frame's lists'), each part of a slice
 * @align bytes aligned: the bytes it takes, 0 for too many slices. No
 * memory yet (parse_arena_bind()). */
size_t parse_arena_layout(parse_arena_t *a, size_t vert_room, size_t idx_room, unsigned slices, size_t align);
void parse_arena_bind(parse_arena_t *a, void *mem);
/* ...or slice @slot's vertices at @verts (vert_room bytes) and its indices
 * at @idx (idx_room bytes, and sort_room after them where the sorted
 * passes go there), apart */
void parse_arena_bind_slot(parse_arena_t *a, unsigned slot, void *verts, void *idx);

/* The frame whose lists these are parsed into slice @slot: what they hold
 * already copied there - and of the vertices, at least the first
 * @vert_head bytes, which the parse keeps though the list is empty (the
 * background polygon's, FillBGP()'s) - and they point there, till
 * parse_arena_detach(). False, and nothing done, where they are not of
 * the room the slices are (a machine whose frames are another size). */
int parse_arena_attach(parse_arena_t *a, unsigned slot, ta_list_t *verts, size_t vert_head, ta_list_t *idx);
/* The frame's own lists back. What was parsed stays in the slice. */
void parse_arena_detach(parse_arena_t *a);

/* Where in the memory slice @slot's vertices and indices are */
size_t parse_arena_vert_offset(const parse_arena_t *a, unsigned slot);
size_t parse_arena_idx_offset(const parse_arena_t *a, unsigned slot);

#ifdef __cplusplus
}
#endif

#endif
