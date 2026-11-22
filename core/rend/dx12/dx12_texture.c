/*
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
#include "dx12_texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <compat/strl.h>
#include "dx12context_lr.h"
#include "dx12_texdecoder.h"
#include "rend/rtt_watch.h"
#include "rend/tex_map.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"

static const IID iid_resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };

/* The renderer's, while it has them */
static bool use_decoder;
static bool use_pool;

#define MAX_U(a, b) ((a) > (b) ? (a) : (b))

/* ---- The texture pool ---- */

/* by TEX_TYPE_*: 565, 5551, 4444, 8888, 8; and the pictures rendered to */
static const DXGI_FORMAT poolFormats[DX12_POOL_FORMATS] = {
	DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UNORM,
	DXGI_FORMAT_R8G8B8A8_UNORM
};
static const char * const poolFormatNames[DX12_POOL_FORMATS] = { "565", "5551", "4444", "8888", "8-bit", "8888 rendered to" };
static const u32 poolTexel[DX12_POOL_FORMATS] = { 2, 2, 2, 4, 1, 4 };

static struct
{
	tex_pool_book_t book;
	bool ready;
	bool usable[DX12_POOL_FORMATS];
	bool writable[DX12_POOL_FORMATS];
	bool targetStorage;
	dx12_pooled_texture *all;
} pool;

/* The 16-bit textures turned where they are not to be in place (textures
 * dumped): as large as the largest, made with the pool when textures are
 * dumped (tex_upload_to_gpu()) */
static u16 *turned;
#define TURNED_TEXELS (1024 * 1024 + 1024 * 1024 / 3 + 1)

/* Rows of @bytes each, @rows of them, into the upload ring at Direct3D's
 * pitch (a multiple of 256), for CopyTextureRegion. False if the ring
 * cannot hold them. */
