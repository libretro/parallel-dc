#ifndef TEXCONV_H
#define TEXCONV_H
/* The texture converters (texconv.c): a texture's texels in video memory,
 * each pixel format of the PowerVR's - planar, twiddled, compressed,
 * paletted - to 16, 32 or 8 bits a texel. Pure functions of video memory,
 * the codebook and the palette. And the tables they use: the twiddle
 * tables and the palette converted for the renderer. C89, for C and C++
 * alike; TexCache.h has PixelBuffer, C++'s face of tex_buf_t. */
#include <stddef.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where texels are written: a picture, or a mipmapped texture's levels one
 * after another from the smallest (1x1) on. @level is the level being
 * written (tex_buf_set_mipmap()), @pitch the texels from one of its lines
 * to the next. */
typedef struct tex_buf
{
	void *start;
	void *level;
	u32 pitch;
	bool borrowed;		/* the memory is the caller's: not freed here */
} tex_buf_t;

/* How much memory a picture of @width by @height texels of @texel bytes
 * takes, with every smaller level if @mipmapped */
size_t tex_buf_bytes(u32 width, u32 height, bool mipmapped, size_t texel);
/* Memory of its own, for a picture of @width by @height texels. False
 * without memory. */
bool tex_buf_alloc(tex_buf_t *b, u32 width, u32 height, size_t texel);
/* The same on memory the caller has, tex_buf_bytes() of it or more:
 * nothing is allocated and nothing freed */
void tex_buf_borrow(tex_buf_t *b, u32 width, u32 height, bool mipmapped, void *memory);
void tex_buf_free(tex_buf_t *b);
/* Level @level of a mipmapped texture's, 0 the 1x1 one */
void tex_buf_set_mipmap(tex_buf_t *b, int level, size_t texel);
/* The texel at (@x, @y) of the level, of @texel bytes */
#define TEX_BUF_AT(b, x, y, texel) ((void *)((u8 *)(b)->level + ((size_t)(b)->pitch * (y) + (x)) * (texel)))

/* A converter: @width by @height texels from @p_in, into @pb at its level */
typedef void tex_conv_fn(tex_buf_t *pb, u8 *p_in, u32 width, u32 height);

/* Planar, to 16 bits */
tex_conv_fn tex565_PL, tex1555_PL, tex4444_PL;
/* Planar, to 32 bits; YUV only to 32 */
tex_conv_fn tex565_PL32, tex1555_PL32, tex4444_PL32, texYUV422_PL;
/* Planar and compressed, to 32 bits */
tex_conv_fn tex565_PLVQ32, tex1555_PLVQ32, tex4444_PLVQ32, texYUV422_PLVQ;
/* Twiddled, to 16 bits; paletted to 16, 32 and to the indices (8 bits) */
tex_conv_fn tex565_TW, tex1555_TW, tex4444_TW, texPAL4_TW, texPAL8_TW;
tex_conv_fn tex565_TW32, tex1555_TW32, tex4444_TW32, texYUV422_TW, texPAL4_TW32, texPAL8_TW32;
tex_conv_fn texPAL4PT_TW, texPAL8PT_TW;
/* Twiddled and compressed. According to the documentation, a texture
 * cannot be compressed and use a palette at the same time. However the
 * hardware displays them just fine. */
tex_conv_fn tex565_VQ, tex1555_VQ, tex4444_VQ, texPAL4_VQ, texPAL8_VQ;
tex_conv_fn tex565_VQ32, tex1555_VQ32, tex4444_VQ32, texYUV422_VQ, texPAL4_VQ32, texPAL8_VQ32;
/* The bump map's are 4444's */
#define texBMP_PL tex4444_PL
#define texBMP_TW tex4444_TW
#define texBMP_VQ tex4444_VQ

/* What the converters read besides video memory: a compressed texture's
 * codebook, and the palette as 16 and as 32-bit texels from @palette_index
 * on (palette_update()) */
extern u8 *vq_codebook;
extern u32 palette_index;
extern u32 palette16_ram[1024];
extern u32 palette32_ram[1024];
/* The palette's banks' hashes, for the textures that use them, and whether
 * the last palette_update() converted it */
