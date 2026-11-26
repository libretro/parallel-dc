/* GPU-side TA processing's decode against the core's own parse, on the
 * frames a game draws. Linked into one program with the core's objects and
 * tools/threads/headless.c as the frontend (ta_gpu_test.sh): the link
 * puts this file's __wrap_ functions in the place of ta_parse_vdrc() and
 * of headless.c's dlopen(), which then opens the program itself.
 *
 * For each frame the renderer parses: the header pass and the decode's
 * tables (ta_gpu_prepare()), the full parse as it always runs, and the
 * decode, on the device and as ta_gpu_test.c's model; the vertices, the
 * modifier volume triangles, the indices, every polygon parameter's index
 * range, the render passes' counts, fZ_max, the draw commands and whether
 * the frame is overrun are then compared with what the parse made, byte
 * for byte. The totals are printed at the end; the first differences as
 * they come. */
#include "types.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/ta_gpu.h"
#include "rend/depth_scale.h"
#include "rend/sorter.h"
#include "rend/shadows.h"
#include "ta_gpu_test.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void *__real_dlopen(const char *file, int mode);
void *__wrap_dlopen(const char *file, int mode)
{
	return __real_dlopen(file != NULL && strcmp(file, "self") == 0 ? NULL : file, mode);
}

bool __real_ta_parse_vdrc(TA_context *ctx);

static double now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* ---- a growing array of elements of one size ---- */
typedef struct vec
{
	u8 *p;
	size_t n, cap, elem;
} vec;
#define VEC(T) { NULL, 0, 0, sizeof(T) }
#define VAT(v, T, i) (((T *)(v).p)[i])

/* @k more at the end: where they go */
static void *vec_add(vec *v, size_t k)
{
	if (v->n + k > v->cap)
	{
		size_t cap = v->cap ? v->cap : 64;
		while (cap < v->n + k)
			cap *= 2;
		v->p = (u8 *)realloc(v->p, cap * v->elem);
		v->cap = cap;
	}
	v->n += k;
	return v->p + (v->n - k) * v->elem;
}
static void vec_push(vec *v, const void *e) { memcpy(vec_add(v, 1), e, v->elem); }
static void vec_assign(vec *v, const void *from, size_t k)
{
	v->n = 0;
	if (k != 0)
		memcpy(vec_add(v, k), from, k * v->elem);
}
static bool vec_same(const vec *a, const vec *b)
{
	return a->n == b->n && (a->n == 0 || !memcmp(a->p, b->p, a->n * a->elem));
}

typedef struct pair32 { u32 a, b; } pair32;
typedef struct pair64 { u64 a; u32 b; u32 pad; } pair64;
static int by_pair32(const void *x, const void *y)
{
	const pair32 *p = (const pair32 *)x, *q = (const pair32 *)y;
	return p->a != q->a ? (p->a < q->a ? -1 : 1) : p->b != q->b ? (p->b < q->b ? -1 : 1) : 0;
}
static int by_pair64(const void *x, const void *y)
{
	const pair64 *p = (const pair64 *)x, *q = (const pair64 *)y;
	return p->a != q->a ? (p->a < q->a ? -1 : 1) : p->b != q->b ? (p->b < q->b ? -1 : 1) : 0;
}
static int by_double(const void *x, const void *y)
{
	const double a = *(const double *)x, b = *(const double *)y;
	return a < b ? -1 : a > b;
}
static void push_pair32(vec *v, u32 a, u32 b)
{
	pair32 *p = (pair32 *)vec_add(v, 1);
	p->a = a;
	p->b = b;
}
static void push_double(vec *v, double d) { vec_push(v, &d); }

/* A list's polygon parameters */
#define PP_N(list) ((int)((PolyParam *)(list)->cur - (PolyParam *)(list)->base))
#define PP_AT(list, i) (((PolyParam *)(list)->base)[i])

static struct
{
	u32 seen, prepared, compared, same, differ, model_differ, scan_differ;
	u32 no_table, fallback, empty, corner, overrun, overrun_agree;
	u32 drawn, draw_same, draw_differ, draw_left;	/* the frames made ready to draw (TA_GPU_DRAW) */
	u32 draw_why[TA_GPU_WHY_COUNT];
	u32 edge_frames, edge_trials, edge_agree;	/* the overrun check at the exact boundary */
	u32 verts, idx, strips;
	vec prep_us, parse_us;
	vec draw_us;	/* the header pass made ready to draw, sort and shadows and all (TA_GPU_DRAW) */
	vec flat_us;	/* ...of it, the flat strips found (TAG_FLAT) */
	double gpu_ms;
	/* the translucent triangles sorted from the frame's depths (TA_GPU_SORT), against GenSortedTo() */
	u32 sort_frames, sort_same, sort_differ;
	u64 sort_tris, sort_runs;
	/* ...the depth prepass's checks (TAG_K_PREPASS): checks, the prepass's
	 * triangles and those the GPU left out, and their areas in pixels of
	 * the 640 x 480 screen */
	u64 prep_checks, prep_tris, prep_out;
	double prep_area, prep_area_out;
	vec sort_us, gensorted_us;
	/* the draw groups drawn again for shadows (TA_GPU_SHADOWS), against PolyOverlaps() */
	u32 shadow_frames, shadow_same, shadow_differ;
	/* the raster renderers' volume triangles with those far out cut (rend/modvol_clip.h), from the blocks against the parse's */
	u32 mvclip_frames, mvclip_differ;
	u64 mvclip_tris;
	u64 shadow_groups, shadow_drawn;
} st;

static TaGpuLayout layout;
static u32 *table;
static u32 table_room;
static bool ready;
/* The device is OpenGL (TAG_GL=1, ta_gpu_test_gl.c): its renderers take a flat
 * triangle's colours from its last vertex, so the frames are made without
 * TA_GPU_PROVOKE, and GenSortedTo() and the GPU sort wind as for OpenGL */
static bool gl_device;
/* the frame as the header pass takes it, before the parse */
static PolyParam saved_bg;
static FB_X_CLIP_type saved_x_clip;
static FB_Y_CLIP_type saved_y_clip;

static double pct(const vec *v, double q)
{
	static vec s = VEC(double);
	size_t at;
	if (v->n == 0)
		return 0;
	vec_assign(&s, v->p, v->n);
	qsort(s.p, s.n, sizeof(double), by_double);
	at = (size_t)(q * s.n);
	return VAT(s, double, at < s.n - 1 ? at : s.n - 1);
}

static double mean(const vec *v)
{
	double t = 0;
	size_t i;
	for (i = 0; i < v->n; i++)
		t += VAT(*v, double, i);
	return v->n == 0 ? 0 : t / v->n;
}

