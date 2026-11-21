/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
#include "dx9_texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <compat/strl.h>
#include "dx9context_lr.h"
#include "dx9_texdecoder.h"
#include "rend/rtt_watch.h"
#include "rend/tex_map.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"

/* The renderer's, while it has them */
static bool use_decoder;
static bool use_pool;

static const texcache_ops dx9_texture_ops;

/* ---- The texture pool ---- */

static const char * const poolNames[DX9_POOL_FORMATS] = {
	"565", "5551", "4444", "8888", "8-bit", "565 drawn", "5551 drawn", "4444 drawn", "8888 drawn", "8-bit drawn", "8888 rendered to"
};

static struct
{
	tex_pool_book_t book;
	bool ready;
	bool usable[DX9_POOL_FORMATS];
	dx9_pooled_texture *all;
} pool;

/* (TEX_TYPE_*) */
static int poolType(u32 f)
{
	return (int)(f >= DX9_POOL_TARGET ? TEX_TYPE_8888 : f >= DX9_POOL_DRAWN ? f - DX9_POOL_DRAWN : f);
}

static u32 poolTexel(u32 f)
{
	const int type = poolType(f);
	return type == TEX_TYPE_8888 ? 4 : type == TEX_TYPE_8 ? 1 : 2;
}

static void pool_free(dx9_pooled_texture *t)
{
	u32 level;
	for (level = 0; level < TEXD_MAX_LEVELS; level++)
		DX9_RELEASE(t->surfaces[level]);
	DX9_RELEASE(t->texture);
	free(t);
}

static dx9_pooled_texture *pool_make(int cls, bool late)
{
	IDirect3DDevice9 *device = dx9c.device;
	const u32 f = tex_pool_format(cls);
	const u32 width = tex_pool_width(cls), height = tex_pool_height(cls);
	dx9_pooled_texture *t;
	DWORD usage;
	D3DPOOL where;
	bool ok;
	u32 level;
	if (device == NULL)
		return NULL;
	t = (dx9_pooled_texture *)calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;
	t->levels = tex_pool_mips(cls) ? 1 + bitscanrev(width > height ? width : height) : 1;
	usage = f >= DX9_POOL_DRAWN ? D3DUSAGE_RENDERTARGET : dx9c_write_usage();
	/* (the managed pool where the device has one and the processor writes the texture without dynamic ones) */
	where = (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC)) != 0 || dx9c.ex ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;
	ok = SUCCEEDED(IDirect3DDevice9_CreateTexture(device, width, height, t->levels, usage,
			dx9c_texture_format(poolType(f)), where, &t->texture, NULL));
	if (!ok)
		t->texture = NULL;
	for (level = 0; ok && f >= DX9_POOL_DRAWN && level < t->levels; level++)
		ok = SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(t->texture, level, &t->surfaces[level]));
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX9: texture pool: a %ux%u %s texture could not be made\n", width, height, poolNames[f]);
		pool_free(t);
		return NULL;
	}
	tex_pool_made(&pool.book, &t->entry, cls, tex_pool_bytes(cls, poolTexel(f)), late, poolNames[f]);
	t->all = pool.all;
	pool.all = t;
	return t;
}

void dx9_pool_init(void)
{
	/* how much games use them, in eighths of what they use 565: the ones
	 * the processor writes fewer when the pixel shader draws most */
	static const u32 written[DX9_POOL_FORMATS] = { 8, 6, 6, 4, 2, 0, 0, 0, 0, 0, 0 };
	static const u32 shared[DX9_POOL_FORMATS] = { 4, 3, 3, 2, 1, 8, 6, 6, 4, 2, 0 };
	static const u8 kind[DX9_POOL_FORMATS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, TEX_POOL_TARGETS };
	static u32 counts[TEX_POOL_CLASSES];
	const bool drawn = dx9_texdec_ready();
	u32 have[DX9_POOL_FORMATS];
	u32 f, i;
	u64 bytes;
	int c;
	if (pool.ready)
		return;
	tex_pool_reset(&pool.book, 1);
	for (f = 0; f < DX9_POOL_FORMATS; f++)
	{
		const int type = poolType(f);
		pool.usable[f] = f == DX9_POOL_TARGET || (dx9c_format_supported(type) && (f < DX9_POOL_DRAWN || (drawn && dx9_texdec_takes(type))));
		have[f] = pool.usable[f] ? poolTexel(f) : 0;
	}
	bytes = tex_pool_plan(&dx9c.vram_budget, have, drawn ? shared : written, kind, DX9_POOL_FORMATS, counts, 1, 0, 1);
	pool.ready = true;
	for (c = 0; c < TEX_POOL_CLASSES; c++)
		for (i = 0; i < counts[c]; i++)
		{
			dx9_pooled_texture *t = pool_make(c, false);
			if (t == NULL)
				break;
			tex_pool_give(&pool.book, &t->entry);
		}
	NOTICE_LOG(RENDERER, "DX9: texture pool of %u textures, %u MB (planned %u MB)\n", pool.book.made,
			(u32)(pool.book.bytesMade >> 20), (u32)(bytes >> 20));
}

