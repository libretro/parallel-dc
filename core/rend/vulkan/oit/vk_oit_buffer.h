/*
    Created on: Nov 12, 2019

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
/* The per-pixel renderer's fragment buffers: the translucent fragments'
 * lists, the a-buffer of each pixel's first, the count of them, and what
 * the counts say of how many bands a frame is drawn in (rend/oit_bands.h) */
#ifndef VK_OIT_BUFFER_H
#define VK_OIT_BUFFER_H
#include "../vk_image.h"
#include "rend/oit_size.h"
#include "rend/oit_bands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The fragments are spread over this many storage buffers when the device
 * cannot bind them as one (the shaders' PixelBuffer[]) */
#define VK_OIT_BINDINGS 4
/* render passes of a frame whose fragments are counted (the per-pixel drawer's) */
#define VK_OIT_COUNTS 64
/* frame slots counted: the command pool's, which are the swap chain's */
#define VK_OIT_COUNT_SLOTS OIT_BANDS_SLOTS

typedef struct vk_oit_buffers
{
	VkDescriptorSet descSet;
	VkDescriptorSetLayout descSetLayout;
	vk_buffer pixelBuffers[VK_OIT_BINDINGS];
	bool compact;
	VkDeviceSize plannedSize;
	VkDeviceSize bufferSize;
	VkDeviceSize bindingSize;
	u32 bindings;
	u32 capacity;
	vk_buffer pixelCounter;
	vk_buffer pixelCounterReset;
	vk_attachment abufferPointerAttachment;
	vk_attachment sumsAttachment;
	bool sums;
	bool abufferPointerTransitionNeeded;
	bool firstFrameAfterInit;
	int maxWidth;
	int maxHeight;
	/* the fragments each render pass asked for, a frame slot's (read by
	 * the CPU once the slot's fence has passed: vk_oit_buffers_choose_bands()) */
	vk_buffer counts;
	u32 written[VK_OIT_COUNT_SLOTS];
	u32 countSlot;
	oit_bands_t bandState;
} vk_oit_buffers;

void vk_oit_buffers_construct(vk_oit_buffers *o);
/* The fragment buffer for a picture of @width x @height: rend/oit_size.h,
 * no more than the device can bind. One storage buffer is no larger than
 * the device can allocate in one piece, nor than a shader can be given as
 * one (Mesa's software Vulkan stops at 128 MB, and a buffer bound past the
 * limit drew nothing at all). */
VkDeviceSize vk_oit_buffers_planned_size(const vk_oit_buffers *o, int width, int height);
/* The compute renderer draws the screen's frames: the buffer is for the
 * frames it leaves to the per-pixel renderer alone
 * (oit_fallback_buffer_bytes()). Before the renderer is set up, or made
 * again for a size: never while a game runs. */
static INLINE void vk_oit_buffers_set_compact(vk_oit_buffers *o, bool on)
{
	o->compact = on;
	o->plannedSize = 0;
}
/* The screen's size: the fragment buffer is made for it, at the next
 * vk_oit_buffers_init(). Renders to a texture larger than the screen are
 * drawn in more bands rather than the buffer made again while a game runs. */
static INLINE void vk_oit_buffers_set_screen_size(vk_oit_buffers *o, int width, int height)
{
	o->plannedSize = vk_oit_buffers_planned_size(o, width, height);
}
/* For a picture of @width x @height (none: the layout only), what it
 * replaces going when the frames of @pool that use it are done. False if
 * something could not be made. */
bool vk_oit_buffers_init(vk_oit_buffers *o, int width, int height, vk_cmd_pool *pool);
void vk_oit_buffers_on_new_frame(vk_oit_buffers *o, VkCommandBuffer cmd);
void vk_oit_buffers_reset_pixel_counter(vk_oit_buffers *o, VkCommandBuffer cmd);
/* How many fragments the render pass just ended asked for, those that
 * found no room too (the shaders count them all), copied out for the CPU
 * to read when this frame slot comes round again:
 * vk_oit_buffers_choose_bands(). Nothing waits for it. */
void vk_oit_buffers_count_pass(vk_oit_buffers *o, VkCommandBuffer cmd);
/* The bands a frame about to be drawn in frame slot @slot is drawn in, a
 * render to a texture's if @rtt (rend/oit_bands.h), the slot's fence
 * waited for by the command pool: what the frame drawn there before
 * counted goes to the bands of its kind, whichever drawer drew it */
u32 vk_oit_buffers_choose_bands(vk_oit_buffers *o, int slot, bool rtt);
/* ...and the bands this one is drawn in */
static INLINE void vk_oit_buffers_set_bands(vk_oit_buffers *o, u32 bands)
{
	if (o->countSlot < VK_OIT_COUNT_SLOTS)
		o->bandState.drawn_in[o->countSlot] = bands;
}
void vk_oit_buffers_term(vk_oit_buffers *o);
/* (and the layout too) */
void vk_oit_buffers_free(vk_oit_buffers *o);

#ifdef __cplusplus
}
#endif

#endif
