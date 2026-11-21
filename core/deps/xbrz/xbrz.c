/* ****************************************************************************
 * This file is part of the HqMAME project. It is distributed under         *
 * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0         *
 * Copyright (C) Zenju (zenju AT gmx DOT de) - All Rights Reserved          *
 *                                                                          *
 * Additionally and as a special exception, the author gives permission     *
 * to link the code of this program with the MAME library (or with modified *
 * versions of MAME that use the same license as MAME), and distribute      *
 * linked combinations including the two. You must obey the GNU General     *
 * Public License in all respects for all of the code used other than MAME. *
 * If you modify this file, you may extend this exception to your version   *
 * of the file, but you are not obligated to do so. If you do not wish to   *
 * do so, delete this exception statement from your version.                *
 * ****************************************************************************/

/* xBRZ 1.5 in C: its templates' instances made by the compiler from
 * xb_scale(), whose factor and colour format are constants where it is
 * called. The YCbCr distance is worked out as xBRZ's table (64 MiB, made
 * at its first use) has it: the same float, with no table. */
#include <math.h>
#include <string.h>
#include <retro_inline.h>
#include "xbrz.h"

/* (perf: quite worth it) */
#if defined(_MSC_VER)
#define XB_INLINE static __forceinline
#elif defined(__GNUC__)
#define XB_INLINE static __inline__ __attribute__((always_inline))
#else
#define XB_INLINE static INLINE
#endif

/* (the tests of xBRZ's ScalerCfg, as it is by default) */
#define XB_EQUAL_COLOR_TOLERANCE 30.0
#define XB_DOMINANT_DIRECTION_THRESHOLD 3.6
#define XB_STEEP_DIRECTION_THRESHOLD 2.2

#define XB_BLEND_NONE 0
#define XB_BLEND_NORMAL 1	/* a normal indication to blend */
#define XB_BLEND_DOMINANT 2	/* a strong indication to blend */

/* blend types of a pixel's four corners, 2 bits each */
#define XB_TOP_L(b) ((b) & 3)
#define XB_TOP_R(b) (((b) >> 2) & 3)
#define XB_BOTTOM_R(b) (((b) >> 4) & 3)
#define XB_BOTTOM_L(b) (((b) >> 6) & 3)

/* rotations, clockwise: 0, 90, 180, 270 degrees */
#define XB_ROT_0 0
#define XB_ROT_90 1
#define XB_ROT_180 2
#define XB_ROT_270 3

/* what a, b, ... i of a 3 x 3 kernel are, rotated */
static const unsigned char xb_rot_ker[4][9] =
{
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8 },
	{ 6, 3, 0, 7, 4, 1, 8, 5, 2 },
	{ 8, 7, 6, 5, 4, 3, 2, 1, 0 },
	{ 2, 5, 8, 1, 4, 7, 0, 3, 6 }
};

#define XB_R(p) ((p) & 0xff)
#define XB_G(p) (((p) >> 8) & 0xff)
#define XB_B(p) (((p) >> 16) & 0xff)
#define XB_A(p) ((p) >> 24)

