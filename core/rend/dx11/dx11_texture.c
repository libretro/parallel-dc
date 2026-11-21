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
#include "dx11_texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <compat/strl.h>
#include "dx11context_lr.h"
#include "dx11_texdecoder.h"
#include "rend/rtt_watch.h"
#include "rend/tex_map.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"

/* The renderer's, while it has them */
static bool use_decoder;
static bool use_pool;

#define MAX_U(a, b) ((a) > (b) ? (a) : (b))

/* ---- The texture pool ---- */

/* by TEX_TYPE_*: 565, 5551, 4444, 8888, 8; and the pictures rendered to */
static const DXGI_FORMAT poolFormats[DX11_POOL_FORMATS] = {
	DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UNORM,
	DXGI_FORMAT_R8G8B8A8_UNORM
};
static const char * const poolFormatNames[DX11_POOL_FORMATS] = { "565", "5551", "4444", "8888", "8-bit", "8888 rendered to" };
static const u32 poolTexel[DX11_POOL_FORMATS] = { 2, 2, 2, 4, 1, 4 };

static struct
{
	tex_pool_book_t book;
	bool ready;
	bool usable[DX11_POOL_FORMATS];
	bool writable[DX11_POOL_FORMATS];
	/* With textures scaled up (xBRZ): the 32-bit ones with levels have
	 * them made on the card (GenerateMips()), as a texture scaled up does */
	bool generates;
	dx11_pooled_texture *all;
} pool;

/* The 16-bit textures turned where they are not to be in place (textures
 * dumped): as large as the largest, made with the pool when textures are
 * dumped (tex_upload_to_gpu()) */
static u16 *turned;
#define TURNED_TEXELS (1024 * 1024 + 1024 * 1024 / 3 + 1)

static void pool_free(dx11_pooled_texture *t)
{
	u32 level;
	for (level = 0; level < TEXD_MAX_LEVELS; level++)
	{
		DX11_RELEASE(t->uavs[level]);
		DX11_RELEASE(t->targets[level]);
	}
	DX11_RELEASE(t->view);
	DX11_RELEASE(t->texture);
	free(t);
}

static dx11_pooled_texture *pool_make(int cls, bool late)
{
	ID3D11Device *device = dx11c.device;
	const u32 f = tex_pool_format(cls);
	const u32 width = tex_pool_width(cls), height = tex_pool_height(cls);
	const bool wide = f == TEX_TYPE_8888 || f == TEX_TYPE_8;
	/* (one whose levels the card makes is filled by copies, not by the decoder) */
	const bool generated = pool.generates && f == TEX_TYPE_8888 && tex_pool_mips(cls);
	dx11_pooled_texture *t;
	D3D11_TEXTURE2D_DESC desc;
	bool ok;
	u32 level;

	if (device == NULL)
		return NULL;
	t = (dx11_pooled_texture *)calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;
	t->levels = tex_pool_mips(cls) ? 1 + bitscanrev(MAX_U(width, height)) : 1;
	t->writable = pool.writable[f] && !generated;

	memset(&desc, 0, sizeof(desc));
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = t->levels;
	desc.ArraySize = 1;
	/* (typeless, for the unsigned integer views the kernels write through) */
	desc.Format = t->writable && wide ? (f == TEX_TYPE_8 ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS) : poolFormats[f];
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (t->writable ? (wide ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET) : 0);
	if (f == DX11_POOL_TARGET)
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	if (generated)
	{
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	}
	ok = SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &t->texture));
	if (!ok)
		t->texture = NULL;
	if (ok)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC view;
		memset(&view, 0, sizeof(view));
		view.Format = poolFormats[f];
		view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		view.Texture2D.MostDetailedMip = 0;
		view.Texture2D.MipLevels = t->levels;
		ok = SUCCEEDED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)t->texture, &view, &t->view));
		if (!ok)
			t->view = NULL;
	}
	if (ok && f == DX11_POOL_TARGET)
	{
		ok = SUCCEEDED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)t->texture, NULL, &t->targets[0]));
		if (!ok)
			t->targets[0] = NULL;
	}
	for (level = 0; ok && t->writable && level < t->levels; level++)
		if (wide)
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav;
			memset(&uav, 0, sizeof(uav));
			uav.Format = f == TEX_TYPE_8 ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R8G8B8A8_UINT;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uav.Texture2D.MipSlice = level;
			ok = SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource *)t->texture, &uav, &t->uavs[level]));
			if (!ok)
				t->uavs[level] = NULL;
		}
		else
		{
			D3D11_RENDER_TARGET_VIEW_DESC rtv;
			memset(&rtv, 0, sizeof(rtv));
			rtv.Format = poolFormats[f];
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			rtv.Texture2D.MipSlice = level;
			ok = SUCCEEDED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)t->texture, &rtv, &t->targets[level]));
			if (!ok)
				t->targets[level] = NULL;
		}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX11: texture pool: a %ux%u %s texture could not be made\n", width, height, poolFormatNames[f]);
		pool_free(t);
		return NULL;
	}
	tex_pool_made(&pool.book, &t->entry, cls, tex_pool_bytes(cls, poolTexel[f]), late, poolFormatNames[f]);
	t->all = pool.all;
	pool.all = t;
	return t;
}

