/* The texture converters (texconv.h). They were templates of the texture
 * cache's: a block converter of a few texels, and around it a loop over
 * the texture - planar, twiddled, compressed - for each pixel format and
 * size of texel. Here the block converters are inline functions and each
 * loop a macro that makes a converter of one of them. */

/* The texture cache works for the renderer: it reads the registers the
 * frame is rendered with, not the ones the emulation has moved on to. */
#ifndef PVR_REGS_FOR_RENDERER
#define PVR_REGS_FOR_RENDERER
#endif
#include <stdlib.h>
#include <string.h>
#include "texconv.h"
#include "oslib/oslib.h"
#include "hw/pvr/pvr_regs.h"
#include <encodings/xxh32.h>

u8 *vq_codebook;
u32 palette_index;
u32 palette16_ram[1024];
u32 palette32_ram[1024];
u32 pal_hash_256[4];
u32 pal_hash_16[64];
bool palette_updated;

u32 detwiddle[2][11][1024];
/* input : address in the yyyyyxxxxx format */
/* output : address in the xyxyxyxy format */
/* U : x resolution , V : y resolution */
/* twiddle works on 64b words */


static u32 twiddle_slow(u32 x,u32 y,u32 x_sz,u32 y_sz)
{
	u32 rv=0;/* low 2 bits are directly passed  -> needs some misc stuff to work.However */
			 /* Pvr internally maps the 64b banks "as if" they were twiddled :p */

	u32 sh=0;
	x_sz>>=1;
	y_sz>>=1;
	while(x_sz!=0 || y_sz!=0)
	{
		if (y_sz)
		{
			u32 temp=y&1;
			rv|=temp<<sh;

			y_sz>>=1;
			y>>=1;
			sh++;
		}
		if (x_sz)
		{
			u32 temp=x&1;
			rv|=temp<<sh;

			x_sz>>=1;
			x>>=1;
			sh++;
		}
	}
	return rv;
}

void texconv_init(void)
{
	u32 s, i;
	for (s = 0; s < 11; s++)
	{
		u32 x_sz = 1024;
		u32 y_sz = 1 << s;
		for (i = 0; i < x_sz; i++)
		{
			detwiddle[0][s][i] = twiddle_slow(i, 0, x_sz, y_sz);
			detwiddle[1][s][i] = twiddle_slow(0, i, y_sz, x_sz);
		}
	}
}

/* Converts the palette for the renderer when it is not the one converted
 * last. Called by the renderer for each frame, with the registers that
 * frame is drawn with.
 *
 * This used to be driven by a flag the register writes set, and was run
 * by the emulation thread when the game started a render. With Threaded
 * Rendering the frame before could still be drawing then, reading the very
 * tables this rewrites. Now the tables belong to the thread that draws, and
 * whether they are stale is seen from the palette itself: 4 KB to compare. */
void palette_update(void)
{
	static u32 converted[1024];
	static u32 converted_ctrl = 0xFFFFFFFF;
	const u32 ctrl = PAL_RAM_CTRL & 3;
	int i;

	if (ctrl == converted_ctrl && !memcmp(converted, PALETTE_RAM, sizeof(converted)))
		return;
	converted_ctrl = ctrl;
	memcpy(converted, PALETTE_RAM, sizeof(converted));
	palette_updated = true;
	switch (ctrl)
	{
	case 0:
		for (i=0;i<1024;i++)
		{
			palette16_ram[i] = ARGB1555(PALETTE_RAM[i]);
			palette32_ram[i] = ARGB1555_32(PALETTE_RAM[i]);
		}
		break;

	case 1:
		for (i=0;i<1024;i++)
		{
			palette16_ram[i] = ARGB565(PALETTE_RAM[i]);
			palette32_ram[i] = ARGB565_32(PALETTE_RAM[i]);
		}
		break;

	case 2:
		for (i=0;i<1024;i++)
		{
			palette16_ram[i] = ARGB4444(PALETTE_RAM[i]);
			palette32_ram[i] = ARGB4444_32(PALETTE_RAM[i]);
		}
		break;

	case 3:
		for (i=0;i<1024;i++)
		{
			palette16_ram[i] = ARGB8888(PALETTE_RAM[i]);
			palette32_ram[i] = ARGB8888_32(PALETTE_RAM[i]);
		}
		break;
	}
	for (i = 0; i < 64; i++)
		pal_hash_16[i] = xxh32(&PALETTE_RAM[i << 4], 16 * 4, 7);
	for (i = 0; i < 4; i++)
		pal_hash_256[i] = xxh32(&PALETTE_RAM[i << 8], 256 * 4, 7);
}

/*
 * Where the texels go
 */
