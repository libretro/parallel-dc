#ifndef TA_INDEX_H
#define TA_INDEX_H
/* A strip's indices, for make_index() (ta_vtx.cpp): the vertices that are
 * drawn, each once, and where two must be to keep the strip's triangles
 * the right way round or to join it to the one before. C89. */
#include <stddef.h>
#include <stdint.h>
#include "ta_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the indices go: the next place and the end of the room, kept here
 * for a whole list of strips and given back to @list at the end; @base is
 * the list's first index */
typedef struct ta_index_out
{
   uint32_t *out;
   uint32_t *end;
   uint32_t *base;
   ta_list_t *list;
} ta_index_out_t;

/* One index put: the list full, the frame is marked and it starts again
 * at its beginning (only staying inside it matters then) */
static INLINE void ta_index_put(ta_index_out_t *o, uint32_t v)
{
   if (o->out == o->end)
   {
      o->list->cur = (unsigned char *)o->end;
      o->out = (uint32_t *)ta_list_full(o->list, 0);
      o->end = (uint32_t *)o->list->end;
   }
   *o->out++ = v;
}

/* The strip of @count vertices of @stride bytes from @vtx, the frame's
 * vertex @first on; @first_index the index the strip's draw starts at;
 * @dupe: the strip is joined to the one before, whose last index has just
 * been repeated. A vertex at infinity, or a NaN, is left out, and the
 * strip goes on past it. */
void ta_index_strip(ta_index_out_t *o, const void *vtx, size_t stride, uint32_t first, uint32_t count,
      int dupe, uint32_t first_index);

#ifdef __cplusplus
}
#endif

#endif
