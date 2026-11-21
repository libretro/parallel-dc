/* The texture cache works for the renderer: it reads the registers the
 * frame is rendered with, not the ones the emulation has moved on to. */
#define PVR_REGS_FOR_RENDERER
#include <stdio.h>
#include <string.h>
#include "TexCache.h"
#include "hw/pvr/pvr_mem.h"
#include "texdecode.h"
#include "rtt_watch.h"
#include "vram_lock.h"

/* for a hash state that can be a local: texcache_compute_hash() allocates nothing */
#include <encodings/xxh32.h>

#ifdef HAVE_TEXUPSCALE
#include <features/features_cpu.h>
#include "band_pool.h"
#include "deps/xbrz/xbrz.h"

/* The threads a texture scaled up is shared out to, by bands of rows
 * (band_pool.h): started with the cache's room (texcache_reserve()), NULL
 * for none */
static band_pool_t *xbrz_pool;
#endif

bool KillTex = false;

const f32 D_Adjust_LoD_Bias[16] = {
		0.f, -4.f, -2.f, -1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f
};

/* A texture's words' fields */
#define TCWF(t, f) TAW_GET((t)->tcw, TCW_##f)
#define TSPF(t, f) TAW_GET((t)->tsp, TSP_##f)
#define TCW_SET(t, f, v) ((t)->tcw = TAW_SET((t)->tcw, TCW_##f, (v)))

static void pieces_reserve(u32 textures);
static void *texcache_upscaled(size_t size);

void texcache_scratch_largest(void)
{
	texcache_scratch(tex_buf_bytes(1024, 1024, true, sizeof(u32)) + 1024 * sizeof(u32));
}

void texcache_reserve(u32 textures)
{
	pieces_reserve(textures);
	if (!settings.rend.GpuTextureDecode)
		texcache_scratch_largest();
#ifdef HAVE_TEXUPSCALE
	/* Textures scaled up: what the largest is converted into (not straight
	 * into the renderer's memory) and scaled up into, and the threads it
	 * is shared out to */
	if (settings.rend.TextureUpscale > 1)
	{
		const size_t side = (size_t)settings.rend.MaxFilteredTextureSize;
#if !defined(TARGET_NO_THREADS)
		if (xbrz_pool == NULL)
			xbrz_pool = band_pool_new(band_pool_threads_for(cpu_features_get_core_amount()));
#endif
		texcache_scratch(tex_buf_bytes((u32)side, (u32)side, false, sizeof(u32)) + 1024 * sizeof(u32));
		texcache_upscaled(side * side * settings.rend.TextureUpscale * settings.rend.TextureUpscale * sizeof(u32));
	}
#endif
	vramlock_reserve(textures);
}

#ifdef HAVE_TEXUPSCALE
typedef struct xbrz_job
{
	int factor;
	const u32 *source;
	u32 *dest;
	int width;
	int height;
	bool has_alpha;
} xbrz_job;

static void xbrz_rows(void *ctx, int first, int last)
{
	const xbrz_job *job = (const xbrz_job *)ctx;
	xbrz_scale(job->factor, job->source, job->dest, job->width, job->height,
			job->has_alpha, first, last);
}

void UpscalexBRZ(int factor, u32 *source, u32 *dest, int width, int height, bool has_alpha)
{
	xbrz_job job;
	job.factor = factor;
	job.source = source;
	job.dest = dest;
	job.width = width;
	job.height = height;
	job.has_alpha = has_alpha;
	band_pool_run(xbrz_pool, xbrz_rows, &job, 0, height);
}
#endif

struct PvrTexInfo
{
	const char* name;
	int bpp;        /* 4/8 for pal. 16 for yuv, rgb, argb */
	int type;	/* TEX_TYPE_* */
	/* Conversion to 16 bpp */
	TexConvFP *PL;
	TexConvFP *TW;
	TexConvFP *VQ;
	/* Conversion to 32 bpp */
	TexConvFP32 *PL32;
	TexConvFP32 *TW32;
	TexConvFP32 *VQ32;
	/* Conversion to 8 bpp (palette) */
	TexConvFP8 *TW8;
	/* Planar and compressed, to 32 bpp */
	TexConvFP32 *PLVQ32;
};

static const struct PvrTexInfo format[8] =
{	/* name     bpp Final format   Planar      Twiddled     VQ           Planar(32b)    Twiddled(32b)  VQ (32b)      Palette (8b)  Planar VQ (32b) */
	{"1555",    16, TEX_TYPE_5551, tex1555_PL, tex1555_TW,  tex1555_VQ,  tex1555_PL32,  tex1555_TW32,  tex1555_VQ32, NULL,         tex1555_PLVQ32 },
	{"565",     16, TEX_TYPE_565,  tex565_PL,  tex565_TW,   tex565_VQ,   tex565_PL32,   tex565_TW32,   tex565_VQ32,  NULL,         tex565_PLVQ32 },
	{"4444",    16, TEX_TYPE_4444, tex4444_PL, tex4444_TW,  tex4444_VQ,  tex4444_PL32,  tex4444_TW32,  tex4444_VQ32, NULL,         tex4444_PLVQ32 },
	{"yuv",     16, TEX_TYPE_8888, NULL,       NULL,        NULL,        texYUV422_PL,  texYUV422_TW,  texYUV422_VQ, NULL,         texYUV422_PLVQ },
	{"bumpmap", 16, TEX_TYPE_4444, texBMP_PL,  texBMP_TW,   texBMP_VQ,   tex4444_PL32,  tex4444_TW32,  tex4444_VQ32, NULL,         tex4444_PLVQ32 },
	{"pal4",    4,  TEX_TYPE_5551, NULL,       texPAL4_TW,  texPAL4_VQ,  NULL,          texPAL4_TW32,  texPAL4_VQ32, texPAL4PT_TW, NULL },
	{"pal8",    8,  TEX_TYPE_5551, NULL,       texPAL8_TW,  texPAL8_VQ,  NULL,          texPAL8_TW32,  texPAL8_VQ32, texPAL8PT_TW, NULL },
	{"ns/1555", 0,  TEX_TYPE_565,  NULL,       NULL,        NULL,        NULL,          NULL,          NULL,         NULL,         NULL },	/* not supported (1555) */
};

static const u32 VQMipPoint[11] =
{
	0x00000,/* 1 */
	0x00001,/* 2 */
	0x00002,/* 4 */
	0x00006,/* 8 */
	0x00016,/* 16 */
	0x00056,/* 32 */
	0x00156,/* 64 */
	0x00556,/* 128 */
	0x01556,/* 256 */
	0x05556,/* 512 */
	0x15556 /* 1024 */
};
static const u32 OtherMipPoint[11] =
{
	0x00003,/* 1 */
	0x00004,/* 2 */
	0x00008,/* 4 */
	0x00018,/* 8 */
	0x00058,/* 16 */
	0x00158,/* 32 */
	0x00558,/* 64 */
	0x01558,/* 128 */
	0x05558,/* 256 */
	0x15558,/* 512 */
	0x55558 /* 1024 */
};

static const int PAL_TYPE[4] = {
	TEX_TYPE_5551, TEX_TYPE_565, TEX_TYPE_4444, TEX_TYPE_8888
};

void texcache_print_name(texcache_data *t)
{
	char str[512];
	char id[32];
	sprintf(str, "Texture: %s", texcache_pixel_format_name(t));

	if (TCWF(t, VQ_Comp))
		strcat(str, " VQ");
	else if (TCWF(t, ScanOrder) == 0)
		strcat(str, " TW");
	else if (TCWF(t, StrideSel))
		strcat(str, " Stride");

	if (TCWF(t, ScanOrder) == 0 && TCWF(t, MipMapped))
		strcat(str, " MM");
	if (TSPF(t, FilterMode) != 0)
		strcat(str, " Bilinear");

	sprintf(str + strlen(str), " %dx%d @ 0x%X", 8 << TSPF(t, TexU), 8 << TSPF(t, TexV), TCWF(t, TexAddr) << 3);
	t->ops->get_id(t, id, sizeof(id));
	sprintf(str + strlen(str), " id=%s", id);
	DEBUG_LOG(RENDERER, "%s\n", str);
}

/* The vram content backing this texture, over the same range the
 * write-protection lock would cover, clamped to VRAM like the lock is,
 * hashed piece by piece. For frequently-updated textures that run
 * unprotected. */
/* The pieces' hashes, and their bits, in arenas: blocks of a power of two
 * words, 8 to 1024 (a texture's most), each size's free ones linked
 * through their first words. Made when the renderer starts
 * (texcache_reserve()), more when a size runs out, which is said; one
 * larger than all is the heap's. Render thread only. */
enum { PIECE_CLASSES = 8, PIECE_MIN_WORDS = 8 };
static void *piece_free_list[PIECE_CLASSES];
static u32 piece_blocks[PIECE_CLASSES];
static bool piece_said;

static int piece_class(u32 words)
{
	int c;
	for (c = 0; c < PIECE_CLASSES; c++)
		if (words <= (u32)PIECE_MIN_WORDS << c)
			return c;
	return -1;
}

/* @count more blocks of class @c: one allocation, never given back */
static bool piece_blocks_more(int c, u32 count)
{
	const size_t bytes = ((size_t)PIECE_MIN_WORDS << c) * sizeof(u32);
	u8 *mem = (u8 *)malloc(bytes * count);
	u32 i;
	if (mem == NULL)
		return false;
	for (i = 0; i < count; i++)
	{
		*(void **)(mem + i * bytes) = piece_free_list[c];
		piece_free_list[c] = mem + i * bytes;
	}
	piece_blocks[c] += count;
	return true;
}

