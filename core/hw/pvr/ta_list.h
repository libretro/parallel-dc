#ifndef TA_LIST_H
#define TA_LIST_H
/* A list a frame's parse appends to: a block of fixed room, where the
 * elements go one after another, and where the next one goes. C89, for C
 * and C++ alike; List<T> (helper_classes.h) is the typed face C++ sees.
 *
 * It is kept as three places - the first element, the next one, the end
 * of the room - so that appending reads two of them and writes one; a
 * count of the room left beside the place written both on every append.
 * What is used and what is left are worked out when asked for, a few
 * times a strip.
 *
 * Full, the list starts again at its beginning and marks the frame
 * (*overrun), which is then not drawn: what is appended after only has to
 * stay inside it (ta_list_full()). */
#include <stddef.h>
#include <retro_inline.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_list
{
   unsigned char *base;      /* the first element */
   unsigned char *cur;       /* where the next one goes */
   unsigned char *end;       /* the end of the room */
   unsigned char *overrun;   /* the frame's flag: set when the list is full */
   const char *name;         /* for the warning; NULL: none */
} ta_list_t;

/* Said once a list is found full, if set (the core's log) */
extern void (*ta_list_warn)(const char *name);

/* The room for @bytes, rounded down to whole elements of @elem bytes, and
 * empty; false if there is no memory */
int ta_list_init(ta_list_t *l, size_t bytes, size_t elem, unsigned char *overrun, const char *name);
void ta_list_free(ta_list_t *l);
/* The list is full: the frame is marked, and @bytes are taken from its
 * beginning (ta_list_append()'s slow way) */
void *ta_list_full(ta_list_t *l, size_t bytes);

/* @bytes appended: where they go */
static INLINE void *ta_list_append(ta_list_t *l, size_t bytes)
{
   unsigned char *rv = l->cur;
   if ((size_t)(l->end - rv) >= bytes)
   {
      l->cur = rv + bytes;
      return rv;
   }
   return ta_list_full(l, bytes);
}

static INLINE void ta_list_clear(ta_list_t *l)
{
   l->cur = l->base;
}

#ifdef __cplusplus
}
#endif

#endif