void dx9_pool_term(void)
{
	if (!pool.ready)
		return;
	tex_pool_summary(&pool.book, "DX9");
	while (pool.all != NULL)
	{
		dx9_pooled_texture *next = pool.all->all;
		pool_free(pool.all);
		pool.all = next;
	}
	tex_pool_reset(&pool.book, 1);
	pool.ready = false;
}

dx9_pooled_texture *dx9_pool_take(u32 f, u32 width, u32 height, bool mips)
{
	int cls;
	TexPoolEntry *e;
	if (!pool.ready || f >= DX9_POOL_FORMATS || !pool.usable[f])
		return NULL;
	cls = tex_pool_class(f, width, height, mips);
	if (cls < 0)
		return NULL;
	e = tex_pool_take(&pool.book, cls);
	/* (the entry first in each) */
	return e != NULL ? (dx9_pooled_texture *)e : pool_make(cls, true);
}

void dx9_pool_give(dx9_pooled_texture *t)
{
	tex_pool_give(&pool.book, &t->entry);
}

u32 dx9_pool_made(void)
{
	return pool.book.made;
}

/* ---- The cache's textures ---- */

static void tex_cancel_decode(dx9_texture *t)
{
	if (t->decode_count != 0 && use_decoder)
		dx9_texdec_cancel(t->decode_job, t->decode_count, t->decode_serial);
	t->decode_count = 0;
}

static void tex_release_pooled(dx9_texture *t)
{
	tex_cancel_decode(t);
	if (t->pooled == NULL)
		return;
	if (t->locked_bits != NULL && t->texture != NULL)
		IDirect3DTexture9_UnlockRect(t->texture, 0);
	t->locked_bits = NULL;
	DX9_RELEASE(t->texture);
	if (use_pool)
		dx9_pool_give(t->pooled);
	t->pooled = NULL;
}

/* The pool's texture of format @f and this shape, the one it has if it
 * is that; false if the pool has none */
static bool tex_take_pooled(dx9_texture *t, u32 f, u32 width, u32 height, bool mips)
{
	dx9_pooled_texture *p;
	if (!use_pool)
		return false;
	if (t->pooled != NULL && (int)t->pooled->entry.cls == tex_pool_class(f, width, height, mips))
		return true;
	p = dx9_pool_take(f, width, height, mips);
	if (p == NULL)
		return false;
	tex_release_pooled(t);
	if (t->texture != NULL)
		dx9c_recycle(&t->texture);
	t->pooled = p;
	DX9_ASSIGN(t->texture, p->texture);
	return true;
}

bool dx9_texture_take_spare(dx9_texture *t, u32 width, u32 height)
{
	dx9_pooled_texture *p = use_pool ? dx9_pool_take(DX9_POOL_TARGET, width, height, false) : NULL;
	if (p == NULL)
		return false;
	dx9_texture_release_spare(t);
	t->spare_pooled = p;
	DX9_ASSIGN(t->spare, p->texture);
	if (FAILED(IDirect3DTexture9_GetSurfaceLevel(t->spare, 0, &t->spare_surface)))
	{
		t->spare_surface = NULL;
		dx9_texture_release_spare(t);
		return false;
	}
	return true;
}

void dx9_texture_release_spare(dx9_texture *t)
{
	if (t->spare_pooled == NULL)
		return;
	DX9_RELEASE(t->spare_surface);
	DX9_RELEASE(t->spare);
	if (use_pool)
		dx9_pool_give(t->spare_pooled);
	t->spare_pooled = NULL;
}