static void pieces_reserve(u32 textures)
{
	/* (for one texture in 8 of the smallest, half as many each size up,
	 * no fewer than 8) */
	int c;
	for (c = 0; c < PIECE_CLASSES; c++)
	{
		u32 want = (textures / 8) >> c;
		if (want < 8)
			want = 8;
		if (piece_blocks[c] < want)
			piece_blocks_more(c, want - piece_blocks[c]);
	}
}

static u32 *pieces_alloc(u32 words)
{
	const int c = piece_class(words);
	void *block;
	if (c < 0)
		return (u32 *)malloc((size_t)words * sizeof(u32));
	if (piece_free_list[c] == NULL)
	{
		if (!piece_said)
		{
			piece_said = true;
			WARN_LOG(RENDERER, "Texture cache: no room for the hashes of %u pieces left, more made (too small for this game)\n", words);
		}
		if (!piece_blocks_more(c, piece_blocks[c] > 8 ? piece_blocks[c] / 2 : 8))
			return NULL;
	}
	block = piece_free_list[c];
	piece_free_list[c] = *(void **)block;
	return (u32 *)block;
}

/* @hashes, of @count pieces, given back */
static void pieces_free(u32 *hashes, u32 count)
{
	const int c = piece_class(count + (count + 31) / 32);
	if (hashes == NULL)
		return;
	if (c < 0)
	{
		free(hashes);
		return;
	}
	*(void **)hashes = piece_free_list[c];
	piece_free_list[c] = hashes;
}

/* (its hashes given back) */
static void pieces_drop(texcache_data *t)
{
	pieces_free(t->piece_hash, t->pieces);
	t->piece_hash = NULL;
	t->pieces = 0;
}

bool texcache_hash_pieces(texcache_data *t, u32 *changed)
{
	const u32 start = t->sa_tex & VRAM_MASK;
	u32 end = t->sa + t->size;
	u32 first, count, i, n;
	u32 *bits;
	bool all;

	*changed = 0;
	if (end > VRAM_SIZE)
		end = VRAM_SIZE;
	if (end <= start)
		return true;
	first = start / TEX_HASH_PIECE;
	count = (end - 1) / TEX_HASH_PIECE - first + 1;
	all = t->piece_hash == NULL || count != t->pieces || first != t->piece_first;
	if (all)
	{
		pieces_free(t->piece_hash, t->pieces);
		t->piece_hash = pieces_alloc(count + (count + 31) / 32);
		t->pieces = 0;
		if (t->piece_hash == NULL)
			return false;
		t->piece_first = first;
		t->pieces = count;
	}
	bits = t->piece_hash + count;
	memset(bits, 0, ((count + 31) / 32) * sizeof(u32));
	n = 0;
	for (i = 0; i < count; i++)
	{
		const u32 from = i == 0 ? start : (first + i) * TEX_HASH_PIECE;
		const u32 to = i == count - 1 ? end : (first + i + 1) * TEX_HASH_PIECE;
		const u32 hash = xxh32(&vram.data[from], to - from, 7);
		if (all || hash != t->piece_hash[i])
		{
			t->piece_hash[i] = hash;
			bits[i >> 5] |= 1u << (i & 31);
			n++;
		}
	}
	*changed = n;
	return true;
}

/* The row length a planar texture is read with: the stride register's, or
 * its own width */
static u32 texcache_stride(const texcache_data *t)
{
	u32 stride = t->w;
	if (TCWF(t, StrideSel))
	{
		stride = (TEXT_CONTROL & 31) * 32;
		/* a stride of nothing is no stride: the texture's own width */
		if (stride == 0)
			stride = t->w;
	}
	return stride;
}

bool texcache_needs_update(texcache_data *t)
{
	bool rc;

	/* Take in what the emulation thread has written since the last look. */
	if (vramlock_pending())
		vramlock_collect();

	rc = t->dirty != 0;
	if (t->stride_read != 0 && texcache_stride(t) != t->stride_read)
		rc = true;
	if (t->tex_type != TEX_TYPE_8)
	{
		if (TCWF(t, PixelFmt) == TA_PIXEL_4BPP && t->palette_hash != pal_hash_16[TCWF(t, PalSelect)])
			rc = true;
		else if (TCWF(t, PixelFmt) == TA_PIXEL_8BPP && t->palette_hash != pal_hash_256[TCWF(t, PalSelect) >> 4])
			rc = true;
	}
	/* Frequently-updated texture running unprotected (see texcache_update()):
	 * compare the content hash of the vram range instead of relying on a
	 * write fault. Once a render: its parse asks for a texture for every
	 * polygon that uses it. */
	if (!rc && t->lock_block == NULL && t->Updates >= FREQUENT_UPDATE_COUNT
			&& (t->hash_frame != FrameCount || t->hash_epoch != vram_write_epoch))
		rc = texcache_hash_changed(t);

	return rc;
}

/* A render to a texture still on the graphics card, drawn as a texture of
 * another shape - fewer lines, through a stride, a part of it: made from
 * the picture where it is (copy_render), not fetched into video memory and
 * made from that, which waits for the card. Its rows have to be the
 * render's and all it reads inside what was rendered; in the render's
 * format, which it is drawn the way the render itself is: RGBA8, as fine.
 * It stays watched by write-protection - a hash would fetch the picture -
 * and is made again with the next render there (VramRendered()). */
bool texcache_from_render(texcache_data *t, u32 stride, u32 rows)
{
	RttWatch watch;
	u32 fmt, col, row, columns;
	int was;

	if (!TCWF(t, ScanOrder) || TCWF(t, VQ_Comp) || TCWF(t, MipMapped) || texcache_is_paletted(t) || rows != t->h
			|| (TCWF(t, PixelFmt) != TA_PIXEL_1555 && TCWF(t, PixelFmt) != TA_PIXEL_565 && TCWF(t, PixelFmt) != TA_PIXEL_4444)
			|| settings.rend.TextureUpscale > 1 || settings.rend.DumpTextures || settings.rend.CustomTextures
			|| !rtt_watch_pending(t->sa, t->size, &watch) || watch.target == NULL || watch.target == t)
		return false;
	fmt = watch.packmode == 1 ? TA_PIXEL_565 : watch.packmode == 2 ? TA_PIXEL_4444 : TA_PIXEL_1555;
	if (TCWF(t, PixelFmt) != fmt || stride * 2 != watch.stride || t->sa < watch.addr || (t->sa - watch.addr) % 2 != 0)
		return false;
	col = (t->sa - watch.addr) % watch.stride / 2;
	row = (t->sa - watch.addr) / watch.stride;
	columns = stride < t->w ? stride : t->w;
	if (col + columns > watch.w || row + t->h > watch.h)
		return false;
	was = (int)t->tex_type;
	if (!t->ops->copy_render(t, (texcache_data *)watch.target, watch.x0 + col, watch.y0 + row, watch.scale, columns))
	{
		t->tex_type = (TEXCACHE_TYPE)was;
		return false;
	}
	t->tex_type = TEX_TYPE_8888;
	t->made_type = 0;
	pieces_drop(t);
	t->Updates = 0;
	libCore_vramlock_Lock(t->sa_tex, t->sa + t->size - 1, t);
	return true;
}

/* (texcache_needs_update()'s, kept out of its way) */
bool texcache_hash_changed(texcache_data *t)
{
	u32 changed;

	if (!texcache_hash_pieces(t, &changed))
		return true;
	if (changed != 0)
	{
		/* (until the texcache_update() that follows) */
		t->changed_pieces = true;
		return true;
	}
	t->hash_frame = FrameCount;
	t->hash_epoch = vram_write_epoch;
	return false;
}

void texcache_stop_custom_loads(void)
{
	custom_texture_terminate();
}

bool texcache_delete(texcache_data *t)
{
	if (retro_atomic_load_acquire_int(&t->custom.in_progress) > 0)
		return false;

	if (t->lock_block)
		vramlock_unlock(t->lock_block);
	t->lock_block = NULL;

	pieces_drop(t);
	free(t->custom.image_data);

	return true;
}

