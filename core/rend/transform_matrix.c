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
/* The renderer works from the registers the frame is rendered with */
#define PVR_REGS_FOR_RENDERER
#include <math.h>
#include "transform_matrix.h"
#include "hw/pvr/pvr_regs.h"

float fb_scale_x, fb_scale_y;

/* roundf(), which C89 has not: half away from zero (exact, in double) */
static float round_away(float x)
{
	const double r = floor(fabs((double)x) + 0.5);
	return (float)(x < 0.f ? -r : r);
}

void transform_matrix_init(transform_matrix_t *m, bool invert_y)
{
	memset(m, 0, sizeof(*m));
	m->invert_y = invert_y;
	xform_set(&m->normal, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f);
	m->scissor = m->normal;
	m->viewport = m->normal;
}

float transform_matrix_rtt_hscale(void)
{
	return SCALER_CTL.hscale ? 2.f : 1.f;
}

bool transform_matrix_clipped(const transform_matrix_t *m)
{
	const rend_context *rc = m->rc;
	return rc->fb_X_CLIP.min != 0
			|| (long)round_away((rc->fb_X_CLIP.max + 1) / m->scale_x) != 640L
			|| rc->fb_Y_CLIP.min != 0
			|| (long)round_away((rc->fb_Y_CLIP.max + 1) / m->scale_y) != 480L;
}

void transform_matrix_jitter(transform_matrix_t *m, float x, float y)
{
	m->normal.tx += 2.f * x / screen_width;
	m->normal.ty += 2.f * y / screen_height * (m->invert_y ? -1.f : 1.f);
	/* (and in pixels, for the compute renderer's frame) */
	m->viewport.tx += x;
	m->viewport.ty += y * (m->invert_y ? -1.f : 1.f);
}

static void framebuffer_scaling(const rend_context *rc, bool scissor, float *scale_x, float *scale_y)
{
	*scale_x = 1.f;
	*scale_y = 1.f;

	if (!rc->isRTT && !rc->isRenderFramebuffer)
	{
		if (!scissor)
		{
			*scale_x = fb_scale_x;
			*scale_y = fb_scale_y;
		}
		if (SCALER_CTL.vscalefactor > 0x400)
		{
			/* Interlace mode A (single framebuffer) (for the clip rectangles
			 * too: they are in the framebuffer's lines, half as many as the
			 * game draws - left as they were, they cut the picture off half
			 * way down) */
			if (SCALER_CTL.interlace == 0)
				*scale_y *= round_away((float)SCALER_CTL.vscalefactor / 0x400);
			else if (SCALER_CTL.interlace == 1 && scissor)
				/* Interlace mode B (alternating framebuffers) */
				*scale_y *= round_away((float)SCALER_CTL.vscalefactor / 0x400);
		}

		/* VO pixel doubling is done after fb rendering/clipping so it
		 * should be used for scissoring as well */
		if (VO_CONTROL.pixel_double && !scissor)
			*scale_x *= 0.5f;

		/* the X Scaler halves the horizontal resolution but before
		 * clipping/scissoring */
		if (SCALER_CTL.hscale)
			*scale_x *= 2.f;
	}
}

void transform_matrix_calc(transform_matrix_t *m, const rend_context *rc)
{
	const float inv = m->invert_y ? -1.f : 1.f;
	xform depth, vp_trans, half;

	m->rc = rc;

	framebuffer_scaling(rc, false, &m->scale_x, &m->scale_y);

	if (rc->isRTT)
	{
		/* The texture's first pixel is the frame's first, where the game's
		 * coordinates start; the pixel clip only says which of them are
		 * written. So the texture goes as far as the clip's last pixel,
		 * from 0. */
		m->dc_viewport.x = rc->fb_X_CLIP.max + 1;
		m->dc_viewport.y = rc->fb_Y_CLIP.max + 1;
		/* The horizontal scaler halves what is drawn on its way to the
		 * framebuffer, for a texture as for the screen: the game draws
		 * twice as wide as the texture is. (The clip registers, which the
		 * texture's size comes from, are in the framebuffer's pixels.)
		 * From upstream. */
		xform_set(&m->normal, 2.0f / (m->dc_viewport.x * transform_matrix_rtt_hscale()), 2.0f / m->dc_viewport.y, 1.f, -1.f, -1.f, 0.f);
		m->scissor = m->normal;
		m->sidebar_width = 0;
	}
	else
	{
		const float startx = 0, starty = 0;
		float scissoring_scale_x, scissoring_scale_y, dc2s_scale_h, x_coef, y_coef;
		xform to_clip;

		m->dc_viewport.x = 640.f * m->scale_x;
		m->dc_viewport.y = 480.f * m->scale_y;

		xform_set(&m->normal, 1.f, 1.f, 1.f, startx, starty, 0.f);
		m->scissor = m->normal;

		framebuffer_scaling(rc, true, &scissoring_scale_x, &scissoring_scale_y);

		dc2s_scale_h = screen_height / 480.0f;
		m->sidebar_width = (screen_width - dc2s_scale_h * 640.0f) / 2;
		x_coef = 2.0f / (screen_width / dc2s_scale_h * m->scale_x);
		y_coef = 2.0f / m->dc_viewport.y * inv;
		/* scaled, then moved to where the picture starts */
		xform_set(&to_clip, x_coef, y_coef, 1.f,
				-1 + 2 * m->sidebar_width / screen_width, m->invert_y ? 1.f : -1.f, 0.f);
		xform_mul(&m->normal, &to_clip, &m->normal);
		xform_set(&to_clip, x_coef * scissoring_scale_x, y_coef * scissoring_scale_y, 1.f,
				-1 + 2 * m->sidebar_width / screen_width, m->invert_y ? 1.f : -1.f, 0.f);
		xform_mul(&m->scissor, &to_clip, &m->scissor);
	}
	xform_set(&depth, 1.f, 1.f, 1 / settings.rend.ExtraDepthScale, 0.f, 0.f, 0.f);
	xform_mul(&m->normal, &depth, &m->normal);

	/* clip space to pixels: moved by 1, then scaled by half the size */
	xform_set(&vp_trans, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f);
	if (rc->isRTT)
		xform_set(&half, m->dc_viewport.x / 2, m->dc_viewport.y / 2, 1.f, 0.f, 0.f, 0.f);
	else
		xform_set(&half, screen_width / 2, screen_height / 2, 1.f, 0.f, 0.f, 0.f);
	xform_mul(&vp_trans, &half, &vp_trans);
	xform_mul(&m->viewport, &vp_trans, &m->normal);
	xform_mul(&m->scissor, &vp_trans, &m->scissor);
}