size_t tex_buf_bytes(u32 width, u32 height, bool mipmapped, size_t texel)
{
	size_t size = (size_t)width * height * texel;
	if (mipmapped)
	{
		do
		{
			width /= 2;
			height /= 2;
			size += (size_t)width * height * texel;
		}
		while (width != 0 && height != 0);
	}
	return size;
}

bool tex_buf_alloc(tex_buf_t *b, u32 width, u32 height, size_t texel)
{
	tex_buf_free(b);
	b->start = b->level = malloc((size_t)width * height * texel);
	b->pitch = width;
	b->borrowed = false;
	return b->start != NULL;
}

void tex_buf_borrow(tex_buf_t *b, u32 width, u32 height, bool mipmapped, void *memory)
{
	(void)height;
	tex_buf_free(b);
	b->start = b->level = memory;
	b->pitch = mipmapped ? 1 : width;
	b->borrowed = true;
}

void tex_buf_free(tex_buf_t *b)
{
	if (b->start != NULL && !b->borrowed)
		free(b->start);
	b->start = b->level = NULL;
	b->borrowed = false;
}

void tex_buf_set_mipmap(tex_buf_t *b, int level, size_t texel)
{
	size_t offset = 0;
	int i;
	for (i = 0; i < level; i++)
		offset += (size_t)1 << (2 * i);
	b->level = (u8 *)b->start + offset * texel;
	b->pitch = (u32)1 << level;
}

/* The four 16-bit pixels of a twiddled 2x2 block to 32 bits, four at a time
 * where the processor can, and into their two rows: the first and third on
 * the upper, the second and fourth on the lower. Every channel is widened
 * by repeating its top bits, exactly as ARGB565_32() and the others do one
 * pixel at a time - the same bits come out. Planar textures get this from
 * the compiler; in twiddled order it has to be asked for. */
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
typedef __m128i tw4;
#define TW4_LOAD(src)      _mm_unpacklo_epi16(_mm_loadl_epi64((const __m128i *)(src)), _mm_setzero_si128())
#define TW4_SET(k)         _mm_set1_epi32((int)(k))
#define TW4_SHR(v, n)      _mm_srli_epi32(v, n)
#define TW4_SHL(v, n)      _mm_slli_epi32(v, n)
#define TW4_AND(a, b)      _mm_and_si128(a, b)
#define TW4_OR(a, b)       _mm_or_si128(a, b)
#define TW4_SIGN16(v)      _mm_srai_epi32(_mm_slli_epi32(v, 16), 31)   /* all ones where bit 15 is set */
static INLINE void tw4_store(u32 *row0, u32 *row1, tw4 px)
{
	px = _mm_shuffle_epi32(px, _MM_SHUFFLE(3, 1, 2, 0));
	_mm_storel_epi64((__m128i *)row0, px);
	_mm_storel_epi64((__m128i *)row1, _mm_unpackhi_epi64(px, px));
}
#define TW4_SIMD
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#include <arm_neon.h>
typedef uint32x4_t tw4;
#define TW4_LOAD(src)      vmovl_u16(vld1_u16((const uint16_t *)(src)))
#define TW4_SET(k)         vdupq_n_u32(k)
#define TW4_SHR(v, n)      vshrq_n_u32(v, n)
#define TW4_SHL(v, n)      vshlq_n_u32(v, n)
#define TW4_AND(a, b)      vandq_u32(a, b)
#define TW4_OR(a, b)       vorrq_u32(a, b)
#define TW4_SIGN16(v)      vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(vshlq_n_u32(v, 16)), 31))
static INLINE void tw4_store(u32 *row0, u32 *row1, tw4 px)
{
	const uint32x2x2_t rows = vuzp_u32(vget_low_u32(px), vget_high_u32(px));
	vst1_u32(row0, rows.val[0]);
	vst1_u32(row1, rows.val[1]);
}
#define TW4_SIMD
#endif

#ifdef TW4_SIMD
/* a channel of five bits to eight, of six, of four */
#define TW4_5TO8(c)        TW4_OR(TW4_SHL(c, 3), TW4_SHR(c, 2))
#define TW4_6TO8(c)        TW4_OR(TW4_SHL(c, 2), TW4_SHR(c, 4))
#define TW4_4TO8(c)        TW4_OR(TW4_SHL(c, 4), c)

static INLINE void tw_block_565(u32 *row0, u32 *row1, const u8 *src)
{
	const tw4 v = TW4_LOAD(src);
	const tw4 r = TW4_5TO8(TW4_SHR(v, 11));
	const tw4 g = TW4_6TO8(TW4_AND(TW4_SHR(v, 5), TW4_SET(0x3F)));
	const tw4 b = TW4_5TO8(TW4_AND(v, TW4_SET(0x1F)));
	tw4_store(row0, row1, TW4_OR(TW4_OR(r, TW4_SHL(g, 8)), TW4_OR(TW4_SHL(b, 16), TW4_SET(0xFF000000))));
}