void texcache_create(texcache_data *t)
{
	const struct PvrTexInfo *tex;

	/* Reset state info .. */
	t->Updates = 0;
	t->dirty = FrameCount;
	t->stride_read = 0;
	t->lock_block = NULL;
	pieces_drop(t);
	t->made_type = 0;
	t->made_mips = false;
	t->changed_pieces = false;
	custom_request_init(&t->custom);

	/* decode info from tsp/tcw into the texture struct */
	tex = t->tex = &format[TCWF(t, PixelFmt) == TA_PIXEL_RESERVED ? TA_PIXEL_1555 : TCWF(t, PixelFmt)];

	t->sa_tex = (TCWF(t, TexAddr) << 3) & VRAM_MASK;	/* texture start address */
	t->sa = t->sa_tex;					/* data texture start address (modified for MIPs, as needed) */
	t->w = 8 << TSPF(t, TexU);				/* tex width */
	t->h = 8 << TSPF(t, TexV);				/* tex height */

	/* PAL texture */
	if (tex->bpp == 4)
		t->palette_index = TCWF(t, PalSelect) << 4;
	else if (tex->bpp == 8)
		t->palette_index = (TCWF(t, PalSelect) >> 4) << 8;

	t->texconv8 = NULL;

	/* Compressed and mipmapped, a texture is twiddled whatever this bit
	 * says: Star Wars Demolition sets it, and its textures came out
	 * corrupted (upstream: or took the emulator down). Not for a paletted
	 * texture, where the bit is part of the palette selection. */
	if (TCWF(t, VQ_Comp) == 1 && TCWF(t, MipMapped) == 1 && !texcache_is_paletted(t))
		TCW_SET(t, ScanOrder, 0);

	if (TCWF(t, ScanOrder) && (tex->PL || tex->PL32))
	{
		u32 stride;
		/* Texture is stored 'planar' in memory, no deswizzle is needed */
		if (TCWF(t, MipMapped) != 0)
		{
			WARN_LOG(RENDERER, "Warning: planar texture with mipmaps (invalid)\n");
			TCW_SET(t, MipMapped, 0);
		}

		/* Planar textures support stride selection, mostly used for non
		 * power of 2 textures (videos) */
		stride = texcache_stride(t);

		if (TCWF(t, VQ_Comp) != 0 && tex->PLVQ32 != NULL)
		{
			/* Compressed as well. This was taken for a mistake and the
			 * compression ignored; it is a texture like any other (the
			 * Genesis emulator in the Teenage Mutant Ninja Turtles
			 * collection draws its screen with one). */
			t->vq_codebook = t->sa;
			t->texconv = NULL;
			t->texconv32 = tex->PLVQ32;
			t->size = 256 * 4 * 2 + stride * t->h / 4;
		}
		else
		{
			TCW_SET(t, VQ_Comp, 0);
			/* Call the format specific conversion code */
			t->texconv = tex->PL;
			t->texconv32 = tex->PL32;
			/* calculate the size, in bytes, for the locking */
			t->size = stride * t->h * tex->bpp / 8;
		}
	}
	else
	{
		/* These two bits mean nothing for a twiddled texture and are taken
		 * out of the way - unless the texture is paletted. There they are
		 * the top two bits of the palette selection, which says which part
		 * of the palette the texture uses. With them cleared,
		 * texcache_needs_update() looked for changes in the wrong part: a
		 * texture using the upper half of the palette was not decoded
		 * again when its colours changed, and was when some other
		 * texture's did. */
		if (!texcache_is_paletted(t))
		{
			TCW_SET(t, ScanOrder, 0);
			TCW_SET(t, StrideSel, 0);
		}
		/* Quake 3 Arena uses one */
		if (TCWF(t, MipMapped))
			/* Mipmapped texture must be square and TexV is ignored */
			t->h = t->w;

		if (TCWF(t, VQ_Comp))
		{
			verify(tex->VQ != NULL || tex->VQ32 != NULL);
			t->vq_codebook = t->sa;
			if (TCWF(t, MipMapped))
				t->sa += VQMipPoint[TSPF(t, TexU) + 3];
			t->texconv = tex->VQ;
			t->texconv32 = tex->VQ32;
			/* From sa: the codebook the converters skip, and a byte for
			 * every four pixels. This was half the pixels' share and no
			 * codebook, so a write to the rest of the texture was not
			 * watched for, and the texture stayed as it was. */
			t->size = 256 * 4 * 2 + t->w * t->h / 4;
		}
		else
		{
			verify(tex->TW != NULL || tex->TW32 != NULL);
			if (TCWF(t, MipMapped))
				t->sa += OtherMipPoint[TSPF(t, TexU) + 3] * tex->bpp / 8;
			t->texconv = tex->TW;
			t->texconv32 = tex->TW32;
			t->size = t->w * t->h * tex->bpp / 8;
			t->texconv8 = tex->TW8;
		}
	}
}

/* Where a texture is converted before it is handed to the GPU. One piece of
 * memory, kept from one texture to the next and only ever grown: every
 * update used to allocate a buffer the size of its texture and free it
 * again a moment later, sixty megabytes a second of that for a video
 * playing as a 512x512 texture. Whatever is in it is the last texture's and
 * of no use, so growing it copies nothing. Updates happen on the one thread
 * that renders. At most a 1024x1024 texture with its mipmaps, 5.6 MB. */
static void *tex_scratch;
static size_t tex_scratch_size;

void *texcache_scratch(size_t size)
{
	if (size > tex_scratch_size)
	{
		free(tex_scratch);
		tex_scratch = malloc(size);
		tex_scratch_size = tex_scratch != NULL ? size : 0;
	}
	return tex_scratch;
}

/* Where texcache_update() converts a texture into: the renderer's memory,
 * which the upload then takes as it is, or the scratch memory, which it
 * copies. */
void *texcache_convert_into(texcache_data *t, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps, bool direct)
{
	void *memory = direct ? t->ops->upload_memory(t, width, height, bytes, pixel, mipmaps) : NULL;

	return memory != NULL ? memory : texcache_scratch(bytes);
}

/* Where a texture scaled up (xBRZ) is put, and grown as the scratch is:
 * both made as large as they can be wanted when the cache's room is
 * (texcache_reserve()), with textures scaled up. */
static void *tex_upscaled;
static size_t tex_upscaled_size;

static void *texcache_upscaled(size_t size)
{
	if (size > tex_upscaled_size)
	{
		free(tex_upscaled);
		tex_upscaled = malloc(size);
		tex_upscaled_size = tex_upscaled != NULL ? size : 0;
	}
	return tex_upscaled;
}

void texcache_scratch_free(void)
{
	free(tex_scratch);
	tex_scratch = NULL;
	tex_scratch_size = 0;
	free(tex_upscaled);
	tex_upscaled = NULL;
	tex_upscaled_size = 0;
#ifdef HAVE_TEXUPSCALE
	band_pool_free(xbrz_pool);
	xbrz_pool = NULL;
#endif
}

/* Twiddled and compressed; for a paletted texture the scan order bit is
 * part of the palette selection */
static bool texcache_planar(const texcache_data *t)
{
	return TCWF(t, ScanOrder) && (t->tex->PL || t->tex->PL32);
}

void texcache_compute_hash(texcache_data *t)
{
	if (TCWF(t, VQ_Comp) && !texcache_planar(t))
	{
		/* A compressed texture's name used to come from the first w*h/8
		 * bytes at sa - part of the codebook and none or some of the
		 * texture, so that two textures could share one. It is now the
		 * hash of the codebook and then of the whole texture, as upstream
		 * names them; the old name is kept, and custom textures saved
		 * under it are still found. */
		xxh32_state_t state;

		t->old_vqtexture_hash = xxh32(&vram.data[t->sa], t->w * t->h / 8, 7);
		if (texcache_is_paletted(t))
			t->old_vqtexture_hash ^= t->palette_hash;
		t->old_texture_hash = t->old_vqtexture_hash;
		t->old_vqtexture_hash ^= t->tcw & 0xFC000000;

		xxh32_reset(&state, 7);
		xxh32_update(&state, &vram.data[t->sa_tex], 256 * 4 * 2);
		xxh32_update(&state, &vram.data[t->sa + 256 * 4 * 2], t->w * t->h / 4);
		t->texture_hash = xxh32_digest(&state);
		if (texcache_is_paletted(t))
			t->texture_hash ^= t->palette_hash;
		t->texture_hash ^= t->tcw & 0xFC000000;
		return;
	}
	t->old_vqtexture_hash = 0;
	t->texture_hash = xxh32(&vram.data[t->sa], t->size, 7);
	if (texcache_is_paletted(t))
		t->texture_hash ^= t->palette_hash;
	t->old_texture_hash = t->texture_hash;
	t->texture_hash ^= t->tcw & 0xFC000000;	/* everything but texaddr, reserved and stride */
}

bool texcache_plan_decode(texcache_data *t, struct texd_plan *plan, u32 stride, u32 rows, u32 out, bool mipmapped)
{
	const struct PvrTexInfo *tex = t->tex;
	const u32 fmt = (u32)(tex - format);
	const bool planar = texcache_planar(t);
	const u32 scan = planar ? (TCWF(t, VQ_Comp) ? TEXD_PLVQ : TEXD_PL) : (TCWF(t, VQ_Comp) ? TEXD_VQ : TEXD_TW);
	const u32 lw = bitscanrev(t->w), lh = bitscanrev(t->h);
	const u32 first = t->sa - t->sa_tex + (TCWF(t, VQ_Comp) ? 256 * 4 * 2 : 0);
	u32 i;

	if (fmt > TEXD_PAL8 || tex->bpp == 0 || stride > 0xFFFF || rows > 0xFFFF)
		return false;
	/* A twiddled texture that runs past the end of video memory - a
	 * paletted one whose palette selection has the bit planar textures are
	 * cut short by - is read there by the converters, from what follows
	 * (the memory's mirror). Not copied for the GPU: left to them. */
	if (!planar && t->sa + (TCWF(t, VQ_Comp) ? 256 * 4 * 2 + t->w * t->h / 4 : t->w * t->h * tex->bpp / 8) > VRAM_SIZE)
		return false;
	plan->src = t->sa_tex;
	plan->bytes = t->sa + t->size - t->sa_tex;
	plan->pal_entries = texcache_is_paletted(t) && out != TEXD_OUT8 ? (tex->bpp == 4 ? 16 : 256) : 0;
	plan->pal_index = t->palette_index;
	plan->pal = (plan->bytes + 3) & ~3u;
	plan->out = out;
	plan->width = t->w;
	plan->height = t->h;
	plan->mipmapped = mipmapped;
	plan->native = scan == TEXD_PL && out == TEXD_OUT16 && fmt != TEXD_YUV && stride >= t->w && rows == t->h;
	if (!mipmapped)
	{
		struct texd_level *lv = &plan->level[0];
		lv->data = first;
		lv->wh = t->w | (t->h << 16);
		lv->sr = stride | (rows << 16);
		lv->mode = TEXD_MODE(fmt, scan, out, 0, lw, lh);
		lv->mip = 0;
		plan->levels = 1;
		return true;
	}
	/* smallest first, as the converters go; level i is 2^i texels square */
	plan->levels = TSPF(t, TexU) + 3 + 1;
	if (plan->levels > TEXD_MAX_LEVELS)
		return false;
	for (i = 0; i < plan->levels; i++)
	{
		struct texd_level *lv = &plan->level[i];
		u32 f = fmt;
		u32 corner = 0;
		u32 l = i;
		if (TCWF(t, VQ_Comp))
		{
			lv->data = 256 * 4 * 2 + VQMipPoint[i];
			if (i == 0)
			{
				/* the corner of a 2x2 block; of the 2x2 level's for YUV,
				 * whose 1x1 block is not a colour */
				corner = 1;
				l = 1;
				if (fmt == TEXD_YUV)
					lv->data = 256 * 4 * 2 + VQMipPoint[1];
			}
		}
		else
		{
			lv->data = OtherMipPoint[i] * tex->bpp / 8;
			if (fmt == TEXD_YUV && i == 0)
				f = TEXD_565;	/* as texcache_update() does it */
		}
		lv->wh = (1u << i) | ((1u << i) << 16);
		lv->sr = 0;
		lv->mode = TEXD_MODE(f, scan, out, corner, l, l);
		lv->mip = plan->levels - 1 - i;
	}
	return true;
}

