#pragma once
#include "oslib/oslib.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/pvr_mem.h"

#include <algorithm>
#include <array>
#include <retro_atomic.h>
#include <memory>
#include <unordered_map>
#include "rend/tex_map.h"
#include "rend/texconv.h"
#include "rend/vram_lock.h"

/* A converter's texels, as C++ has them: the picture or mipmapped
 * texture of tex_buf_t (texconv.h) for texels of pixel_type */
template<class pixel_type>
class PixelBuffer
{
	tex_buf_t b;

public:
	PixelBuffer() { b.start = b.level = NULL; b.pitch = 0; b.borrowed = false; }
	~PixelBuffer() { deinit(); }

	// How much memory a picture this size takes, with every smaller level if @mipmapped
	static size_t bytes(u32 width, u32 height, bool mipmapped)
	{
		return tex_buf_bytes(width, height, mipmapped, sizeof(pixel_type));
	}

	/* On memory the caller has, bytes() of it or more: nothing is
	 * allocated and nothing freed. For what is converted and handed on at
	 * once, texture after texture (texcache_scratch()). */
	void init(u32 width, u32 height, bool mipmapped, void *memory)
	{
		tex_buf_borrow(&b, width, height, mipmapped, memory);
	}

	void init(u32 width, u32 height)
	{
		tex_buf_alloc(&b, width, height, sizeof(pixel_type));
	}

	void deinit() { tex_buf_free(&b); }

	void steal_data(PixelBuffer &buffer)
	{
		deinit();
		b = buffer.b;
		buffer.b.start = buffer.b.level = NULL;
		buffer.b.borrowed = false;
	}

	void set_mipmap(int level) { tex_buf_set_mipmap(&b, level, sizeof(pixel_type)); }

	pixel_type *data(u32 x = 0, u32 y = 0) { return (pixel_type *)TEX_BUF_AT(&b, x, y, sizeof(pixel_type)); }

	// pixels from one line to the next
	u32 pitch() const { return b.pitch; }

	/* what a converter (tex_conv_fn) writes to */
	tex_buf_t *raw() { return &b; }
};

void texcache_scratch_free();
// Stops the custom texture loader, which then has no texture: see BaseTextureCache::Clear()
void texcache_stop_custom_loads();
/* Room for the locks on video memory of @textures textures, made now, so
 * that locking one takes none (TexCache.cpp): more is made if it runs
 * out, and that is said */
void texcache_reserve(u32 textures);
/* The memory a texture converted on the CPU is converted into, where the
 * renderer has none of its own for it, as large as the largest: made when
 * the cache's room is with GPU Texture Decoding off, and when that is
 * turned off */
void texcache_scratch_largest(void);
/* How many times textures are scaled up (xBRZ), 1 for not: for the pools'
 * plans (TexPoolBook::Plan()) */
static inline u32 texcache_upscale_factor(void)
{
#ifdef HAVE_TEXUPSCALE
	return settings.rend.TextureUpscale > 1 ? (u32)settings.rend.TextureUpscale : 1;
#else
	return 1;
#endif
}

class BaseTextureCacheData;

void libCore_vramlock_Lock(u32 start_offset, u32 end_offset, BaseTextureCacheData *texture);

#ifdef HAVE_TEXUPSCALE
void UpscalexBRZ(int factor, u32* source, u32* dest, int width, int height, bool has_alpha);
#endif

struct PvrTexInfo;
struct texd_plan;
/* (each size of texel's: tex_conv_fn, texconv.h) */
typedef tex_conv_fn TexConvFP;
typedef tex_conv_fn TexConvFP8;
typedef tex_conv_fn TexConvFP32;
enum class TextureType { _565, _5551, _4444, _8888, _8 };

/* After this many updates a texture is treated as frequently updated: its vram
 * is left writable and changes are detected by content hash instead of a write
 * fault (see BaseTextureCacheData::Update / NeedsUpdate). */
#define FREQUENT_UPDATE_COUNT 8

/* Such a texture is hashed by pieces of video memory this size, on their
 * multiples: the pieces that changed are made again, not the texture */
#define TEX_HASH_PIECE 4096

/* A part of a texture's image made again: @w by @h texels from (@x, @y)
 * of the image's level @level, 0 the largest. Multiples of 8. */
struct TexRegion
{
	u32 level;
	u32 x, y;
	u32 w, h;
};

/* The most regions a texture is made again in */
#define TEXP_REGIONS 16

/* What GpuDecodePart() did: made the regions; nothing, and the texture is
 * made whole; nothing, as GpuDecode() would have done, and the regions
 * are made on the CPU */
enum { TEXP_WHOLE, TEXP_DONE, TEXP_CPU };

