/* The HDR output with Vulkan: every picture handed to the frontend goes
 * through the Display Transform (rend/hdr/) into an image of the format
 * the frontend took, and that image is handed over in its place. Nothing
 * of it is submitted on its own: it is recorded into the command buffers
 * of the frame, which go in the frame's one submission, under its fence.
 *
 *  - As the last subpass of the frame's render pass (the rasterizers):
 *    the picture read as an input attachment where it was just drawn, the
 *    HDR image the subpass's colour attachment. On a tiler the picture
 *    does not leave the tile for it.
 *  - Otherwise (the compute renderer, a framebuffer the CPU wrote, the
 *    picture kept from another context): one triangle over the picture,
 *    outside a render pass, into an image of the encoder's own for each of
 *    the frontend's frames.
 *
 * What a frame writes - the transform's constants, a descriptor set - is
 * kept by the frame's command pool slot, which the pool waits for before
 * it comes round; what goes is let go of then too. The renderers draw as
 * they always have, and the 24-bit output never comes here. C89. */
#ifndef HDR_ENCODE_VK_H__
#define HDR_ENCODE_VK_H__

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>

#include "vk_heap.h"

#ifdef __cplusplus
extern "C" {
#endif

struct retro_hw_render_interface_vulkan;
struct hdr_output;
typedef struct hdr_vk hdr_vk_t;

/* The shaders' SPIR-V: SSAA_RESOLVE_VERT's triangle, HDR_ENCODE_FRAG
 * (the picture sampled) and HDR_ENCODE_FRAG_SUBPASS (an input attachment) */
typedef struct hdr_vk_code
{
   const uint32_t *vert;
   size_t          vert_size;
   const uint32_t *frag;
   size_t          frag_size;
   const uint32_t *frag_subpass;
   size_t          frag_subpass_size;
} hdr_vk_code_t;

/* @format: what is handed over, VK_FORMAT_A2B10G10R10_UNORM_PACK32 for
 * HDR10 or VK_FORMAT_R16G16B16A16_SFLOAT for scRGB; @slots: the command
 * pools' ring length. NULL when the device cannot draw it. */
hdr_vk_t *hdr_vk_new(const struct retro_hw_render_interface_vulkan *iface,
      vk_heap_t *heap, VkPipelineCache cache, VkFormat format,
      unsigned slots, const hdr_vk_code_t *code);

/* Frees everything: the GPU is to be done with it (the context's Term()
 * waits for the device first). */
void hdr_vk_free(hdr_vk_t *e);

VkFormat hdr_vk_format(const hdr_vk_t *e);

/* The frame's transform, once a command pool frame (@ring the pool, @slot
 * its index, @epoch how many frames it has begun; again with the same
 * epoch does nothing): its constants written, the tables made again when
 * the settings changed (their upload recorded into @cmd, which is outside
 * a render pass), and what the slot let go of last time freed. */
void hdr_vk_prepare(hdr_vk_t *e, const struct hdr_output *out, VkCommandBuffer cmd,
      unsigned ring, unsigned slot, uint64_t epoch);

/* ---- the last subpass of the frame's render pass ---- */

/* A pipeline for subpass @subpass of @render_pass (and those compatible
 * with it), which reads the picture as its input attachment 0 and writes
 * colour attachment 0; the caller's to destroy. VK_NULL_HANDLE when it
 * could not be made. */
VkPipeline hdr_vk_subpass_pipeline(hdr_vk_t *e, VkRenderPass render_pass, uint32_t subpass);

/* Draws it, in that subpass, over @width x @height: the picture is @view,
 * 8 bits a channel when @unorm8. After hdr_vk_prepare() for the same ring
 * and slot. */
void hdr_vk_subpass_draw(hdr_vk_t *e, const struct hdr_output *out, VkCommandBuffer cmd,
      unsigned ring, unsigned slot, VkPipeline pipeline, VkImageView view,
      unsigned width, unsigned height, bool unorm8);

/* ---- a pass of its own ---- */

/* The picture (@image, @view, in the shader-read layout, 8 bits a channel
 * when @unorm8), @width x @height, encoded into @cmd, outside a render
 * pass, into the image for the frontend's frame @index; @image and @view
 * become that image's, in the same layout. Prepares the frame itself.
 * False, both as they were, when it could not be. */
bool hdr_vk_encode(hdr_vk_t *e, const struct hdr_output *out, VkCommandBuffer cmd,
      unsigned ring, unsigned slot, uint64_t epoch, unsigned index,
      VkImage *image, VkImageView *view, unsigned width, unsigned height, bool unorm8);

#ifdef __cplusplus
}
#endif

#endif
