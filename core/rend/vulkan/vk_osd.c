/*
    Created on: Dec 13, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <string.h>
#include "vk_osd.h"

void vk_osd_construct(vk_osd *o)
{
	unsigned i;
	memset(o, 0, sizeof(*o));
	for (i = 0; i < VK_OSD_VMUS; i++)
		vk_texture_construct(&o->vmu[i]);
	for (i = 0; i < VK_OSD_XHAIRS; i++)
		vk_texture_construct(&o->xhair[i]);
}

bool vk_osd_init(vk_osd *o, vk_quad_pipeline *pipeline)
{
	unsigned i;
	o->pipeline = pipeline;
	for (i = 0; i < VK_OSD_VMUS + VK_OSD_XHAIRS; i++)
		if (!vk_quad_drawer_init(&o->drawers[i], pipeline))
			return false;
	return true;
}

/* @t not shown: its image destroyed */
static void hide(vk_texture *t, bool *shown)
{
	if (!*shown)
		return;
	vk_texture_destroy(t);
	vk_texture_construct(t);
	*shown = false;
}

void vk_osd_term(vk_osd *o)
{
	unsigned i;
	for (i = 0; i < VK_OSD_VMUS; i++)
		hide(&o->vmu[i], &o->vmuShown[i]);
	for (i = 0; i < VK_OSD_XHAIRS; i++)
		hide(&o->xhair[i], &o->xhairShown[i]);
	for (i = 0; i < VK_OSD_VMUS + VK_OSD_XHAIRS; i++)
		vk_quad_drawer_term(&o->drawers[i]);
	o->pipeline = NULL;
}

/* (made straight in the upload ring's piece for it, which the upload then
 * takes as it is; in @fallback if it has no room) */
static u8 *upload_memory(vk_texture *t, u32 width, u32 height, u8 *fallback)
{
	u8 *pixels;
	t->tc.tex_type = TEX_TYPE_8888;
	pixels = (u8 *)vk_texture_upload_memory(t, width, height, (size_t)width * height * 4, 4, false);
	return pixels != NULL ? pixels : fallback;
}

static void upload(vk_texture *t, u32 width, u32 height, u8 *pixels, vk_cmd_pool *pool)
{
	t->commandBuffer = vk_cmd_pool_allocate(pool);
	vk_texture_upload_to_gpu(t, (int)width, (int)height, pixels, false, false);
	t->commandBuffer = VK_NULL_HANDLE;
}

void vk_osd_prepare(vk_osd *o, vk_cmd_pool *pool)
{
	static u8 temp_tex_buffer[VMU_SCREEN_HEIGHT * VMU_SCREEN_WIDTH * 4];
	int i;
	for (i = 0; i < VK_OSD_VMUS; i++)
	{
		vmu_screen_params_t *param = &vmu_screen_params[i];
		const u8 *src;
		u8 *pixels, *dst;
		int x, y;
		/* a new picture from the game, if there is one */
		const bool lcdChanged = vmu_lcd_refresh(i);

		if (!param->vmu_screen_display || vmu_lcd_pixels(i) == NULL)
		{
			hide(&o->vmu[i], &o->vmuShown[i]);
			continue;
		}
		if (!o->vmuShown[i])
			o->vmuShown[i] = true;
		else if (!lcdChanged && !param->vmu_screen_needs_update)
			continue;

		pixels = upload_memory(&o->vmu[i], VMU_SCREEN_WIDTH, VMU_SCREEN_HEIGHT, temp_tex_buffer);
		dst = pixels;
		src = vmu_lcd_pixels(i);
		for (y = 0; y < VMU_SCREEN_HEIGHT; y++)
			for (x = 0; x < VMU_SCREEN_WIDTH; x++)
			{
				if (*src++)
				{
					*dst++ = param->vmu_pixel_on_R;
					*dst++ = param->vmu_pixel_on_G;
					*dst++ = param->vmu_pixel_on_B;
				}
				else
				{
					*dst++ = param->vmu_pixel_off_R;
					*dst++ = param->vmu_pixel_off_G;
					*dst++ = param->vmu_pixel_off_B;
				}
				*dst++ = 0xff;
			}
		upload(&o->vmu[i], VMU_SCREEN_WIDTH, VMU_SCREEN_HEIGHT, pixels, pool);
		param->vmu_screen_needs_update = false;
	}

	for (i = 0; i < VK_OSD_XHAIRS; i++)
	{
		const u8 *colour, *src;
		u8 *pixels, *dst;
		int x, y;
		if (lightgun_params[i].offscreen || lightgun_params[i].colour == 0)
		{
			hide(&o->xhair[i], &o->xhairShown[i]);
			continue;
		}
		if (!o->xhairShown[i])
			o->xhairShown[i] = true;
		else if (!lightgun_params[i].dirty)
			continue;

		/* (as the VMUs') */
		pixels = upload_memory(&o->xhair[i], LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE, temp_tex_buffer);
		dst = pixels;
		colour = &lightgun_palette[lightgun_params[i].colour * 3];
		src = lightgun_img_crosshair;
		for (y = 0; y < LIGHTGUN_CROSSHAIR_SIZE; y++)
			for (x = 0; x < LIGHTGUN_CROSSHAIR_SIZE; x++)
			{
				if (*src++)
				{
					*dst++ = colour[0];
					*dst++ = colour[1];
					*dst++ = colour[2];
					*dst++ = 0xFF;
				}
				else
				{
					*dst++ = 0;
					*dst++ = 0;
					*dst++ = 0;
					*dst++ = 0;
				}
			}
		upload(&o->xhair[i], LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE, pixels, pool);
		lightgun_params[i].dirty = false;
	}
}

