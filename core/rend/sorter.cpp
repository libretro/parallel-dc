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
#include <string.h>
#include "sorter.h"
#include "sort_pairs.h"

/* (sorting: in memory kept from one frame to the next) */
static grow_buf_t sortPairs, sortPairsTmp;
/* SortPParams()'s, GenSortedTo()'s, GenPrepassTo()'s */
static grow_buf_t gathered, lstBuf, orderBuf, runsBuf, runsTmp;

struct IndexTrig
{
	u32 id[3];
	u16 pid;
	f32 z;
};

/* (as std::min(): the first when neither is less) */
static INLINE float min2(float a, float b)
{
	return b < a ? b : a;
}

static float minZ(const Vertex *v, const u32 *mod)
{
	return min2(min2(v[mod[0]].z, v[mod[1]].z), v[mod[2]].z);
}


void SortPParams(int first, int count)
{
	if (pvrrc.verts.used() == 0 || count <= 1)
		return;

	Vertex* vtx_base=pvrrc.verts.head();
	u32* idx_base = pvrrc.idx.head();

	PolyParam* pp = &pvrrc.global_param_tr.head()[first];
	PolyParam* pp_end = pp + count;

	while(pp!=pp_end)
	{
		if (pp->count<2)
		{
			pp->zvZ=0;
		}
		else
		{
			u32* idx = idx_base + pp->first;

			Vertex* vtx=vtx_base+idx[0];
			Vertex* vtx_end=vtx_base + idx[pp->count-1]+1;

			u32 zv=0xFFFFFFFF;
			while(vtx!=vtx_end)
			{
				if ((u32&)vtx->z < zv)
					zv = (u32&)vtx->z;
				vtx++;
			}

			pp->zvZ=(f32&)zv;
		}
		pp++;
	}

	/* sorted by zvZ, stably: the polygons gathered in that order, and put back */
	PolyParam * const base = pvrrc.global_param_tr.head() + first;
	sort_pair_t *pairs = (sort_pair_t *)grow_buf_reserve(&sortPairs, (size_t)count * sizeof(sort_pair_t));
	PolyParam *to = (PolyParam *)grow_buf_reserve(&gathered, (size_t)count * sizeof(PolyParam));
	if (pairs == NULL || to == NULL)
		return;
	for (int i = 0; i < count; i++)
	{
		pairs[i].key = sort_key(base[i].zvZ);
		pairs[i].index = (u32)i;
	}
	pairs = sort_pairs(pairs, (u32)count, &sortPairsTmp);
	if (pairs == NULL)
		return;
	for (int i = 0; i < count; i++)
		to[i] = base[pairs[i].index];
	memcpy(base, to, (size_t)count * sizeof(PolyParam));
}



//are two poly params the same?
static bool PP_EQ(const PolyParam *pp0, const PolyParam *pp1)
{
	return SortSameDraw(pp0, pp1);
}

static void fill_id(u32 *d, const Vertex *v0, const Vertex *v1, const Vertex *v2,  const Vertex *vb)
{	
	d[0] = (u32)(v0 - vb);
	d[1] = (u32)(v1 - vb);
	d[2] = (u32)(v2 - vb);
}

u32 SortedTriangles(int first, int count)
{
	const PolyParam *pp = pvrrc.global_param_tr.head() + first;
	const PolyParam * const pp_end = pp + count;
	u32 tris = 0;

	for (; pp < pp_end; pp++)
		if (pp->count > 2)
			tris += pp->count - 2;
	return tris;
}

SortList *SortPasses(grow_buf_t *lists, u32 passes)
{
	const size_t had = lists->size / sizeof(SortList);
	if (passes > had)
	{
		if (grow_buf_reserve_keep(lists, (size_t)passes * sizeof(SortList)) == NULL)
			return NULL;
		/* (the new ones empty: zeroed) */
		memset((SortList *)lists->data + had, 0, lists->size - had * sizeof(SortList));
	}
	return (SortList *)lists->data;
}

void SortPassesFree(grow_buf_t *lists)
{
	SortList *list = (SortList *)lists->data;
	for (size_t i = 0; i < lists->size / sizeof(SortList); i++)
		grow_buf_free(&list[i].buf);
	grow_buf_free(lists);
}