void dx11_pool_init(void)
{
	/* how much games use them, in eighths of what they use 565 */
	static const u32 use[DX11_POOL_FORMATS] = { 8, 6, 6, 4, 2, 0 };
	static const u8 kind[DX11_POOL_FORMATS] = { 0, 0, 0, 0, 0, TEX_POOL_TARGETS };
	static u32 counts[TEX_POOL_CLASSES];
	u32 have[DX11_POOL_FORMATS];
	u64 bytes;
	int f, c;
	u32 i;
	if (pool.ready)
		return;
	tex_pool_reset(&pool.book, 1);
	for (f = 0; f < DX11_POOL_FORMATS; f++)
	{
		pool.usable[f] = f == DX11_POOL_TARGET || dx11c_format_supported(f);
		pool.writable[f] = f != DX11_POOL_TARGET && pool.usable[f] && dx11_texdec_takes(f);
		have[f] = pool.usable[f] ? poolTexel[f] : 0;
	}
	pool.generates = texcache_upscale_factor() > 1 && pool.usable[TEX_TYPE_8888]
			&& dx11c_can_generate_mips(TEX_TYPE_8888);
	bytes = tex_pool_plan(&dx11c.vram_budget, have, use, kind, DX11_POOL_FORMATS, counts,
			texcache_upscale_factor(), (u32)settings.rend.MaxFilteredTextureSize,
			settings.rend.RenderToTextureUpscale > 1 ? (u32)settings.rend.RenderToTextureUpscale : 1u);
	pool.ready = true;
	for (c = 0; c < TEX_POOL_CLASSES; c++)
		for (i = 0; i < counts[c]; i++)
		{
			dx11_pooled_texture *t = pool_make(c, false);
			if (t == NULL)
				break;
			tex_pool_give(&pool.book, &t->entry);
		}
	if (turned == NULL && settings.rend.DumpTextures)
		turned = (u16 *)malloc(TURNED_TEXELS * sizeof(u16));
	NOTICE_LOG(RENDERER, "DX11: texture pool of %u textures, %u MB (planned %u MB)\n", pool.book.made,
			(u32)(pool.book.bytesMade >> 20), (u32)(bytes >> 20));
}

void dx11_pool_term(void)
{
	if (!pool.ready)
		return;
	tex_pool_summary(&pool.book, "DX11");
	while (pool.all != NULL)
	{
		dx11_pooled_texture *next = pool.all->all;
		pool_free(pool.all);
		pool.all = next;
	}
	tex_pool_reset(&pool.book, 1);
	pool.ready = false;
	free(turned);
	turned = NULL;
}

u32 dx11_pool_made(void)
{
	return pool.book.made;
}

static dx11_pooled_texture *pool_take_class(int cls)
{
	TexPoolEntry *e;
	if (cls < 0)
		return NULL;
	e = tex_pool_take(&pool.book, cls);
	/* (the entry first in each) */
	return e != NULL ? (dx11_pooled_texture *)e : pool_make(cls, true);
}

/* A texture of @type, @width by @height, with its levels or not: a free
 * one of that shape, or one made now if there is none. Null if the shape
 * is not one of the pool's. */
static dx11_pooled_texture *pool_take(int type, u32 width, u32 height, bool mips)
{
	const u32 f = (u32)type;
	if (!pool.ready || f >= 5 || !pool.usable[f])
		return NULL;
	return pool_take_class(tex_pool_class(f, width, height, mips));
}

/* A picture to render to, @width by @height; null if the pool has none
 * of that shape */
static dx11_pooled_texture *pool_take_target(u32 width, u32 height)
{
	if (!pool.ready)
		return NULL;
	return pool_take_class(tex_pool_class(DX11_POOL_TARGET, width, height, false));
}

static void pool_give(dx11_pooled_texture *t)
{
	tex_pool_give(&pool.book, &t->entry);
}

/* ---- The cache's textures ---- */

bool dx11_make_texture(u32 width, u32 height, DXGI_FORMAT format, UINT bind,
		ID3D11Texture2D **texture, ID3D11ShaderResourceView **view, const void *data, UINT pitch)
{
	D3D11_TEXTURE2D_DESC desc;
	D3D11_SUBRESOURCE_DATA initial;
	memset(&desc, 0, sizeof(desc));
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind;
	memset(&initial, 0, sizeof(initial));
	initial.pSysMem = data;
	initial.SysMemPitch = pitch;
	DX11_RELEASE(*view);
	DX11_RELEASE(*texture);
	if (FAILED(ID3D11Device_CreateTexture2D(dx11c.device, &desc, data != NULL ? &initial : NULL, texture)))
		*texture = NULL;
	else if (FAILED(ID3D11Device_CreateShaderResourceView(dx11c.device, (ID3D11Resource *)*texture, NULL, view)))
		*view = NULL;
	if (*view == NULL)
	{
		WARN_LOG(RENDERER, "DX11: texture creation failed (%d x %d, format %d)\n", width, height, (int)format);
		DX11_RELEASE(*texture);
		return false;
	}
	return true;
}

static void tex_cancel_decode(dx11_texture *t)
{
	if (t->decode_count != 0 && use_decoder)
		dx11_texdec_cancel(t->decode_job, t->decode_count, t->decode_serial);
	t->decode_count = 0;
}

static void tex_release_pooled(dx11_texture *t)
{
	tex_cancel_decode(t);
	if (t->pooled == NULL)
		return;
	DX11_RELEASE(t->texture_view);
	DX11_RELEASE(t->texture);
	if (use_pool)
		pool_give(t->pooled);
	t->pooled = NULL;
}

/* The pool's texture for this shape, the one it has if it is that;
 * false if the pool has none of this shape */
static bool tex_take_pooled(dx11_texture *t, u32 width, u32 height, bool mips)
{
	dx11_pooled_texture *p;
	if (!use_pool)
		return false;
	if (t->pooled != NULL && (int)t->pooled->entry.cls == tex_pool_class((u32)t->base.tex_type, width, height, mips))
		return true;
	p = pool_take((int)t->base.tex_type, width, height, mips);
	if (p == NULL)
		return false;
	tex_release_pooled(t);
	t->pooled = p;
	DX11_ASSIGN(t->texture, p->texture);
	DX11_ASSIGN(t->texture_view, p->view);
	return true;
}