static void report(void)
{
	printf("ta_gpu_test: %u frames, %u decoded on the GPU, %u compared: %u the same, %u different (model against GPU: %u different, scan %u)\n",
			st.seen, st.prepared, st.compared, st.same, st.differ, st.model_differ, st.scan_differ);
	printf("ta_gpu_test: not decoded: %u without a table, %u the header pass left to the full parse; not compared: %u with nothing to draw, "
			"%u where a parameter came inside an unterminated strip, %u overrun (%u with the GPU's overrun flag the same)\n",
			st.no_table, st.fallback, st.empty, st.corner, st.overrun, st.overrun_agree);
	if (st.compared)
		printf("ta_gpu_test: per frame %.0f vertices, %.0f indices, %.0f strips; the header pass and tables mean %.1f us, p99 %.1f us, max %.1f us; "
				"the full parse mean %.1f us, p99 %.1f us, max %.1f us; the device (submit to fence) mean %.2f ms\n",
				(double)st.verts / st.compared, (double)st.idx / st.compared, (double)st.strips / st.compared,
				mean(&st.prep_us), pct(&st.prep_us, 0.99), pct(&st.prep_us, 1.0),
				mean(&st.parse_us), pct(&st.parse_us, 0.99), pct(&st.parse_us, 1.0), st.gpu_ms / st.compared);
	printf("ta_gpu_test: made ready to draw from the GPU: %u frames, %u with the parse's draws, %u different, %u left to the parse "
			"(%u frame skipping, %u that might run out of room)\n",
			st.drawn, st.draw_same, st.draw_differ, st.draw_left, st.draw_why[TA_GPU_WHY_PARSE], st.draw_why[TA_GPU_WHY_ROOM]);
	if (st.draw_us.n != 0)
		printf("ta_gpu_test: the header pass made ready to draw%s: mean %.1f us, p99 %.1f us, max %.1f us\n",
				ta_flat_strips() ? ", flat strips looked for" : "", mean(&st.draw_us), pct(&st.draw_us, 0.99), pct(&st.draw_us, 1.0));
	if (st.flat_us.n != 0)
		printf("ta_gpu_test: of it, the flat strips found from the depth words: mean %.1f us, p99 %.1f us, max %.1f us\n",
				mean(&st.flat_us), pct(&st.flat_us, 0.99), pct(&st.flat_us, 1.0));
	printf("ta_gpu_test: overrun at the exact room: %u frames, %u trials, the parse and the GPU agree on %u\n",
			st.edge_frames, st.edge_trials, st.edge_agree);
	printf("ta_gpu_test: sorted from the frame's depths: %u frames, %u the same as GenSortedTo(), %u different; per frame %.0f triangles, %.0f draws; "
			"the sort mean %.1f us, p99 %.1f us, max %.1f us; GenSortedTo() mean %.1f us, p99 %.1f us, max %.1f us\n",
			st.sort_frames, st.sort_same, st.sort_differ, st.sort_frames ? (double)st.sort_tris / st.sort_frames : 0.0,
			st.sort_frames ? (double)st.sort_runs / st.sort_frames : 0.0,
			mean(&st.sort_us), pct(&st.sort_us, 0.99), pct(&st.sort_us, 1.0), mean(&st.gensorted_us), pct(&st.gensorted_us, 0.99), pct(&st.gensorted_us, 1.0));
	printf("ta_gpu_test: the depth prepass: %llu checks, %llu triangles (%.0f pixels), %llu of them left out by the GPU (%.0f pixels), "
			"per sorted frame %.0f pixels of %.0f\n",
			(unsigned long long)st.prep_checks, (unsigned long long)st.prep_tris, st.prep_area, (unsigned long long)st.prep_out, st.prep_area_out,
			st.sort_frames ? st.prep_area_out / st.sort_frames : 0.0, st.sort_frames ? st.prep_area / st.sort_frames : 0.0);
	printf("ta_gpu_test: shadows the GPU decides: %u frames, %u the same as the parse's, %u different; %llu groups named, %llu drawn again\n",
			st.shadow_frames, st.shadow_same, st.shadow_differ, (unsigned long long)st.shadow_groups, (unsigned long long)st.shadow_drawn);
	printf("ta_gpu_test: volume triangles cut: %u frames with some, %u different from the parse's; %llu triangles drawn for them\n",
			st.mvclip_frames, st.mvclip_differ, (unsigned long long)st.mvclip_tris);
	printf("ta_gpu_test: validation errors: %d\n", tgt_validation_errors());
	printf("ta_gpu_test: %s\n", st.differ == 0 && st.model_differ == 0 && st.scan_differ == 0 && tgt_validation_errors() == 0
			&& st.overrun == st.overrun_agree && st.compared > 0 && st.draw_differ == 0 && st.edge_agree == st.edge_trials
			&& st.sort_differ == 0 && st.shadow_differ == 0 && st.mvclip_differ == 0 ? "PASS" : "FAIL");
	fflush(stdout);
}

static bool env_on(const char *name)
{
	const char *v = getenv(name);
	return v != NULL && atoi(v) != 0;
}

static void setup(void)
{
	TaGpuSizes sz;
	struct tgt_sizes ts;
	unsigned words[13];
	vec empty_d = VEC(double);

	st.prep_us = st.parse_us = st.draw_us = st.flat_us = st.sort_us = st.gensorted_us = empty_d;
	/* (TAG_GL=1: the device is OpenGL, and the frames are made as its renderers draw them: see gl_device) */
	gl_device = env_on("TAG_GL");
	ta_set_gpu_processing(true);
	ta_gpu_sort_reserve(true);
	/* (as the raster renderers have it: the cut volume triangles made by the parse and from the blocks, and compared) */
	ta_set_modvol_clip(true);
	/* (TAG_FLAT=1: flat strips looked for, as with SSAA; the draw groups' single-rate bits compared too) */
	if (env_on("TAG_FLAT"))
		ta_set_flat_strips(true);
	ta_gpu_sizes(&sz);
	ts.ta_words = ta_data_size() / 4;
	ts.table_words = sz.table_words;
	ts.verts = sz.verts;
	ts.idx = sz.idx;
	ts.list[0] = sz.list[0];
	ts.list[1] = sz.list[1];
	ts.list[2] = sz.list[2];
	ts.modtrig = sz.modtrig;
	ts.headers = sz.headers;
	tgt_init(getenv("TAG_SPV") ? getenv("TAG_SPV") : ".", env_on("TAG_VALIDATE"), &ts, words);
	memcpy(layout.words, words, sizeof(words));
	table_room = sz.table_words;
	table = (u32 *)malloc(table_room * 4);
	if (!tgt_softfloat_test())
		st.differ++;
	atexit(report);
	ready = true;
}

/* The frontend ends with _exit(), which runs no atexit() handler: the
 * totals and the verdict are printed on the way out here instead */
void __real__exit(int code);
void __wrap__exit(int code)
{
	if (ready)
		report();
	fflush(stdout);
	__real__exit(code);
}

