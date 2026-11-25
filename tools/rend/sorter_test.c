/* The translucent polygons sorted (core/rend/sorter.c): frames of strips
 * made at random - depths alike and not, polygons drawn alike and not,
 * corners repeated, render passes sorted and not - put through every
 * function the sorter has, the prepass's tests of a polygon among them,
 * with and without the prepasses and both provoking vertices. What it
 * prints is hashes of what came out; they must be what they always were
 * (sorter_expected.txt). See sorter_run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/sorter.h"
#include "rend/vulkan/pso_key.h"

#ifdef SORTER_TEST_OLD
/* (the old code's C++ functions, and its header's, through a shim) */
#define SortPParams old_SortPParams
#define SortedTriangles old_SortedTriangles
#define SortPasses old_SortPasses
#define SortPassesFree old_SortPassesFree
#define SortReserve old_SortReserve
#define SortPassesReserve old_SortPassesReserve
#define SortListReserve old_SortListReserve
#define SortIndicesReserve old_SortIndicesReserve
#define SortPassesRoom old_SortPassesRoom
#define SortPassesTo old_SortPassesTo
#define GenSortedTo old_GenSortedTo
#define GenPrepassTo old_GenPrepassTo
#define SortSameDraw old_SortSameDraw
#define PrepassWanted old_PrepassWanted
#define PrepassAlwaysHides old_PrepassAlwaysHides
#define PrepassCandidate old_PrepassCandidate
#define PrepassAlphaFromVertices old_PrepassAlphaFromVertices
#define PrepassMakeKey old_PrepassMakeKey
#define PrepassKeyEq old_PrepassKeyEq
#define PrepassKeyLess old_PrepassKeyLess
void SortPParams(int first, int count);
u32 SortedTriangles(int first, int count);
SortList *SortPasses(grow_buf_t *lists, u32 passes);
void SortPassesFree(grow_buf_t *lists);
void SortReserve(void);
void SortPassesReserve(grow_buf_t *lists);
void SortListReserve(SortList *list);
void SortIndicesReserve(grow_buf_t *buf);
u32 SortPassesRoom(void);
u32 SortPassesTo(grow_buf_t *sorted, grow_buf_t *prepass, u32 *out, bool prepasses, bool firstProvoking);
u32 GenSortedTo(int first, int count, SortList *draws, u32 *out, bool firstProvoking);
u32 GenPrepassTo(int first, int count, bool lastPass, SortList *draws, u32 *out, u32 base, bool firstProvoking);
bool SortSameDraw(const PolyParam *pp0, const PolyParam *pp1);
bool PrepassWanted(bool lastPass);
bool PrepassAlwaysHides(const PolyParam *pp);
bool PrepassCandidate(const PolyParam *pp, bool lastPass);
bool PrepassAlphaFromVertices(const PolyParam *pp);
PrepassKey PrepassMakeKey(const PolyParam *pp, u64 texid);
bool PrepassKeyEq(const PrepassKey *a, const PrepassKey *b);
bool PrepassKeyLess(const PrepassKey *a, const PrepassKey *b);
#endif

settings_t settings;
TA_context *_pvrrc;

