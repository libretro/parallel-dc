/* Sorting by a float, stably, in memory kept from one sort to the next.
 *
 * A radix sort of (key, index) pairs, the keys the floats' bits made to
 * order as the floats do (sort_key(): -0 as 0, the two being equal to a
 * comparison, so keeping their order), in three passes of 11 bits, a pass
 * all of whose pairs have the same digit skipped; fewer, by merging runs
 * sorted by insertion, where the radix sort's tables would cost more. What
 * a stable sort of the same floats by < gives, for any that are not NaN;
 * and no comparison a pair. C89. */
#ifndef SORT_PAIRS_H
#define SORT_PAIRS_H

#include <string.h>
#include <retro_inline.h>
#include "grow_buf.h"

typedef struct sort_pair
{
   unsigned key;     /* sort_key() */
   unsigned index;   /* what is sorted: kept with its key */
} sort_pair_t;

/* (fewer than this, by insertion; than SORT_PAIRS_MERGE, merged) */
#define SORT_PAIRS_FEW 64
#define SORT_PAIRS_MERGE 512

/* [lo, hi) of @pairs by insertion */
static INLINE void sort_pairs_insert(sort_pair_t *pairs, unsigned lo, unsigned hi)
{
   unsigned i;
   for (i = lo + 1; i < hi; i++)
   {
      const sort_pair_t p = pairs[i];
      unsigned j = i;
      for (; j > lo && pairs[j - 1].key > p.key; j--)
         pairs[j] = pairs[j - 1];
      pairs[j] = p;
   }
}

static INLINE unsigned sort_key(float f)
{
   unsigned bits;
   if (f == 0.f)
      return 0x80000000u;
   memcpy(&bits, &f, 4);
   return (bits & 0x80000000u) ? ~bits : bits | 0x80000000u;
}

/* @n pairs sorted by key, stably, through @tmp (grown as needed): where they
 * are after, @pairs or @tmp's memory; NULL if there is no memory */
static INLINE sort_pair_t *sort_pairs(sort_pair_t *pairs, unsigned n, grow_buf_t *tmp)
{
   sort_pair_t *from, *to, *t;
   unsigned shift, i, b, at, c;
   unsigned counts[2048];

   if (n < SORT_PAIRS_FEW)
   {
      sort_pairs_insert(pairs, 0, n);
      return pairs;
   }
   to = (sort_pair_t *)grow_buf_reserve(tmp, (size_t)n * sizeof(sort_pair_t));
   if (to == NULL)
      return NULL;
   from = pairs;
   if (n < SORT_PAIRS_MERGE)
   {
      unsigned width, lo;
      /* runs of 16 by insertion, then merged, the earlier run first among equals */
      for (lo = 0; lo < n; lo += 16)
         sort_pairs_insert(from, lo, lo + 16 < n ? lo + 16 : n);
      for (width = 16; width < n; width *= 2)
      {
         for (lo = 0; lo < n; lo += 2 * width)
         {
            const unsigned mid = lo + width < n ? lo + width : n;
            const unsigned hi = lo + 2 * width < n ? lo + 2 * width : n;
            unsigned a = lo, b2 = mid, k = lo;
            while (a < mid && b2 < hi)
               to[k++] = from[b2].key < from[a].key ? from[b2++] : from[a++];
            while (a < mid)
               to[k++] = from[a++];
            while (b2 < hi)
               to[k++] = from[b2++];
         }
         t = from;
         from = to;
         to = t;
      }
      return from;
   }
   for (shift = 0; shift < 32; shift += 11)
   {
      memset(counts, 0, sizeof(counts));
      for (i = 0; i < n; i++)
         counts[(from[i].key >> shift) & 2047]++;
      /* (all of one digit: already in order by it) */
      if (counts[(from[0].key >> shift) & 2047] == n)
         continue;
      at = 0;
      for (b = 0; b < 2048; b++)
      {
         c = counts[b];
         counts[b] = at;
         at += c;
      }
      for (i = 0; i < n; i++)
         to[counts[(from[i].key >> shift) & 2047]++] = from[i];
      t = from;
      from = to;
      to = t;
   }
   return from;
}

#endif