/* What differs between the parse's frame and a decode's: NULL for nothing */
static const char *compare(TA_context *ctx, const TaGpuFrame *gf, const struct tgt_out *o, char *why, size_t room)
{
	static const char *const names[3] = { "opaque", "punch-through", "translucent" };
	static vec drawn = VEC(Vertex);
	static vec a = VEC(pair32), b = VEC(pair32);
	const rend_context *rc = &ctx->rend;
	const ta_list_t *lists[3];
	const u32 nv = (u32)TA_LIST_USED(rc->verts, Vertex);
	const u32 *idx = TA_LIST_HEAD(rc->idx, u32);
	const int nidx = TA_LIST_USED(rc->idx, u32);
	u32 l, v, last;
	int i;

	lists[0] = &rc->global_param_op.l;
	lists[1] = &rc->global_param_pt.l;
	lists[2] = &rc->global_param_tr.l;
	/* the vertices as the Vulkan renderers draw them: the parse's, with the
	 * provoking vertex's colours moved (vk_base_drawer's provoking
	 * vertices); as the OpenGL ones do (TAG_GL), the parse's */
	vec_assign(&drawn, rc->verts.l.base, nv);
	for (l = 0; l < (gl_device ? 0u : 3u); l++)
		for (i = 0; i < PP_N(lists[l]); i++)
		{
			const PolyParam *pp = &PP_AT(lists[l], i);
			u32 n;
			if (!TAW_GET(pp->pcw, PCW_Gouraud) && pp->count > 2)
				for (n = 0; n < pp->count - 2; n++)
				{
					Vertex *vertex = &VAT(drawn, Vertex, idx[pp->first + n]);
					const Vertex *lastVertex = &VAT(drawn, Vertex, idx[pp->first + n + 2]);
					memcpy(vertex->col, lastVertex->col, 4);
					memcpy(vertex->vtx_spc, lastVertex->vtx_spc, 4);
					memcpy(vertex->col1, lastVertex->col1, 4);
					memcpy(vertex->spc1, lastVertex->spc1, 4);
				}
		}
	for (v = 0; v < nv; v++)
		if (memcmp(&VAT(drawn, Vertex, v), &o->vtx[v * TAG_V_WORDS], sizeof(Vertex)) != 0)
		{
			const u32 *c = (const u32 *)&VAT(drawn, Vertex, v);
			u32 k;
			for (k = 0; k < TAG_V_WORDS; k++)
				if (c[k] != o->vtx[v * TAG_V_WORDS + k])
				{
					snprintf(why, room, "vertex %u of %u word %u: %08x, %08x", v, nv, k, c[k], o->vtx[v * TAG_V_WORDS + k]);
					return why;
				}
		}
	if ((u32)TA_LIST_USED(rc->modtrig, ModTriangle) != gf->modtrig
			|| memcmp(rc->modtrig.l.base, o->mod, gf->modtrig * sizeof(ModTriangle)) != 0)
		return "the modifier volume triangles";
	if ((u32)nidx != o->frm[TAG_O_NIDX])
	{
		snprintf(why, room, "indices: %d, %u", nidx, o->frm[TAG_O_NIDX]);
		return why;
	}
	for (i = 0; i < nidx; i++)
		if (idx[i] != o->idx[i])
		{
			snprintf(why, room, "index %d of %d: %u, %u", i, nidx, idx[i], o->idx[i]);
			return why;
		}
	last = TAG_O_PASS + (gf->passes - 1) * 3;
	for (l = 0; l < 3; l++)
	{
		const u32 n = (u32)PP_N(lists[l]);
		const u32 base = layout.words[10 + l];
		u32 k;
		if (n != o->frm[last + l])
		{
			snprintf(why, room, "%s polygon parameters: %u, %u", names[l], n, o->frm[last + l]);
			return why;
		}
		for (k = 0; k < n; k++)
		{
			const PolyParam *pp = &PP_AT(lists[l], k);
			const u32 *r = &o->rec[base + k * TAG_C_WORDS];
			if (pp->first != r[TAG_C_FIRST] || pp->count != r[TAG_C_COUNT])
			{
				snprintf(why, room, "%s polygon parameter %u: first %u/%u count %u/%u", names[l], k, pp->first, r[TAG_C_FIRST], pp->count, r[TAG_C_COUNT]);
				return why;
			}
		}
	}
	/* the render passes: the header pass's, but for those the parse finds
	 * nothing new in */
	{
		const RenderPass *passes = TA_LIST_HEAD(rc->render_passes, RenderPass);
		const u32 npasses = (u32)TA_LIST_USED(rc->render_passes, RenderPass);
		u32 kept = 0, prev[3] = { 0, 0, 0 }, p;
		for (p = 0; p < gf->passes; p++)
		{
			const u32 *c = &o->frm[TAG_O_PASS + p * 3];
			const RenderPass *pa, *pb;
			if (p != 0 && c[0] == prev[0] && c[1] == prev[1] && c[2] == prev[2])
				continue;
			if (kept >= npasses)
				return "more render passes";
			pa = &passes[kept];
			pb = &gf->pass[p];
			if (pa->op_count != c[0] || pa->pt_count != c[1] || pa->tr_count != c[2] || pa->autosort != pb->autosort
					|| pa->z_clear != pb->z_clear || pa->mv_op_tr_shared != pb->mv_op_tr_shared
					|| pa->mvo_count != pb->mvo_count || pa->mvo_tr_count != pb->mvo_tr_count)
			{
				snprintf(why, room, "render pass %u: op %u/%u pt %u/%u tr %u/%u", kept, pa->op_count, c[0], pa->pt_count, c[1], pa->tr_count, c[2]);
				return why;
			}
			prev[0] = c[0]; prev[1] = c[1]; prev[2] = c[2];
			kept++;
		}
		if (kept != npasses)
			return "fewer render passes";
	}
	{
		u32 z;
		float s;
		memcpy(&z, &rc->fZ_max, 4);
		if (z != o->frm[TAG_O_ZMAX])
		{
			snprintf(why, room, "fZ_max: %08x, %08x", z, o->frm[TAG_O_ZMAX]);
			return why;
		}
		/* its reciprocal, which the vertex shaders take from the frame
		 * (within two units in the last place: a device need not divide
		 * exactly) */
		s = pvr_depth_scale(rc->fZ_max);
		memcpy(&z, &s, 4);
		if (z > o->frm[TAG_O_ZSCALE] + 2 || z + 2 < o->frm[TAG_O_ZSCALE])
		{
			snprintf(why, room, "1/fZ_max: %08x, %08x", z, o->frm[TAG_O_ZSCALE]);
			return why;
		}
	}
	if (gf->x_clip.full != rc->fb_X_CLIP.full || gf->y_clip.full != rc->fb_Y_CLIP.full)
		return "the clipping rectangle";
	/* the draw commands: the opaque and punch-through groups that have
	 * indices, by where they start */
	{
		u32 s;
		a.n = b.n = 0;
		for (l = 0; l < 2; l++)
			for (i = 0; i < PP_N(lists[l]); i++)
				if (PP_AT(lists[l], i).count != 0)
					push_pair32(&a, PP_AT(lists[l], i).first, PP_AT(lists[l], i).count);
		for (s = 0; s < gf->slots; s++)
			if (o->cmd[s * TAG_C_WORDS + TAG_C_COUNT] != 0)
				push_pair32(&b, o->cmd[s * TAG_C_WORDS + TAG_C_FIRST], o->cmd[s * TAG_C_WORDS + TAG_C_COUNT]);
		qsort(a.p, a.n, sizeof(pair32), by_pair32);
		qsort(b.p, b.n, sizeof(pair32), by_pair32);
		if (!vec_same(&a, &b))
		{
			snprintf(why, room, "draw commands: %u groups, %u", (u32)a.n, (u32)b.n);
			return why;
		}
	}
	/* the translucent polygon parameters' words, as the per-pixel renderer
	 * makes them (its main buffer's upload); and the translucent draw
	 * records, which group by group are the parse's polygon parameters in
	 * their order, numbered as they are in the list, and nothing more */
	{
		const ta_list_t *tr = lists[2];
		const int ntr = PP_N(tr);
		u32 next = 0, at;
		for (i = 0; i < ntr; i++)
		{
			const PolyParam *pp = &PP_AT(tr, i);
			if (o->trp[i * TAG_P_WORDS] != ((pp->tsp & 0xffff00c0) | ((pp->isp >> 16) & 0xe400) | ((pp->pcw >> 7) & 1)
					| (pp->tsp1 != (u32)-1 || pp->tcw1 != (u32)-1 ? TAG_P_TWOVOL : 0u))
					|| o->trp[i * TAG_P_WORDS + 1] != pp->tsp1)
			{
				snprintf(why, room, "translucent polygon parameter words %d", i);
				return why;
			}
		}
		for (at = 0; at < gf->trd; )
		{
			const u32 n = o->trd[at * TAG_C_WORDS];
			u32 j;
			for (j = 0; j < n; j++, next++)
			{
				const u32 *r = &o->trd[(at + 1 + j) * TAG_C_WORDS];
				if ((int)next >= ntr || r[TAG_C_COUNT] != PP_AT(tr, next).count || r[TAG_C_FIRST] != PP_AT(tr, next).first
						|| r[TAG_C_INST] != 1 || r[3] != 0 || r[TAG_C_INSTANCE] != next)
				{
					snprintf(why, room, "translucent draw record %u (polygon parameter %u)", at + 1 + j, next);
					return why;
				}
			}
			/* the group's room past its count: zeroes, up to the next group,
			 * whose count is the first word that is not (a group has a
			 * strip at least) */
			at += 1 + n;
			while (at < gf->trd && o->trd[at * TAG_C_WORDS] == 0)
			{
				for (j = 0; j < TAG_C_WORDS; j++)
					if (o->trd[at * TAG_C_WORDS + j] != 0)
						return "the translucent draw records' room";
				at++;
			}
		}
		if ((int)next != ntr)
		{
			snprintf(why, room, "translucent draw records: %u, %d polygon parameters", next, ntr);
			return why;
		}
	}
	if (o->frm[TAG_O_OVERRUN] != 0)
		return "an overrun the parse did not have";
	return NULL;
}