bool dx11_texture_take_spare(dx11_texture *t, u32 width, u32 height)
{
	dx11_pooled_texture *p = use_pool ? pool_take_target(width, height) : NULL;
	if (p == NULL)
		return false;
	dx11_texture_release_spare(t);
	t->spare_pooled = p;
	DX11_ASSIGN(t->spare, p->texture);
	DX11_ASSIGN(t->spare_view, p->view);
	DX11_ASSIGN(t->spare_target, p->targets[0]);
	return true;
}

void dx11_texture_release_spare(dx11_texture *t)
{
	if (t->spare_pooled == NULL)
		return;
	DX11_RELEASE(t->spare_target);
	DX11_RELEASE(t->spare_view);
	DX11_RELEASE(t->spare);
	if (use_pool)
		pool_give(t->spare_pooled);
	t->spare_pooled = NULL;
}

void dx11_texture_swap_with_spare(dx11_texture *t)
{
	ID3D11Texture2D *texture;
	ID3D11ShaderResourceView *view;
	/* (the texture it had goes back to the pool, if it is the pool's: the spare is that now) */
	tex_release_pooled(t);
	t->pooled = t->spare_pooled;
	t->spare_pooled = NULL;
	texture = t->texture;
	t->texture = t->spare;
	t->spare = texture;
	view = t->texture_view;
	t->texture_view = t->spare_view;
	t->spare_view = view;
	DX11_RELEASE(t->spare_target);
}

/* Made on the GPU (dx11_texdecoder.h), or copied as it is. Everything that
 * can say no does before anything is changed but the texture it is made
 * into, which a no leaves for the CPU's upload to take. */
static bool tex_gpu_decode(texcache_data *base, const struct texd_plan *plan)
{
	dx11_texture *t = (dx11_texture *)base;
	const int type = (int)base->tex_type;
	u32 base_word, d3d16, i;
	u8 *to;
	if (dx11c.device == NULL || !use_pool)
		return false;
	if (plan->native)
	{
		/* A planar 16-bit texture: the PowerVR's texels are Direct3D's
		 * as they are, and its rows go from video memory to the texture */
		if (type == TEX_TYPE_8888 || type == TEX_TYPE_8 || !dx11c_format_supported(type)
				|| !tex_take_pooled(t, plan->width, plan->height, false))
			return false;
		tex_cancel_decode(t);
		ID3D11DeviceContext_UpdateSubresource(dx11c.device_context, (ID3D11Resource *)t->texture, 0, NULL,
				&vram.data[plan->src + plan->level[0].data], (plan->level[0].sr & 0xFFFF) * 2, 0);
		return true;
	}
	if (!use_decoder || !dx11_texdec_takes(type)
			|| !tex_take_pooled(t, plan->width, plan->height, plan->mipmapped != 0) || !t->pooled->writable
			|| t->pooled->levels < plan->levels)
		return false;
	tex_cancel_decode(t);
	to = dx11_texdec_begin(plan->pal + plan->pal_entries * 4, plan->levels, &base_word);
	if (to == NULL)
		return false;
	memcpy(to, &vram.data[plan->src], plan->bytes);
	if (plan->pal_entries != 0)
		memcpy(to + plan->pal, (plan->out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan->pal_index, plan->pal_entries * 4);

	d3d16 = plan->out != TEXD_OUT16 ? 0
			: TEXD_D3D16(type == TEX_TYPE_5551 ? TEXD_D3D_5551 : type == TEX_TYPE_4444 ? TEXD_D3D_4444 : TEXD_D3D_565);
	t->decode_job = dx11_texdec_next();
	t->decode_serial = dx11_texdec_serial();
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
		if (plan->out == TEXD_OUT16)
			dx11_texdec_add(NULL, t->pooled->targets[lv->mip], pc);
		else
			dx11_texdec_add(t->pooled->uavs[lv->mip], NULL, pc);
	}
	t->decode_count = plan->levels;
	return true;
}

/* The 16-bit textures come out of the texture cache as OpenGL has them,
 * red in the top bits and alpha in the bottom one or four. Direct3D's
 * formats with alpha have it in the top bits, and the rest in the same
 * order: a turn of the bits to the right puts them there. */
static const u16 *turnBits(u8 *data, size_t texels, unsigned by, bool inPlace)
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

