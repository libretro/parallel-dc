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

/* A texture whose upload is kept, as the GPU would have it */
class CacheTex : public BaseTextureCacheData
{
public:
	u8 *gpu;
	size_t gpu_bytes;
	bool made;

	std::string GetId() override { return std::string(); }
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded) override
	{
		const size_t pixel = tex_type == TextureType::_8888 ? 4 : tex_type == TextureType::_8 ? 1 : 2;
		gpu_bytes = (size_t)width * height * pixel;
		gpu = (u8 *)realloc(gpu, gpu_bytes);
		memcpy(gpu, data, gpu_bytes);
	}
};

class CacheOf : public BaseTextureCache<CacheTex>
{
public:
	void Free()
	{
		std::unordered_map<u64, CacheTex>::iterator it;
		for (it = cache.begin(); it != cache.end(); ++it)
			free(it->second.gpu);
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

int main(int argc, char **argv)
{
	static const u32 sizes[][2] = { {8, 8}, {32, 32}, {64, 16}, {16, 64}, {256, 256} };
#ifndef UPSTREAM
	if (argc > 1 && !strcmp(argv[1], "cache"))
		return cache_test();
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