/* The decode against the model's, word for word where both wrote */
static const char *compare_model(const TaGpuFrame *gf, const struct tgt_out *g, const struct tgt_out *m, const u32 *table)
{
	const u32 nidx = g->frm[TAG_O_NIDX] < table[TAG_T_ROOM_IDX] ? g->frm[TAG_O_NIDX] : table[TAG_T_ROOM_IDX];
	const u32 ntr = g->frm[TAG_O_PASS + (gf->passes - 1) * 3 + 2];
	/* (the model has no indirect dispatches to give arguments for) */
	if (memcmp(g->frm, m->frm, TAG_O_ARGS64 * 4) != 0 || memcmp(g->frm + TAG_O_PASS, m->frm + TAG_O_PASS, (TAG_O_WORDS - TAG_O_PASS) * 4) != 0)
		return "the frame words";
	if (memcmp(g->vtx, m->vtx, gf->verts * TAG_V_WORDS * 4) != 0)
		return "vertices";
	if (memcmp(g->mod, m->mod, gf->modtrig * 9 * 4) != 0)
		return "modifier volume triangles";
	if (memcmp(g->idx, m->idx, nidx * 4) != 0)
		return "indices";
	if (memcmp(g->cmd, m->cmd, gf->slots * TAG_C_WORDS * 4) != 0)
		return "commands";
	if (memcmp(g->work + table[TAG_T_W_SSTATE], m->work + table[TAG_T_W_SSTATE], g->frm[TAG_O_NSTRIPS] * TAG_ST_WORDS * 4) != 0)
		return "strip states";
	if (memcmp(g->trd, m->trd, gf->trd * TAG_C_WORDS * 4) != 0)
		return "translucent draw records";
	if (memcmp(g->trp, m->trp, (ntr < table[TAG_T_ROOM_TR] ? ntr : table[TAG_T_ROOM_TR]) * TAG_P_WORDS * 4) != 0)
		return "translucent polygon parameter words";
	return NULL;
}

extern int ta_parse_cnt;

/* The frame as it was before the parse (its background's), to parse it again */
static struct
{
	PolyParam bg;
	Vertex bgv[4];
	FB_X_CLIP_type x_clip;
	FB_Y_CLIP_type y_clip;
	bool rtt, fb, clear;
} pre;

static void snap_pre(TA_context *ctx)
{
	const rend_context *rc = &ctx->rend;
	pre.bg = *TA_LIST_HEAD(rc->global_param_op, PolyParam);
	memcpy(pre.bgv, rc->verts.l.base, sizeof(pre.bgv));
	pre.x_clip = rc->fb_X_CLIP;
	pre.y_clip = rc->fb_Y_CLIP;
	pre.rtt = rc->isRTT;
	pre.fb = rc->isRenderFramebuffer;
	pre.clear = rc->clearFramebuffer;
}

/* the parse again from the frame as it was, with list @l (0-2, 3 the indices) given room @room (in its elements) */
static bool parse_again(TA_context *ctx, int l, int room)
{
	rend_context *rc = &ctx->rend;
	ta_list_t *lists[4];
	ta_list_t *list = NULL;
	unsigned char *saved = NULL;
	int cnt;
	bool overrun;

	lists[0] = &rc->global_param_op.l;
	lists[1] = &rc->global_param_pt.l;
	lists[2] = &rc->global_param_tr.l;
	lists[3] = &rc->idx.l;
	rend_context_clear(rc);
	if (l >= 0)
	{
		list = lists[l];
		saved = list->end;
		list->end = list->base + (size_t)room * (l < 3 ? sizeof(PolyParam) : sizeof(u32));
	}
	*(PolyParam *)ta_list_append(&rc->global_param_op.l, sizeof(PolyParam)) = pre.bg;
	memcpy(ta_list_append(&rc->verts.l, sizeof(pre.bgv)), pre.bgv, sizeof(pre.bgv));
	rc->fb_X_CLIP = pre.x_clip;
	rc->fb_Y_CLIP = pre.y_clip;
	rc->isRTT = pre.rtt;
	rc->isRenderFramebuffer = pre.fb;
	rc->clearFramebuffer = pre.clear;
	cnt = ta_parse_cnt;
	__real_ta_parse_vdrc(ctx);
	ta_parse_cnt = cnt;
	overrun = rc->Overrun;
	if (list != NULL)
	{
		ta_list_clear(list);
		list->end = saved;
	}
	return overrun;
}

/* The overrun check at the exact boundary: each list given the room the
 * frame takes - its strips and the one being filled, the indices - and one
 * less, and the parse's verdict against the GPU's with the same room in
 * its tables. The frame is parsed again as it was after. */
static void edge_check(TA_context *ctx, const TaGpuFrame *gf, const struct tgt_out *o)
{
	static vec t = VEC(u32);
	static const u32 room_word[4] = { TAG_T_ROOM_OP, TAG_T_ROOM_PT, TAG_T_ROOM_TR, TAG_T_ROOM_IDX };
	static const u32 bit[4] = { 2, 4, 8, 1 };
	const u32 last = TAG_O_PASS + (gf->passes - 1) * 3;
	u32 need[4];
	const u32 bytes = (u32)(tad_end(&ctx->tad) - ctx->tad.thd_root);
	int l;

	need[0] = o->frm[last] + 1;
	need[1] = o->frm[last + 1] + 1;
	need[2] = o->frm[last + 2] + 1;
	need[3] = o->frm[TAG_O_NIDX];
	vec_assign(&t, table, gf->table_words);
	st.edge_frames++;
	for (l = 0; l < 4; l++)
	{
		u32 room;
		/* (a list with no polygon of its own - the opaque one has the
		 * background - never has one being filled: the parse's room is
		 * then its strips, the GPU's one more, and a list has room for
		 * thousands) */
		if (need[l] <= (l == 0 ? 2u : 1u))
			continue;
		for (room = need[l] - 1; room <= need[l]; room++)
		{
			const bool cpu = parse_again(ctx, l, (int)room);
			struct tgt_out g;
			double ms;
			bool gpu;
			VAT(t, u32, room_word[l]) = room;
			tgt_gpu(ctx->tad.thd_root, bytes, (const unsigned *)t.p, gf->table_words, gf->trd, &g, &ms);
			gpu = (g.frm[TAG_O_OVERRUN] & bit[l]) != 0;
			st.edge_trials++;
			if (cpu == gpu && cpu == (room < need[l]))
				st.edge_agree++;
			else if (st.edge_trials - st.edge_agree < 10)
				printf("ta_gpu_test: frame %u: list %d with room for %u of %u: the parse %s, the GPU %s\n", st.seen, l, room, need[l],
						cpu ? "runs out" : "does not", gpu ? "runs out" : "does not");
		}
		VAT(t, u32, room_word[l]) = table[room_word[l]];
	}
	parse_again(ctx, -1, 0);
}

/* Whether two polygon parameters are drawn the same way, texture and all */
static bool same_draw_state(const PolyParam *a, const PolyParam *b)
{
	return a->tsp == b->tsp && a->tcw == b->tcw && ((a->pcw ^ b->pcw) & ~0x100u) == 0
			&& a->isp == b->isp && a->tsp1 == b->tsp1 && a->tcw1 == b->tcw1
			&& a->tileclip == b->tileclip && a->texid == b->texid && a->texid1 == b->texid1;
}

/* ...and at the same rate */
static bool same_draw_rate(const PolyParam *a, const PolyParam *b)
{
	return same_draw_state(a, b) && ((a->pcw ^ b->pcw) & 0x100u) == 0;
}

/* GenSortedTo() of the parse's frame, each sorted render pass, the draws'
 * first counting from the first pass's first index, as the Vulkan
 * per-triangle renderer has them (its sorted triangles); the polygon
 * parameters' words kept, which the header pass's lists then replace */
typedef struct SortRef
{
	vec runs;	/* SortTrigDrawParam */
	vec pp;		/* PolyParam */
	vec idx;	/* u32 */
	/* each sorted pass's depth prepass (GenPrepassTo()), as the header pass
	 * lays it out: every pass's sorted indices first, then every pass's
	 * prepass indices, the prepass draws' first counting from the start */
	vec pruns;	/* SortTrigDrawParam */
	vec ppp;	/* PolyParam */
	vec pidx;	/* u32 */
	vec prun_pass;	/* u32 */
} SortRef;

