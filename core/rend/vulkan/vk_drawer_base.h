/* What the Vulkan drawers share of a frame's set-up: the base scissor and
 * the flat polygons' provoking vertices */
#ifndef VK_DRAWER_BASE_H
#define VK_DRAWER_BASE_H
#include "vk_context.h"
#include "rend/transform_matrix.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The frame's (pvrrc's) scissor, by its matrices @m: the pixel clip, or
 * the whole screen with widescreen and no clip, or the framebuffer's */
void vk_drawer_base_scissor(const transform_matrix_t *m, VkRect2D *out);
/* Vulkan uses the colour values of the first vertex for flat shaded
 * triangle strips. On Dreamcast the last vertex is the provoking one so it
 * is copied onto the first. */
void vk_drawer_provoking_vertices(void);

#ifdef __cplusplus
}
#endif

#endif
