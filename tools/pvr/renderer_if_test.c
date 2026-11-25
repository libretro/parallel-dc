/* The renderer interface (core/hw/pvr/Renderer_if.c, renderer_dispatch.c)
 * by itself, every function of it called at random: renders the game
 * starts - to the screen, to a texture, of the framebuffer it wrote, kept
 * for another render, overrun, not to be shown - drawn where they start or
 * handed to the libretro thread, vblanks with and without a framebuffer the
 * game wrote, renderers made, failing, falling back, kept pictures, states
 * of V22 saved and loaded. What it asks of the renderer, the frames, the
 * scheduler and the other thread goes into a hash, step by step; it must
 * be what it always was (renderer_if_expected.txt). The renderer is
 * renderer_if_shim.c's. See renderer_if_run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/spg.h"
#include "hw/sh4/sh4_if.h"
#include "rend/last_picture.h"
#include "rend/rtt_watch.h"
#include "rend/sorter.h"
#include "rend/texconv.h"
#include "cheats.h"
#include "serialize.h"
#include "libretro/emu_baton.h"

#define STEPS 6000
#define CTXS 4

u8 pvr_regs[pvr_RegSize];
u8 *rend_pvr_regs = pvr_regs;
settings_t settings;
TA_context *ta_ctx;
retro_log_printf_t log_cb;
emu_baton_t emu_baton;

static u32 h = 2166136261u;
static u32 rng = 3;
static TA_context ctxs[CTXS];
static u8 ctx_regs[CTXS][pvr_RendRegSize];
static TA_context *queued;	/* QueueRender()'s */
static bool picture_kept;

/* For renderer_if_shim.c: what each renderer made answers */
int fake_init_ok = 1;

void mix(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		h ^= v & 0xff;
		h *= 16777619u;
		v >>= 8;
	}
}

static u32 rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

static u32 ctx_id(const TA_context *c)
{
	return c == NULL ? 0xff : (u32)(c - ctxs);
}

/* --- what Renderer_if asks of the rest of the core --- */

unsigned VRAM_MASK = 0x7fffff;

bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	if (*dest != NULL)
	{
		memcpy(*dest, src, src_size);
		*dest = (u8 *)*dest + src_size;
	}
	*total_size += src_size;
	return true;
}

bool ra_unserialize(void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	memcpy(src, *dest, src_size);
	*dest = (u8 *)*dest + src_size;
	*total_size += src_size;
	return true;
}

void palette_update(void) { mix(0x100); }
void SortReserve(void) { mix(0x101); }
void rtt_watch_reserve(void) { mix(0x102); }
void texcache_scratch_free(void) { mix(0x103); }
void cheats_apply(void) { mix(0x104); }
void spg_screen_render(void) { mix(0x105); }
void SetREP(int size) { mix(0x106); mix((u32)size); }
void HostFloatModeBack(void) { mix(0x107); }
void RestoreHostRoundingMode(void) { mix(0x108); }

void last_picture_drop(void) { mix(0x109); picture_kept = false; }
const u8 *last_picture(int *width, int *height)
{
	static const u8 one = 0;
	mix(0x10a);
	if (!picture_kept)
		return NULL;
	*width = 320;
	*height = 240;
	return &one;
}

void ta_select_passes(TA_context *ctx) { mix(0x110); mix(ctx_id(ctx)); }
void tactx_Remove(TA_context *ctx) { mix(0x111); mix(ctx_id(ctx)); }
void tactx_Drawn(TA_context *ctx) { mix(0x112); mix(ctx_id(ctx)); }
void FillBGP(TA_context *ctx) { mix(0x113); mix(ctx_id(ctx)); }
void tactx_reset_lists(TA_context *ctx) { mix(0x114); mix(ctx_id(ctx)); }

void SetCurrentTARC(u32 addr)
{
	mix(0x115);
	mix(addr);
	ta_ctx = addr == TACTX_NONE ? NULL : rnd() % 8 == 0 ? NULL : &ctxs[addr % CTXS];
}

TA_context *tactx_Get(u32 addr)
{
	TA_context *c;
	mix(0x116);
	mix(addr);
	if (rnd() % 10 == 0)
		return NULL;
	c = &ctxs[rnd() % CTXS];
	/* (a frame as the game left it) */
	c->rend.isRenderFramebuffer = rnd() % 8 == 0;
	c->rend.keep = rnd() % 4 == 0;
	c->rend.Overrun = rnd() % 10 == 0;
	c->drawn = rnd() % 4 == 0;
	c->tad.thd_data = c->tad.thd_root + rnd() % 4096;
	return c;
}

bool QueueRender(TA_context *ctx)
{
	mix(0x117);
	mix(ctx_id(ctx));
	if (rnd() % 6 == 0)
		return false;
	queued = ctx;
	return true;
}

TA_context *DequeueRender(void)
{
	TA_context *c = queued;
	mix(0x118);
	mix(ctx_id(c));
	queued = NULL;
	return c;
}

void FinishRender(TA_context *ctx)
{
	mix(0x119);
	mix(ctx_id(ctx));
	mix(ctx->rend.isRTT); mix(ctx->rend.clearFramebuffer);
	mix(ctx->rend.fb_X_CLIP.full); mix(ctx->rend.fb_Y_CLIP.full);
	{
		u32 a, b;
		memcpy(&a, &ctx->rend.fog_clamp_min, 4);
		memcpy(&b, &ctx->rend.fog_clamp_max, 4);
		mix(a); mix(b);
	}
}