/* front blended at @m / @n over an opaque back */
XB_INLINE uint32_t xb_gradient_rgb(unsigned m, unsigned n, uint32_t front, uint32_t back)
{
	unsigned r = (XB_R(front) * m + XB_R(back) * (n - m)) / n;
	unsigned g = (XB_G(front) * m + XB_G(back) * (n - m)) / n;
	unsigned b = (XB_B(front) * m + XB_B(back) * (n - m)) / n;
	return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* the colour between two with alpha, weighed by it (no alpha blending) */
XB_INLINE uint32_t xb_gradient_argb(unsigned m, unsigned n, uint32_t front, uint32_t back)
{
	unsigned wf = XB_A(front) * m;
	unsigned wb = XB_A(back) * (n - m);
	unsigned ws = wf + wb;
	unsigned r, g, b;
	if (ws == 0)
		return 0;
	r = (XB_R(front) * wf + XB_R(back) * wb) / ws;
	g = (XB_G(front) * wf + XB_G(back) * wb) / ws;
	b = (XB_B(front) * wf + XB_B(back) * wb) / ws;
	return ((uint32_t)(ws / n) << 24) | (b << 16) | (g << 8) | r;
}

/* YCbCr distance (ITU-R BT.2020) of two colours, each difference halved
 * into a byte as xBRZ's table has it, not divided by 255 */
XB_INLINE double xb_dist_ycbcr(uint32_t p1, uint32_t p2)
{
	const double k_b = 0.0593;
	const double k_r = 0.2627;
	const double k_g = 1 - k_b - k_r;
	const double scale_b = 0.5 / (1 - k_b);
	const double scale_r = 0.5 / (1 - k_r);
	int r_diff = ((int)XB_R(p1) - (int)XB_R(p2) + 0xFF) / 2 * 2 - 0xFF;
	int g_diff = ((int)XB_G(p1) - (int)XB_G(p2) + 0xFF) / 2 * 2 - 0xFF;
	int b_diff = ((int)XB_B(p1) - (int)XB_B(p2) + 0xFF) / 2 * 2 - 0xFF;
	double y = k_r * r_diff + k_g * g_diff + k_b * b_diff;
	double c_b = scale_b * (b_diff - y);
	double c_r = scale_r * (r_diff - y);
	return (float)sqrt(y * y + c_b * c_b + c_r * c_r);
}

XB_INLINE double xb_dist(int argb, uint32_t p1, uint32_t p2)
{
	double d = xb_dist_ycbcr(p1, p2);
	double a1, a2;
	if (!argb)
		return d;
	a1 = XB_A(p1) / 255.0;
	a2 = XB_A(p2) / 255.0;
	if (a1 < a2)
		return a1 * d + 255 * (a2 - a1);
	return a2 * d + 255 * (a1 - a2);
}

XB_INLINE int xb_eq(int argb, uint32_t p1, uint32_t p2)
{
	return xb_dist(argb, p1, p2) < XB_EQUAL_COLOR_TOLERANCE;
}

/* cell (@i, @j) (row, column) of an @n x @n block at @out, rotated */
XB_INLINE uint32_t *xb_ref(uint32_t *out, int w, int n, int rot, int i, int j)
{
	int io, jo;
	switch (rot)
	{
	default:
		io = i;
		jo = j;
		break;
	case XB_ROT_90:
		io = n - 1 - j;
		jo = i;
		break;
	case XB_ROT_180:
		io = n - 1 - i;
		jo = n - 1 - j;
		break;
	case XB_ROT_270:
		io = j;
		jo = n - 1 - i;
		break;
	}
	return out + jo + io * w;
}

XB_INLINE void xb_grad(int argb, unsigned m, unsigned n, uint32_t *back, uint32_t front)
{
	*back = argb ? xb_gradient_argb(m, n, front, *back) : xb_gradient_rgb(m, n, front, *back);
}

#define XB_LINE_SHALLOW 0
#define XB_LINE_STEEP 1
#define XB_LINE_STEEP_AND_SHALLOW 2
#define XB_LINE_DIAGONAL 3
#define XB_CORNER 4

#define XB_GR(m, d, i, j) xb_grad(argb, m, d, xb_ref(out, w, scale, rot, i, j), col)
#define XB_SET(i, j) (*xb_ref(out, w, scale, rot, i, j) = col)

/* a corner's line or round corner in @col, into a block of @scale */
XB_INLINE void xb_blend(int kind, int scale, int argb, int rot, uint32_t col, uint32_t *out, int w)
{
	switch (scale)
	{
	case 2:
		switch (kind)
		{
		case XB_LINE_SHALLOW:
			XB_GR(1, 4, 1, 0); XB_GR(3, 4, 1, 1);
			break;
		case XB_LINE_STEEP:
			XB_GR(1, 4, 0, 1); XB_GR(3, 4, 1, 1);
			break;
		case XB_LINE_STEEP_AND_SHALLOW:
			XB_GR(1, 4, 1, 0); XB_GR(1, 4, 0, 1);
			XB_GR(5, 6, 1, 1);	/* [!] fixes 7/8 used in xBR */
			break;
		case XB_LINE_DIAGONAL:
			XB_GR(1, 2, 1, 1);
			break;
		default:	/* exact: 1 - pi/4 = 0.2146018366 */
			XB_GR(21, 100, 1, 1);
			break;
		}
		break;
	case 3:
		switch (kind)
		{
		case XB_LINE_SHALLOW:
			XB_GR(1, 4, 2, 0); XB_GR(1, 4, 1, 2);
			XB_GR(3, 4, 2, 1); XB_SET(2, 2);
			break;
		case XB_LINE_STEEP:
			XB_GR(1, 4, 0, 2); XB_GR(1, 4, 2, 1);
			XB_GR(3, 4, 1, 2); XB_SET(2, 2);
			break;
		case XB_LINE_STEEP_AND_SHALLOW:
			XB_GR(1, 4, 2, 0); XB_GR(1, 4, 0, 2);
			XB_GR(3, 4, 2, 1); XB_GR(3, 4, 1, 2);
			XB_SET(2, 2);
			break;
		case XB_LINE_DIAGONAL:
			XB_GR(1, 8, 1, 2); XB_GR(1, 8, 2, 1); XB_GR(7, 8, 2, 2);
			break;
		default:	/* exact: 0.4545939598 */
			XB_GR(45, 100, 2, 2);
			break;
		}
		break;
	case 4:
		switch (kind)
		{
		case XB_LINE_SHALLOW:
			XB_GR(1, 4, 3, 0); XB_GR(1, 4, 2, 2);
			XB_GR(3, 4, 3, 1); XB_GR(3, 4, 2, 3);
			XB_SET(3, 2); XB_SET(3, 3);
			break;
		case XB_LINE_STEEP:
			XB_GR(1, 4, 0, 3); XB_GR(1, 4, 2, 2);
			XB_GR(3, 4, 1, 3); XB_GR(3, 4, 3, 2);
			XB_SET(2, 3); XB_SET(3, 3);
			break;
		case XB_LINE_STEEP_AND_SHALLOW:
			XB_GR(3, 4, 3, 1); XB_GR(3, 4, 1, 3);
			XB_GR(1, 4, 3, 0); XB_GR(1, 4, 0, 3);
			XB_GR(1, 3, 2, 2);	/* [!] fixes 1/4 used in xBR */
			XB_SET(3, 3); XB_SET(3, 2); XB_SET(2, 3);
			break;
		case XB_LINE_DIAGONAL:
			XB_GR(1, 2, 3, 2); XB_GR(1, 2, 2, 3); XB_SET(3, 3);
			break;
		default:	/* exact: 0.6848532563, 0.08677704501 */
			XB_GR(68, 100, 3, 3); XB_GR(9, 100, 3, 2); XB_GR(9, 100, 2, 3);
			break;
		}
		break;
	case 5:
		switch (kind)
		{
		case XB_LINE_SHALLOW:
			XB_GR(1, 4, 4, 0); XB_GR(1, 4, 3, 2); XB_GR(1, 4, 2, 4);
			XB_GR(3, 4, 4, 1); XB_GR(3, 4, 3, 3);
			XB_SET(4, 2); XB_SET(4, 3); XB_SET(4, 4); XB_SET(3, 4);
			break;
		case XB_LINE_STEEP:
			XB_GR(1, 4, 0, 4); XB_GR(1, 4, 2, 3); XB_GR(1, 4, 4, 2);
			XB_GR(3, 4, 1, 4); XB_GR(3, 4, 3, 3);
			XB_SET(2, 4); XB_SET(3, 4); XB_SET(4, 4); XB_SET(4, 3);
			break;
		case XB_LINE_STEEP_AND_SHALLOW:
			XB_GR(1, 4, 0, 4); XB_GR(1, 4, 2, 3); XB_GR(3, 4, 1, 4);
			XB_GR(1, 4, 4, 0); XB_GR(1, 4, 3, 2); XB_GR(3, 4, 4, 1);
			XB_GR(2, 3, 3, 3);
			XB_SET(2, 4); XB_SET(3, 4); XB_SET(4, 4);
			XB_SET(4, 2); XB_SET(4, 3);
			break;
		case XB_LINE_DIAGONAL:
			XB_GR(1, 8, 4, 2); XB_GR(1, 8, 3, 3); XB_GR(1, 8, 2, 4);
			XB_GR(7, 8, 4, 3); XB_GR(7, 8, 3, 4);
			XB_SET(4, 4);
			break;
		default:	/* exact: 0.8631434088, 0.2306749731 */
			XB_GR(86, 100, 4, 4); XB_GR(23, 100, 4, 3); XB_GR(23, 100, 3, 4);
			break;
		}
		break;
	default:
		switch (kind)
		{
		case XB_LINE_SHALLOW:
			XB_GR(1, 4, 5, 0); XB_GR(1, 4, 4, 2); XB_GR(1, 4, 3, 4);
			XB_GR(3, 4, 5, 1); XB_GR(3, 4, 4, 3); XB_GR(3, 4, 3, 5);
			XB_SET(5, 2); XB_SET(5, 3); XB_SET(5, 4); XB_SET(5, 5);
			XB_SET(4, 4); XB_SET(4, 5);
			break;
		case XB_LINE_STEEP:
			XB_GR(1, 4, 0, 5); XB_GR(1, 4, 2, 4); XB_GR(1, 4, 4, 3);
			XB_GR(3, 4, 1, 5); XB_GR(3, 4, 3, 4); XB_GR(3, 4, 5, 3);
			XB_SET(2, 5); XB_SET(3, 5); XB_SET(4, 5); XB_SET(5, 5);
			XB_SET(4, 4); XB_SET(5, 4);
			break;
		case XB_LINE_STEEP_AND_SHALLOW:
			XB_GR(1, 4, 0, 5); XB_GR(1, 4, 2, 4); XB_GR(3, 4, 1, 5); XB_GR(3, 4, 3, 4);
			XB_GR(1, 4, 5, 0); XB_GR(1, 4, 4, 2); XB_GR(3, 4, 5, 1); XB_GR(3, 4, 4, 3);
			XB_SET(2, 5); XB_SET(3, 5); XB_SET(4, 5); XB_SET(5, 5);
			XB_SET(4, 4); XB_SET(5, 4);
			XB_SET(5, 2); XB_SET(5, 3);
			break;
		case XB_LINE_DIAGONAL:
			XB_GR(1, 2, 5, 3); XB_GR(1, 2, 4, 4); XB_GR(1, 2, 3, 5);
			XB_SET(4, 5); XB_SET(5, 5); XB_SET(5, 4);
			break;
		default:	/* exact: 0.9711013910, 0.4236372243, 0.05652034508 */
			XB_GR(97, 100, 5, 5); XB_GR(42, 100, 4, 5); XB_GR(42, 100, 5, 4);
			XB_GR(6, 100, 5, 3); XB_GR(6, 100, 3, 5);
			break;
		}
		break;
	}
}

#undef XB_GR
#undef XB_SET

/* corners F, G, J, K of a 4 x 4 kernel (A B C D / E F G H / I J K L /
 * M N O P, the pixel at F): which to blend, and how */
typedef struct xb_ker4
{
	uint32_t a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p;
} xb_ker4;

typedef struct xb_blends
{
	int f, g, j, k;
} xb_blends;

XB_INLINE void xb_pre_process_corners(int argb, const xb_ker4 *ker, xb_blends *res)
{
	double jg, fk;
	res->f = res->g = res->j = res->k = XB_BLEND_NONE;
	if ((ker->f == ker->g && ker->j == ker->k) || (ker->f == ker->j && ker->g == ker->k))
		return;
	jg = xb_dist(argb, ker->i, ker->f) + xb_dist(argb, ker->f, ker->c)
		+ xb_dist(argb, ker->n, ker->k) + xb_dist(argb, ker->k, ker->h)
		+ 4 * xb_dist(argb, ker->j, ker->g);
	fk = xb_dist(argb, ker->e, ker->j) + xb_dist(argb, ker->j, ker->o)
		+ xb_dist(argb, ker->b, ker->g) + xb_dist(argb, ker->g, ker->l)
		+ 4 * xb_dist(argb, ker->f, ker->k);
	if (jg < fk)
	{
		int dominant = XB_DOMINANT_DIRECTION_THRESHOLD * jg < fk;
		if (ker->f != ker->g && ker->f != ker->j)
			res->f = dominant ? XB_BLEND_DOMINANT : XB_BLEND_NORMAL;
		if (ker->k != ker->j && ker->k != ker->g)
			res->k = dominant ? XB_BLEND_DOMINANT : XB_BLEND_NORMAL;
	}
	else if (fk < jg)
	{
		int dominant = XB_DOMINANT_DIRECTION_THRESHOLD * fk < jg;
		if (ker->j != ker->f && ker->j != ker->k)
			res->j = dominant ? XB_BLEND_DOMINANT : XB_BLEND_NORMAL;
		if (ker->g != ker->f && ker->g != ker->k)
			res->g = dominant ? XB_BLEND_DOMINANT : XB_BLEND_NORMAL;
	}
}

/* the bottom right corner, @rot turned, of the pixel at E of a 3 x 3
 * kernel (A B C / D E F / G H I) blended into its block at @target, by
 * @blend_info (its four corners' blend types) */
XB_INLINE void xb_blend_pixel(int scale, int argb, int rot, const uint32_t *k3,
		uint32_t *target, int trg_width, unsigned blend_info)
{
	const unsigned char *rk = xb_rot_ker[rot];
	uint32_t b = k3[rk[1]], c = k3[rk[2]], d = k3[rk[3]];
	uint32_t e = k3[rk[4]], f = k3[rk[5]], g = k3[rk[6]];
	uint32_t h = k3[rk[7]], i = k3[rk[8]];
	unsigned blend = rot == XB_ROT_0 ? blend_info
		: ((blend_info << (2 * rot)) | (blend_info >> (8 - 2 * rot))) & 0xff;
	int line_blend;
	uint32_t px;

	if (XB_BOTTOM_R(blend) < XB_BLEND_NORMAL)
		return;

	if (XB_BOTTOM_R(blend) >= XB_BLEND_DOMINANT)
		line_blend = 1;
	/* no second blending in an adjacent rotation for this pixel:
	 * insular pixels, mario eyes (but double-blending for 90 degree
	 * corners) */
	else if (XB_TOP_R(blend) != XB_BLEND_NONE && !xb_eq(argb, e, g))
		line_blend = 0;
	else if (XB_BOTTOM_L(blend) != XB_BLEND_NONE && !xb_eq(argb, e, c))
		line_blend = 0;
	/* no full blending for L-shapes; corner only (mario mushroom eyes) */
	else if (!xb_eq(argb, e, i) && xb_eq(argb, g, h) && xb_eq(argb, h, i)
			&& xb_eq(argb, i, f) && xb_eq(argb, f, c))
		line_blend = 0;
	else
		line_blend = 1;

	/* the most similar colour */
	px = xb_dist(argb, e, f) <= xb_dist(argb, e, h) ? f : h;

	if (line_blend)
	{
		double fg = xb_dist(argb, f, g);
		double hc = xb_dist(argb, h, c);
		int shallow = XB_STEEP_DIRECTION_THRESHOLD * fg <= hc && e != g && d != g;
		int steep = XB_STEEP_DIRECTION_THRESHOLD * hc <= fg && e != c && b != c;
		if (shallow)
			xb_blend(steep ? XB_LINE_STEEP_AND_SHALLOW : XB_LINE_SHALLOW, scale, argb, rot, px, target, trg_width);
		else
			xb_blend(steep ? XB_LINE_STEEP : XB_LINE_DIAGONAL, scale, argb, rot, px, target, trg_width);
	}
	else
		xb_blend(XB_CORNER, scale, argb, rot, px, target, trg_width);
}

#define XB_MIN(a, b) ((a) < (b) ? (a) : (b))
#define XB_MAX(a, b) ((a) > (b) ? (a) : (b))

/* the 4 x 4 kernel at (@x, row s_0), edges repeated */
XB_INLINE void xb_ker4_read(xb_ker4 *k, const uint32_t *s_m1, const uint32_t *s_0,
		const uint32_t *s_p1, const uint32_t *s_p2, int x, int src_width)
{
	int x_m1 = XB_MAX(x - 1, 0);
	int x_p1 = XB_MIN(x + 1, src_width - 1);
	int x_p2 = XB_MIN(x + 2, src_width - 1);
	k->a = s_m1[x_m1]; k->b = s_m1[x]; k->c = s_m1[x_p1]; k->d = s_m1[x_p2];
	k->e = s_0[x_m1]; k->f = s_0[x]; k->g = s_0[x_p1]; k->h = s_0[x_p2];
	k->i = s_p1[x_m1]; k->j = s_p1[x]; k->k = s_p1[x_p1]; k->l = s_p1[x_p2];
	k->m = s_p2[x_m1]; k->n = s_p2[x]; k->o = s_p2[x_p1]; k->p = s_p2[x_p2];
}

XB_INLINE void xb_scale(int scale, int argb, const uint32_t *src, uint32_t *trg,
		int src_width, int src_height, int y_first, int y_last)
{
	int trg_width, x, y;
	unsigned char *pre;	/* the blend types made ahead, a row's */

	y_first = XB_MAX(y_first, 0);
	y_last = XB_MIN(y_last, src_height);
	if (y_first >= y_last || src_width <= 0)
		return;

	trg_width = src_width * scale;
	/* (kept at the end of the slice's rows of @trg: written last) */
	pre = (unsigned char *)(trg + y_last * scale * trg_width) - src_width;
	memset(pre, 0, src_width);

	/* the first row's top corners, from the row above: this cannot be
	 * shared with the slice above, which writes at the same time */
	if (y_first > 0)
	{
		const uint32_t *s_m1, *s_0, *s_p1, *s_p2;
		y = y_first - 1;
		s_m1 = src + src_width * XB_MAX(y - 1, 0);
		s_0 = src + src_width * y;
		s_p1 = src + src_width * XB_MIN(y + 1, src_height - 1);
		s_p2 = src + src_width * XB_MIN(y + 2, src_height - 1);
		for (x = 0; x < src_width; ++x)
		{
			xb_ker4 ker;
			xb_blends res;
			xb_ker4_read(&ker, s_m1, s_0, s_p1, s_p2, x, src_width);
			xb_pre_process_corners(argb, &ker, &res);
			pre[x] |= res.j << 2;
			if (x + 1 < src_width)
				pre[x + 1] |= res.k;
		}
	}

	for (y = y_first; y < y_last; ++y)
	{
		uint32_t *out = trg + scale * y * trg_width;
		const uint32_t *s_m1 = src + src_width * XB_MAX(y - 1, 0);
		const uint32_t *s_0 = src + src_width * y;
		const uint32_t *s_p1 = src + src_width * XB_MIN(y + 1, src_height - 1);
		const uint32_t *s_p2 = src + src_width * XB_MIN(y + 2, src_height - 1);
		unsigned blend_xy1 = 0;	/* corners of (x, y + 1) */

		for (x = 0; x < src_width; ++x, out += scale)
		{
			xb_ker4 ker4;
			xb_blends res;
			unsigned blend_xy;	/* of (x, y) */
			int bx, by;

			xb_ker4_read(&ker4, s_m1, s_0, s_p1, s_p2, x, src_width);

			/* the four corners at the bottom right of the pixel */
			xb_pre_process_corners(argb, &ker4, &res);
			/* (all four of (x, y) known now) */
			blend_xy = pre[x] | (res.f << 4);
			/* the second known of (x, y + 1), kept for the next row */
			blend_xy1 |= res.j << 2;
			pre[x] = (unsigned char)blend_xy1;
			/* the first known of (x + 1, y + 1) */
			blend_xy1 = res.k;
			/* the third known of (x + 1, y) */
			if (x + 1 < src_width)
				pre[x + 1] |= res.g << 6;

			/* (after the above: the last pixel's block is where pre is) */
			for (by = 0; by < scale; ++by)
				for (bx = 0; bx < scale; ++bx)
					out[by * trg_width + bx] = ker4.f;

			if (blend_xy != 0)
			{
				uint32_t k3[9];
				k3[0] = ker4.a; k3[1] = ker4.b; k3[2] = ker4.c;
				k3[3] = ker4.e; k3[4] = ker4.f; k3[5] = ker4.g;
				k3[6] = ker4.i; k3[7] = ker4.j; k3[8] = ker4.k;
				xb_blend_pixel(scale, argb, XB_ROT_0, k3, out, trg_width, blend_xy);
				xb_blend_pixel(scale, argb, XB_ROT_90, k3, out, trg_width, blend_xy);
				xb_blend_pixel(scale, argb, XB_ROT_180, k3, out, trg_width, blend_xy);
				xb_blend_pixel(scale, argb, XB_ROT_270, k3, out, trg_width, blend_xy);
			}
		}
	}
}

#define XB_SCALER(s, a, name) \
static void name(const uint32_t *src, uint32_t *trg, int w, int h, int y0, int y1) \
{ \
	xb_scale(s, a, src, trg, w, h, y0, y1); \
}
XB_SCALER(2, 0, xb_scale2_rgb)
XB_SCALER(3, 0, xb_scale3_rgb)
XB_SCALER(4, 0, xb_scale4_rgb)
XB_SCALER(5, 0, xb_scale5_rgb)
XB_SCALER(6, 0, xb_scale6_rgb)
XB_SCALER(2, 1, xb_scale2_argb)
XB_SCALER(3, 1, xb_scale3_argb)
XB_SCALER(4, 1, xb_scale4_argb)
XB_SCALER(5, 1, xb_scale5_argb)
XB_SCALER(6, 1, xb_scale6_argb)

void xbrz_scale(int factor, const uint32_t *src, uint32_t *trg,
		int src_width, int src_height, bool argb, int y_first, int y_last)
{
	typedef void (*scaler)(const uint32_t *, uint32_t *, int, int, int, int);
	static const scaler scalers[2][5] =
	{
		{ xb_scale2_rgb, xb_scale3_rgb, xb_scale4_rgb, xb_scale5_rgb, xb_scale6_rgb },
		{ xb_scale2_argb, xb_scale3_argb, xb_scale4_argb, xb_scale5_argb, xb_scale6_argb }
	};
	if (factor < 2 || factor > 6)
		return;
	scalers[argb ? 1 : 0][factor - 2](src, trg, src_width, src_height, y_first, y_last);
}