/* What a renderer copies for the GPU to make regions of a texture with
 * (tex_plan_part()): a compressed texture's codebook, @codebook bytes
 * from the plan's src, at the start; for region i, @bytes[i] from the
 * plan's src + @from[i], at @at[i]; and at @pal the palette words the plan
 * has. @total bytes in all. The kernel is run over region i with the data
 * of its level of the plan, @level[i], where the copy has it: @data[i]. */
struct TexPartPlan
{
	u32 codebook;
	u32 pal;
	u32 total;
	u32 level[TEXP_REGIONS];
	u32 data[TEXP_REGIONS];
	u32 from[TEXP_REGIONS];
	u32 bytes[TEXP_REGIONS];
	u32 at[TEXP_REGIONS];
};
bool tex_plan_part(const texd_plan *plan, const TexRegion *regions, u32 count, TexPartPlan *part);

class BaseTextureCacheData
{
public:
	TSP tsp;        //dreamcast texture parameters
	TCW tcw;

	// Decoded/filtered texture format
	TextureType tex_type;

	u32 sa;         //pixel data start address in vram (might be offset for mipmaps/etc)
	u32 sa_tex;		//texture data start address in vram
	u32 w,h;        //width & height of the texture
	u32 size;       //size, in bytes, in vram
	/* The row length a planar texture was last read from video memory
	 * with; 0 if it is not planar or was not read (one rendered to). The
	 * stride register is not part of the texture's key: a change to it
	 * alone has the texture read again. */
	u32 stride_read;

	const PvrTexInfo* tex;
	TexConvFP*  texconv;
	TexConvFP32*  texconv32;
	TexConvFP8 *texconv8;

	u32 dirty;
	vram_block* lock_block;

	u32 Updates;
	/* For a texture updated often, watched by its hashes instead of
	 * write-protection: a hash for each of the pieces of video memory
	 * (TEX_HASH_PIECE) the locked range is on, from the one at @piece_first
	 * on, @pieces of them, and after them a bit for each that changed */
	u32 *piece_hash = nullptr;
	u32 piece_first = 0;
	u32 pieces = 0;
	/* The render the hashes were last found to hold in, and vram_write_epoch
	 * then: the texture is not hashed again until one of them moves. */
	u32 hash_frame;
	u32 hash_epoch;

	u32 palette_index;
	u8 area = 0;				// 1 if it was first asked for as a second volume's texture: see IsGpuHandledPaletted()
	//used for palette updates
	u32 palette_hash;			// Palette hash at time of last update
	u32 vq_codebook;            // VQ quantizers table for compressed textures
	u32 texture_hash;			// xxhash of texture data, used for custom textures
	u32 old_texture_hash;		// legacy hash
	u32 old_vqtexture_hash;		// the name a compressed texture had while only part of it was hashed; 0 if not compressed
	u8* custom_image_data;		// loaded custom image data
	u32 custom_width;
	u32 custom_height;
	retro_atomic_int_t custom_load_in_progress;
	BaseTextureCacheData *custom_load_next;	// link in the custom texture loader's work list
	/* What the loader is asked to look for: the three names above as they
	 * were when the renderer asked, which is when it had the texture and
	 * video memory to itself. Written by the renderer before it counts the
	 * request in custom_load_in_progress; read by the loader after it has
	 * read that count - so a request that changes them under the loader is
	 * one more for it to answer, and what it found is thrown away. */
	retro_atomic_int_t custom_request_hash;
	retro_atomic_int_t custom_request_old_hash;
	retro_atomic_int_t custom_request_old_vqhash;
	/* What a whole update made the image last, tex_type and all; 0 if
	 * something else has been in it since (a custom texture, a render) */
	u8 made_type;
	bool made_mips;
	/* NeedsUpdate() found nothing changed but some of the pieces */
	bool changed_pieces;

	void PrintTextureName();
	/* what the renderer calls it, into @s (@n bytes) */
	virtual void GetId(char *s, size_t n) = 0;

	bool IsPaletted()
	{
		return tcw.PixelFmt == PixelPal4 || tcw.PixelFmt == PixelPal8;
	}

	bool IsMipmapped()
	{
		// for a paletted texture that bit is not the scan order but part of
		// the palette selection
		return tcw.MipMapped != 0 && (IsPaletted() || tcw.ScanOrder == 0) && !settings.rend.ForceTextureLOD0;
	}

	const char* GetPixelFormatName()
	{
		switch (tcw.PixelFmt)
		{
		case Pixel1555: return "1555";
		case Pixel565: return "565";
		case Pixel4444: return "4444";
		case PixelYUV: return "yuv";
		case PixelBumpMap: return "bumpmap";
		case PixelPal4: return "pal4";
		case PixelPal8: return "pal8";
		default: return "unknown";
		}
	}

	bool IsCustomTextureAvailable()
	{
		return retro_atomic_load_acquire_int(&custom_load_in_progress) == 0 && custom_image_data != NULL;
	}