static void tex_upload_to_gpu(texcache_data *base, int width, int height, u8 *data, bool mipmapped, bool mipmaps_included)
{
	dx11_texture *t = (dx11_texture *)base;
	const int type = (int)base->tex_type;
	ID3D11Device *device = dx11c.device;
	ID3D11DeviceContext *deviceContext = dx11c.device_context;
	DXGI_FORMAT format;
	u32 bpp = 2, levels = 1, i;
	unsigned turn = 0;
	bool generate, fromPool, withData;
	D3D11_TEXTURE2D_DESC desc;
	D3D11_SUBRESOURCE_DATA initial[16];
	int dim;
	if (device == NULL)
		return;
	/* what the decoder was to make of it this frame is this now */
	tex_cancel_decode(t);

	switch (type)
	{
	case TEX_TYPE_5551:
		format = DXGI_FORMAT_B5G5R5A1_UNORM;
		turn = 1;
		break;
	case TEX_TYPE_565:
		format = DXGI_FORMAT_B5G6R5_UNORM;
		break;
	case TEX_TYPE_4444:
		format = DXGI_FORMAT_B4G4R4A4_UNORM;
		turn = 4;
		break;
	case TEX_TYPE_8888:
		format = DXGI_FORMAT_R8G8B8A8_UNORM;
		bpp = 4;
		break;
	case TEX_TYPE_8:
		format = DXGI_FORMAT_R8_UNORM;
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
	generate = mipmapped && !mipmaps_included && dx11c_can_generate_mips(type);
	if (generate)
		for (dim = (width > height ? width : height) / 2; dim != 0; dim /= 2)
			levels++;

	memset(&desc, 0, sizeof(desc));
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = levels;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (generate ? D3D11_BIND_RENDER_TARGET : 0);
	desc.MiscFlags = generate ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

	/* One of the pool's, made before the game (one whose smaller levels the
	 * card makes where the pool's 32-bit ones are made for that) */
	fromPool = generate ? use_pool && pool.generates && type == TEX_TYPE_8888 && tex_take_pooled(t, width, height, true)
			: tex_take_pooled(t, width, height, mipmaps_included);
	if (!fromPool)
		tex_release_pooled(t);
	if (t->texture != NULL && !fromPool)
	{
		/* Kept if it is what the new one would be. (A render to this
		 * texture leaves one that can be rendered to, which is not.) */
		D3D11_TEXTURE2D_DESC cur;
		ID3D11Texture2D_GetDesc(t->texture, &cur);
		if (cur.Width != desc.Width || cur.Height != desc.Height || cur.Format != desc.Format
				|| cur.MipLevels != desc.MipLevels || cur.BindFlags != desc.BindFlags || cur.MiscFlags != desc.MiscFlags)
		{
			DX11_RELEASE(t->texture_view);
			DX11_RELEASE(t->texture);
		}
	}
	if (turn != 0)
	{
		size_t texels = (size_t)width * height;
		if (mipmaps_included)
			for (dim = width / 2; dim != 0; dim /= 2)
				texels += (size_t)dim * dim;
		/* (in the texture cache's own buffer, which nothing reads after
		 * this but a texture dump, which wants it as it is) */
		data = (u8 *)turnBits(data, texels, turn, !settings.rend.DumpTextures);
	}
	/* A texture made anew is made with its pixels, which saves copying
	 * them again after (one whose smaller levels the card makes is made,
	 * and then filled) */
	withData = !generate && levels <= 16;
	if (withData)
	{
		const u8 *p = data;
		for (i = 0; i < levels; i++)
		{
			/* (the levels included are from the smallest, 1x1, to the largest) */
			const u32 level = mipmaps_included ? levels - i - 1 : 0;
			const u32 w = mipmaps_included ? 1u << i : (u32)width;
			const u32 h = mipmaps_included ? 1u << i : (u32)height;
			initial[level].pSysMem = p;
			initial[level].SysMemPitch = w * bpp;
			initial[level].SysMemSlicePitch = w * h * bpp;
			p += (size_t)w * h * bpp;
		}
	}
	if (t->texture == NULL)
	{
		HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, withData ? initial : NULL, &t->texture);
		if (FAILED(hr))
		{
			ERROR_LOG(RENDERER, "Texture creation failed (%x): type %d, %d x %d, %d levels\n", (unsigned)hr, type, width, height, levels);
			t->texture = NULL;
			return;
		}
		hr = ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)t->texture, NULL, &t->texture_view);
		if (FAILED(hr))
		{
			t->texture_view = NULL;
			ERROR_LOG(RENDERER, "Texture view creation failed (%x)\n", (unsigned)hr);
		}
		if (withData)
			return;
	}

	if (mipmaps_included)
	{
		/* from the smallest level, 1x1, to the largest */
		for (i = 0; i < levels; i++)
		{
			const u32 d = 1 << i;
			ID3D11DeviceContext_UpdateSubresource(deviceContext, (ID3D11Resource *)t->texture, levels - i - 1, NULL, data,
					d * bpp, d * d * bpp);
			data += (size_t)d * d * bpp;
		}
	}
	else
	{
		ID3D11DeviceContext_UpdateSubresource(deviceContext, (ID3D11Resource *)t->texture, 0, NULL, data,
				width * bpp, width * height * bpp);
		if (generate && t->texture_view != NULL)
			ID3D11DeviceContext_GenerateMips(deviceContext, t->texture_view);
	}
}

/* The format the texture's image has for texels of @type, and how far
 * the texture cache's 16-bit ones are turned for it (tex_upload_to_gpu()) */