/* Where in a twiddled picture 2^@lw by 2^@lh texel @t is: its bits are y's
 * and x's in turn, y's first, as long as both have any, then the rest of
 * the larger one's (detwiddle[][][]) */
static void tw_position(u32 t, u32 lw, u32 lh, u32 *x, u32 *y)
{
	const u32 n = lw < lh ? lw : lh;
	u32 i;
	u32 px = 0, py = 0;

	for (i = 0; i < n; i++)
	{
		py |= ((t >> (2 * i)) & 1) << i;
		px |= ((t >> (2 * i + 1)) & 1) << i;
	}
	if (lw > lh)
		px |= (t >> (2 * n)) << n;
	else
		py |= (t >> (2 * n)) << n;
	*x = px;
	*y = py;
}

/* The regions a texture's changed texels are in, up to TEXP_REGIONS of
 * them; full if there would have to be more */
typedef struct part_list
{
	TexRegion *r;
	u32 count;
	bool full;
} part_list;

/* Texels @x0, @y0 to @x1, @y1 (past the last) of level @level, @t by @t
 * squares of a @width by @height level from a multiple of @t on, into
 * @list: joined to a region of the level they overlap, or that the two
 * fill most of together; one region more otherwise, or where there is no
 * room for one, joined to the region that grows the least. The regions
 * never overlap. */
static void part_add(part_list *list, u32 level, u32 t, u32 width, u32 height, u32 x0, u32 y0, u32 x1, u32 y1)
{
	TexRegion r;
	u32 i;

	r.level = level;
	r.x = x0 & ~(t - 1);
	r.y = y0 & ~(t - 1);
	x1 = (x1 + t - 1) & ~(t - 1);
	y1 = (y1 + t - 1) & ~(t - 1);
	r.w = (x1 < width ? x1 : width) - r.x;
	r.h = (y1 < height ? y1 : height) - r.y;
	for (;;)
	{
		u32 best = list->count;
		u32 grows = 0xFFFFFFFF;
		bool joined = false;

		for (i = 0; i < list->count && !joined; i++)
		{
			const TexRegion *o = &list->r[i];
			const u32 ux0 = o->x < r.x ? o->x : r.x, uy0 = o->y < r.y ? o->y : r.y;
			const u32 ux1 = o->x + o->w > r.x + r.w ? o->x + o->w : r.x + r.w;
			const u32 uy1 = o->y + o->h > r.y + r.h ? o->y + o->h : r.y + r.h;
			const u32 both = o->w * o->h + r.w * r.h;
			const u32 u = (ux1 - ux0) * (uy1 - uy0);
			const bool overlap = o->x < r.x + r.w && r.x < o->x + o->w && o->y < r.y + r.h && r.y < o->y + o->h;

			if (o->level != level)
				continue;
			if (overlap || u <= both + both / 8)
			{
				r.x = ux0;
				r.y = uy0;
				r.w = ux1 - ux0;
				r.h = uy1 - uy0;
				list->r[i] = list->r[--list->count];
				joined = true;
			}
			else if (u - both < grows)
			{
				grows = u - both;
				best = i;
			}
		}
		if (joined)
			continue;	/* (and what it is now may join another) */
		if (list->count < TEXP_REGIONS)
		{
			list->r[list->count++] = r;
			return;
		}
		if (best == list->count)
		{
			list->full = true;
			return;
		}
		{
			const TexRegion *o = &list->r[best];
			const u32 ux0 = o->x < r.x ? o->x : r.x, uy0 = o->y < r.y ? o->y : r.y;
			const u32 ux1 = o->x + o->w > r.x + r.w ? o->x + o->w : r.x + r.w;
			const u32 uy1 = o->y + o->h > r.y + r.h ? o->y + o->h : r.y + r.h;
			r.x = ux0;
			r.y = uy0;
			r.w = ux1 - ux0;
			r.h = uy1 - uy0;
			list->r[best] = list->r[--list->count];
		}
	}
}

/* Texels @t0 to @t1 of a twiddled level 2^@lw by 2^@lh, into @list: as
 * runs of a power of two on a multiple of it, each a rectangle */
static void part_add_twiddled(part_list *list, u32 level, u32 t0, u32 t1, u32 lw, u32 lh)
{
	const u32 n = lw < lh ? lw : lh;
	const u32 t = n < 4 ? 8 : 16;

	while (t0 < t1 && !list->full)
	{
		u32 k = 0;
		u32 x, y, bw, bh;

		while (k < lw + lh && (t0 & ((2u << k) - 1)) == 0 && t0 + (2u << k) <= t1)
			k++;
		tw_position(t0, lw, lh, &x, &y);
		if (k <= 2 * n)
		{
			bw = 1u << (k / 2);
			bh = 1u << ((k + 1) / 2);
		}
		else if (lw > lh)
		{
			bw = 1u << (k - n);
			bh = 1u << n;
		}
		else
		{
			bw = 1u << n;
			bh = 1u << (k - n);
		}
		part_add(list, level, t, 1u << lw, 1u << lh, x, y, x + bw, y + bh);
		t0 += 1u << k;
	}
}

/* Texels a converter makes of a block: 8 bytes of a twiddled texture, an
 * index of a compressed one */
static u32 block_texels(u32 bpp)
{
	return bpp == 4 ? 16 : bpp == 8 ? 8 : 4;
}

u32 texcache_changed_regions(texcache_data *t, TexRegion *regions, u32 stride, u32 rows, bool mipmapped)
{
	const struct PvrTexInfo *tex = t->tex;
	const bool planar = texcache_planar(t);
	const u32 levels = mipmapped ? TSPF(t, TexU) + 3 + 1 : 1;
	const u32 start = t->sa_tex & VRAM_MASK;
	const u32 B = block_texels(tex->bpp);
	const u32 *bits;
	part_list list;
	u32 end = t->sa + t->size;
	u32 i, l, texels, all;

	if (t->piece_hash == NULL || t->pieces == 0 || levels > TEXD_MAX_LEVELS || tex->bpp == 0)
		return 0;
	if (end > VRAM_SIZE)
		end = VRAM_SIZE;
	bits = t->piece_hash + t->pieces;
	list.r = regions;
	list.count = 0;
	list.full = false;

	for (i = 0; i < t->pieces && !list.full; )
	{
		u32 j = i;
		u32 b0, b1;

		if ((bits[i >> 5] & (1u << (i & 31))) == 0)
		{
			i++;
			continue;
		}
		while (j < t->pieces && (bits[j >> 5] & (1u << (j & 31))) != 0)
			j++;
		b0 = (t->piece_first + i) * TEX_HASH_PIECE;
		b1 = (t->piece_first + j) * TEX_HASH_PIECE;
		if (b0 < start)
			b0 = start;
		if (b1 > end)
			b1 = end;
		i = j;
		/* the codebook is all of a compressed texture */
		if (TCWF(t, VQ_Comp) && b0 < t->sa_tex + 256 * 4 * 2)
			return 0;
		if (planar)
		{
			/* rows of the texture's width, read 'stride' texels apart */
			const u32 row = TCWF(t, VQ_Comp) ? stride / 4 : stride * 2;
			const u32 data = TCWF(t, VQ_Comp) ? t->sa + 256 * 4 * 2 : t->sa;
			const u32 data_end = data + row * rows;
			u32 r0 = b0 > data ? b0 - data : 0;
			u32 r1 = (b1 < data_end ? b1 : data_end) - data;

			if (b1 <= data || r0 >= r1)
				continue;
			part_add(&list, 0, 8, t->w, t->h, 0, r0 / row, t->w, (r1 + row - 1) / row);
			continue;
		}
		for (l = 0; l < levels; l++)
		{
			/* the level's data where its converter reads it (a codebook index for each block) */
			const u32 lw = mipmapped ? l : bitscanrev(t->w);
			const u32 lh = mipmapped ? l : bitscanrev(t->h);
			const u32 n = (1u << lw) << lh;
			u32 data, data_end, r0, r1;

			if (TCWF(t, VQ_Comp))
			{
				data = (mipmapped ? t->sa_tex + VQMipPoint[l] : t->sa) + 256 * 4 * 2;
				data_end = data + (n + B - 1) / B;
			}
			else
			{
				data = mipmapped ? t->sa_tex + OtherMipPoint[l] * tex->bpp / 8 : t->sa;
				data_end = data + (n * tex->bpp + 7) / 8;
			}
			if (b1 <= data || b0 >= data_end)
				continue;
			/* the smallest levels (and their corners and special cases)
			 * are made whole with the texture */
			if (lw < 3 || lh < 3)
				return 0;
			r0 = b0 > data ? b0 - data : 0;
			r1 = (b1 < data_end ? b1 : data_end) - data;
			if (TCWF(t, VQ_Comp))
				part_add_twiddled(&list, levels - 1 - l, r0 * B, r1 * B < n ? r1 * B : n, lw, lh);
			else
				part_add_twiddled(&list, levels - 1 - l, r0 / 8 * B, (r1 + 7) / 8 * B < n ? (r1 + 7) / 8 * B : n, lw, lh);
		}
	}
	if (list.full)
		return 0;

	/* half of it or more: made whole */
	texels = 0;
	for (i = 0; i < list.count; i++)
		texels += regions[i].w * regions[i].h;
	all = 0;
	for (l = 0; l < levels; l++)
		all += mipmapped ? 1u << (2 * l) : t->w * t->h;
	if (texels * 2 >= all)
		return 0;
	return list.count;
}