static u32 h = 2166136261u;
static void hb(const void *p, size_t n)
{
	const u8 *b = (const u8 *)p;
	size_t i;
	for (i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
}
static void hu(u32 v) { hb(&v, sizeof(v)); }

static u32 rng = 0x2545F491u;
static u32 rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

#define MAX_VERTS 6000
#define MAX_POLYS 600
#define MAX_IDX (MAX_POLYS * 9)

static TA_context ctx;
#define VERTS TA_LIST_HEAD(ctx.rend.verts, Vertex)
#define IDX TA_LIST_HEAD(ctx.rend.idx, u32)
#define POLYS TA_LIST_HEAD(ctx.rend.global_param_tr, PolyParam)
#define PASSES TA_LIST_HEAD(ctx.rend.render_passes, RenderPass)

/* How polygons are drawn: a handful of ways, so that some are alike */
typedef struct style
{
	u32 pcw, isp, tsp, tcw, tileclip;
	u64 texid;
} style_t;
static style_t styles[8];

static void make_styles(void)
{
	static const u32 dst[6] = { 0, 5, 5, 0, 1, 4 };
	int i;
	for (i = 0; i < 8; i++)
	{
		style_t *s = &styles[i];
		s->pcw = rnd();
		s->isp = rnd();
		s->tsp = rnd();
		s->tcw = rnd();
		s->tileclip = rnd() % 3;
		s->texid = (u64)rnd() << 32 | rnd();
		s->tsp = TAW_SET(s->tsp, TSP_DstInstr, dst[rnd() % 6]);
		if (rnd() % 4 != 0)
		{
			s->tsp = TAW_SET(s->tsp, TSP_SrcSelect, 0);
			s->tsp = TAW_SET(s->tsp, TSP_DstSelect, 0);
		}
		if (rnd() % 3 != 0)
			s->tsp = TAW_SET(s->tsp, TSP_ColorClamp, 0);
		if (rnd() % 3 != 0)
			s->tsp = TAW_SET(s->tsp, TSP_UseAlpha, 1);
		if (rnd() % 2)
			s->isp = TAW_SET(s->isp, ISP_TSP_ZWriteDis, 0);
	}
	/* a trilinear pass whose alpha is scaled by more than D's low bits */
	styles[0].tsp = TAW_SET(styles[0].tsp, TSP_DstInstr, 5);
	styles[0].tsp = TAW_SET(styles[0].tsp, TSP_SrcSelect, 0);
	styles[0].tsp = TAW_SET(styles[0].tsp, TSP_DstSelect, 0);
	styles[0].tsp = TAW_SET(styles[0].tsp, TSP_FilterMode, 2);
	styles[0].tsp = TAW_SET(styles[0].tsp, TSP_MipMapD, rnd() & 4);
	styles[0].pcw = TAW_SET(styles[0].pcw, PCW_Texture, 1);
	styles[0].tcw = TAW_SET(styles[0].tcw, TCW_MipMapped, 1);
	/* and pairs that differ in one thing: the texture word, the clipping,
	 * the bit of the control word that does not change the drawing, and
	 * only where the prepass does not look */
	styles[4] = styles[1];
	styles[4].tcw ^= 1u << (rnd() % 32);
	styles[5] = styles[1];
	styles[5].tileclip ^= 4;
	styles[6] = styles[2];
	styles[6].pcw ^= 1;
	styles[7] = styles[3];
	styles[7].tsp = TAW_SET(styles[7].tsp, TSP_SrcInstr, TAW_GET(styles[7].tsp, TSP_SrcInstr) + 1);
	styles[7].isp = TAW_SET(styles[7].isp, ISP_TSP_DepthMode, TAW_GET(styles[7].isp, ISP_TSP_DepthMode) + 1);
}

/* A frame: @n polygons of strips in @passes render passes */
static void make_frame(int n, int passes)
{
	int nv = 0, ni = 0, p, i;
	float zs[4];
	ta_list_clear(&ctx.rend.verts.l);
	ta_list_clear(&ctx.rend.idx.l);
	ta_list_clear(&ctx.rend.global_param_tr.l);
	ta_list_clear(&ctx.rend.render_passes.l);
	for (i = 0; i < 4; i++)
		zs[i] = (float)(rnd() % 1000) / 7.0f + 0.001f;
	for (p = 0; p < n; p++)
	{
		PolyParam *pp = (PolyParam *)ta_list_append(&ctx.rend.global_param_tr.l, sizeof(PolyParam));
		const style_t *s = &styles[rnd() % 8];
		u32 count = rnd() % 10, k;
		if (count == 9)
			count = 1 + rnd() % 2;
		memset(pp, 0, sizeof(*pp));
		pp->first = (u32)ni;
		pp->count = count;
		TAW_FULL(pp->pcw) = s->pcw;
		TAW_FULL(pp->isp) = s->isp;
		TAW_FULL(pp->tsp) = s->tsp;
		TAW_FULL(pp->tcw) = s->tcw;
		pp->tileclip = s->tileclip;
		pp->texid = s->texid;
		pp->zvZ = 1234.5f;
		for (k = 0; k < count; k++)
		{
			Vertex *v = (Vertex *)ta_list_append(&ctx.rend.verts.l, sizeof(Vertex));
			u32 *ix = (u32 *)ta_list_append(&ctx.rend.idx.l, sizeof(u32));
			u32 r = rnd() % 10;
			memset(v, 0, sizeof(*v));
			v->x = (float)(rnd() % 640);
			v->y = (float)(rnd() % 480);
			v->z = r < 3 ? zs[rnd() % 4] : (float)(rnd() % 100000) / 13.0f + 0.0001f;
			v->col[0] = (u8)rnd();
			v->col[3] = rnd() % 3 == 0 ? 255 : (u8)(rnd() % 255);
			/* (now and then a corner the same as the one before, or the
			 * one before that) */
			r = rnd() % 16;
			*ix = k > 0 && r == 0 ? (u32)(nv - 1) : k > 1 && r == 1 ? IDX[pp->first + k - 2] : (u32)nv;
			nv++;
			ni++;
		}
	}
	for (i = 0, p = 0; i < passes; i++)
	{
		RenderPass *rp = (RenderPass *)ta_list_append(&ctx.rend.render_passes.l, sizeof(RenderPass));
		int end = i == passes - 1 ? n : n * (i + 1) / passes - (int)(rnd() % 3);
		memset(rp, 0, sizeof(*rp));
		rp->autosort = rnd() % 4 != 0;
		p = end > p ? end : p;
		rp->tr_count = (u32)p;
	}
}

static void hash_draws(const SortList *list)
{
	u32 i;
	const SortTrigDrawParam *d = SortDraws(list);
	hu(list->count);
	for (i = 0; i < list->count; i++)
	{
		hu((u32)(d[i].ppid - POLYS));
		hu(d[i].first);
		hu(d[i].count);
	}
}

static void hash_key(const PrepassKey *k)
{
	hb(&k->texid, sizeof(k->texid));
	hu(k->tsp);
	hu(k->tcw);
	hu(k->pcw);
	hu(k->isp);
	hu(k->tileclip);
}

static u32 out[MAX_IDX * 6 + 64];
static grow_buf_t sorted, prepass;
static SortList one;

/* The polygons by themselves: how the prepass sees each, and two
 * neighbours compared */
static void polygons(int n)
{
	int i;
	for (i = 0; i < n; i++)
	{
		const PolyParam *pp = POLYS + i;
		const PolyParam *pq = POLYS + (i + 1) % n;
		PrepassKey a = PrepassMakeKey(pp, pp->texid);
		PrepassKey b = PrepassMakeKey(pq, i & 1 ? pq->texid : 77);
		hu(SortSameDraw(pp, pq));
		hu(PrepassAlwaysHides(pp));
		hu(PrepassCandidate(pp, true));
		hu(PrepassCandidate(pp, false));
		hu(PrepassAlphaFromVertices(pp));
		hash_key(&a);
		hash_key(&b);
		hu(PrepassKeyEq(&a, &b));
		hu(PrepassKeyEq(&a, &a));
		hu(PrepassKeyLess(&a, &b));
		hu(PrepassKeyLess(&b, &a));
		hu(PrepassKeyLess(&a, &a));
	}
	hu(PrepassWanted(true));
	hu(PrepassWanted(false));
}

static void frame(int n, int passes)
{
	int c, first, count;
	u32 total, k;

	make_frame(n, passes);
	polygons(n);
	hu(SortedTriangles(0, n));
	hu(SortPassesRoom());

	for (c = 0; c < 4; c++)
	{
		u32 pass;
		total = SortPassesTo(&sorted, &prepass, out, c & 1, (c >> 1) & 1);
		hu(total);
		hb(out, total * sizeof(u32));
		for (pass = 0; pass < (u32)passes; pass++)
		{
			u32 cnt;
			const SortTrigDrawParam *d = SortPassDraws(&sorted, pass, &cnt);
			hu(cnt);
			hu(d != NULL);
			hash_draws(SortPass(&sorted, pass));
			hash_draws(SortPass(&prepass, pass));
		}
		hu(SortPass(&sorted, passes) == NULL);
	}

	/* a part of them, each way */
	first = n ? (int)(rnd() % n) : 0;
	count = n - first ? (int)(rnd() % (n - first + 1)) : 0;
	for (c = 0; c < 2; c++)
	{
		total = GenSortedTo(first, count, &one, out, c != 0);
		hu(total);
		hb(out, total * sizeof(u32));
		hash_draws(&one);
		for (k = 0; k < 2; k++)
		{
			u32 more = GenPrepassTo(first, count, k != 0, &one, out + total, total + 5, c != 0);
			hu(more);
			hb(out + total, more * sizeof(u32));
			hash_draws(&one);
		}
	}

	/* sorted by strip: the polygons' order, and their depths */
	SortPParams(first, count);
	hb(POLYS, (size_t)n * sizeof(PolyParam));
	SortPParams(0, n);
	hb(POLYS, (size_t)n * sizeof(PolyParam));
	/* ...and by triangle again, of the polygons in their new order */
	total = SortPassesTo(&sorted, &prepass, out, true, false);
	hu(total);
	hb(out, total * sizeof(u32));
}

int main(void)
{
	/* the prepass's alpha as the Vulkan shader has it (shaders/prepass.frag,
	 * pso_key.h), both stored as floats (on 32-bit x86, whose x87 keeps
	 * floats wider, the literals are otherwise compared at the wider
	 * precision) */
	volatile float alpha = PREPASS_ALPHA_MIN, alphaGlsl = (float)PREPASS_ALPHA_MIN_GLSL;
	int f;
	SortList *lists;

	if (alpha != alphaGlsl)
	{
		printf("sorter_test: FAIL: PREPASS_ALPHA_MIN %.9g, the Vulkan shader's %.9g\n", alpha, alphaGlsl);
		return 1;
	}

	ta_list_init(&ctx.rend.verts.l, MAX_VERTS * sizeof(Vertex), sizeof(Vertex), (unsigned char *)&ctx.rend.Overrun, NULL);
	ta_list_init(&ctx.rend.idx.l, MAX_IDX * sizeof(u32), sizeof(u32), (unsigned char *)&ctx.rend.Overrun, NULL);
	ta_list_init(&ctx.rend.global_param_tr.l, MAX_POLYS * sizeof(PolyParam), sizeof(PolyParam), (unsigned char *)&ctx.rend.Overrun, NULL);
	ta_list_init(&ctx.rend.render_passes.l, 10 * sizeof(RenderPass), sizeof(RenderPass), (unsigned char *)&ctx.rend.Overrun, NULL);
	_pvrrc = &ctx;
	settings.System = DC_PLATFORM_DREAMCAST;

	SortReserve();
	SortPassesReserve(&sorted);
	SortListReserve(&one);
	hu((u32)sorted.size);
	hu(one.buf.size >= 1024 * sizeof(SortTrigDrawParam));
	lists = SortPasses(&prepass, 3);
	hu(lists != NULL && lists[2].count == 0 && lists[2].buf.data == NULL);
	{
		/* (one more list than there is room for: made, empty) */
		grow_buf_t g;
		u32 more;
		memset(&g, 0, sizeof(g));
		lists = SortPasses(&g, 1);
		more = (u32)(g.size / sizeof(SortList)) + 1;
		hu(more);
		lists[0].count = 5;
		lists = SortPasses(&g, more);
		hu(lists[0].count);
		hu(lists[more - 1].count == 0 && lists[more - 1].buf.data == NULL);
		hu((u32)g.size);
		SortPassesFree(&g);
		hu(g.data == NULL);
	}
	{
		grow_buf_t ib;
		memset(&ib, 0, sizeof(ib));
		SortIndicesReserve(&ib);
		hu((u32)ib.size);
		grow_buf_free(&ib);
	}
	printf("reserve: %08x\n", h);

	/* nothing to sort */
	make_frame(0, 1);
	hu(SortPassesTo(&sorted, &prepass, out, true, true));
	hu(SortPassesRoom());
	hu(GenSortedTo(0, 0, &one, out, false));
	hu(GenPrepassTo(0, 0, true, &one, out, 0, false));
	SortPParams(0, 0);
	printf("empty: %08x\n", h);

	for (f = 0; f < 300; f++)
	{
		if (f % 25 == 0)
			make_styles();
		settings.rend.TranslucentPolygonDepthMask = (f / 7) & 1;
		frame(f % 50 == 49 ? MAX_POLYS : 1 + (int)(rnd() % 120), 1 + (int)(rnd() % 4));
		if (f % 50 == 49)
			printf("frames to %d: %08x\n", f + 1, h);
	}

	SortPassesFree(&sorted);
	SortPassesFree(&prepass);
	grow_buf_free(&one.buf);
	hu(sorted.data == NULL && sorted.size == 0);
	printf("freed: %08x\n", h);
	ta_list_free(&ctx.rend.verts.l);
	ta_list_free(&ctx.rend.idx.l);
	ta_list_free(&ctx.rend.global_param_tr.l);
	ta_list_free(&ctx.rend.render_passes.l);
	return 0;
}
