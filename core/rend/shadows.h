/* Shadows in the per-triangle renderers: what both of them need to know
 * about where on the screen the modifier volumes and the polygons are.
 *
 * Inside a modifier volume the PowerVR2 scales what a polygon is shaded
 * with - its base and offset colours - and then combines them with the
 * texture and fogs the result as usual. Darkening the finished pixel, which
 * is all these renderers used to do, is not the same thing: it also darkens
 * a decal texture, which the hardware leaves alone, and the fog.
 *
 * So once the volumes are in the stencil buffer, the polygons that take
 * shadows are drawn a second time, with a shader that scales the two
 * colours, into the pixels that are theirs (same depth) and in a volume
 * (stencil). Only the polygons that reach into the part of the screen the
 * volumes cover are drawn again, and only that part of them: nothing
 * outside it can be in a volume. The pixels drawn this way are marked, in
 * stencil bit 6, and what is not marked is darkened the old way after:
 * polygons that are blended, which cannot be drawn twice, or that left no
 * depth to be found by. */
#ifndef SHADOWS_H
#define SHADOWS_H
/* C89, for C and C++ alike; C++ has its old names as well */
#include <retro_inline.h>
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/ta_words.h"

/* The part of the screen something covers, in the coordinates of the vertices */
typedef struct ScreenBounds ScreenBounds;
struct ScreenBounds
{
	float min_x, min_y, max_x, max_y;
#ifdef __cplusplus
	void add(float x, float y);
	bool empty() const;
	bool overlaps(const ScreenBounds& o) const;
#endif
};

/* (written so that a coordinate that is not a number changes nothing) */
static INLINE void screen_bounds_add(ScreenBounds *b, float x, float y)
{
	if (x < b->min_x) b->min_x = x;
	if (x > b->max_x) b->max_x = x;
	if (y < b->min_y) b->min_y = y;
	if (y > b->max_y) b->max_y = y;
}

static INLINE bool screen_bounds_empty(const ScreenBounds *b)
{
	return !(b->min_x <= b->max_x && b->min_y <= b->max_y);
}

/* Whether a polygon that takes shadows can be drawn a second time for them */
static INLINE bool CanDrawShadowed(u32 listType, const PolyParam *gp)
{
	if (gp->count <= 2 || !TAW_GET(TAW_FULL(gp->pcw), PCW_Shadow))
		return false;
	/* blended with what was there before: cannot be drawn twice */
	if (TAW_GET(TAW_FULL(gp->tsp), TSP_SrcInstr) != 1 || TAW_GET(TAW_FULL(gp->tsp), TSP_DstInstr) != 0)
		return false;
	/* left no depth of its own to be found by */
	if (listType == ListType_Opaque && (TAW_GET(TAW_FULL(gp->isp), ISP_TSP_ZWriteDis) || TAW_GET(TAW_FULL(gp->isp), ISP_TSP_DepthMode) == 0))
		return false;
	return true;
}

#include <math.h>
#include "xform.h"
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define SHADOWS_SSE2
#endif

/* What the modifier volumes of a render pass cover */
static INLINE ScreenBounds shadow_modvol_bounds(int first, int count)
{
	ScreenBounds b;
	const ModifierVolumeParam *params = TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam) + first;
	int i;
	b.min_x = 1e30f;
	b.min_y = 1e30f;
	b.max_x = -1e30f;
	b.max_y = -1e30f;
	for (i = 0; i < count; i++)
	{
		const ModTriangle *t = TA_LIST_HEAD(pvrrc.modtrig, ModTriangle) + params[i].first;
		u32 n;
		for (n = params[i].count; n != 0; n--, t++)
		{
			screen_bounds_add(&b, t->x0, t->y0);
			screen_bounds_add(&b, t->x1, t->y1);
			screen_bounds_add(&b, t->x2, t->y2);
		}
	}
	return b;
}

/* Whether a polygon reaches into the part of the screen the volumes cover:
 * whether the rectangle around its vertices overlaps that one.
 *
 * The rectangle only grows as vertices are added, so once it overlaps it
 * always will: the walk over the vertices stops there, which with volumes
 * that cover much of the picture is early for most polygons. It is looked
 * at every eight vertices. And the two corners are kept as pairs, a
 * minimum and a maximum taken of x and y at once, where the processor has
 * that. (A coordinate that is not a number still changes nothing: the
 * corner is the operand that such a minimum or maximum hands back.) */
