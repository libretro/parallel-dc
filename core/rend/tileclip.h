/*
	Copyright 2020 flyinghead
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
#ifndef REND_TILECLIP_H
#define REND_TILECLIP_H
#include <math.h>
#include <retro_inline.h>
#include "xform.h"

#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/pvr_regs.h"

#define TILECLIP_INSIDE  0	/* Render stuff outside the region */
#define TILECLIP_OFF     1	/* Always passes */
#define TILECLIP_OUTSIDE 2	/* Render stuff inside the region */

/* lroundf()'s: half away from zero */
static INLINE long tileclip_round(float v)
{
	const double d = v;
	return d >= 0 ? (long)floor(d + 0.5) : -(long)floor(-d + 0.5);
}

static INLINE int tileclip_at_least_0(int v)
{
	return v > 0 ? v : 0;
}

/* @clip_rect gets x, y, width, height; TILECLIP_* */
static INLINE int tile_clip(u32 val, const xform *viewport, int *clip_rect)
{
	u32 clipmode = val >> 28;
	int mode;
	float csx, cex, csy, cey;

	if (clipmode < 2)
		return TILECLIP_OFF;	/* always passes */

	if (clipmode & 1)
		mode = TILECLIP_INSIDE;		/* render stuff outside the region */
	else
		mode = TILECLIP_OUTSIDE;	/* render stuff inside the region */

	csx = (float)(val & 63);
	cex = (float)((val >> 6) & 63);
	csy = (float)((val >> 12) & 31);
	cey = (float)((val >> 17) & 31);
	csx = csx * 32;
	cex = cex * 32 + 32;
	csy = csy * 32;
	cey = cey * 32 + 32;

	if (csx <= 0 && csy <= 0 && cex >= 640 && cey >= 480)
		return TILECLIP_OFF;

	if (!pvrrc.isRTT)
	{
		csx = xform_x(viewport, csx);
		csy = xform_y(viewport, csy);
		cey = xform_y(viewport, cey);
		cex = xform_x(viewport, cex);
	}
	else
	{
		/* (across, by half that where the scaler halves what is drawn: transform_matrix.h) */
		const float across = settings.rend.RenderToTextureUpscale / (SCALER_CTL.hscale ? 2.f : 1.f);
		csx *= across;
		csy *= settings.rend.RenderToTextureUpscale;
		cex *= across;
		cey *= settings.rend.RenderToTextureUpscale;
	}
	clip_rect[0] = tileclip_at_least_0((int)tileclip_round(csx));
	clip_rect[1] = tileclip_at_least_0((int)tileclip_round(csy < cey ? csy : cey));
	clip_rect[2] = tileclip_at_least_0((int)tileclip_round(cex - csx));
	clip_rect[3] = tileclip_at_least_0((int)tileclip_round((float)fabs(cey - csy)));

	return mode;
}

/* A polygon kept inside its tile clip rectangle is still kept inside the
 * pixel clip (FB_X_CLIP / FB_Y_CLIP) as well: the chip writes nothing
 * outside that. Both rectangles are in the render target's pixels. */
static INLINE void intersect_clip_rect(int *clip_rect, int x, int y, int width, int height)
{
	const int x0 = clip_rect[0] > x ? clip_rect[0] : x;
	const int y0 = clip_rect[1] > y ? clip_rect[1] : y;
	const int x1 = clip_rect[0] + clip_rect[2] < x + width ? clip_rect[0] + clip_rect[2] : x + width;
	const int y1 = clip_rect[1] + clip_rect[3] < y + height ? clip_rect[1] + clip_rect[3] : y + height;
	clip_rect[0] = x0;
	clip_rect[1] = y0;
	clip_rect[2] = tileclip_at_least_0(x1 - x0);
	clip_rect[3] = tileclip_at_least_0(y1 - y0);
}


#endif
