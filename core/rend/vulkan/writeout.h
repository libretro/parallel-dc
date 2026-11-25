/* The chip's write-out (rend/tile/pvr_scaler.h: the Y filter, then the
 * packing to a framebuffer of 16 bits a pixel and its dither) for the
 * Vulkan raster renderers' pictures, as the core option
 * reicast_pvr2_scaler asks: the compute renderer's own pass over its
 * picture (pvr_compute.h's PVRC_K_YFILTER, the same SPIR-V), run in place
 * on the raster renderer's after the frame's render pass. Its pipeline is
 * asked of the pipeline thread (pso.h) by the first vk_writeout_init(), as the
 * renderer starts, whether the option is on or not, and kept: a change of
 * size (a change of the option is one) makes only the pictures' sets,
 * slots and framebuffers again. Nothing is made in a frame, and a frame
 * without a write-out records nothing. What goes over
 * the picture (the VMUs, the crosshairs) is drawn after it, as on the
 * console: the frame's render pass is ended, the pass run, and they are
 * drawn in a render pass of their own that loads the picture
 * (vk_writeout_begin_overlay()), made in vk_writeout_init() too. */
#ifndef VK_WRITEOUT_H
#define VK_WRITEOUT_H

#include "vk_context.h"
#include "vk_buffer.h"
#include "pso.h"
#include "rend/tile/compute_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* (vk_compute_drawer.c has the kernels' SPIR-V) */
const u32 *vk_compute_kernel(u32 kernel, size_t *size);

typedef struct vk_writeout
{
	vk_pso_builder builder;
	VkDescriptorSetLayout setLayout;
	VkPipelineLayout pipelineLayout;
	VkShaderModule module;
	PsoSlot slot;
	/* what the pictures of a size have */
	VkDescriptorPool pool;
	VkDescriptorSet *sets;
	u32 count;
	vk_buffer words;
	u32 *mapped;
	VkRenderPass overlayPass;
	VkFramebuffer *overlayFramebuffers;
	VkExtent2D extent;
} vk_writeout;

/* whether the pictures are to be made for it (VK_IMAGE_USAGE_STORAGE_BIT) */
static INLINE bool vk_writeout_wanted(void)
{
	return !compute_frame_scalers_off;
}

void vk_writeout_construct(vk_writeout *w);
/* the @count pictures @views, @width x @height: a set, a slot of words and
 * a framebuffer of the overlay's for each. False if one could not be made. */
bool vk_writeout_init(vk_writeout *w, const VkImageView *views, u32 count, u32 width, u32 height);
/* everything, the pipeline too (the renderer's end) */
void vk_writeout_term(vk_writeout *w);

/* whether the frame in the registers has a write-out, to be recorded */
static INLINE bool vk_writeout_runs(const vk_writeout *w, u32 height)
{
	return w->count != 0 && PVRC_FF_YLINES(compute_frame_write_out(height)) != 0;
}

/* the overlay's render pass begun on picture @index, after record() */
void vk_writeout_begin_overlay(vk_writeout *w, VkCommandBuffer cmd, u32 index);
/* The pass over picture @index, @image, @width x @height, which the
 * render pass left in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, and leaves
 * so; nothing when the frame in the registers has no write-out */
void vk_writeout_record(vk_writeout *w, VkCommandBuffer cmd, u32 index, VkImage image, VkImageView view, u32 width, u32 height);

#ifdef __cplusplus
}
#endif

#endif