static INLINE void tw_block_1555(u32 *row0, u32 *row1, const u8 *src)
{
	const tw4 v = TW4_LOAD(src);
	const tw4 r = TW4_5TO8(TW4_AND(TW4_SHR(v, 10), TW4_SET(0x1F)));
	const tw4 g = TW4_5TO8(TW4_AND(TW4_SHR(v, 5), TW4_SET(0x1F)));
	const tw4 b = TW4_5TO8(TW4_AND(v, TW4_SET(0x1F)));
	const tw4 a = TW4_AND(TW4_SIGN16(v), TW4_SET(0xFF000000));
	tw4_store(row0, row1, TW4_OR(TW4_OR(r, TW4_SHL(g, 8)), TW4_OR(TW4_SHL(b, 16), a)));
}

static INLINE void tw_block_4444(u32 *row0, u32 *row1, const u8 *src)
{
	const tw4 v = TW4_LOAD(src);
	const tw4 a = TW4_4TO8(TW4_SHR(v, 12));
	const tw4 r = TW4_4TO8(TW4_AND(TW4_SHR(v, 8), TW4_SET(0xF)));
	const tw4 g = TW4_4TO8(TW4_AND(TW4_SHR(v, 4), TW4_SET(0xF)));
	const tw4 b = TW4_4TO8(TW4_AND(v, TW4_SET(0xF)));
	tw4_store(row0, row1, TW4_OR(TW4_OR(r, TW4_SHL(g, 8)), TW4_OR(TW4_SHL(b, 16), TW4_SHL(a, 24))));
}
#else
/* one at a time, where there is nothing better */
#define TW_BLOCK_SCALAR(name, unpack) \
static INLINE void name(u32 *row0, u32 *row1, const u8 *src) \
{ \
	const u16 *p_in = (const u16 *)src; \
	row0[0] = unpack(p_in[0]); \
	row1[0] = unpack(p_in[1]); \
	row0[1] = unpack(p_in[2]); \
	row1[1] = unpack(p_in[3]); \
}
TW_BLOCK_SCALAR(tw_block_565, ARGB565_32)
TW_BLOCK_SCALAR(tw_block_1555, ARGB1555_32)
TW_BLOCK_SCALAR(tw_block_4444, ARGB4444_32)
#endif

/* And to 16 bits. The three conversions there are a 16-bit turn to the left
 * - by nothing for 565, by one bit for 1555 (the alpha bit goes from the top
 * to the bottom), by four for 4444 (the alpha nibble likewise) - so a block's
 * four pixels are turned at once as the 64-bit number they are in memory,
 * and written as two 32-bit halves, one a row. Where bytes are the other
 * way round they are done one by one. */
#ifndef MSB_FIRST
static INLINE void tw16_block(u16 *row0, u16 *row1, const u8 *src, const u32 turn)
{
	u64 v;
	u32 lo, hi, upper, lower;

	memcpy(&v, src, sizeof(v));
	if (turn != 0)
	{
		const u64 low_bits = (((u64)0x00010001u << 32) | 0x00010001u) * ((1u << turn) - 1);
		v = ((v << turn) & ~low_bits) | ((v >> (16 - turn)) & low_bits);
	}
	lo = (u32)v;
	hi = (u32)(v >> 32);
	upper = (lo & 0xFFFF) | (hi << 16);          /* the first and the third */
	lower = (lo >> 16) | (hi & 0xFFFF0000);      /* the second and the fourth */
	memcpy(row0, &upper, sizeof(upper));
	memcpy(row1, &lower, sizeof(lower));
}
#else
static INLINE void tw16_block(u16 *row0, u16 *row1, const u8 *src, const u32 turn)
{
	const u16 *p_in = (const u16 *)src;
	row0[0] = (u16)((p_in[0] << turn) | (p_in[0] >> ((16 - turn) & 15)));
	row1[0] = (u16)((p_in[1] << turn) | (p_in[1] >> ((16 - turn) & 15)));
	row0[1] = (u16)((p_in[2] << turn) | (p_in[2] >> ((16 - turn) & 15)));
	row1[1] = (u16)((p_in[3] << turn) | (p_in[3] >> ((16 - turn) & 15)));
}
#endif

/* Two or four pixels next to each other, written as one number where that
 * is a store less (or three). For the palette converters, whose pixels come
 * out of a table one by one. */
