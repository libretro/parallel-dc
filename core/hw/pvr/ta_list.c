/* See ta_list.h. C89. */
#include <stdlib.h>
#include "ta_list.h"

void (*ta_list_warn)(const char *name) = NULL;

int ta_list_init(ta_list_t *l, size_t bytes, size_t elem, unsigned char *overrun, const char *name)
{
   bytes -= bytes % elem;
   l->base = (unsigned char *)malloc(bytes);
   l->cur = l->base;
   l->end = l->base != NULL ? l->base + bytes : NULL;
   l->overrun = overrun;
   l->name = name;
   return l->base != NULL;
}

void ta_list_free(ta_list_t *l)
{
   free(l->base);
   l->base = l->cur = l->end = NULL;
}

void *ta_list_full(ta_list_t *l, size_t bytes)
{
   *l->overrun = 1;
   if (l->name != NULL && ta_list_warn != NULL)
      ta_list_warn(l->name);
   /* (no list is smaller than what is asked for here: 4 elements at most) */
   l->cur = l->base + bytes;
   return l->base;
}
