/* The texture converters, against upstream's.
 *
 * Every converter the texture cache has - each pixel format, twiddled,
 * compressed, planar, paletted, to 16 and to 32 bits - is given the same
 * bytes at five sizes, and a hash and the first four pixels of what comes
 * out are printed, one line each. texconv_expected.txt is what upstream
 * flycast's converters print for the same bytes; texconv_test.sh compares.
 *
 * Built with -DUPSTREAM in an upstream tree (and its texconv.cpp object)
 * this prints that file's contents, which is how it was made:
 *   (echo "# random"; ./a.out; echo "# ramp"; ./a.out ramp)
 *
 * The converters are pure functions of their input, so this is also what a
 * rewrite of them has to keep printing. */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#ifdef UPSTREAM
#include "rend/texconv.h"
#define T opengl::pvrTexInfo
#else
#include "rend/TexCache.h"
#endif

static u8 *buf;
static u32 rnd_state = 0x12345678;
static u32 rnd() { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5; return rnd_state; }

template<typename P, typename F> static void run(const char *name, F fn, u32 w, u32 h)
{
	// upstream's VQ converters are handed the indices, this tree's the codebook in front of them
	u32 off = 0;
#ifdef UPSTREAM
	if (strstr(name, "VQ")) off = 2048;
#endif
	if (!fn) { printf("%-12s %4ux%-4u (none)\n", name, w, h); return; }
	PixelBuffer<P> pb;
	pb.init(w, h);
	memset(pb.data(), 0xEE, (size_t)w * h * sizeof(P));
	fn(&pb, buf + 2048 + off, w, h);
	const u8 *d = (const u8 *)pb.data();
	u32 hash = 2166136261u;
	for (size_t i = 0; i < (size_t)w * h * sizeof(P); i++) hash = (hash ^ d[i]) * 16777619u;
	printf("%-12s %4ux%-4u %08x  first", name, w, h, hash);
	for (int i = 0; i < 4; i++) { u32 v = 0; memcpy(&v, d + i * sizeof(P), sizeof(P)); printf(" %0*x", (int)sizeof(P) * 2, v); }
	printf("\n");
}

#ifndef UPSTREAM
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

extern VArray2 vram;
extern "C" unsigned texconv_XXH32(const void *input, size_t length, unsigned seed);

settings_t settings;
u32 FrameCount = 1;
unsigned VRAM_SIZE = 8 << 20;
unsigned VRAM_MASK = (8 << 20) - 1;
static u8 cache_regs[pvr_RegSize];
u8 *rend_pvr_regs = cache_regs;
static u8 cache_protected[(16 << 20) / PAGE_SIZE];
static size_t cache_hashed;