bool tex_plan_part(const struct texd_plan *plan, const TexRegion *regions, u32 count, TexPartPlan *part)
{
	u32 at, i, l;

	if (count > TEXP_REGIONS)
		return false;
	part->codebook = ((plan->level[0].mode >> 4) & 1) != 0 ? 256 * 4 * 2 : 0;
	at = part->codebook;
	for (i = 0; i < count; i++)
	{
		const TexRegion *r = &regions[i];
		const struct texd_level *lv = NULL;
		u32 mode, fmt, scan, lw, lh, first, last;

		for (l = 0; l < plan->levels; l++)
			if (plan->level[l].mip == r->level)
				lv = &plan->level[l];
		if (lv == NULL || r->w == 0 || r->h == 0)
			return false;
		part->level[i] = (u32)(lv - plan->level);
		mode = lv->mode;
		fmt = mode & 15;
		scan = (mode >> 4) & 3;
		lw = (mode >> 12) & 15;
		lh = (mode >> 16) & 15;
		/* the bytes the texels of the region are made from: texd_texel() */
		if (scan == TEXD_PL || scan == TEXD_PLVQ)
		{
			const u32 stride = lv->sr & 0xFFFF;
			const u32 rows = lv->sr >> 16;
			const u32 row = scan == TEXD_PL ? stride * 2 : stride / 4;
			const u32 y1 = r->y + r->h < rows ? r->y + r->h : rows;

			first = lv->data + r->y * row;
			last = r->y < y1 ? lv->data + y1 * row : first;
		}
		else
		{
			const u32 div = fmt == TEXD_PAL4 ? 16 : fmt == TEXD_PAL8 ? 8 : 4;
			/* (the first texel of a box of a twiddled picture is first in it, the last last) */
			const u32 t0 = texd_twiddle(r->x, r->y, lw, lh);
			const u32 t1 = texd_twiddle(r->x + r->w - 1, r->y + r->h - 1, lw, lh);

			if (scan == TEXD_VQ)
			{
				first = lv->data + t0 / div;
				last = lv->data + t1 / div + 1;
			}
			else
			{
				first = lv->data + t0 / div * 8;
				last = lv->data + (t1 / div + 1) * 8;
			}
		}
		first &= ~3u;
		if (last > plan->bytes)
			last = plan->bytes;
		if (last < first)
			last = first;
		part->from[i] = first;
		part->bytes[i] = last - first;
		part->at[i] = at;
		part->data[i] = lv->data + at - first;
		at += (part->bytes[i] + 3) & ~3u;
	}
	part->pal = at;
	part->total = at + plan->pal_entries * 4;
	return true;
}

/* The converter of @out (TEXD_OUT8, 16 or 32) over @width by @height
 * texels from @src, into @dst in rows of @pitch texels */
static void part_convert(const texcache_data *t, u32 out, void *dst, u32 pitch, u8 *src, u32 width, u32 height)
{
	tex_buf_t b;
	memset(&b, 0, sizeof(b));
	tex_buf_borrow(&b, pitch, height, false, dst);
	if (out == TEXD_OUT32)
		t->texconv32(&b, src, width, height);
	else if (out == TEXD_OUT8)
		t->texconv8(&b, src, width, height);
	else
		t->texconv(&b, src, width, height);
}

/* The regions the changed pieces are in made again, the rest of the image
 * left as it is: on the GPU, as a whole update would be made there, or
 * converted here. False if the texture is to be made whole. */
bool texcache_update_part(texcache_data *t, u32 stride, u32 rows, bool mipmapped, bool need_32bit_buffer)
{
	const struct PvrTexInfo *tex = t->tex;
	const bool planar = texcache_planar(t);
	const int before = (int)t->tex_type;
	TexRegion regions[TEXP_REGIONS];
	size_t offset[TEXP_REGIONS];
	size_t bytes = 0;
	int type = (int)t->tex_type;
	struct texd_plan plan;
	void *memory = NULL;
	u32 count, out, pixel, i;

	if (t->made_type == 0 || t->made_mips != mipmapped)
		return false;
	/* the converter a whole update takes, and the texture type it makes */
	if (t->texconv32 != NULL && need_32bit_buffer)
	{
		out = TEXD_OUT32;
		type = TEX_TYPE_8888;
	}
	else if (t->texconv8 != NULL && t->tex_type == TEX_TYPE_8)
		out = TEXD_OUT8;
	else if (t->texconv != NULL)
		out = TEXD_OUT16;
	else
		return false;
	if ((u32)type + 1 != t->made_type)
		return false;
	count = texcache_changed_regions(t, regions, stride, rows, mipmapped);
	if (count == 0)
		return false;

	t->tex_type = (TEXCACHE_TYPE)type;
	if (texcache_plan_decode(t, &plan, stride, rows, out, mipmapped) && (settings.rend.GpuTextureDecode || plan.native))
	{
		const int done = t->ops->gpu_decode_part(t, &plan, regions, count);
		if (done == TEXP_DONE)
			return true;
		if (done == TEXP_WHOLE)
		{
			t->tex_type = (TEXCACHE_TYPE)before;
			return false;
		}
	}

	pixel = out == TEXD_OUT32 ? 4 : out == TEXD_OUT8 ? 1 : 2;
	for (i = 0; i < count; i++)
	{
		offset[i] = bytes;
		bytes += ((size_t)regions[i].w * regions[i].h * pixel + 15) & ~(size_t)15;
	}
	/* (where a stride wider than the texture runs on past the last row) */
	if (planar && stride > t->w)
		bytes += (size_t)(stride - t->w) * pixel;
	if (!t->ops->part_memory(t, bytes, &memory))
	{
		t->tex_type = (TEXCACHE_TYPE)before;
		return false;
	}
	if (memory == NULL)
		memory = texcache_scratch(bytes);

	for (i = 0; i < count; i++)
	{
		const TexRegion *r = &regions[i];
		u8 * const dst = (u8 *)memory + offset[i];

		if (planar)
		{
			const u32 made = r->y >= rows ? 0 : r->y + r->h > rows ? rows - r->y : r->h;
			u32 y;

			if (made != 0)
				part_convert(t, out, dst, t->w, &vram.data[t->sa + r->y * (TCWF(t, VQ_Comp) ? stride / 4 : stride * 2)], stride, made);
			if (made < r->h)
				memset(dst + (size_t)made * t->w * pixel, 0, (size_t)(r->h - made) * t->w * pixel);
			if (stride < t->w)
				for (y = 0; y < made; y++)
					memset(dst + ((size_t)y * t->w + stride) * pixel, 0, (size_t)(t->w - stride) * pixel);
		}
		else
		{
			/* in the squares the texture is laid out in, each one a
			 * twiddled picture of its own from where the square starts */
			const u32 l = mipmapped ? TSPF(t, TexU) + 3 - r->level : 0;
			const u32 lw = mipmapped ? l : bitscanrev(t->w);
			const u32 lh = mipmapped ? l : bitscanrev(t->h);
			const u32 n = lw < lh ? lw : lh;
			const u32 sq = n < 4 ? 8 : 16;
			const u32 B = block_texels(tex->bpp);
			u32 data, x, y;

			if (TCWF(t, VQ_Comp))
				data = mipmapped ? t->sa_tex + VQMipPoint[l] : t->sa;
			else
				data = mipmapped ? t->sa_tex + OtherMipPoint[l] * tex->bpp / 8 : t->sa;
			for (y = r->y; y < r->y + r->h; y += sq)
				for (x = r->x; x < r->x + r->w; x += sq)
				{
					const u32 at = detwiddle[0][lh][x] + detwiddle[1][lw][y];
					part_convert(t, out, dst + ((size_t)(y - r->y) * r->w + (x - r->x)) * pixel, r->w,
							&vram.data[data + (TCWF(t, VQ_Comp) ? at / B : at / B * 8)], sq, sq);
				}
		}
	}
	t->ops->upload_part(t, regions, count, (u8 *)memory, offset);
	return true;
}

