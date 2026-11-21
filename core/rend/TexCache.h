#ifndef TEXCACHE_H
#define TEXCACHE_H
/* The texture cache (TexCache.c): a texture's state, how it is made from
 * video memory and watched for changes, and the framebuffer read. C89, for
 * C and C++ alike. A renderer's texture is a texcache_data with its own
 * calls (texcache_ops); C++ has it as a class to derive from
 * (TexCacheImpl<>), and the cache of them (BaseTextureCache<>). */
#include "types.h"
#include "oslib/oslib.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/ta_words.h"

#include <retro_atomic.h>
#include "rend/texconv.h"
#include "rend/vram_lock.h"
#include "rend/custom_texture.h"
#include "rend/tex_map.h"

#ifdef __cplusplus
#include <algorithm>
#include <array>
#include <memory>
#include <unordered_map>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Stops the custom texture loader, which then has no texture: see BaseTextureCache::Clear() */
void texcache_stop_custom_loads(void);
/* Room for the locks on video memory of @textures textures, made now, so
 * that locking one takes none: more is made if it runs out, and that is
 * said */
void texcache_reserve(u32 textures);
/* The memory a texture converted on the CPU is converted into, where the
 * renderer has none of its own for it, as large as the largest: made when
 * the cache's room is with GPU Texture Decoding off, and when that is
 * turned off (libretro.c's too) */
void texcache_scratch_largest(void);
/* That memory, @size bytes of it or more: kept from one texture to the
 * next, only ever grown. Render thread only. */
void *texcache_scratch(size_t size);
/* How many times textures are scaled up (xBRZ), 1 for not: for the pools'
 * plans (TexPoolBook::Plan()) */
static INLINE u32 texcache_upscale_factor(void)
{
#ifdef HAVE_TEXUPSCALE
	return settings.rend.TextureUpscale > 1 ? (u32)settings.rend.TextureUpscale : 1;
#else
	return 1;
#endif
}

#ifdef HAVE_TEXUPSCALE
/* xBRZ, @factor times, over the rows of @source, shared out to the
 * cache's threads */
void UpscalexBRZ(int factor, u32 *source, u32 *dest, int width, int height, bool has_alpha);
/* ...to @pool's (band_pool.h; NULL: here) (texupscale.cpp) */
struct band_pool;
void texupscale_run(struct band_pool *pool, int factor, u32 *source, u32 *dest, int width, int height, bool has_alpha);
#endif

/* (each size of texel's: tex_conv_fn, texconv.h) */
typedef tex_conv_fn TexConvFP;
typedef tex_conv_fn TexConvFP8;
typedef tex_conv_fn TexConvFP32;

