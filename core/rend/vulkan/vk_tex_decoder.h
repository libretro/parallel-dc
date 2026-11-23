/* Textures made on the GPU from their bytes in video memory.
 *
 * The texture cache converts a texture on the CPU, texel by texel, into
 * the upload ring, and the GPU copies the texels to the image. Here the
 * texture's bytes are copied into a ring of their own as they are - the
 * texels, the codebook of a compressed texture, the part of the palette a
 * paletted one uses - and a compute kernel makes the texels from them,
 * straight into the image: rend/texdecode.h, the same text the CPU side
 * is tested against, so the texels are the converters' to the bit. A
 * planar 16-bit texture in a format the image can have its texels in as
 * they are - 565 always, 1555 as A1R5G5B5, 4444 as B4G4R4A4 with its
 * channels swizzled back by the view - is not converted at all: its rows
 * are copied to the image as they are.
 *
 * What a frame asks for is recorded at the end of Process(), before the
 * frame's draws, in one command buffer of the texture pool's: a barrier for
 * all the images, the copies and the dispatches, a barrier for all of them
 * again. Everything is made when the renderer starts and sized then - the
 * pipelines, the ring (from the video memory budget), the descriptor pools
 * and the lists - and nothing is allocated while the game runs. A frame
 * that asks for more than there is room for has the rest converted on the
 * CPU, as before. The ring's piece and the descriptor pool of a frame are
 * used again when that frame's place comes round, which is when the
 * texture command pool has seen its fence (vk_cmd_pool_begin_frame()). */
#ifndef VK_TEX_DECODER_H
#define VK_TEX_DECODER_H

#include "vk_context.h"
#include "vk_buffer.h"
#include "vk_cmd_pool.h"
#include "pso.h"
#include "rend/texdecode.h"
#include "rend/TexCache.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { VK_TEXD_MAX_JOBS = 1024, VK_TEXD_MAX_DISPATCHES = 4096 };

typedef struct vk_texd_level
{
	VkImageView view;		/* the level's storage view */
	texd_u32 pc[6];			/* base (in words), data, pal, wh, sr, mode */
	u32 group[4];			/* the groups of 8x8 texels run: from x, y, so many across, down */
} vk_texd_level;

typedef struct vk_texd_job
{
	VkImage image;
	u32 first, count;		/* dispatches; for a copy, the copy's place in copies */
	u32 levels;
	u32 out;
	bool fresh;
	bool copy;
	bool cancelled;
} vk_texd_job;

typedef struct vk_texd_copy
{
	u32 offset, rowLength, width, height, y;
} vk_texd_copy;

/* (its kernels' pipelines: asked of the pipeline thread by init(), pso.h;
 * the first frame that decodes takes them, or makes them there) */
typedef struct vk_tex_decoder
{
	vk_pso_builder builder;
	bool ready;
	bool flushed;	/* this frame's are recorded: no more until the next */
	int frames;
	int index;
	u32 serial;

	vk_buffer ring;
	u8 *ringMapped;
	u32 ringFrame;		/* bytes for each frame */
	u32 ringUsed;

	bool takes32, takes16[3], takes8;
	VkFormat native[3];		/* by TEX_TYPE_5551, 565, 4444 */

	VkDescriptorSetLayout ringLayout, imageLayout;
	VkPipelineLayout pipelineLayout;
	VkShaderModule modules[3];	/* by TEXD_OUT8, 16, 32 */
	PsoSlot pipelines[3];
	VkDescriptorPool ringPool;
	VkDescriptorSet ringSet;
	VkDescriptorPool *pools;	/* by frame */

	/* sized once, in one allocation */
	void *arena;
	vk_texd_job *jobs;
	vk_texd_level *dispatches;
	vk_texd_copy *copies;
	VkDescriptorSet *sets;		/* frames * VK_TEXD_MAX_DISPATCHES */
	VkDescriptorImageInfo *imageInfos;
	VkWriteDescriptorSet *writes;
	VkImageMemoryBarrier *barriers;
	u32 jobCount, dispatchCount, copyCount;

	/* what has been done so far, for measuring */
	u64 statBytes, statTextures, statDispatches, statCopies;
	u64 statFull;		/* textures that found no room */
} vk_tex_decoder;

void vk_tex_decoder_construct(vk_tex_decoder *d);
/* Everything made, for the swap chain the context has; again if that
 * changed, nothing done otherwise. Nothing is decoded here when the
 * device cannot. */
void vk_tex_decoder_init(vk_tex_decoder *d);
void vk_tex_decoder_term(vk_tex_decoder *d);
/* Whether an image of @format can be made by the kernel */
bool vk_tex_decoder_takes(const vk_tex_decoder *d, VkFormat format);
/* The format a planar 16-bit texture of @type (TEX_TYPE_*) is copied into
 * as it is, or VK_FORMAT_UNDEFINED */
VkFormat vk_tex_decoder_native_format(const vk_tex_decoder *d, int type);
/* Whether this frame has @bytes of ring and @dispatches left */
bool vk_tex_decoder_room(const vk_tex_decoder *d, u32 bytes, u32 dispatches);
void vk_tex_decoder_begin_frame(vk_tex_decoder *d, int index);
/* @bytes of the frame's ring, and where in the ring's buffer they are
 * (a multiple of 16); NULL if there is no room */
u8 *vk_tex_decoder_reserve(vk_tex_decoder *d, u32 bytes, u32 *offset);
/* The texture's image made from the ring: @count levels, of an image of
 * @mipLevels; @fresh if it has never been written. Returns what
 * vk_tex_decoder_cancel() takes. */
int vk_tex_decoder_add_decode(vk_tex_decoder *d, VkImage image, bool fresh, u32 mipLevels, u32 out,
		const vk_texd_level *levels, u32 count);
/* The texture's image copied from the ring: @width by @height texels,
 * rows of @rowLength, from @offset, to its row @y on */
int vk_tex_decoder_add_copy(vk_tex_decoder *d, VkImage image, bool fresh, u32 offset, u32 rowLength,
		u32 width, u32 height, u32 y);
/* Whether a level can be run over some of its groups only */
static INLINE bool vk_tex_decoder_runs_part(const vk_tex_decoder *d)
{
	return d->ready && vkCmdDispatchBase != NULL;
}
/* What was added is not done after all (the texture is made another way,
 * or goes); returns whether its image was fresh */
bool vk_tex_decoder_cancel(vk_tex_decoder *d, int job);
/* What the frame asked for, recorded into a command buffer of @pool's.
 * False if a kernel could not be made (what used it is not drawn). */
bool vk_tex_decoder_flush(vk_tex_decoder *d, vk_cmd_pool *pool);

#ifdef __cplusplus
}
#endif

#endif