/* the other thread */
void emu_baton_wait_work_done(emu_baton_t *b) { (void)b; mix(0x120); }
void emu_baton_ack_work(emu_baton_t *b) { (void)b; mix(0x121); }
void emu_baton_hand_over(emu_baton_t *b, void *w)
{
	(void)b;
	mix(0x122);
	mix(ctx_id((TA_context *)w));
	{
		/* (the registers the render goes with) */
		const u8 *r = ((TA_context *)w)->regs;
		int i;
		for (i = 0; i < 0x200; i += 4)
			mix((u32)r[i] | (u32)r[i + 1] << 8 | (u32)r[i + 2] << 16 | (u32)r[i + 3] << 24);
	}
	/* the libretro thread draws it, as retro_run does */
	mix(rend_single_frame() ? 1 : 2);
}

static void regs_at_random(void)
{
	int i;
	for (i = 0; i < 32; i++)
		pvr_regs[rnd() % 0x200] = (u8)rnd();
	/* (a few framebuffers, a texture now and then) */
	FB_W_SOF1 = (rnd() % 3) * 0x200000 | (rnd() % 5 == 0 ? 0x1000000 : 0);
	FB_R_CTRL.full = rnd() & 0xff;
}

static void step(void)
{
	switch (rnd() % 22)
	{
	case 0: case 1: case 2: case 3: case 4:
		rend_start_render();
		break;
	case 5: case 6:
		rend_vblank();
		break;
	case 7:
		rend_end_render();
		break;
	case 8:
		mix(rend_frame_produced() ? 3 : 4);
		break;
	case 9:
		rend_skip_screen(rnd() % 3 == 0);
		break;
	case 10:
		regs_at_random();
		break;
	case 11:
		settings.rend.ThreadedRendering = rnd() & 1;
		break;
	case 12:	/* another renderer chosen */
		settings.pvr.rend = (int)(rnd() % 6);
		renderer_changed = true;
		break;
	case 13:
		fake_init_ok = rnd() % 4 != 0 ? 1 : (int)(rnd() % 2) * 2;
		break;
	case 14:
		rend_prepare_renderer();
		break;
	case 15:	/* the context goes, its picture kept */
		picture_kept = rnd() & 1;
		rend_keep_picture();
		rend_term_renderer();
		break;
	case 16:
		mix(rend_present_kept_picture() ? 5 : 6);
		break;
	case 17:
		rend_resize((int)(rnd() % 2000), (int)(rnd() % 2000));
		break;
	case 18:	/* a state of V22 saved and loaded, or one from before */
	{
		u8 buf[16], *p = buf;
		unsigned int size = 0;
		void *data = p;
		pvr_serialize_v22(&data, &size);
		mix(size);
		{
			u32 a, b;
			memcpy(&a, buf + 1, 4);
			memcpy(&b, buf + 5, 4);
			mix(buf[0]); mix(a); mix(b);
		}
		if (rnd() % 3 == 0)
			buf[0] = (u8)(rnd() & 3);
		data = buf;
		size = 0;
		if (rnd() % 4 == 0)
			pvr_state_before_v22();
		else
			pvr_unserialize_v22(&data, &size);
		break;
	}
	case 19:
		fb_dirty = rnd() % 3 != 0;
		break;
	case 20:
		check_framebuffer_write();
		break;
	default:
		mix(rend_single_frame() ? 7 : 8);
		break;
	}
	mix(fb_dirty); mix(fb_watch_addr_start); mix(fb_watch_addr_end);
	mix(pend_rend); mix(PARAM_BASE);
	mix((u32)max_idx); mix((u32)max_vtx); mix((u32)max_op); mix((u32)max_pt);
	mix((u32)max_tr); mix((u32)max_mvo); mix((u32)max_modt); mix((u32)ovrn);
	mix(ctx_id(ta_ctx));
}

int main(void)
{
	static u8 list_room[CTXS][8][4096];
	static u8 tad_room[CTXS][4096];
	int i, n;

	for (i = 0; i < CTXS; i++)
	{
		ta_list_t *lists[7];
		int l;
		TA_context *c = &ctxs[i];
		c->Address = (u32)i * 0x100000;
		c->regs = ctx_regs[i];
		c->tad.thd_root = c->tad.thd_data = tad_room[i];
		lists[0] = &c->rend.idx.l;
		lists[1] = &c->rend.verts.l;
		lists[2] = &c->rend.global_param_op.l;
		lists[3] = &c->rend.global_param_pt.l;
		lists[4] = &c->rend.global_param_tr.l;
		lists[5] = &c->rend.global_param_mvo.l;
		lists[6] = &c->rend.modtrig.l;
		for (l = 0; l < 7; l++)
		{
			lists[l]->base = list_room[i][l];
			lists[l]->cur = list_room[i][l] + (size_t)((i + 1) * (l + 3)) * 64;
		}
	}
	for (n = 0; n < STEPS; n++)
	{
		step();
		if (n % 200 == 199)
			printf("%d: %08x\n", n, (unsigned)h);
	}
	rend_term_renderer();
	printf("end: %08x\n", (unsigned)h);
	return 0;
}
