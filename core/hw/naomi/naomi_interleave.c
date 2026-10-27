/* See naomi_interleave.h. C89. */
#include <string.h>

#include "naomi_interleave.h"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define NAOMI_INTERLEAVE_SSE2
#endif

void naomi_copy_interleaved(uint16_t *to, const uint8_t *blob, uint32_t len)
{
   const uint8_t *from = blob;
   uint32_t       i    = len / 2;

#ifdef NAOMI_INTERLEAVE_SSE2
   /* eight words at a time: spread out with nothing between them, and
    * put over what is there with the other ROM's words kept */
   {
      const __m128i zero  = _mm_setzero_si128();
      const __m128i other = _mm_set1_epi32((int)0xFFFF0000u);

      /* (never the last word this way: the sixteen words stored reach one
       * past the eighth one read, and after the last word of the second
       * ROM of a pair that is the end of the cartridge) */
      for (; i > 8; i -= 8, from += 16, to += 16)
      {
         const __m128i words = _mm_loadu_si128((const __m128i *)from);
         const __m128i low   = _mm_unpacklo_epi16(words, zero);
         const __m128i high  = _mm_unpackhi_epi16(words, zero);
         __m128i a = _mm_loadu_si128((const __m128i *)to);
         __m128i b = _mm_loadu_si128((const __m128i *)(to + 8));

         a = _mm_or_si128(_mm_and_si128(a, other), low);
         b = _mm_or_si128(_mm_and_si128(b, other), high);
         _mm_storeu_si128((__m128i *)to, a);
         _mm_storeu_si128((__m128i *)(to + 8), b);
      }
   }
#endif
   /* (a word at a time from where the bytes lie, aligned or not) */
   for (; i; i--, from += 2, to += 2)
      memcpy(to, from, 2);
}

int naomi_interleave_piece(void *ud, uint64_t offset, const uint8_t *p, size_t len)
{
   naomi_copy_interleaved((uint16_t *)ud + offset, p, (uint32_t)len);
   return 1;
}