/* What a texture's texels are made into: TextureType in C++ */
#define TEX_TYPE_565  0
#define TEX_TYPE_5551 1
#define TEX_TYPE_4444 2
#define TEX_TYPE_8888 3
#define TEX_TYPE_8    4
#ifdef __cplusplus
}
enum class TextureType : int { _565, _5551, _4444, _8888, _8 };
extern "C" {
#define TEXCACHE_TYPE TextureType
#else
#define TEXCACHE_TYPE int
#endif

/* After this many updates a texture is treated as frequently updated: its vram
 * is left writable and changes are detected by content hash instead of a write
 * fault (see texcache_update() / texcache_needs_update()). */
#define FREQUENT_UPDATE_COUNT 8

/* Such a texture is hashed by pieces of video memory this size, on their
 * multiples: the pieces that changed are made again, not the texture */
#define TEX_HASH_PIECE 4096

/* A part of a texture's image made again: @w by @h texels from (@x, @y)
 * of the image's level @level, 0 the largest. Multiples of 8. */
typedef struct TexRegion
{
	u32 level;
	u32 x, y;
	u32 w, h;
} TexRegion;

/* The most regions a texture is made again in */
#define TEXP_REGIONS 16

/* What gpu_decode_part did: made the regions; nothing, and the texture is
 * made whole; nothing, as gpu_decode would have done, and the regions
 * are made on the CPU */
enum { TEXP_WHOLE, TEXP_DONE, TEXP_CPU };

struct texd_plan;

/* What a renderer copies for the GPU to make regions of a texture with
 * (tex_plan_part()): a compressed texture's codebook, @codebook bytes
 * from the plan's src, at the start; for region i, @bytes[i] from the
 * plan's src + @from[i], at @at[i]; and at @pal the palette words the plan
 * has. @total bytes in all. The kernel is run over region i with the data
 * of its level of the plan, @level[i], where the copy has it: @data[i]. */
typedef struct TexPartPlan
{
	u32 codebook;
	u32 pal;
	u32 total;
	u32 level[TEXP_REGIONS];
	u32 data[TEXP_REGIONS];
	u32 from[TEXP_REGIONS];
	u32 bytes[TEXP_REGIONS];
	u32 at[TEXP_REGIONS];
} TexPartPlan;
bool tex_plan_part(const struct texd_plan *plan, const TexRegion *regions, u32 count, TexPartPlan *part);

struct PvrTexInfo;
#ifndef TEXCACHE_DATA_T
#define TEXCACHE_DATA_T
typedef struct texcache_data texcache_data;
#endif

/* What a renderer does with its textures */
typedef struct texcache_ops
{
	/* what the renderer calls it, into @s (@n bytes) */
	void (*get_id)(texcache_data *t, char *s, size_t n);
	void (*upload_to_gpu)(texcache_data *t, int width, int height, u8 *data, bool mipmapped, bool mipmaps_included);
	/* Memory for texcache_update() to convert the texture into, which
	 * upload_to_gpu can then take as it stands: @bytes of it, for a
	 * texture of @width by @height in texels of @pixel bytes, with its
	 * mipmap levels after one another from the smallest if @mipmaps. Null
	 * if the renderer has none to give, and the texture is converted into
	 * the cache's own memory and copied by upload_to_gpu. What is returned
	 * is only good for the upload_to_gpu that follows. */
	void *(*upload_memory)(texcache_data *t, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps);
	/* The texture made on the GPU instead of converted here, if the
	 * renderer can: @plan says which bytes of video memory to copy and
	 * what to make of them (rend/texdecode.h). What the CPU would have
	 * made, to the bit; tex_type is already what it would have been.
	 * False, and the texture is converted here as before. */
	bool (*gpu_decode)(texcache_data *t, const struct texd_plan *plan);
	/* Part of the image made again, the rest left as it is: the image has
	 * to be what the last whole update made (made_type), with nothing for
	 * it still to be done. part_memory: true if it is, and then *@memory
	 * is where the regions are made (@bytes of it), or null for the
	 * cache's own (false, and the texture is made whole); upload_part
	 * takes them from @data, region i at @offset[i], in rows of its width,
	 * and may change them on the way. gpu_decode_part: the regions made
	 * from @plan on the GPU, as gpu_decode would make them (TEXP_*). */
	bool (*part_memory)(texcache_data *t, size_t bytes, void **memory);
	void (*upload_part)(texcache_data *t, const TexRegion *regions, u32 count, u8 *data, const size_t *offset);
	int (*gpu_decode_part)(texcache_data *t, const struct texd_plan *plan, const TexRegion *regions, u32 count);
	/* The image made w by h, RGBA8, from what was rendered to @from: its
	 * pixels from (@x, @y) on, the console's, @scale times finer there;
	 * @columns of each row, the rest of the row what zeros are in the
	 * texture's format (texcache_clear_opaque()). False if it cannot be,
	 * and nothing is changed. */
	bool (*copy_render)(texcache_data *t, texcache_data *from, u32 x, u32 y, u32 scale, u32 columns);
	/* the texture made 32-bit, whatever its type (TEX_TYPE_*) */
	bool (*force_32bit)(const texcache_data *t, int type);
	/* What it holds given back, the cache's (texcache_delete()) and the
	 * renderer's; false while the custom texture loader has it */
	bool (*del)(texcache_data *t);
} texcache_ops;

struct texcache_data
{
	const texcache_ops *ops;

	TA_WORD(TSP) tsp;	/* the Dreamcast's texture parameters */
	TA_WORD(TCW) tcw;

	/* what the texels are made into */
	TEXCACHE_TYPE tex_type;

	u32 sa;         /* pixel data start address in vram (might be offset for mipmaps/etc) */
	u32 sa_tex;     /* texture data start address in vram */
	u32 w, h;       /* width & height of the texture */
	u32 size;       /* size, in bytes, in vram */
	/* The row length a planar texture was last read from video memory
	 * with; 0 if it is not planar or was not read (one rendered to). The
	 * stride register is not part of the texture's key: a change to it
	 * alone has the texture read again. */
	u32 stride_read;

	const struct PvrTexInfo *tex;
	TexConvFP *texconv;
	TexConvFP32 *texconv32;
	TexConvFP8 *texconv8;

	u32 dirty;
	vram_block *lock_block;

	u32 Updates;
	/* For a texture updated often, watched by its hashes instead of
	 * write-protection: a hash for each of the pieces of video memory
	 * (TEX_HASH_PIECE) the locked range is on, from the one at @piece_first
	 * on, @pieces of them, and after them a bit for each that changed */
	u32 *piece_hash;
	u32 piece_first;
	u32 pieces;
	/* The render the hashes were last found to hold in, and vram_write_epoch
	 * then: the texture is not hashed again until one of them moves. */
	u32 hash_frame;
	u32 hash_epoch;

	u32 palette_index;
	u8 area;		/* 1 if it was first asked for as a second volume's texture: see tex_gpu_paletted() */
	/* used for palette updates */
	u32 palette_hash;	/* palette hash at time of last update */
	u32 vq_codebook;	/* VQ quantizers table for compressed textures */
	u32 texture_hash;	/* xxhash of texture data, used for custom textures */
	u32 old_texture_hash;	/* legacy hash */
	u32 old_vqtexture_hash;	/* the name a compressed texture had while only part of it was hashed; 0 if not compressed */
	custom_request_t custom;	/* its custom picture, asked for and loaded (custom_texture.h) */
	/* What a whole update made the image last, tex_type and all; 0 if
	 * something else has been in it since (a custom texture, a render) */
	u8 made_type;
	bool made_mips;
	/* texcache_needs_update() found nothing changed but some of the pieces */
	bool changed_pieces;
};

static INLINE bool texcache_is_paletted(const texcache_data *t)
{
	const u32 fmt = TAW_GET(TAW_FULL(t->tcw), TCW_PixelFmt);
	return fmt == TA_PIXEL_4BPP || fmt == TA_PIXEL_8BPP;
}

static INLINE bool texcache_is_mipmapped(const texcache_data *t)
{
	/* for a paletted texture that bit is not the scan order but part of
	 * the palette selection */
	return TAW_GET(TAW_FULL(t->tcw), TCW_MipMapped) != 0
			&& (texcache_is_paletted(t) || TAW_GET(TAW_FULL(t->tcw), TCW_ScanOrder) == 0)
			&& !settings.rend.ForceTextureLOD0;
}

static INLINE const char *texcache_pixel_format_name(const texcache_data *t)
{
	switch (TAW_GET(TAW_FULL(t->tcw), TCW_PixelFmt))
	{
	case TA_PIXEL_1555: return "1555";
	case TA_PIXEL_565: return "565";
	case TA_PIXEL_4444: return "4444";
	case TA_PIXEL_YUV422: return "yuv";
	case TA_PIXEL_BUMPMAP: return "bumpmap";
	case TA_PIXEL_4BPP: return "pal4";
	case TA_PIXEL_8BPP: return "pal8";
	default: return "unknown";
	}
}

static INLINE bool texcache_custom_available(const texcache_data *t)
{
	return retro_atomic_load_acquire_int((retro_atomic_int_t *)&t->custom.in_progress) == 0 && t->custom.image_data != NULL;
}

/* The alpha of texels of zeros in this texture's format: opaque in
 * 565, which has none */
static INLINE bool texcache_clear_opaque(const texcache_data *t)
{
	return TAW_GET(TAW_FULL(t->tcw), TCW_PixelFmt) == TA_PIXEL_565;
}

void texcache_print_name(texcache_data *t);
/* Made what its words say it is (a new texture, or its words changed) */
void texcache_create(texcache_data *t);
void texcache_compute_hash(texcache_data *t);
/* The pieces hashed; those that differ from their hashes marked, how
 * many returned. False if there is no memory for the hashes. */
bool texcache_hash_pieces(texcache_data *t, u32 *changed);
bool texcache_hash_changed(texcache_data *t);
bool texcache_from_render(texcache_data *t, u32 stride, u32 rows);
/* Made again from video memory, or its custom picture asked for */
void texcache_update(texcache_data *t);
/* What gpu_decode is handed: the texture's bytes and, for each level,
 * what texcache_update() would convert it with - the same addresses,
 * sizes and converter, as rend/texdecode.h has them. @out is TEXD_OUT8,
 * 16 or 32, the converter it would take; @stride and @rows what it would
 * read a planar texture with. False if there is nothing the kernel does
 * for this texture. */
bool texcache_plan_decode(texcache_data *t, struct texd_plan *plan, u32 stride, u32 rows, u32 out, bool mipmapped);
/* The regions the changed pieces are in, up to TEXP_REGIONS; 0 if the
 * texture is to be made whole */
u32 texcache_changed_regions(texcache_data *t, TexRegion *regions, u32 stride, u32 rows, bool mipmapped);
bool texcache_update_part(texcache_data *t, u32 stride, u32 rows, bool mipmapped, bool need_32bit_buffer);
void *texcache_convert_into(texcache_data *t, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps, bool direct);
/* Its custom picture uploaded, if it has come */
void texcache_check_custom(texcache_data *t);
/* True if dirty, or a paletted texture whose palette's hash changed */
bool texcache_needs_update(texcache_data *t);
/* What it holds of the cache's given back; false while the custom
 * texture loader has it, and nothing is */
bool texcache_delete(texcache_data *t);

void libCore_vramlock_Lock(u32 start_offset, u32 end_offset, texcache_data *texture);

/* ---- A renderer's textures by their key: a tex_map (rend/tex_map.h) of
 * its textures, each starting with its texcache_data ---- */

/* The key of the texture of @tsp, @tcw (and @area: see tex_gpu_paletted()) */
u64 texcache_key(u32 tsp, u32 tcw, int area);
/* Room for @textures, and their locks on video memory, made now: the
 * renderer's start (texcache_reserve()) */
void texcache_map_reserve(tex_map_t *m, u32 textures);
/* ...as many as a pool of @images textures' images serves: twice that, no
 * fewer than 1024 */
void texcache_map_reserve_for(tex_map_t *m, u32 images);
/* The texture of @tsp, @tcw, made (with @ops, NULL: the one the element's
 * own making gave it) if there is none */
texcache_data *texcache_map_get(tex_map_t *m, const texcache_ops *ops, u32 tsp, u32 tcw, int area);
/* A few a frame of those not used for 120 frames, deleted */
void texcache_map_collect(tex_map_t *m);
/* Every one deleted, the custom texture loader stopped first if it has one */
void texcache_map_clear(tex_map_t *m);

/* The framebuffer the game wrote, as RGBA8: its geometry first, so that a
 * renderer can find room of that size where the card reads it, then
 * converted straight there at that room's pitch. ReadFramebuffer() does
 * both into the texture cache's memory. */
typedef struct FramebufferRead
{
	u32 addr;      /* first texel, in the 32-bit area */
	int width;     /* pixels */
	int height;
	u32 skip;      /* bytes from the end of one row to the next */
	u32 depth;     /* FB_R_CTRL.fb_depth */
	u32 concat;    /* FB_R_CTRL.fb_concat */
} FramebufferRead;
void ReadFramebufferGeometry(FramebufferRead *fb);
void ReadFramebufferTo(const FramebufferRead *fb, u8 *dst, size_t pitch);

static INLINE void MakeFogTexture(u8 *tex_data)
{
	make_fog_texture(tex_data);
}
/* Rough approximation of LoD bias from D adjust param, only used to increase LoD */
extern const f32 D_Adjust_LoD_Bias[16];

#ifdef __cplusplus
}

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

