/* The texture converters, against upstream's.
 *
 * Every converter the texture cache has - each pixel format, twiddled,
 * compressed, planar, paletted, to 16 and to 32 bits - is given the same
 * bytes at five sizes, and a hash and the first four pixels of what comes
 * out are printed, one line each. texconv_expected.txt is what upstream
 * flycast's converters printed for the same bytes (the planar ones to 16
 * bits this tree's own); texconv_test.sh compares.
 *
 * The converters are pure functions of their input, so this is also what a
 * rewrite of them has to keep printing. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rend/TexCache.h"

static u8 *buf;
static u32 rnd_state = 0x12345678;
static u32 rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5; return rnd_state; }

/* @fn on the bytes, into a picture of @pixel bytes a texel */
static void run(const char *name, tex_conv_fn *fn, u32 w, u32 h, size_t pixel)
{
	tex_buf_t pb;
	const u8 *d;
	u32 hash = 2166136261u;
	size_t i;
	int k;

	memset(&pb, 0, sizeof(pb));
	tex_buf_alloc(&pb, w, h, pixel);
	memset(TEX_BUF_AT(&pb, 0, 0, pixel), 0xEE, (size_t)w * h * pixel);
	/* (the VQ converters are handed the codebook in front of the indices) */
	fn(&pb, buf + 2048, w, h);
	d = (const u8 *)TEX_BUF_AT(&pb, 0, 0, pixel);
	for (i = 0; i < (size_t)w * h * pixel; i++) hash = (hash ^ d[i]) * 16777619u;
	printf("%-12s %4ux%-4u %08x  first", name, w, h, hash);
	for (k = 0; k < 4; k++) { u32 v = 0; memcpy(&v, d + k * pixel, pixel); printf(" %0*x", (int)pixel * 2, v); }
	printf("\n");
	tex_buf_free(&pb);
}

/* "cache": the texture cache's checks for change, against a full upload.
 *
 * Textures over the same few pages are asked for as a render's parse asks:
 * many times a render, with video memory written between renders as the
 * SH4 writes it (faulting on a protected page) and, now and then, in the
 * middle of one by the renderer (VramLockedWriteRange()). After every
 * request the texture's last upload has to be what a new texture made then
 * from video memory uploads. Then one texture, written no more, is asked
 * for 300 times a render: it may be hashed once a render. */
#include "hw/pvr/pvr_regs.h"
#include "rend/texdecode.h"
#include "rend/rtt_watch.h"
#include "hw/mem/_vmem.h"

extern VArray2 vram;
#include <encodings/xxh32.h>
uint32_t texconv_xxh32(const void *data, size_t len, uint32_t seed);
void texconv_xxh32_reset(xxh32_state_t *s, uint32_t seed);
void texconv_xxh32_update(xxh32_state_t *s, const void *data, size_t len);
uint32_t texconv_xxh32_digest(const xxh32_state_t *s);

settings_t settings;
u32 FrameCount = 1;
unsigned VRAM_SIZE = 8 << 20;
unsigned VRAM_MASK = (8 << 20) - 1;
static u8 cache_regs[pvr_RegSize];
u8 *rend_pvr_regs = cache_regs;
static u8 cache_protected[(16 << 20) / PAGE_SIZE];
static size_t cache_hashed;

uint32_t xxh32(const void *data, size_t len, uint32_t seed)
{
	cache_hashed += len;
	return texconv_xxh32(data, len, seed);
}
void xxh32_reset(xxh32_state_t *s, uint32_t seed)
{
	texconv_xxh32_reset(s, seed);
}
void xxh32_update(xxh32_state_t *s, const void *data, size_t len)
{
	texconv_xxh32_update(s, data, len);
}
uint32_t xxh32_digest(const xxh32_state_t *s)
{
	return texconv_xxh32_digest(s);
}
void _vmem_protect_vram(u32 addr, u32 size)
{
	u32 page;
	for (page = addr / PAGE_SIZE; page <= (addr + size - 1) / PAGE_SIZE; page++)
		cache_protected[page] = 1;
}
void _vmem_unprotect_vram(u32 addr, u32 size)
{
	u32 page;
	for (page = addr / PAGE_SIZE; page <= (addr + size - 1) / PAGE_SIZE; page++)
		cache_protected[page] = 0;
}
bool _vmem_vram_page_protected(u32 page) { return cache_protected[page] != 0; }
/* (no log) */
retro_log_printf_t log_cb;
/* (no render to a texture is watched here: rend/rtt_watch.h) */
bool rtt_watch_pending(u32 offset, u32 size, RttWatch *watch) { (void)offset; (void)size; (void)watch; return false; }
void rtt_watch_overwrite(u32 offset, u32 size) { (void)offset; (void)size; }