u32 GenSortedTo(int first, int count, SortList *draws, u32 *out, bool firstProvoking)
{
	draws->count = 0;

	if (pvrrc.verts.used() == 0 || count == 0)
		return 0;

	const Vertex * const vtx_base = pvrrc.verts.head();
	const u32 * const idx_base = pvrrc.idx.head();

	const PolyParam * const pp_base = &pvrrc.global_param_tr.head()[first];
	const PolyParam *pp = pp_base;
	const PolyParam * const pp_end = pp + count;
	while (pp->count == 0 && pp < pp_end)
		pp++;
	if (pp == pp_end)
		return 0;

	const int vtx_count = pvrrc.verts.used() - idx_base[pp->first];
	if (vtx_count<=0)
		return 0;

	/* every triangle, with its polygon and its vertices; and a pair of its
	 * depth and its place, which is what is sorted. (Kept from one frame to
	 * the next: nothing allocated once they have grown.) */
	const u32 tris = SortedTriangles(first, count);
	IndexTrig *lst = (IndexTrig *)grow_buf_reserve(&lstBuf, (size_t)(tris ? tris : 1) * sizeof(IndexTrig));
	sort_pair_t *pairs = (sort_pair_t *)grow_buf_reserve(&sortPairs, (size_t)(tris ? tris : 1) * sizeof(sort_pair_t));
	if (lst == NULL || pairs == NULL)
		return 0;

	u32 pfsti=0;

	while(pp!=pp_end)
	{
		u32 ppid = (u32)(pp - pp_base);

		if (pp->count>2)
		{
			const u32 *idx = idx_base + pp->first;
			u32 flip = 0;

			for (u32 i = 0; i < pp->count - 2; i++)
			{
				const Vertex *v0, *v1, *v2;
				if (flip && firstProvoking)
				{
					// The same winding, starting with the vertex that holds
					// this triangle's colour (strip vertex i, not i + 1).
					v0 = vtx_base + idx[i];
					v1 = vtx_base + idx[i + 2];
					v2 = vtx_base + idx[i + 1];
				}
				else if (flip)
				{
					v0 = vtx_base + idx[i + 1];
					v1 = vtx_base + idx[i];
					v2 = vtx_base + idx[i + 2];
				}
				else
				{
					v0 = vtx_base + idx[i];
					v1 = vtx_base + idx[i + 1];
					v2 = vtx_base + idx[i + 2];
				}
				fill_id(lst[pfsti].id,v0,v1,v2,vtx_base);
				lst[pfsti].pid= ppid ;
				lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
				pairs[pfsti].key = sort_key(lst[pfsti].z);
				pairs[pfsti].index = pfsti;
				pfsti++;

				flip ^= 1;
			}
		}
		pp++;
	}

	const u32 aused=pfsti;

	//sort them: by depth, those at the same depth in the order sent
	pairs = sort_pairs(pairs, aused, &sortPairsTmp);
	if (pairs == NULL)
		return 0;

	//Merge pids/draw cmds if two different pids are actually equal
	for (u32 k=1;k<aused;k++)
	{
		IndexTrig& cur = lst[pairs[k].index];
		const IndexTrig& prev = lst[pairs[k - 1].index];
		if (cur.pid!=prev.pid && PP_EQ(&pp_base[cur.pid],&pp_base[prev.pid]))
			cur.pid=prev.pid;
	}

	//re-assemble them into drawing commands: at most one a triangle
	SortTrigDrawParam *list = (SortTrigDrawParam *)grow_buf_reserve(&draws->buf, (size_t)(aused ? aused : 1) * sizeof(SortTrigDrawParam));
	if (list == NULL)
		return 0;
	u32 n = 0;
	int idx=-1;

	for (u32 i=0; i<aused; i++, out += 3)
	{
		const IndexTrig& t = lst[pairs[i].index];
		const int pid=t.pid;

		// (stored in order, whole: @out may be memory the card reads)
		out[0]=t.id[0];
		out[1]=t.id[1];
		out[2]=t.id[2];

		if (idx!=pid)
		{
			if (idx!=-1)
				list[n - 1].count = i * 3 - list[n - 1].first;
			list[n].ppid = pp_base + pid;
			list[n].first = i * 3;
			list[n].count = 0;
			n++;
			idx=pid;
		}
	}

	if (n != 0)
		list[n - 1].count = aused * 3 - list[n - 1].first;
	draws->count = n;

	return aused * 3;
}

/* Whether a polygon that blends with one minus its alpha can have a pixel
 * at alpha 1, as far as its vertices say: not if its alpha is theirs (or
 * theirs times the texture's) and no vertex has 255 */
static bool PrepassVerticesOpaque(const PolyParam *pp, const Vertex *vtx_base, const u32 *idx_base)
{
	if (!PrepassAlphaFromVertices(pp))
		return true;
	const u32 *idx = idx_base + pp->first;
	for (u32 i = 0; i < pp->count; i++)
		if (vtx_base[idx[i]].col[3] == 255)
			return true;
	return false;
}

