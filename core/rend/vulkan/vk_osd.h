/* The VMU screens and the light guns' crosshairs, drawn over the picture */
#ifndef VK_OSD_H
#define VK_OSD_H

#include "vk_texture.h"
#include "vk_quad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_OSD_VMUS 4
#define VK_OSD_XHAIRS 4

/* (each texture there is made when it is shown, its image destroyed when
 * it is not: the textures themselves are here, none allocated) */
typedef struct vk_osd
{
	vk_texture vmu[VK_OSD_VMUS];
	bool vmuShown[VK_OSD_VMUS];
	vk_texture xhair[VK_OSD_XHAIRS];
	bool xhairShown[VK_OSD_XHAIRS];
	vk_quad_drawer drawers[VK_OSD_VMUS + VK_OSD_XHAIRS];
	vk_quad_pipeline *pipeline;
} vk_osd;

void vk_osd_construct(vk_osd *o);
bool vk_osd_init(vk_osd *o, vk_quad_pipeline *pipeline);
void vk_osd_term(vk_osd *o);
/* The pictures that changed uploaded, in command buffers of @pool's */
void vk_osd_prepare(vk_osd *o, vk_cmd_pool *pool);
/* @pool: the one @cmd is of; @with, a pipeline of another render pass's to
 * draw the VMUs with (the same layout), or NULL. False if a pipeline
 * could not be made. */
bool vk_osd_draw(vk_osd *o, VkCommandBuffer cmd, u32 width, u32 height, vk_cmd_pool *pool, vk_quad_pipeline *with);
/* whether vk_osd_draw() has anything to draw */
bool vk_osd_drawing(const vk_osd *o);

#ifdef __cplusplus
}
#endif

#endif
