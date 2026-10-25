/*
	The frontend's temporal upscaler (RETRO_ENVIRONMENT_GET_HW_RENDER_UPSCALER_INTERFACE):
	the render size it asks for, the jitter each frame is drawn under, the
	motion vectors a frame's geometry gets, and the frame's inputs handed
	over beside its picture.

	The Dreamcast sends the renderer screen-space vertices and no camera:
	a motion vector cannot be reprojected through a matrix. Instead each
	opaque and punch-through strip of a frame is matched with the strip of
	the frame before that was sent under the same polygon header (its ISP,
	TSP, TCW words and texture) with the same number of vertices, counting
	strips alike from the start of the frame; the vertices of a matched
	strip keep where its vertices were, and the motion vector pass draws
	the difference. A strip without a match, or one that moved further
	than anything on screen moves between two frames, stands still.

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
#pragma once
#include <vector>
#include "types.h"
#include <libretro.h>

struct rend_context;

/* The interface the frontend answered with, or NULL when the context
 * went or it has none: nothing below does anything then */
void upscaler_set_interface(const struct retro_hw_render_upscaler_interface *iface);

/* Once a frame, before it is run: the frontend's request, when the
 * renderer in use draws for the upscaler (@wanted; another's frames are
 * shown as they are). True when the render size it asks for is not the
 * one being drawn at, which the geometry then takes
 * (upscaler_render_size()). */
bool upscaler_poll(bool wanted);
/* ...and whether a request stands: frames are drawn for the upscaler */
bool upscaler_active();
/* The render size the request asks for, 0 x 0 without one */
void upscaler_render_size(int *width, int *height);

/* This frame's jitter, in pixels of the render size: added to where
 * the vertices land (x right, y down), the same for every pass of the
 * frame, and told to the frontend with the frame. */
void upscaler_jitter(float *x, float *y);

/* The motion vector pass's second vertex stream: for each of @ctx's
 * vertices, where it was in the frame before (x, y, in the game's
 * screen space, as Vertex::x and y are). A vertex with no match is
 * where it is now. Remembers this frame's strips for the next. */
void upscaler_match(const rend_context& ctx, std::vector<float>& prev_xy);

/* The frame's inputs, after its picture was handed over: the motion
 * vectors (two floats a pixel, in pixels of the render size, from each
 * pixel to where it was) and the depth the frame left, both the render
 * size, in the API's handles (retro_hw_upscaler_image). @flags are
 * RETRO_HW_UPSCALER_*; the frame's jitter and the reset of a frame that
 * does not follow the one before are added here. Advances the jitter. */
void upscaler_submit(const struct retro_hw_upscaler_image& motion_vectors,
		const struct retro_hw_upscaler_image& depth, unsigned flags);

/* The next frame does not follow this one (a new context, a size
 * change): the upscaler starts over */
void upscaler_reset();