static DXGI_FORMAT typeFormat(int type, u32 *bpp, unsigned *turn)
{
	*bpp = 2;
	*turn = 0;
	switch (type)
	{
	case TEX_TYPE_5551: *turn = 1; return DXGI_FORMAT_B5G5R5A1_UNORM;
	case TEX_TYPE_565: return DXGI_FORMAT_B5G6R5_UNORM;
	case TEX_TYPE_4444: *turn = 4; return DXGI_FORMAT_B4G4R4A4_UNORM;
	case TEX_TYPE_8888: *bpp = 4; return DXGI_FORMAT_R8G8B8A8_UNORM;
	case TEX_TYPE_8: *bpp = 1; return DXGI_FORMAT_R8_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

/* The image the last whole update made, and nothing the decoder still has
 * to do to it */
static bool tex_part_ready(const dx11_texture *t)
{
	const texcache_data *base = &t->base;
	u32 bpp;
	unsigned turn;
	D3D11_TEXTURE2D_DESC desc;

	if (base->made_type != (u8)base->tex_type + 1 || t->texture == NULL || dx11c.device == NULL
			|| (t->decode_count != 0 && use_decoder && t->decode_serial == dx11_texdec_serial()))
		return false;
	ID3D11Texture2D_GetDesc(t->texture, &desc);
	return desc.Width == base->w && desc.Height == base->h && desc.Format == typeFormat((int)base->tex_type, &bpp, &turn)
			&& desc.MipLevels == (base->made_mips ? 1 + bitscanrev(base->w) : 1) && desc.ArraySize == 1;
}

static bool tex_part_memory(texcache_data *base, size_t bytes, void **memory)
{
	(void)bytes;
	/* (the cache's own: UpdateSubresource() copies it) */
	*memory = NULL;
	return tex_part_ready((const dx11_texture *)base);
}

static void tex_upload_part(texcache_data *base, const TexRegion *regions, u32 count, u8 *data, const size_t *offset)
{
	dx11_texture *t = (dx11_texture *)base;
	u32 bpp, i;
	unsigned turn;

	typeFormat((int)base->tex_type, &bpp, &turn);
	for (i = 0; i < count; i++)
	{
		const TexRegion *r = &regions[i];
		D3D11_BOX box;
		box.left = r->x;
		box.top = r->y;
		box.front = 0;
		box.right = r->x + r->w;
		box.bottom = r->y + r->h;
		box.back = 1;
		if (turn != 0)
			turnBits(data + offset[i], (size_t)r->w * r->h, turn, true);
		ID3D11DeviceContext_UpdateSubresource(dx11c.device_context, (ID3D11Resource *)t->texture, r->level, &box,
				data + offset[i], r->w * bpp, 0);
	}
}

/* A planar 16-bit texture's rows go from video memory to the image as
 * they are (tex_gpu_decode()); the decoder's textures are made whole */
static int tex_gpu_decode_part(texcache_data *base, const struct texd_plan *plan, const TexRegion *regions, u32 count)
{
	dx11_texture *t = (dx11_texture *)base;
	const int type = (int)base->tex_type;
	u32 i;

	if (!plan->native)
		return !use_decoder || !dx11_texdec_takes(type) ? TEXP_CPU : TEXP_WHOLE;
	if (type == TEX_TYPE_8888 || type == TEX_TYPE_8 || !dx11c_format_supported(type) || !tex_part_ready(t))
		return TEXP_WHOLE;
	for (i = 0; i < count; i++)
	{
		const TexRegion *r = &regions[i];
		const u32 stride = plan->level[0].sr & 0xFFFF;
		D3D11_BOX box;
		box.left = 0;
		box.top = r->y;
		box.front = 0;
		box.right = plan->width;
		box.bottom = r->y + r->h;
		box.back = 1;
		ID3D11DeviceContext_UpdateSubresource(dx11c.device_context, (ID3D11Resource *)t->texture, 0, &box,
				&vram.data[plan->src + plan->level[0].data + r->y * stride * 2], stride * 2, 0);
	}
	return TEXP_DONE;
}

static bool tex_force_32bit(const texcache_data *base, int type)
{
	(void)base;
	return !dx11c_format_supported(type);
}

static bool tex_delete(texcache_data *base)
{
	dx11_texture *t = (dx11_texture *)base;
	if (!texcache_delete(base))
		return false;
	tex_release_pooled(t);
	dx11_texture_release_spare(t);
	DX11_RELEASE(t->texture_view);
	DX11_RELEASE(t->texture);
	DX11_RELEASE(t->spare_target);
	DX11_RELEASE(t->spare_view);
	DX11_RELEASE(t->spare);
	return true;
}

static void tex_get_id(texcache_data *base, char *s, size_t n)
{
	char id[32];
	sprintf(id, "%p", (void *)base);
	strlcpy(s, id, n);
}

/* ---- Renders to textures, and video memory (rend/rtt_watch.h) ---- */

/* A picture that can be read from this side, kept from one read to the
 * next: a read can come at any time, the middle of a frame included, and
 * what is made for it is used again. And the target a picture made finer
 * than the console's is brought down to the console's size in. */
static ID3D11Texture2D *staging;
static ID3D11Texture2D *small_texture;
static ID3D11RenderTargetView *small_target;

static bool makeStaging(ID3D11Device *device, u32 width, u32 height)
{
	D3D11_TEXTURE2D_DESC desc;
	if (staging != NULL)
	{
		ID3D11Texture2D_GetDesc(staging, &desc);
		if (desc.Width >= width && desc.Height >= height)
			return true;
		width = MAX_U(width, desc.Width);
		height = MAX_U(height, desc.Height);
		DX11_RELEASE(staging);
	}
	memset(&desc, 0, sizeof(desc));
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)))
	{
		WARN_LOG(RENDERER, "Staging texture creation failed (%d x %d)\n", width, height);
		staging = NULL;
		return false;
	}
	return true;
}

/* @width by @height from (@x, @y) of @texture into @rgba, through the
 * staging picture */