void dx9_texture_swap_with_spare(dx9_texture *t)
{
	IDirect3DTexture9 *texture;
	/* (the texture it had goes back to the pool, if it is the pool's: the spare is that now) */
	tex_release_pooled(t);
	t->pooled = t->spare_pooled;
	t->spare_pooled = NULL;
	texture = t->texture;
	t->texture = t->spare;
	t->spare = texture;
	DX9_RELEASE(t->spare_surface);
	t->swap_rb = false;
}

/* Whether the texture is the one wanted, and made if not (the one it had
 * goes back to the context, to be used again). The pool's, if it has one
 * of this shape. */
static bool tex_prepare(dx9_texture *t, u32 width, u32 height, u32 levels, DWORD usage, D3DFORMAT format)
{
	const int type = (int)t->base.tex_type;
	if ((usage & D3DUSAGE_AUTOGENMIPMAP) == 0 && (unsigned)type < 5 && format == dx9c_texture_format(type)
			&& tex_take_pooled(t, (u32)type, width, height, levels > 1))
		return true;
	tex_release_pooled(t);
	if (t->texture != NULL)
	{
		/* (A render to this texture leaves one that can be rendered to,
		 * which is not one of these) */
		D3DSURFACE_DESC desc;
		if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc(t->texture, 0, &desc)) && desc.Width == width && desc.Height == height
				&& desc.Format == format && desc.Usage == usage
				&& ((usage & D3DUSAGE_AUTOGENMIPMAP) != 0 || IDirect3DTexture9_GetLevelCount(t->texture) == levels))
			return true;
		dx9c_recycle(&t->texture);
	}
	t->texture = dx9c_make_texture(width, height, levels, usage, format);
	return t->texture != NULL;
}

/* A planar 16-bit texture copied as it is: R5G6B5, A1R5G5B5 and A4R4G4B4
 * have the PowerVR's bits in its order, and its rows go from video memory
 * to the texture where it is locked, with no conversion. Or made by the
 * pixel shader (dx9_texdecoder.h), with the core option for it. */