	void Create();
	void ComputeHash();
	/* The pieces hashed; those that differ from their hashes marked, how
	 * many returned. False if there is no memory for the hashes. */
	bool HashPieces(u32 *changed);
	bool HashChanged();
	bool FromRender(u32 stride, u32 rows);
	void Update();
	bool PlanDecode(texd_plan& plan, u32 stride, u32 rows, u32 out, bool mipmapped);
	/* The regions the changed pieces are in, up to TEXP_REGIONS; 0 if the
	 * texture is to be made whole */
	u32 ChangedRegions(TexRegion *regions, u32 stride, u32 rows, bool mipmapped);
	bool UpdatePart(u32 stride, u32 rows, bool mipmapped, bool need_32bit_buffer);
	void *ConvertInto(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps, bool direct);
	virtual void UploadToGPU(int width, int height, u8 *temp_tex_buffer, bool mipmapped, bool mipmapsIncluded = false) = 0;
	/* Memory for Update() to convert the texture into, which UploadToGPU()
	 * can then take as it stands: @bytes of it, for a texture of @width by
	 * @height in texels of @pixel bytes, with its mipmap levels after one
	 * another from the smallest if @mipmaps. Null if the renderer has none
	 * to give, and the texture is converted into the cache's own memory and
	 * copied by UploadToGPU() as before. What is returned is only good for
	 * the UploadToGPU() that follows. */
	virtual void *UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps) { return NULL; }
	/* The texture made on the GPU instead of converted here, if the
	 * renderer can: @plan says which bytes of video memory to copy and
	 * what to make of them (rend/texdecode.h). What the CPU would have
	 * made, to the bit; tex_type is already what it would have been.
	 * False, and the texture is converted here as before. */
	virtual bool GpuDecode(const texd_plan& plan) { return false; }
	/* Part of the image made again, the rest left as it is: the image has
	 * to be what the last whole update made (made_type), with nothing for
	 * it still to be done. PartMemory(): true if it is, and then *@memory
	 * is where the regions are made (@bytes of it), or null for the
	 * cache's own (false, and the texture is made whole); UploadPart()
	 * takes them from @data, region i at @offset[i], in rows of its width,
	 * and may change them on the way. GpuDecodePart(): the regions made
	 * from @plan on the GPU, as GpuDecode() would make them (TEXP_*). */
	virtual bool PartMemory(size_t bytes, void **memory) { return false; }
	virtual void UploadPart(const TexRegion *regions, u32 count, u8 *data, const size_t *offset) {}
	/* The image made w by h, RGBA8, from what was rendered to @from: its
	 * pixels from (@x, @y) on, the console's, @scale times finer there;
	 * @columns of each row, the rest of the row what zeros are in the
	 * texture's format (ClearOpaque()). False if it cannot be, and nothing
	 * is changed. */
	virtual bool CopyRender(BaseTextureCacheData *from, u32 x, u32 y, u32 scale, u32 columns) { return false; }
	/* The alpha of texels of zeros in this texture's format: opaque in
	 * 565, which has none */
	bool ClearOpaque() const { return tcw.PixelFmt == Pixel565; }
	virtual int GpuDecodePart(const texd_plan& plan, const TexRegion *regions, u32 count) { return TEXP_CPU; }
	virtual bool Force32BitTexture(TextureType type) const { return false; }
	void CheckCustomTexture();
	//true if : dirty or paletted texture and hashes don't match
	bool NeedsUpdate();
	virtual bool Delete();
	virtual ~BaseTextureCacheData() {}
	static bool IsGpuHandledPaletted(TSP tsp, TCW tcw, int area = 0)
	{
		// Some palette textures are handled on the GPU
		// This is currently limited to textures using nearest filtering and not mipmapped.
		// Enabling texture upscaling or dumping also disables this mode.
		// So do custom textures: a replacement is colours, not indices to look up.
		/* And to a polygon's first texture. The shaders have one palette
		 * position to look colours up from, the first texture's: a second
		 * volume's texture left as indices was looked up with that, or, with
		 * a first texture that has no palette, shown as its indices. It is
		 * made colours here, like any texture. */
		return (tcw.PixelFmt == PixelPal4 || tcw.PixelFmt == PixelPal8)
				&& settings.rend.TextureUpscale == 1
				&& !settings.rend.DumpTextures
				&& !settings.rend.CustomTextures
				&& tsp.FilterMode == 0
				&& !tcw.MipMapped
				&& !tcw.VQ_Comp
				&& area == 0;
	}
};

template<typename Texture>
class BaseTextureCache
{
public:
	/* Room for @textures, and their locks on video memory, made now: the
	 * renderer's start (rend/tex_map.h, texcache_reserve()) */
	void Reserve(u32 textures)
	{
		cache.Reserve(textures);
		texcache_reserve(textures);
	}
	/* ...as many as a pool of @images textures' images serves: twice
	 * that, no fewer than 1024 */
	void ReserveFor(u32 images) { Reserve(images * 2 > 1024 ? images * 2 : 1024); }