/* The polygons that can hide, in their order (order), in runs of
 * neighbours drawn alike (runs): a model's polygons come one after the
 * other, so it is the runs that are sorted, few of them. */
struct PrepassRun
{
	PrepassKey key;
	u32 start, count;	// in order
	u32 tris;		// the most triangles they make
};

/* @n runs sorted by key, stably (so those of a key keep the order they
 * were made in, by start): merged from runs of one up, through @tmp; where
 * they are, after */
static PrepassRun *sort_runs(PrepassRun *runs, PrepassRun *tmp, u32 n)
{
	PrepassRun *from = runs, *to = tmp;
	for (u32 width = 1; width < n; width *= 2)
	{
		for (u32 lo = 0; lo < n; lo += 2 * width)
		{
			const u32 mid = lo + width < n ? lo + width : n;
			const u32 hi = lo + 2 * width < n ? lo + 2 * width : n;
			u32 a = lo, b = mid, k = lo;
			while (a < mid && b < hi)
				to[k++] = from[b].key < from[a].key ? from[b++] : from[a++];
			while (a < mid)
				to[k++] = from[a++];
			while (b < hi)
				to[k++] = from[b++];
		}
		PrepassRun *t = from;
		from = to;
		to = t;
	}
	return from;
}

u32 GenPrepassTo(int first, int count, bool lastPass, SortList *draws, u32 *out, u32 base,
		bool firstProvoking)
{
	/* (kept from one frame to the next: no allocation once they have grown) */

	if (count <= 0 || pvrrc.verts.used() == 0 || !PrepassWanted(lastPass))
		return 0;
	const Vertex * const vtx_base = pvrrc.verts.head();
	const u32 * const idx_base = pvrrc.idx.head();
	const PolyParam * const pp_base = pvrrc.global_param_tr.head();

	/* at most a run and an entry of order a polygon */
	u32 *order = (u32 *)grow_buf_reserve(&orderBuf, (size_t)count * sizeof(u32));
	PrepassRun *runs = (PrepassRun *)grow_buf_reserve(&runsBuf, (size_t)count * sizeof(PrepassRun));
	PrepassRun *tmp = (PrepassRun *)grow_buf_reserve(&runsTmp, (size_t)count * sizeof(PrepassRun));
	if (order == NULL || runs == NULL || tmp == NULL)
		return 0;
	u32 norder = 0, nruns = 0;
	for (int n = first; n < first + count; n++)
	{
		const PolyParam *pp = pp_base + n;
		if (!PrepassCandidate(pp, lastPass) || !PrepassVerticesOpaque(pp, vtx_base, idx_base))
			continue;
		const PrepassKey key = PrepassMakeKey(pp, pp->texid);
		if (nruns != 0 && runs[nruns - 1].start + runs[nruns - 1].count == norder && runs[nruns - 1].key == key)
		{
			runs[nruns - 1].count++;
			runs[nruns - 1].tris += pp->count - 2;
		}
		else
		{
			runs[nruns].key = key;
			runs[nruns].start = norder;
			runs[nruns].count = 1;
			runs[nruns].tris = pp->count - 2;
			nruns++;
		}
		order[norder++] = (u32)n;
	}
	if (nruns == 0)
		return 0;
	runs = sort_runs(runs, tmp, nruns);

	/* the draws: at most one a run more than there are */
	SortTrigDrawParam *list = (SortTrigDrawParam *)grow_buf_reserve_keep(&draws->buf,
			((size_t)draws->count + nruns) * sizeof(SortTrigDrawParam));
	if (list == NULL)
		return 0;
	u32 nd = draws->count;
	const u32 had = nd;

	// the triangles, as GenSortedTo() makes them of a strip, written in place
	u32 *dst = out;
	for (u32 k = 0; k < nruns; k++)
	{
		const PrepassRun& r = runs[k];
		if (k == 0 || !(r.key == runs[k - 1].key))
		{
			// (a group before it all of whose triangles drew nothing is not kept)
			if (k != 0 && list[nd - 1].count == 0)
				nd--;
			list[nd].ppid = pp_base + order[r.start];
			list[nd].first = (u32)(base + (dst - out));
			list[nd].count = 0;
			nd++;
		}
		u32 * const runStart = dst;
		for (u32 j = r.start; j < r.start + r.count; j++)
		{
			const PolyParam *pp = pp_base + order[j];
			const u32 *idx = idx_base + pp->first;
			u32 flip = 0;
			for (u32 i = 0; i + 2 < pp->count; i++, flip ^= 1)
			{
				const u32 a = idx[i], b = idx[i + 1], c = idx[i + 2];
				// (a triangle with two corners the same draws nothing)
				if (a == b || b == c || a == c)
					continue;
				dst[0] = a;
				if (flip && firstProvoking)
				{
					dst[1] = c;
					dst[2] = b;
				}
				else if (flip)
				{
					dst[0] = b;
					dst[1] = a;
					dst[2] = c;
				}
				else
				{
					dst[1] = b;
					dst[2] = c;
				}
				dst += 3;
			}
		}
		list[nd - 1].count += (u32)(dst - runStart);
	}
	if (nd > had && list[nd - 1].count == 0)
		nd--;
	draws->count = nd;
	return (u32)(dst - out);
}