#ifndef MSB_FIRST
static INLINE void tw_put2_16(u16 *row, u32 a, u32 b)
{
	const u32 v = (a & 0xFFFF) | (b << 16);
	memcpy(row, &v, sizeof(v));
}
static INLINE void tw_put2_32(u32 *row, u32 a, u32 b)
{
	const u64 v = a | ((u64)b << 32);
	memcpy(row, &v, sizeof(v));
}
static INLINE void tw_put4_16(u16 *row, u32 a, u32 b, u32 c, u32 d)
{
	const u64 v = (a & 0xFFFF) | ((u64)(b & 0xFFFF) << 16) | ((u64)(c & 0xFFFF) << 32) | ((u64)d << 48);
	memcpy(row, &v, sizeof(v));
}
#else
static INLINE void tw_put2_16(u16 *row, u32 a, u32 b) { row[0] = (u16)a; row[1] = (u16)b; }
static INLINE void tw_put2_32(u32 *row, u32 a, u32 b) { row[0] = a; row[1] = b; }
static INLINE void tw_put4_16(u16 *row, u32 a, u32 b, u32 c, u32 d) { row[0] = (u16)a; row[1] = (u16)b; row[2] = (u16)c; row[3] = (u16)d; }
#endif
static INLINE void tw_put4_32(u32 *row, u32 a, u32 b, u32 c, u32 d)
{
	tw_put2_32(row, a, b);
	tw_put2_32(row + 2, c, d);
}

/* Four YUV pixels at a time. They come as U Y V Y U Y V Y, two pixels
 * sharing a U and a V, and go through the same arithmetic as YUV422()
 * above - the divisions rounding towards zero as C's do, the results held
 * to 0..255 by the saturating pack - sixteen bits a lane. The four pixels
 * come out in order. */
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define YUV4_SSE2
/* a lane divided by 8, 32 or 64 (@shift 3, 5, 6), towards zero */
#define YUV4_DIV(t, shift) _mm_srai_epi16(_mm_add_epi16(t, _mm_and_si128(_mm_srai_epi16(t, 15), _mm_set1_epi16((1 << (shift)) - 1))), shift)
static INLINE void yuv_convert(const __m128i in, __m128i *rg, __m128i *ba)
{
	const __m128i y = _mm_srli_epi16(in, 8);
	const __m128i uv = _mm_and_si128(in, _mm_set1_epi16(0x00FF));
	__m128i u = _mm_and_si128(uv, _mm_set1_epi32(0x0000FFFF));
	__m128i v = _mm_srli_epi32(uv, 16);
	__m128i t, r, g, b, rb, ga;

	/* each U and V under both of its pixels, less 128 */
	u = _mm_sub_epi16(_mm_or_si128(u, _mm_slli_epi32(u, 16)), _mm_set1_epi16(128));
	v = _mm_sub_epi16(_mm_or_si128(v, _mm_slli_epi32(v, 16)), _mm_set1_epi16(128));
	t = _mm_mullo_epi16(v, _mm_set1_epi16(11));
	r = _mm_add_epi16(y, YUV4_DIV(t, 3));
	t = _mm_add_epi16(_mm_mullo_epi16(u, _mm_set1_epi16(11)), _mm_slli_epi16(t, 1));
	g = _mm_sub_epi16(y, YUV4_DIV(t, 5));
	t = _mm_mullo_epi16(u, _mm_set1_epi16(110));
	b = _mm_add_epi16(y, YUV4_DIV(t, 6));
	rb = _mm_packus_epi16(r, b);
	ga = _mm_packus_epi16(g, _mm_set1_epi16(255));
	*rg = _mm_unpacklo_epi8(rb, ga);
	*ba = _mm_unpackhi_epi8(rb, ga);
}

/* four pixels, from eight bytes in the low half of @in */
static INLINE __m128i yuv4_convert(const __m128i in)
{
	__m128i rg, ba;
	yuv_convert(in, &rg, &ba);
	return _mm_unpacklo_epi16(rg, ba);
}
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#define YUV4_NEON
#define YUV4_DIV(t, shift) vshr_n_s16(vadd_s16(t, vand_s16(vshr_n_s16(t, 15), vdup_n_s16((1 << (shift)) - 1))), shift)
static INLINE void yuv4_convert(const uint8x8_t in, uint8x8_t *first, uint8x8_t *second)
{
	const uint16x4_t w = vreinterpret_u16_u8(in);
	const int16x4_t y = vreinterpret_s16_u16(vshr_n_u16(w, 8));
	const uint32x2_t uv = vreinterpret_u32_u16(vand_u16(w, vdup_n_u16(0x00FF)));
	uint32x2_t u32v = vand_u32(uv, vdup_n_u32(0x0000FFFF));
	uint32x2_t v32v = vshr_n_u32(uv, 16);
	int16x4_t u, v, t, r, g, b;
	uint8x8_t rb, ga;
	uint8x8x2_t bytes;
	uint16x4x2_t words;

	u = vsub_s16(vreinterpret_s16_u32(vorr_u32(u32v, vshl_n_u32(u32v, 16))), vdup_n_s16(128));
	v = vsub_s16(vreinterpret_s16_u32(vorr_u32(v32v, vshl_n_u32(v32v, 16))), vdup_n_s16(128));
	t = vmul_n_s16(v, 11);
	r = vadd_s16(y, YUV4_DIV(t, 3));
	t = vadd_s16(vmul_n_s16(u, 11), vshl_n_s16(t, 1));
	g = vsub_s16(y, YUV4_DIV(t, 5));
	t = vmul_n_s16(u, 110);
	b = vadd_s16(y, YUV4_DIV(t, 6));
	rb = vqmovun_s16(vcombine_s16(r, b));
	ga = vqmovun_s16(vcombine_s16(g, vdup_n_s16(255)));
	bytes = vzip_u8(rb, ga);
	words = vzip_u16(vreinterpret_u16_u8(bytes.val[0]), vreinterpret_u16_u8(bytes.val[1]));
	*first = vreinterpret_u8_u16(words.val[0]);
	*second = vreinterpret_u8_u16(words.val[1]);
}
#endif

