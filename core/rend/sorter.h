/*
	 This file is part of reicast.

	 reicast is free software: you can redistribute it and/or modify
	 it under the terms of the GNU General Public License as published by
	 the Free Software Foundation, either version 2 of the License, or
	 (at your option) any later version.

	 reicast is distributed in the hope that it will be useful,
	 but WITHOUT ANY WARRANTY; without even the implied warranty of
	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	 GNU General Public License for more details.

	 You should have received a copy of the GNU General Public License
	 along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef SORTER_H
#define SORTER_H
/* The translucent polygons sorted (sorter.c). C89, for C and C++ alike. */
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_words.h"
#include "grow_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether two polygons are drawn the same way, so that GenSortedTo() makes
 * one draw of triangles of both that come one after the other */
static INLINE bool SortSameDraw(const PolyParam *pp0, const PolyParam *pp1)
{
	return (TAW_FULL(pp0->pcw) & PCW_DRAW_MASK) == (TAW_FULL(pp1->pcw) & PCW_DRAW_MASK)
			&& TAW_FULL(pp0->isp) == TAW_FULL(pp1->isp) && TAW_FULL(pp0->tcw) == TAW_FULL(pp1->tcw)
			&& TAW_FULL(pp0->tsp) == TAW_FULL(pp1->tsp) && pp0->tileclip == pp1->tileclip;
}

/* Sort based on min-z of each strip */
void SortPParams(int first, int count);

/* (typedef'd in ta_ctx.h) */
struct SortTrigDrawParam
{
	const PolyParam* ppid;
	u32 first;
	u32 count;
};

/* A list kept from one frame to the next, in memory only ever grown
 * (grow_buf.h): of draws, @count of them. Zeroed, it is an empty one. */
typedef struct SortList SortList;
struct SortList
{
	grow_buf_t buf;
	u32 count;
};

static INLINE SortTrigDrawParam *SortDraws(const SortList *list)
{
	return (SortTrigDrawParam *)list->buf.data;
}

/* A render pass's lists, one each, for as many passes as a frame has: the
 * @passes first of @lists, made (zeroed) where there were fewer; NULL if
 * there is no memory. SortPassesFree(): every list's memory and theirs. */
SortList *SortPasses(grow_buf_t *lists, u32 passes);
void SortPassesFree(grow_buf_t *lists);
/* The sorter's own room, reserved before a renderer's first frame */
void SortReserve(void);
/* A renderer's lists of the passes' draws, reserved the same way */
void SortPassesReserve(grow_buf_t *lists);
/* ...and one list's draws, as many as a frame mostly has; and the room for
 * the sorted triangles' indices of one, u32 each, @buf (OpenGL's) */
void SortListReserve(SortList *list);
void SortIndicesReserve(grow_buf_t *buf);
/* The most indices SortPassesTo() writes of this frame */
u32 SortPassesRoom(void);
/* Every render pass's sorted triangles (@sorted, a list a pass) and, if
 * @prepasses, their depth prepasses' (@prepass), the indices of the one
 * after the other's to @out (6 * SortedTriangles() of each pass's
 * translucent polygons at most), the draws' first counting from @out; how
 * many indices. Those passes not sorted have no draws. */
u32 SortPassesTo(grow_buf_t *sorted, grow_buf_t *prepass, u32 *out, bool prepasses, bool firstProvoking);
/* Pass @pass's list of @lists, or NULL if there is none */
static INLINE SortList *SortPass(const grow_buf_t *lists, u32 pass)
{
	return pass < lists->size / sizeof(SortList) ? (SortList *)lists->data + pass : NULL;
}
/* ...its draws, @count of them: none if there is no list */
static INLINE const SortTrigDrawParam *SortPassDraws(const grow_buf_t *lists, u32 pass, u32 *count)
{
	const SortList *list = SortPass(lists, pass);
	*count = list != NULL ? list->count : 0;
	return list != NULL ? SortDraws(list) : NULL;
}

/* The triangles of translucent polygons [first, first + count): GenSortedTo()
 * makes three indices of each, and GenPrepassTo() at most as many again, so
 * 6 times this is the room the two need together. */
u32 SortedTriangles(int first, int count);
/* Sorted by the min-z of each triangle, its draws into @draws (emptied
 * first) and its indices written to @out (3 * SortedTriangles() of room),
 * once each and in order, so that it may be the memory the card reads them
 * from: how many.
 * firstProvoking: the renderer takes a flat triangle's colour from its first
 * vertex (Vulkan), where the strip's colours have been moved one triangle
 * back; otherwise from its last (OpenGL). */
u32 GenSortedTo(int first, int count, SortList *draws, u32 *out, bool firstProvoking);

/* The depth prepass of a sorted translucent list (per triangle or per
 * strip). Sorting by triangle or by strip draws a polygon that is behind
 * another after it now and then - a long one reaching back to the horizon
 * sorts as far away - and as these do not write depth, it shows through.
 * The hardware sorts each pixel: a layer whose blending leaves nothing of
 * what is behind it (destination factor zero, or one minus the source
 * alpha with the alpha at 1) hides all that is behind it, whatever the
 * order. So before the colours are drawn, the depth of such pixels is
 * written - colour writes off, a pixel's alpha worked out as the colour
 * pass does and the pixel discarded unless it writes as 255 in 8 bits -
 * and the colour pass then draws nothing behind them. Nothing that was in
 * the right order changes. The depth is written in any order, so the
 * polygons are drawn grouped by what the prepass draws them with, not
 * sorted.
 *
 * What a polygon is drawn with in the prepass: everything that goes into
 * its alpha, its culling and its clipping. Two polygons that agree on all
 * of it are drawn in one draw. */