static void gensorted_ref(TA_context *ctx, SortRef *ref, double *us)
{
	static SortList polys;
	static vec idx = VEC(u32);
	TA_context *const saved = _pvrrc;
	const rend_context *rc = &ctx->rend;
	const RenderPass *passes = TA_LIST_HEAD(rc->render_passes, RenderPass);
	const int npasses = TA_LIST_USED(rc->render_passes, RenderPass);
	RenderPass prev;
	int p;
	size_t q;

	memset(&prev, 0, sizeof(prev));
	_pvrrc = ctx;
	ref->runs.n = ref->pp.n = ref->idx.n = ref->pruns.n = ref->ppp.n = ref->pidx.n = ref->prun_pass.n = 0;
	*us = 0;
	/* (GenPrepassTo() leaves out a polygon none of whose vertices has an
	 * alpha of 255 where its alpha is theirs: PrepassVerticesOpaque(). The
	 * header pass has not got the vertices; the GPU leaves such a strip
	 * out, TAG_K_PREPASS) */
	for (p = 0; p < npasses; p++)
	{
		const RenderPass *cur = &passes[p];
		if (cur->autosort)
		{
			const int first = prev.tr_count, count = cur->tr_count - prev.tr_count;
			double t0;
			u32 sorted, n, i;
			idx.n = 0;
			vec_add(&idx, 6 * SortedTriangles(first, count) + 1);
			t0 = now_us();
			sorted = GenSortedTo(first, count, &polys, (u32 *)idx.p, !gl_device);
			*us += now_us() - t0;
			for (i = 0; i < polys.count; i++)
			{
				const SortTrigDrawParam *r = &SortDraws(&polys)[i];
				SortTrigDrawParam qd = *r;
				qd.first += (u32)ref->idx.n;
				vec_push(&ref->runs, &qd);
				vec_push(&ref->pp, r->ppid);
			}
			memcpy(vec_add(&ref->idx, sorted), idx.p, sorted * sizeof(u32));
			/* (the prepass's, kept apart: GenPrepassTo() writes after the
			 * sorted indices, its draws' first counting from idx's start) */
			polys.count = 0;
			n = GenPrepassTo(first, count, p == npasses - 1, &polys,
					(u32 *)idx.p + sorted, sorted, !gl_device);
			for (i = 0; i < polys.count; i++)
			{
				const SortTrigDrawParam *r = &SortDraws(&polys)[i];
				SortTrigDrawParam qd = *r;
				u32 pass = (u32)p;
				qd.first = qd.first - sorted + (u32)ref->pidx.n;
				vec_push(&ref->pruns, &qd);
				vec_push(&ref->ppp, r->ppid);
				vec_push(&ref->prun_pass, &pass);
			}
			if (n != 0)
				memcpy(vec_add(&ref->pidx, n), (u32 *)idx.p + sorted, n * sizeof(u32));
		}
		prev = *cur;
	}
	/* (the prepass's indices go after all the sorted ones) */
	for (q = 0; q < ref->pruns.n; q++)
		VAT(ref->pruns, SortTrigDrawParam, q).first += (u32)ref->idx.n;
	if (ref->pidx.n != 0)
		memcpy(vec_add(&ref->idx, ref->pidx.n), ref->pidx.p, ref->pidx.n * sizeof(u32));
	_pvrrc = saved;
}

/* A prepass draw's triangles, each as its three indices (provoking vertex
 * and winding as they are), in a canonical order: a depth-only draw that
 * passes on what is nearer writes the same depth whatever the order of its
 * triangles, and so does a prepass whatever the order of its draws */
static void prepass_tris(const u32 *idx, size_t nidx, u32 first, u32 count, vec *tris)
{
	u32 i;
	tris->n = 0;
	for (i = first; i + 3 <= first + count && i + 3 <= nidx; i += 3)
		/* (one the GPU left out, TAG_K_PREPASS, is a point: it draws nothing) */
		if (idx[i] != idx[i + 1] || idx[i] != idx[i + 2])
		{
			pair64 *t = (pair64 *)vec_add(tris, 1);
			t->a = ((u64)idx[i] << 32) | idx[i + 1];
			t->b = idx[i + 2];
			t->pad = 0;
		}
	qsort(tris->p, tris->n, sizeof(pair64), by_pair64);
}

/* a vertex's word @w on the 640 x 480 screen (what is off it does not count) */
static double on_screen(const unsigned *vtx, u32 v, u32 w)
{
	float x;
	const double most = w == TAG_V_X ? 640.0 : 480.0;
	double d;
	memcpy(&x, &vtx[v * TAG_V_WORDS + w], 4);
	if (!isfinite(x))
		return 0.0;
	d = x < 0 ? 0.0 : (double)x;
	return d > most ? most : d;
}

/* a triangle's area in pixels of the 640 x 480 screen, from the device's vertices */
static double tri_area(const unsigned *vtx, u32 a, u32 b, u32 c)
{
	const double ax = on_screen(vtx, a, TAG_V_X), ay = on_screen(vtx, a, TAG_V_Y);
	const double s = (on_screen(vtx, b, TAG_V_X) - ax) * (on_screen(vtx, c, TAG_V_Y) - ay)
			- (on_screen(vtx, c, TAG_V_X) - ax) * (on_screen(vtx, b, TAG_V_Y) - ay);
	return isfinite(s) ? fabs(s) / 2 : 0.0;
}

/* The header pass's sort (TA_GPU_SORT) against GenSortedTo()'s: the same
 * indices, and the same draws of them, drawn the same way; then each
 * pass's depth prepass against GenPrepassTo()'s: the same draws, drawn the
 * same way with the same triangles, in any order (GenPrepassTo() orders its
 * groups by texture first, which the header pass has not got) */