/*
 * The block converters: a block of texels from @data, to @p (its lines
 * @pitch texels apart), through @pal for those with a palette
 */

/* Planar, four texels in a row */
#define PL4_16(name, unpack) \
static INLINE void name(u16 *p, u32 pitch, const u32 *pal, const u8 *data) \
{ \
	const u16 *p_in = (const u16 *)data; \
	(void)pitch; (void)pal; \
	p[0] = (u16)unpack(p_in[0]); \
	p[1] = (u16)unpack(p_in[1]); \
	p[2] = (u16)unpack(p_in[2]); \
	p[3] = (u16)unpack(p_in[3]); \
}
#define PL4_32(name, unpack) \
static INLINE void name(u32 *p, u32 pitch, const u32 *pal, const u8 *data) \
{ \
	const u16 *p_in = (const u16 *)data; \
	(void)pitch; (void)pal; \
	p[0] = unpack(p_in[0]); \
	p[1] = unpack(p_in[1]); \
	p[2] = unpack(p_in[2]); \
	p[3] = unpack(p_in[3]); \
}
PL4_16(conv565_PL, ARGB565)
PL4_16(conv1555_PL, ARGB1555)
PL4_16(conv4444_PL, ARGB4444)
PL4_32(conv565_PL32, ARGB565_32)
PL4_32(conv1555_PL32, ARGB1555_32)
PL4_32(conv4444_PL32, ARGB4444_32)

static INLINE void convYUV_PL(u32 *p, u32 pitch, const u32 *pal, const u8 *data)
{
#if defined(YUV4_SSE2)
	(void)pitch; (void)pal;
	_mm_storeu_si128((__m128i *)p, yuv4_convert(_mm_loadl_epi64((const __m128i *)data)));
#elif defined(YUV4_NEON)
	uint8x8_t first, second;
	(void)pitch; (void)pal;
	yuv4_convert(vld1_u8(data), &first, &second);
	vst1_u8((u8 *)p, first);
	vst1_u8((u8 *)(p + 2), second);
#else
	/* convert 4x1 4444 to 4x1 8888 */
	const u32 *p_in = (const u32 *)data;
	s32 Y0 = (p_in[0] >> 8) & 255;
	s32 Yu = (p_in[0] >> 0) & 255;
	s32 Y1 = (p_in[0] >> 24) & 255;
	s32 Yv = (p_in[0] >> 16) & 255;
	(void)pitch; (void)pal;

	p[0] = YUV422(Y0, Yu, Yv);
	p[1] = YUV422(Y1, Yu, Yv);

	/* next 4 bytes */
	p_in += 1;

	Y0 = (p_in[0] >> 8) & 255;
	Yu = (p_in[0] >> 0) & 255;
	Y1 = (p_in[0] >> 24) & 255;
	Yv = (p_in[0] >> 16) & 255;

	p[2] = YUV422(Y0, Yu, Yv);
	p[3] = YUV422(Y1, Yu, Yv);
#endif
}

/* The same, eight pixels at a time, for a planar texture's rows - a video's,
 * usually. (The four-pixel one stays for compressed planar textures, whose
 * codebook entries are four pixels.) */
static INLINE void convYUV_PL8(u32 *p, u32 pitch, const u32 *pal, const u8 *data)
{
#if defined(YUV4_SSE2)
	__m128i rg, ba;
	(void)pitch; (void)pal;
	yuv_convert(_mm_loadu_si128((const __m128i *)data), &rg, &ba);
	_mm_storeu_si128((__m128i *)p, _mm_unpacklo_epi16(rg, ba));
	_mm_storeu_si128((__m128i *)(p + 4), _mm_unpackhi_epi16(rg, ba));
#else
	convYUV_PL(p, pitch, pal, data);
	convYUV_PL(p + 4, pitch, pal, data + 8);
#endif
}

