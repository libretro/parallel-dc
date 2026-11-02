#ifndef PARSE_ARENA_GL_H
#define PARSE_ARENA_GL_H
/* The parse's arena (rend/parse_arena.h) with OpenGL: a vertex and an
 * index buffer for each of a few frames in flight, of immutable storage
 * mapped persistently and coherently in the client's memory (OpenGL 4.4
 * or ARB_buffer_storage), where the frame is parsed and then drawn from:
 * no copy into a buffer for the card.
 *
 * A slot is taken when the fence put after the last frame drawn from it
 * has passed; when none has, the frame is parsed into its own lists and
 * copied, as ever. Nothing waits. Nothing either where the mapping reads
 * back slowly (write-combined memory): the parse reads its vertices back.
 * C89. */
#include <glsm/glsmsym.h>
#include "../parse_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct parse_arena_gl
{
   parse_arena_t arena;
   GLuint verts[PARSE_ARENA_SLOTS];
   GLuint idx[PARSE_ARENA_SLOTS];
   GLsync fence[PARSE_ARENA_SLOTS];
   unsigned next;    /* the slot looked at first */
   int ready;        /* (all zero, as a static one is before it is made: none) */
} parse_arena_gl_t;

/* Made for @slots frames of lists of @vert_room and @idx_room bytes, in
 * the context that is current; false, and nothing kept, where the buffers
 * cannot be made and mapped, or read back slowly */
int parse_arena_gl_init(parse_arena_gl_t *a, size_t vert_room, size_t idx_room, unsigned slots);
void parse_arena_gl_term(parse_arena_gl_t *a);

/* The frame whose lists these are parsed into a free slot, from now till
 * parse_arena_gl_done() (its draws issued) or parse_arena_gl_cancel() (it
 * is not drawn); false where no slot is free, or the lists are not of the
 * arena's size (parse_arena_attach(), @vert_head as there) */
int parse_arena_gl_attach(parse_arena_gl_t *a, ta_list_t *verts, size_t vert_head, ta_list_t *idx);
void parse_arena_gl_done(parse_arena_gl_t *a);
void parse_arena_gl_cancel(parse_arena_gl_t *a);

/* The frame's buffers while it is attached, 0 otherwise */
GLuint parse_arena_gl_vertex_buffer(const parse_arena_gl_t *a);
GLuint parse_arena_gl_index_buffer(const parse_arena_gl_t *a);

#ifdef __cplusplus
}
#endif

#endif