static const char *compare_sorted(TA_context *ctx, const SortRef *ref, const TaGpuFrame *gf, char *why, size_t room)
{
	static vec idx = VEC(u32), cpu = VEC(u32), model = VEC(u32);
	static vec span = VEC(pair32), a = VEC(pair64), b = VEC(pair64);
	static vec used = VEC(u8);
	const u32 checks = ta_gpu_prepass_checks();
	size_t gpu_pre = 0, sorted, i, k = 0, gpu_draws = 0;
	u32 p, at;

	idx.n = 0;
	if (ta_gpu_sorted_words() != 0)
	{
		memset(vec_add(&idx, ta_gpu_sorted_words()), 0, ta_gpu_sorted_words() * sizeof(u32));
		ta_gpu_sorted_write((u32 *)idx.p);
	}
	/* the prepass's checks (TAG_K_PREPASS), on the device and by the model,
	 * the same; the device's indices from here on */
	if (checks != 0)
	{
		struct tgt_out g, m;
		double ms;
		if (table[TAG_T_PREP] != ta_gpu_sorted_indices())
		{
			snprintf(why, room, "the prepass's checks at %u, the sorted indices end at %u", table[TAG_T_PREP], ta_gpu_sorted_indices());
			return why;
		}
		vec_assign(&cpu, idx.p, idx.n);
		vec_assign(&model, idx.p, idx.n);
		tgt_sorted((unsigned *)model.p, (unsigned)model.n, checks);
		tgt_model(ctx->tad.thd_root, table, gf->trd, &m);
		tgt_sorted((unsigned *)idx.p, (unsigned)idx.n, checks);
		tgt_gpu(ctx->tad.thd_root, (u32)(tad_end(&ctx->tad) - ctx->tad.thd_root), table, gf->table_words, gf->trd, &g, &ms);
		if (!vec_same(&idx, &model))
		{
			snprintf(why, room, "the prepass's checks: the GPU and the model differ");
			return why;
		}
		st.prep_checks += checks;
		/* (the prepass's triangles: its draws') */
		for (p = 0; p < gf->passes; p++)
		{
			u32 n, j;
			const SortTrigDrawParam *r = ta_gpu_prepass(p, &n);
			for (j = 0; j < n; j++)
			{
				u32 t;
				for (t = r[j].first; t + 3 <= r[j].first + r[j].count && t + 3 <= ta_gpu_sorted_indices(); t += 3)
				{
					const u32 *c = (const u32 *)cpu.p, *x = (const u32 *)idx.p;
					const double area = tri_area(g.vtx, c[t], c[t + 1], c[t + 2]);
					st.prep_tris++;
					st.prep_area += area;
					if (x[t] == x[t + 1] && x[t] == x[t + 2])
					{
						st.prep_out++;
						st.prep_area_out += area;
					}
				}
			}
		}
	}
	idx.n = ta_gpu_sorted_indices();
	/* (the sorted ones; the prepass's, which the GPU makes fewer of, are compared by their draws below) */
	for (p = 0; p < gf->passes; p++)
	{
		u32 n, j;
		const SortTrigDrawParam *r = ta_gpu_prepass(p, &n);
		for (j = 0; j < n; j++)
			gpu_pre += r[j].count;
	}
	sorted = ref->idx.n - ref->pidx.n;
	if (gpu_pre > idx.n || idx.n - gpu_pre != sorted)
	{
		snprintf(why, room, "sorted indices: %zu, GenSortedTo() %zu", idx.n - (gpu_pre < idx.n ? gpu_pre : idx.n), sorted);
		return why;
	}
	for (i = 0; i < sorted; i++)
		if (VAT(idx, u32, i) != VAT(ref->idx, u32, i))
		{
			snprintf(why, room, "sorted index %zu of %zu: %u, GenSortedTo() %u", i, sorted, VAT(idx, u32, i), VAT(ref->idx, u32, i));
			return why;
		}
	for (p = 0; p < gf->passes; p++)
	{
		u32 n, j;
		const SortTrigDrawParam *r = ta_gpu_sorted(p, &n);
		for (j = 0; j < n; j++, k++)
			if (k >= ref->runs.n || r[j].first != VAT(ref->runs, SortTrigDrawParam, k).first
					|| r[j].count != VAT(ref->runs, SortTrigDrawParam, k).count
					|| !same_draw_state(r[j].ppid, &VAT(ref->pp, PolyParam, k)))
			{
				snprintf(why, room, "sorted draw %zu (pass %u): first %u count %u, GenSortedTo() %s", k, p, r[j].first, r[j].count,
						k >= ref->runs.n ? "has fewer" : "differs");
				return why;
			}
	}
	if (k != ref->runs.n)
	{
		snprintf(why, room, "sorted draws: %zu, GenSortedTo() %zu", k, ref->runs.n);
		return why;
	}
	/* the depth prepass: its draws cover the indices after the sorted ones,
	 * each once, and each pass's draws are GenPrepassTo()'s */
	span.n = 0;
	used.n = 0;
	if (ref->pruns.n != 0)
		memset(vec_add(&used, ref->pruns.n), 0, ref->pruns.n);
	for (p = 0; p < gf->passes; p++)
	{
		u32 n, j;
		const SortTrigDrawParam *r = ta_gpu_prepass(p, &n);
		gpu_draws += n;
		for (j = 0; j < n; j++)
		{
			bool found = false;
			size_t q;
			if (r[j].first < sorted || r[j].count == 0 || r[j].count % 3 != 0 || r[j].first + r[j].count > idx.n)
			{
				snprintf(why, room, "prepass draw %u of pass %u: first %u count %u, outside its indices", j, p, r[j].first, r[j].count);
				return why;
			}
			push_pair32(&span, r[j].first, r[j].count);
			prepass_tris((const u32 *)idx.p, idx.n, r[j].first, r[j].count, &a);
			/* (a draw all of whose triangles the GPU left out is one GenPrepassTo() has not got) */
			if (a.n == 0)
			{
				gpu_draws--;
				continue;
			}
			for (q = 0; q < ref->pruns.n && !found; q++)
			{
				const SortTrigDrawParam *pr = &VAT(ref->pruns, SortTrigDrawParam, q);
				if (VAT(used, u8, q) || VAT(ref->prun_pass, u32, q) != p || pr->count != a.n * 3
						|| !same_draw_state(r[j].ppid, &VAT(ref->ppp, PolyParam, q)))
					continue;
				prepass_tris((const u32 *)ref->idx.p, ref->idx.n, pr->first, pr->count, &b);
				if (vec_same(&a, &b))
				{
					VAT(used, u8, q) = 1;
					found = true;
				}
			}
			if (!found)
			{
				snprintf(why, room, "prepass draw %u of pass %u (first %u count %u): not one of GenPrepassTo()'s", j, p, r[j].first, r[j].count);
				return why;
			}
		}
	}
	if (gpu_draws != ref->pruns.n)
	{
		snprintf(why, room, "prepass draws: %zu, GenPrepassTo() %zu", gpu_draws, ref->pruns.n);
		return why;
	}
	qsort(span.p, span.n, sizeof(pair32), by_pair32);
	at = (u32)sorted;
	for (i = 0; i < span.n; i++)
	{
		const pair32 *s = &VAT(span, pair32, i);
		if (s->a != at)
		{
			snprintf(why, room, "prepass draws: index %u drawn %s", at, s->a < at ? "twice" : "by none");
			return why;
		}
		at += s->b;
	}
	if (at != idx.n)
	{
		snprintf(why, room, "prepass draws: indices %u to %zu drawn by none", at, idx.n);
		return why;
	}
	return NULL;
}

/* The draw groups drawn again for shadows (TA_GPU_SHADOWS): each render
 * pass's volumes' bounds against shadow_modvol_bounds() of the parse's
 * triangles, and the groups whose second commands the GPU (and the model)
 * gives indices against those the CPU draws again (CanDrawShadowed() and
 * poly_overlaps() of the parse's groups), with the header pass's frame in
 * the context and the parse's vertices, indices and triangles */
static const char *compare_shadows(TA_context *ctx, const TaGpuFrame *gf, const vec *parse, char *why, size_t room)
{
	static vec gpu = VEC(pair32), cpu = VEC(pair32);
	rend_context *rc = &ctx->rend;
	TA_context *const saved = _pvrrc;
	const char *d = NULL;
	struct tgt_out g, m;
	double ms;
	const u32 bytes = (u32)(tad_end(&ctx->tad) - ctx->tad.thd_root);
	const ta_list_t *lists[2];
	const RenderPass *passes = TA_LIST_HEAD(rc->render_passes, RenderPass);
	const int npasses = TA_LIST_USED(rc->render_passes, RenderPass);
	u32 base, n, sc, e;
	RenderPass prev;
	int p;

	tgt_model(ctx->tad.thd_root, table, gf->trd, &m);
	tgt_gpu(ctx->tad.thd_root, bytes, table, gf->table_words, gf->trd, &g, &ms);
	base = table[TAG_T_OFF_SHD];
	n = table[TAG_T_NSHD];
	sc = table[TAG_T_SHD_CMD];
	gpu.n = cpu.n = 0;
	for (e = 0; e < n && d == NULL; e++)
	{
		const u32 at = (sc + TAG_SH_SLOT(table[base + TAG_SH_ENTRY + e])) * TAG_C_WORDS;
		if (memcmp(&g.cmd[at], &m.cmd[at], TAG_C_WORDS * 4) != 0)
		{
			snprintf(why, room, "shadow entry %u: the GPU and the model differ", e);
			d = why;
		}
		else if (g.cmd[at + TAG_C_COUNT] > 2)
			push_pair32(&gpu, g.cmd[at + TAG_C_FIRST], g.cmd[at + TAG_C_COUNT]);
	}
	st.shadow_groups += n;
	st.shadow_drawn += gpu.n;
	_pvrrc = ctx;
	memset(&prev, 0, sizeof(prev));
	lists[0] = &rc->global_param_op.l;
	lists[1] = &rc->global_param_pt.l;
	for (p = 0; p < npasses && d == NULL; p++)
	{
		const RenderPass *cur = &passes[p];
		const ScreenBounds area = shadow_modvol_bounds(prev.mvo_count, cur->mvo_count - prev.mvo_count);
		u32 from[2], to[2], l;
		if (memcmp(&area, rc->gpu_shadow_area[p], sizeof(area)) != 0 || memcmp(&area, &table[base + TAG_SH_AREA + p * 4], sizeof(area)) != 0)
		{
			snprintf(why, room, "render pass %d: the volumes' bounds", p);
			d = why;
			break;
		}
		from[0] = prev.op_count; from[1] = prev.pt_count;
		to[0] = cur->op_count; to[1] = cur->pt_count;
		for (l = 0; l < 2 && !screen_bounds_empty(&area) && d == NULL; l++)
		{
			u32 i;
			for (i = from[l]; i < to[l]; i++)
			{
				/* the parse's group of the header pass's (by its command), and what the CPU does with it */
				const PolyParam *gp = &PP_AT(lists[l], i);
				const u32 *c = &g.cmd[gp->first * TAG_C_WORDS];
				const PolyParam *pp = NULL;
				size_t q;
				if (c[TAG_C_COUNT] == 0)
					continue;
				for (q = 0; q < parse[l].n; q++)
				{
					const PolyParam *x = &VAT(parse[l], PolyParam, q);
					if (x->count == c[TAG_C_COUNT] && x->first == c[TAG_C_FIRST])
					{
						pp = x;
						break;
					}
				}
				if (pp == NULL)
				{
					snprintf(why, room, "render pass %d: no group of the parse's at %u", p, c[TAG_C_FIRST]);
					d = why;
					break;
				}
				/* (a group of two vertices, which the header pass's count is,
				 * makes only triangles of no area, whatever its indices: the
				 * renderer leaves it) */
				if (CanDrawShadowed(l == 0 ? ListType_Opaque : ListType_Punch_Through, pp) && gp->count > 2 && poly_overlaps(pp, &area))
					push_pair32(&cpu, pp->first, pp->count);
			}
		}
		prev = *cur;
	}
	_pvrrc = saved;
	qsort(gpu.p, gpu.n, sizeof(pair32), by_pair32);
	qsort(cpu.p, cpu.n, sizeof(pair32), by_pair32);
	if (d == NULL && !vec_same(&gpu, &cpu))
	{
		snprintf(why, room, "the groups drawn again for shadows: %zu, the parse's %zu", gpu.n, cpu.n);
		d = why;
	}
	return d;
}