static bool tex_gpu_decode(texcache_data *base, const struct texd_plan *plan)
{
	dx9_texture *t = (dx9_texture *)base;
	const int type = (int)base->tex_type;
	u32 base_word, i;
	u8 *to;
	if (dx9c.device == NULL)
		return false;
	if (plan->native)
	{
		D3DLOCKED_RECT rect;
		const u8 *from;
		u32 stride, row, y;
		if ((unsigned)type >= 3 || !dx9c_format_supported(type))
			return false;
		tex_cancel_decode(t);
		if (t->locked_bits != NULL && t->texture != NULL)
			IDirect3DTexture9_UnlockRect(t->texture, 0);
		t->locked_bits = NULL;
		if (!tex_prepare(t, plan->width, plan->height, 1, dx9c_write_usage(), dx9c_texture_format(type)))
			return false;
		if (FAILED(IDirect3DTexture9_LockRect(t->texture, 0, &rect, NULL, dx9c_write_lock_flags())))
			return false;
		from = &vram.data[plan->src + plan->level[0].data];
		stride = (plan->level[0].sr & 0xFFFF) * 2;
		row = plan->width * 2;
		if (stride == row && (u32)rect.Pitch == row)
			memcpy(rect.pBits, from, (size_t)row * plan->height);
		else
			for (y = 0; y < plan->height; y++)
				memcpy((u8 *)rect.pBits + (size_t)y * rect.Pitch, from + (size_t)y * stride, row);
		IDirect3DTexture9_UnlockRect(t->texture, 0);
		t->swap_rb = false;
		return true;
	}
	if (!settings.rend.GpuTextureDecodeD3D9 || !use_decoder || !dx9_texdec_takes(type)
			|| !tex_take_pooled(t, DX9_POOL_DRAWN + (u32)type, plan->width, plan->height, plan->mipmapped != 0)
			|| t->pooled->levels < plan->levels)
		return false;
	tex_cancel_decode(t);
	to = dx9_texdec_begin(plan->pal + plan->pal_entries * 4, plan->levels, &base_word);
	if (to == NULL)
		return false;
	memcpy(to, &vram.data[plan->src], plan->bytes);
	if (plan->pal_entries != 0)
		memcpy(to + plan->pal, (plan->out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan->pal_index, plan->pal_entries * 4);
	t->decode_job = dx9_texdec_next();
	t->decode_serial = dx9_texdec_serial();
	for (i = 0; i < plan->levels; i++)
	{
		const struct texd_level *lv = &plan->level[i];
		u32 pc[6];
		pc[0] = base_word;
		pc[1] = lv->data;
		pc[2] = plan->pal;
		pc[3] = lv->wh;
		pc[4] = lv->sr;
		pc[5] = lv->mode;
		dx9_texdec_add(t->pooled->surfaces[lv->mip], type, pc);
	}
	t->decode_count = plan->levels;
	/* (drawn with its channels where they are sampled from) */
	t->swap_rb = false;
	return true;
}

static void *tex_upload_memory(texcache_data *base, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	dx9_texture *t = (dx9_texture *)base;
	const int type = (int)base->tex_type;
	D3DFORMAT format;
	D3DLOCKED_RECT rect;
	t->locked_bits = NULL;
	/* Into the texture itself: one level, at a pitch that is the texture's
	 * width (a converter writes the rows one after another), and not a
	 * 16-bit format whose bits are in another order here (which they are
	 * turned to on the way in) */
	if (mipmaps || dx9c.device == NULL || bytes != (size_t)width * height * pixel
			|| type == TEX_TYPE_5551 || type == TEX_TYPE_4444)
		return NULL;
	format = dx9c_texture_format(type);
	if (format == D3DFMT_UNKNOWN || !tex_prepare(t, width, height, 1, dx9c_write_usage(), format))
		return NULL;
	if (FAILED(IDirect3DTexture9_LockRect(t->texture, 0, &rect, NULL, dx9c_write_lock_flags())))
		return NULL;
	if ((u32)rect.Pitch != width * pixel)
	{
		IDirect3DTexture9_UnlockRect(t->texture, 0);
		return NULL;
	}
	t->locked_bits = rect.pBits;
	return t->locked_bits;
}

/* The 16-bit textures come out of the texture cache as OpenGL has them,
 * red in the top bits and alpha in the bottom one or four. Direct3D's
 * formats with alpha have it in the top bits, and the rest in the same
 * order: a turn of the bits to the right puts them there, as they are
 * written. */
static void copyRow(u8 *dst, const u8 *src, u32 bytes, unsigned turn)
{
	const u16 *s = (const u16 *)src;
	u16 *d = (u16 *)dst;
	u32 i;
	if (turn == 0)
	{
		memcpy(dst, src, bytes);
		return;
	}
	for (i = 0; i < bytes / 2; i++)
		d[i] = (u16)((s[i] >> turn) | (s[i] << (16 - turn)));
}

static void tex_upload_to_gpu(texcache_data *base, int width, int height, u8 *data, bool mipmapped, bool mipmaps_included)
{
	dx9_texture *t = (dx9_texture *)base;
	const int type = (int)base->tex_type;
	D3DFORMAT format;
	u32 bpp = 2, levels = 1, i;
	unsigned turn = 0;
	bool generate;
	DWORD usage;
	int dim;
	/* what the decoder was to make of it this frame is this now */
	tex_cancel_decode(t);
	if (t->locked_bits != NULL)
	{
		const bool in_place = data == t->locked_bits;
		IDirect3DTexture9_UnlockRect(t->texture, 0);
		t->locked_bits = NULL;
		if (in_place)
		{
			t->swap_rb = type == TEX_TYPE_8888;
			return;
		}
	}
	if (dx9c.device == NULL)
		return;

	format = dx9c_texture_format(type);
	switch (type)
	{
	case TEX_TYPE_5551:
		turn = 1;
		break;
	case TEX_TYPE_4444:
		turn = 4;
		break;
	case TEX_TYPE_565:
		break;
	case TEX_TYPE_8888:
		bpp = 4;
		break;
	case TEX_TYPE_8:
		bpp = 1;
		break;
	default:
		WARN_LOG(RENDERER, "Unsupported texture type %d\n", type);
		return;
	}

	if (mipmaps_included)
		for (dim = width / 2; dim != 0; dim /= 2)
			levels++;
	/* smaller levels made on the card, where it can make them */
	generate = mipmapped && !mipmaps_included && dx9c_can_generate_mips(type);
	usage = dx9c_write_usage() | (generate ? D3DUSAGE_AUTOGENMIPMAP : 0);
	if (!tex_prepare(t, width, height, generate ? 0 : levels, usage, format))
	{
		ERROR_LOG(RENDERER, "Texture creation failed: type %d, %d x %d, %d levels\n", type, width, height, levels);
		return;
	}
	t->swap_rb = type == TEX_TYPE_8888;

	/* (the levels included are from the smallest, 1x1, to the largest) */
	for (i = 0; i < levels; i++)
	{
		const u32 level = levels - i - 1;
		const u32 w = mipmaps_included ? 1u << i : (u32)width;
		const u32 h = mipmaps_included ? 1u << i : (u32)height;
		const u32 row = w * bpp;
		D3DLOCKED_RECT rect;
		u32 y;
		if (FAILED(IDirect3DTexture9_LockRect(t->texture, level, &rect, NULL, dx9c_write_lock_flags())))
		{
			WARN_LOG(RENDERER, "Texture lock failed: %d x %d, level %d\n", w, h, level);
			return;
		}
		if ((u32)rect.Pitch == row)
			copyRow((u8 *)rect.pBits, data, row * h, turn);
		else
			for (y = 0; y < h; y++)
				copyRow((u8 *)rect.pBits + (size_t)y * rect.Pitch, data + (size_t)y * row, row, turn);
		IDirect3DTexture9_UnlockRect(t->texture, level);
		data += (size_t)row * h;
	}
}

static bool tex_force_32bit(const texcache_data *base, int type)
{
	(void)base;
	return !dx9c_format_supported(type);
}

static bool tex_delete(texcache_data *base)
{
	dx9_texture *t = (dx9_texture *)base;
	if (!texcache_delete(base))
		return false;
	if (t->locked_bits != NULL && t->texture != NULL)
		IDirect3DTexture9_UnlockRect(t->texture, 0);
	t->locked_bits = NULL;
	tex_release_pooled(t);
	dx9_texture_release_spare(t);
	dx9c_recycle(&t->texture);
	DX9_RELEASE(t->spare_surface);
	dx9c_recycle(&t->spare);
	return true;
}

static void tex_get_id(texcache_data *base, char *s, size_t n)
{
	char id[32];
	sprintf(id, "%p", (void *)base);
	strlcpy(s, id, n);
}

/* ---- Renders to textures, and video memory (rend/rtt_watch.h) ---- */

/* Pictures in system memory a render target is read back into, kept from
 * one read to the next by size: a read can come at any time, the middle
 * of a frame included, and what is made for it is used again. And the
 * target a picture made finer than the console's is brought down to the
 * console's size in. */
typedef struct dx9_staging
{
	u32 width, height;
	IDirect3DSurface9 *surface;
} dx9_staging;
static dx9_staging staging[4];
static u32 stagingNext;
static IDirect3DTexture9 *small_texture;
static IDirect3DSurface9 *small_surface;

static IDirect3DSurface9 *stagingFor(IDirect3DDevice9 *device, u32 width, u32 height)
{
	dx9_staging *s;
	u32 i;
	for (i = 0; i < ARRAY_SIZE(staging); i++)
		if (staging[i].surface != NULL && staging[i].width == width && staging[i].height == height)
			return staging[i].surface;
	s = &staging[stagingNext++ % ARRAY_SIZE(staging)];
	DX9_RELEASE(s->surface);
	if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(device, width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s->surface, NULL)))
	{
		WARN_LOG(RENDERER, "Read back surface creation failed (%d x %d)\n", width, height);
		s->surface = NULL;
		return NULL;
	}
	s->width = width;
	s->height = height;
	return s->surface;
}

