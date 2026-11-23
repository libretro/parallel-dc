/* The per-pixel renderer's render passes: three subpasses (four with
 * SSAA), made in each of the kinds a frame drawn in several render passes
 * needs. All of them are compatible with each other: a pipeline made for
 * one works with any. One set for the screen, one for renders to a texture
 * (@rtt: their picture is R8G8B8A8, sampled after). */
#ifndef VK_OIT_RENDERPASS_H
#define VK_OIT_RENDERPASS_H
#include "../vk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The subpasses with SSAA: 0 depth and stencil, 1 colour and the
 * translucent lists, 2 the translucent modifier volumes (and, in a
 * render pass that is not the last, each sample put together, and the
 * translucent depth for the next), 3 the picture put together and
 * resolved. Without, 2 does all that 2 and 3 do. */
#define VK_OIT_SSAA_FINAL_SUBPASS 3

typedef struct vk_oit_passes
{
	VkRenderPass passes[16];
	u32 samples;		/* SSAA's samples a pixel (1: off) */
	bool keepDepth;		/* the last render pass stores the depth (rend/upscaler.h reads it) */
	bool rtt;
} vk_oit_passes;

void vk_oit_passes_construct(vk_oit_passes *p, bool rtt);
/* @depthWrites: the colour subpass may write depth. It reads the depth
 * buffer as an input, so the buffer is normally in a read-only layout
 * there, which forbids writing; translucent polygons that are not
 * sorted are drawn in that subpass and do write. For a frame that has
 * such polygons the buffer is in the general layout instead, which
 * allows both. The two kinds of render pass differ in nothing else and
 * a pipeline made for one works with the other.
 * @resumed: a band of a frame drawn in bands (OITDrawer::Draw()) after
 * the first. The picture's attachment holds the bands drawn before, and
 * is loaded, not cleared and not taken from an undefined layout, which
 * would let the device drop all of it. Again a pipeline made for one
 * render pass works with the other.
 * VK_NULL_HANDLE if it could not be made. */
VkRenderPass vk_oit_passes_get(vk_oit_passes *p, bool initial, bool last, bool depthWrites, bool resumed);
/* Every kind of render pass made now, when the renderer starts or the
 * picture changes size: the ones for drawing in bands (@resumed) are
 * first needed while a game runs, and nothing is made then. False if one
 * could not be. */
bool vk_oit_passes_make_all(vk_oit_passes *p);
void vk_oit_passes_reset(vk_oit_passes *p);
/* the render passes made for another count, or to keep the depth or not,
 * are dropped */
void vk_oit_passes_set_samples(vk_oit_passes *p, u32 samples);
void vk_oit_passes_set_keep_depth(vk_oit_passes *p, bool keep);

/* The layout the depth buffer is in while the colour subpass reads it */
static INLINE VkImageLayout vk_oit_depth_input_layout(bool depthWrites)
{
	return depthWrites ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
}

static INLINE int vk_oit_final_subpass(const vk_oit_passes *p)
{
	return p->samples > 1 ? VK_OIT_SSAA_FINAL_SUBPASS : 2;
}

#ifdef __cplusplus
}
#endif

#endif
