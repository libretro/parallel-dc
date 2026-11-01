#ifndef GROW_BUF_H
#define GROW_BUF_H
/* Memory that is only ever grown: a block and how large it is. What a
 * frame needs is had with grow_buf_reserve() each frame, which does nothing
 * once the block is as large as the frames need - no allocation, no copy,
 * no zeroing of what is about to be written over (the contents are not
 * kept when it grows; grow_buf_reserve_keep() keeps them). C89, for C and
 * C++ alike. */
#include <stdlib.h>
#include <string.h>
#include <retro_inline.h>

typedef struct grow_buf
{
   void *data;
   size_t size;      /* bytes */
} grow_buf_t;

#define GROW_BUF_INIT { NULL, 0 }

/* The next size for @bytes: a power of two, at least 4 KB */
static INLINE size_t grow_buf_size(size_t have, size_t bytes)
{
   size_t n = have != 0 ? have : 4096;
   while (n < bytes)
      n *= 2;
   return n;
}

/* At least @bytes, what was there not kept; NULL if there is no memory */
static INLINE void *grow_buf_reserve(grow_buf_t *b, size_t bytes)
{
   if (bytes > b->size)
   {
      const size_t n = grow_buf_size(b->size, bytes);
      free(b->data);
      b->data = malloc(n);
      b->size = b->data != NULL ? n : 0;
   }
   return b->data;
}

/* The same, what was there kept */
static INLINE void *grow_buf_reserve_keep(grow_buf_t *b, size_t bytes)
{
   if (bytes > b->size)
   {
      const size_t n = grow_buf_size(b->size, bytes);
      void *p = realloc(b->data, n);
      if (p == NULL)
         return NULL;
      b->data = p;
      b->size = n;
   }
   return b->data;
}

static INLINE void grow_buf_free(grow_buf_t *b)
{
   free(b->data);
   b->data = NULL;
   b->size = 0;
}

/* The two blocks change places (a frame's and the one before's) */
static INLINE void grow_buf_swap(grow_buf_t *a, grow_buf_t *b)
{
   const grow_buf_t t = *a;
   *a = *b;
   *b = t;
}

#endif
