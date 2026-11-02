#ifndef PARSE_ARENA_H
#define PARSE_ARENA_H
/* Where a frame's parse writes what the card draws when the card draws it
 * from there: memory of the renderer's, which the card reads and the
 * processor has in its cache (a device whose memory is the host's, or the
 * client's memory a buffer is mapped in), in slices, one for each of the
 * renderer's frames in flight.
 *
 * A render's frame is parsed into the slice of its frame in flight (the
 * renderer's slot, which is free when the frame before in that slot is
 * done on the card: nothing here waits), and drawn from there. Its lists -
 * the vertices, the indices, the modifier volumes' triangles and those of
 * them cut (rend/modvol_clip.h) - are not copied into a buffer for the
 * card: they are in one. A slice has the room a frame's lists have
 * (ta_ctx.h), so a frame never runs out of it here where it would not have
 * in its own; and the sorted passes' indices go after the frame's, in room
 * of their own.
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

/* A slice's parts: a list of the frame's each */
enum
{
   PARSE_ARENA_VERTS = 0,
   PARSE_ARENA_IDX,
   PARSE_ARENA_MODTRIG,
   PARSE_ARENA_MV_CLIP,
   PARSE_ARENA_PARTS
};

typedef struct parse_arena
{
   unsigned char *mem;                    /* mapped: the slices one after another */
   /* each part of each slice, where it is: in mem, or in a buffer of its
    * own mapped (parse_arena_bind_slot()) */
   unsigned char *part_mem[PARSE_ARENA_SLOTS][PARSE_ARENA_PARTS];
   size_t room[PARSE_ARENA_PARTS];        /* a list's bytes */
   size_t space[PARSE_ARENA_PARTS];       /* ...and a part's: the indices' with the sorted passes' after */
   size_t at[PARSE_ARENA_PARTS];          /* where in a slice a part starts */
   size_t sort_room;                      /* (the sorted passes' bytes) */
   size_t slice;                          /* a slice's bytes */
   unsigned slices;
   /* while a frame's lists are in a slice: which, and the lists as they
    * were, to be put back */
   int slot;
   ta_list_t *lists[PARSE_ARENA_PARTS];
   ta_list_t own[PARSE_ARENA_PARTS];
} parse_arena_t;

/* The layout for @slices (PARSE_ARENA_SLOTS at most) of the rooms @room
 * (a frame's lists', in bytes, by PARSE_ARENA_*), each part of a slice
 * @align bytes aligned: the bytes it takes, 0 for too many slices. No
 * memory yet (parse_arena_bind()). */
size_t parse_arena_layout(parse_arena_t *a, const size_t *room, unsigned slices, size_t align);
void parse_arena_bind(parse_arena_t *a, void *mem);
/* ...or part @part of slice @slot at @mem (space[part] bytes), apart */
void parse_arena_bind_slot(parse_arena_t *a, unsigned slot, unsigned part, void *mem);

/* The frame whose lists these are (@lists, by PARSE_ARENA_*) parsed into
 * slice @slot: what they hold already copied there - and of the vertices,
 * at least the first @vert_head bytes, which the parse keeps though the
 * list is empty (the background polygon's, FillBGP()'s) - and they point
 * there, till parse_arena_detach(). False, and nothing done, where they
 * are not of the rooms the parts are (a machine whose frames are another
 * size). */
int parse_arena_attach(parse_arena_t *a, unsigned slot, ta_list_t *const *lists, size_t vert_head);
/* The frame's own lists back. What was parsed stays in the slice. */
void parse_arena_detach(parse_arena_t *a);

/* Where in the memory part @part of slice @slot is */
size_t parse_arena_offset(const parse_arena_t *a, unsigned slot, unsigned part);

#ifdef __cplusplus
}
#endif

#endif