static INLINE bool poly_overlaps(const PolyParam *gp, const ScreenBounds *area)
{
	const u32 *idx = TA_LIST_HEAD(pvrrc.idx, u32) + gp->first;
	const Vertex *verts = TA_LIST_HEAD(pvrrc.verts, Vertex);
	u32 n = gp->count;
#ifdef SHADOWS_SSE2
	const __m128 area_min = _mm_setr_ps(area->min_x, area->min_y, 0.f, 0.f);
	const __m128 area_max = _mm_setr_ps(area->max_x, area->max_y, 0.f, 0.f);
	__m128 lo = _mm_set1_ps(1e30f);
	__m128 hi = _mm_set1_ps(-1e30f);

	while (n != 0)
	{
		u32 run = n < 8 ? n : 8;

		n -= run;
		do
		{
			/* x and y, which are next to each other in a vertex (one
			 * 8-byte load, as movq: a vertex's x is 4-byte aligned, which a
			 * double load does not allow) */
			const __m128 xy = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)&verts[*idx++].x));
			lo = _mm_min_ps(xy, lo);
			hi = _mm_max_ps(xy, hi);
		} while (--run != 0);
		if ((_mm_movemask_ps(_mm_and_ps(_mm_cmple_ps(lo, area_max), _mm_cmpge_ps(hi, area_min))) & 3) == 3)
			return true;
	}
	return false;
#else
	ScreenBounds b;
	b.min_x = 1e30f;
	b.min_y = 1e30f;
	b.max_x = -1e30f;
	b.max_y = -1e30f;

	while (n != 0)
	{
		u32 run = n < 8 ? n : 8;

		n -= run;
		do
		{
			screen_bounds_add(&b, verts[*idx].x, verts[*idx].y);
			idx++;
		} while (--run != 0);
		if (b.min_x <= area->max_x && b.max_x >= area->min_x && b.min_y <= area->max_y && b.max_y >= area->min_y)
			return true;
	}
	return false;
#endif
}

/* (huge coordinates are common in volumes: kept where rounding is defined) */
static INLINE float shadow_coord_clamp(float v)
{
	return v < -32768.f ? -32768.f : v > 32768.f ? 32768.f : v;
}

/* Bounds to a scissor rectangle (x, y, width, height), a pixel wider all round */
static INLINE void bounds_to_scissor(const ScreenBounds *b, const xform *viewport, int *rect)
{
	float sx = shadow_coord_clamp(b->min_x - 1.f);
	float sy = shadow_coord_clamp(b->min_y - 1.f);
	float ex = shadow_coord_clamp(b->max_x + 1.f);
	float ey = shadow_coord_clamp(b->max_y + 1.f);
	int x0, y0, x1, y1;

	if (!pvrrc.isRTT)
	{
		sx = xform_x(viewport, sx);
		sy = xform_y(viewport, sy);
		ex = xform_x(viewport, ex);
		ey = xform_y(viewport, ey);
	}
	else
	{
		const float across = settings.rend.RenderToTextureUpscale / (SCALER_CTL.hscale ? 2.f : 1.f);
		sx *= across;
		sy *= settings.rend.RenderToTextureUpscale;
		ex *= across;
		ey *= settings.rend.RenderToTextureUpscale;
	}
	x0 = (int)floor(sx < ex ? sx : ex);
	y0 = (int)floor(sy < ey ? sy : ey);
	x1 = (int)ceil(sx > ex ? sx : ex);
	y1 = (int)ceil(sy > ey ? sy : ey);
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 < x0)
		x1 = x0;
	if (y1 < y0)
		y1 = y0;
	rect[0] = x0;
	rect[1] = y0;
	rect[2] = x1 - x0;
	rect[3] = y1 - y0;
}

/* Where two polygons are at the very same depth, the one that was left
 * showing the first time has to be the one left showing the second time:
 * the later one if its depth test lets equal depths through (equal, less or
 * equal, greater or equal, always; punch-through is always greater or
 * equal), the earlier one if not (less, greater, not equal). */
static INLINE bool shadow_later_wins(u32 listType, const PolyParam *gp)
{
	return listType == ListType_Punch_Through || ((0xCC >> TAW_GET(TAW_FULL(gp->isp), ISP_TSP_DepthMode)) & 1);
}

#ifdef __cplusplus
inline void ScreenBounds::add(float x, float y)
{
	screen_bounds_add(this, x, y);
}
inline bool ScreenBounds::empty() const
{
	return screen_bounds_empty(this);
}
inline bool ScreenBounds::overlaps(const ScreenBounds& o) const
{
	return min_x <= o.max_x && max_x >= o.min_x && min_y <= o.max_y && max_y >= o.min_y;
}

static inline ScreenBounds ModVolBounds(int first, int count)
{
	return shadow_modvol_bounds(first, count);
}
static inline bool PolyOverlaps(const PolyParam *gp, const ScreenBounds& area)
{
	return poly_overlaps(gp, &area);
}
static inline void BoundsToScissor(const ScreenBounds& b, const xform& viewport, int *rect)
{
	bounds_to_scissor(&b, &viewport, rect);
}
static inline bool ShadowLaterWins(u32 listType, const PolyParam *gp)
{
	return shadow_later_wins(listType, gp);
}
#endif

#endif