static bool readBack(ID3D11Texture2D *texture, u32 x, u32 y, u32 width, u32 height, u8 *rgba)
{
	ID3D11DeviceContext *deviceContext = dx11c.device_context;
	D3D11_BOX box;
	D3D11_MAPPED_SUBRESOURCE mapped;
	u32 l;

	if (!dx11c.immediate || !makeStaging(dx11c.device, width, height))
	{
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	box.left = x;
	box.top = y;
	box.front = 0;
	box.right = x + width;
	box.bottom = y + height;
	box.back = 1;
	ID3D11DeviceContext_CopySubresourceRegion(deviceContext, (ID3D11Resource *)staging, 0, 0, 0, 0,
			(ID3D11Resource *)texture, 0, &box);
	if (FAILED(ID3D11DeviceContext_Map(deviceContext, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)))
	{
		WARN_LOG(RENDERER, "A picture could not be read back from the card\n");
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	for (l = 0; l < height; l++)
		memcpy(rgba + (size_t)l * width * 4, (const u8 *)mapped.pData + (size_t)l * mapped.RowPitch, (size_t)width * 4);
	ID3D11DeviceContext_Unmap(deviceContext, (ID3D11Resource *)staging, 0);
	return true;
}

bool dx11_read_picture(ID3D11Texture2D *texture, u32 width, u32 height, u8 *rgba)
{
	bool ok;
	dx11c_lock();
	ok = readBack(texture, 0, 0, width, height, rgba);
	dx11c_unlock();
	return ok;
}

/* Copies of pictures off the card, each into its rows of one staging
 * picture, a ring - as wide as a screen and RTT_RING_RENDERS screens tall -
 * made with the renderer (dx11_rtt_init()), and nothing made in the fault
 * handler: made ahead of the read where rtt_watch says which
 * (dx11_rtt_prefetch()), or by the read itself, whose Map() waits for the
 * card only if it has not got there yet. */
#define RTT_COPIES 18
#define RTT_RING_RENDERS 4

typedef struct Dx11RttCopy
{
	u32 serial;         /* the render's (RttWatch::serial), 0 if none */
	uintptr_t tex;
	u32 w, h;
	u64 stamp;          /* when it was last made, for the oldest */
	u32 at, rows;       /* its rows of the ring */
} Dx11RttCopy;

static Dx11RttCopy rttCopies[RTT_COPIES];
static u64 rttStamp;
static ID3D11Texture2D *rttRing;
static u32 rttRingW, rttRingH, rttRingHead;
static bool rttSaidLarger;

static Dx11RttCopy *rtt_copy_of(const RttWatch *watch)
{
	u32 i;
	for (i = 0; i < RTT_COPIES; i++)
	{
		Dx11RttCopy *c = &rttCopies[i];
		if (c->serial == watch->serial && c->serial != 0 && c->tex == watch->tex)
			return c;
	}
	return NULL;
}

/* The ring, at least @w wide and @rows tall: made larger if it is not,
 * every copy gone - never in the fault handler */
static bool rtt_ring(u32 w, u32 rows)
{
	ID3D11Device *device = dx11c.device;
	D3D11_TEXTURE2D_DESC desc;
	u32 i;

	if (w <= rttRingW && rows <= rttRingH)
		return true;
	if (device == NULL)
		return false;
	if (rttRing != NULL && !rttSaidLarger)
	{
		rttSaidLarger = true;
		WARN_LOG(RENDERER, "Render to a texture read back: its room made larger, %u x %u\n", w, rows);
	}
	for (i = 0; i < RTT_COPIES; i++)
	{
		rttCopies[i].serial = 0;
		rttCopies[i].rows = 0;
	}
	w = MAX_U(w, MAX_U(rttRingW, 640u));
	rows = MAX_U(rows, rttRingH);
	DX11_RELEASE(rttRing);
	rttRingW = rttRingH = rttRingHead = 0;
	memset(&desc, 0, sizeof(desc));
	desc.Width = w;
	desc.Height = rows;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &rttRing)))
	{
		WARN_LOG(RENDERER, "Staging texture creation failed (%d x %d)\n", w, rows);
		rttRing = NULL;
		return false;
	}
	rttRingW = desc.Width;
	rttRingH = desc.Height;
	return true;
}

/* One to copy @rows into, with its rows of the ring: the ring's next, from
 * its top where the bottom is too near; the copies there before go, and
 * the one that held none or was made longest ago - not after @since (in
 * the same batch) - gives its place. Null if what is there was made in
 * this batch, or the ring is too small. */
static Dx11RttCopy *rtt_copy_free(u64 since, u32 rows)
{
	Dx11RttCopy *place = NULL;
	u32 at = rttRingHead, i;

	if (rows > rttRingH)
		return NULL;
	if (at + rows > rttRingH)
		at = 0;
	for (i = 0; i < RTT_COPIES; i++)
	{
		const Dx11RttCopy *c = &rttCopies[i];
		if (c->rows != 0 && c->at < at + rows && at < c->at + c->rows && c->serial != 0 && c->stamp > since)
			return NULL;
	}
	for (i = 0; i < RTT_COPIES; i++)
	{
		Dx11RttCopy *c = &rttCopies[i];
		if (c->rows != 0 && c->at < at + rows && at < c->at + c->rows)
		{
			c->serial = 0;
			c->rows = 0;
		}
		if (c->serial != 0 && c->stamp > since)
			continue;
		if (place == NULL || (place->rows != 0 && (c->rows == 0 || c->stamp < place->stamp)))
			place = c;
	}
	if (place == NULL)
		return NULL;
	place->at = at;
	place->rows = rows;
	rttRingHead = at + rows;
	return place;
}

/* The target a finer picture is brought down to the console's size in,
 * at least @w by @h: made larger if it is not - never in the fault handler */
static bool rtt_small(u32 w, u32 h)
{
	ID3D11Device *device = dx11c.device;
	D3D11_TEXTURE2D_DESC desc;

	if (small_texture != NULL)
	{
		ID3D11Texture2D_GetDesc(small_texture, &desc);
		if (desc.Width >= w && desc.Height >= h)
			return true;
		DX11_RELEASE(small_target);
		DX11_RELEASE(small_texture);
	}
	if (device == NULL)
		return false;
	memset(&desc, 0, sizeof(desc));
	desc.Width = MAX_U(w, 640u);
	desc.Height = MAX_U(h, 480u);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &small_texture)))
		small_texture = NULL;
	else if (FAILED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)small_texture, NULL, &small_target)))
		small_target = NULL;
	if (small_target == NULL)
	{
		WARN_LOG(RENDERER, "Render to texture read back: target creation failed\n");
		DX11_RELEASE(small_texture);
		return false;
	}
	return true;
}

void dx11_rtt_init(void)
{
	dx11c_lock();
	rtt_ring(640, 480 * RTT_RING_RENDERS);
	if (settings.rend.RenderToTextureUpscale > 1)
		rtt_small(640, 480);
	dx11c_unlock();
}

/* The part of @texture from (x0, y0) on, the console's w by h of it,
 * @scale times finer there, drawn onto the small picture at the console's
 * size, filtered: what the console would have of it, on the card */