/* A mipmapped texture's levels converted with @conv into @b, @texel bytes
 * a texel, from the smallest: the 1x1 level of a compressed one is the
 * corner of a 2x2 block. For a YUV texture that block is not a colour -
 * it came out magenta, behind the pitch in World Series Baseball 2K2 - and
 * the 2x2 level's is used; and a twiddled YUV texture's 1x1 level is
 * converted as 565 (@yuv1: that converter). */
static void convert_mipmaps(texcache_data *t, tex_buf_t *b, tex_conv_fn *conv, size_t texel, tex_conv_fn *yuv1)
{
	u32 i;
	for (i = 0; i <= TSPF(t, TexU) + 3u; i++)
	{
		u32 vram_addr;
		tex_buf_set_mipmap(b, (int)i, texel);
		if (TCWF(t, VQ_Comp))
		{
			vram_addr = t->sa_tex + VQMipPoint[i];
			if (i == 0)
			{
				/* (room for what a paletted texture's converter writes: a
				 * block of 4x4 texels, rows 2 apart) */
				u32 corner[16];
				tex_buf_t b0;
				memset(&b0, 0, sizeof(b0));
				tex_buf_borrow(&b0, 2, 2, false, corner);
				if (yuv1 != NULL && TCWF(t, PixelFmt) == TA_PIXEL_YUV422)
					vram_addr = t->sa_tex + VQMipPoint[1];
				conv(&b0, (u8 *)&vram.data[vram_addr], 2, 2);
				memcpy(TEX_BUF_AT(b, 0, 0, texel), TEX_BUF_AT(&b0, 1, 1, texel), texel);
				continue;
			}
		}
		else
			vram_addr = t->sa_tex + OtherMipPoint[i] * t->tex->bpp / 8;
		if (yuv1 != NULL && TCWF(t, PixelFmt) == TA_PIXEL_YUV422 && i == 0)
			/* Special case for YUV at 1x1 LoD */
			yuv1(b, &vram.data[vram_addr], 1, 1);
		else
			conv(b, &vram.data[vram_addr], 1 << i, 1 << i);
	}
	tex_buf_set_mipmap(b, 0, texel);
}

/* A planar texture's rows not read (@rows of @h) cleared, and the part of
 * each row past a stride narrower than the texture (@clear_x: the stride,
 * or the width for none) */
static void clear_unread(texcache_data *t, tex_buf_t *b, size_t texel, u32 rows, u32 clear_x)
{
	u32 y;
	if (rows < t->h)
		memset((u8 *)TEX_BUF_AT(b, 0, 0, texel) + (size_t)rows * t->w * texel, 0, (size_t)(t->h - rows) * t->w * texel);
	if (clear_x < t->w)
		for (y = 0; y < t->h; y++)
			memset(TEX_BUF_AT(b, clear_x, y, texel), 0, (t->w - clear_x) * texel);
}

