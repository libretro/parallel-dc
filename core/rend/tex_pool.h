/* Textures' images kept and handed out again, so that none is made while
 * a game runs.
 *
 * A texture used to make its image the first time it was used, and again
 * whenever its size or format changed - in the middle of a game, a scene's
 * worth of them in one frame when it loads, which is what the time spent
 * updating textures went on. Images are made when the renderer starts
 * instead, in the shapes the PowerVR's textures come in, and a texture
 * takes one of its shape from here and gives it back when it is done
 * with it.
 *
 * The PowerVR's textures are 8 to 1024 texels wide and high, in powers of
 * two, with all their mipmap levels or only the largest: with the
 * renderer's few formats, the shapes are few. An image's class is its
 * format, its width, its height and whether it has the levels; each class
 * has a list of the free images of that shape. How many of each are made
 * at the start is a share of the video memory the renderer may use
 * (rend/vram_budget.h), spread over the classes the way games' textures
 * are: mostly 16-bit, mipmapped and square, 64 to 256 texels, and a few
 * of the smallest squares whatever the share; a quarter of it to
 * the rectangles, by fixed shares - the smaller the shape and the nearer
 * to square, the more. Pictures rendered to (a format of the kind
 * TARGETS) are a fixed few of the shapes renders come in, from 64x64 to
 * 1024x1024 (half as many of 256 a side and larger where the share is the
 * least). A class that has
 * none left when one is asked for has one made then - which means the
 * share was too small for the game - and it stays in the pool after: it is
 * counted, and said once for each class.
 *
 * The lists are threaded through the images' own entries, and nothing is
 * allocated to take an image or to give one back. Only the thread that
 * renders touches a pool. */
#ifndef REND_TEX_POOL_H
#define REND_TEX_POOL_H

#include <string.h>
#include <retro_inline.h>
#include "types.h"
#include "log/Log.h"
#include "rend/vram_budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the pool knows of an image: the renderer's entry starts with this */
typedef struct TexPoolEntry TexPoolEntry;
struct TexPoolEntry
{
	TexPoolEntry *next;
	u32 cls;
	u32 bytes;
};

#define TEX_POOL_SIZES 8
#define TEX_POOL_MIN_LOG2 3
#define TEX_POOL_MAX_FORMATS 12
#define TEX_POOL_MAX_FRAMES 8
/* What a format's images are for: textures; or pictures rendered to */
#define TEX_POOL_TEXTURES 0
#define TEX_POOL_TARGETS 1
/* (the format of a 32-bit texture's, TEX_TYPE_8888: what a texture scaled up is) */
#define TEX_POOL_FORMAT_8888 3
#define TEX_POOL_CLASSES (TEX_POOL_MAX_FORMATS * TEX_POOL_SIZES * TEX_POOL_SIZES * 2)

/* (an image of the PowerVR's sizes, 8 to 1024 a side: its log2 - 3; -1 if not) */
static INLINE int tex_pool_log2(u32 v)
{
	int l;
	if (v < (8u << 0) || v > (8u << (TEX_POOL_SIZES - 1)) || (v & (v - 1)) != 0)
		return -1;
	for (l = 0; (8u << l) != v; l++)
		;
	return l;
}

/* The class of an image of @format (the renderer's own number for it,
 * below TEX_POOL_MAX_FORMATS), @width by @height, with its mipmap levels
 * or not; -1 if it is not a shape the pool has */
static INLINE int tex_pool_class(u32 format, u32 width, u32 height, bool mips)
{
	const int lw = tex_pool_log2(width), lh = tex_pool_log2(height);
	if (format >= TEX_POOL_MAX_FORMATS || lw < 0 || lh < 0)
		return -1;
	/* (a mipmapped texture of the PowerVR's is square) */
	if (mips && lw != lh)
		return -1;
	return (int)(((format * TEX_POOL_SIZES + (u32)lw) * TEX_POOL_SIZES + (u32)lh) * 2 + (mips ? 1 : 0));
}
static INLINE u32 tex_pool_format(int cls) { return (u32)cls / (TEX_POOL_SIZES * TEX_POOL_SIZES * 2); }
static INLINE u32 tex_pool_width(int cls) { return 8u << (((u32)cls / (TEX_POOL_SIZES * 2)) % TEX_POOL_SIZES); }
static INLINE u32 tex_pool_height(int cls) { return 8u << (((u32)cls / 2) % TEX_POOL_SIZES); }
static INLINE bool tex_pool_mips(int cls) { return (cls & 1) != 0; }
/* The image's texels, all levels, for @texel bytes a texel */
static INLINE u32 tex_pool_bytes(int cls, u32 texel)
{
	u32 w = tex_pool_width(cls), h = tex_pool_height(cls), bytes = w * h * texel;
	if (tex_pool_mips(cls))
		for (w /= 2; w != 0; w /= 2)
			bytes += w * w * texel;
	return bytes;
}