static bool shrink(const dx11_texture *texture, u32 x0, u32 y0, u32 w, u32 h, u32 scale)
{
	D3D11_TEXTURE2D_DESC desc, src;
	if (small_texture == NULL)
		return false;
	ID3D11Texture2D_GetDesc(small_texture, &desc);
	if (desc.Width < w || desc.Height < h)
		return false;
	ID3D11Texture2D_GetDesc(texture->texture, &src);
	dx11c_set_targets(small_target, NULL);
	dx11c_set_viewport((float)w, (float)h);
	dx11c_draw_quad_at(texture->texture_view, dx11_sampler_linear(),
			-1.f, -1.f, 2.f, 2.f,
			(float)x0 / src.Width, (float)y0 / src.Height,
			(float)(x0 + w * scale) / src.Width, (float)(y0 + h * scale) / src.Height, NULL, NULL);
	return true;
}

/* The picture as the console would have it, w by h, R G B A, copied into
 * c's staging picture: from the texture at the clip; or made the console's
 * size on the card first, filtered, where it is finer (the clip's part of
 * the texture drawn onto a picture of w by h). Not waited for. */
static bool rtt_copy(const RttWatch *watch, Dx11RttCopy *c)
{
	const dx11_texture *texture = (const dx11_texture *)watch->tex;
	const u32 w = watch->w, h = watch->h, scale = watch->scale > 1 ? watch->scale : 1;
	const u32 x0 = watch->x0 * scale, y0 = watch->y0 * scale;
	ID3D11Texture2D *source = texture->texture;
	u32 sx = x0, sy = y0;
	D3D11_BOX box;

	if (dx11c.device == NULL || !dx11c.immediate || texture->texture == NULL || rttRing == NULL
			|| w > rttRingW || h > c->rows)
		return false;
	if (scale > 1)
	{
		if (!shrink(texture, x0, y0, w, h, scale))
			return false;
		source = small_texture;
		sx = sy = 0;
	}
	box.left = sx;
	box.top = sy;
	box.front = 0;
	box.right = sx + w;
	box.bottom = sy + h;
	box.back = 1;
	ID3D11DeviceContext_CopySubresourceRegion(dx11c.device_context, (ID3D11Resource *)rttRing, 0, 0, c->at, 0,
			(ID3D11Resource *)source, 0, &box);
	c->serial = watch->serial;
	c->tex = watch->tex;
	c->w = w;
	c->h = h;
	c->stamp = ++rttStamp;
	return true;
}

/* rtt_watch's: begun now, together, and sent to the card, not waited for */
static void dx11_rtt_prefetch(const RttWatch *const *watches, unsigned count)
{
	bool any = false;
	u32 mostW = 0, mostH = 0, smallW = 0, smallH = 0;
	const u64 since = rttStamp;
	unsigned i;

	dx11c_lock();
	/* (the ring and the small target made larger here, not in the fault
	 * handler, for a render larger than a screen) */
	for (i = 0; i < count; i++)
	{
		mostW = MAX_U(mostW, watches[i]->w);
		mostH = MAX_U(mostH, watches[i]->h);
		if (watches[i]->scale > 1)
		{
			smallW = MAX_U(smallW, watches[i]->w);
			smallH = MAX_U(smallH, watches[i]->h);
		}
	}
	if (count != 0)
		rtt_ring(mostW, mostH * RTT_RING_RENDERS);
	if (smallW != 0)
		rtt_small(smallW, smallH);
	for (i = 0; i < count; i++)
		if (rtt_copy_of(watches[i]) == NULL)
		{
			Dx11RttCopy *c = rtt_copy_free(since, watches[i]->h);
			if (c != NULL && rtt_copy(watches[i], c))
				any = true;
		}
	if (any)
		ID3D11DeviceContext_Flush(dx11c.device_context);
	dx11c_unlock();
}

/* The picture, from its copy - made ahead as often as not, and done by
 * now - or from one made here, which Map() waits for. */
static void dx11_rtt_read(const RttWatch *watch, u8 *scratch, RttWatchTake take)
{
	ID3D11DeviceContext *deviceContext = dx11c.device_context;
	Dx11RttCopy *c;
	D3D11_MAPPED_SUBRESOURCE mapped;

	(void)scratch;
	dx11c_lock();
	c = rtt_copy_of(watch);
	if (c == NULL)
	{
		c = rtt_copy_free(rttStamp, watch->h);
		if (c == NULL || !rtt_copy(watch, c))
		{
			take(watch, NULL, 0);
			dx11c_unlock();
			return;
		}
	}
	if (FAILED(ID3D11DeviceContext_Map(deviceContext, (ID3D11Resource *)rttRing, 0, D3D11_MAP_READ, 0, &mapped)))
	{
		WARN_LOG(RENDERER, "A picture could not be read back from the card\n");
		take(watch, NULL, 0);
	}
	else
	{
		take(watch, (const u8 *)mapped.pData + (size_t)c->at * mapped.RowPitch, mapped.RowPitch);
		ID3D11DeviceContext_Unmap(deviceContext, (ID3D11Resource *)rttRing, 0);
	}
	c->serial = 0;
	dx11c_unlock();
}

/* The texture cache's, never this module's to delete. */
static void dx11_rtt_release(uintptr_t tex)
{
	(void)tex;
}

static const RttWatchBackend dx11_rtt_backend = { dx11_rtt_read, dx11_rtt_release, dx11_rtt_prefetch };

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

/* From the texture rendered to: a copy of its part, or that part drawn
 * the console's size first where it is finer (shrink()). On the immediate
 * context, in order with the render and what draws with this. */