void texcache_update(texcache_data *t)
{
	const struct PvrTexInfo *tex = t->tex;
	/* only what texcache_needs_update() just hashed is known to be the rest of it */
	const bool part = t->changed_pieces;
	bool watched = false;
	bool has_alpha = false;
	bool converted = true;
	bool textureUpscaling, need_32bit_buffer, mipmapped, direct, cut_short;
	u32 changed, stride, rows, whole, stride_over;
	void *temp_tex_buffer = NULL;
	u32 upscaled_w = t->w;
	u32 upscaled_h = t->h;
	tex_buf_t pb;

	/* (empty: tex_buf_borrow() gives back what a buffer had) */
	memset(&pb, 0, sizeof(pb));
	t->changed_pieces = false;
	/* texture state tracking stuff */
	t->Updates++;
	t->dirty = 0;

	t->tex_type = (TEXCACHE_TYPE)tex->type;

	if (texcache_is_paletted(t))
	{
		if (tex_gpu_paletted(t->tsp, t->tcw, t->area))
			t->tex_type = TEX_TYPE_8;
		else
		{
			t->tex_type = (TEXCACHE_TYPE)PAL_TYPE[PAL_RAM_CTRL & 3];
			if (t->tex_type != TEX_TYPE_565)
				has_alpha = true;
		}

		/* Get the palette hash to check for future updates */
		if (TCWF(t, PixelFmt) == TA_PIXEL_4BPP)
			t->palette_hash = pal_hash_16[TCWF(t, PalSelect)];
		else
			t->palette_hash = pal_hash_256[TCWF(t, PalSelect) >> 4];
	}

	palette_index = t->palette_index;	/* might be used if pal. tex */
	vq_codebook = &vram.data[t->vq_codebook];	/* might be used if VQ tex */

	/* texture conversion work */
	stride = t->w;

	if (texcache_planar(t))
	{
		u32 bytes;

		stride = texcache_stride(t);
		/* What is watched and hashed is what this stride reads, which the
		 * stride the texture was made or last read with need not be. */
		bytes = TCWF(t, VQ_Comp) ? 256 * 4 * 2 + stride * t->h / 4 : stride * t->h * tex->bpp / 8;
		if (bytes != t->size && t->lock_block != NULL)
		{
			vramlock_unlock(t->lock_block);
			t->lock_block = NULL;
		}
		t->size = bytes;
		t->stride_read = stride;
	}

	/* How many of the texture's lines there are to read. All of them, but
	 * for a texture that runs past the end of video memory: Shenmue's Space
	 * Harrier mini-arcade loads one, and only uses the top of it.
	 *
	 * That was done by cutting the texture's height and size down for the
	 * update and putting the height back after. The size stayed cut - it is
	 * what is locked and hashed, and has to end inside video memory - so the
	 * second update found nothing running past the end, cut nothing, and
	 * read all the lines, the ones outside video memory included. And the
	 * first update converted into a buffer made for the cut height, then
	 * uploaded the whole. (From upstream, done here by what the texture
	 * would take whole rather than by the size as it stands.) */
	rows = t->h;
	cut_short = TCWF(t, ScanOrder) && stride > 0 && !TCWF(t, VQ_Comp) && tex->bpp != 0;
	whole = cut_short ? stride * t->h * tex->bpp / 8 : t->size;
	if (t->sa_tex > VRAM_SIZE || t->size == 0 || t->sa + (whole > t->size ? whole : t->size) > VRAM_SIZE)
	{
		if (t->sa < VRAM_SIZE && cut_short)
		{
			rows = (VRAM_SIZE - t->sa) * 8 / stride / tex->bpp;
			t->size = stride * rows * tex->bpp / 8;
		}
		else
		{
			WARN_LOG(RENDERER, "Warning: invalid texture. Address %08X %08X size %d\n", t->sa_tex, t->sa, t->size);
			return;
		}
	}
	if (texcache_from_render(t, stride, rows))
	{
		texcache_print_name(t);
		return;
	}
	/* (its names worked out here and now: the renderer is updating the
	 * texture, and the game is held while it does) */
	if (settings.rend.CustomTextures && custom_texture_available())
	{
		texcache_compute_hash(t);
		custom_texture_request(&t->custom, t->texture_hash, t->old_vqtexture_hash, t->old_texture_hash);
	}

	/* Watched by its hashes from now on: of what is converted, before it
	 * is (texcache_needs_update()'s, if it has just hashed it) */
	if (t->Updates >= FREQUENT_UPDATE_COUNT)
	{
		watched = part || texcache_hash_pieces(t, &changed);
		t->hash_frame = FrameCount;
		t->hash_epoch = vram_write_epoch;
	}

	/* A planar texture is converted a row of 'stride' pixels at a time into
	 * rows of the texture's width. With a stride wider than the texture,
	 * every row runs on into the next, which is then written over it - and
	 * the last runs past the end of the buffer by the difference: room is
	 * left for that. With a narrower one the rest of each row was whatever
	 * the memory held, and went to the GPU so; it is cleared. */
	stride_over = stride > t->w ? stride - t->w : 0;

	/* Figure out if we really need to use a 32-bit pixel buffer */
	textureUpscaling = settings.rend.TextureUpscale > 1
			/* Don't process textures that are too big */
			&& (int)(t->w * t->h) <= settings.rend.MaxFilteredTextureSize * settings.rend.MaxFilteredTextureSize
			/* Don't process YUV textures */
			&& TCWF(t, PixelFmt) != TA_PIXEL_YUV422;
	need_32bit_buffer = true;
	if (!textureUpscaling
		&& (!texcache_is_paletted(t) || t->tex_type != TEX_TYPE_8888)
		&& t->texconv != NULL
		&& !t->ops->force_32bit(t, (int)t->tex_type))
		need_32bit_buffer = false;

	mipmapped = texcache_is_mipmapped(t) && !settings.rend.DumpTextures;
	/* Straight into the renderer's memory, if it gives any: not a texture
	 * that is scaled up, which is converted and then scaled into memory of
	 * the scaler's own; nor when textures are dumped, which reads them back. */
	direct = !textureUpscaling && !settings.rend.DumpTextures;

	/* Only the pieces that changed made again, where they can be */
	if (part && direct && !settings.rend.CustomTextures && texcache_update_part(t, stride, rows, mipmapped, need_32bit_buffer))
	{
		texcache_print_name(t);
		return;
	}
	t->made_type = 0;

	/* Or made from its bytes on the GPU, where the renderer can: with the
	 * converter, the format and the levels it would have here. Not a
	 * texture scaled up or dumped, which need its texels on this side. A
	 * planar 16-bit texture whose texels the renderer takes as they are
	 * (plan.native) is offered whether the GPU makes textures or not: it
	 * is a copy, which converting it here would only make a slower one. */
	if (direct)
	{
		u32 out = 0xFF;
		int type = (int)t->tex_type;
		struct texd_plan plan;
		if (t->texconv32 != NULL && need_32bit_buffer)
		{
			out = TEXD_OUT32;
			type = TEX_TYPE_8888;
		}
		else if (t->texconv8 != NULL && t->tex_type == TEX_TYPE_8)
			out = TEXD_OUT8;
		else if (t->texconv != NULL)
			out = TEXD_OUT16;
		if (out != 0xFF && texcache_plan_decode(t, &plan, stride, rows, out, mipmapped) && (settings.rend.GpuTextureDecode || plan.native))
		{
			const int before = (int)t->tex_type;
			t->tex_type = (TEXCACHE_TYPE)type;
			t->made_type = (u8)(type + 1);
			t->made_mips = plan.mipmapped != 0;
			if (t->ops->gpu_decode(t, &plan))
			{
				if (!watched)
					libCore_vramlock_Lock(t->sa_tex, t->sa + t->size - 1, t);
				texcache_print_name(t);
				return;
			}
			t->made_type = 0;
			t->tex_type = (TEXCACHE_TYPE)before;
		}
	}

	if (t->texconv32 != NULL && need_32bit_buffer)
	{
		if (textureUpscaling)
			/* don't use mipmaps if upscaling */
			mipmapped = false;
		/* Force the texture type since that's the only 32-bit one we know */
		t->tex_type = TEX_TYPE_8888;

		if (mipmapped)
		{
			tex_buf_borrow(&pb, t->w, t->h, true,
					texcache_convert_into(t, t->w, t->h, tex_buf_bytes(t->w, t->h, true, 4), 4, true, direct));
			convert_mipmaps(t, &pb, t->texconv32, 4, format[TA_PIXEL_565].TW32);
		}
		else
		{
			tex_buf_borrow(&pb, t->w, t->h, false,
					texcache_convert_into(t, t->w, t->h, tex_buf_bytes(t->w, t->h, false, 4) + stride_over * sizeof(u32), 4, false, direct));
			t->texconv32(&pb, (u8 *)&vram.data[t->sa], stride, rows);
			clear_unread(t, &pb, 4, rows, stride);

#ifdef HAVE_TEXUPSCALE
			/* xBRZ scaling */
			if (textureUpscaling)
			{
				const u32 uw = t->w * settings.rend.TextureUpscale, uh = t->h * settings.rend.TextureUpscale;
				tex_buf_t up;
				memset(&up, 0, sizeof(up));
				tex_buf_borrow(&up, uw, uh, false, texcache_upscaled((size_t)uw * uh * sizeof(u32)));

				if (TCWF(t, PixelFmt) == TA_PIXEL_1555 || TCWF(t, PixelFmt) == TA_PIXEL_4444)
					/* Alpha channel formats. Palettes with alpha are already handled */
					has_alpha = true;
				UpscalexBRZ(settings.rend.TextureUpscale, (u32 *)TEX_BUF_AT(&pb, 0, 0, 4),
						(u32 *)TEX_BUF_AT(&up, 0, 0, 4), (int)t->w, (int)t->h, has_alpha);
				pb = up;
				upscaled_w *= settings.rend.TextureUpscale;
				upscaled_h *= settings.rend.TextureUpscale;
			}
#endif
		}
		temp_tex_buffer = TEX_BUF_AT(&pb, 0, 0, 4);
	}
	else if (t->texconv8 != NULL && t->tex_type == TEX_TYPE_8)
	{
		if (mipmapped)
		{
			u32 i;
			tex_buf_borrow(&pb, t->w, t->h, true,
					texcache_convert_into(t, t->w, t->h, tex_buf_bytes(t->w, t->h, true, 1), 1, true, direct));
			for (i = 0; i <= TSPF(t, TexU) + 3u; i++)
			{
				const u32 vram_addr = t->sa_tex + OtherMipPoint[i] * tex->bpp / 8;
				tex_buf_set_mipmap(&pb, (int)i, 1);
				t->texconv8(&pb, &vram.data[vram_addr], 1 << i, 1 << i);
			}
			tex_buf_set_mipmap(&pb, 0, 1);
		}
		else
		{
			tex_buf_borrow(&pb, t->w, t->h, false,
					texcache_convert_into(t, t->w, t->h, tex_buf_bytes(t->w, t->h, false, 1), 1, false, direct));
			t->texconv8(&pb, &vram.data[t->sa], stride, rows);
			/* (the rows not read; a narrower stride's rest is the converter's) */
			clear_unread(t, &pb, 1, rows, t->w);
		}
		temp_tex_buffer = TEX_BUF_AT(&pb, 0, 0, 1);
	}
	else if (t->texconv != NULL)
	{
		if (mipmapped)
		{
			tex_buf_borrow(&pb, t->w, t->h, true,
					texcache_convert_into(t, t->w, t->h, tex_buf_bytes(t->w, t->h, true, 2), 2, true, direct));
			convert_mipmaps(t, &pb, t->texconv, 2, NULL);
		}
		else
		{
			tex_buf_borrow(&pb, t->w, t->h, false,
					texcache_convert_into(t, t->w, t->h, tex_buf_bytes(t->w, t->h, false, 2) + stride_over * sizeof(u16), 2, false, direct));
			t->texconv(&pb, (u8 *)&vram.data[t->sa], stride, rows);
			clear_unread(t, &pb, 2, rows, stride);
		}
		temp_tex_buffer = TEX_BUF_AT(&pb, 0, 0, 2);
	}
	else
	{
		/* fill it in with a temp color */
		WARN_LOG(RENDERER, "UNHANDLED TEXTURE\n");
		converted = false;
		tex_buf_borrow(&pb, t->w, t->h, false, texcache_scratch(tex_buf_bytes(t->w, t->h, false, 2)));
		memset(TEX_BUF_AT(&pb, 0, 0, 2), 0x80, t->w * t->h * 2);
		temp_tex_buffer = TEX_BUF_AT(&pb, 0, 0, 2);
		mipmapped = false;
	}
	/* lock the texture to detect changes in it, unless it is frequently
	 * updated: each update of a protected texture costs a SIGSEGV (the write
	 * fault), the invalidation, a full re-upload, and a re-protect --
	 * measured as 1-2 faults per frame sustained, with matching frame-time
	 * jitter, on titles that rewrite textures every frame (e.g. VF3tb). Past
	 * the update threshold, leave the pages writable and detect changes with
	 * a content hash at lookup time instead (see texcache_needs_update()): a
	 * hash over the same range is cheaper than the fault path, and when the
	 * game rewrites identical content it avoids the re-upload entirely. */
	if (!watched)
		libCore_vramlock_Lock(t->sa_tex, t->sa + t->size - 1, t);

	/* (an image a part of is made again: not scaled up, not grey) */
	if (direct && converted)
	{
		t->made_type = (u8)((int)t->tex_type + 1);
		t->made_mips = mipmapped;
	}
	t->ops->upload_to_gpu(t, (int)upscaled_w, (int)upscaled_h, (u8 *)temp_tex_buffer, texcache_is_mipmapped(t), mipmapped);
	if (settings.rend.DumpTextures)
	{
		enum custom_image_texels texels = CUSTOM_IMAGE_8888;
		bool known = true;
		texcache_compute_hash(t);
		switch ((int)t->tex_type)
		{
		case TEX_TYPE_4444: texels = CUSTOM_IMAGE_4444; break;
		case TEX_TYPE_565:  texels = CUSTOM_IMAGE_565;  break;
		case TEX_TYPE_5551: texels = CUSTOM_IMAGE_5551; break;
		case TEX_TYPE_8888: texels = CUSTOM_IMAGE_8888; break;
		default:
			WARN_LOG(RENDERER, "dumpTexture: unsupported picture format %x\n", (u32)t->tex_type);
			known = false;
			break;
		}
		if (known)
			custom_texture_dump(t->texture_hash, upscaled_w, upscaled_h, texels, temp_tex_buffer);
		NOTICE_LOG(RENDERER, "Dumped texture %x.png. Old hash %x\n", t->texture_hash, t->old_texture_hash);
	}
	texcache_print_name(t);
}

void texcache_check_custom(texcache_data *t)
{
	if (texcache_custom_available(t))
	{
		t->made_type = 0;
		t->tex_type = TEX_TYPE_8888;
		t->ops->upload_to_gpu(t, (int)t->custom.width, (int)t->custom.height, t->custom.image_data, texcache_is_mipmapped(t), false);
		free(t->custom.image_data);
		t->custom.image_data = NULL;
	}
}