/* How the test's textures are made: on the GPU (the kernel run here,
 * rend/texdecode.h, as the renderers run it there), and with parts of them
 * made again; and what that took */
static bool cache_gpu;
static bool cache_parts;
static bool cache_force32;
static u32 cache_part_count;
static u32 cache_whole_count;
static size_t cache_part_bytes;

/* Regions that are not what a renderer is to be handed: off their level,
 * not on multiples of 8, or overlapping (a copy's must not) */
static u32 cache_bad_regions;

static void part_check(const texcache_data *t, bool mips, const TexRegion *r, u32 count)
{
	const u32 top = bitscanrev(t->w);
	u32 i, j;

	if (count == 0 || count > TEXP_REGIONS)
		cache_bad_regions++;
	for (i = 0; i < count; i++)
	{
		const u32 lw = mips ? 1u << (top - r[i].level) : t->w, lh = mips ? lw : t->h;
		if ((r[i].x | r[i].y | r[i].w | r[i].h) & 7 || r[i].w == 0 || r[i].h == 0
				|| r[i].x + r[i].w > lw || r[i].y + r[i].h > lh || (!mips && r[i].level != 0))
			cache_bad_regions++;
		for (j = 0; j < i; j++)
			if (r[j].level == r[i].level && r[j].x < r[i].x + r[i].w && r[i].x < r[j].x + r[j].w
					&& r[j].y < r[i].y + r[i].h && r[i].y < r[j].y + r[j].h)
				cache_bad_regions++;
	}
}

/* A texture whose upload is kept, as the GPU would have it: its levels
 * from the smallest, as the cache hands them over */
typedef struct cache_tex
{
	texcache_data base;
	u8 *gpu;
	size_t gpu_bytes;
	bool made;
} cache_tex;

static size_t tex_pixel(int type)
{
	return type == TEX_TYPE_8888 ? 4 : type == TEX_TYPE_8 ? 1 : 2;
}

/* Where level @level of the image (0 the largest) is kept, and its width */
static size_t level_at(const cache_tex *t, u32 level, u32 *width)
{
	const size_t pixel = tex_pixel(t->base.tex_type);
	size_t at = 0;
	u32 l, top;

	if (!t->base.made_mips)
	{
		*width = t->base.w;
		return 0;
	}
	top = bitscanrev(t->base.w);
	for (l = 0; l < top - level; l++)
		at += ((size_t)1 << (2 * l)) * pixel;
	*width = 1u << (top - level);
	return at;
}