u32 SortPassesTo(grow_buf_t *sortedLists, grow_buf_t *prepassLists, u32 *out, bool prepasses, bool firstProvoking)
{
	const u32 passes = (u32)pvrrc.render_passes.used();
	SortList * const sorted = SortPasses(sortedLists, passes);
	SortList * const prepass = SortPasses(prepassLists, passes);
	if (sorted == NULL || prepass == NULL)
	{
		/* (no memory: nothing drawn sorted, rather than the last frame's) */
		for (u32 i = 0; sorted != NULL && i < passes; i++)
			sorted[i].count = 0;
		for (u32 i = 0; prepass != NULL && i < passes; i++)
			prepass[i].count = 0;
		return 0;
	}
	u32 total = 0;
	int previous = 0;

	for (u32 pass = 0; pass < passes; pass++)
	{
		const RenderPass& current = pvrrc.render_passes.head()[pass];
		sorted[pass].count = 0;
		prepass[pass].count = 0;
		if (current.autosort)
		{
			const int count = current.tr_count - previous;
			u32 n = GenSortedTo(previous, count, &sorted[pass], out, firstProvoking);
			if (prepasses)
				n += GenPrepassTo(previous, count, pass == passes - 1, &prepass[pass], out + n, n, firstProvoking);
			SortTrigDrawParam *draw = SortDraws(&sorted[pass]);
			for (u32 i = 0; i < sorted[pass].count; i++)
				draw[i].first += total;
			draw = SortDraws(&prepass[pass]);
			for (u32 i = 0; i < prepass[pass].count; i++)
				draw[i].first += total;
			total += n;
			out += n;
		}
		previous = current.tr_count;
	}
	return total;
}

u32 SortPassesRoom(void)
{
	u32 tris = 0;
	int previous = 0;

	for (int pass = 0; pass < pvrrc.render_passes.used(); pass++)
	{
		const RenderPass& current = pvrrc.render_passes.head()[pass];
		if (current.autosort)
			tris += SortedTriangles(previous, current.tr_count - previous);
		previous = current.tr_count;
	}
	return tris * 6;
}

/* The translucent polygons a frame can have (TA_context::Alloc()'s list),
 * and the triangles of them sorted that a frame mostly has: reserved
 * before the first frame, so that a frame up to them allocates nothing */
#define SORT_RESERVE_POLYS (10240 * (ta_data_size() / TA_DATA_SIZE))
#define SORT_RESERVE_TRIS 65536
#define SORT_RESERVE_DRAWS 1024

void SortReserve(void)
{
	const size_t polys = SORT_RESERVE_POLYS;
	const size_t pairs = polys > SORT_RESERVE_TRIS ? polys : SORT_RESERVE_TRIS;
	grow_buf_reserve(&sortPairs, pairs * sizeof(sort_pair_t));
	grow_buf_reserve(&sortPairsTmp, pairs * sizeof(sort_pair_t));
	grow_buf_reserve(&gathered, polys * sizeof(PolyParam));
	grow_buf_reserve(&lstBuf, (size_t)SORT_RESERVE_TRIS * sizeof(IndexTrig));
	grow_buf_reserve(&orderBuf, polys * sizeof(u32));
	grow_buf_reserve(&runsBuf, polys * sizeof(PrepassRun));
	grow_buf_reserve(&runsTmp, polys * sizeof(PrepassRun));
}

void SortPassesReserve(grow_buf_t *lists)
{
	/* (the render passes a frame can have: TA_context's) */
	SortList *list = SortPasses(lists, 10);
	u32 i;
	for (i = 0; list != NULL && i < 10; i++)
		grow_buf_reserve_keep(&list[i].buf, (size_t)SORT_RESERVE_DRAWS * sizeof(SortTrigDrawParam));
}