/* @list made @v's elements again */
static void list_put(ta_list_t *list, const vec *v)
{
	ta_list_clear(list);
	if (v->n != 0)
		memcpy(ta_list_append(list, v->n * v->elem), v->p, v->n * v->elem);
}

/* The frame made ready to draw from the GPU, as the Vulkan renderers have
 * it (TA_GPU_DRAW), against the parse's: each opaque and punch-through
 * draw group's command, in the lists' order, and each translucent one's
 * strips, are the parse's polygon parameters that have indices, in order,
 * drawn the same way; the render passes count the groups, the modifier
 * volumes and the clipping rectangle are the same. The parse's lists are
 * put back after. NULL for nothing different, "" for a frame left to the
 * parse. */
static const char *compare_draw(TA_context *ctx, const struct tgt_out *o, u32 flags, char *why, size_t room)
{
	static const char *const names[3] = { "opaque", "punch-through", "translucent" };
	static vec parse[3] = { VEC(PolyParam), VEC(PolyParam), VEC(PolyParam) };
	static vec passes = VEC(RenderPass);
	static vec mvo[2] = { VEC(ModifierVolumeParam), VEC(ModifierVolumeParam) };
	static vec mvclip = VEC(ModTriangle);
	static vec mvclip_range = VEC(u32);
	static SortRef ref = { VEC(SortTrigDrawParam), VEC(PolyParam), VEC(u32), VEC(SortTrigDrawParam), VEC(PolyParam), VEC(u32), VEC(u32) };
	rend_context *rc = &ctx->rend;
	ta_list_t *lists[3];
	const rend_context saved = *rc;
	double gensorted_us = 0, t0;
	TaGpuFrame gf;
	const int cnt = ta_parse_cnt;
	const char *d = NULL;
	bool ok;
	u32 l;

	lists[0] = &rc->global_param_op.l;
	lists[1] = &rc->global_param_pt.l;
	lists[2] = &rc->global_param_tr.l;
	vec_assign(&mvclip, rc->mv_clip.l.base, TA_LIST_USED(rc->mv_clip, ModTriangle));
	vec_assign(&mvclip_range, rc->mv_clip_range.l.base, TA_LIST_USED(rc->mv_clip_range, u32));
	for (l = 0; l < 3; l++)
		vec_assign(&parse[l], lists[l]->base, PP_N(lists[l]));
	vec_assign(&passes, rc->render_passes.l.base, TA_LIST_USED(rc->render_passes, RenderPass));
	vec_assign(&mvo[0], rc->global_param_mvo.l.base, TA_LIST_USED(rc->global_param_mvo, ModifierVolumeParam));
	vec_assign(&mvo[1], rc->global_param_mvo_tr.l.base, TA_LIST_USED(rc->global_param_mvo_tr, ModifierVolumeParam));
	/* (the background as the frame had it before the parse, which the header pass takes) */
	*TA_LIST_HEAD(rc->global_param_op, PolyParam) = saved_bg;
	rc->fb_X_CLIP = saved_x_clip;
	rc->fb_Y_CLIP = saved_y_clip;

	if (flags & TA_GPU_SORT)
		gensorted_ref(ctx, &ref, &gensorted_us);

	t0 = now_us();
	ok = ta_gpu_prepare(ctx, &layout, table, table_room, &gf, flags | TA_GPU_DRAW);
	if (ok)
	{
		push_double(&st.draw_us, now_us() - t0);
		if (ta_flat_strips())
			push_double(&st.flat_us, ta_gpu_flat_ns() / 1e3);
	}
	if (!ok)
	{
		d = "";
		st.draw_why[gf.why < TA_GPU_WHY_COUNT ? gf.why : 0]++;
	}
	else
	{
		/* with flat strips looked for, the single-rate bit is the same too:
		 * a group's, and each translucent strip's */
		bool (*same_state)(const PolyParam *, const PolyParam *) = ta_flat_strips() ? same_draw_rate : same_draw_state;
		const RenderPass *now_passes;
		size_t p;
		ta_parse_cnt = cnt;
		for (l = 0; l < 3 && d == NULL; l++)
		{
			size_t next = 0;
			const PolyParam *pl = (const PolyParam *)parse[l].p;
			int i;
			for (i = 0; i < PP_N(lists[l]) && d == NULL; i++)
			{
				const PolyParam *g = &PP_AT(lists[l], i);
				if (l < 2)
				{
					const u32 *c = &o->cmd[g->first * TAG_C_WORDS];
					/* (the parse's group, its head and the ones joined to it, which have no indices) */
					while (next < parse[l].n && pl[next].count == 0)
						next++;
					if (c[TAG_C_COUNT] == 0)
						continue;
					if (next >= parse[l].n || pl[next].first != c[TAG_C_FIRST] || pl[next].count != c[TAG_C_COUNT]
							|| !same_state(&pl[next], g))
					{
						snprintf(why, room, "%s draw group %d", names[l], i);
						d = why;
					}
					next++;
				}
				else
				{
					const u32 n = o->trd[g->first * TAG_C_WORDS];
					u32 j;
					if (n > g->count)
					{
						snprintf(why, room, "translucent draw group %d: %u strips, room for %u", i, n, g->count);
						d = why;
					}
					for (j = 0; j < n && d == NULL; j++, next++)
						if (next >= parse[l].n || !same_state(&pl[next], g)
								|| o->trd[(g->first + 1 + j) * TAG_C_WORDS + TAG_C_FIRST] != pl[next].first)
						{
							snprintf(why, room, "translucent draw group %d strip %u", i, j);
							d = why;
						}
				}
			}
			if (d == NULL && l < 2)
				while (next < parse[l].n && pl[next].count == 0)
					next++;
			if (d == NULL && next != parse[l].n)
			{
				snprintf(why, room, "%s draw groups: %zu of %zu polygon parameters", names[l], next, parse[l].n);
				d = why;
			}
		}
		now_passes = TA_LIST_HEAD(rc->render_passes, RenderPass);
		if (d == NULL && TA_LIST_USED(rc->render_passes, RenderPass) != (int)passes.n)
			d = "the render passes";
		for (p = 0; d == NULL && p < passes.n; p++)
		{
			const RenderPass *pa = &VAT(passes, RenderPass, p), *pb = &now_passes[p];
			const u32 pa_op = p ? VAT(passes, RenderPass, p - 1).op_count : 0, pb_op = p ? now_passes[p - 1].op_count : 0;
			const u32 pa_tr = p ? VAT(passes, RenderPass, p - 1).tr_count : 0, pb_tr = p ? now_passes[p - 1].tr_count : 0;
			if (pa->autosort != pb->autosort || pa->z_clear != pb->z_clear || pa->mv_op_tr_shared != pb->mv_op_tr_shared
					|| pa->mvo_count != pb->mvo_count || pa->mvo_tr_count != pb->mvo_tr_count
					|| (pa->op_count == pa_op) != (pb->op_count == pb_op)
					|| (pa->tr_count == pa_tr) != (pb->tr_count == pb_tr))
			{
				snprintf(why, room, "render pass %zu", p);
				d = why;
			}
		}
		if (d == NULL && (TA_LIST_USED(rc->global_param_mvo, ModifierVolumeParam) != (int)mvo[0].n
				|| TA_LIST_USED(rc->global_param_mvo_tr, ModifierVolumeParam) != (int)mvo[1].n
				|| (mvo[0].n && memcmp(rc->global_param_mvo.l.base, mvo[0].p, mvo[0].n * sizeof(ModifierVolumeParam)) != 0)
				|| (mvo[1].n && memcmp(rc->global_param_mvo_tr.l.base, mvo[1].p, mvo[1].n * sizeof(ModifierVolumeParam)) != 0)))
			d = "the modifier volumes";
		if (d == NULL && (rc->fb_X_CLIP.full != saved.fb_X_CLIP.full || rc->fb_Y_CLIP.full != saved.fb_Y_CLIP.full))
			d = "the clipping rectangle";
		if (d == NULL && (!rc->gpu || rc->gpu_modtrig != (u32)TA_LIST_USED(saved.modtrig, ModTriangle)))
			d = "the modifier volume triangles";
		if (d == NULL && mvclip.n != 0)
		{
			st.mvclip_frames++;
			st.mvclip_tris += mvclip.n;
		}
		if (d == NULL && (TA_LIST_USED(rc->mv_clip, ModTriangle) != (int)mvclip.n || TA_LIST_USED(rc->mv_clip_range, u32) != (int)mvclip_range.n
				|| (mvclip.n && memcmp(rc->mv_clip.l.base, mvclip.p, mvclip.n * sizeof(ModTriangle)) != 0)
				|| (mvclip_range.n && memcmp(rc->mv_clip_range.l.base, mvclip_range.p, mvclip_range.n * sizeof(u32)) != 0)))
		{
			st.mvclip_differ++;
			snprintf(why, room, "the cut volume triangles: %d and %d ranges from the blocks, %zu and %zu from the parse",
					TA_LIST_USED(rc->mv_clip, ModTriangle), TA_LIST_USED(rc->mv_clip_range, u32), mvclip.n, mvclip_range.n);
			d = why;
		}
		if (d == NULL && (flags & TA_GPU_SORT) && ref.idx.n != 0)
		{
			st.sort_frames++;
			st.sort_tris += ref.idx.n / 3;
			st.sort_runs += ref.runs.n;
			push_double(&st.sort_us, ta_gpu_sorted_ns() / 1e3);
			push_double(&st.gensorted_us, gensorted_us);
			d = compare_sorted(ctx, &ref, &gf, why, room);
			if (d == NULL)
				st.sort_same++;
			else
				st.sort_differ++;
		}
		else if (d == NULL && (flags & TA_GPU_SORT) && ta_gpu_sorted_indices() != 0)
		{
			st.sort_differ++;
			d = "sorted triangles where GenSortedTo() has none";
		}
		if (d == NULL && gf.shadows != 0)
		{
			st.shadow_frames++;
			d = compare_shadows(ctx, &gf, parse, why, room);
			if (d == NULL)
				st.shadow_same++;
			else
				st.shadow_differ++;
		}
	}
	/* the parse's frame, back */
	for (l = 0; l < 3; l++)
		list_put(lists[l], &parse[l]);
	list_put(&rc->render_passes.l, &passes);
	list_put(&rc->global_param_mvo.l, &mvo[0]);
	list_put(&rc->global_param_mvo_tr.l, &mvo[1]);
	rc->fb_X_CLIP = saved.fb_X_CLIP;
	rc->fb_Y_CLIP = saved.fb_Y_CLIP;
	list_put(&rc->mv_clip.l, &mvclip);
	list_put(&rc->mv_clip_range.l, &mvclip_range);
	rc->gpu = false;
	rc->gpu_modtrig = 0;
	return d;
}

