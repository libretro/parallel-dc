/* The parse's speed (ta_parse_vdrc()) on the frames a game draws: each
 * frame parsed again, as it was before the parse, PARSE_BENCH_REPEAT times
 * (32), and timed. Linked into one program with the core's objects and
 * tools/threads/headless.c, with ta_parse_vdrc() wrapped: see
 * ta_parse_bench.sh. A frame's time is its fastest parse. At the end: the
 * renders timed, and of the screen's (not those to textures) their
 * vertices a frame, and their time a frame and a vertex - the median
 * frame's, and all of them together. And a check: each frame's parse made
 * once more with no strip known good to the indices (ta_strip_ok_ignored),
 * and what it made the same; "check: ..." at the end. */
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool __real_ta_parse_vdrc(TA_context *ctx);
void *__real_dlopen(const char *file, int mode);

/* headless.c opens the core; "self" is the program, the core linked in */
void *__wrap_dlopen(const char *file, int mode)
{
	return __real_dlopen(file != NULL && strcmp(file, "self") == 0 ? NULL : file, mode);
}
extern int ta_parse_cnt;

static double *times;			/* ns a screen's frame (its fastest parse) */
static unsigned ntimes, times_cap;
static unsigned long long known_good;	/* strips the vertex loop found drawable whole */
static unsigned different;		/* frames whose parse came out otherwise without them */
static unsigned renders, skip;
static unsigned long long vertices;
static double total;

static double now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* The frame as it was before the parse (FillBGP()'s), to parse it again */
static struct
{
	PolyParam bg;
	Vertex bgv[4];
	FB_X_CLIP_type x_clip;
	FB_Y_CLIP_type y_clip;
	bool rtt, fb, clear;
} pre;

static void again(TA_context *ctx)
{
	rend_context *rc = &ctx->rend;
	rend_context_clear(rc);
	*(PolyParam *)ta_list_append(&rc->global_param_op.l, sizeof(PolyParam)) = pre.bg;
	memcpy(ta_list_append(&rc->verts.l, sizeof(pre.bgv)), pre.bgv, sizeof(pre.bgv));
	rc->fb_X_CLIP = pre.x_clip;
	rc->fb_Y_CLIP = pre.y_clip;
	rc->isRTT = pre.rtt;
	rc->isRenderFramebuffer = pre.fb;
	rc->clearFramebuffer = pre.clear;
}

/* What the parse made: its indices, vertices and polygons */
static u32 sum(const rend_context *rc)
{
	const ta_list_t *lists[3];
	u32 h = 2166136261u, i;
	const u8 *p = (const u8 *)rc->idx.l.base;
	int l;

	lists[0] = &rc->global_param_op.l;
	lists[1] = &rc->global_param_pt.l;
	lists[2] = &rc->global_param_tr.l;
	for (i = 0; i < TA_LIST_BYTES(rc->idx); i++)
		h = (h ^ p[i]) * 16777619u;
	p = (const u8 *)rc->verts.l.base;
	for (i = 0; i < TA_LIST_BYTES(rc->verts); i++)
		h = (h ^ p[i]) * 16777619u;
	for (l = 0; l < 3; l++)
	{
		const PolyParam *pp;
		for (pp = (const PolyParam *)lists[l]->base; pp != (const PolyParam *)lists[l]->cur; pp++)
		{
			h = (h ^ pp->first) * 16777619u;
			h = (h ^ pp->count) * 16777619u;
			h = (h ^ pp->pcw) * 16777619u;
		}
	}
	return h;
}

static unsigned env_or(const char *name, unsigned dflt)
{
	const char *v = getenv(name);
	return v != NULL ? (unsigned)atoi(v) : dflt;
}

bool __wrap_ta_parse_vdrc(TA_context *ctx)
{
	static unsigned repeat, from;
	const rend_context *rc = &ctx->rend;
	int cnt, l;
	double best = 1e30;
	unsigned i;
	u32 looked, h, w;
	bool rv;

	if (skip == 0)
	{
		repeat = env_or("PARSE_BENCH_REPEAT", 32);
		from = env_or("PARSE_BENCH_FROM", 100);
	}
	if (skip++ < from)
		return __real_ta_parse_vdrc(ctx);
	pre.bg = *TA_LIST_HEAD(rc->global_param_op, PolyParam);
	memcpy(pre.bgv, rc->verts.l.base, sizeof(pre.bgv));
	pre.x_clip = rc->fb_X_CLIP;
	pre.y_clip = rc->fb_Y_CLIP;
	pre.rtt = rc->isRTT;
	pre.fb = rc->isRenderFramebuffer;
	pre.clear = rc->clearFramebuffer;
	cnt = ta_parse_cnt;
	for (i = 0; i < repeat; i++)
	{
		double t0, t;
		again(ctx);
		t0 = now_ns();
		__real_ta_parse_vdrc(ctx);
		t = now_ns() - t0;
		if (t < best)
			best = t;
		ta_parse_cnt = cnt;
	}
	/* once with no strip known good to the indices, for the check */
	again(ctx);
	ta_strip_ok_ignored = 1;
	__real_ta_parse_vdrc(ctx);
	ta_strip_ok_ignored = 0;
	ta_parse_cnt = cnt;
	looked = sum(rc);
	/* and the one the frame is drawn from */
	again(ctx);
	rv = __real_ta_parse_vdrc(ctx);
	h = sum(rc);
	renders++;
	if (h != looked)
		different++;
	for (l = 0; l < 3; l++)
		for (w = 0; w < rc->strip_ok[l].hi; w++)
			known_good += (unsigned)__builtin_popcount(rc->strip_ok[l].bits[w]);
	/* what the parse made, for two builds' to be compared frame by frame */
	if (getenv("PARSE_BENCH_SUMS"))
		printf("sum %u %d %d %08x\n", renders, TA_LIST_USED(rc->idx, u32), TA_LIST_USED(rc->verts, Vertex), h);
	if (!rc->isRTT)
	{
		if (ntimes == times_cap)
		{
			times_cap = times_cap ? times_cap * 2 : 1024;
			times = (double *)realloc(times, times_cap * sizeof(double));
		}
		times[ntimes++] = best;
		total += best;
		vertices += (unsigned long long)TA_LIST_USED(rc->verts, Vertex);
	}
	return rv;
}

static int by_value(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static void report(void)
{
	double v, median;

	if (ntimes == 0)
	{
		printf("ta_parse_bench: no frame timed\n");
		return;
	}
	qsort(times, ntimes, sizeof(double), by_value);
	v = (double)vertices / ntimes;
	median = times[ntimes / 2];
	printf("ta_parse_bench: %u renders, %u of the screen with %.0f vertices a frame; parse: median %.0f ns a frame "
			"(%.3f a vertex), all %.3f ns a vertex\n", renders, ntimes, v, median, median / v, total / vertices);
	printf("check: %llu strips known good to the indices, %u renders different without them\n", known_good, different);
}

/* (headless.c leaves through _exit(), past the destructors) */
void __real__exit(int status);
void __wrap__exit(int status)
{
	report();
	fflush(stdout);
	__real__exit(status);
}
