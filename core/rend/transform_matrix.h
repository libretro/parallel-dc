/*
    Created on: Oct 22, 2019

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
#ifndef REND_TRANSFORM_MATRIX_H
#define REND_TRANSFORM_MATRIX_H
/* The matrices a frame is drawn with: the game's coordinates to clip
 * space (normal), to the scissor rectangles' (scissor), and to pixels
 * (viewport). C89, for C and C++ alike (transform_matrix.c); C++ has
 * TransformMatrix<invertY>. */
#include "xform.h"
#include "hw/pvr/ta_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The picture's scale: the framebuffer's to the screen's (gles.cpp sets it) */
extern float fb_scale_x, fb_scale_y;
extern int screen_width, screen_height;

typedef struct transform_matrix
{
	const rend_context *rc;
	bool invert_y;		/* y up, OpenGL's */
	xform normal;
	xform scissor;
	xform viewport;
	vec2f dc_viewport;
	float scale_x;
	float scale_y;
	float sidebar_width;
} transform_matrix_t;

/* The identity, y down or (@invert_y) up */
void transform_matrix_init(transform_matrix_t *m, bool invert_y);
/* The matrices of the frame of @rc */
void transform_matrix_calc(transform_matrix_t *m, const rend_context *rc);
/* Whether the frame's pixel clip leaves out part of the screen */
bool transform_matrix_clipped(const transform_matrix_t *m);
/* The temporal upscaler's jitter (rend/upscaler.h): the vertices moved by
 * @x, @y pixels of the screen, the scissors not */
void transform_matrix_jitter(transform_matrix_t *m, float x, float y);
/* How many times wider than a render to a texture's target the game draws */
float transform_matrix_rtt_hscale(void);

#ifdef __cplusplus
}

template<bool invertY>
class TransformMatrix
{
public:
	TransformMatrix() { transform_matrix_init(&m, invertY); }
	TransformMatrix(const rend_context& renderingContext)
	{
		transform_matrix_init(&m, invertY);
		CalcMatrices(&renderingContext);
	}

	bool IsClipped() const { return transform_matrix_clipped(&m); }
	const xform& GetNormalMatrix() const { return m.normal; }
	const xform& GetScissorMatrix() const { return m.scissor; }
	const xform& GetViewportMatrix() const { return m.viewport; }
	float GetSidebarWidth() const { return m.sidebar_width; }
	vec2f GetDreamcastViewport() const { return m.dc_viewport; }
	static float RttHorizontalScale() { return transform_matrix_rtt_hscale(); }
	void Jitter(float x, float y) { transform_matrix_jitter(&m, x, y); }
	void CalcMatrices(const rend_context *renderingContext) { transform_matrix_calc(&m, renderingContext); }

private:
	transform_matrix_t m;
};
#endif

#endif
