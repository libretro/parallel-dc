/* The Vulkan renderers' textures: those of the texture cache (rend/TexCache.h)
 * and the renderers' own (the frontend's picture, the fog and palette
 * tables, the VMUs), and the cache of them. */
#ifndef VK_TEXTURE_H
#define VK_TEXTURE_H

#include "vk_context.h"
#include "vk_image.h"
#include "vk_tex_decoder.h"
#include "rend/TexCache.h"
#include "rend/tex_map.h"
#include "rend/grow_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* For the texture debug log (settings.rend.TextureDebug): how the image
 * was last written */
enum { VK_TEX_DBG_NONE, VK_TEX_DBG_STAGED, VK_TEX_DBG_LINEAR, VK_TEX_DBG_KERNEL, VK_TEX_DBG_COPY, VK_TEX_DBG_TARGET };

typedef struct vk_texture
{
	texcache_data tc;	/* (first: the cache's) */
	VkFormat format;
	VkExtent2D extent;
	u32 mipmapLevels;
	bool needsStaging;
	/* handed to the frontend as its frame: copied from as well (a frontend may read it back) */
	bool presented;
	/* what the image was made to be used for */
	VkImageUsageFlags imageUsage;
	/* (the last render's pixels, width << 16 | height, outside which the
	 * image is black; 0 once anything else writes it: vk_texture_clear_outside()) */
	u32 rttArea;
	/* the piece of the upload ring the texture was converted into, between
	 * upload_memory and the upload_to_gpu that follows; and whether the
	 * image was made anew for it */
	u8 *slot;
	VkBuffer slotBuffer;
	VkDeviceSize slotOffset;
	/* the piece part_memory handed out, for the upload_part that follows */
	u8 *partSlot;
	VkBuffer partBuffer;
	VkDeviceSize partOffset;
	bool prepared;
	bool preparedNew;
	VkCommandBuffer commandBuffer;
	vk_cmd_pool *commandPool;
	/* the image has never been written: it is made from nothing */
	bool fresh;
	/* made for the kernel to write, with a view for each level */
	bool storage;
	VkImageView storageViews[TEXD_MAX_LEVELS];
	int decodeJob;
	u32 decodeSerial;
	/* the debug log's: VK_TEX_DBG_*, and the checksums of what was uploaded,
	 * by image level, where the CPU made it */
	u8 debugPath;
	u8 debugLevels;		/* how many levels debugSum has, from level 0 */
	u32 debugSum[TEXD_MAX_LEVELS];
	vk_pooled_image *pooled;
	/* a texture of the cache's: its images come from the pool (not the
	 * renderer's own pictures, which go when it says) */
	bool cached;
	vk_heap_alloc_t alloc;
	bool allocated;
	VkImage image;
	VkImageView imageView;
	u64 ownSerial;			/* imageView's */
	/* the view the frames in flight sample, kept while a new image is made */
	VkImageView readOnlyImageView;
	/* The frames (the command pool's places, bit by bit) whose in-flight
	 * lists have it: vk_texcache */
	u32 inFlightSlots;
	u64 readOnlySerial;		/* readOnlyImageView's */
} vk_texture;

/* The renderer's decoder and pool, while it has them */
extern vk_tex_decoder *vk_texture_decoder;
extern vk_tex_pool *vk_texture_pool;
/* how many times vk_texture_clear_outside() cleared (said with the renders
 * to textures, tools/vulkan/vk_vram.sh) */
extern u32 vk_texture_outside_cleared;

void vk_texture_construct(vk_texture *t);
/* What it holds given back: its pool image now, its own destroyed */
void vk_texture_destroy(vk_texture *t);

static INLINE bool vk_texture_is_new(const vk_texture *t)
{
	return t->image == VK_NULL_HANDLE && t->pooled == NULL;
}

static INLINE VkImage vk_texture_image(const vk_texture *t)
{
	return t->pooled != NULL ? t->pooled->image : t->image;
}