/* A rectangle of the PowerVR's, 2^(@lw + 3) by 2^(@lh + 3) texels: its
 * share of the rectangles of a format used as much as 565. Strips and
 * large ones are few. */
u32 tex_pool_rect_share(u32 lw, u32 lh);
/* The fewest square textures of 2^(@s + 3) texels without mipmaps of
 * each format there is: the smallest ones, fonts and the like, which
 * the tables give none of, eight of 8x8 and four of 16x16 (a few KB) */
static INLINE u32 tex_pool_least(u32 s) { return s == 0 ? 8 : s == 1 ? 4 : 0; }
/* How many of each shape of picture rendered to there are: the square
 * ones, eight of 64, two of 128 to 512, and 1024x1024; and
 * what renders of a screen are, 1024x512 (640x480 and the like) and
 * 512x256; one of each twice as long as it is wide from 64; and two of
 * each four times as long, to 256 (a strip rendered to and its copy:
 * 128x32 and the like, 340 KB in all at 32 bits) */
u32 tex_pool_target_count(u32 lw, u32 lh);
/* Where a texture of @f, 2^(@lw + 3) by 2^(@lh + 3), with mipmaps or
 * not, is drawn from: its own class (@texel[f] bytes a texel), or, scaled
 * up by 2^@k (xBRZ: no larger than @up_max a side to be), the 32-bit
 * class @k sizes up (its mipmaps, if it has them, made from it). -1:
 * none (scaled past 1024). */
int tex_pool_drawn(u32 f, u32 lw, u32 lh, bool mips, u32 k, u32 up_max, const u32 *texel, u32 *bytes);
/* How many images of each class to make at the start (into @counts,
 * TEX_POOL_CLASSES of them), for formats of @texel bytes a texel (0 for a
 * format there is none of), of the kind @kind[f] (TEX_POOL_TEXTURES,
 * TEX_POOL_TARGETS; all textures if null) and the given relative use
 * (@use, in eighths: 8 for a format games use as much as 565). What the
 * textures take is a sixteenth of what the budget has left, but no less
 * than 16 MB and no more than 128 MB: three quarters of it the square
 * ones, a quarter the rectangles. With textures scaled up @upscale times
 * (xBRZ: those no larger than @up_max a side), the textures as they are
 * drawn: 32-bit, as large as they are made (tex_pool_drawn()) - none of
 * them for a factor no power of two, whose sizes no image of the pool's
 * has. With renders to a texture @rtt times finer, the targets of those
 * sizes as well as the console's (read back at its size). Returns the
 * bytes, the targets' too. */
u64 tex_pool_plan(const VramBudget *budget, const u32 *texel, const u32 *use, const u8 *kind, u32 formats, u32 *counts,
		u32 upscale, u32 up_max, u32 rtt);

/* The book of a pool's images: which are free, of each class */
typedef struct tex_pool_book
{
	/* what has been done so far, for measuring */
	u32 made, madeLate, lateClasses;
	u64 bytesMade, bytesLate;

	TexPoolEntry *free[TEX_POOL_CLASSES];
	TexPoolEntry *retired[TEX_POOL_MAX_FRAMES];
	bool said[TEX_POOL_CLASSES];
	u32 frames;
	u32 current;
} tex_pool_book_t;

/* Empty, for frames @frames in flight */
void tex_pool_reset(tex_pool_book_t *b, u32 frames);
/* A free image of class @cls, or null: the class has none left, and the
 * renderer makes one (tex_pool_made() with @late) */
static INLINE TexPoolEntry *tex_pool_take(tex_pool_book_t *b, int cls)
{
	TexPoolEntry *e = b->free[cls];
	if (e != NULL)
		b->free[cls] = e->next;
	return e;
}
/* An image made: at the start, or because its class had none left */
void tex_pool_made(tex_pool_book_t *b, TexPoolEntry *e, int cls, u32 bytes, bool late, const char *format);
/* Free again now: nothing the GPU has still to do uses it */
static INLINE void tex_pool_give(tex_pool_book_t *b, TexPoolEntry *e)
{
	e->next = b->free[e->cls];
	b->free[e->cls] = e;
}
/* Free again once the frames that may still use it are done: when this
 * frame's place comes round again (tex_pool_frame()) */
static INLINE void tex_pool_retire(tex_pool_book_t *b, TexPoolEntry *e)
{
	e->next = b->retired[b->current];
	b->retired[b->current] = e;
}
/* @index is the current frame's from here on: what was retired under
 * it the last time is free */
void tex_pool_frame(tex_pool_book_t *b, u32 index);
/* Everything there is, free, retired or not, for the renderer to
 * destroy, one at a time: NULL when there is none left (what textures
 * hold they give back first) */
TexPoolEntry *tex_pool_drain(tex_pool_book_t *b);
void tex_pool_summary(const tex_pool_book_t *b, const char *api);

#ifdef __cplusplus
}
#endif

#endif
