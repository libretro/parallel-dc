/* sort_pairs() (core/rend/sort_pairs.h), the translucent sorter's sort,
 * against a stable merge sort of the same floats by <: the same order, for
 * lists of every size about the insertion, merge and radix paths'
 * boundaries and far past them, of depths alike and not, with -0 and 0, infinities, numbers
 * too small to be normal and many equal ones (whose order is the order they
 * were in). Then each kind of list is sorted again into the memory the last
 * sort grew, as the sorter does from one frame to the next.
 *
 *   sh tools/rend/sort_pairs_test.sh
 *
 * C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/rend/sort_pairs.h"

static unsigned long rng = 12345;

static unsigned next_rand(void)
{
   rng = rng * 1103515245ul + 12345ul;
   return (unsigned)((rng >> 8) & 0xFFFFFFul);
}

/* indices [0, n) stably sorted by z[] with <, through tmp */
static void ref_sort(const float *z, unsigned *idx, unsigned *tmp, unsigned n)
{
   unsigned width, lo, i;
   for (i = 0; i < n; i++)
      idx[i] = i;
   for (width = 1; width < n; width *= 2)
   {
      for (lo = 0; lo < n; lo += 2 * width)
      {
         unsigned mid = lo + width < n ? lo + width : n;
         unsigned hi = lo + 2 * width < n ? lo + 2 * width : n;
         unsigned a = lo, b = mid, k = lo;
         while (a < mid && b < hi)
            tmp[k++] = z[idx[b]] < z[idx[a]] ? idx[b++] : idx[a++];
         while (a < mid)
            tmp[k++] = idx[a++];
         while (b < hi)
            tmp[k++] = idx[b++];
      }
      memcpy(idx, tmp, n * sizeof(unsigned));
   }
}

static float make_depth(int kind, unsigned i)
{
   static const float odd[] = { 0.f, -0.f, 1e-40f, -1e-40f, 1.f / 0.f, -1.f / 0.f,
      1.f, -1.f, 3.4e38f, -3.4e38f, 1e-38f, 0.5f };
   unsigned r = next_rand();
   switch (kind)
   {
   case 0:  /* depths as games have them: 1/w, in (0, 1] mostly */
      return (float)(r + 1) / 16777216.f;
   case 1:  /* few different ones: many equal */
      return (float)(r % 7) * 0.125f;
   case 2:  /* the odd ones */
      return odd[r % (sizeof(odd) / sizeof(odd[0]))];
   case 3:  /* any sign and size */
      return ((r & 1) ? -1.f : 1.f) * (float)(r >> 1) * (float)(1 << (r % 24)) * 1e-6f;
   case 4:  /* all the same */
      return 0.25f;
   default: /* already in order, then against it */
      return i < 5000 ? (float)i : -(float)i;
   }
}

int main(void)
{
   static const unsigned sizes[] = { 0, 1, 2, 3, 15, 16, 17, 31, 62, 63, 64, 65, 100, 511, 512, 513, 1000, 2047, 2048,
      2049, 4096, 20000, 100000 };
   const unsigned most = 100000;
   float *z = (float *)malloc(most * sizeof(float));
   unsigned *idx = (unsigned *)malloc(most * sizeof(unsigned));
   unsigned *tmp = (unsigned *)malloc(most * sizeof(unsigned));
   sort_pair_t *pairs = (sort_pair_t *)malloc(most * sizeof(sort_pair_t));
   grow_buf_t sortTmp = GROW_BUF_INIT;
   unsigned s, i, cases = 0, fails = 0;
   int kind, round;

   if (z == NULL || idx == NULL || tmp == NULL || pairs == NULL)
      return 2;
   for (round = 0; round < 2; round++)
      for (kind = 0; kind < 6; kind++)
         for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
         {
            const unsigned n = sizes[s];
            sort_pair_t *out;
            for (i = 0; i < n; i++)
            {
               z[i] = make_depth(kind, i);
               pairs[i].key = sort_key(z[i]);
               pairs[i].index = i;
            }
            ref_sort(z, idx, tmp, n);
            out = sort_pairs(pairs, n, &sortTmp);
            cases++;
            if (out == NULL && n >= SORT_PAIRS_FEW)
            {
               printf("FAIL: no memory for %u\n", n);
               fails++;
               continue;
            }
            for (i = 0; i < n; i++)
               if (out[i].index != idx[i])
               {
                  printf("FAIL: kind %d, %u depths: at %u, %u (%g), sorted by < %u (%g)\n",
                        kind, n, i, out[i].index, z[out[i].index], idx[i], z[idx[i]]);
                  fails++;
                  break;
               }
         }
   grow_buf_free(&sortTmp);
   free(z);
   free(idx);
   free(tmp);
   free(pairs);
   printf("sort_pairs_test: %u lists, %u different from a stable sort by <\n", cases, fails);
   return fails != 0;
}