static bool upload_rows(const u8 *src, u32 srcPitch, u32 bytes, u32 rows, dx12_upload *out, u32 *pitch)
{
	*pitch = (bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	if (!dx12c_upload(*pitch * rows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, out))
		return false;
	if (src != NULL)
	{
		if (*pitch == bytes && srcPitch == bytes)
			memcpy(out->cpu, src, (size_t)bytes * rows);
		else
		{
			u32 y;
			for (y = 0; y < rows; y++)
				memcpy(out->cpu + (size_t)y * *pitch, src + (size_t)y * srcPitch, bytes);
		}
	}
	return true;
}

/* The copy of what upload_rows() put in the ring into @level of @texture,
 * at @x, @y of it */
static void copy_to_region(dx12_pooled_texture *texture, u32 level, u32 x, u32 y, u32 width, u32 height,
		const dx12_upload *upload, u32 pitch)
{
	D3D12_TEXTURE_COPY_LOCATION dst, src;
	memset(&dst, 0, sizeof(dst));
	dst.pResource = texture->resource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = level;
	memset(&src, 0, sizeof(src));
	src.pResource = upload->buffer;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = upload->offset;
	src.PlacedFootprint.Footprint.Format = texture->format;
	src.PlacedFootprint.Footprint.Width = width;
	src.PlacedFootprint.Footprint.Height = height;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = pitch;
	ID3D12GraphicsCommandList_CopyTextureRegion(dx12c_cmd(), &dst, x, y, 0, &src, NULL);
}

static void to_copy(dx12_pooled_texture *t)
{
	dx12c_transition_resource(t->resource, &t->state, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}

static void to_sampled(dx12_pooled_texture *t)
{
	dx12c_transition_resource(t->resource, &t->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}

static bool pool_make_resource(dx12_pooled_texture *t, u32 f, u32 width, u32 height, u32 levels)
{
	const bool wide = f == TEX_TYPE_8888 || f == TEX_TYPE_8;
	/* (typeless, for the unsigned integer views the kernels write through) */
	const DXGI_FORMAT format = t->writable && wide ? (f == TEX_TYPE_8 ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS) : poolFormats[f];
	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
	t->width = width;
	t->height = height;
	t->levels = levels;
	t->format = poolFormats[f];
	t->srv = ~0u;
	t->rtv = ~0u;
	if (f == DX12_POOL_TARGET || (t->writable && !wide))
		flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	else if (t->writable)
		flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (f == DX12_POOL_TARGET && pool.targetStorage)
		flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	t->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	if (!dx12_make_texture(dx12c.device, width, height, levels, format, flags, t->state, &t->resource, NULL))
		return false;
	t->srv = dx12_heap_alloc(&dx12c.srv_heap);
	if (t->srv == ~0u)
	{
		WARN_LOG(RENDERER, "DX12: the bindless table is full\n");
		DX12_RELEASE(t->resource);
		return false;
	}
	dx12c_make_srv(t->resource, poolFormats[f], levels, t->srv);
	if (f == DX12_POOL_TARGET)
	{
		t->rtv = dx12_heap_alloc(&dx12c.rtv_heap);
		if (t->rtv == ~0u)
		{
			dx12_heap_free(&dx12c.srv_heap, t->srv);
			t->srv = ~0u;
			DX12_RELEASE(t->resource);
			return false;
		}
		ID3D12Device_CreateRenderTargetView(dx12c.device, t->resource, NULL, dx12_heap_cpu(&dx12c.rtv_heap, t->rtv));
	}
	return true;
}

static dx12_pooled_texture *pool_make(int cls, bool late)
{
	const u32 f = tex_pool_format(cls);
	const u32 width = tex_pool_width(cls), height = tex_pool_height(cls);
	dx12_pooled_texture *t = (dx12_pooled_texture *)calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;
	t->writable = pool.writable[f];
	if (!pool_make_resource(t, f, width, height, tex_pool_mips(cls) ? 1 + bitscanrev(MAX_U(width, height)) : 1))
	{
		WARN_LOG(RENDERER, "DX12: texture pool: a %ux%u %s texture could not be made\n", width, height, poolFormatNames[f]);
		free(t);
		return NULL;
	}
	tex_pool_made(&pool.book, &t->entry, cls, tex_pool_bytes(cls, poolTexel[f]), late, poolFormatNames[f]);
	t->all = pool.all;
	pool.all = t;
	return t;
}

void dx12_pool_init(void)
{
	/* how much games use them, in eighths of what they use 565 */
	static const u32 use[DX12_POOL_FORMATS] = { 8, 6, 6, 4, 2, 0 };
	static const u8 kind[DX12_POOL_FORMATS] = { 0, 0, 0, 0, 0, TEX_POOL_TARGETS };
	static u32 counts[TEX_POOL_CLASSES];
	u32 have[DX12_POOL_FORMATS];
	u64 bytes;
	int f, c;
	u32 i;
	if (pool.ready)
		return;
	tex_pool_reset(&pool.book, 1);
	for (f = 0; f < DX12_POOL_FORMATS; f++)
	{
		pool.usable[f] = f == DX12_POOL_TARGET || dx12c_format_supported(f);
		pool.writable[f] = f != DX12_POOL_TARGET && pool.usable[f] && dx12_texdec_takes(f);
		have[f] = pool.usable[f] ? poolTexel[f] : 0;
	}
	bytes = tex_pool_plan(&dx12c.vram_budget, have, use, kind, DX12_POOL_FORMATS, counts,
			texcache_upscale_factor(), (u32)settings.rend.MaxFilteredTextureSize,
			settings.rend.RenderToTextureUpscale > 1 ? (u32)settings.rend.RenderToTextureUpscale : 1u);
	pool.ready = true;
	for (c = 0; c < TEX_POOL_CLASSES; c++)
		for (i = 0; i < counts[c]; i++)
		{
			dx12_pooled_texture *t = pool_make(c, false);
			if (t == NULL)
				break;
			tex_pool_give(&pool.book, &t->entry);
		}
	if (turned == NULL && settings.rend.DumpTextures)
		turned = (u16 *)malloc(TURNED_TEXELS * sizeof(u16));
	NOTICE_LOG(RENDERER, "DX12: texture pool of %u textures, %u MB (planned %u MB)\n", pool.book.made,
			(u32)(pool.book.bytesMade >> 20), (u32)(bytes >> 20));
}

void dx12_pool_term(void)
{
	if (!pool.ready)
		return;
	tex_pool_summary(&pool.book, "DX12");
	while (pool.all != NULL)
	{
		dx12_pooled_texture *next = pool.all->all;
		if (pool.all->srv != ~0u)
			dx12_heap_free(&dx12c.srv_heap, pool.all->srv);
		if (pool.all->rtv != ~0u)
			dx12_heap_free(&dx12c.rtv_heap, pool.all->rtv);
		DX12_RELEASE(pool.all->resource);
		free(pool.all);
		pool.all = next;
	}
	tex_pool_reset(&pool.book, 1);
	pool.ready = false;
	free(turned);
	turned = NULL;
}

u32 dx12_pool_made(void)
{
	return pool.book.made;
}

void dx12_pool_set_target_storage(bool on)
{
	pool.targetStorage = on;
}

bool dx12_pool_targets_storable(void)
{
	return pool.targetStorage;
}

dx12_pooled_texture *dx12_pool_make_own(u32 f, u32 width, u32 height, u32 levels)
{
	dx12_pooled_texture *t;
	if (!pool.ready || f >= DX12_POOL_FORMATS || !pool.usable[f] || width == 0 || height == 0)
		return NULL;
	t = (dx12_pooled_texture *)calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;
	t->writable = false;
	t->own = true;
	t->entry.cls = ~0u;
	if (!pool_make_resource(t, f, width, height, levels))
	{
		WARN_LOG(RENDERER, "DX12: a %ux%u %s texture could not be made\n", width, height, poolFormatNames[f]);
		free(t);
		return NULL;
	}
	t->entry.bytes = 0;
	return t;
}

void dx12_pool_give(dx12_pooled_texture *texture)
{
	if (texture == NULL)
		return;
	if (!texture->own)
	{
		tex_pool_give(&pool.book, &texture->entry);
		return;
	}
	/* one made for one texture: goes once the GPU is done with it */
	if (texture->resource != NULL)
	{
		dx12c_release((IUnknown *)texture->resource);
		texture->resource = NULL;
	}
	dx12c_release_srv(texture->srv);
	if (texture->rtv != ~0u)
		dx12_heap_free(&dx12c.rtv_heap, texture->rtv);
	free(texture);
}

static dx12_pooled_texture *pool_take_class(int cls)
{
	TexPoolEntry *e;
	if (cls < 0)
		return NULL;
	e = tex_pool_take(&pool.book, cls);
	/* (the entry first in each) */
	return e != NULL ? (dx12_pooled_texture *)e : pool_make(cls, true);
}

dx12_pooled_texture *dx12_pool_take_target(u32 width, u32 height)
{
	if (!pool.ready)
		return NULL;
	return pool_take_class(tex_pool_class(DX12_POOL_TARGET, width, height, false));
}

/* A texture of @type, @width by @height, with its levels or not: a free
 * one of that shape, or one made now if there is none. Null if the shape
 * is not one of the pool's. */
static dx12_pooled_texture *pool_take(int type, u32 width, u32 height, bool mips)
{
	const u32 f = (u32)type;
	if (!pool.ready || f >= 5 || !pool.usable[f])
		return NULL;
	return pool_take_class(tex_pool_class(f, width, height, mips));
}

/* ---- The cache's textures ---- */

static void tex_cancel_decode(dx12_texture *t)
{
	if (t->decode_count != 0 && use_decoder)
		dx12_texdec_cancel(t->decode_job, t->decode_count, t->decode_serial);
	t->decode_count = 0;
}

static void tex_release_pooled(dx12_texture *t)
{
	tex_cancel_decode(t);
	t->upload_cpu = NULL;
	if (t->pooled == NULL)
		return;
	if (use_pool)
		dx12_pool_give(t->pooled);
	t->pooled = NULL;
}

/* The pool's texture for this shape, the one it has if it is that;
 * false if the pool has none of this shape */
static bool tex_take_pooled(dx12_texture *t, u32 width, u32 height, bool mips)
{
	dx12_pooled_texture *p;
	if (!use_pool)
		return false;
	if (t->pooled != NULL && !t->pooled->own
			&& (int)t->pooled->entry.cls == tex_pool_class((u32)t->base.tex_type, width, height, mips))
		return true;
	p = pool_take((int)t->base.tex_type, width, height, mips);
	if (p == NULL)
		return false;
	tex_release_pooled(t);
	t->pooled = p;
	return true;
}

bool dx12_texture_take_spare(dx12_texture *t, u32 width, u32 height)
{
	dx12_pooled_texture *p = use_pool ? dx12_pool_take_target(width, height) : NULL;
	if (p == NULL)
		return false;
	dx12_texture_release_spare(t);
	t->spare = p;
	return true;
}

void dx12_texture_set_spare(dx12_texture *t, dx12_pooled_texture *spare)
{
	dx12_texture_release_spare(t);
	t->spare = spare;
}

void dx12_texture_release_spare(dx12_texture *t)
{
	if (t->spare == NULL)
		return;
	if (use_pool)
		dx12_pool_give(t->spare);
	t->spare = NULL;
}

void dx12_texture_swap_with_spare(dx12_texture *t)
{
	tex_release_pooled(t);
	t->pooled = t->spare;
	t->spare = NULL;
}

/* Made on the GPU (dx12_texdecoder.h), or copied as it is. Everything that
 * can say no does before anything is changed but the texture it is made
 * into, which a no leaves for the CPU's upload to take. */
static bool tex_gpu_decode(texcache_data *base, const struct texd_plan *plan)
{
	dx12_texture *t = (dx12_texture *)base;
	const int type = (int)base->tex_type;
	u32 base_word, d3d16, i;
	u8 *to;
	if (dx12c.device == NULL || !use_pool)
		return false;
	if (plan->native)
	{
		dx12_upload upload;
		u32 pitch;
		/* A planar 16-bit texture: the PowerVR's texels are Direct3D's
		 * as they are, and its rows go from video memory to the texture */
		if (type == TEX_TYPE_8888 || type == TEX_TYPE_8 || !dx12c_format_supported(type)
				|| !tex_take_pooled(t, plan->width, plan->height, false))
			return false;
		tex_cancel_decode(t);
		if (!upload_rows(&vram.data[plan->src + plan->level[0].data], (plan->level[0].sr & 0xFFFF) * 2, plan->width * 2,
				plan->height, &upload, &pitch))
			return false;
		to_copy(t->pooled);
		copy_to_region(t->pooled, 0, 0, 0, plan->width, plan->height, &upload, pitch);
		to_sampled(t->pooled);
		return true;
	}
	if (!use_decoder || !dx12_texdec_takes(type)
			|| !tex_take_pooled(t, plan->width, plan->height, plan->mipmapped != 0) || !t->pooled->writable
			|| t->pooled->levels < plan->levels)
		return false;
	tex_cancel_decode(t);
	to = dx12_texdec_begin(plan->pal + plan->pal_entries * 4, plan->levels, &base_word);
	if (to == NULL)
		return false;
	memcpy(to, &vram.data[plan->src], plan->bytes);
	if (plan->pal_entries != 0)
		memcpy(to + plan->pal, (plan->out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan->pal_index, plan->pal_entries * 4);

	d3d16 = plan->out != TEXD_OUT16 ? 0
			: TEXD_D3D16(type == TEX_TYPE_5551 ? TEXD_D3D_5551 : type == TEX_TYPE_4444 ? TEXD_D3D_4444 : TEXD_D3D_565);
	t->decode_job = dx12_texdec_next();
	t->decode_serial = dx12_texdec_serial();
	for (i = 0; i < plan->levels; i++)
	{
		const struct texd_level *lv = &plan->level[i];
		u32 pc[6];
		pc[0] = base_word;
		pc[1] = lv->data;
		pc[2] = plan->pal;
		pc[3] = lv->wh;
		pc[4] = lv->sr;
		pc[5] = lv->mode | d3d16;
		dx12_texdec_add(t->pooled, lv->mip, pc);
	}
	t->decode_count = plan->levels;
	return true;
}

/* The 16-bit textures come out of the texture cache as OpenGL has them,
 * red in the top bits and alpha in the bottom one or four. Direct3D's
 * formats with alpha have it in the top bits, and the rest in the same
 * order: a turn of the bits to the right puts them there. */
static const u16 *turn_bits(u8 *data, size_t texels, unsigned by, bool inPlace)
{
	u16 *src = (u16 *)data;
	u16 *dst = src;
	size_t i;
	/* (in place when there is no room: the dump has the texture turned) */
	if (!inPlace && turned != NULL && texels <= TURNED_TEXELS)
		dst = turned;
	for (i = 0; i < texels; i++)
		dst[i] = (u16)((src[i] >> by) | (src[i] << (16 - by)));
	return dst;
}

/* The cache converts straight into the upload ring where the texture's
 * rows are Direct3D's pitch as they are (a multiple of 256 bytes), and
 * nothing is turned after: the texture types whose bits are in Direct3D's
 * order (upload memory is write-combined: not to be read back). */
static void *tex_upload_memory(texcache_data *base, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	dx12_texture *t = (dx12_texture *)base;
	const int type = (int)base->tex_type;
	t->upload_cpu = NULL;
	if (dx12c.device == NULL || !use_pool || mipmaps || (width * pixel) % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0
			|| bytes != (size_t)width * height * pixel || type == TEX_TYPE_5551 || type == TEX_TYPE_4444
			|| tex_pool_class((u32)type, width, height, false) < 0)
		return NULL;
	if (!dx12c_upload((u32)bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &t->upload_piece))
		return NULL;
	t->upload_cpu = t->upload_piece.cpu;
	t->upload_pitch = width * pixel;
	return t->upload_piece.cpu;
}

static void tex_upload_to_gpu(texcache_data *base, int width, int height, u8 *data, bool mipmapped, bool mipmaps_included)
{
	dx12_texture *t = (dx12_texture *)base;
	const int type = (int)base->tex_type;
	u8 *inRing;
	u32 bpp = 2, levels = 1;
	unsigned turn = 0;
	int dim;
	(void)mipmapped;

	if (dx12c.device == NULL || !use_pool)
		return;
	/* what the decoder was to make of it this frame is this now */
	tex_cancel_decode(t);
	inRing = t->upload_cpu;
	t->upload_cpu = NULL;

	switch (type)
	{
	case TEX_TYPE_5551:
		turn = 1;
		break;
	case TEX_TYPE_565:
		break;
	case TEX_TYPE_4444:
		turn = 4;
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
	/* (a mipmapped texture whose levels are not given - a custom one - is
	 * sampled from its largest level: Direct3D 12 makes no levels itself) */

	/* One of the pool's, made before the game; one of its own for a shape
	 * the pool has not (a custom texture) */
	if (!tex_take_pooled(t, width, height, mipmaps_included))
	{
		dx12_pooled_texture *p = t->pooled;
		if (p == NULL || !p->own || p->width != (u32)width || p->height != (u32)height
				|| p->levels != levels || p->format != poolFormats[type])
		{
			tex_release_pooled(t);
			t->pooled = dx12_pool_make_own((u32)type, width, height, levels);
			if (t->pooled == NULL)
				return;
		}
	}
	if (turn != 0 && data != inRing)
	{
		size_t texels = (size_t)width * height;
		if (mipmaps_included)
			for (dim = width / 2; dim != 0; dim /= 2)
				texels += (size_t)dim * dim;
		/* (in the texture cache's own buffer, which nothing reads after
		 * this but a texture dump, which wants it as it is) */
		data = (u8 *)turn_bits(data, texels, turn, !settings.rend.DumpTextures);
	}

	to_copy(t->pooled);
	if (data == inRing && inRing != NULL)
		/* converted into the ring already (tex_upload_memory()) */
		copy_to_region(t->pooled, 0, 0, 0, width, height, &t->upload_piece, t->upload_pitch);
	else if (mipmaps_included)
	{
		u32 i;
		/* from the smallest level, 1x1, to the largest */
		for (i = 0; i < levels; i++)
		{
			const u32 d = 1u << i;
			dx12_upload upload;
			u32 pitch;
			if (upload_rows(data, d * bpp, d * bpp, d, &upload, &pitch))
				copy_to_region(t->pooled, levels - i - 1, 0, 0, d, d, &upload, pitch);
			data += (size_t)d * d * bpp;
		}
	}
	else
	{
		dx12_upload upload;
		u32 pitch;
		if (upload_rows(data, width * bpp, width * bpp, height, &upload, &pitch))
			copy_to_region(t->pooled, 0, 0, 0, width, height, &upload, pitch);
	}
	to_sampled(t->pooled);
}

/* The image the last whole update made, and nothing the decoder still has
 * to do to it */
static bool tex_part_ready(const dx12_texture *t)
{
	const texcache_data *base = &t->base;
	const dx12_pooled_texture *p = t->pooled;
	if (base->made_type != (u8)base->tex_type + 1 || p == NULL || (unsigned)base->tex_type >= 5 || dx12c.device == NULL
			|| (t->decode_count != 0 && use_decoder && t->decode_serial == dx12_texdec_serial()))
		return false;
	return p->width == base->w && p->height == base->h && p->format == poolFormats[(int)base->tex_type]
			&& p->levels == (base->made_mips ? 1 + bitscanrev(base->w) : 1);
}

static bool tex_part_memory(texcache_data *base, size_t bytes, void **memory)
{
	dx12_texture *t = (dx12_texture *)base;
	(void)bytes;
	/* (the cache's own: copied into the ring at Direct3D's pitch) */
	*memory = NULL;
	t->upload_cpu = NULL;
	return tex_part_ready(t);
}

static void tex_upload_part(texcache_data *base, const TexRegion *regions, u32 count, u8 *data, const size_t *offset)
{
	dx12_texture *t = (dx12_texture *)base;
	const int type = (int)base->tex_type;
	const u32 bpp = type == TEX_TYPE_8888 ? 4 : type == TEX_TYPE_8 ? 1 : 2;
	const unsigned turn = type == TEX_TYPE_5551 ? 1 : type == TEX_TYPE_4444 ? 4 : 0;
	u32 i;

	to_copy(t->pooled);
	for (i = 0; i < count; i++)
	{
		const TexRegion *r = &regions[i];
		dx12_upload upload;
		u32 pitch;
		if (turn != 0)
			turn_bits(data + offset[i], (size_t)r->w * r->h, turn, true);
		if (upload_rows(data + offset[i], r->w * bpp, r->w * bpp, r->h, &upload, &pitch))
			copy_to_region(t->pooled, r->level, r->x, r->y, r->w, r->h, &upload, pitch);
	}
	to_sampled(t->pooled);
}

/* A planar 16-bit texture's rows go from video memory to the image as
 * they are (tex_gpu_decode()); the decoder's textures are made whole */
static int tex_gpu_decode_part(texcache_data *base, const struct texd_plan *plan, const TexRegion *regions, u32 count)
{
	dx12_texture *t = (dx12_texture *)base;
	const int type = (int)base->tex_type;
	const u32 stride = plan->level[0].sr & 0xFFFF;
	u32 i;

	if (!plan->native)
		return !use_decoder || !dx12_texdec_takes(type) ? TEXP_CPU : TEXP_WHOLE;
	if (type == TEX_TYPE_8888 || type == TEX_TYPE_8 || !dx12c_format_supported(type) || !tex_part_ready(t))
		return TEXP_WHOLE;
	to_copy(t->pooled);
	for (i = 0; i < count; i++)
	{
		const TexRegion *r = &regions[i];
		dx12_upload upload;
		u32 pitch;
		if (upload_rows(&vram.data[plan->src + plan->level[0].data + r->y * stride * 2], stride * 2, plan->width * 2, r->h,
				&upload, &pitch))
			copy_to_region(t->pooled, 0, 0, r->y, plan->width, r->h, &upload, pitch);
	}
	to_sampled(t->pooled);
	return TEXP_DONE;
}

static bool tex_force_32bit(const texcache_data *base, int type)
{
	(void)base;
	return !dx12c_format_supported(type);
}

static bool tex_delete(texcache_data *base)
{
	dx12_texture *t = (dx12_texture *)base;
	if (!texcache_delete(base))
		return false;
	tex_release_pooled(t);
	dx12_texture_release_spare(t);
	return true;
}

static void tex_get_id(texcache_data *base, char *s, size_t n)
{
	char id[32];
	sprintf(id, "%p", (void *)base);
	strlcpy(s, id, n);
}

/* ---- The samplers ---- */

void dx12_make_samplers(void)
{
	ID3D12Device *device = dx12c.device;
	const int aniso = settings.rend.AnisotropicFiltering;
	u32 i;
	if (device == NULL)
		return;
	for (i = 0; i < DX12_SAMPLER_TABLE_SIZE; i++)
	{
		/* the bits back from the index: MipMapD, then FilterMode, ClampV, ClampU, FlipV, FlipU */
		const u32 tsp = ((i & 0xF) << 8) | (((i >> 4) & 0x3F) << 13);
		D3D12_SAMPLER_DESC desc;
		memset(&desc, 0, sizeof(desc));
		if (TAW_GET(tsp, TSP_FilterMode) == 0)
			desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		else if (aniso > 1)
			desc.Filter = D3D12_FILTER_ANISOTROPIC;
		else
			/* (the mipmap level nearest, as the Vulkan renderer's: trilinear filtering is two passes) */
			desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		desc.AddressU = TAW_GET(tsp, TSP_ClampU) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
				: TAW_GET(tsp, TSP_FlipU) ? D3D12_TEXTURE_ADDRESS_MODE_MIRROR : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		desc.AddressV = TAW_GET(tsp, TSP_ClampV) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
				: TAW_GET(tsp, TSP_FlipV) ? D3D12_TEXTURE_ADDRESS_MODE_MIRROR : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		desc.MipLODBias = D_Adjust_LoD_Bias[TAW_GET(tsp, TSP_MipMapD)];
		desc.MaxAnisotropy = aniso < 1 ? 1 : aniso > 16 ? 16 : (UINT)aniso;
		desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		desc.MinLOD = 0.f;
		desc.MaxLOD = D3D12_FLOAT32_MAX;
		ID3D12Device_CreateSampler(device, &desc, dx12_heap_cpu(&dx12c.sampler_heap, i));
	}
}

/* ---- Renders to textures, and video memory (rend/rtt_watch.h) ---- */

/* @width by @height from (@x, @y) of @texture (in @state) into @rgba,
 * through the context's readback buffer: what is recorded is submitted,
 * and waited for */
static bool read_back(ID3D12Resource *texture, D3D12_RESOURCE_STATES *state, u32 x, u32 y, u32 width, u32 height, u8 *rgba)
{
	const u32 pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	ID3D12Resource *buffer = dx12c_readback_buffer((u64)pitch * height);
	D3D12_RESOURCE_STATES was;
	D3D12_TEXTURE_COPY_LOCATION dst, src;
	D3D12_BOX box;
	D3D12_RANGE range, none;
	void *p = NULL;
	u64 done;
	u32 l;
	if (buffer == NULL || texture == NULL)
	{
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	was = *state;
	dx12c_transition_resource(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	memset(&dst, 0, sizeof(dst));
	dst.pResource = buffer;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	dst.PlacedFootprint.Footprint.Width = width;
	dst.PlacedFootprint.Footprint.Height = height;
	dst.PlacedFootprint.Footprint.Depth = 1;
	dst.PlacedFootprint.Footprint.RowPitch = pitch;
	memset(&src, 0, sizeof(src));
	src.pResource = texture;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	box.left = x;
	box.top = y;
	box.front = 0;
	box.right = x + width;
	box.bottom = y + height;
	box.back = 1;
	ID3D12GraphicsCommandList_CopyTextureRegion(dx12c_cmd(), &dst, 0, 0, 0, &src, &box);
	dx12c_transition_resource(texture, state, was, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	done = dx12c_submit();
	dx12c_wait_for(done);
	range.Begin = 0;
	range.End = (SIZE_T)pitch * height;
	if (FAILED(ID3D12Resource_Map(buffer, 0, &range, &p)))
	{
		WARN_LOG(RENDERER, "A picture could not be read back from the card\n");
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	/* (a readback heap's: the host's cache is in front of it) */
	for (l = 0; l < height; l++)
		memcpy(rgba + (size_t)l * width * 4, (const u8 *)p + (size_t)l * pitch, (size_t)width * 4);
	none.Begin = 0;
	none.End = 0;
	ID3D12Resource_Unmap(buffer, 0, &none);
	return true;
}

bool dx12_read_picture(ID3D12Resource *texture, D3D12_RESOURCE_STATES *state, u32 width, u32 height, u8 *rgba)
{
	return read_back(texture, state, 0, 0, width, height, rgba);
}

/* Copies of pictures off the card, each into its piece of one readback
 * buffer, a ring, made with the renderer (dx12_rtt_init()) - room for
 * RTT_RING_RENDERS renders of a screen's size - and nothing made in the
 * fault handler: recorded ahead of the read where rtt_watch says which
 * (dx12_rtt_prefetch()), or by the read itself. Each waits for the fence
 * value of the list it was submitted with - passed by the time it is read,
 * as often as not. */
#define RTT_COPIES 18
#define RTT_RING_RENDERS 4

typedef struct Dx12RttCopy
{
	u32 serial;         /* the render's (RttWatch::serial), 0 if none */
	uintptr_t tex;
	u32 w, h, pitch;
	u64 value;          /* the fence value its list signals */
	u64 stamp;          /* when it was last recorded, for the oldest */
	u64 at, len;        /* its piece of the ring */
} Dx12RttCopy;

static Dx12RttCopy rttCopies[RTT_COPIES];
static u64 rttStamp;
static ID3D12Resource *rttRing;
static u64 rttRingSize, rttRingHead;
/* (a ring made larger: the old one kept until the card is past it) */
static ID3D12Resource *rttRingOld;
static u64 rttRingOldValue;
static bool rttSaidLarger;

static u32 rtt_pitch(u32 w)
{
	return (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

/* The ring, at least @bytes: made larger if it is not (every copy gone,
 * once the card has written them) - never in the fault handler */
static bool rtt_ring(u64 bytes)
{
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC desc;
	u32 i;

	if (bytes <= rttRingSize)
		return true;
	if (dx12c.device == NULL)
		return false;
	if (rttRingSize != 0 && !rttSaidLarger)
	{
		rttSaidLarger = true;
		WARN_LOG(RENDERER, "Render to a texture read back: its room made larger, %u KB\n", (u32)(bytes >> 10));
	}
	/* (made larger twice before the card is past the first: waited for, the
	 * once) */
	if (rttRingOld != NULL && dx12c_completed() < rttRingOldValue)
		dx12c_wait_for(rttRingOldValue);
	DX12_RELEASE(rttRingOld);
	for (i = 0; i < RTT_COPIES; i++)
	{
		rttCopies[i].serial = 0;
		rttCopies[i].len = 0;
	}
	if (rttRing != NULL)
	{
		rttRingOld = rttRing;
		rttRing = NULL;
		rttRingOldValue = dx12c.fence_value;
	}
	rttRingSize = rttRingHead = 0;
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = (bytes + 0xffff) & ~(u64)0xffff;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(ID3D12Device_CreateCommittedResource(dx12c.device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, &iid_resource, (void **)&rttRing)))
	{
		WARN_LOG(RENDERER, "DX12: readback buffer creation failed (%u KB)\n", (u32)(bytes / 1024));
		rttRing = NULL;
		return false;
	}
	rttRingSize = desc.Width;
	return true;
}

void dx12_rtt_init(void)
{
	rtt_ring((u64)rtt_pitch(640) * 480 * RTT_RING_RENDERS);
}

static Dx12RttCopy *rtt_copy_of(const RttWatch *watch)
{
	u32 i;
	for (i = 0; i < RTT_COPIES; i++)
		if (rttCopies[i].serial == watch->serial && rttCopies[i].serial != 0 && rttCopies[i].tex == watch->tex)
			return &rttCopies[i];
	return NULL;
}

/* One to record a copy of @len bytes into, with its piece of the ring: the
 * ring's next, from its start where the end is too near; the copies there
 * before go (the card writes the ring in the order the copies are
 * recorded), and the one that held none or was recorded longest ago - not
 * after @since (in the list being recorded) - gives its place. Null if
 * what is there was recorded in this list. */
static Dx12RttCopy *rtt_copy_free(u64 since, u64 len)
{
	Dx12RttCopy *place = NULL;
	u64 at = rttRingHead;
	u32 i;

	if (len > rttRingSize)
		return NULL;
	if (at + len > rttRingSize)
		at = 0;
	for (i = 0; i < RTT_COPIES; i++)
	{
		const Dx12RttCopy *c = &rttCopies[i];
		if (c->len != 0 && c->at < at + len && at < c->at + c->len && c->serial != 0 && c->stamp > since)
			return NULL;
	}
	for (i = 0; i < RTT_COPIES; i++)
	{
		Dx12RttCopy *c = &rttCopies[i];
		if (c->len != 0 && c->at < at + len && at < c->at + c->len)
		{
			c->serial = 0;
			c->len = 0;
		}
		if (c->serial != 0 && c->stamp > since)
			continue;
		if (place == NULL || (place->len != 0 && (c->len == 0 || c->stamp < place->stamp)))
			place = c;
	}
	if (place == NULL)
		return NULL;
	place->at = at;
	place->len = len;
	rttRingHead = at + len;
	return place;
}

/* The copy of the clip's part of @image, w by h at (x0, y0), into c's
 * buffer, recorded */
static bool rtt_record_copy(Dx12RttCopy *c, ID3D12Resource *texture, D3D12_RESOURCE_STATES *state, u32 x0, u32 y0, u32 w, u32 h)
{
	const u32 pitch = rtt_pitch(w);
	D3D12_RESOURCE_STATES was;
	D3D12_TEXTURE_COPY_LOCATION dst, src;
	D3D12_BOX box;

	if (rttRing == NULL || (u64)pitch * h > c->len)
		return false;
	was = *state;
	dx12c_transition_resource(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	memset(&dst, 0, sizeof(dst));
	dst.pResource = rttRing;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Offset = c->at;
	dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	dst.PlacedFootprint.Footprint.Width = w;
	dst.PlacedFootprint.Footprint.Height = h;
	dst.PlacedFootprint.Footprint.Depth = 1;
	dst.PlacedFootprint.Footprint.RowPitch = pitch;
	memset(&src, 0, sizeof(src));
	src.pResource = texture;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	box.left = x0;
	box.top = y0;
	box.front = 0;
	box.right = x0 + w;
	box.bottom = y0 + h;
	box.back = 1;
	ID3D12GraphicsCommandList_CopyTextureRegion(dx12c_cmd(), &dst, 0, 0, 0, &src, &box);
	dx12c_transition_resource(texture, state, was, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	c->w = w;
	c->h = h;
	c->pitch = pitch;
	return true;
}

/* @image's part from (@x, @y), @w by @h of it, drawn onto the picture @rtv
 * at (0, 0), @columns by @rows - filtered where @linear */
static void draw_part(const dx12_pooled_texture *image, u32 rtv, bool linear, u32 x, u32 y, u32 w, u32 h, u32 columns, u32 rows)
{
	D3D12_RECT scissor;
	dx12c_set_targets(rtv, ~0u, 0);
	dx12c_set_viewport((float)columns, (float)rows);
	scissor.left = 0;
	scissor.top = 0;
	scissor.right = (LONG)columns;
	scissor.bottom = (LONG)rows;
	dx12c_set_scissor(&scissor);
	dx12c_draw_quad_at(image->srv, linear, -1.f, -1.f, 2.f, 2.f, (float)x / image->width, (float)y / image->height,
			(float)(x + w) / image->width, (float)(y + h) / image->height, DX12_BLEND_KEY, NULL);
	dx12c_set_targets(~0u, ~0u, 0);
}

/* The picture as the console would have it, w by h, R G B A, copied into
 * c's buffer by commands recorded: from the texture at the clip; or made
 * the console's size on the card first, filtered, where it is finer (the
 * clip's part of the texture drawn onto one of the pool's pictures). */
static bool rtt_record(const RttWatch *watch, Dx12RttCopy *c)
{
	dx12_texture *texture = (dx12_texture *)watch->tex;
	dx12_pooled_texture *image = texture->pooled;
	const u32 w = watch->w, h = watch->h, scale = watch->scale > 1 ? watch->scale : 1;
	const u32 x0 = watch->x0 * scale, y0 = watch->y0 * scale;
	bool done;

	if (dx12c.device == NULL || image == NULL || image->resource == NULL)
		return false;
	if (scale == 1)
		done = rtt_record_copy(c, image->resource, &image->state, x0, y0, w, h);
	else
	{
		u32 pw = 8, ph = 8;
		dx12_pooled_texture *small;
		while (pw < w)
			pw *= 2;
		while (ph < h)
			ph *= 2;
		small = use_pool ? dx12_pool_take_target(pw, ph) : NULL;
		if (small == NULL)
		{
			WARN_LOG(RENDERER, "Render to texture read back: no %ux%u picture\n", pw, ph);
			return false;
		}
		dx12c_transition_resource(small->resource, &small->state, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
		draw_part(image, small->rtv, true, x0, y0, w * scale, h * scale, w, h);
		done = rtt_record_copy(c, small->resource, &small->state, 0, 0, w, h);
		/* (back to the pool: whatever draws on it next comes after the copy
		 * on the queue) */
		dx12_pool_give(small);
	}
	if (!done)
		return false;
	c->serial = watch->serial;
	c->tex = watch->tex;
	c->stamp = ++rttStamp;
	return true;
}

/* Copies of @watches recorded, those that have one already left alone, and
 * submitted together; the fence value they wait for, 0 if none was */
static u64 rtt_copies(const RttWatch *const *watches, unsigned count)
{
	Dx12RttCopy *recorded[RTT_COPIES];
	unsigned n = 0, i;
	const u64 since = rttStamp;
	u64 value;

	for (i = 0; i < count && n < RTT_COPIES; i++)
	{
		/* (pieces start where a copy may: D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) */
		const u64 len = ((u64)rtt_pitch(watches[i]->w) * watches[i]->h + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)
				& ~(u64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
		Dx12RttCopy *c;
		if (rtt_copy_of(watches[i]) != NULL)
			continue;
		c = rtt_copy_free(since, len);
		if (c == NULL)
			break;
		if (rtt_record(watches[i], c))
			recorded[n++] = c;
	}
	if (n == 0)
		return 0;
	value = dx12c_submit();
	for (i = 0; i < n; i++)
		recorded[i]->value = value;
	return value;
}

/* rtt_watch's: begun now, not waited for (and the ring made larger first,
 * here and not in the fault handler, for a render larger than a screen) */
static void dx12_rtt_prefetch(const RttWatch *const *watches, unsigned count)
{
	u64 most = 0;
	unsigned i;

	if (dx12c.device == NULL)
		return;
	for (i = 0; i < count; i++)
	{
		const u64 bytes = (u64)rtt_pitch(watches[i]->w) * watches[i]->h;
		if (bytes > most)
			most = bytes;
	}
	if (most * RTT_RING_RENDERS > rttRingSize)
		rtt_ring(most * RTT_RING_RENDERS);
	rtt_copies(watches, count);
}

/* The picture, from its copy - recorded ahead as often as not, and done by
 * now - or from one recorded here and waited for. */
static void dx12_rtt_read(const RttWatch *watch, u8 *scratch, RttWatchTake take)
{
	Dx12RttCopy *c;
	D3D12_RANGE range;
	void *p = NULL;
	(void)scratch;

	if (dx12c.device == NULL)
	{
		take(watch, NULL, 0);
		return;
	}
	c = rtt_copy_of(watch);
	if (c == NULL)
	{
		if (rtt_copies(&watch, 1) == 0)
		{
			/* every copy is being written: all of them waited for */
			dx12c_wait_for(dx12c.fence_value);
			rtt_copies(&watch, 1);
		}
		c = rtt_copy_of(watch);
		if (c == NULL)
		{
			take(watch, NULL, 0);
			return;
		}
	}
	if (dx12c_completed() < c->value)
		dx12c_wait_for(c->value);
	range.Begin = (SIZE_T)c->at;
	range.End = (SIZE_T)(c->at + (u64)c->pitch * c->h);
	if (FAILED(ID3D12Resource_Map(rttRing, 0, &range, &p)))
	{
		WARN_LOG(RENDERER, "A picture could not be read back from the card\n");
		take(watch, NULL, 0);
	}
	else
	{
		D3D12_RANGE none;
		/* (a readback heap's: the host's cache is in front of it) */
		take(watch, (const u8 *)p + c->at, c->pitch);
		none.Begin = 0;
		none.End = 0;
		ID3D12Resource_Unmap(rttRing, 0, &none);
	}
	c->serial = 0;
}

/* The texture cache's, never this module's to delete. */
static void dx12_rtt_release(uintptr_t tex)
{
	(void)tex;
}

static const RttWatchBackend dx12_rtt_backend = { dx12_rtt_read, dx12_rtt_release, dx12_rtt_prefetch };

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

/* From the texture rendered to: its part drawn onto a picture of the
 * pool's to render to, which is this texture's then - filtered where the
 * render is finer, as it is read back (rtt_record()). In the one command
 * list, after the render and before what draws with this. */
static bool tex_copy_render(texcache_data *base, texcache_data *from, u32 x, u32 y, u32 scale, u32 columns)
{
	dx12_texture *t = (dx12_texture *)base;
	dx12_pooled_texture *image = ((dx12_texture *)from)->pooled;
	dx12_pooled_texture *p;

	if (dx12c.device == NULL || !use_pool || from == base || image == NULL || image->resource == NULL
			|| image->format != DXGI_FORMAT_R8G8B8A8_UNORM || columns == 0 || columns > base->w)
		return false;
	if (scale < 1)
		scale = 1;
	if ((x + columns) * scale > image->width || (y + base->h) * scale > image->height)
		return false;
	p = dx12_pool_take_target(base->w, base->h);
	if (p == NULL)
		p = dx12_pool_make_own(DX12_POOL_TARGET, base->w, base->h, 1);
	if (p == NULL)
		return false;
	tex_cancel_decode(t);
	tex_release_pooled(t);
	t->pooled = p;
	base->tex_type = TEX_TYPE_8888;
	dx12c_transition_resource(p->resource, &p->state, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	if (columns < base->w)
	{
		/* (the rest of each row a narrower stride leaves) */
		float clear[4];
		clear[0] = clear[1] = clear[2] = 0.f;
		clear[3] = texcache_clear_opaque(base) ? 1.f : 0.f;
		ID3D12GraphicsCommandList_ClearRenderTargetView(dx12c_cmd(), dx12_heap_cpu(&dx12c.rtv_heap, p->rtv), clear, 0, NULL);
	}
	draw_part(image, p->rtv, scale > 1, x * scale, y * scale, columns * scale, base->h * scale, columns, base->h);
	to_sampled(p);
	return true;
}

void dx12_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	u32 stride, bytes;

	covered(&addr, x0, y0, &width, height, &stride, &bytes);
	if (bytes != 0)
		rtt_watch_supersede(addr, bytes);
}

void dx12_rtt_watch(dx12_texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height)
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
		rtt_watch_add(&watch, &dx12_rtt_backend);
}

void dx12_rtt_term(void)
{
	u32 i;
	rtt_watch_term();
	/* (the card may still be writing it) */
	if ((rttRing != NULL || rttRingOld != NULL) && dx12c.device != NULL && dx12c_completed() < dx12c.fence_value)
		dx12c_wait_for(dx12c.fence_value);
	DX12_RELEASE(rttRingOld);
	for (i = 0; i < RTT_COPIES; i++)
	{
		rttCopies[i].serial = 0;
		rttCopies[i].value = 0;
		rttCopies[i].len = 0;
	}
	DX12_RELEASE(rttRing);
	rttRingSize = rttRingHead = 0;
	rttSaidLarger = false;
}

/* ---- The cache ---- */

static const texcache_ops dx12_texture_ops = {
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
	dx12_texture *t = (dx12_texture *)e;
	tex_release_pooled(t);
	dx12_texture_release_spare(t);
}

static void tex_cache_ready(void)
{
	if (tex_cache.elem == 0)
		tex_map_init(&tex_cache, sizeof(dx12_texture), NULL, tex_unmake);
}

dx12_texture *dx12_texcache_get(u32 tsp, u32 tcw, int area)
{
	tex_cache_ready();
	return (dx12_texture *)texcache_map_get(&tex_cache, &dx12_texture_ops, tsp, tcw, area);
}

void dx12_texcache_reserve_for(u32 images)
{
	tex_cache_ready();
	texcache_map_reserve_for(&tex_cache, images);
}

void dx12_texcache_cleanup(void)
{
	tex_cache_ready();
	texcache_map_collect(&tex_cache);
}

void dx12_texcache_clear(void)
{
	tex_cache_ready();
	texcache_map_clear(&tex_cache);
}

void dx12_texcache_free(void)
{
	if (tex_cache.elem != 0)
		tex_map_free(&tex_cache);
	memset(&tex_cache, 0, sizeof(tex_cache));
}

void dx12_textures_attach(bool decoder, bool pool_too)
{
	use_decoder = decoder;
	use_pool = pool_too;
}