extern "C" unsigned XXH32(const void *input, size_t length, unsigned seed)
{
	cache_hashed += length;
	return texconv_XXH32(input, length, seed);
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
void GenericLog(LogTypes::LOG_LEVELS level, LogTypes::LOG_TYPE type, const char *file, int line, const char *fmt, ...) { }

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

static void part_check(const BaseTextureCacheData *t, bool mips, const TexRegion *r, u32 count)
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
class CacheTex : public BaseTextureCacheData
{
public:
	u8 *gpu;
	size_t gpu_bytes;
	bool made;

	std::string GetId() override { return std::string(); }
	static size_t Pixel(TextureType type)
	{
		return type == TextureType::_8888 ? 4 : type == TextureType::_8 ? 1 : 2;
	}
	/* Where level @level of the image (0 the largest) is kept, and its width */
	size_t LevelAt(u32 level, u32 *width)
	{
		const size_t pixel = Pixel(tex_type);
		size_t at = 0;
		u32 l, top;

		if (!made_mips)
		{
			*width = w;
			return 0;
		}
		top = bitscanrev(w);
		for (l = 0; l < top - level; l++)
			at += ((size_t)1 << (2 * l)) * pixel;
		*width = 1u << (top - level);
		return at;
	}
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded) override
	{
		const size_t pixel = Pixel(tex_type);
		gpu_bytes = (size_t)width * height * pixel;
		if (mipmapsIncluded)
			for (int d = width / 2; d != 0; d /= 2)
				gpu_bytes += (size_t)d * d * pixel;
		gpu = (u8 *)realloc(gpu, gpu_bytes);
		memcpy(gpu, data, gpu_bytes);
		cache_whole_count++;
	}
	bool Force32BitTexture(TextureType type) const override { return cache_force32; }
	bool PartMemory(size_t bytes, void **memory) override
	{
		*memory = NULL;
		return cache_parts && gpu != NULL;
	}
	void UploadPart(const TexRegion *regions, u32 count, u8 *data, const size_t *offset) override
	{
		const size_t pixel = Pixel(tex_type);
		u32 i, y;

		part_check(this, made_mips, regions, count);
		for (i = 0; i < count; i++)
		{
			u32 width;
			const size_t at = LevelAt(regions[i].level, &width);
			for (y = 0; y < regions[i].h; y++)
				memcpy(gpu + at + ((size_t)(regions[i].y + y) * width + regions[i].x) * pixel,
						data + offset[i] + (size_t)y * regions[i].w * pixel, regions[i].w * pixel);
			cache_part_bytes += (size_t)regions[i].w * regions[i].h * pixel;
		}
		cache_part_count++;
	}
	bool GpuDecode(const texd_plan& plan) override
	{
		const size_t pixel = Pixel(tex_type);
		u8 *bytes;
		u32 i;

		if (!cache_gpu)
			return false;
		bytes = (u8 *)calloc(plan.pal + plan.pal_entries * 4 + 4, 1);
		memcpy(bytes, &vram[plan.src], plan.bytes);
		if (plan.pal_entries != 0)
			memcpy(bytes + plan.pal, (plan.out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan.pal_index, plan.pal_entries * 4);
		gpu_bytes = (size_t)plan.width * plan.height * pixel;
		if (plan.mipmapped)
			for (u32 d = plan.width / 2; d != 0; d /= 2)
				gpu_bytes += (size_t)d * d * pixel;
		gpu = (u8 *)realloc(gpu, gpu_bytes);
		for (i = 0; i < plan.levels; i++)
		{
			u32 width;
			const size_t at = LevelAt(plan.level[i].mip, &width);
			texd_c_level(bytes, &plan, &plan.level[i], gpu + at, width);
		}
		free(bytes);
		cache_whole_count++;
		return true;
	}
	int GpuDecodePart(const texd_plan& plan, const TexRegion *regions, u32 count) override
	{
		const size_t pixel = Pixel(tex_type);
		TexPartPlan part;
		u8 *ring;
		u32 i, x, y;

		if (!cache_gpu)
			return TEXP_CPU;
		if (!cache_parts || gpu == NULL || !tex_plan_part(&plan, regions, count, &part))
			return TEXP_WHOLE;
		part_check(this, made_mips, regions, count);
		/* the copy as a renderer makes it, and the kernel over each region */
		ring = (u8 *)calloc(part.total + 4, 1);
		memcpy(ring, &vram[plan.src], part.codebook);
		for (i = 0; i < count; i++)
			memcpy(ring + part.at[i], &vram[plan.src + part.from[i]], part.bytes[i]);
		if (plan.pal_entries != 0)
			memcpy(ring + part.pal, (plan.out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan.pal_index, plan.pal_entries * 4);
		texd_c_set(ring);
		for (i = 0; i < count; i++)
		{
			const texd_level *lv = &plan.level[part.level[i]];
			u32 width;
			const size_t at = LevelAt(regions[i].level, &width);
			for (y = regions[i].y; y < regions[i].y + regions[i].h; y++)
				for (x = regions[i].x; x < regions[i].x + regions[i].w; x++)
				{
					const u32 v = texd_texel(part.data[i], part.pal, lv->sr, lv->mode, x, y);
					memcpy(gpu + at + ((size_t)y * width + x) * pixel, &v, pixel);
				}
			cache_part_bytes += (size_t)regions[i].w * regions[i].h * pixel;
		}
		free(ring);
		cache_part_count++;
		return TEXP_DONE;
	}
};

class CacheOf : public BaseTextureCache<CacheTex>
{
public:
	void Free()
	{
		std::unordered_map<u64, CacheTex>::iterator it;
		for (it = cache.begin(); it != cache.end(); ++it)
		{
			free(it->second.gpu);
			it->second.Delete();
		}
	}
};

/* address, format, width and height (8 << n), planar, compressed */
static const u32 cache_textures[][6] = {
	{ 0x10000, Pixel565,  3, 3, 0, 0 },
	{ 0x10000, Pixel1555, 3, 3, 0, 0 },
	{ 0x12000, Pixel4444, 2, 2, 0, 0 },
	{ 0x12800, Pixel565,  2, 1, 1, 0 },
	{ 0x13000, Pixel565,  4, 2, 0, 0 },
	{ 0x14000, Pixel565,  3, 3, 0, 1 },
};
#define CACHE_TEXTURES (sizeof(cache_textures) / sizeof(cache_textures[0]))

static void cache_key(u32 i, TSP *tsp, TCW *tcw)
{
	tsp->full = 0;
	tcw->full = 0;
	tsp->TexU = cache_textures[i][2];
	tsp->TexV = cache_textures[i][3];
	tcw->TexAddr = cache_textures[i][0] >> 3;
	tcw->PixelFmt = cache_textures[i][1];
	tcw->ScanOrder = cache_textures[i][4];
	tcw->VQ_Comp = cache_textures[i][5];
}

/* What a texture made now uploads, against what @t has uploaded */
static bool cache_same_as_new(const CacheTex *t, u32 i)
{
	CacheTex fresh;
	bool same;

	cache_key(i, &fresh.tsp, &fresh.tcw);
	fresh.area = 0;
	fresh.gpu = NULL;
	fresh.Create();
	/* (unprotected, as a texture updated often is: it leaves no lock) */
	fresh.Updates = FREQUENT_UPDATE_COUNT;
	fresh.Update();
	same = fresh.gpu != NULL && t->gpu_bytes == fresh.gpu_bytes && !memcmp(t->gpu, fresh.gpu, fresh.gpu_bytes);
	free(fresh.gpu);
	fresh.Delete();
	return same;
}

static CacheTex *cache_get(CacheOf *cache, u32 i)
{
	TSP tsp;
	TCW tcw;
	CacheTex *t;

	cache_key(i, &tsp, &tcw);
	t = cache->getTextureCacheData(tsp, tcw);
	if (!t->made)
	{
		t->gpu = NULL;
		t->made = true;
		t->Create();
	}
	if (t->NeedsUpdate())
		t->Update();
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
			vram[addr + i] = (u8)rnd();
}

static int cache_test(void)
{
	CacheOf cache;
	CacheTex *t;
	u32 frame;
	u32 i;
	u32 checks = 0;
	size_t still;

	vram.data = (u8 *)malloc(8 << 20);
	vram.size = 8 << 20;
	for (i = 0; i < (8u << 20); i++)
		vram[i] = (u8)rnd();
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
					vram[addr + n] = (u8)rnd();
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
	if (t->lock_block != nullptr || t->Updates < FREQUENT_UPDATE_COUNT)
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
	if (still > t->size)
	{
		fprintf(stderr, "FAIL: a texture asked for 300 times in a render had %u bytes hashed\n", (unsigned)still);
		return 1;
	}
	printf("texture cache: %u requests, every one what a full upload makes; %u bytes hashed for 300 of one\n",
			checks, (unsigned)still);
	cache.Free();
	return 0;
}
#endif

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
	{ 0x100238, Pixel565,     5, 5, 0, 0, 0, 0, 0,  0 },
	{ 0x140010, Pixel1555,    3, 5, 0, 0, 0, 0, 0,  0 },
	{ 0x150000, Pixel4444,    6, 6, 0, 0, 0, 0, 0,  0 },
	{ 0x1d0108, PixelYUV,     4, 4, 0, 0, 0, 0, 0,  0 },
	{ 0x1e01c8, Pixel565,     5, 4, 1, 0, 0, 1, 0,  0 },
	{ 0x210040, Pixel4444,    6, 3, 1, 0, 0, 1, 0,  0 },
	{ 0x260f98, Pixel1555,    5, 4, 1, 0, 0, 0, 0,  0 },
	{ 0x290f40, PixelYUV,     5, 5, 1, 0, 0, 0, 0,  0 },
	{ 0x2b1828, Pixel565,     5, 5, 0, 1, 0, 0, 0,  0 },
	{ 0x2c8000, Pixel1555,    5, 4, 1, 1, 0, 0, 0,  0 },
	{ 0x2d0400, PixelPal4,    5, 5, 0, 0, 0, 0, 3,  0 },
	{ 0x2e0000, PixelPal8,    4, 4, 0, 0, 0, 0, 16, 1 },
	{ 0x2f0008, PixelPal8,    5, 3, 0, 0, 0, 0, 32, 0 },
	{ 0x300040, Pixel565,     5, 5, 0, 0, 1, 0, 0,  0 },
	{ 0x340000, Pixel4444,    6, 6, 0, 1, 1, 0, 0,  0 },
	{ 0x350010, PixelPal4,    4, 4, 0, 0, 1, 0, 5,  1 },
	{ 0x360000, PixelBumpMap, 4, 5, 0, 0, 0, 0, 0,  0 },
	{ 0x7f8300, Pixel565,     5, 5, 1, 0, 0, 0, 0,  0 },	/* past the end of video memory: its first 62 rows */
};
#define PART_TEXTURES (sizeof(part_textures) / sizeof(part_textures[0]))
static u32 part_made[PART_TEXTURES];

static void part_key(u32 i, TSP *tsp, TCW *tcw)
{
	const u32 *d = part_textures[i];
	tsp->full = 0;
	tcw->full = 0;
	tsp->TexU = d[2];
	tsp->TexV = d[3];
	tsp->FilterMode = d[9];
	tcw->TexAddr = d[0] >> 3;
	tcw->PixelFmt = d[1];
	tcw->VQ_Comp = d[5];
	tcw->MipMapped = d[6];
	/* (a paletted texture's selection has the scan order's and the stride's bits) */
	if (d[1] == PixelPal4 || d[1] == PixelPal8)
		tcw->PalSelect = d[8];
	else
	{
		tcw->ScanOrder = d[4];
		tcw->StrideSel = d[7];
	}
}

static bool part_same_as_new(const CacheTex *t, u32 i)
{
	const bool gpu = cache_gpu, parts = cache_parts;
	CacheTex fresh;
	bool same;

	cache_gpu = cache_parts = false;
	part_key(i, &fresh.tsp, &fresh.tcw);
	fresh.area = 0;
	fresh.gpu = NULL;
	fresh.made = true;
	fresh.Create();
	fresh.Updates = FREQUENT_UPDATE_COUNT;
	fresh.Update();
	same = fresh.gpu != NULL && t->gpu_bytes == fresh.gpu_bytes && !memcmp(t->gpu, fresh.gpu, fresh.gpu_bytes);
	free(fresh.gpu);
	fresh.Delete();
	cache_gpu = gpu;
	cache_parts = parts;
	return same;
}

static CacheTex *part_get(CacheOf *cache, u32 i)
{
	TSP tsp;
	TCW tcw;
	CacheTex *t;

	part_key(i, &tsp, &tcw);
	t = cache->getTextureCacheData(tsp, tcw);
	if (!t->made)
	{
		t->gpu = NULL;
		t->made = true;
		t->Create();
		/* watched by its hashes from its first update */
		t->Updates = FREQUENT_UPDATE_COUNT;
	}
	if (t->NeedsUpdate())
	{
		const u32 parts = cache_part_count;
		t->Update();
		if (cache_part_count != parts)
			part_made[i]++;
	}
	return t;
}

/* A few bytes of texture @i; its codebook; much of it; the bytes either
 * side of where one of the pieces it is hashed by ends; or nothing. (Its
 * bytes as the cache locks and hashes them: from its first to the end of
 * its largest level.) */
static void part_write(CacheOf *cache, u32 i)
{
	const u32 kind = rnd() % 16;
	TSP tsp;
	TCW tcw;
	CacheTex *t;
	u32 first, bytes, at, n, k;

	part_key(i, &tsp, &tcw);
	t = cache->getTextureCacheData(tsp, tcw);
	first = t->sa_tex;
	bytes = t->sa + t->size - t->sa_tex;
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
		vram[at + k] = (u8)rnd();
}

static void part_palette(void)
{
	PALETTE_RAM[rnd() % 1024] = rnd();
	palette_update();
}

static int part_run(bool gpu, u32 ctrl, bool force32)
{
	CacheOf cache;
	u32 frame, i;
	size_t whole = 0;

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
			CacheTex *t = part_get(&cache, i);
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
	cache.Free();
	return 0;
}

static int part_test(void)
{
	u32 i;

	vram.data = (u8 *)malloc(8 << 20);
	vram.size = 8 << 20;
	for (i = 0; i < (8u << 20); i++)
		vram[i] = (u8)rnd();
	settings.rend.TextureUpscale = 1;
	settings.rend.MaxFilteredTextureSize = 256;
	for (i = 0; i < 8; i++)
		if (part_run((i & 1) != 0, (i & 2) ? 3 : 1, (i & 4) != 0))
			return 1;
	free(vram.data);
	return 0;
}

int main(int argc, char **argv)
{
	static const u32 sizes[][2] = { {8, 8}, {32, 32}, {64, 16}, {16, 64}, {256, 256} };
#ifndef UPSTREAM
	if (argc > 1 && !strcmp(argv[1], "cache"))
		return cache_test();
	if (argc > 1 && !strcmp(argv[1], "part"))
		return part_test();
#endif
	buf = (u8 *)malloc(4 * 1024 * 1024);
	if (argc > 1 && !strcmp(argv[1], "ramp"))
		for (u32 i = 0; i < 4 * 1024 * 1024; i += 2) { buf[i] = (i / 2) & 0xFF; buf[i + 1] = ((i / 2) >> 8) & 0xFF; }
	else
		for (u32 i = 0; i < 4 * 1024 * 1024; i++) buf[i] = (u8)rnd();
	vq_codebook = buf;
	palette_index = 64;
	for (u32 i = 0; i < 1024; i++) { palette16_ram[i] = (i * 2654435761u) >> 16; palette32_ram[i] = i * 2246822519u; }
	for (auto& s : sizes)
	{
		u32 w = s[0], h = s[1];
#ifdef UPSTREAM
		run<u16>("1555_TW", T[0].TW, w, h);   run<u16>("1555_VQ", T[0].VQ, w, h);
		run<u16>("565_TW", T[1].TW, w, h);    run<u16>("565_VQ", T[1].VQ, w, h);
		run<u16>("4444_TW", T[2].TW, w, h);   run<u16>("4444_VQ", T[2].VQ, w, h);
		run<u16>("BMP_TW", T[4].TW, w, h);    run<u16>("BMP_VQ", T[4].VQ, w, h);
		run<u16>("PAL4_TW", T[5].TW, w, h);   run<u16>("PAL4_VQ", T[5].VQ, w, h);
		run<u16>("PAL8_TW", T[6].TW, w, h);   run<u16>("PAL8_VQ", T[6].VQ, w, h);
		run<u32>("1555_PL32", T[0].PL32, w, h); run<u32>("1555_TW32", T[0].TW32, w, h); run<u32>("1555_VQ32", T[0].VQ32, w, h);
		run<u32>("565_PL32", T[1].PL32, w, h);  run<u32>("565_TW32", T[1].TW32, w, h);  run<u32>("565_VQ32", T[1].VQ32, w, h);
		run<u32>("4444_PL32", T[2].PL32, w, h); run<u32>("4444_TW32", T[2].TW32, w, h); run<u32>("4444_VQ32", T[2].VQ32, w, h);
		run<u32>("YUV_PL", T[3].PL32, w, h);    run<u32>("YUV_TW", T[3].TW32, w, h);    run<u32>("YUV_VQ", T[3].VQ32, w, h);
		run<u32>("PAL4_TW32", T[5].TW32, w, h); run<u32>("PAL4_VQ32", T[5].VQ32, w, h);
		run<u32>("PAL8_TW32", T[6].TW32, w, h); run<u32>("PAL8_VQ32", T[6].VQ32, w, h);
		run<u8>("PAL4PT_TW", T[5].TW8, w, h);   run<u8>("PAL8PT_TW", T[6].TW8, w, h);
		run<u32>("1555_PLVQ32", T[0].PLVQ32, w, h); run<u32>("565_PLVQ32", T[1].PLVQ32, w, h);
		run<u32>("4444_PLVQ32", T[2].PLVQ32, w, h); run<u32>("YUV_PLVQ", T[3].PLVQ32, w, h);
#else
		run<u16>("1555_TW", &tex1555_TW, w, h);   run<u16>("1555_VQ", &tex1555_VQ, w, h);
		run<u16>("565_TW", &tex565_TW, w, h);     run<u16>("565_VQ", &tex565_VQ, w, h);
		run<u16>("4444_TW", &tex4444_TW, w, h);   run<u16>("4444_VQ", &tex4444_VQ, w, h);
		run<u16>("BMP_TW", &texBMP_TW, w, h);     run<u16>("BMP_VQ", &texBMP_VQ, w, h);
		run<u16>("PAL4_TW", &texPAL4_TW, w, h);   run<u16>("PAL4_VQ", &texPAL4_VQ, w, h);
		run<u16>("PAL8_TW", &texPAL8_TW, w, h);   run<u16>("PAL8_VQ", &texPAL8_VQ, w, h);
		run<u32>("1555_PL32", &tex1555_PL32, w, h); run<u32>("1555_TW32", &tex1555_TW32, w, h); run<u32>("1555_VQ32", &tex1555_VQ32, w, h);
		run<u32>("565_PL32", &tex565_PL32, w, h);   run<u32>("565_TW32", &tex565_TW32, w, h);   run<u32>("565_VQ32", &tex565_VQ32, w, h);
		run<u32>("4444_PL32", &tex4444_PL32, w, h); run<u32>("4444_TW32", &tex4444_TW32, w, h); run<u32>("4444_VQ32", &tex4444_VQ32, w, h);
		run<u32>("YUV_PL", &texYUV422_PL, w, h);    run<u32>("YUV_TW", &texYUV422_TW, w, h);    run<u32>("YUV_VQ", &texYUV422_VQ, w, h);
		run<u32>("PAL4_TW32", &texPAL4_TW32, w, h); run<u32>("PAL4_VQ32", &texPAL4_VQ32, w, h);
		run<u32>("PAL8_TW32", &texPAL8_TW32, w, h); run<u32>("PAL8_VQ32", &texPAL8_VQ32, w, h);
		run<u8>("PAL4PT_TW", &texPAL4PT_TW, w, h);  run<u8>("PAL8PT_TW", &texPAL8PT_TW, w, h);
		run<u32>("1555_PLVQ32", &tex1555_PLVQ32, w, h); run<u32>("565_PLVQ32", &tex565_PLVQ32, w, h);
		run<u32>("4444_PLVQ32", &tex4444_PLVQ32, w, h); run<u32>("YUV_PLVQ", &texYUV422_PLVQ, w, h);
#endif
	}
	return 0;
}