typedef struct PrepassKey PrepassKey;
struct PrepassKey
{
	u64 texid;
	u32 tsp;	/* the TSP word without the source factor, fog reduced to whether it is the alpha's (3) */
	u32 tcw;
	u32 pcw;
	u32 isp;	/* the culling */
	u32 tileclip;
};

static INLINE bool PrepassKeyEq(const PrepassKey *a, const PrepassKey *b)
{
	return a->texid == b->texid && a->tsp == b->tsp && a->tcw == b->tcw && a->pcw == b->pcw
			&& a->isp == b->isp && a->tileclip == b->tileclip;
}

/* (in the order of its fields) */
static INLINE bool PrepassKeyLess(const PrepassKey *a, const PrepassKey *b)
{
	if (a->texid != b->texid) return a->texid < b->texid;
	if (a->tsp != b->tsp) return a->tsp < b->tsp;
	if (a->tcw != b->tcw) return a->tcw < b->tcw;
	if (a->pcw != b->pcw) return a->pcw < b->pcw;
	if (a->isp != b->isp) return a->isp < b->isp;
	return a->tileclip < b->tileclip;
}

/* Whether a render pass's sorted translucent list gets a prepass: always
 * in the last render pass; in one before it only where the depth of the
 * sorted polygons is written after them anyway for the next pass
 * (TranslucentPolygonDepthMask), and then only for the polygons whose depth
 * that writes - so a later pass never sees a depth it did not see before. */
static INLINE bool PrepassWanted(bool lastPass)
{
	return lastPass || settings.rend.TranslucentPolygonDepthMask;
}

/* Whether the prepass discards no pixel of the polygon: blended with a
 * destination factor of zero, every pixel hides what is behind it */
static INLINE bool PrepassAlwaysHides(const PolyParam *pp)
{
	return TAW_GET(TAW_FULL(pp->tsp), TSP_DstInstr) == 0;
}

/* Whether a polygon of the list can have pixels that hide what is behind
 * them, by its parameters: blended into the picture (not an accumulation
 * buffer) with a destination factor of zero, or of one minus the source
 * alpha where the alpha can reach 1 (not a trilinear pass that scales it
 * down); in a render pass before the last, only one whose depth is
 * written for the next pass (see PrepassWanted()) */
static INLINE bool PrepassCandidate(const PolyParam *pp, bool lastPass)
{
	const u32 tsp = TAW_FULL(pp->tsp);
	if (pp->count <= 2 || TAW_GET(tsp, TSP_SrcSelect) || TAW_GET(tsp, TSP_DstSelect)
			|| (!lastPass && TAW_GET(TAW_FULL(pp->isp), ISP_TSP_ZWriteDis)))
		return false;
	if (TAW_GET(tsp, TSP_DstInstr) == 0)
		return true;
	if (TAW_GET(tsp, TSP_DstInstr) != 5)
		return false;
	/* (pass A's alpha is 1 - D / 4, pass B's D / 4 with D at most 3) */
	if (TAW_GET(TAW_FULL(pp->pcw), PCW_Texture) && TAW_GET(tsp, TSP_FilterMode) > 1
			&& TAW_GET(TAW_FULL(pp->tcw), TCW_MipMapped)
			&& (TAW_GET(tsp, TSP_FilterMode) != 2 || (TAW_GET(tsp, TSP_MipMapD) & 3) != 0))
		return false;
	return true;
}

/* Whether a polygon's alpha is its vertices' (or theirs times the
 * texture's): then it can reach 1 only where a vertex has 255. Not with
 * fog in the alpha, or a clamp that may raise it. (A destination factor of
 * zero needs no alpha.) */
static INLINE bool PrepassAlphaFromVertices(const PolyParam *pp)
{
	const u32 tsp = TAW_FULL(pp->tsp);
	return !(TAW_GET(tsp, TSP_DstInstr) == 0 || TAW_GET(tsp, TSP_FogCtrl) == 3 || TAW_GET(tsp, TSP_ColorClamp)
			|| !TAW_GET(tsp, TSP_UseAlpha)
			|| (TAW_GET(TAW_FULL(pp->pcw), PCW_Texture) && TAW_GET(tsp, TSP_ShadInstr) < 2));
}

static INLINE PrepassKey PrepassMakeKey(const PolyParam *pp, u64 texid)
{
	PrepassKey key;
	u32 tsp = TAW_SET(TAW_FULL(pp->tsp), TSP_SrcInstr, 0);
	if (TAW_GET(tsp, TSP_FogCtrl) != 3)
		tsp = TAW_SET(tsp, TSP_FogCtrl, 0);
	key.texid = TAW_GET(TAW_FULL(pp->pcw), PCW_Texture) ? texid : 0;
	key.tsp = tsp;
	key.tcw = TAW_FULL(pp->tcw);
	key.pcw = TAW_FULL(pp->pcw);
	key.isp = TAW_SET(0, ISP_TSP_CullMode, TAW_GET(TAW_FULL(pp->isp), ISP_TSP_CullMode));
	key.tileclip = pp->tileclip;
	return key;
}

/* The prepass of render pass list [first, first + count) of the
 * translucent polygons: its draws, added to @draws (ppid: a polygon of the
 * group, which says how all of it is drawn; first and count: of the pass's
 * indices, triangles), and their triangles' indices written to @out as
 * GenSortedTo()'s, with the provoking vertex as GenSortedTo() has it, where
 * the pass's @base'th index is (what its draws' first count from): how
 * many. @lastPass: see PrepassWanted(). */
u32 GenPrepassTo(int first, int count, bool lastPass, SortList *draws, u32 *out, u32 base,
		bool firstProvoking);
/* (the alpha the prepass compares against: 254.5 / 255, the least that writes as 255) */
#define PREPASS_ALPHA_MIN 0.998039216f

#ifdef __cplusplus
}
#endif

#endif