bool __wrap_ta_parse_vdrc(TA_context *ctx)
{
	TaGpuFrame gf;
	bool table_ok, prepared, rv;
	u32 flags, bytes;
	double t0, t1, t2, t3, ms;
	struct tgt_out g, m;
	int scan_ok;
	char why[256];
	const char *d;

	if (!ready)
		setup();
	st.seen++;
	table_ok = ctx->walk_count != TA_WALK_INVALID || tad_end(&ctx->tad) == ctx->tad.thd_root;
	/* (once untimed: the textures the frame asks for are converted, and the
	 * pass timed below finds them made, as the parse after it does) */
	flags = (gl_device ? 0 : TA_GPU_PROVOKE) | TA_GPU_INSTANCE;
	snap_pre(ctx);
	saved_bg = *TA_LIST_HEAD(ctx->rend.global_param_op, PolyParam);
	saved_x_clip = ctx->rend.fb_X_CLIP;
	saved_y_clip = ctx->rend.fb_Y_CLIP;
	ta_gpu_prepare(ctx, &layout, table, table_room, &gf, flags);
	t0 = now_us();
	prepared = ta_gpu_prepare(ctx, &layout, table, table_room, &gf, flags);
	t1 = now_us();
	if (prepared)
		memset(TA_LIST_HEAD(ctx->rend.verts, Vertex) + 4, 0, (gf.verts - 4) * sizeof(Vertex));
	t2 = now_us();
	rv = __real_ta_parse_vdrc(ctx);
	t3 = now_us();

	if (!prepared)
	{
		if (!table_ok)
			st.no_table++;
		else
			st.fallback++;
		return rv;
	}
	st.prepared++;
	if (!rv && !ctx->rend.Overrun)
	{
		st.empty++;
		return rv;
	}
	bytes = (u32)(tad_end(&ctx->tad) - ctx->tad.thd_root);
	scan_ok = tgt_model(ctx->tad.thd_root, table, gf.trd, &m);
	tgt_gpu(ctx->tad.thd_root, bytes, table, gf.table_words, gf.trd, &g, &ms);
	d = compare_model(&gf, &g, &m, table);
	if (d != NULL && st.model_differ++ < 10)
		printf("ta_gpu_test: frame %u: the GPU differs from the model: %s\n", st.seen, d);
	if (!scan_ok && st.scan_differ++ < 10)
		printf("ta_gpu_test: frame %u: the model's scan differs from its states\n", st.seen);

	if (ctx->rend.Overrun)
	{
		st.overrun++;
		if (g.frm[TAG_O_OVERRUN] != 0)
			st.overrun_agree++;
		return rv;
	}
	if ((u32)TA_LIST_USED(ctx->rend.verts, Vertex) != gf.verts)
	{
		if (st.corner++ < 5)
			printf("ta_gpu_test: frame %u: the parse made %d vertices, the walk %u\n", st.seen, TA_LIST_USED(ctx->rend.verts, Vertex), gf.verts);
		return rv;
	}
	st.compared++;
	st.verts += gf.verts;
	st.idx += g.frm[TAG_O_NIDX];
	st.strips += g.frm[TAG_O_NSTRIPS];
	push_double(&st.prep_us, t1 - t0);
	push_double(&st.parse_us, t3 - t2);
	st.gpu_ms += ms;
	d = compare(ctx, &gf, &g, why, sizeof(why));
	if (d == NULL)
		st.same++;
	else if (st.differ++ < 20)
		printf("ta_gpu_test: frame %u differs from the parse: %s\n", st.seen, d);
	/* and as the renderers draw it: its draw groups, by the device's
	 * commands and records (the frame's tables made again, the same) */
	d = compare_draw(ctx, &g, flags | TA_GPU_SORT | TA_GPU_SHADOWS, why, sizeof(why));
	if (d != NULL && *d == 0)
		st.draw_left++;
	else
	{
		st.drawn++;
		if (d == NULL)
			st.draw_same++;
		else if (st.draw_differ++ < 20)
			printf("ta_gpu_test: frame %u made ready to draw differs from the parse: %s\n", st.seen, d);
	}
	/* (every so often: it parses and decodes the frame nine times more) */
	if (st.compared % 50 == 1)
		edge_check(ctx, &gf, &g);
	if (st.compared % 300 == 0)
		report();
	return rv;
}