/* Where the framebuffer is, its format and size, as the registers have it */
void ReadFramebufferGeometry(FramebufferRead *fb)
{
	int width = (FB_R_SIZE.fb_x_size + 1) << 1;     /* in 16-bit words */
	int height = FB_R_SIZE.fb_y_size + 1;
	int modulus = (FB_R_SIZE.fb_modulus - 1) << 1;
	int bpp;
	u32 addr;

	switch (FB_R_CTRL.fb_depth)
	{
		case FBDE_0555:
		case FBDE_565:
			bpp = 2;
			break;
		case FBDE_888:
			bpp = 3;
			width = (width * 2) / 3;		/* in pixels */
			modulus = (modulus * 2) / 3;	/* in pixels */
			break;
		case FBDE_C888:
			bpp = 4;
			width /= 2;             /* in pixels */
			modulus /= 2;           /* in pixels */
			break;
		default:
			die("Invalid framebuffer format\n");
			bpp = 4;
			break;
	}

	addr = FB_R_SOF1;
	if (SPG_CONTROL.interlace)
	{
		if (width == modulus && FB_R_SOF2 == FB_R_SOF1 + width * bpp)
		{
			/* Typical case alternating even and odd lines -> take the whole buffer at once */
			modulus = 0;
			height *= 2;
		}
		else
		{
			addr = SPG_STATUS.fieldnum ? FB_R_SOF2 : FB_R_SOF1;
		}
	}
	else if (FB_R_CTRL.vclk_div == 0)
	{
		/* Not interlaced, on a television: 240 lines are all there are,
		 * whatever the size register says. Upstream: the Echelon intro of
		 * Confidential Mission (PAL). */
		if (height > 240)
			height = 240;
	}

	fb->addr = addr;
	fb->width = width;
	fb->height = height;
	fb->skip = modulus * bpp;
	fb->depth = FB_R_CTRL.fb_depth;
	fb->concat = FB_R_CTRL.fb_concat;
}

/* The framebuffer's words, read where pvr_read_area1() would, without a
 * call each (a few hundred thousand of them a frame) */
static INLINE u32 fb_read16(u32 addr)
{
	return *(const u16 *)&vram.data[pvr_map32(addr) & ~1u];
}
static INLINE u32 fb_read32(u32 addr)
{
	return *(const u32 *)&vram.data[pvr_map32(addr) & ~3u];
}

#define FB_RGBX(r, g, b) ((u32)(u8)(r) | ((u32)(u8)(g) << 8) | ((u32)(u8)(b) << 16) | 0xFF000000u)

/* The picture as RGBA8, rows @pitch bytes apart from @dst on: each texel
 * stored once, whole and in order, so @dst may be memory the card reads
 * (write-combined, never read back) */
void ReadFramebufferTo(const FramebufferRead *fb, u8 *dst, size_t pitch)
{
	const int width = fb->width;
	const int height = fb->height;
	const u32 skip = fb->skip;
	const u32 c = fb->concat;
	u32 addr = fb->addr;
	int x;
	int y;

	switch (fb->depth)
	{
		case FBDE_0555:    /* 555 RGB */
			for (y = 0; y < height; y++, dst += pitch)
			{
				u32 *out = (u32 *)dst;
				for (x = 0; x < width; x++, addr += 2)
				{
					u32 src = fb_read16(addr);
					out[x] = FB_RGBX((((src >> 10) & 0x1F) << 3) + c,
							(((src >> 5) & 0x1F) << 3) + c,
							((src & 0x1F) << 3) + c);
				}
				addr += skip;
			}
			break;

		case FBDE_565:    /* 565 RGB */
			for (y = 0; y < height; y++, dst += pitch)
			{
				u32 *out = (u32 *)dst;
				for (x = 0; x < width; x++, addr += 2)
				{
					u32 src = fb_read16(addr);
					out[x] = FB_RGBX((((src >> 11) & 0x1F) << 3) + c,
							(((src >> 5) & 0x3F) << 2) + (c & 3),
							((src & 0x1F) << 3) + c);
				}
				addr += skip;
			}
			break;

		case FBDE_888:		/* 888 RGB: four pixels in three words */
			for (y = 0; y < height; y++, dst += pitch)
			{
				u32 *out = (u32 *)dst;
				for (x = 0; x < width; x += 4)
				{
					u32 src = fb_read32(addr);
					u32 src2;
					u32 src3;
					addr += 4;
					*out++ = FB_RGBX(src >> 16, src >> 8, src);
					if (x + 1 >= width)
						break;
					src2 = fb_read32(addr);
					addr += 4;
					*out++ = FB_RGBX(src2 >> 8, src2, src >> 24);
					if (x + 2 >= width)
						break;
					src3 = fb_read32(addr);
					addr += 4;
					*out++ = FB_RGBX(src3, src2 >> 24, src2 >> 16);
					if (x + 3 >= width)
						break;
					*out++ = FB_RGBX(src3 >> 24, src3 >> 16, src3 >> 8);
				}
				addr += skip;
			}
			break;

		case FBDE_C888:     /* 0888 RGB */
			for (y = 0; y < height; y++, dst += pitch)
			{
				u32 *out = (u32 *)dst;
				for (x = 0; x < width; x++, addr += 4)
				{
					u32 src = fb_read32(addr);
					out[x] = FB_RGBX(src >> 16, src >> 8, src);
				}
				addr += skip;
			}
			break;
	}
}

#undef FB_RGBX

/* A texture's lock dropped by a write over what it was made from: it is
 * made again when next used (vram_lock.c gives the lock back after) */
void vramlock_dropped(void *owner)
{
	texcache_data *tcd = (texcache_data *)owner;
	tcd->dirty = FrameCount;
	tcd->lock_block = NULL;
}

void libCore_vramlock_Lock(u32 start_offset64, u32 end_offset64, texcache_data *texture)
{
	vramlock_lock(start_offset64, end_offset64, texture, &texture->lock_block);
}

/* ---- A renderer's textures by their key ---- */

/* (of the TSP, TexU and TexV alone; of the TCW, all but the stride select) */
#define TSP_KEY_MASK (TAW_MASK(TSP_TexU) | TAW_MASK(TSP_TexV))
#define TCW_KEY_MASK (TAW_MASK(TCW_TexAddr) | TAW_MASK(TCW_ScanOrder) | TAW_MASK(TCW_PixelFmt) \
		| TAW_MASK(TCW_VQ_Comp) | TAW_MASK(TCW_MipMapped))

u64 texcache_key(u32 tsp, u32 tcw, int area)
{
	const u32 fmt = TAW_GET(tcw, TCW_PixelFmt);
	u64 key = tsp & TSP_KEY_MASK;

	if ((fmt == TA_PIXEL_4BPP || fmt == TA_PIXEL_8BPP) && !tex_gpu_paletted(tsp, tcw, area))
		/* A paletted texture's palette selection is part of its key, and
		 * the palette's type, so that a texture whose palette's type comes
		 * back is still there; bit 9: made colours here, which a texture
		 * of the same address left as indices is not */
		key |= ((u64)tcw << 32) | ((PAL_RAM_CTRL & 3) << 6) | ((u64)(TAW_GET(tsp, TSP_FilterMode) != 0) << 8) | (1 << 9);
	else
		key |= (u64)(tcw & TCW_KEY_MASK) << 32;
	return key;
}

void texcache_map_reserve(tex_map_t *m, u32 textures)
{
	tex_map_reserve(m, textures);
	texcache_reserve(textures);
}

void texcache_map_reserve_for(tex_map_t *m, u32 images)
{
	texcache_map_reserve(m, images * 2 > 1024 ? images * 2 : 1024);
}

texcache_data *texcache_map_get(tex_map_t *m, const texcache_ops *ops, u32 tsp, u32 tcw, int area)
{
	const u64 key = texcache_key(tsp, tcw, area);
	texcache_data *t = (texcache_data *)tex_map_find(m, key);

	if (t != NULL)
	{
		/* (needed if the texture is updated) */
		t->tcw = TAW_SET(t->tcw, TCW_StrideSel, TAW_GET(tcw, TCW_StrideSel));
		return t;
	}
	t = (texcache_data *)tex_map_add(m, key);
	if (t == NULL)
		die("Out of memory");
	if (ops != NULL)
		t->ops = ops;
	t->tsp = tsp;
	t->tcw = tcw;
	t->area = (u8)area;
	return t;
}

void texcache_map_collect(tex_map_t *m)
{
	/* (a few a frame, those not used for 120 frames) */
	const u32 target = (FrameCount > 120 ? FrameCount : 120) - 120;
	u64 list[6];
	u32 n = 0, e, i;
	u64 key;

	for (e = 0; e < tex_map_places(m) && n < 6; e++)
	{
		const texcache_data *t = (const texcache_data *)tex_map_at(m, e, &key);
		if (t != NULL && t->dirty && t->dirty < target)
			list[n++] = key;
	}
	for (i = 0; i < n; i++)
	{
		texcache_data *t = (texcache_data *)tex_map_find(m, list[i]);
		if (t->ops->del(t))
			tex_map_remove(m, list[i]);
	}
}

void texcache_map_clear(tex_map_t *m)
{
	u32 e;
	u64 key;

	/* A texture whose custom image is still being loaded is the loader's as
	 * well as the cache's: the loader has a pointer to it and writes to it
	 * when it is done, and a delete refuses it. If there is one, the
	 * loader is stopped first, which leaves it with none; it starts again
	 * with the next texture that asks. (A count at zero is a texture the
	 * loader has finished with or never had: only the renderer raises it
	 * from zero, and bringing it back there is the last thing the loader
	 * does to a texture.) */
	for (e = 0; e < tex_map_places(m); e++)
	{
		texcache_data *t = (texcache_data *)tex_map_at(m, e, &key);
		if (t != NULL && retro_atomic_load_acquire_int(&t->custom.in_progress) > 0)
		{
			texcache_stop_custom_loads();
			break;
		}
	}
	for (e = 0; e < tex_map_places(m); e++)
	{
		texcache_data *t = (texcache_data *)tex_map_at(m, e, &key);
		if (t != NULL)
			t->ops->del(t);
	}
	tex_map_clear(m);
	KillTex = false;
	INFO_LOG(RENDERER, "Texture cache cleared\n");
}