/* Twiddled, a 2x2 block */
static INLINE void conv565_TW(u16 *p, u32 pitch, const u32 *pal, const u8 *data) { (void)pal; tw16_block(p, p + pitch, data, 0); }
static INLINE void conv1555_TW(u16 *p, u32 pitch, const u32 *pal, const u8 *data) { (void)pal; tw16_block(p, p + pitch, data, 1); }
static INLINE void conv4444_TW(u16 *p, u32 pitch, const u32 *pal, const u8 *data) { (void)pal; tw16_block(p, p + pitch, data, 4); }
static INLINE void conv565_TW32(u32 *p, u32 pitch, const u32 *pal, const u8 *data) { (void)pal; tw_block_565(p, p + pitch, data); }
static INLINE void conv1555_TW32(u32 *p, u32 pitch, const u32 *pal, const u8 *data) { (void)pal; tw_block_1555(p, p + pitch, data); }
static INLINE void conv4444_TW32(u32 *p, u32 pitch, const u32 *pal, const u8 *data) { (void)pal; tw_block_4444(p, p + pitch, data); }

static INLINE void convYUV_TW(u32 *p, u32 pitch, const u32 *pal, const u8 *data)
{
#if defined(YUV4_SSE2)
	/* the block's second and third words change places: then it is two pixels of the upper row and two of the lower */
	const __m128i px = yuv4_convert(_mm_shufflelo_epi16(_mm_loadl_epi64((const __m128i *)data), _MM_SHUFFLE(3, 1, 2, 0)));
	(void)pal;
	_mm_storel_epi64((__m128i *)p, px);
	_mm_storel_epi64((__m128i *)(p + pitch), _mm_unpackhi_epi64(px, px));
#elif defined(YUV4_NEON)
	static const u8 order[8] = { 0, 1, 4, 5, 2, 3, 6, 7 };
	uint8x8_t upper, lower;
	(void)pal;
	yuv4_convert(vtbl1_u8(vld1_u8(data), vld1_u8(order)), &upper, &lower);
	vst1_u8((u8 *)p, upper);
	vst1_u8((u8 *)(p + pitch), lower);
#else
	/* convert 4x1 4444 to 4x1 8888 */
	const u16 *p_in = (const u16 *)data;
	s32 Y0 = (p_in[0] >> 8) & 255;
	s32 Yu = (p_in[0] >> 0) & 255;
	s32 Y1 = (p_in[2] >> 8) & 255;
	s32 Yv = (p_in[2] >> 0) & 255;
	(void)pal;

	p[0] = YUV422(Y0, Yu, Yv);
	p[1] = YUV422(Y1, Yu, Yv);

	Y0 = (p_in[1] >> 8) & 255;
	Yu = (p_in[1] >> 0) & 255;
	Y1 = (p_in[3] >> 8) & 255;
	Yv = (p_in[3] >> 0) & 255;

	p[pitch + 0] = YUV422(Y0, Yu, Yv);
	p[pitch + 1] = YUV422(Y1, Yu, Yv);
#endif
}

/* Sixteen pixels, a nibble each, in twiddled order: the first eight are
 * the left half of the block and the last eight the right, each half going
 * down its two columns two rows at a time. A row is written at once. */
#define PAL4_N(i) pal[(p_in[(i) >> 1] >> (((i) & 1) * 4)) & 0xF]
#define CONV_PAL4_TW(name, T, put4) \
static INLINE void name(T *p, u32 pitch, const u32 *pal, const u8 *data) \
{ \
	const u8 * const p_in = data; \
	T * const r0 = p; \
	T * const r1 = r0 + pitch; \
	T * const r2 = r1 + pitch; \
	T * const r3 = r2 + pitch; \
	put4(r0, PAL4_N(0), PAL4_N(2), PAL4_N(8), PAL4_N(10)); \
	put4(r1, PAL4_N(1), PAL4_N(3), PAL4_N(9), PAL4_N(11)); \
	put4(r2, PAL4_N(4), PAL4_N(6), PAL4_N(12), PAL4_N(14)); \
	put4(r3, PAL4_N(5), PAL4_N(7), PAL4_N(13), PAL4_N(15)); \
}
CONV_PAL4_TW(convPAL4_TW16, u16, tw_put4_16)
CONV_PAL4_TW(convPAL4_TW32, u32, tw_put4_32)
#undef PAL4_N

