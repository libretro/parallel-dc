/* What the textures and the drawers' attachments are made of: images and
 * their views, the barrier between two layouts of one, the framebuffer
 * kept while its views are the same, the texture samplers, the upload
 * ring and the pool of texture images. */
#ifndef VK_IMAGE_H
#define VK_IMAGE_H

#include "vk_context.h"
#include "vk_buffer.h"
#include "vk_cmd_pool.h"
#include "vk_tex_decoder.h"
#include "rend/tex_pool.h"
#include "rend/texdecode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* @image from @from to @to, its @levels: the barrier between the two */
void vk_set_image_layout(VkCommandBuffer cmd, VkImage image, VkFormat format, u32 levels, VkImageLayout from, VkImageLayout to);

/* A view's serial, never one before: what a framebuffer or a descriptor set
 * made of views is kept by (a destroyed view's handle can come again) */
u64 vk_new_view_serial(void);

static INLINE bool vk_format_has_stencil(VkFormat format)
{
	return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT;
}

/* An attachment of a drawer's: the image, its memory, its view (and its
 * stencil's, for an input attachment), and a buffer the CPU reads it back
 * through when it is @readable */
typedef struct vk_attachment
{
	VkFormat format;
	VkExtent2D extent;
	vk_buffer staging;
	VkImage image;
	vk_heap_alloc_t alloc;
	bool allocated;
	VkImageView view;
	VkImageView stencilView;
	u64 viewSerial;
} vk_attachment;

/* False if it could not be made (and it is empty) */
bool vk_attachment_init(vk_attachment *a, u32 width, u32 height, VkFormat format, VkImageUsageFlags usage, bool readable,
		VkSampleCountFlagBits samples);
void vk_attachment_free(vk_attachment *a);
/* @a goes once the frames of @pool that may use it are done (now with no
 * pool), and is left empty */
void vk_attachment_defer(vk_cmd_pool *pool, vk_attachment *a);
/* ...and so does @b */
void vk_buffer_defer(vk_cmd_pool *pool, vk_buffer *b);

/* A framebuffer kept while its render pass, size and views are the same
 * (by their serials: vk_new_view_serial()) */
typedef struct vk_kept_framebuffer
{
	VkFramebuffer framebuffer;
	VkRenderPass pass;
	u32 count, width, height;
	u64 serials[3];
} vk_kept_framebuffer;

/* VK_NULL_HANDLE if it could not be made */
VkFramebuffer vk_kept_framebuffer_get(vk_kept_framebuffer *k, VkRenderPass pass, u32 count, const VkImageView *views,
		const u64 *serials, u32 width, u32 height);
void vk_kept_framebuffer_free(vk_kept_framebuffer *k);

/* The texture samplers, by what of a polygon's TSP they depend on */
enum { VK_SAMPLERS = 16 * 2 * 3 * 3 };
#define VK_SAMPLER_TSP_MASK 0x7ef00

typedef struct vk_samplers
{
	VkSampler samplers[VK_SAMPLERS];
} vk_samplers;

void vk_samplers_init(vk_samplers *s);
void vk_samplers_term(vk_samplers *s);
/* (one made now if there was none: the device had no room at init) */
VkSampler vk_samplers_get(vk_samplers *s, u32 tsp);

/* The textures' upload ring: pieces handed out in order, a frame's given
 * back once the command pool's fence says its frame is done */
enum { VK_UPLOAD_RING_FRAMES = 8 };

typedef struct vk_upload_ring
{
	vk_buffer buffer;
	u8 *mapped;
	u64 size;
	u64 head, tail;		/* bytes handed out and given back, ever */
	u64 frameEnd[VK_UPLOAD_RING_FRAMES];	/* where each place's last frame ended, */
	bool frameLive[VK_UPLOAD_RING_FRAMES];	/* while it may be on the GPU */
	u32 current;
	vk_cmd_pool *pool;
	u64 statFull;		/* pieces there was no room for */
	u64 statPolled;		/* frames found done before their turn */
} vk_upload_ring;

/* the one there is */
vk_upload_ring *vk_upload_ring_get(void);
/* False if it could not be made */
bool vk_upload_ring_init(vk_upload_ring *r, size_t bytes, vk_cmd_pool *pool);
void vk_upload_ring_term(vk_upload_ring *r);
void vk_upload_ring_set_current_index(vk_upload_ring *r, int index);
/* @bytes of it, and the buffer and offset they are at; NULL if there is no room */
u8 *vk_upload_ring_allocate(vk_upload_ring *r, size_t bytes, VkBuffer *buffer, VkDeviceSize *offset);

/* An image of the texture pool's: its image, memory and views, made at the
 * start (or late, when the pool had none of its kind) and kept until the
 * renderer goes */
typedef struct vk_pooled_image vk_pooled_image;
struct vk_pooled_image
{
	TexPoolEntry entry;		/* (first: the book's entries are these) */
	VkImage image;
	vk_heap_alloc_t alloc;
	VkImageView view;
	u64 viewSerial;
	VkImageView storageViews[TEXD_MAX_LEVELS];
	VkImageUsageFlags usage;
	VkFormat format;
	u32 levels;
	bool storage;
	vk_pooled_image *all;	/* every image there is, for vk_tex_pool_term() */
};

enum { VK_TEX_POOL_FORMATS = 8, VK_TEX_POOL_TARGET = 7 };

typedef struct vk_tex_pool
{
	tex_pool_book_t book;
	bool ready;
	const vk_tex_decoder *decoder;
	VkFormat formats[VK_TEX_POOL_FORMATS];
	bool storable[VK_TEX_POOL_FORMATS];
	bool usable[VK_TEX_POOL_FORMATS];
	bool targetStorage;
	VkImageUsageFlags targetUsage;	/* of the images rendered to */
	vk_pooled_image *all;
} vk_tex_pool;

void vk_tex_pool_construct(vk_tex_pool *p);
void vk_tex_pool_init(vk_tex_pool *p, const vk_tex_decoder *decoder);
void vk_tex_pool_term(vk_tex_pool *p);
/* NULL if it has none of the kind and cannot make one */
vk_pooled_image *vk_tex_pool_take(vk_tex_pool *p, VkFormat format, u32 width, u32 height, bool mips);
vk_pooled_image *vk_tex_pool_take_target(vk_tex_pool *p, u32 width, u32 height, VkImageUsageFlags usage);

static INLINE void vk_tex_pool_set_target_storage(vk_tex_pool *p, bool on)
{
	if (!p->ready)
		p->targetStorage = on;
}

static INLINE bool vk_tex_pool_targets_storable(const vk_tex_pool *p)
{
	return p->ready && p->targetStorage;
}

static INLINE void vk_tex_pool_give(vk_tex_pool *p, vk_pooled_image *image)
{
	tex_pool_give(&p->book, &image->entry);
}

static INLINE void vk_tex_pool_retire(vk_tex_pool *p, vk_pooled_image *image)
{
	tex_pool_retire(&p->book, &image->entry);
}

static INLINE void vk_tex_pool_frame(vk_tex_pool *p, u32 index)
{
	if (p->ready)
		tex_pool_frame(&p->book, index);
}

#ifdef __cplusplus
}
#endif

#endif