	Texture *getTextureCacheData(TSP tsp, TCW tcw, int area = 0)
	{
		u64 key = tsp.full & TSPTextureCacheMask.full;
		if ((tcw.PixelFmt == PixelPal4 || tcw.PixelFmt == PixelPal8)
				&& !BaseTextureCacheData::IsGpuHandledPaletted(tsp, tcw, area))
			// Paletted textures have a palette selection that must be part of the key
			// We also add the palette type to the key to avoid thrashing the cache
			// when the palette type is changed. If the palette type is changed back in the future,
			// this texture will stil be available.
			// (bit 9: made colours here, which a texture of the same address left as indices is not)
			key |= ((u64)tcw.full << 32) | ((PAL_RAM_CTRL & 3) << 6) | ((tsp.FilterMode != 0) << 8) | (1 << 9);
		else
			key |= (u64)(tcw.full & TCWTextureCacheMask.full) << 32;

		Texture* texture = cache.Find(key);
		if (texture != nullptr)
			// Needed if the texture is updated
			texture->tcw.StrideSel = tcw.StrideSel;
		else //create if not existing
		{
			texture = cache.Add(key);
			if (texture == nullptr)
				die("Out of memory");

			texture->tsp = tsp;
			texture->tcw = tcw;
			texture->area = (u8)area;
		}

		return texture;
	}

   virtual ~BaseTextureCache() {}

	void CollectCleanup()
	{
		/* (a few a frame, those not used for 120 frames) */
		u64 list[6];
		u32 n = 0;

		u32 TargetFrame = std::max((u32)120, FrameCount) - 120;

		cache.Each([&](u64 key, Texture& texture) {
			if (texture.dirty && texture.dirty < TargetFrame)
				list[n++] = key;
			return n < 6;
		});

		for (u32 i = 0; i < n; i++)
		{
			if (cache.Find(list[i])->Delete())
				cache.Remove(list[i]);
		}
	}

	void Clear()
	{
		/* A texture whose custom image is still being loaded is the
		 * loader's as well as the cache's: the loader has a pointer to it
		 * and writes to it when it is done. Delete() refuses those, and
		 * they were destroyed here all the same. If there is one, the
		 * loader is stopped first, which leaves it with none; it starts
		 * again with the next texture that asks.
		 *
		 * A count at zero is a texture the loader has finished with or
		 * never had: only the renderer raises it from zero, and bringing
		 * it back there is the last thing the loader does to a texture. */
		bool loading = false;
		cache.Each([&](u64, Texture& texture) {
			loading = retro_atomic_load_acquire_int(&texture.custom_load_in_progress) > 0;
			return !loading;
		});
		if (loading)
			texcache_stop_custom_loads();
		cache.Each([](u64, Texture& texture) {
			texture.Delete();
			return true;
		});

		cache.Clear();
		KillTex = false;
		INFO_LOG(RENDERER, "Texture cache cleared\n");
	}

protected:
	TexMap<Texture> cache;
	// Only use TexU and TexV from TSP in the cache key
	//     TexV : 7, TexU : 7
	const TSP TSPTextureCacheMask = { { 7, 7 } };
	//     TexAddr : 0x1FFFFF, Reserved : 0, StrideSel : 0, ScanOrder : 1, PixelFmt : 7, VQ_Comp : 1, MipMapped : 1
	const TCW TCWTextureCacheMask = { { 0x1FFFFF, 0, 0, 1, 7, 1, 1 } };
};

/* The framebuffer the game wrote, as RGBA8: its geometry first, so that a
 * renderer can find room of that size where the card reads it, then
 * converted straight there at that room's pitch. ReadFramebuffer() does
 * both into the texture cache's memory. */
struct FramebufferRead
{
	u32 addr;      /* first texel, in the 32-bit area */
	int width;     /* pixels */
	int height;
	u32 skip;      /* bytes from the end of one row to the next */
	u32 depth;     /* FB_R_CTRL.fb_depth */
	u32 concat;    /* FB_R_CTRL.fb_concat */
};
void ReadFramebufferGeometry(FramebufferRead *fb);
void ReadFramebufferTo(const FramebufferRead *fb, u8 *dst, size_t pitch);
void ReadFramebuffer(PixelBuffer<u32>& pb, int& width, int& height);

static inline void MakeFogTexture(u8 *tex_data)
{
	u8 *fog_table = (u8 *)FOG_TABLE;
	for (int i = 0; i < 128; i++)
	{
		tex_data[i] = fog_table[i * 4];
		tex_data[i + 128] = fog_table[i * 4 + 1];
	}
}
extern const std::array<f32, 16> D_Adjust_LoD_Bias;
