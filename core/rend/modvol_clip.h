/* Modifier volume triangles for the raster renderers, with a vertex far off
 * the screen clipped first.
 *
 * A game's shadow volume near the eye has vertices hundreds of millions of
 * pixels out (Jet Grind Radio's are 1e9 pixels out, at 1/w 1e5). The raster
 * renderers hand such a vertex to the GPU as it is, and the GPU's clipper
 * then cuts the triangle to the screen in 32-bit float: where an edge
 * between two such vertices crosses the screen's side, the point it finds
 * is the difference of two coordinates of 30 (x w) that should come to
 * 1e-5 (w), and its place on the screen is out by a fifth of the screen's
 * width. The triangle covers a strip by the screen's edge it does not, or
 * misses one it covers, and every pixel there is the wrong side of the
 * shadow: on the jgr13 frame a band down the right of the street, and in
 * Direct3D 12 a strip along the top, on lavapipe. (refsw and the compute
 * renderer, which take the exact geometry, draw neither.)
 *
 * So a triangle with a vertex more than MV_CLIP_LIMIT pixels from the
 * screen's origin is cut here, in double precision, to the square of that
 * size about the origin - which holds the screen at any resolution and
 * any aspect the matrix gives, many times over - and handed on as the fan
 * of up to five triangles the polygon left makes. What is inside the
 * square is the triangle's, to well under a 1/256 pixel: each point is
 * the edge's, and 1/w, which is linear across the screen, is taken along
 * the edge to it. The GPU then has vertices no farther than the square,
 * whose clipping float keeps. The fan's triangles have the triangle's
 * winding, and share their edges exactly, so a sample is in one of them or
 * none, as it was in the triangle or not.
 *
 * An edge two triangles share is cut to the same points in both (each cut
 * is worked out from the edge's two ends taken in one order, whatever the
 * order the triangle has them in), so a volume's faces still meet with no
 * gap and no sample in both. A triangle whose vertices are all within the
 * square is not touched: a frame with none farther than it is drawn as it
 * always was, to the bit. */
#ifndef MODVOL_CLIP_H
#define MODVOL_CLIP_H
/* C89, for C and C++ alike */
#include <math.h>
#include <retro_inline.h>
#include "types.h"
#include "hw/pvr/ta_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MV_CLIP_LIMIT 32768.0f
#define MV_CLIP_MAX 5	/* triangles a clipped one can become */

/* Whether the triangle is to be cut: a vertex beyond the square, and all
 * three in front of the eye with a 1/w the volumes' shader draws (it
 * leaves a triangle with a 1/w below 0, above 3.4e37 or not a number
 * undrawn, and this does not change that) */
static INLINE bool mv_clip_wanted(const ModTriangle *t)
{
	const float L = MV_CLIP_LIMIT;
	const bool beyond = !(fabs(t->x0) <= L && fabs(t->y0) <= L && fabs(t->x1) <= L && fabs(t->y1) <= L
			&& fabs(t->x2) <= L && fabs(t->y2) <= L);
	return beyond && t->z0 > 0.f && t->z1 > 0.f && t->z2 > 0.f && t->z0 <= 3.4e37f && t->z1 <= 3.4e37f && t->z2 <= 3.4e37f
			&& !float_is_nan(t->x0) && !float_is_nan(t->y0) && !float_is_nan(t->x1) && !float_is_nan(t->y1)
			&& !float_is_nan(t->x2) && !float_is_nan(t->y2);
}

typedef struct MvClipPoint
{
	double x, y, z;
} MvClipPoint;

/* Where the edge from @a to @b crosses the line coord @k = @at: from the
 * ends taken in one order, so that the edge gives the same point in both
 * of the triangles that have it */
static INLINE MvClipPoint mv_clip_cross(MvClipPoint a, MvClipPoint b, int k, double at)
{
	double ca, cb, t;
	MvClipPoint p;
	if (a.x > b.x || (a.x == b.x && (a.y > b.y || (a.y == b.y && a.z > b.z))))
	{
		const MvClipPoint s = a;
		a = b;
		b = s;
	}
	ca = k == 0 ? a.x : a.y;
	cb = k == 0 ? b.x : b.y;
	t = (at - ca) / (cb - ca);
	p.x = k == 0 ? at : a.x + t * (b.x - a.x);
	p.y = k == 1 ? at : a.y + t * (b.y - a.y);
	p.z = a.z + t * (b.z - a.z);
	return p;
}

/* The triangle cut to the square: up to MV_CLIP_MAX triangles into @out,
 * how many (0: it does not reach into the square) */
static INLINE u32 mv_clip_triangle(const ModTriangle *t, ModTriangle *out)
{
	MvClipPoint buf[2][9];
	MvClipPoint *p = buf[0], *q = buf[1];
	u32 n = 3, count = 0, i;
	const double L = MV_CLIP_LIMIT;
	int side;
	p[0].x = t->x0; p[0].y = t->y0; p[0].z = t->z0;
	p[1].x = t->x1; p[1].y = t->y1; p[1].z = t->z1;
	p[2].x = t->x2; p[2].y = t->y2; p[2].z = t->z2;
	for (side = 0; side < 4 && n != 0; side++)
	{
		const int k = side >> 1;
		const double s = side & 1 ? -1.0 : 1.0;	/* inside: s * coord <= L */
		u32 m = 0;
		MvClipPoint *x;
		for (i = 0; i < n; i++)
		{
			const MvClipPoint *a = &p[i];
			const MvClipPoint *b = &p[i + 1 == n ? 0 : i + 1];
			const bool ain = s * (k == 0 ? a->x : a->y) <= L;
			const bool bin = s * (k == 0 ? b->x : b->y) <= L;
			if (ain)
				q[m++] = *a;
			if (ain != bin)
				q[m++] = mv_clip_cross(*a, *b, k, s * L);
		}
		x = p;
		p = q;
		q = x;
		n = m;
	}
	if (n < 3)
		return 0;
	for (i = 1; i + 1 < n; i++)
	{
		ModTriangle *o = &out[count++];
		o->x0 = (f32)p[0].x; o->y0 = (f32)p[0].y; o->z0 = (f32)p[0].z;
		o->x1 = (f32)p[i].x; o->y1 = (f32)p[i].y; o->z1 = (f32)p[i].z;
		o->x2 = (f32)p[i + 1].x; o->y2 = (f32)p[i + 1].y; o->z2 = (f32)p[i + 1].z;
	}
	return count;
}