static bool tex_copy_render(texcache_data *base, texcache_data *from, u32 x, u32 y, u32 scale, u32 columns)
{
	dx11_texture *t = (dx11_texture *)base;
	const dx11_texture *src = (const dx11_texture *)from;
	ID3D11Device *device = dx11c.device;
	ID3D11Texture2D *source;
	D3D11_TEXTURE2D_DESC sd;
	D3D11_BOX box;
	u32 sx = x, sy = y;

	if (device == NULL || src == t || src->texture == NULL || columns == 0 || columns > base->w)
		return false;
	if (scale < 1)
		scale = 1;
	ID3D11Texture2D_GetDesc(src->texture, &sd);
	if (sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM || (x + columns) * scale > sd.Width || (y + base->h) * scale > sd.Height)
		return false;
	tex_cancel_decode(t);
	base->tex_type = TEX_TYPE_8888;
	if (!tex_take_pooled(t, base->w, base->h, false))
	{
		D3D11_TEXTURE2D_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.Width = base->w;
		desc.Height = base->h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		tex_release_pooled(t);
		if (t->texture != NULL)
		{
			D3D11_TEXTURE2D_DESC cur;
			ID3D11Texture2D_GetDesc(t->texture, &cur);
			if (cur.Width != desc.Width || cur.Height != desc.Height || cur.Format != desc.Format
					|| cur.MipLevels != 1 || cur.BindFlags != desc.BindFlags || cur.MiscFlags != 0)
			{
				DX11_RELEASE(t->texture_view);
				DX11_RELEASE(t->texture);
			}
		}
		if (t->texture == NULL)
		{
			if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &t->texture)))
				t->texture = NULL;
			else if (FAILED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)t->texture, NULL, &t->texture_view)))
				t->texture_view = NULL;
			if (t->texture_view == NULL)
			{
				DX11_RELEASE(t->texture);
				return false;
			}
		}
	}
	source = src->texture;
	if (scale > 1)
	{
		if (!shrink(src, x * scale, y * scale, columns, base->h, scale))
			return false;
		source = small_texture;
		sx = sy = 0;
	}
	if (columns < base->w)
	{
		/* (the rest of each row a narrower stride leaves: cleared a row at
		 * a time, from one row of the widest texture) */
		static u32 clear[1024];
		const u32 value = texcache_clear_opaque(base) ? 0xFF000000u : 0u;
		u32 i, row;
		if (clear[0] != value || clear[1023] != value)
			for (i = 0; i < 1024; i++)
				clear[i] = value;
		for (row = 0; row < base->h && base->w <= 1024; row++)
		{
			D3D11_BOX rest;
			rest.left = columns;
			rest.top = row;
			rest.front = 0;
			rest.right = base->w;
			rest.bottom = row + 1;
			rest.back = 1;
			ID3D11DeviceContext_UpdateSubresource(dx11c.device_context, (ID3D11Resource *)t->texture, 0, &rest, clear,
					(base->w - columns) * 4, 0);
		}
	}
	box.left = sx;
	box.top = sy;
	box.front = 0;
	box.right = sx + columns;
	box.bottom = sy + base->h;
	box.back = 1;
	ID3D11DeviceContext_CopySubresourceRegion(dx11c.device_context, (ID3D11Resource *)t->texture, 0, 0, 0, 0,
			(ID3D11Resource *)source, 0, &box);
	return true;
}

void dx11_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	u32 stride, bytes;

	covered(&addr, x0, y0, &width, height, &stride, &bytes);
	if (bytes != 0)
		rtt_watch_supersede(addr, bytes);
}

void dx11_rtt_watch(dx11_texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height)
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
		rtt_watch_add(&watch, &dx11_rtt_backend);
}

void dx11_rtt_term(void)
{
	u32 i;
	rtt_watch_term();
	for (i = 0; i < RTT_COPIES; i++)
	{
		rttCopies[i].serial = 0;
		rttCopies[i].rows = 0;
	}
	DX11_RELEASE(rttRing);
	rttRingW = rttRingH = rttRingHead = 0;
	rttSaidLarger = false;
	DX11_RELEASE(small_target);
	DX11_RELEASE(small_texture);
	DX11_RELEASE(staging);
}

/* ---- The cache ---- */

/* (none of its own: the cache converts into its own memory) */
static void *tex_upload_memory(texcache_data *base, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	(void)base;
	(void)width;
	(void)height;
	(void)bytes;
	(void)pixel;
	(void)mipmaps;
	return NULL;
}

static const texcache_ops dx11_texture_ops = {
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
	dx11_texture *t = (dx11_texture *)e;
	tex_release_pooled(t);
	dx11_texture_release_spare(t);
	DX11_RELEASE(t->spare_target);
	DX11_RELEASE(t->spare_view);
	DX11_RELEASE(t->spare);
	DX11_RELEASE(t->texture_view);
	DX11_RELEASE(t->texture);
}

static void tex_cache_ready(void)
{
	if (tex_cache.elem == 0)
		tex_map_init(&tex_cache, sizeof(dx11_texture), NULL, tex_unmake);
}

dx11_texture *dx11_texcache_get(u32 tsp, u32 tcw, int area)
{
	tex_cache_ready();
	return (dx11_texture *)texcache_map_get(&tex_cache, &dx11_texture_ops, tsp, tcw, area);
}

void dx11_texcache_reserve_for(u32 images)
{
	tex_cache_ready();
	texcache_map_reserve_for(&tex_cache, images);
}

void dx11_texcache_cleanup(void)
{
	tex_cache_ready();
	texcache_map_collect(&tex_cache);
}

void dx11_texcache_clear(void)
{
	tex_cache_ready();
	texcache_map_clear(&tex_cache);
}

void dx11_texcache_free(void)
{
	if (tex_cache.elem != 0)
		tex_map_free(&tex_cache);
	memset(&tex_cache, 0, sizeof(tex_cache));
}

void dx11_textures_attach(bool decoder, bool pool_too)
{
	use_decoder = decoder;
	use_pool = pool_too;
}