/* For a renderer with nowhere of its own to convert to: where textures are converted */
static inline void ReadFramebuffer(PixelBuffer<u32>& pb, int& width, int& height)
{
	FramebufferRead fb;
	ReadFramebufferGeometry(&fb);
	width = fb.width;
	height = fb.height;
	pb.init(width, height, false, texcache_scratch(PixelBuffer<u32>::bytes(width, height, false)));
	ReadFramebufferTo(&fb, (u8 *)pb.data(), (size_t)width * 4);
}

typedef struct texd_plan texd_plan;

/* A renderer's texture, as C++ has it: texcache_data, its calls the
 * cache's, and the defaults of the renderer's own (TexCacheImpl<>) */
struct BaseTextureCacheData : public texcache_data
{
	BaseTextureCacheData()
	{
		ops = NULL;
		piece_hash = NULL;
		piece_first = 0;
		pieces = 0;
		area = 0;
	}

	void PrintTextureName() { texcache_print_name(this); }
	bool IsPaletted() const { return texcache_is_paletted(this); }
	bool IsMipmapped() const { return texcache_is_mipmapped(this); }
	const char *GetPixelFormatName() const { return texcache_pixel_format_name(this); }
	bool IsCustomTextureAvailable() const { return texcache_custom_available(this); }
	void Create() { texcache_create(this); }
	void ComputeHash() { texcache_compute_hash(this); }
	bool HashPieces(u32 *changed) { return texcache_hash_pieces(this, changed); }
	bool HashChanged() { return texcache_hash_changed(this); }
	bool FromRender(u32 stride, u32 rows) { return texcache_from_render(this, stride, rows); }
	void Update() { texcache_update(this); }
	bool PlanDecode(texd_plan& plan, u32 stride, u32 rows, u32 out, bool mipmapped)
	{
		return texcache_plan_decode(this, &plan, stride, rows, out, mipmapped);
	}
	u32 ChangedRegions(TexRegion *regions, u32 stride, u32 rows, bool mipmapped)
	{
		return texcache_changed_regions(this, regions, stride, rows, mipmapped);
	}
	bool UpdatePart(u32 stride, u32 rows, bool mipmapped, bool need_32bit_buffer)
	{
		return texcache_update_part(this, stride, rows, mipmapped, need_32bit_buffer);
	}
	void *ConvertInto(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps, bool direct)
	{
		return texcache_convert_into(this, width, height, bytes, pixel, mipmaps, direct);
	}
	bool ClearOpaque() const { return texcache_clear_opaque(this); }
	void CheckCustomTexture() { texcache_check_custom(this); }
	bool NeedsUpdate() { return texcache_needs_update(this); }
	bool Delete() { return texcache_delete(this); }
	static bool IsGpuHandledPaletted(TSP tsp, TCW tcw, int area = 0)
	{
		return tex_gpu_paletted(tsp.full, tcw.full, area);
	}