/* The volume triangles to cut: @get(@ctx, t) the triangle t; @restart
 * (may be NULL), before each list's are asked for again */
typedef ModTriangle (*mv_clip_get_t)(void *ctx, u32 t);
typedef void (*mv_clip_restart_t)(void *ctx);

/* The frame's mv_clip and mv_clip_range (rend_context), from its
 * @triangles volume triangles: left empty when none of them is to be cut,
 * or when the cut ones do not fit (the volumes are then drawn as they
 * are). A volume left with no triangle keeps one with no area, so that a
 * closing draw that goes with its last triangle is still made. Each
 * list's volumes have their triangles in increasing order, the opaque
 * list's and the translucent list's numbered together. */
static INLINE void mv_clip_build(rend_context *rc, u32 triangles, mv_clip_get_t get, mv_clip_restart_t restart, void *ctx)
{
	ta_list_t *clip = &rc->mv_clip.l;
	ta_list_t *ranges = &rc->mv_clip_range.l;
	bool any = false;
	u32 t, lists;
	int l;
#define MVC_USED(lp, T) ((u32)((T *)(lp)->cur - (T *)(lp)->base))
#define MVC_AVAIL(lp, T) ((u32)((T *)(lp)->end - (T *)(lp)->cur))
	ta_list_clear(clip);
	ta_list_clear(ranges);
	for (t = 0; t < triangles && !any; t++)
	{
		const ModTriangle tri = get(ctx, t);
		any = mv_clip_wanted(&tri);
	}
	if (!any)
		return;
	lists = MVC_USED(&rc->global_param_mvo.l, ModifierVolumeParam) + MVC_USED(&rc->global_param_mvo_tr.l, ModifierVolumeParam);
	if (MVC_AVAIL(ranges, u32) < 2 * lists)
		return;
	for (l = 0; l < 2; l++)
	{
		const ta_list_t *list = l == 0 ? &rc->global_param_mvo.l : &rc->global_param_mvo_tr.l;
		const ModifierVolumeParam *p;
		if (restart != NULL)
			restart(ctx);
		for (p = (const ModifierVolumeParam *)list->base; p != (const ModifierVolumeParam *)list->cur; p++)
		{
			u32 *range = (u32 *)ta_list_append(ranges, 2 * sizeof(u32));
			range[0] = MVC_USED(clip, ModTriangle);
			for (t = p->first; t < p->first + p->count && t < triangles; t++)
			{
				ModTriangle tri;
				if (MVC_AVAIL(clip, ModTriangle) < MV_CLIP_MAX + 1)
				{
					ta_list_clear(clip);
					ta_list_clear(ranges);
					return;
				}
				tri = get(ctx, t);
				if (mv_clip_wanted(&tri))
				{
					const u32 n = mv_clip_triangle(&tri, (ModTriangle *)clip->cur);
					ta_list_append(clip, (size_t)n * sizeof(ModTriangle));
				}
				else
					*(ModTriangle *)ta_list_append(clip, sizeof(ModTriangle)) = tri;
			}
			if (MVC_USED(clip, ModTriangle) == range[0] && p->count != 0)
			{
				ModTriangle *o = (ModTriangle *)ta_list_append(clip, sizeof(ModTriangle));
				o->x0 = o->x1 = o->x2 = 0.f;
				o->y0 = o->y1 = o->y2 = 0.f;
				o->z0 = o->z1 = o->z2 = 1.f;
			}
			range[1] = MVC_USED(clip, ModTriangle) - range[0];
		}
	}
#undef MVC_USED
#undef MVC_AVAIL
}

/* A volume's first triangle and count as the raster renderers draw them:
 * in mv_clip when the frame has it, else its own. @list is the list the
 * parameter is in (global_param_mvo's head or global_param_mvo_tr's). */
static INLINE void mv_clip_range_get(const rend_context *rc, const ModifierVolumeParam *list, const ModifierVolumeParam *p, u32 *first, u32 *count)
{
	const ModifierVolumeParam *op = (const ModifierVolumeParam *)rc->global_param_mvo.l.base;
	const u32 *range = (const u32 *)rc->mv_clip_range.l.base;
	u32 i;
	if (rc->mv_clip.l.cur == rc->mv_clip.l.base)
	{
		*first = p->first;
		*count = p->count;
		return;
	}
	i = (u32)(p - list) + (list == op ? 0 : (u32)((const ModifierVolumeParam *)rc->global_param_mvo.l.cur - op));
	*first = range[2 * i];
	*count = range[2 * i + 1];
}

#ifdef __cplusplus
}

static inline void mv_clip_range_of(const rend_context& rc, const ModifierVolumeParam *list, const ModifierVolumeParam& p, u32& first, u32& count)
{
	mv_clip_range_get(&rc, list, &p, &first, &count);
}
#endif

#endif