static void place(VkCommandBuffer cmd, float x, float y, float w, float h, float alpha)
{
	VkViewport viewport;
	VkRect2D scissor;
	float blendConstants[4];
	viewport.x = x;
	viewport.y = y;
	viewport.width = w;
	viewport.height = h;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 0.f;	/* (as the quads have always been drawn: they have no depth) */
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	scissor.offset.x = (int32_t)x;
	scissor.offset.y = (int32_t)y;
	scissor.extent.width = (u32)w;
	scissor.extent.height = (u32)h;
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	blendConstants[0] = blendConstants[1] = blendConstants[2] = blendConstants[3] = alpha;
	vkCmdSetBlendConstants(cmd, blendConstants);
}

bool vk_osd_draw(vk_osd *o, VkCommandBuffer cmd, u32 width, u32 height, vk_cmd_pool *pool, vk_quad_pipeline *with)
{
	static const QuadVertex vtx[4] = {
		{ { -1.f, -1.f, 0.f }, { 0.f, 1.f } },
		{ {  1.f, -1.f, 0.f }, { 1.f, 1.f } },
		{ { -1.f,  1.f, 0.f }, { 0.f, 0.f } },
		{ {  1.f,  1.f, 0.f }, { 1.f, 0.f } },
	};
	VkPipeline pipeline = vk_quad_pipeline_get(with != NULL ? with : o->pipeline, false);
	int i;
	if (pipeline == VK_NULL_HANDLE)
		return false;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	for (i = 0; i < VK_OSD_VMUS; i++)
	{
		const vmu_screen_params_t *param = &vmu_screen_params[i];
		const float w = (float)(VMU_SCREEN_WIDTH * param->vmu_screen_size_mult);
		const float h = (float)(VMU_SCREEN_HEIGHT * param->vmu_screen_size_mult);
		float x = 0, y = 0;
		if (!o->vmuShown[i])
			continue;
		switch (param->vmu_screen_position)
		{
		case UPPER_LEFT:
			break;
		case UPPER_RIGHT:
			x = width - w;
			break;
		case LOWER_LEFT:
			y = height - h;
			break;
		case LOWER_RIGHT:
			x = width - w;
			y = height - h;
			break;
		}
		place(cmd, x, y, w, h, param->vmu_screen_opacity / 255.f);
		vk_quad_drawer_draw(&o->drawers[i], cmd, vk_texture_view(&o->vmu[i]), vtx, true, pool);
	}

	pipeline = vk_quad_pipeline_get(o->pipeline, true);
	if (pipeline == VK_NULL_HANDLE)
		return false;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	for (i = 0; i < VK_OSD_XHAIRS; i++)
	{
		const float w = LIGHTGUN_CROSSHAIR_SIZE, h = LIGHTGUN_CROSSHAIR_SIZE;
		/* (the game's 640x480 is in the middle of a widescreen picture) */
		const float sidebar = (width - height * 4.f / 3.f) / 2;
		if (!o->xhairShown[i])
			continue;
		place(cmd, sidebar + lightgun_params[i].x * height / 480 - w / 2, lightgun_params[i].y * height / 480 - h / 2, w, h, 1.f);
		vk_quad_drawer_draw(&o->drawers[VK_OSD_VMUS + i], cmd, vk_texture_view(&o->xhair[i]), vtx, true, pool);
	}
	return true;
}

bool vk_osd_drawing(const vk_osd *o)
{
	unsigned i;
	for (i = 0; i < VK_OSD_VMUS; i++)
		if (o->vmuShown[i])
			return true;
	for (i = 0; i < VK_OSD_XHAIRS; i++)
		if (o->xhairShown[i])
			return true;
	return false;
}
