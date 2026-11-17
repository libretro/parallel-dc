/* The compute renderer's frame words (core/rend/tile/compute_frame.c) for
 * frames, registers, settings and renderers made at random: each frame's
 * words, the count, the primitives and the reason said into a hash, with
 * every call made to the renderer, and the hash printed every so many
 * frames. What it prints must be what it always was
 * (compute_frame_expected.txt; compute_frame_run.sh). */
#define PVR_REGS_FOR_RENDERER
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/xform.h"
#include "rend/tile/pvr_compute.h"
#ifdef COMPUTE_FRAME_TEST_OLD
/* (the class this was, through a shim: compute_frame_shim.cpp) */
u32 old_pack(u32 width, u32 height, u32 tileLog2, u32 pool, u32 runs, u32 chunks,
		bool lastPicture, bool lastScaled, u32 rttScale, u32 *out, u32 room, u32 *prims, u32 *why);
u32 old_max_words(void);
u32 old_max_prims(void);
u32 old_max_rtt_tiles(void);
u32 old_rtt_scale(void);
u32 old_write_out(u32 height);
u32 old_tile_log2(u32 width);
void old_scalers_off(bool off);
#else
#include "rend/tile/compute_frame.h"
#endif

settings_t settings;
u8 *rend_pvr_regs;
TA_context *_pvrrc;

static u8 regs[pvr_RegSize];
static TA_context ctx;
static bool flat_strips;

bool ta_flat_strips(void)
{
	return flat_strips;
}

static u32 hash = 2166136261u;

void mix(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		hash ^= (v >> (i * 8)) & 0xff;
		hash *= 16777619u;
	}
}

static u32 seed = 0x2545f491u;

