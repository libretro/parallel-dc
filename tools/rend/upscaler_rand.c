/* The temporal upscaler's side in the core (core/rend/upscaler.c) on
 * frames made at random: strips under a few headers, their vertices moving
 * a little or jumping, appearing and going, frames reset, the frontend's
 * request coming, changing and refused. Where each vertex was, the jitter
 * and the inputs handed over go into a hash a frame; it must be what it
 * always was (upscaler_expected.txt). See upscaler_run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/pvr/ta_ctx.h"

#ifndef UPSCALER_TEST_OLD
#include "rend/upscaler.h"
#else
#include <libretro.h>
/* (the old code's C++ functions, through a shim) */
#define upscaler_match old_upscaler_match
#define upscaler_submit old_upscaler_submit
#define upscaler_set_interface old_upscaler_set_interface
#define upscaler_poll old_upscaler_poll
#define upscaler_active old_upscaler_active
#define upscaler_render_size old_upscaler_render_size
#define upscaler_jitter old_upscaler_jitter
#define upscaler_reset old_upscaler_reset
void old_upscaler_set_interface(const struct retro_hw_render_upscaler_interface *iface);
bool old_upscaler_poll(bool wanted);
bool old_upscaler_active(void);
void old_upscaler_render_size(int *width, int *height);
void old_upscaler_jitter(float *x, float *y);
void old_upscaler_match(const struct rend_context *ctx, float *prev_xy);
void old_upscaler_submit(const struct retro_hw_upscaler_image *motion_vectors,
		const struct retro_hw_upscaler_image *depth, unsigned flags);
void old_upscaler_reset(void);
#endif

#define FRAMES 1500
#define MAX_VERTS 4096
#define MAX_IDX 4096
#define MAX_POLYS 512

retro_log_printf_t log_cb;

static rend_context ctx;
static u32 h;
static u32 rng = 7;
static float prev[MAX_VERTS * 2];

/* the frontend */
static struct retro_hw_upscaler_request req;
static int refuse;	/* frames the request is refused for */
static int nothing;	/* frames a size of nothing is asked for */
static struct retro_hw_upscaler_request kept_req;

static u32 rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

static void mix(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		h ^= v & 0xff;
		h *= 16777619u;
		v >>= 8;
	}
}

static void mixf(float f)
{
	u32 v;
	memcpy(&v, &f, 4);
	mix(v);
}

static bool get_request(void *handle, struct retro_hw_upscaler_request *r)
{
	(void)handle;
	if (refuse > 0)
		return false;
	*r = req;
	return true;
}

static void set_inputs(void *handle, const struct retro_hw_upscaler_inputs *in)
{
	(void)handle;
	mix(0xabcd0000u | in->flags);
	mix((u32)in->motion_vectors.image);
	mix(in->motion_vectors.width);
	mix((u32)in->depth.image);
	mixf(in->jitter_x); mixf(in->jitter_y);
	mixf(in->mv_scale_x); mixf(in->mv_scale_y);
	mixf(in->pre_exposure); mixf(in->exposure_scale);
	mixf(in->frame_time_ms);
}

static struct retro_hw_render_upscaler_interface iface;

/* The headers strips are sent under: a few, so that they repeat */
typedef struct strip_kind
{
	u32 isp, tsp, tcw, pcw;
	u64 texid;
} strip_kind;
static strip_kind kinds[12];

/* The strips that last from one frame to the next: where they are */
typedef struct thing
{
	int kind, count, list;
	float x, y;
	bool alive;
} thing;
static thing things[64];

static void frame(void)
{
	Vertex *verts = TA_LIST_HEAD(ctx.verts, Vertex);
	u32 *idx = TA_LIST_HEAD(ctx.idx, u32);
	PolyParam *lists[2];
	int used[2] = { 0, 0 };
	u32 nv = 0, ni = 0;
	int t, v, l;

	lists[0] = TA_LIST_HEAD(ctx.global_param_op, PolyParam);
	lists[1] = TA_LIST_HEAD(ctx.global_param_pt, PolyParam);
	for (t = 0; t < 64; t++)
	{
		thing *th = &things[t];
		PolyParam *pp;
		if (rnd() % 16 == 0)
			th->alive = !th->alive;
		if (!th->alive)
			continue;
		switch (rnd() % 8)
		{
		case 0:	/* a jump */
			th->x += (float)(rnd() % 600) - 300.f;
			break;
		case 1:
			th->y += (float)(rnd() % 600) - 300.f;
			break;
		default:
			th->x += (float)(rnd() % 9) - 4.f + (float)(rnd() % 16) / 16.f;
			th->y += (float)(rnd() % 9) - 4.f;
			break;
		}
		if (rnd() % 32 == 0)
			th->count = (int)(rnd() % 7);
		l = th->list;
		pp = &lists[l][used[l]++];
		memset(pp, 0, sizeof(*pp));
		pp->isp = kinds[th->kind].isp;
		pp->tsp = kinds[th->kind].tsp;
		pp->tcw = kinds[th->kind].tcw;
		pp->pcw = kinds[th->kind].pcw;
		pp->texid = kinds[th->kind].texid;
		pp->first = ni;
		pp->count = (u32)th->count;
		for (v = 0; v < th->count; v++)
		{
			/* (a vertex now and then shared with the one before) */
			if (v > 0 && rnd() % 8 == 0)
				idx[ni++] = nv - 1;
			else
			{
				verts[nv].x = th->x + (float)(v * 13 % 40);
				verts[nv].y = th->y + (float)(v * 7 % 30);
				idx[ni++] = nv++;
			}
		}
	}
	/* (now and then a crowd, punch-through: the room made larger) */
	if (rnd() % 50 == 0)
		for (t = 200 + (int)(rnd() % 100); t > 0; t--)
		{
			PolyParam *pp = &lists[1][used[1]++];
			memset(pp, 0, sizeof(*pp));
			pp->isp = kinds[t % 12].isp;
			pp->tsp = kinds[t % 12].tsp;
			pp->first = ni;
			pp->count = 6;
			for (v = 0; v < 6; v++, nv++)
			{
				verts[nv].x = (float)(t * 3 + v);
				verts[nv].y = (float)(v * 5);
				idx[ni++] = nv;
			}
		}
	/* (vertices of other lists, never matched) */
	for (v = (int)(rnd() % 8); v > 0; v--, nv++)
	{
		verts[nv].x = (float)(rnd() % 640);
		verts[nv].y = (float)(rnd() % 480);
	}
	ctx.verts.l.cur = (unsigned char *)(verts + nv);
	ctx.idx.l.cur = (unsigned char *)(idx + ni);
	ctx.global_param_op.l.cur = (unsigned char *)(lists[0] + used[0]);
	ctx.global_param_pt.l.cur = (unsigned char *)(lists[1] + used[1]);
}

