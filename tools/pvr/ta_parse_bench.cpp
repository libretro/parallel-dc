/* The parse's speed (ta_parse_vdrc()) on the frames a game draws: each
 * frame parsed again, as it was before the parse, PARSE_BENCH_REPEAT times
 * (32), and timed. Linked into one program with the core's objects and
 * tools/threads/headless.c, with ta_parse_vdrc() wrapped: see
 * ta_parse_bench.sh. A frame's time is its fastest parse. At the end: the
 * renders timed, and of the screen's (not those to textures) their
 * vertices a frame, and their time a frame and a vertex - the median
 * frame's, and all of them together. */
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

extern "C" bool __real__Z13ta_parse_vdrcP10TA_context(TA_context *ctx);
extern "C" void *__real_dlopen(const char *file, int mode);

/* headless.c opens the core; "self" is the program, the core linked in */
extern "C" void *__wrap_dlopen(const char *file, int mode)
{
	return __real_dlopen(file != NULL && strcmp(file, "self") == 0 ? NULL : file, mode);
}
extern int ta_parse_cnt;

static std::vector<double> times;	// ns a screen's frame (its fastest parse)
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
	rend_context &rc = ctx->rend;
	rc.Clear();
	*rc.global_param_op.Append() = pre.bg;
	memcpy(rc.verts.Append(4), pre.bgv, sizeof(pre.bgv));
	rc.fb_X_CLIP = pre.x_clip;
	rc.fb_Y_CLIP = pre.y_clip;
	rc.isRTT = pre.rtt;
	rc.isRenderFramebuffer = pre.fb;
	rc.clearFramebuffer = pre.clear;
}

extern "C" bool __wrap__Z13ta_parse_vdrcP10TA_context(TA_context *ctx)
{
	static const unsigned repeat = getenv("PARSE_BENCH_REPEAT") ? (unsigned)atoi(getenv("PARSE_BENCH_REPEAT")) : 32;
	static const unsigned from = getenv("PARSE_BENCH_FROM") ? (unsigned)atoi(getenv("PARSE_BENCH_FROM")) : 100;
	const rend_context &rc = ctx->rend;

	if (skip++ < from)
		return __real__Z13ta_parse_vdrcP10TA_context(ctx);
	pre.bg = *rc.global_param_op.head();
	memcpy(pre.bgv, rc.verts.head(), sizeof(pre.bgv));
	pre.x_clip = rc.fb_X_CLIP;
	pre.y_clip = rc.fb_Y_CLIP;
	pre.rtt = rc.isRTT;
	pre.fb = rc.isRenderFramebuffer;
	pre.clear = rc.clearFramebuffer;
	const int cnt = ta_parse_cnt;
	double best = 1e30;
	for (unsigned i = 0; i < repeat; i++)
	{
		again(ctx);
		const double t0 = now_ns();
		__real__Z13ta_parse_vdrcP10TA_context(ctx);
		best = std::min(best, now_ns() - t0);
		ta_parse_cnt = cnt;
	}
	// and the one the frame is drawn from
	again(ctx);
	const bool rv = __real__Z13ta_parse_vdrcP10TA_context(ctx);
	renders++;
	if (getenv("PARSE_BENCH_SUMS"))
	{
		// what the parse made, for two builds' to be compared frame by frame
		const List<PolyParam> *lists[3] = { &rc.global_param_op, &rc.global_param_pt, &rc.global_param_tr };
		u32 h = 2166136261u;
		const u8 *p = (const u8 *)rc.idx.head();
		for (int i = 0; i < rc.idx.bytes(); i++)
			h = (h ^ p[i]) * 16777619u;
		p = (const u8 *)rc.verts.head();
		for (int i = 0; i < rc.verts.bytes(); i++)
			h = (h ^ p[i]) * 16777619u;
		for (int l = 0; l < 3; l++)
			for (const PolyParam *pp = lists[l]->head(); pp != lists[l]->end(); pp++)
			{
				h = (h ^ pp->first) * 16777619u;
				h = (h ^ pp->count) * 16777619u;
				h = (h ^ pp->pcw.full) * 16777619u;
			}
		printf("sum %u %d %d %08x\n", renders, rc.idx.used(), rc.verts.used(), h);
	}
	if (!rc.isRTT)
	{
		times.push_back(best);
		total += best;
		vertices += (unsigned long long)rc.verts.used();
	}
	return rv;
}

static void report(void)
{
	if (times.empty())
	{
		printf("ta_parse_bench: no frame timed\n");
		return;
	}
	std::sort(times.begin(), times.end());
	const double v = (double)vertices / times.size();
	const double median = times[times.size() / 2];
	printf("ta_parse_bench: %u renders, %u of the screen with %.0f vertices a frame; parse: median %.0f ns a frame "
			"(%.3f a vertex), all %.3f ns a vertex\n", renders, (unsigned)times.size(), v, median, median / v, total / vertices);
}

/* (headless.c leaves through _exit(), past the destructors) */
extern "C" void __real__exit(int status);
extern "C" void __wrap__exit(int status)
{
	report();
	fflush(stdout);
	__real__exit(status);
}