/* Palette 4bpp -> 8bpp: the indices only */
static INLINE void convPAL4PT_TW(u8 *p, u32 pitch, const u32 *pal, const u8 *data)
{
	const u8 *p_in = data;
#define PT(x, y, v) p[(y) * pitch + (x)] = (u8)(v)
	(void)pal;
	PT(0, 0, p_in[0] & 0xF);
	PT(0, 1, (p_in[0] >> 4) & 0xF); p_in++;
	PT(1, 0, p_in[0] & 0xF);
	PT(1, 1, (p_in[0] >> 4) & 0xF); p_in++;

	PT(0, 2, p_in[0] & 0xF);
	PT(0, 3, (p_in[0] >> 4) & 0xF); p_in++;
	PT(1, 2, p_in[0] & 0xF);
	PT(1, 3, (p_in[0] >> 4) & 0xF); p_in++;

	PT(2, 0, p_in[0] & 0xF);
	PT(2, 1, (p_in[0] >> 4) & 0xF); p_in++;
	PT(3, 0, p_in[0] & 0xF);
	PT(3, 1, (p_in[0] >> 4) & 0xF); p_in++;

	PT(2, 2, p_in[0] & 0xF);
	PT(2, 3, (p_in[0] >> 4) & 0xF); p_in++;
	PT(3, 2, p_in[0] & 0xF);
	PT(3, 3, (p_in[0] >> 4) & 0xF);
}

/* eight pixels, a byte each: two columns, going down two rows at a time */
#define CONV_PAL8_TW(name, T, put2) \
static INLINE void name(T *p, u32 pitch, const u32 *pal, const u8 *data) \
{ \
	const u8 * const p_in = data; \
	T * const r0 = p; \
	T * const r1 = r0 + pitch; \
	T * const r2 = r1 + pitch; \
	T * const r3 = r2 + pitch; \
	put2(r0, pal[p_in[0]], pal[p_in[2]]); \
	put2(r1, pal[p_in[1]], pal[p_in[3]]); \
	put2(r2, pal[p_in[4]], pal[p_in[6]]); \
	put2(r3, pal[p_in[5]], pal[p_in[7]]); \
}
CONV_PAL8_TW(convPAL8_TW16, u16, tw_put2_16)
CONV_PAL8_TW(convPAL8_TW32, u32, tw_put2_32)

/* Palette 8bpp -> 8bpp (untwiddle only) */
static INLINE void convPAL8PT_TW(u8 *p, u32 pitch, const u32 *pal, const u8 *data)
{
	const u8 *p_in = data;
	(void)pal;
	PT(0, 0, p_in[0]); p_in++;
	PT(0, 1, p_in[0]); p_in++;
	PT(1, 0, p_in[0]); p_in++;
	PT(1, 1, p_in[0]); p_in++;

	PT(0, 2, p_in[0]); p_in++;
	PT(0, 3, p_in[0]); p_in++;
	PT(1, 2, p_in[0]); p_in++;
	PT(1, 3, p_in[0]);
#undef PT
}

/*
 * The loops: the texture's blocks, each handed to a block converter
 * @conv of @xpp by @ypp texels
 */
/* (the palette as texels of this size: 32 bits for the indices, which
 * have none) */
#define PAL_FOR(T) ((sizeof(T) == 2 ? palette16_ram : palette32_ram) + palette_index)

/* Planar */
#define TEX_PL(name, T, conv, xpp, ypp) \
void name(tex_buf_t *pb, u8 *p_in, u32 Width, u32 Height) \
{ \
	T *line = (T *)pb->level; \
	const u32 pitch = pb->pitch; \
	const u32 * const pal = PAL_FOR(T); \
	u32 x, y; \
	Height /= (ypp); \
	Width /= (xpp); \
	for (y = 0; y < Height; y++) \
	{ \
		for (x = 0; x < Width; x++) \
		{ \
			conv(line + x * (xpp), pitch, pal, p_in); \
			p_in += 2 * (xpp);	/* two bytes a pixel */ \
		} \
		line += pitch * (ypp); \
	} \
}

/* Compressed and not twiddled: each byte names a codebook entry, as for
 * any compressed texture, and an entry is four pixels in a row, left to
 * right. */
#define TEX_PLVQ(name, T, conv, xpp, ypp) \
void name(tex_buf_t *pb, u8 *p_in, u32 Width, u32 Height) \
{ \
	u8 * const codebook = vq_codebook; \
	T *line = (T *)pb->level; \
	const u32 pitch = pb->pitch; \
	const u32 * const pal = PAL_FOR(T); \
	u32 x, y; \
	p_in += 256 * 4 * 2;	/* Skip VQ codebook */ \
	Height /= (ypp); \
	Width /= (xpp); \
	for (y = 0; y < Height; y++) \
	{ \
		for (x = 0; x < Width; x++) \
			conv(line + x * (xpp), pitch, pal, &codebook[*p_in++ * 8]); \
		line += pitch * (ypp); \
	} \
}

/* Twiddled: the block for (x, y) is at the sum of two table entries, one for
 * each. The one for y is the same along a row. */