	/* What a renderer's texture has unless it has its own (TexCacheImpl<>) */
	void *UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps) { return NULL; }
	bool GpuDecode(const texd_plan& plan) { return false; }
	bool PartMemory(size_t bytes, void **memory) { return false; }
	void UploadPart(const TexRegion *regions, u32 count, u8 *data, const size_t *offset) {}
	bool CopyRender(BaseTextureCacheData *from, u32 x, u32 y, u32 scale, u32 columns) { return false; }
	int GpuDecodePart(const texd_plan& plan, const TexRegion *regions, u32 count) { return TEXP_CPU; }
	bool Force32BitTexture(TextureType type) const { return false; }
};

/* A renderer's texture class D: its calls the cache's (texcache_ops),
 * D's own where it has them, BaseTextureCacheData's where it has not */
template<class D>
struct TexCacheImpl : public BaseTextureCacheData
{
	TexCacheImpl() { ops = &table; }

private:
	static D *self(texcache_data *t) { return static_cast<D *>(static_cast<BaseTextureCacheData *>(t)); }
	static const D *self(const texcache_data *t) { return static_cast<const D *>(static_cast<const BaseTextureCacheData *>(t)); }
	static void get_id(texcache_data *t, char *s, size_t n) { self(t)->GetId(s, n); }
	static void upload_to_gpu(texcache_data *t, int width, int height, u8 *data, bool mipmapped, bool mipmaps_included)
	{
		self(t)->UploadToGPU(width, height, data, mipmapped, mipmaps_included);
	}
	static void *upload_memory(texcache_data *t, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
	{
		return self(t)->UploadMemory(width, height, bytes, pixel, mipmaps);
	}
	static bool gpu_decode(texcache_data *t, const struct texd_plan *plan) { return self(t)->GpuDecode(*plan); }
	static bool part_memory(texcache_data *t, size_t bytes, void **memory) { return self(t)->PartMemory(bytes, memory); }
	static void upload_part(texcache_data *t, const TexRegion *regions, u32 count, u8 *data, const size_t *offset)
	{
		self(t)->UploadPart(regions, count, data, offset);
	}
	static int gpu_decode_part(texcache_data *t, const struct texd_plan *plan, const TexRegion *regions, u32 count)
	{
		return self(t)->GpuDecodePart(*plan, regions, count);
	}
	static bool copy_render(texcache_data *t, texcache_data *from, u32 x, u32 y, u32 scale, u32 columns)
	{
		return self(t)->CopyRender(static_cast<BaseTextureCacheData *>(from), x, y, scale, columns);
	}
	static bool force_32bit(const texcache_data *t, int type) { return self(t)->Force32BitTexture((TextureType)type); }
	static bool del(texcache_data *t) { return self(t)->Delete(); }
	static const texcache_ops table;
};

template<class D>
const texcache_ops TexCacheImpl<D>::table = {
	TexCacheImpl<D>::get_id, TexCacheImpl<D>::upload_to_gpu, TexCacheImpl<D>::upload_memory,
	TexCacheImpl<D>::gpu_decode, TexCacheImpl<D>::part_memory, TexCacheImpl<D>::upload_part,
	TexCacheImpl<D>::gpu_decode_part, TexCacheImpl<D>::copy_render, TexCacheImpl<D>::force_32bit,
	TexCacheImpl<D>::del
};

template<typename Texture>
class BaseTextureCache
{
public:
	/* Room for @textures, and their locks on video memory, made now: the
	 * renderer's start (rend/tex_map.h, texcache_reserve()) */
	void Reserve(u32 textures) { texcache_map_reserve(cache.Map(), textures); }
	/* ...as many as a pool of @images textures' images serves: twice
	 * that, no fewer than 1024 */
	void ReserveFor(u32 images) { texcache_map_reserve_for(cache.Map(), images); }

	/* (texcache_data first in each: where the map's element starts) */
	Texture *getTextureCacheData(TSP tsp, TCW tcw, int area = 0)
	{
		return static_cast<Texture *>(static_cast<BaseTextureCacheData *>(
				texcache_map_get(cache.Map(), NULL, tsp.full, tcw.full, area)));
	}

	virtual ~BaseTextureCache() {}

	void CollectCleanup() { texcache_map_collect(cache.Map()); }
	void Clear() { texcache_map_clear(cache.Map()); }

protected:
	TexMap<Texture> cache;
};
#endif

#endif