/* @width by @height from (@x, @y) of a render target into @rgba */
static bool readBack(IDirect3DSurface9 *surface, u32 x, u32 y, u32 width, u32 height, u8 *rgba)
{
	IDirect3DDevice9 *device = dx9c.device;
	D3DSURFACE_DESC desc;
	IDirect3DSurface9 *sysmem = NULL;
	D3DLOCKED_RECT rect;
	RECT area;
	u32 l, i;
	if (device == NULL || surface == NULL || FAILED(IDirect3DSurface9_GetDesc(surface, &desc))
			|| x + width > desc.Width || y + height > desc.Height
			|| (sysmem = stagingFor(device, desc.Width, desc.Height)) == NULL)
	{
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	dx9c_end_scene();
	area.left = (LONG)x;
	area.top = (LONG)y;
	area.right = (LONG)(x + width);
	area.bottom = (LONG)(y + height);
	if (FAILED(IDirect3DDevice9_GetRenderTargetData(device, surface, sysmem))
			|| FAILED(IDirect3DSurface9_LockRect(sysmem, &rect, &area, D3DLOCK_READONLY)))
	{
		WARN_LOG(RENDERER, "A picture could not be read back from the card\n");
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	/* B G R A to R G B A */
	for (l = 0; l < height; l++)
	{
		const u8 *src = (const u8 *)rect.pBits + (size_t)l * rect.Pitch;
		u8 *dst = rgba + (size_t)l * width * 4;
		for (i = 0; i < width; i++, src += 4, dst += 4)
		{
			dst[0] = src[2];
			dst[1] = src[1];
			dst[2] = src[0];
			dst[3] = src[3];
		}
	}
	IDirect3DSurface9_UnlockRect(sysmem);
	return true;
}

bool dx9_read_picture(IDirect3DTexture9 *texture, u32 width, u32 height, u8 *rgba)
{
	IDirect3DSurface9 *surface = NULL;
	bool ok;
	dx9c_lock();
	if (texture == NULL || FAILED(IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface)))
	{
		memset(rgba, 0, (size_t)width * height * 4);
		dx9c_unlock();
		return false;
	}
	ok = readBack(surface, 0, 0, width, height, rgba);
	DX9_RELEASE(surface);
	dx9c_unlock();
	return ok;
}

/* The picture as the console would have it: w by h, R G B A. */
static void dx9_rtt_read_locked(const RttWatch *watch, u8 *rgba)
{
	IDirect3DDevice9 *device = dx9c.device;
	dx9_texture *texture = (dx9_texture *)watch->tex;
	const u32 w = watch->w, h = watch->h, scale = watch->scale > 1 ? watch->scale : 1;
	const u32 x0 = watch->x0 * scale, y0 = watch->y0 * scale;
	IDirect3DSurface9 *surface = NULL;
	D3DSURFACE_DESC src;

	if (device == NULL || texture->texture == NULL || FAILED(IDirect3DTexture9_GetSurfaceLevel(texture->texture, 0, &surface)))
	{
		memset(rgba, 0, (size_t)w * h * 4);
		return;
	}
	if (scale == 1)
	{
		readBack(surface, x0, y0, w, h, rgba);
		DX9_RELEASE(surface);
		return;
	}
	/* Made the console's size on the card, filtered, and read at that: the
	 * clip's part of the texture drawn onto a picture of w by h. */
	if (small_texture != NULL)
	{
		D3DSURFACE_DESC desc;
		IDirect3DTexture9_GetLevelDesc(small_texture, 0, &desc);
		if (desc.Width < w || desc.Height < h)
		{
			DX9_RELEASE(small_surface);
			DX9_RELEASE(small_texture);
		}
	}
	if (small_texture == NULL)
	{
		small_texture = dx9c_make_texture(w > 640 ? w : 640, h > 480 ? h : 480, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8);
		if (small_texture == NULL || FAILED(IDirect3DTexture9_GetSurfaceLevel(small_texture, 0, &small_surface)))
		{
			WARN_LOG(RENDERER, "Render to texture read back: target creation failed\n");
			small_surface = NULL;
			DX9_RELEASE(small_texture);
			memset(rgba, 0, (size_t)w * h * 4);
			DX9_RELEASE(surface);
			return;
		}
	}
	IDirect3DSurface9_GetDesc(surface, &src);
	dx9c_set_target(small_surface, NULL, w, h);
	dx9c_draw_quad_at(texture->texture, true, false, -1.f, -1.f, 2.f, 2.f,
			(float)x0 / src.Width, (float)y0 / src.Height,
			(float)(x0 + w * scale) / src.Width, (float)(y0 + h * scale) / src.Height, false, 1.f);
	readBack(small_surface, 0, 0, w, h, rgba);
	DX9_RELEASE(surface);
}

static void dx9_rtt_read(const RttWatch *watch, u8 *rgba)
{
	dx9c_lock();
	dx9_rtt_read_locked(watch, rgba);
	dx9c_unlock();
}

/* The texture cache's, never this module's to delete. */
static void dx9_rtt_release(uintptr_t tex)
{
	(void)tex;
}

/* (read back through a surface locked a line at a time: handed over from @scratch) */
static void dx9_rtt_take(const RttWatch *watch, u8 *scratch, RttWatchTake take)
{
	dx9_rtt_read(watch, scratch);
	take(watch, scratch, (size_t)watch->w * 4);
}

static const RttWatchBackend dx9_rtt_backend = { dx9_rtt_take, dx9_rtt_release };

/* What the render covers of video memory: the clip's pixels, from (x0, y0)
 * of the frame at addr, which moves to the first of them. (As the Vulkan
 * renderers' rtt_read.cpp has it.) */
static void covered(u32 *addr, u32 x0, u32 y0, u32 *width, u32 height, u32 *stride, u32 *bytes)
{
	*stride = FB_W_LINESTRIDE.stride * 8;
	if (*stride == 0)
		*stride = (x0 + *width) * 2;
	else if ((x0 + *width) * 2 > *stride)
		/* Happens for Virtua Tennis */
		*width = x0 * 2 >= *stride ? 0 : *stride / 2 - x0;
	*addr += y0 * *stride + x0 * 2;
	*bytes = *width == 0 || height == 0 ? 0 : *stride * (height - 1) + *width * 2;
}

/* From the texture rendered to: its part stretched onto a texture that
 * can be rendered to - the pool's, or one made - filtered where the render
 * is finer. On the device, in order with the render and what draws with
 * this. */
static bool tex_copy_render(texcache_data *base, texcache_data *from, u32 x, u32 y, u32 scale, u32 columns)
{
	dx9_texture *t = (dx9_texture *)base;
	IDirect3DDevice9 *device = dx9c.device;
	dx9_texture *src = (dx9_texture *)from;
	IDirect3DSurface9 *from_surface = NULL, *to_surface = NULL;
	D3DSURFACE_DESC sd;
	RECT rect, to;
	bool ok;

	if (device == NULL || src == t || src->texture == NULL || columns == 0 || columns > base->w
			|| FAILED(IDirect3DTexture9_GetLevelDesc(src->texture, 0, &sd)) || sd.Format != D3DFMT_A8R8G8B8
			|| (sd.Usage & D3DUSAGE_RENDERTARGET) == 0)
		return false;
	if (scale < 1)
		scale = 1;
	if ((x + columns) * scale > sd.Width || (y + base->h) * scale > sd.Height)
		return false;
	tex_cancel_decode(t);
	base->tex_type = TEX_TYPE_8888;
	if (!tex_take_pooled(t, DX9_POOL_TARGET, base->w, base->h, false))
	{
		D3DSURFACE_DESC desc;
		tex_release_pooled(t);
		if (t->texture != NULL && (FAILED(IDirect3DTexture9_GetLevelDesc(t->texture, 0, &desc)) || desc.Width != base->w
				|| desc.Height != base->h || desc.Format != D3DFMT_A8R8G8B8 || (desc.Usage & D3DUSAGE_RENDERTARGET) == 0
				|| IDirect3DTexture9_GetLevelCount(t->texture) != 1))
			dx9c_recycle(&t->texture);
		if (t->texture == NULL)
			t->texture = dx9c_make_texture(base->w, base->h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8);
		if (t->texture == NULL)
			return false;
	}
	if (FAILED(IDirect3DTexture9_GetSurfaceLevel(src->texture, 0, &from_surface)))
		return false;
	if (FAILED(IDirect3DTexture9_GetSurfaceLevel(t->texture, 0, &to_surface)))
	{
		DX9_RELEASE(from_surface);
		return false;
	}
	rect.left = (LONG)(x * scale);
	rect.top = (LONG)(y * scale);
	rect.right = (LONG)((x + columns) * scale);
	rect.bottom = (LONG)((y + base->h) * scale);
	to.left = 0;
	to.top = 0;
	to.right = (LONG)columns;
	to.bottom = (LONG)base->h;
	ok = true;
	if (columns < base->w)
	{
		/* (the rest of each row a narrower stride leaves) */
		RECT rest;
		rest.left = (LONG)columns;
		rest.top = 0;
		rest.right = (LONG)base->w;
		rest.bottom = (LONG)base->h;
		ok = SUCCEEDED(IDirect3DDevice9_ColorFill(device, to_surface, &rest,
				D3DCOLOR_ARGB(texcache_clear_opaque(base) ? 255 : 0, 0, 0, 0)));
	}
	ok = ok && SUCCEEDED(IDirect3DDevice9_StretchRect(device, from_surface, &rect, to_surface, &to,
			scale > 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT));
	DX9_RELEASE(to_surface);
	DX9_RELEASE(from_surface);
	if (!ok)
		return false;
	t->swap_rb = false;
	return true;
}

void dx9_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	u32 stride, bytes;

	covered(&addr, x0, y0, &width, height, &stride, &bytes);
	if (bytes != 0)
		rtt_watch_supersede(addr, bytes);
}