#define TEX_TW(name, T, conv, xpp, ypp) \
void name(tex_buf_t *pb, u8 *p_in, u32 Width, u32 Height) \
{ \
	const u32 divider = (xpp) * (ypp); \
	const u32 * const of_x = detwiddle[0][bitscanrev(Height)]; \
	const u32 * const of_y = detwiddle[1][bitscanrev(Width)]; \
	T *line = (T *)pb->level; \
	const u32 pitch = pb->pitch; \
	const u32 * const pal = PAL_FOR(T); \
	u32 x, y; \
	for (y = 0; y < Height; y += (ypp)) \
	{ \
		const u32 row = of_y[y]; \
		for (x = 0; x < Width; x += (xpp)) \
			conv(line + x, pitch, pal, &p_in[((of_x[x] + row) / divider) << 3]); \
		line += pitch * (ypp); \
	} \
}

#define TEX_VQ(name, T, conv, xpp, ypp) \
void name(tex_buf_t *pb, u8 *p_in, u32 Width, u32 Height) \
{ \
	const u32 divider = (xpp) * (ypp); \
	const u32 * const of_x = detwiddle[0][bitscanrev(Height)]; \
	const u32 * const of_y = detwiddle[1][bitscanrev(Width)]; \
	u8 * const codebook = vq_codebook; \
	T *line = (T *)pb->level; \
	const u32 pitch = pb->pitch; \
	const u32 * const pal = PAL_FOR(T); \
	u32 x, y; \
	p_in += 256 * 4 * 2;	/* Skip VQ codebook */ \
	for (y = 0; y < Height; y += (ypp)) \
	{ \
		const u32 row = of_y[y]; \
		for (x = 0; x < Width; x += (xpp)) \
			conv(line + x, pitch, pal, &codebook[p_in[(of_x[x] + row) / divider] * 8]); \
		line += pitch * (ypp); \
	} \
}

/* Planar */
TEX_PL(tex565_PL, u16, conv565_PL, 4, 1)
TEX_PL(tex1555_PL, u16, conv1555_PL, 4, 1)
TEX_PL(tex4444_PL, u16, conv4444_PL, 4, 1)
TEX_PL(texYUV422_PL, u32, convYUV_PL8, 8, 1)
TEX_PL(tex565_PL32, u32, conv565_PL32, 4, 1)
TEX_PL(tex1555_PL32, u32, conv1555_PL32, 4, 1)
TEX_PL(tex4444_PL32, u32, conv4444_PL32, 4, 1)

/* Planar and compressed */
TEX_PLVQ(tex565_PLVQ32, u32, conv565_PL32, 4, 1)
TEX_PLVQ(tex1555_PLVQ32, u32, conv1555_PL32, 4, 1)
TEX_PLVQ(tex4444_PLVQ32, u32, conv4444_PL32, 4, 1)
TEX_PLVQ(texYUV422_PLVQ, u32, convYUV_PL, 4, 1)

/* Twiddled */
TEX_TW(tex565_TW, u16, conv565_TW, 2, 2)
TEX_TW(tex1555_TW, u16, conv1555_TW, 2, 2)
TEX_TW(tex4444_TW, u16, conv4444_TW, 2, 2)
TEX_TW(texYUV422_TW, u32, convYUV_TW, 2, 2)
TEX_TW(texPAL4_TW, u16, convPAL4_TW16, 4, 4)
TEX_TW(texPAL8_TW, u16, convPAL8_TW16, 2, 4)
TEX_TW(texPAL4_TW32, u32, convPAL4_TW32, 4, 4)
TEX_TW(texPAL8_TW32, u32, convPAL8_TW32, 2, 4)
TEX_TW(texPAL4PT_TW, u8, convPAL4PT_TW, 4, 4)
TEX_TW(texPAL8PT_TW, u8, convPAL8PT_TW, 2, 4)
TEX_TW(tex565_TW32, u32, conv565_TW32, 2, 2)
TEX_TW(tex1555_TW32, u32, conv1555_TW32, 2, 2)
TEX_TW(tex4444_TW32, u32, conv4444_TW32, 2, 2)

/* Twiddled and compressed */
TEX_VQ(tex565_VQ, u16, conv565_TW, 2, 2)
TEX_VQ(tex1555_VQ, u16, conv1555_TW, 2, 2)
TEX_VQ(tex4444_VQ, u16, conv4444_TW, 2, 2)
TEX_VQ(texYUV422_VQ, u32, convYUV_TW, 2, 2)
TEX_VQ(texPAL4_VQ, u16, convPAL4_TW16, 4, 4)
TEX_VQ(texPAL8_VQ, u16, convPAL8_TW16, 2, 4)
TEX_VQ(tex565_VQ32, u32, conv565_TW32, 2, 2)
TEX_VQ(tex1555_VQ32, u32, conv1555_TW32, 2, 2)
TEX_VQ(tex4444_VQ32, u32, conv4444_TW32, 2, 2)
TEX_VQ(texPAL4_VQ32, u32, convPAL4_TW32, 4, 4)
TEX_VQ(texPAL8_VQ32, u32, convPAL8_TW32, 2, 4)
