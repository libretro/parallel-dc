#ifndef TA_INDEX_H
#define TA_INDEX_H
/* A strip's indices, for make_index() (ta_vtx.cpp): the vertices that are
 * drawn, each once, and where two must be to keep the strip's triangles
 * the right way round or to join it to the one before. C89. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
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
/* 1e25 and 3.4e37, as their bits: x and y are dropped from 1e25 up (Manic
 * Panic Ghosts), z is from a NaN of either sign or a huge positive value (a
 * negative one is drawn: the base runner views in World Series Baseball
 * 2K1) */
#define TA_HUGE_XY 0x69045951u
#define TA_HUGE_Z  0x7DCCA14Cu

/* Whether a vertex at @x, @y, @z (their bits) is left out: 1 or 0, without
 * a branch. Asked of the bits, not the values: a float's bits with the sign
 * taken off go up as its size does, with infinity and the NaNs above
 * everything, so one integer comparison says "huge, infinite or not a
 * number". And it has to be the bits: built with fast-math the compiler is
 * told there are no NaNs, and tests of the values for them are dropped. */
static INLINE uint32_t ta_vertex_bad(uint32_t x, uint32_t y, uint32_t z)
{
   return (uint32_t)((x & 0x7FFFFFFFu) > TA_HUGE_XY) | (uint32_t)((y & 0x7FFFFFFFu) > TA_HUGE_XY)
         | (uint32_t)((z & 0x7FFFFFFFu) > 0x7F800000u) | (uint32_t)(z < 0x80000000u && z > TA_HUGE_Z);
}

/* ta_vertex_bad() of many vertices at once, kept as one value: started
 * with ta_bad_none(), each vertex's position (12 bytes at @xyz, and 4 more
 * that may be read and are not looked at) added with ta_bad_add(), and
 * ta_bad_any() nonzero where one of them is left out. With SSE2 or NEON
 * the three words are tested together, with no branch and no flag
 * (TA_BAD_SCALAR: not, for the test of the one against the other). */
#if !defined(TA_BAD_SCALAR) && !defined(MSB_FIRST) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
typedef __m128i ta_bad_t;
#define ta_bad_none() _mm_setzero_si128()
static INLINE ta_bad_t ta_bad_add(ta_bad_t acc, const void *xyz)
{
   const __m128i bits = _mm_loadu_si128((const __m128i *)xyz);
   /* x and y, sizes past 1e25; z, a NaN; z, huge and positive */
   const __m128i size = _mm_cmpgt_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF)),
         _mm_set_epi32(0x7FFFFFFF, 0x7F800000, (int)TA_HUGE_XY, (int)TA_HUGE_XY));
   const __m128i deep = _mm_cmpgt_epi32(bits, _mm_set_epi32(0x7FFFFFFF, (int)TA_HUGE_Z, 0x7FFFFFFF, 0x7FFFFFFF));
   return _mm_or_si128(acc, _mm_or_si128(size, deep));
}
#define ta_bad_any(acc) _mm_movemask_epi8(acc)
#elif !defined(TA_BAD_SCALAR) && !defined(MSB_FIRST) && (defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64))
#include <arm_neon.h>
typedef uint32x4_t ta_bad_t;
#define ta_bad_none() vdupq_n_u32(0)
static INLINE ta_bad_t ta_bad_add(ta_bad_t acc, const void *xyz)
{
   static const int32_t size_max[4] = { (int32_t)TA_HUGE_XY, (int32_t)TA_HUGE_XY, 0x7F800000, 0x7FFFFFFF };
   static const int32_t deep_max[4] = { 0x7FFFFFFF, 0x7FFFFFFF, (int32_t)TA_HUGE_Z, 0x7FFFFFFF };
   const int32x4_t bits = vreinterpretq_s32_u8(vld1q_u8((const uint8_t *)xyz));
   const uint32x4_t size = vcgtq_s32(vandq_s32(bits, vdupq_n_s32(0x7FFFFFFF)), vld1q_s32(size_max));
   const uint32x4_t deep = vcgtq_s32(bits, vld1q_s32(deep_max));
   return vorrq_u32(acc, vorrq_u32(size, deep));
}
static INLINE int ta_bad_any(ta_bad_t acc)
{
   const uint32x2_t half = vorr_u32(vget_low_u32(acc), vget_high_u32(acc));
   return (vget_lane_u32(half, 0) | vget_lane_u32(half, 1)) != 0;
}
#else
typedef uint32_t ta_bad_t;
#define ta_bad_none() 0u
static INLINE ta_bad_t ta_bad_add(ta_bad_t acc, const void *xyz)
{
   uint32_t w[3];
   memcpy(w, xyz, sizeof(w));
   return acc | ta_vertex_bad(w[0], w[1], w[2]);
}
#define ta_bad_any(acc) ((acc) != 0)
#endif

/* The strips of a polygon list the vertex loop wrote whole and found every
 * vertex of drawable: a bit a polygon parameter, by its place in the list.
 * Their indices need no look at the vertices (ta_index_strip()'s
 * @known_good). Only the words a frame set are cleared for the next. */
typedef struct ta_strip_ok
{
   uint32_t *bits;
   uint32_t polys;   /* room, in polygons */
   uint32_t hi;      /* words set since the last clear */
} ta_strip_ok_t;

/* Test hook: nonzero, every strip is looked at as if none were known good
 * (tools/pvr/ta_parse_bench.sh's check) */
extern int ta_strip_ok_ignored;

static INLINE void ta_strip_ok_set(ta_strip_ok_t *s, uint32_t poly)
{
   if (poly < s->polys)
   {
      s->bits[poly >> 5] |= (uint32_t)1 << (poly & 31);
      if ((poly >> 5) >= s->hi)
         s->hi = (poly >> 5) + 1;
   }
}

static INLINE int ta_strip_ok_get(const ta_strip_ok_t *s, uint32_t poly)
{
   return poly < s->polys && !ta_strip_ok_ignored && ((s->bits[poly >> 5] >> (poly & 31)) & 1);
}

/* @s over the words at @words, @polys bits of them; and cleared */
void ta_strip_ok_bind(ta_strip_ok_t *s, uint32_t *words, uint32_t polys);
void ta_strip_ok_clear(ta_strip_ok_t *s);

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
/* @known_good: every vertex of the strip is known to be drawn (a strip
 * ta_strip_ok has), and none is looked at */
void ta_index_strip(ta_index_out_t *o, const void *vtx, size_t stride, uint32_t first, uint32_t count,
      int dupe, uint32_t first_index, int known_good);

#ifdef __cplusplus
}
#endif

#endif