static INLINE VkImageView vk_texture_view(const vk_texture *t)
{
	return t->pooled != NULL ? t->pooled->view : t->imageView;
}

static INLINE u64 vk_texture_view_serial(const vk_texture *t)
{
	return t->pooled != NULL ? t->pooled->viewSerial : t->ownSerial;
}

static INLINE VkImageView vk_texture_read_only_view(const vk_texture *t)
{
	return t->readOnlyImageView != VK_NULL_HANDLE ? t->readOnlyImageView : vk_texture_view(t);
}

/* Which view vk_texture_read_only_view() is, of every one made: a number
 * no other view ever has, which a handle - made again at the same value
 * once its view is gone - is not. What a descriptor set is kept by. */
static INLINE u64 vk_texture_read_only_serial(const vk_texture *t)
{
	return t->readOnlyImageView != VK_NULL_HANDLE ? t->readOnlySerial : vk_texture_view_serial(t);
}

static INLINE void vk_texture_keep_read_only_view(vk_texture *t)
{
	t->readOnlyImageView = vk_texture_view(t);
	t->readOnlySerial = vk_texture_view_serial(t);
}

void vk_texture_upload_to_gpu(vk_texture *t, int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded);
void *vk_texture_upload_memory(vk_texture *t, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps);
/* The image rendered to, extent by extent, with @usage: the pool's, or one
 * made where the pool has none of that shape. False if none could be. */
bool vk_texture_create_render_target(vk_texture *t, VkImageUsageFlags usage);
/* A render to a texture draws its own pixels, @width by @height from the
 * first (Render To Texture Upscaling's), not the whole image, a power of
 * two: the rest is black, as it was when the render pass cleared the
 * whole image. Said before the render pass, with the image's @layout,
 * which is the one it is in after: cleared when it is not black already -
 * the image new, or written since by another render, or anything else. */
void vk_texture_clear_outside(vk_texture *t, VkCommandBuffer cmd, u32 width, u32 height, VkImageLayout *layout);

/* The cache's textures, and what has to wait for the frames in flight */
typedef struct vk_texcache
{
	tex_map_t map;
	grow_buf_t inFlightLists;	/* vk_texcache_list of vk_texture *, a place each */
	u32 inFlightCount;
	grow_buf_t trashLists;		/* vk_texcache_list of what was retired, a place each */
	u32 trashCount;
	u32 currentIndex;
} vk_texcache;

void vk_texcache_construct(vk_texcache *c);
/* everything destroyed and given back (the renderer's end) */
void vk_texcache_free(vk_texcache *c);
vk_texture *vk_texcache_get(vk_texcache *c, u32 tsp, u32 tcw, int area);
void vk_texcache_reserve_for(vk_texcache *c, u32 images);
/* The frame of the command pool's place @index begins: the textures this
 * place had are in no frame now but those after it, and what it retired
 * is destroyed */
void vk_texcache_set_current_index(vk_texcache *c, int index);
bool vk_texcache_is_in_flight(const vk_texcache *c, const vk_texture *t);
void vk_texcache_set_in_flight(vk_texcache *c, vk_texture *t);
/* Its image retired, to be destroyed once the frames that can use it are done */
void vk_texcache_destroy_later(vk_texcache *c, vk_texture *t);
/* a few a frame of those not used for 120 frames, deleted */
void vk_texcache_cleanup(vk_texcache *c);
/* Empties the cache between two frames. The frames before this one can
 * still be on their way through the GPU, drawing with these textures, so
 * the images are not destroyed here: they are retired, as a texture that
 * is replaced is, and destroyed once those frames are done. */
void vk_texcache_clear_later(vk_texcache *c);
/* Empties the cache and destroys everything now, what was retired too.
 * For when the renderer is going away and nothing is drawing any more. */
void vk_texcache_clear(vk_texcache *c);

#ifdef __cplusplus
}
#endif

#endif