static void ct_get_id(texcache_data *t, char *s, size_t n) { (void)t; if (n != 0) s[0] = '\0'; }
static void ct_upload_to_gpu(texcache_data *base, int width, int height, u8 *data, bool mipmapped, bool mipmaps_included)
{
	cache_tex *t = (cache_tex *)base;
	const size_t pixel = tex_pixel(base->tex_type);
	int d;
	(void)mipmapped;
	t->gpu_bytes = (size_t)width * height * pixel;
	if (mipmaps_included)
		for (d = width / 2; d != 0; d /= 2)
			t->gpu_bytes += (size_t)d * d * pixel;
	t->gpu = (u8 *)realloc(t->gpu, t->gpu_bytes);
	memcpy(t->gpu, data, t->gpu_bytes);
	cache_whole_count++;
}
static void *ct_upload_memory(texcache_data *t, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	(void)t; (void)width; (void)height; (void)bytes; (void)pixel; (void)mipmaps;
	return NULL;
}
static bool ct_force_32bit(const texcache_data *t, int type) { (void)t; (void)type; return cache_force32; }
static bool ct_part_memory(texcache_data *base, size_t bytes, void **memory)
{
	(void)bytes;
	*memory = NULL;
	return cache_parts && ((cache_tex *)base)->gpu != NULL;
}
static void ct_upload_part(texcache_data *base, const TexRegion *regions, u32 count, u8 *data, const size_t *offset)
{
	cache_tex *t = (cache_tex *)base;
	const size_t pixel = tex_pixel(base->tex_type);
	u32 i, y;

	part_check(base, base->made_mips, regions, count);
	for (i = 0; i < count; i++)
	{
		u32 width;
		const size_t at = level_at(t, regions[i].level, &width);
		for (y = 0; y < regions[i].h; y++)
			memcpy(t->gpu + at + ((size_t)(regions[i].y + y) * width + regions[i].x) * pixel,
					data + offset[i] + (size_t)y * regions[i].w * pixel, regions[i].w * pixel);
		cache_part_bytes += (size_t)regions[i].w * regions[i].h * pixel;
	}
	cache_part_count++;
}
static bool ct_gpu_decode(texcache_data *base, const struct texd_plan *plan)
{
	cache_tex *t = (cache_tex *)base;
	const size_t pixel = tex_pixel(base->tex_type);
	u8 *bytes;
	u32 i, d;

	if (!cache_gpu)
		return false;
	bytes = (u8 *)calloc(plan->pal + plan->pal_entries * 4 + 4, 1);
	memcpy(bytes, &vram.data[plan->src], plan->bytes);
	if (plan->pal_entries != 0)
		memcpy(bytes + plan->pal, (plan->out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan->pal_index, plan->pal_entries * 4);
	t->gpu_bytes = (size_t)plan->width * plan->height * pixel;
	if (plan->mipmapped)
		for (d = plan->width / 2; d != 0; d /= 2)
			t->gpu_bytes += (size_t)d * d * pixel;
	t->gpu = (u8 *)realloc(t->gpu, t->gpu_bytes);
	for (i = 0; i < plan->levels; i++)
	{
		u32 width;
		const size_t at = level_at(t, plan->level[i].mip, &width);
		texd_c_level(bytes, plan, &plan->level[i], t->gpu + at, width);
	}
	free(bytes);
	cache_whole_count++;
	return true;
}
static int ct_gpu_decode_part(texcache_data *base, const struct texd_plan *plan, const TexRegion *regions, u32 count)
{
	cache_tex *t = (cache_tex *)base;
	const size_t pixel = tex_pixel(base->tex_type);
	TexPartPlan part;
	u8 *ring;
	u32 i, x, y;

	if (!cache_gpu)
		return TEXP_CPU;
	if (!cache_parts || t->gpu == NULL || !tex_plan_part(plan, regions, count, &part))
		return TEXP_WHOLE;
	part_check(base, base->made_mips, regions, count);
	/* the copy as a renderer makes it, and the kernel over each region */
	ring = (u8 *)calloc(part.total + 4, 1);
	memcpy(ring, &vram.data[plan->src], part.codebook);
	for (i = 0; i < count; i++)
		memcpy(ring + part.at[i], &vram.data[plan->src + part.from[i]], part.bytes[i]);
	if (plan->pal_entries != 0)
		memcpy(ring + part.pal, (plan->out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan->pal_index, plan->pal_entries * 4);
	texd_c_set(ring);
	for (i = 0; i < count; i++)
	{
		const struct texd_level *lv = &plan->level[part.level[i]];
		u32 width;
		const size_t at = level_at(t, regions[i].level, &width);
		for (y = regions[i].y; y < regions[i].y + regions[i].h; y++)
			for (x = regions[i].x; x < regions[i].x + regions[i].w; x++)
			{
				const u32 v = texd_texel(part.data[i], part.pal, lv->sr, lv->mode, x, y);
				memcpy(t->gpu + at + ((size_t)y * width + x) * pixel, &v, pixel);
			}
		cache_part_bytes += (size_t)regions[i].w * regions[i].h * pixel;
	}
	free(ring);
	cache_part_count++;
	return TEXP_DONE;
}
static bool ct_copy_render(texcache_data *t, texcache_data *from, u32 x, u32 y, u32 scale, u32 columns)
{
	(void)t; (void)from; (void)x; (void)y; (void)scale; (void)columns;
	return false;
}
static bool ct_del(texcache_data *t) { return texcache_delete(t); }

static const texcache_ops ct_ops =
{
	ct_get_id, ct_upload_to_gpu, ct_upload_memory, ct_gpu_decode, ct_part_memory, ct_upload_part,
	ct_gpu_decode_part, ct_copy_render, ct_force_32bit, ct_del
};

/* The renderer's cache of them */
static void cache_init(tex_map_t *m)
{
	tex_map_init(m, sizeof(cache_tex), NULL, NULL);
	m->name = "Texture cache";
}

static void cache_free(tex_map_t *m)
{
	u32 e;
	u64 key;
	for (e = 0; e < tex_map_places(m); e++)
	{
		cache_tex *t = (cache_tex *)tex_map_at(m, e, &key);
		if (t != NULL)
		{
			free(t->gpu);
			texcache_delete(&t->base);
		}
	}
	tex_map_free(m);
}

static cache_tex *cache_lookup(tex_map_t *m, u32 tsp, u32 tcw)
{
	return (cache_tex *)texcache_map_get(m, &ct_ops, tsp, tcw, 0);
}

/* A texture made now, its own */
static void fresh_init(cache_tex *t)
{
	memset(t, 0, sizeof(*t));
	t->base.ops = &ct_ops;
}

/* address, format, width and height (8 << n), planar, compressed */
static const u32 cache_textures[][6] = {
	{ 0x10000, TA_PIXEL_565,  3, 3, 0, 0 },
	{ 0x10000, TA_PIXEL_1555, 3, 3, 0, 0 },
	{ 0x12000, TA_PIXEL_4444, 2, 2, 0, 0 },
	{ 0x12800, TA_PIXEL_565,  2, 1, 1, 0 },
	{ 0x13000, TA_PIXEL_565,  4, 2, 0, 0 },
	{ 0x14000, TA_PIXEL_565,  3, 3, 0, 1 },
};
#define CACHE_TEXTURES (sizeof(cache_textures) / sizeof(cache_textures[0]))

static void cache_key(u32 i, u32 *tsp, u32 *tcw)
{
	*tsp = 0;
	*tcw = 0;
	*tsp = TAW_SET(*tsp, TSP_TexU, cache_textures[i][2]);
	*tsp = TAW_SET(*tsp, TSP_TexV, cache_textures[i][3]);
	*tcw = TAW_SET(*tcw, TCW_TexAddr, cache_textures[i][0] >> 3);
	*tcw = TAW_SET(*tcw, TCW_PixelFmt, cache_textures[i][1]);
	*tcw = TAW_SET(*tcw, TCW_ScanOrder, cache_textures[i][4]);
	*tcw = TAW_SET(*tcw, TCW_VQ_Comp, cache_textures[i][5]);
}

/* What a texture made now uploads, against what @t has uploaded */
static bool cache_same_as_new(const cache_tex *t, u32 i)
{
	cache_tex fresh;
	bool same;

	fresh_init(&fresh);
	cache_key(i, &fresh.base.tsp, &fresh.base.tcw);
	texcache_create(&fresh.base);
	/* (unprotected, as a texture updated often is: it leaves no lock) */
	fresh.base.Updates = FREQUENT_UPDATE_COUNT;
	texcache_update(&fresh.base);
	same = fresh.gpu != NULL && t->gpu_bytes == fresh.gpu_bytes && !memcmp(t->gpu, fresh.gpu, fresh.gpu_bytes);
	free(fresh.gpu);
	texcache_delete(&fresh.base);
	return same;
}

static cache_tex *cache_get(tex_map_t *cache, u32 i)
{
	u32 tsp, tcw;
	cache_tex *t;

	cache_key(i, &tsp, &tcw);
	t = cache_lookup(cache, tsp, tcw);
	if (!t->made)
	{
		t->gpu = NULL;
		t->made = true;
		texcache_create(&t->base);
	}
	if (texcache_needs_update(&t->base))
		texcache_update(&t->base);
	return t;
}

/* The SH4's write: a protected page faults first */
static void cache_write(u32 addr, u32 bytes, bool same)
{
	u32 page;
	u32 i;

	for (page = addr / PAGE_SIZE; page <= (addr + bytes - 1) / PAGE_SIZE; page++)
		if (cache_protected[page])
			VramLockedWriteOffset(page * PAGE_SIZE);
	if (!same)
		for (i = 0; i < bytes; i++)
			vram.data[addr + i] = (u8)rnd();
}

static int cache_test(void)
{
	tex_map_t cache;
	cache_tex *t = NULL;
	u32 frame;
	u32 i;
	u32 checks = 0;
	size_t still;

	cache_init(&cache);
	vram.data = (u8 *)malloc(8 << 20);
	vram.size = 8 << 20;
	for (i = 0; i < (8u << 20); i++)
		vram.data[i] = (u8)rnd();
	settings.rend.TextureUpscale = 1;
	settings.rend.MaxFilteredTextureSize = 256;

	for (frame = 0; frame < 400; frame++)
	{
		u32 n;
		u32 lookup;

		FrameCount++;
		for (n = rnd() % 4; n != 0; n--)
			cache_write(0x10000 + (rnd() % 0x8000 & ~1u), 2 + (rnd() % 4096 & ~1u), (rnd() & 1) != 0);
		for (lookup = 0; lookup < 24; lookup++)
		{
			i = rnd() % CACHE_TEXTURES;
			if (lookup == 12 && rnd() % 3 == 0)
			{
				const u32 addr = 0x10000 + (rnd() % 0x8000 & ~1u);
				const u32 bytes = 2 + (rnd() % 4096 & ~1u);

				VramLockedWriteRange(addr, bytes);
				for (n = 0; n < bytes; n++)
					vram.data[addr + n] = (u8)rnd();
			}
			t = cache_get(&cache, i);
			checks++;
			if (!cache_same_as_new(t, i))
			{
				fprintf(stderr, "FAIL: texture %u in render %u, request %u: not what a new one uploads\n", i, frame, lookup);
				return 1;
			}
		}
	}

	/* changed often enough to be watched by its hash, then written no
	 * more: hashed once a render, however often it is asked for */
	for (frame = 0; frame <= FREQUENT_UPDATE_COUNT; frame++)
	{
		FrameCount++;
		cache_write(cache_textures[0][0], 2, false);
		t = cache_get(&cache, 0);
	}
	if (t->base.lock_block != NULL || t->base.Updates < FREQUENT_UPDATE_COUNT)
	{
		fprintf(stderr, "FAIL: the texture is not watched by its hash\n");
		return 1;
	}
	for (frame = 0; frame < 12; frame++)
	{
		u32 lookup;

		FrameCount++;
		cache_hashed = 0;
		for (lookup = 0; lookup < 300; lookup++)
			cache_get(&cache, 0);
	}
	still = cache_hashed;
	if (still > t->base.size)
	{
		fprintf(stderr, "FAIL: a texture asked for 300 times in a render had %u bytes hashed\n", (unsigned)still);
		return 1;
	}
	printf("texture cache: %u requests, every one what a full upload makes; %u bytes hashed for 300 of one\n",
			checks, (unsigned)still);
	cache_free(&cache);
	return 0;
}

/* "part": textures watched by their hashes, with a few bytes of them
 * written between renders, as an atlas is: only the regions the changed
 * pieces are in made again, on the CPU and on the GPU (the kernel run
 * here, over the copy a renderer makes for it), every request held to
 * what a full upload of a new texture makes. Twiddled, planar (with a
 * stride wider and narrower than the texture), compressed (the codebook
 * written too), paletted (the palette changed too), mipmapped; to 16, 32
 * and 8 bits. And what is made: fewer texels than the texture has. */

/* address, format, width and height (8 << n), planar, compressed,
 * mipmapped, stride selected, palette selection, filtered */
static const u32 part_textures[][10] = {
	{ 0x100238, TA_PIXEL_565,     5, 5, 0, 0, 0, 0, 0,  0 },
	{ 0x140010, TA_PIXEL_1555,    3, 5, 0, 0, 0, 0, 0,  0 },
	{ 0x150000, TA_PIXEL_4444,    6, 6, 0, 0, 0, 0, 0,  0 },
	{ 0x1d0108, TA_PIXEL_YUV422,  4, 4, 0, 0, 0, 0, 0,  0 },
	{ 0x1e01c8, TA_PIXEL_565,     5, 4, 1, 0, 0, 1, 0,  0 },
	{ 0x210040, TA_PIXEL_4444,    6, 3, 1, 0, 0, 1, 0,  0 },
	{ 0x260f98, TA_PIXEL_1555,    5, 4, 1, 0, 0, 0, 0,  0 },
	{ 0x290f40, TA_PIXEL_YUV422,  5, 5, 1, 0, 0, 0, 0,  0 },
	{ 0x2b1828, TA_PIXEL_565,     5, 5, 0, 1, 0, 0, 0,  0 },
	{ 0x2c8000, TA_PIXEL_1555,    5, 4, 1, 1, 0, 0, 0,  0 },
	{ 0x2d0400, TA_PIXEL_4BPP,    5, 5, 0, 0, 0, 0, 3,  0 },
	{ 0x2e0000, TA_PIXEL_8BPP,    4, 4, 0, 0, 0, 0, 16, 1 },
	{ 0x2f0008, TA_PIXEL_8BPP,    5, 3, 0, 0, 0, 0, 32, 0 },
	{ 0x300040, TA_PIXEL_565,     5, 5, 0, 0, 1, 0, 0,  0 },
	{ 0x340000, TA_PIXEL_4444,    6, 6, 0, 1, 1, 0, 0,  0 },
	{ 0x350010, TA_PIXEL_4BPP,    4, 4, 0, 0, 1, 0, 5,  1 },
	{ 0x360000, TA_PIXEL_BUMPMAP, 4, 5, 0, 0, 0, 0, 0,  0 },
	{ 0x7f8300, TA_PIXEL_565,     5, 5, 1, 0, 0, 0, 0,  0 },	/* past the end of video memory: its first 62 rows */
};
#define PART_TEXTURES (sizeof(part_textures) / sizeof(part_textures[0]))
static u32 part_made[PART_TEXTURES];

static void part_key(u32 i, u32 *tsp, u32 *tcw)
{
	const u32 *d = part_textures[i];
	*tsp = 0;
	*tcw = 0;
	*tsp = TAW_SET(*tsp, TSP_TexU, d[2]);
	*tsp = TAW_SET(*tsp, TSP_TexV, d[3]);
	*tsp = TAW_SET(*tsp, TSP_FilterMode, d[9]);
	*tcw = TAW_SET(*tcw, TCW_TexAddr, d[0] >> 3);
	*tcw = TAW_SET(*tcw, TCW_PixelFmt, d[1]);
	*tcw = TAW_SET(*tcw, TCW_VQ_Comp, d[5]);
	*tcw = TAW_SET(*tcw, TCW_MipMapped, d[6]);
	/* (a paletted texture's selection has the scan order's and the stride's bits) */
	if (d[1] == TA_PIXEL_4BPP || d[1] == TA_PIXEL_8BPP)
		*tcw = TAW_SET(*tcw, TCW_PalSelect, d[8]);
	else
	{
		*tcw = TAW_SET(*tcw, TCW_ScanOrder, d[4]);
		*tcw = TAW_SET(*tcw, TCW_StrideSel, d[7]);
	}
}

static bool part_same_as_new(const cache_tex *t, u32 i)
{
	const bool gpu = cache_gpu, parts = cache_parts;
	cache_tex fresh;
	bool same;

	cache_gpu = cache_parts = false;
	fresh_init(&fresh);
	part_key(i, &fresh.base.tsp, &fresh.base.tcw);
	fresh.made = true;
	texcache_create(&fresh.base);
	fresh.base.Updates = FREQUENT_UPDATE_COUNT;
	texcache_update(&fresh.base);
	same = fresh.gpu != NULL && t->gpu_bytes == fresh.gpu_bytes && !memcmp(t->gpu, fresh.gpu, fresh.gpu_bytes);
	free(fresh.gpu);
	texcache_delete(&fresh.base);
	cache_gpu = gpu;
	cache_parts = parts;
	return same;
}

static cache_tex *part_get(tex_map_t *cache, u32 i)
{
	u32 tsp, tcw;
	cache_tex *t;

	part_key(i, &tsp, &tcw);
	t = cache_lookup(cache, tsp, tcw);
	if (!t->made)
	{
		t->gpu = NULL;
		t->made = true;
		texcache_create(&t->base);
		/* watched by its hashes from its first update */
		t->base.Updates = FREQUENT_UPDATE_COUNT;
	}
	if (texcache_needs_update(&t->base))
	{
		const u32 parts = cache_part_count;
		texcache_update(&t->base);
		if (cache_part_count != parts)
			part_made[i]++;
	}
	return t;
}

/* A few bytes of texture @i; its codebook; much of it; the bytes either
 * side of where one of the pieces it is hashed by ends; or nothing. (Its
 * bytes as the cache locks and hashes them: from its first to the end of
 * its largest level.) */
static void part_write(tex_map_t *cache, u32 i)
{
	const u32 kind = rnd() % 16;
	u32 tsp, tcw;
	cache_tex *t;
	u32 first, bytes, at, n, k;

	part_key(i, &tsp, &tcw);
	t = cache_lookup(cache, tsp, tcw);
	first = t->base.sa_tex;
	bytes = t->base.sa + t->base.size - t->base.sa_tex;
	if (kind < 10)
	{
		n = 1 + rnd() % (kind < 6 ? 64 : 3000);
		at = first + rnd() % bytes;
	}
	else if (kind == 10 && part_textures[i][5])
	{
		n = 1;
		at = first + rnd() % 2048;
	}
	else if (kind == 11)
	{
		n = bytes / 2 + rnd() % (bytes / 2);
		at = first + rnd() % (bytes - n + 1);
	}
	else if (kind < 15)
	{
		/* the last few bytes of a piece, or its first */
		n = 1 + rnd() % 8;
		at = (first + rnd() % bytes) & ~(TEX_HASH_PIECE - 1);
		if (kind != 14)
			at -= n;
		if (at < first)
			at = first;
	}
	else
		return;
	if (at + n > first + bytes)
		n = first + bytes - at;
	for (k = 0; k < n; k++)
		vram.data[at + k] = (u8)rnd();
}

static void part_palette(void)
{
	/* (the value drawn first, then where it goes) */
	const u32 v = rnd();
	PALETTE_RAM[rnd() % 1024] = v;
	palette_update();
}

static int part_run(bool gpu, u32 ctrl, bool force32)
{
	tex_map_t cache;
	u32 frame, i;
	size_t whole = 0;

	cache_init(&cache);
	cache_gpu = gpu;
	cache_parts = true;
	cache_force32 = force32;
	settings.rend.GpuTextureDecode = gpu;
	PAL_RAM_CTRL = ctrl;
	TEXT_CONTROL = 10;	/* a stride of 320 */
	palette_update();
	memset(part_made, 0, sizeof(part_made));
	cache_part_bytes = 0;
	for (frame = 0; frame < 120; frame++)
	{
		FrameCount++;
		if (frame != 0)
			for (i = 0; i < PART_TEXTURES; i++)
			{
				u32 n;
				for (n = 1 + rnd() % 3; n != 0; n--)
					part_write(&cache, i);
			}
		if (rnd() % 23 == 0)
			part_palette();
		if (rnd() % 37 == 0)
			TEXT_CONTROL = TEXT_CONTROL == 10 ? 12 : 10;
		for (i = 0; i < PART_TEXTURES; i++)
		{
			cache_tex *t = part_get(&cache, i);
			if (!part_same_as_new(t, i))
			{
				fprintf(stderr, "FAIL: %s, palette %u%s: texture %u in render %u is not what a new one uploads\n",
						gpu ? "GPU" : "CPU", ctrl, force32 ? ", 32-bit" : "", i, frame);
				return 1;
			}
			if (frame == 0)
				whole += t->gpu_bytes;
		}
	}
	if (cache_bad_regions != 0)
	{
		fprintf(stderr, "FAIL: %s, palette %u%s: %u regions off their level, not on multiples of 8 or overlapping\n",
				gpu ? "GPU" : "CPU", ctrl, force32 ? ", 32-bit" : "", cache_bad_regions);
		return 1;
	}
	for (i = 0; i < PART_TEXTURES; i++)
		if (part_made[i] == 0)
		{
			fprintf(stderr, "FAIL: %s, palette %u%s: texture %u never had a part of it made again\n",
					gpu ? "GPU" : "CPU", ctrl, force32 ? ", 32-bit" : "", i);
			return 1;
		}
	printf("texture parts (%s, palette %u%s): every request what a full upload makes; %u KB made in parts, of %u KB a whole round\n",
			gpu ? "GPU" : "CPU", ctrl, force32 ? ", 32-bit" : "", (unsigned)(cache_part_bytes >> 10), (unsigned)(whole >> 10));
	cache_free(&cache);
	return 0;
}

static int part_test(void)
{
	u32 i;

	vram.data = (u8 *)malloc(8 << 20);
	vram.size = 8 << 20;
	for (i = 0; i < (8u << 20); i++)
		vram.data[i] = (u8)rnd();
	settings.rend.TextureUpscale = 1;
	settings.rend.MaxFilteredTextureSize = 256;
	for (i = 0; i < 8; i++)
		if (part_run((i & 1) != 0, (i & 2) ? 3 : 1, (i & 4) != 0))
			return 1;
	free(vram.data);
	return 0;
}

#define RUN16(name, fn) run(name, fn, w, h, 2)
#define RUN32(name, fn) run(name, fn, w, h, 4)
#define RUN8(name, fn)  run(name, fn, w, h, 1)

int main(int argc, char **argv)
{
	static const u32 sizes[][2] = { {8, 8}, {32, 32}, {64, 16}, {16, 64}, {256, 256} };
	u32 i, s;

	texconv_init();	/* (the twiddle tables: libPvr_Init()'s) */
	if (argc > 1 && !strcmp(argv[1], "cache"))
		return cache_test();
	if (argc > 1 && !strcmp(argv[1], "part"))
		return part_test();
	texconv_init();	/* (the twiddle tables, made again: the same) */
	buf = (u8 *)malloc(4 * 1024 * 1024);
	if (argc > 1 && !strcmp(argv[1], "ramp"))
		for (i = 0; i < 4 * 1024 * 1024; i += 2) { buf[i] = (i / 2) & 0xFF; buf[i + 1] = ((i / 2) >> 8) & 0xFF; }
	else
		for (i = 0; i < 4 * 1024 * 1024; i++) buf[i] = (u8)rnd();
	vq_codebook = buf;
	palette_index = 64;
	for (i = 0; i < 1024; i++) { palette16_ram[i] = (i * 2654435761u) >> 16; palette32_ram[i] = i * 2246822519u; }
	for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
	{
		const u32 w = sizes[s][0], h = sizes[s][1];
		RUN16("1555_TW", tex1555_TW);   RUN16("1555_VQ", tex1555_VQ);
		RUN16("565_TW", tex565_TW);     RUN16("565_VQ", tex565_VQ);
		RUN16("4444_TW", tex4444_TW);   RUN16("4444_VQ", tex4444_VQ);
		RUN16("BMP_TW", texBMP_TW);     RUN16("BMP_VQ", texBMP_VQ);
		RUN16("PAL4_TW", texPAL4_TW);   RUN16("PAL4_VQ", texPAL4_VQ);
		RUN16("PAL8_TW", texPAL8_TW);   RUN16("PAL8_VQ", texPAL8_VQ);
		RUN32("1555_PL32", tex1555_PL32); RUN32("1555_TW32", tex1555_TW32); RUN32("1555_VQ32", tex1555_VQ32);
		RUN32("565_PL32", tex565_PL32);   RUN32("565_TW32", tex565_TW32);   RUN32("565_VQ32", tex565_VQ32);
		RUN32("4444_PL32", tex4444_PL32); RUN32("4444_TW32", tex4444_TW32); RUN32("4444_VQ32", tex4444_VQ32);
		RUN32("YUV_PL", texYUV422_PL);    RUN32("YUV_TW", texYUV422_TW);    RUN32("YUV_VQ", texYUV422_VQ);
		RUN32("PAL4_TW32", texPAL4_TW32); RUN32("PAL4_VQ32", texPAL4_VQ32);
		RUN32("PAL8_TW32", texPAL8_TW32); RUN32("PAL8_VQ32", texPAL8_VQ32);
		RUN8("PAL4PT_TW", texPAL4PT_TW);  RUN8("PAL8PT_TW", texPAL8PT_TW);
		RUN32("1555_PLVQ32", tex1555_PLVQ32); RUN32("565_PLVQ32", tex565_PLVQ32);
		RUN32("4444_PLVQ32", tex4444_PLVQ32); RUN32("YUV_PLVQ", texYUV422_PLVQ);
		/* (planar to 16 bits, as this tree has always made them: not upstream's) */
		RUN16("1555_PL", tex1555_PL);   RUN16("565_PL", tex565_PL);
		RUN16("4444_PL", tex4444_PL);   RUN16("BMP_PL", texBMP_PL);
	}
	return 0;
}