void dx9_rtt_watch(dx9_texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	RttWatch watch;

	memset(&watch, 0, sizeof(watch));
	covered(&addr, x0, y0, &width, height, &watch.stride, &watch.bytes);
	watch.addr = addr;
	watch.w = width;
	watch.h = height;
	watch.x0 = x0;
	watch.y0 = y0;
	watch.packmode = FB_W_CTRL.fb_packmode;
	watch.kval_bit = (FB_W_CTRL.fb_kval & 0x80) << 8;
	watch.alpha_threshold = FB_W_CTRL.fb_alpha_threshold;
	watch.dither = FB_W_CTRL.fb_dither;
	watch.tex = (uintptr_t)texture;
	watch.target = &texture->base;
	watch.scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	if (watch.bytes != 0)
		rtt_watch_add(&watch, &dx9_rtt_backend);
}

void dx9_rtt_term(void)
{
	u32 i;
	rtt_watch_term();
	DX9_RELEASE(small_surface);
	DX9_RELEASE(small_texture);
	for (i = 0; i < ARRAY_SIZE(staging); i++)
		DX9_RELEASE(staging[i].surface);
}

/* ---- The cache ---- */

/* (no part of a texture made again alone: the whole of it, or on the CPU) */
static bool tex_part_memory(texcache_data *base, size_t bytes, void **memory)
{
	(void)base;
	(void)bytes;
	(void)memory;
	return false;
}