extern u32 pal_hash_256[4];
extern u32 pal_hash_16[64];
extern bool palette_updated;

/* A twiddled texture's texel (x, y): detwiddle[0][log2 height][x] +
 * detwiddle[1][log2 width][y], in texels (texconv_init()) */
extern u32 detwiddle[2][11][1024];
#define twop(x,y,bcx,bcy) (detwiddle[0][bcy][x]+detwiddle[1][bcx][y])

/* The twiddle tables made: once, before anything is converted */
void texconv_init(void);
/* Converts the palette for the renderer when it is not the one converted
 * last, from the registers the frame is drawn with (the thread that
 * draws) */
void palette_update(void);

/* Unpack to 16-bit word */
#define ARGB1555( word )	( ((word>>15)&1) | (((word>>10) & 0x1F)<<11)  | (((word>>5) & 0x1F)<<6)  | (((word>>0) & 0x1F)<<1) )
#define ARGB565( word )	( (((word>>0)&0x1F)<<0) | (((word>>5)&0x3F)<<5) | (((word>>11)&0x1F)<<11) )
#define ARGB4444( word ) ( (((word>>0)&0xF)<<4) | (((word>>4)&0xF)<<8) | (((word>>8)&0xF)<<12) | (((word>>12)&0xF)<<0) )
#define ARGB8888( word ) ( (((word>>4)&0xF)<<4) | (((word>>12)&0xF)<<8) | (((word>>20)&0xF)<<12) | (((word>>28)&0xF)<<0) )

/* Unpack to 32-bit word */
#define ARGB1555_32( word )    ( ((word & 0x8000) ? 0xFF000000 : 0) | \
								(((word >> 10) & 0x1F) << 3) | (((word >> 12) & 0x7) << 0) | \
								(((word >> 5) & 0x1F) << 11) | (((word >> 7) & 0x7) << 8) | \
								(((word >> 0) & 0x1F) << 19) | (((word >> 2) & 0x7) << 16) )
#define ARGB565_32( word )     ( (((word >> 11) & 0x1F) << 3) | (((word >> 13) & 0x7) << 0) | \
								(((word >> 5) & 0x3F) << 10) | (((word >> 9) & 0x3) << 8) | \
								(((word >> 0) & 0x1F) << 19) | (((word >> 2) & 0x7) << 16) | \
								0xFF000000 )
#define ARGB4444_32( word ) ( (((word >> 12) & 0xF) << 28) | (((word >> 12) & 0xF) << 24) | \
								(((word >> 8) & 0xF) << 4) | (((word >> 8) & 0xF) << 0) | \
								(((word >> 4) & 0xF) << 12) | (((word >> 4) & 0xF) << 8) | \
								(((word >> 0) & 0xF) << 20) | (((word >> 0) & 0xF) << 16) )
#define ARGB8888_32( word ) ( ((word >> 0) & 0xFF000000) | (((word >> 16) & 0xFF) << 0) | (((word >> 8) & 0xFF) << 8) | ((word & 0xFF) << 16) )

static INLINE u32 YUV422(s32 Y, s32 Yu, s32 Yv)
{
	s32 R, G, B;
	Yu -= 128;
	Yv -= 128;

	R = Y + Yv * 11 / 8;            /* Y + (Yv-128) * (11/8) ? */
	G = Y - (Yu * 11 + Yv * 22) / 32; /* Y - (Yu-128) * (11/8) * 0.25 - (Yv-128) * (11/8) * 0.5 ? */
	B = Y + Yu * 110 / 64;          /* Y + (Yu-128) * (11/8) * 1.25 ? */

	/* each held to 0..255 without a branch: what a pixel's colour is cannot be predicted */
	R = R < 0 ? 0 : R;
	G = G < 0 ? 0 : G;
	B = B < 0 ? 0 : B;
	R = R > 255 ? 255 : R;
	G = G > 255 ? 255 : G;
	B = B > 255 ? 255 : B;
	return (u32)R | ((u32)G << 8) | ((u32)B << 16) | 0xFF000000u;
}

#ifdef __cplusplus
}
#endif

#endif