static u32 rnd(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

static u32 rn(u32 n)
{
	return rnd() % n;
}

/* ---- The renderer ---- */

static xform viewport;
static int base[4];
static u32 slot_calls;

u32 t_slot(u64 texid, u32 tsp)
{
	u32 h = (u32)texid * 2654435761u ^ (u32)(texid >> 32) ^ tsp * 40503u ^ slot_calls++ * 97u;
	mix(0x100);
	mix((u32)texid);
	mix((u32)(texid >> 32));
	mix(tsp);
	h ^= h >> 15;
	return h % 61 == 0 ? PVRC_NONE : h % PVRC_TEX_SLOTS;
}

const xform *t_begin(void)
{
	mix(0x101);
	return &viewport;
}

void t_base_scissor(int *x, int *y, int *width, int *height)
{
	mix(0x102);
	*x = base[0];
	*y = base[1];
	*width = base[2];
	*height = base[3];
}

/* (it changes the vertices, which must then be copied as changed) */
void t_provoking_vertices(void)
{
	Vertex *v = TA_LIST_HEAD(pvrrc.verts, Vertex);
	int i, n = TA_LIST_USED(pvrrc.verts, Vertex);
	mix(0x103);
	for (i = 0; i < n; i += 3)
		v[i].col[0] ^= 0x5a;
}

#ifndef COMPUTE_FRAME_TEST_OLD
static u32 slot_of(void *self, u64 texid, u32 tsp)
{
	(void)self;
	return t_slot(texid, tsp);
}

static const xform *begin_of(void *self)
{
	(void)self;
	return t_begin();
}

static void base_scissor_of(void *self, int *x, int *y, int *width, int *height)
{
	(void)self;
	t_base_scissor(x, y, width, height);
}

static void provoking_of(void *self)
{
	(void)self;
	t_provoking_vertices();
}
#endif

/* ---- The frames ---- */

#define MAX_POLYS 48
#define MAX_MVPS 24
#define MAX_VERTS 64
#define MAX_IDX 96
#define MAX_TRIGS 32
#define MAX_PASSES 12

static PolyParam ops[MAX_POLYS], pts[MAX_POLYS], trs[MAX_POLYS];
static ModifierVolumeParam mvos[MAX_MVPS], mvts[MAX_MVPS];
static Vertex verts[MAX_VERTS];
static u32 idx[MAX_IDX];
static ModTriangle trigs[MAX_TRIGS];
static RenderPass passes[MAX_PASSES];

static void set_list(ta_list_t *l, void *base_, size_t elem, u32 used, u32 cap)
{
	l->base = (unsigned char *)base_;
	l->cur = l->base + used * elem;
	l->end = l->base + cap * elem;
	l->overrun = NULL;
	l->name = NULL;
}

/* A word whose fields are as often 0 or all ones as anything */
static u32 word(void)
{
	switch (rn(4))
	{
	case 0: return rnd() & rnd();
	case 1: return rnd() | rnd();
	default: return rnd();
	}
}

static u32 tile_clip_word(void)
{
	u32 w = rnd() & 0x3fffff;
	switch (rn(5))
	{
	case 0: return w;	/* off */
	case 1: return w | (2u << 28);
	case 2: return w | (3u << 28);
	case 3: return (rn(4) << 28) | 0x3e0fc0;	/* the whole screen */
	default: return rnd();
	}
}

static void make_poly(PolyParam *p)
{
	memset(p, 0, sizeof(*p));
	p->first = rn(4) == 0 ? rnd() : rn(MAX_IDX);
	p->count = rn(8);
	p->texid = rn(6) == 0 ? (u64)-1 : ((u64)rnd() << 32 | rnd());
	p->tsp = word();
	p->tcw = word();
	p->pcw = word();
	p->isp = word();
	p->tileclip = tile_clip_word();
	if (rn(2))
	{
		p->tsp1 = (u32)-1;
		p->tcw1 = (u32)-1;
		p->texid1 = (u64)-1;
	}
	else
	{
		p->tsp1 = rn(3) ? word() : (u32)-1;
		p->tcw1 = word();
		p->texid1 = rn(3) == 0 ? (u64)-1 : ((u64)rnd() << 32 | rnd());
	}
}

static void make_mvp(ModifierVolumeParam *m)
{
	memset(m, 0, sizeof(*m));
	m->first = rn(MAX_TRIGS);
	m->count = rn(3) == 0 ? 0 : rn(5);
	m->isp = word();
	m->tileclip = tile_clip_word();
}

static float pick(void)
{
	/* quarters, so that clip rectangles land on halves */
	static const float q[] = { 0.25f, 0.5f, 0.75f, 1.f, 1.25f, 1.5f, 2.f, 3.f, 4.f };
	if (rn(3))
		return q[rn(9)];
	return (float)(rnd() % 40000) / 1000.f - 4.f;
}

static void make_registers(void)
{
	u32 i;
	for (i = 0; i < pvr_RegSize; i += 4)
	{
		const u32 w = word();
		memcpy(regs + i, &w, 4);
	}
}

static u32 make_frame(void)
{
	rend_context *rc = &pvrrc;
	u32 nop = rn(MAX_POLYS + 1), npt = rn(MAX_POLYS + 1), ntr = rn(MAX_POLYS + 1);
	u32 nmvo = rn(MAX_MVPS + 1), nmvt = rn(MAX_MVPS + 1);
	u32 npasses = rn(5) == 0 ? rn(MAX_PASSES + 1) : 1 + rn(3);
	u32 i, k;
	u32 nverts = rn(MAX_VERTS + 1), nidx = rn(MAX_IDX + 1), ntrigs = rn(MAX_TRIGS + 1);

	if (rn(3) == 0)
	{
		nop = rn(4);
		npt = rn(4);
		ntr = rn(4);
	}
	for (i = 0; i < nop; i++)
		make_poly(&ops[i]);
	for (i = 0; i < npt; i++)
		make_poly(&pts[i]);
	for (i = 0; i < ntr; i++)
		make_poly(&trs[i]);
	for (i = 0; i < nmvo; i++)
		make_mvp(&mvos[i]);
	for (i = 0; i < nmvt; i++)
		make_mvp(&mvts[i]);
	for (i = 0; i < nverts; i++)
		for (k = 0; k < sizeof(Vertex); k++)
			((u8 *)&verts[i])[k] = (u8)rnd();
	for (i = 0; i < nidx; i++)
		idx[i] = rn(MAX_VERTS);
	for (i = 0; i < ntrigs; i++)
		for (k = 0; k < sizeof(ModTriangle); k++)
			((u8 *)&trigs[i])[k] = (u8)rnd();
	/* the passes' counts: mostly growing, now and then not */
	for (i = 0; i < npasses; i++)
	{
		RenderPass *p = &passes[i];
		const bool last = i + 1 == npasses;
		memset(p, 0, sizeof(*p));
		p->autosort = rn(2);
		p->z_clear = rn(2);
		p->mv_op_tr_shared = rn(3) == 0;
		p->op_count = last || rn(6) == 0 ? nop : rn(nop + 1);
		p->pt_count = last || rn(6) == 0 ? npt : rn(npt + 1);
		p->tr_count = last || rn(6) == 0 ? ntr : rn(ntr + 1);
		p->mvo_count = last || rn(6) == 0 ? nmvo : rn(nmvo + 1);
		p->mvo_tr_count = last || rn(6) == 0 ? nmvt : rn(nmvt + 1);
		if (i > 0 && rn(4))
		{
			const RenderPass *q = &passes[i - 1];
			if (p->op_count < q->op_count) p->op_count = q->op_count;
			if (p->pt_count < q->pt_count) p->pt_count = q->pt_count;
			if (p->tr_count < q->tr_count) p->tr_count = q->tr_count;
			if (p->mvo_count < q->mvo_count) p->mvo_count = q->mvo_count;
			if (p->mvo_tr_count < q->mvo_tr_count) p->mvo_tr_count = q->mvo_tr_count;
		}
	}
	set_list(&rc->global_param_op.l, ops, sizeof(PolyParam), nop, MAX_POLYS);
	set_list(&rc->global_param_pt.l, pts, sizeof(PolyParam), npt, MAX_POLYS);
	set_list(&rc->global_param_tr.l, trs, sizeof(PolyParam), ntr, MAX_POLYS);
	set_list(&rc->global_param_mvo.l, mvos, sizeof(ModifierVolumeParam), nmvo, MAX_MVPS);
	set_list(&rc->global_param_mvo_tr.l, mvts, sizeof(ModifierVolumeParam), nmvt, MAX_MVPS);
	set_list(&rc->verts.l, verts, sizeof(Vertex), nverts, MAX_VERTS);
	set_list(&rc->idx.l, idx, 4, nidx, MAX_IDX - rn(7));
	set_list(&rc->modtrig.l, trigs, sizeof(ModTriangle), ntrigs, MAX_TRIGS);
	set_list(&rc->render_passes.l, passes, sizeof(RenderPass), npasses, MAX_PASSES);

	rc->gpu = rn(4) == 0;
	rc->isRTT = rn(3) == 0;
	rc->clearFramebuffer = rn(2);
	rc->fb_X_CLIP.full = rnd() & 0x07ff07ff;
	rc->fb_Y_CLIP.full = rnd() & 0x03ff03ff;
	if (rn(2))
	{
		rc->fb_X_CLIP.full = (rnd() & 0x1ff) | ((640 + rn(400)) << 16);
		rc->fb_Y_CLIP.full = (rnd() & 0xff) | ((480 + rn(200)) << 16);
	}
	rc->fog_clamp_min = rnd();
	rc->fog_clamp_max = rnd();
	rc->fZ_max = rn(4) == 0 ? 0.f : (float)(rnd() % 100000) / 7.f + 0.001f;

	viewport.sx = pick();
	viewport.sy = rn(2) ? -pick() : pick();
	viewport.sz = rn(8) == 0 ? -pick() : pick();
	viewport.tx = (float)((int)rn(2000) - 1000) / 4.f;
	viewport.ty = (float)((int)rn(2000) - 1000) / 4.f;
	viewport.tz = pick();
	if (rn(2))
	{
		base[0] = 0;
		base[1] = 0;
		base[2] = 640 + rn(2000);
		base[3] = 480 + rn(1500);
	}
	else
	{
		base[0] = (int)rn(3000) - 500;
		base[1] = (int)rn(3000) - 500;
		base[2] = (int)rn(4000) - 500;
		base[3] = (int)rn(4000) - 500;
	}

	flat_strips = rn(2);
	settings.System = rn(4) == 0 ? DC_PLATFORM_NAOMI2 : DC_PLATFORM_DREAMCAST;
	settings.rend.RenderToTextureUpscale = (int)rn(10) - 1;
	settings.rend.TextureUpscale = rn(3) ? 1 : 2;
	settings.rend.DumpTextures = rn(8) == 0;
	settings.rend.CustomTextures = rn(8) == 0;
	return npasses;
}

/* (Pack()'s reasons: none, passes, room, textures) */
#define CF_WHYS 4
#define ROOM_MAX 65536
static u32 words[ROOM_MAX + 64];

static u32 pack_one(void)
{
	u32 width = rn(4) ? 640 + rn(3) * 320 : 1 + rn(4000);
	u32 height = rn(4) ? 480 + rn(3) * 240 : 1 + rn(3000);
	u32 tileLog2 = rn(4) ? (width > 1280 ? 6 : 5) : 3 + rn(5);
	u32 pool = rnd();
	u32 chunks = rn(4);
	u32 runs = rn(4) ? 0x7fffffffu : rn(20000);
	bool lastPicture = rn(2), lastScaled = rn(2);
	u32 rttScale = rn(10);
	u32 room = rn(5) == 0 ? rn(4000) : ROOM_MAX;
	u32 prims = 0xdeadbeef, why = 0xdeadbeef, n, i, last = 0;
	bool off = rn(2);

	for (i = 0; i < ROOM_MAX + 64; i++)
		words[i] = 0xa5a5a5a5u ^ i;
	slot_calls = 0;
#ifdef COMPUTE_FRAME_TEST_OLD
	old_scalers_off(off);
	n = old_pack(width, height, tileLog2, pool, runs, chunks, lastPicture, lastScaled, rttScale,
			words, room, &prims, &why);
#else
	{
		compute_frame_layout l;
		compute_frame_renderer r;
		compute_frame_scalers_off = off;
		l.width = width;
		l.height = height;
		l.tileLog2 = tileLog2;
		l.pool = pool;
		l.runs = runs;
		l.chunks = chunks;
		l.lastPicture = lastPicture;
		l.lastScaled = lastScaled;
		l.rttScale = rttScale;
		r.self = &l;
		r.slot = slot_of;
		r.begin = begin_of;
		r.base_scissor = base_scissor_of;
		r.provoking_vertices = provoking_of;
		n = compute_frame_pack(&l, &r, words, room, &prims, &why);
	}
#endif
	mix(n);
	mix(prims);
	mix(why);
	/* the words written, up to the last */
	for (i = 0; i < ROOM_MAX + 64; i++)
		if (words[i] != (0xa5a5a5a5u ^ i))
			last = i + 1;
	mix(last);
	for (i = 0; i < last; i++)
		mix(words[i]);
	/* the vertices, as the renderer left them */
	for (i = 0; i < (u32)TA_LIST_USED(pvrrc.verts, Vertex); i++)
		mix(TA_LIST_HEAD(pvrrc.verts, Vertex)[i].col[0]);
	/* ...and the same frame in a word less room than it took: refused
	 * before anything is asked of the renderer */
	if (n != 0)
	{
		u32 why2 = 0, prims2 = 0xdeadbeef, n2;
#ifdef COMPUTE_FRAME_TEST_OLD
		n2 = old_pack(width, height, tileLog2, pool, runs, chunks, lastPicture, lastScaled, rttScale,
				words, n - 1, &prims2, &why2);
#else
		{
			compute_frame_layout l;
			compute_frame_renderer r;
			memset(&l, 0, sizeof(l));
			l.width = width;
			l.height = height;
			r.self = NULL;
			r.slot = NULL;
			r.begin = NULL;
			r.base_scissor = NULL;
			r.provoking_vertices = NULL;
			n2 = compute_frame_pack(&l, &r, words, n - 1, &prims2, &why2);
		}
#endif
		mix(n2);
		mix(prims2);
		mix(why2);
	}
	return why;
}

static void statics(void)
{
	u32 i;
#ifdef COMPUTE_FRAME_TEST_OLD
	mix(old_max_words());
	mix(old_max_prims());
	mix(old_max_rtt_tiles());
	mix(old_rtt_scale());
	for (i = 0; i < 8; i++)
	{
		const u32 h = rn(4) ? rn(3000) : rn(20000);
		old_scalers_off(rn(4) == 0);
		mix(old_write_out(h));
		mix(old_tile_log2(rn(3000)));
	}
#else
	mix(compute_frame_max_words());
	mix(compute_frame_max_prims());
	mix(compute_frame_max_rtt_tiles());
	mix(compute_frame_rtt_scale());
	for (i = 0; i < 8; i++)
	{
		const u32 h = rn(4) ? rn(3000) : rn(20000);
		compute_frame_scalers_off = rn(4) == 0;
		mix(compute_frame_write_out(h));
		mix(compute_frame_tile_log2(rn(3000)));
	}
#endif
}

int main(int argc, char **argv)
{
	const u32 frames = argc > 1 ? (u32)atoi(argv[1]) : 6000;
	u32 f, why, hist[CF_WHYS + 1];
	memset(hist, 0, sizeof(hist));
	rend_pvr_regs = regs;
	_pvrrc = &ctx;
	for (f = 0; f < frames; f++)
	{
		if (f % 16 == 0)
			make_registers();
		make_frame();
		statics();
		why = pack_one();
		hist[why < CF_WHYS ? why : CF_WHYS]++;
		if (f % 500 == 499)
			printf("%u %08x\n", f + 1, hash);
	}
	/* (how many frames each reason had: every one is tried) */
	for (f = 0; f <= CF_WHYS; f++)
		printf("why %u: %u\n", f, hist[f]);
	printf("compute_frame_test: %u frames %08x\n", frames, hash);
	return 0;
}