static void tex_upload_part(texcache_data *base, const TexRegion *regions, u32 count, u8 *data, const size_t *offset)
{
	(void)base;
	(void)regions;
	(void)count;
	(void)data;
	(void)offset;
}

static int tex_gpu_decode_part(texcache_data *base, const struct texd_plan *plan, const TexRegion *regions, u32 count)
{
	(void)base;
	(void)plan;
	(void)regions;
	(void)count;
	return TEXP_CPU;
}

static const texcache_ops dx9_texture_ops = {
	tex_get_id,
	tex_upload_to_gpu,
	tex_upload_memory,
	tex_gpu_decode,
	tex_part_memory,
	tex_upload_part,
	tex_gpu_decode_part,
	tex_copy_render,
	tex_force_32bit,
	tex_delete
};

static tex_map_t tex_cache;

/* (the texture going: what it holds given back, as a texture's own end) */
static void tex_unmake(void *e)
{
	dx9_texture *t = (dx9_texture *)e;
	tex_release_pooled(t);
	dx9_texture_release_spare(t);
	DX9_RELEASE(t->spare_surface);
	DX9_RELEASE(t->spare);
	DX9_RELEASE(t->texture);
}

static void tex_cache_ready(void)
{
	if (tex_cache.elem == 0)
		tex_map_init(&tex_cache, sizeof(dx9_texture), NULL, tex_unmake);
}

dx9_texture *dx9_texcache_get(u32 tsp, u32 tcw, int area)
{
	tex_cache_ready();
	return (dx9_texture *)texcache_map_get(&tex_cache, &dx9_texture_ops, tsp, tcw, area);
}

void dx9_texcache_reserve_for(u32 images)
{
	tex_cache_ready();
	texcache_map_reserve_for(&tex_cache, images);
}

void dx9_texcache_cleanup(void)
{
	tex_cache_ready();
	texcache_map_collect(&tex_cache);
}

void dx9_texcache_clear(void)
{
	tex_cache_ready();
	texcache_map_clear(&tex_cache);
}

void dx9_texcache_free(void)
{
	if (tex_cache.elem != 0)
		tex_map_free(&tex_cache);
	memset(&tex_cache, 0, sizeof(tex_cache));
}

void dx9_textures_attach(bool decoder, bool pool_too)
{
	use_decoder = decoder;
	use_pool = pool_too;
}
