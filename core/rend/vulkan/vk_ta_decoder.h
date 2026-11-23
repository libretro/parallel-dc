/* GPU-side TA processing on Vulkan: a frame's vertices, indices and
 * modifier volume triangles made on the GPU from the blocks the game sent
 * (rend/ta_gpu.h), and the frame drawn from them, with nothing read back.
 *
 * - The TA contexts' data are slices of one persistently mapped buffer of
 *   host-cached memory (tactx_gpu_pool_attach()): the SH4 writes a frame
 *   straight into memory the GPU reads, and reads its control words back
 *   from it at the speed of ordinary memory. A slice is written again once
 *   the work of the frame that read it last is done, which is known by
 *   polling the texture command pool's fences, never by waiting.
 * - At the start of a frame the header pass (ta_gpu_prepare()) writes
 *   the frame's tables into this frame's piece of a ring, and makes the
 *   render context's lists of draw groups in the full parse's place; the
 *   decode is recorded into a command buffer of the texture pool's,
 *   submitted with the frame before its draws, which are indirect: their
 *   counts are the GPU's (vk_ta_gpu_draw). A frame the header pass does not do
 *   (ta_gpu_prepare()'s TA_GPU_WHY_*) is parsed and drawn as ever.
 * - The kernels (SPIR-V made when the core is built: rend/ta_gpu_spv.inc),
 *   every buffer (sized for the largest frame the machine has), the
 *   descriptor sets and the timestamp queries are made in vk_ta_decoder_setup(), when
 *   the renderer is set up or the option changes, and the pipelines go
 *   through the context's pipeline cache; nothing is allocated or
 *   compiled while a game runs.
 * - The outputs are on the device, one copy: frames are decoded one after
 *   the other on the one queue, and each decode waits for whatever the one
 *   before is read by.
 *
 * Off (TA processing: CPU), none of this exists. */
#ifndef VK_TA_DECODER_H
#define VK_TA_DECODER_H
#include "vk_cmd_pool.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/ta_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a frame decoded on the GPU is drawn from (rend_context::gpu): the
 * vertex, index and modifier volume triangle buffers; the opaque and
 * punch-through draw groups' commands, at their slot (PolyParam::first);
 * the translucent draw records, each translucent draw group's at its
 * place (first: a count, then at most PolyParam::count draws); and the
 * translucent polygon parameters' words of the per-pixel renderer.
 * drawCount: vkCmdDrawIndexedIndirectCountKHR(), or null, when a group's
 * whole room is drawn (its records past the count draw nothing). */
typedef struct vk_ta_gpu_draw
{
	VkBuffer vertices, indices, modtrig, commands, trDraws, trParams;
	VkDeviceSize trParamsSize;
	/* the frame's words (TAG_O_*): 1/fZ_max at TAG_O_ZSCALE, which the
	 * drawers copy into their vertex uniforms (rend/depth_scale.h) */
	VkBuffer frame;
	PFN_vkCmdDrawIndexedIndirectCountKHR drawCount;
	/* The second commands of the opaque and punch-through draw groups for
	 * shadows (TA_GPU_SHADOWS, TAG_K_SHADOW): a group's at this many bytes
	 * on from its own */
	VkDeviceSize shadowCommands;
	/* (TA_GPU_SORT) this frame's room for the sorted translucent triangles'
	 * indices (ta_gpu_sorted_write()), mapped */
	VkBuffer sorted;
	VkDeviceSize sortedOffset;
	u32 *sortedMap;
} vk_ta_gpu_draw;

struct vk_ta_res;

typedef struct vk_ta_decoder
{
	struct vk_ta_res *res;
	bool ready;
	bool failed;		/* could not be made on this device: not tried again */
	bool instance;		/* the translucent draws can number their strips (firstInstance) */
	bool sortWanted;
	bool sortable;
	bool prepared;
	int frames;
	int index;
	u32 serial;			/* this frame's */
	u32 done;			/* every frame up to this one is done */
	u32 flags;
	TaGpuFrame frame;
	vk_ta_gpu_draw draw;

	/* for measuring */
	u64 statFrames;		/* decoded */
	u64 statSkipped;	/* left to the CPU */
	u64 statWhy[TA_GPU_WHY_COUNT];
	double statGpuMs;	/* GPU time of the decodes timed */
	double statGpuMaxMs;
	u64 statGpuTimed;
	u64 statSorted;		/* decoded with a sorted pass, sorted on this side */
	double statSortUs;	/* ...what the sorts took */
	double statSortMaxUs;
	u64 statShadowed;	/* with draw groups the GPU says take shadows or not */
} vk_ta_decoder;

void vk_ta_decoder_construct(vk_ta_decoder *d);
/* Made, or taken down, as the option says (@wanted), for the texture
 * pool's size: when the renderer is set up (its context made, or its
 * size changed) and when the options change, never while a frame is
 * drawn. Off, or where it cannot be made, nothing of it exists. */
void vk_ta_decoder_setup(vk_ta_decoder *d, vk_cmd_pool *pool, bool wanted);
/* At the start of each frame, after the texture pool's begin_frame();
 * nothing is made here. The GPU's progress is told to the TA contexts'
 * pool here. */
void vk_ta_decoder_begin_frame(vk_ta_decoder *d, vk_cmd_pool *pool);
void vk_ta_decoder_term(vk_ta_decoder *d);
/* The frame's tables, and with TA_GPU_DRAW (in @flags, TA_GPU_*) its
 * render context made ready to draw in the full parse's place; whether
 * it is decoded, and then (@rv) whether it has something to draw */
bool vk_ta_decoder_prepare(vk_ta_decoder *d, TA_context *ctx, u32 flags, bool *rv);
/* ...and its decode recorded; false if a kernel's pipeline could not be
 * made (the command buffer is then left as it is) */
bool vk_ta_decoder_record(vk_ta_decoder *d, TA_context *ctx, vk_cmd_pool *pool);

#ifdef __cplusplus
}
#endif

#endif