int main(void)
{
	int f, i;
	static u8 vbuf[MAX_VERTS * sizeof(Vertex)], ibuf[MAX_IDX * 4];
	static u8 opbuf[MAX_POLYS * sizeof(PolyParam)], ptbuf[MAX_POLYS * sizeof(PolyParam)];

	memset(&ctx, 0, sizeof(ctx));
	ctx.verts.l.base = ctx.verts.l.cur = vbuf;
	ctx.idx.l.base = ctx.idx.l.cur = ibuf;
	ctx.global_param_op.l.base = ctx.global_param_op.l.cur = opbuf;
	ctx.global_param_pt.l.base = ctx.global_param_pt.l.cur = ptbuf;
	for (i = 0; i < 12; i++)
	{
		kinds[i].isp = rnd();
		kinds[i].tsp = rnd() & (i < 6 ? 0xff : ~0u);	/* (some alike but for one word) */
		kinds[i].tcw = i < 4 ? 0 : rnd();
		kinds[i].pcw = rnd() % 3;
		kinds[i].texid = i % 3 == 0 ? 0 : ((u64)rnd() << 32) | rnd();
	}
	/* (alike but for the PCW, and but for the texture's high word) */
	kinds[1] = kinds[0];
	kinds[1].pcw ^= 1;
	kinds[4] = kinds[5];
	kinds[4].texid ^= (u64)1 << 40;
	for (i = 0; i < 64; i++)
	{
		things[i].kind = (int)(rnd() % 12);
		things[i].count = 3 + (int)(rnd() % 5);
		things[i].list = (int)(rnd() % 4 != 0);	/* (most punch-through) */
		things[i].x = (float)(rnd() % 640);
		things[i].y = (float)(rnd() % 480);
		things[i].alive = rnd() % 4 != 0;
	}
	iface.handle = &iface;
	iface.get_request = get_request;
	iface.set_inputs = set_inputs;
	req.render_width = 640;
	req.render_height = 480;
	req.output_width = 1280;
	req.output_height = 960;

	upscaler_set_interface(&iface);
	for (f = 0; f < FRAMES; f++)
	{
		struct retro_hw_upscaler_image mv, depth;
		float jx, jy;
		int w, ht;
		u32 n;

		h = 2166136261u;
		switch (rnd() % 160)
		{
		case 0:	/* the frontend asks for another size */
			req.render_width = 320 + rnd() % 960;
			req.render_height = 240 + rnd() % 720;
			req.output_width = req.render_width * (1 + rnd() % 4) - rnd() % 3;
			break;
		case 1:
			refuse = 1 + (int)(rnd() % 4);
			break;
		case 2:
			upscaler_reset();
			break;
		case 3:	/* the context goes, or goes and comes back */
			upscaler_set_interface(rnd() % 4 == 0 ? NULL : &iface);
			mix(upscaler_active() ? 5 : 6);
			break;
		case 4:	/* a size of nothing, a while */
			if (nothing == 0)
				kept_req = req;
			nothing = 1 + (int)(rnd() % 4);
			req.render_width = rnd() & 1 ? 0 : 640;
			req.render_height = req.render_width != 0 ? 0 : rnd() & 1 ? 0 : 480;
			break;
		case 5:	/* only the height another */
			req.render_height = 240 + rnd() % 720;
			break;
		}
		if (rnd() % 64 == 0)	/* (and comes back) */
			upscaler_set_interface(&iface);
		mix(upscaler_poll(rnd() % 16 != 0) ? 1 : 2);
		if (refuse > 0)
			refuse--;
		if (nothing > 0 && --nothing == 0)
			req = kept_req;
		mix(upscaler_active() ? 3 : 4);
		upscaler_render_size(&w, &ht);
		mix((u32)w); mix((u32)ht);
		upscaler_jitter(&jx, &jy);
		mixf(jx); mixf(jy);

		frame();
		upscaler_match(&ctx, prev);
		n = (u32)TA_LIST_USED(ctx.verts, Vertex);
		for (i = 0; i < (int)n * 2; i++)
			mixf(prev[i]);
		memset(&mv, 0, sizeof(mv));
		memset(&depth, 0, sizeof(depth));
		mv.image = (u64)(0x1000 + f);
		mv.width = (unsigned)w;
		depth.image = (u64)(0x2000 + f);
		if (rnd() % 8 != 0)
			upscaler_submit(&mv, &depth, rnd() % 4);
		printf("%d %u %08x\n", f, (unsigned)n, (unsigned)h);
	}
	return 0;
}
