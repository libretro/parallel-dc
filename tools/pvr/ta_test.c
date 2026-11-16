/* The tile accelerator's front (core/hw/pvr/ta.c) and its frames
 * (ta_ctx.c): the state machine fed blocks every way they come - one at
 * a time, many at once, made in place - its lists begun, continued and
 * ended, frames looked up, handed over, drawn, kept, aged and recycled,
 * the GPU pool's slices taken and given back, the control-word walk's
 * tables, and save states of every version both ways. What it prints is
 * hashes of all it saw; they must be what they always were
 * (ta_expected.txt). See ta_run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "types.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta_list.h"
#include "hw/pvr/ta_index.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/holly/holly_intc.h"

#ifdef TA_TEST_OLD
/* (the old code's C++ functions and members, through a shim) */
#define rend_context_clear old_rend_context_clear
#define tactx_all_passes old_tactx_all_passes
#define tactx_mark_rend old_tactx_mark_rend
#define tactx_vert_room old_tactx_vert_room
#define tactx_idx_room old_tactx_idx_room
#define tactx_modtrig_room old_tactx_modtrig_room
#define tactx_mv_clip_room old_tactx_mv_clip_room
#define tactx_arena_rooms old_tactx_arena_rooms
#define tactx_arena_lists old_tactx_arena_lists
#define tactx_alloc_lists old_tactx_alloc_lists
#define tactx_reset_lists old_tactx_reset_lists
#define tactx_free_lists old_tactx_free_lists
#define SetCurrentTARC old_SetCurrentTARC
#define QueueRender old_QueueRender
#define DequeueRender old_DequeueRender
#define FinishRender old_FinishRender
#define tactx_Find old_tactx_Find
#define tactx_Alloc old_tactx_Alloc
#define tactx_Recycle old_tactx_Recycle
#define tactx_Get old_tactx_Get
#define tactx_Remove old_tactx_Remove
#define tactx_Drawn old_tactx_Drawn
#define SerializeTAContext old_SerializeTAContext
#define UnserializeTAContext old_UnserializeTAContext
#define pvr_serialize_v21 old_pvr_serialize_v21
#define pvr_unserialize_v21 old_pvr_unserialize_v21
#define pvr_state_before_v21 old_pvr_state_before_v21
#define pvr_serialize_v28 old_pvr_serialize_v28
#define pvr_unserialize_v28 old_pvr_unserialize_v28
#define pvr_state_before_v28 old_pvr_state_before_v28
#define tactx_gpu_done old_tactx_gpu_done
#define tactx_gpu_pool_attach old_tactx_gpu_pool_attach
#define tactx_gpu_pool_stats old_tactx_gpu_pool_stats
#define tactx_gpu_pool_detach old_tactx_gpu_pool_detach
void rend_context_clear(rend_context *r);
bool tactx_all_passes(const TA_context *ctx);
void tactx_mark_rend(TA_context *ctx, u32 render_pass);
size_t tactx_vert_room(void);
size_t tactx_idx_room(void);
size_t tactx_modtrig_room(void);
size_t tactx_mv_clip_room(void);
void tactx_arena_rooms(size_t *room);
void tactx_arena_lists(TA_context *ctx, ta_list_t **lists);
bool tactx_alloc_lists(TA_context *ctx, u8 *data, int slot);
void tactx_reset_lists(TA_context *ctx);
void tactx_free_lists(TA_context *ctx);
void SetCurrentTARC(u32 addr);
bool QueueRender(TA_context *ctx);
TA_context *DequeueRender(void);
void FinishRender(TA_context *ctx);
TA_context *tactx_Find(u32 addr, bool allocnew);
TA_context *tactx_Alloc(void);
void tactx_Recycle(TA_context *ctx);
TA_context *tactx_Get(u32 addr);
void tactx_Remove(TA_context *ctx);
void tactx_Drawn(TA_context *ctx);
void SerializeTAContext(void **data, unsigned int *total_size);
void UnserializeTAContext(void **data, unsigned int *total_size, serialize_version_enum version);
void pvr_serialize_v21(void **data, unsigned int *total_size);
bool pvr_unserialize_v21(void **data, unsigned int *total_size);
void pvr_state_before_v21(void);
void pvr_serialize_v28(void **data, unsigned int *total_size);
bool pvr_unserialize_v28(void **data, unsigned int *total_size);
void pvr_state_before_v28(void);
void tactx_gpu_done(u32 serial);
bool tactx_gpu_pool_attach(u8 *base, u32 slice, u32 count);
void tactx_gpu_pool_stats(u32 *short_frames, u32 *allocated);
void tactx_gpu_pool_detach(void);
#endif

/*
 * What is hashed
 */
static u32 h = 2166136261u;
static void hb(const void *p, size_t n)
{
	const u8 *b = (const u8 *)p;
	size_t i;
	for (i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
}
static void hu(u32 v) { hb(&v, sizeof(v)); }

/*
 * The rest of the machine, as far as the tile accelerator sees it
 */
settings_t settings;
u8 pvr_regs[pvr_RegSize];
u32 FrameCount;
u32 YUV_index;
u32 fskip;
retro_log_printf_t log_cb;

static u32 irq_count;
void asic_RaiseInterrupt(HollyInterruptID inter) { irq_count++; hu(0xA0000000u | (u32)inter); }
void asic_RaiseInterruptBothCLX(HollyInterruptID inter) { irq_count++; hu(0xB0000000u | (u32)inter); }

static u32 bind_hash = 2166136261u;
void ta_strip_ok_bind(ta_strip_ok_t *s, uint32_t *words, uint32_t polys)
{
	u32 v[2];
	u32 i;
	v[0] = words != NULL;
	v[1] = polys;
	for (i = 0; i < 8; i++)
		bind_hash = (bind_hash ^ ((const u8 *)v)[i]) * 16777619u;
	s->bits = words;
	s->polys = polys;
	s->hi = 0;
}

static u32 log_count, log_hash = 2166136261u;
static void test_log(enum retro_log_level level, const char *fmt, ...)
{
	char line[512];
	size_t i;
	va_list ap;
	va_start(ap, fmt);
	vsprintf(line, fmt, ap);
	va_end(ap);
	for (i = 0; line[i]; i++)
		log_hash = (log_hash ^ (u8)line[i]) * 16777619u;
	if (log_count++ < 12)
		printf("  log %d: %s", (int)level, line);
}

/* A table of polygon types, the same for both builds: by object control,
 * a header of 32 or 64 bytes and a vertex type */
u32 ta_test_lut(int i)
{
	const u32 vt = (u32)((i * 7 + (i >> 3)) % 18);
	return ((i % 3) == 0 ? 0x80000000u : 0x40000000u) | (u32)(i << 8) | vt;
}
#ifndef TA_TEST_OLD
const u32 *ta_type_lut(void)
{
	static u32 table[256];
	int i;
	for (i = 0; i < 256; i++)
		table[i] = ta_test_lut(i);
	return table;
}
#endif

static u8 *state_base;
static size_t state_limit;
static bool state_failed;
bool ra_serialize(const void *src, unsigned int size, void **dest, unsigned int *total)
{
	if (*dest != NULL && size != 0)
	{
		memcpy(*dest, src, size);
		*dest = (u8 *)*dest + size;
	}
	*total += size;
	return true;
}
bool ra_unserialize_has(const void *data, size_t size)
{
	size_t at;
	if (data == NULL || state_base == NULL)
		return false;
	at = (size_t)((const u8 *)data - state_base);
	return at <= state_limit && size <= state_limit - at;
}
void ra_unserialize_fail(void) { state_failed = true; }
bool ra_unserialize(void *dst, unsigned int size, void **src, unsigned int *total)
{
	if (*src != NULL && size != 0)
	{
		if (!ra_unserialize_has(*src, size))
		{
			state_failed = true;
			memset(dst, 0, size);
			*total += size;
			return false;
		}
		memcpy(dst, *src, size);
		*src = (u8 *)*src + size;
	}
	*total += size;
	return true;
}

/* The light gun's parts of a state */
static u8 gun[5] = { 1, 2, 3, 4, 5 };
static u8 gun28 = 9;
void spg_gun_serialize(void **data, unsigned int *total_size) { LIBRETRO_SA(gun, 5); }
void spg_gun_unserialize(void **data, unsigned int *total_size) { LIBRETRO_USA(gun, 5); }
void spg_gun_before_v21(void) { hu(0xC21); }
void spg_gun_serialize_v28(void **data, unsigned int *total_size) { LIBRETRO_S(gun28); }
void spg_gun_unserialize_v28(void **data, unsigned int *total_size) { LIBRETRO_US(gun28); }
void spg_gun_before_v28(void) { hu(0xC28); }

/*
 * The frames
 */
static u32 rng = 0x12345678u;
static u32 rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

/* A frame's own number, kept in the last bytes of its data's room: the
 * same frame found again, or another. (Linked with
 * -Wl,--wrap=posix_memalign: the room of a frame made is never one that
 * was given back with a number in it, whatever the allocator.) */
int __real_posix_memalign(void **p, size_t align, size_t size);
int __wrap_posix_memalign(void **p, size_t align, size_t size)
{
	int ret = __real_posix_memalign(p, align, size);
	if (ret == 0 && size >= 8)
		memset((u8 *)*p + size - 8, 0, 8);
	return ret;
}

#define TAG_MAGIC 0x7A67C0DEu
static u32 tag_next;
static u32 ctx_id(TA_context *c)
{
	u32 t[2];
	u8 *at;
	if (c == NULL)
		return 0;
	at = c->tad.thd_root + c->data_size - 8;
	memcpy(t, at, 8);
	if (t[0] != TAG_MAGIC)
	{
		t[0] = TAG_MAGIC;
		t[1] = ++tag_next;
		memcpy(at, t, 8);
	}
	return t[1];
}

static void hash_tad(const tad_context *t)
{
	u32 i;
	hu((u32)(t->thd_data - t->thd_root));
	hu((u32)(t->thd_old_data - t->thd_root));
	hu(t->render_pass_count);
	for (i = 0; i < t->render_pass_count && i < 10; i++)
		hu((u32)(t->render_passes[i] - t->thd_root));
	hb(t->ol_base, sizeof(t->ol_base));
}

/* All of a frame (that is not the one being written: its pointers are
 * in ta_tad), its data with it */
static void hash_ctx(TA_context *c)
{
	const tad_context *t = c == ta_ctx ? &ta_tad : &c->tad;
	hu(ctx_id(c));
	hu(c->Address);
	hu(c->data_size);
	hu(c->lastFrameUsed);
	hash_tad(t);
	hu(c->walk_size);
	hu(c->walk_count);
	hu(c->walk_restart);
	if (c->walk_count != TA_WALK_INVALID)
		hb(c->walk, (c->walk_count < c->walk_size ? c->walk_count : c->walk_size) * sizeof(u32));
	hu((u32)c->pool);
	hu(c->gpu_serial);
	hu(c->drawn);
	hu(c->rend.keep);
	hu(c->rend.Overrun);
	hb(t->thd_root, tad_end(t) - t->thd_root);
}

/* The state machine and the frame being written, after each step */
static void hash_front(void)
{
	hu(ta_fsm[2048]);
	hu(ta_fsm_cl);
	hu((u32)ta_vtx_list());
	hu(ta_vtx_half());
	if (ta_ctx != NULL)
	{
		hu(ta_ctx->Address);
		hash_tad(&ta_tad);
		hu(ta_ctx->walk_count);
		hu(ta_ctx->walk_restart);
	}
	else
	{
		hu(0xDEAD);
		hu(ta_tad.thd_root == NULL && ta_tad.thd_data == NULL && ta_tad.thd_old_data == NULL);
	}
}

/* A block of one kind or another */
static void make_block(u8 *b)
{
	u32 i, w, r = rnd() % 100;
	u32 para;
	for (i = 0; i < 32; i += 4)
	{
		w = rnd();
		memcpy(b + i, &w, 4);
	}
	if (r < 55)
		para = 7;
	else if (r < 72)
		para = 4;
	else if (r < 78)
		para = 5;
	else if (r < 88)
		para = 0;
	else if (r < 94)
		para = 1 + rnd() % 2;
	else
		para = (rnd() & 1) ? 3 : 6;
	/* (one call a statement: the order is the same in every build) */
	w = para << 29;
	w |= (rnd() % 8) << 24;
	w |= rnd() & 0xFF;
	if (rnd() % 4 != 0)
	{
		w &= ~(7u << 24);
		w |= (rnd() % 5) << 24;
	}
	memcpy(b, &w, 4);
}

static u8 blocks[64 * 32 + 32];

/* @n blocks, one of three ways */
static void feed(u32 n)
{
	u8 *src = blocks + (32 - ((size_t)blocks & 31)) % 32;
	u32 i, how = rnd() % 3;
	if (how == 2)
	{
		u8 *room = ta_vtx_room(n);
		hu(room != NULL);
		if (room != NULL)
		{
			for (i = 0; i < n; i++)
				make_block(room + i * 32);
			ta_vtx_data((u32 *)room, n);
			return;
		}
	}
	for (i = 0; i < n; i++)
		make_block(src + i * 32);
	if (how == 0)
		for (i = 0; i < n; i++)
			ta_vtx_data32(src + i * 32);
	else
		ta_vtx_data((u32 *)src, n);
}

static void select_addr(u32 addr)
{
	TA_ISP_BASE = addr;
	TA_OL_BASE = rnd() & 0x3FFFFFFu;
}

/* A render the game starts of the frame at @addr: kept for another, or
 * handed over, drawn and given back */
static void render(u32 addr)
{
	TA_context *ctx = tactx_Get(addr);
	hu(0x5E);
	hu(ctx != NULL);
	if (ctx == NULL)
		return;
	hu(ctx == ta_ctx);
	hash_ctx(ctx);
	ctx->rend.keep = rnd() % 4 == 0;
	if (ctx->rend.keep)
	{
		ctx->rend.Overrun = false;
		if (ta_ctx == ctx)
			SetCurrentTARC(TACTX_NONE);
		ctx->drawn = true;
	}
	else
		tactx_Remove(ctx);
	hu(QueueRender(ctx));
	hu(DequeueRender() == ctx);
	hu(FrameCount);
	FinishRender(ctx);
	hu(DequeueRender() == NULL);
}

/* A state saved, the machine moved on, and the state loaded: what it
 * has must come back, and be saved the same again */
static u8 *state;
static void state_trip(const char *what)
{
	void *d = NULL;
	unsigned int size = 0, used, used2;
	u32 h1;
	u32 save_h = h;
	unsigned i;
	bool ok;

	pvr_serialize_v21(&d, &size);
	pvr_serialize_v28(&d, &size);
	d = state;
	used = 0;
	pvr_serialize_v21(&d, &used);
	pvr_serialize_v28(&d, &used);
	h = 2166136261u;
	hb(state, used);
	h1 = h;
	h = save_h;
	hu(size);
	hu(used);
	hu(h1);
	if (what != NULL)
		printf("%s: room %u, used %u, %08x\n", what, size, used, h1);

	/* moved on */
	tactx_Reset();
	for (i = 0; i < 3; i++)
	{
		select_addr((rnd() % 16) << 20);
		ta_vtx_ListInit();
		feed(1 + rnd() % 8);
		tactx_Get((rnd() % 16) << 20);
	}

	d = state;
	used2 = 0;
	state_base = state;
	state_limit = used;
	state_failed = false;
	ok = pvr_unserialize_v21(&d, &used2);
	ok = pvr_unserialize_v28(&d, &used2) && ok;
	state_base = NULL;
	hu(ok);
	hu(used2);
	hu(state_failed);
	hash_front();

	d = state + used;
	used2 = 0;
	pvr_serialize_v21(&d, &used2);
	pvr_serialize_v28(&d, &used2);
	save_h = h;
	h = 2166136261u;
	hb(state + used, used2);
	h1 = h == h1 && used2 == used;
	h = save_h;
	hu(h1);
	if (what != NULL)
		printf("%s: loaded %d, saved again the same %d\n", what, ok, (int)h1);
}

/* The state machine through a long run of everything */
static void run(u32 steps, u32 addrs, bool states)
{
	u32 s;
	for (s = 0; s < steps; s++)
	{
		u32 r = rnd() % 1000;
		hu(s);
		if (r < 20)
		{
			select_addr((rnd() % addrs) << 20);
			ta_vtx_ListInit();
		}
		else if (r < 30)
			ta_vtx_ListCont();
		else if (r < 33)
			ta_vtx_SoftReset();
		else if (r < 36)
			ta_vtx_Reset();
		else if (r < 48)
			render((rnd() % addrs) << 20);
		else if (r < 51)
			ta_set_walk(rnd() & 1);
		else if (r < 52)
			tactx_Reset();
		else if (r < 53 && states)
			state_trip(NULL);
		else
		{
			u32 most = rnd() % 4 == 0 ? 64 : 4;
			feed(1 + rnd() % most);
		}
		hash_front();
		if (s % 512 == 0 && ta_ctx != NULL)
			hash_ctx(ta_ctx);
	}
}

static void phase(const char *what)
{
	printf("%s: %08x irq %u binds %08x logs %u %08x\n", what, h, irq_count, bind_hash, log_count, log_hash);
}

/* A frame made by hand, its lists looked at */
static void lists(void)
{
	TA_context *c = (TA_context *)calloc(1, sizeof(TA_context));
	ta_list_t *al[PARSE_ARENA_PARTS];
	size_t room[PARSE_ARENA_PARTS];
	u8 *slice = (u8 *)OS_aligned_malloc(32, ta_data_size());
	int i;

	printf("rooms: verts %u idx %u modtrig %u mv_clip %u\n", (unsigned)tactx_vert_room(),
		(unsigned)tactx_idx_room(), (unsigned)tactx_modtrig_room(), (unsigned)tactx_mv_clip_room());
	tactx_arena_rooms(room);
	for (i = 0; i < PARSE_ARENA_PARTS; i++)
		hu((u32)room[i]);

	for (i = 0; i < 3; i++)
	{
		const rend_context *r = &c->rend;
		const ta_list_t *named[9];
		int j;
		bool ok;
		c->gpu_serial = 77;
		ok = tactx_alloc_lists(c, i == 1 ? slice : NULL, i == 1 ? 2 : i == 2 ? 5 : -1);
		hu(ok);
		hu(c->data_size);
		hu((u32)c->pool);
		hu(c->gpu_serial);
		hu(i == 1 ? c->tad.thd_root == slice : c->tad.thd_root != NULL);
		named[0] = &r->verts.l;
		named[1] = &r->idx.l;
		named[2] = &r->modtrig.l;
		named[3] = &r->global_param_mvo.l;
		named[4] = &r->global_param_mvo_tr.l;
		named[5] = &r->global_param_op.l;
		named[6] = &r->global_param_pt.l;
		named[7] = &r->global_param_tr.l;
		named[8] = &r->render_passes.l;
		for (j = 0; j < 9; j++)
		{
			hu(named[j]->overrun == (unsigned char *)&r->Overrun);
			if (named[j]->name != NULL)
				hb(named[j]->name, strlen(named[j]->name));
		}
		hu(c->regs != NULL);
		hu((u32)(r->verts.l.end - r->verts.l.base));
		hu((u32)(r->idx.l.end - r->idx.l.base));
		hu((u32)(r->modtrig.l.end - r->modtrig.l.base));
		hu((u32)(r->global_param_mvo.l.end - r->global_param_mvo.l.base));
		hu((u32)(r->global_param_mvo_tr.l.end - r->global_param_mvo_tr.l.base));
		hu((u32)(r->global_param_op.l.end - r->global_param_op.l.base));
		hu((u32)(r->global_param_pt.l.end - r->global_param_pt.l.base));
		hu((u32)(r->global_param_tr.l.end - r->global_param_tr.l.base));
		hu((u32)(r->render_passes.l.end - r->render_passes.l.base));
		hu((u32)(r->mv_clip.l.end - r->mv_clip.l.base));
		hu((u32)(r->mv_clip_range.l.end - r->mv_clip_range.l.base));
		hu(r->verts.l.cur == r->verts.l.base);
		hu(r->verts.l.overrun == (unsigned char *)&r->Overrun);
		hu(r->mv_clip.l.overrun == (unsigned char *)&r->mv_clip_full);
		hu(r->mv_clip_range.l.overrun == (unsigned char *)&r->mv_clip_full);
		hu(r->mv_clip.l.name == NULL);
		hu(r->global_param_tr.l.name != NULL && strcmp(r->global_param_tr.l.name, "global_param_tr") == 0);
		hu(r->region_tiles != NULL);
		hu(r->strip_ok_words != NULL);
		hu(r->strip_ok[1].bits == r->strip_ok_words + (r->strip_ok[0].polys + 31) / 32);
		hu(c->walk != NULL);
		hu(c->walk_size);
		hu(c->walk_count);
		hu(r->proc_start == c->tad.thd_root && r->proc_end == c->tad.thd_root);
		hu(r->pass_first);
		hu(r->pass_last);
		hu(r->keep);
		hu(c->drawn);
		hash_tad(&c->tad);
		hb(&r->fZ_min, sizeof(r->fZ_min));
		hb(&r->fZ_max, sizeof(r->fZ_max));

		tactx_arena_lists(c, al);
		hu(al[PARSE_ARENA_VERTS] == &c->rend.verts.l && al[PARSE_ARENA_IDX] == &c->rend.idx.l
			&& al[PARSE_ARENA_MODTRIG] == &c->rend.modtrig.l && al[PARSE_ARENA_MV_CLIP] == &c->rend.mv_clip.l);

		/* some of it used, and emptied again */
		c->tad.thd_data = c->tad.thd_root + 640;
		tad_continue(&c->tad);
		c->tad.thd_data += 320;
		tad_continue(&c->tad);
		c->tad.thd_data += 96;
		c->rend.pass_first = 0;
		c->rend.pass_last = 2;
		hu(tactx_all_passes(c));
		c->rend.pass_last = 1;
		hu(tactx_all_passes(c));
		c->rend.pass_first = 1;
		c->rend.pass_last = 9;
		hu(tactx_all_passes(c));
		{
			u32 p;
			for (p = 0; p < 3; p++)
			{
				tactx_mark_rend(c, p);
				hu((u32)(c->rend.proc_start - c->tad.thd_root));
				hu((u32)(c->rend.proc_end - c->tad.thd_root));
			}
			c->tad.thd_old_data = c->tad.thd_data;
			c->tad.thd_data = c->tad.thd_root;
			tactx_mark_rend(c, 2);
			hu((u32)(c->rend.proc_end - c->tad.thd_root));
		}
		c->rend.verts.l.cur += 3 * sizeof(Vertex);
		c->rend.global_param_op.l.cur += sizeof(PolyParam);
		c->rend.mv_clip_range.l.cur += 4;
		c->rend.region_tile_count = 7;
		c->rend.gpu = true;
		c->rend.gpu_modtrig = 5;
		c->rend.Overrun = true;
		c->rend.fZ_min = 3;
		c->rend.isRenderFramebuffer = true;
		c->rend.clearFramebuffer = true;
		c->rend.isRTT = true;
		c->rend.mv_clip_full = true;
		rend_context_clear(&c->rend);
		hu(r->verts.l.cur == r->verts.l.base && r->global_param_op.l.cur == r->global_param_op.l.base
			&& r->mv_clip_range.l.cur == r->mv_clip_range.l.base);
		hu(r->region_tile_count);
		hu(r->gpu);
		hu(r->gpu_modtrig);
		hu(r->Overrun);
		hu(r->isRenderFramebuffer);
		hu(r->clearFramebuffer);
		hu(r->isRTT);
		hu(r->mv_clip_full);
		hb(&r->fZ_min, sizeof(r->fZ_min));

		c->rend.keep = true;
		c->drawn = true;
		c->walk_count = 3;
		c->walk_restart = true;
		c->rend.pass_last = 4;
		c->rend.verts.l.cur += sizeof(Vertex);
		tactx_reset_lists(c);
		hu(r->verts.l.cur == r->verts.l.base);
		hu(r->keep);
		hu(c->drawn);
		hu(c->walk_count);
		hu(c->walk_restart);
		hu(r->pass_first);
		hu(r->pass_last);
		hu(r->proc_start == c->tad.thd_root && r->proc_end == c->tad.thd_root);
		hash_tad(&c->tad);

		tactx_free_lists(c);
		hu((u32)c->pool);
		hu(r->verts.l.base == NULL);
		hu(r->region_tiles == NULL);
		hu(r->strip_ok_words == NULL);
		hu(c->walk == NULL);
		hu(r->strip_ok[2].bits == NULL && r->strip_ok[2].polys == 0);
	}
	OS_aligned_free(slice);
	free(c);
}

/* A frame filled to the end of its room, and more sent */
static void overflow(void)
{
	u8 *src = blocks + (32 - ((size_t)blocks & 31)) % 32;
	u32 i, n;
	u32 w = (4u << 29);	/* an opaque polygon, then vertices */

	select_addr(9 << 20);
	ta_vtx_ListInit();
	memset(src, 0, 64 * 32);
	memcpy(src, &w, 4);
	ta_vtx_data32(src);
	w = 7u << 29;
	for (i = 0; i < 64; i++)
		memcpy(src + i * 32, &w, 4);
	n = ta_ctx->data_size / 32 - 1;
	while (n > 0)
	{
		u32 k = n < 64 ? n : 64;
		/* (near the end one at a time, to one block less than there is
		 * room for, all at once) */
		if (n == 65 || (n > 200 && rnd() % 2))
			ta_vtx_data((u32 *)src, k);
		else
		{
			k = 1;
			ta_vtx_data32(src);
		}
		n -= k;
	}
	hash_front();
	hu(ta_vtx_room(1) == NULL);
	hu(ta_vtx_room(0) == ta_tad.thd_data);
	ta_vtx_data32(src);
	ta_vtx_data((u32 *)src, 5);
	hash_front();
	hash_ctx(ta_ctx);
	/* the lists begun again: the room is the frame's again */
	ta_vtx_ListInit();
	hu(ta_vtx_room(3) == ta_tad.thd_root);
	ta_vtx_data((u32 *)src, 3);
	hash_front();

	/* more parameters than the walk's table has room for, in a frame that
	 * goes in a state: it is not to be trusted, as sent or as walked again */
	ta_set_walk(true);
	select_addr(10 << 20);
	ta_vtx_ListInit();
	w = 1u << 29;
	for (i = 0; i < 64; i++)
		memcpy(src + i * 32, &w, 4);
	for (n = 0; n < TA_WALK_MAX; n += 64)
		ta_vtx_data((u32 *)src, 64);
	hu(ta_ctx->walk_count);
	hu(ta_ctx->walk[TA_WALK_MAX - 1]);
	state_trip("walk just full");
	hu(ta_ctx->walk_count);
	hu(ta_ctx->walk[TA_WALK_MAX - 1]);
	ta_vtx_data((u32 *)src, 1);
	hash_front();
	state_trip("walk full");
	hu(ta_ctx->walk_count);
	ta_set_walk(false);
}

/* Frames not looked at for long given to new addresses, and every
 * address at once */
static void ages(void)
{
	u32 i;
	tactx_Reset();
	for (i = 0; i < 4; i++)
	{
		select_addr(i << 20);
		ta_vtx_ListInit();
		feed(2);
		hu(ctx_id(ta_ctx));
	}
	for (i = 0; i < 70; i++)
		hu(tactx_Get(15u << 20) != NULL);
	state_trip("aged");
	select_addr(1 << 20);
	ta_vtx_ListInit();
	for (i = 4; i < 7; i++)
	{
		select_addr(i << 20);
		ta_vtx_ListInit();
		hu(ctx_id(ta_ctx));
		hash_front();
	}
	for (i = 0; i < 16; i++)
	{
		select_addr(i << 20);
		ta_vtx_ListInit();
		feed(1);
		hu(ctx_id(ta_ctx));
		hu(ta_ctx->lastFrameUsed);
	}
	state_trip("ages");
	pvr_state_before_v28();
	state_trip(NULL);
	hu(tactx_Find(3 << 20, false) != NULL);
	hu(tactx_Find(3 << 20, false)->lastFrameUsed);
}

/* A frame not looked at for just as long as is let go, and one more;
 * and one of a machine whose frames were another size */
static void ages_edge(void)
{
	u32 g, i;
	for (g = 60; g <= 61; g++)
	{
		tactx_Reset();
		select_addr(0);
		ta_vtx_ListInit();
		feed(1);
		hu(ctx_id(ta_ctx));
		for (i = 0; i < g; i++)
			tactx_Get(15u << 20);
		select_addr(1 << 20);
		ta_vtx_ListInit();
		hu(ctx_id(ta_ctx));
		hu(ta_ctx->Address);
	}
	tactx_Reset();
	settings.System = DC_PLATFORM_NAOMI2;
	select_addr(2 << 20);
	ta_vtx_ListInit();
	feed(2);
	hu(ctx_id(ta_ctx));
	settings.System = DC_PLATFORM_DREAMCAST;
	for (i = 0; i < 70; i++)
		tactx_Get(15u << 20);
	select_addr(3 << 20);
	ta_vtx_ListInit();
	hu(ctx_id(ta_ctx));
	hu(ta_ctx->data_size);
	select_addr(2 << 20);
	ta_vtx_ListInit();
	hu(ctx_id(ta_ctx));
	hu(ta_ctx->data_size);
	hash_front();
}

/* Frames being written that do not all fit in a state, and one drawn
 * and kept after them */
static void state_room(void)
{
	static const u32 order[4] = { 3, 0, 1, 2 };
	u8 *src = blocks + (32 - ((size_t)blocks & 31)) % 32;
	u32 i, k, w;
	tactx_Reset();
	for (k = 0; k < 4; k++)
	{
		select_addr(order[k] << 20);
		ta_vtx_ListInit();
		w = 4u << 29;
		memset(src, k, 64 * 32);
		memcpy(src, &w, 4);
		ta_vtx_data32(src);
		w = 7u << 29;
		for (i = 0; i < 64; i++)
			memcpy(src + i * 32, &w, 4);
		for (i = 0; i < 350; i++)
			ta_vtx_data((u32 *)src, 64);
		if (k == 0)
		{
			TA_context *ctx = tactx_Get(order[k] << 20);
			ctx->rend.keep = true;
			SetCurrentTARC(TACTX_NONE);
			ctx->drawn = true;
		}
	}
	/* (and the last begun again, nothing sent yet) */
	ta_vtx_ListInit();
	state_trip("room");
}

/* A render handed over while another is, and the frame skipped */
static void queue(void)
{
	TA_context *a, *b;
	select_addr(2 << 20);
	ta_vtx_ListInit();
	feed(3);
	select_addr(3 << 20);
	ta_vtx_ListInit();
	feed(3);
	a = tactx_Get(2 << 20);
	tactx_Remove(a);
	b = tactx_Get(3 << 20);
	tactx_Remove(b);
	hu(ta_ctx == NULL);
	hu(QueueRender(a));
	hu(QueueRender(b));
	hu(DequeueRender() == a);
	FinishRender(a);
	FinishRender(NULL);
	hu(DequeueRender() == NULL);
	hu(FrameCount);
	hu(fskip);
}

/* Save states: the frame being written as old states had it, and the
 * newer states' frames, the broken ones among them */
static void put32(u8 **p, u32 v) { memcpy(*p, &v, 4); *p += 4; }
static void put8(u8 **p, u8 v) { **p = v; *p += 1; }

static void old_states(void)
{
	static const serialize_version_enum versions[2] = { V11, V12 };
	static const u32 addrs[5] = { 0, 1u << 20, 7u << 20, 2u << 20, 4u << 20 };
	void *d;
	unsigned int size;
	u8 *p;
	int v, k;

	ta_set_walk(true);
	d = NULL;
	size = 0;
	SerializeTAContext(&d, &size);
	hu(size);
	tactx_Reset();
	d = state;
	size = 0;
	SerializeTAContext(&d, &size);
	hu(size);
	hb(state, size);
	select_addr(5 << 20);
	ta_vtx_ListInit();
	feed(4);
	d = state;
	size = 0;
	SerializeTAContext(&d, &size);
	hu(size);
	hb(state, size);

	for (v = 0; v < 2; v++)
		for (k = 0; k < 5; k++)
		{
			unsigned int used = 0;
			u32 i;
			p = state;
			switch (k)
			{
			case 0:
				put32(&p, 0xFFFFFFFFu);
				break;
			case 1:
				put32(&p, 0x6123456u);
				put32(&p, 100);
				for (i = 0; i < 100; i++)
					put8(&p, (u8)(i * 3));
				put32(&p, 3);
				put32(&p, 33);
				put32(&p, 64);
				put32(&p, 1000);
				break;
			case 2:
				put32(&p, 7u << 20);
				put32(&p, 0x7FFFFFFFu);
				break;
			case 3:
				put32(&p, 2u << 20);
				put32(&p, 64);
				for (i = 0; i < 64; i++)
					put8(&p, (u8)i);
				put32(&p, 14);
				for (i = 0; i < 14; i++)
					put32(&p, i * 8);
				break;
			default:
				put32(&p, 4u << 20);
				put32(&p, 32);
				break;
			}
			if (k != 0)
			{
				/* (the frame the state names, with something sent to it,
				 * walked, and a mark past its last render pass) */
				select_addr(addrs[k]);
				ta_vtx_ListInit();
				feed(3);
				ta_vtx_ListCont();
				feed(1);
				ta_tad.render_passes[9] = ta_tad.thd_root + 32;
				hu(ta_ctx->walk_count);
			}
			state_base = state;
			state_limit = (size_t)(p - state);
			state_failed = false;
			d = state;
			UnserializeTAContext(&d, &used, versions[v]);
			state_base = NULL;
			hu(used);
			hu(state_failed);
			hash_front();
			if (ta_ctx != NULL && k != 0)
			{
				/* (what was loaded is in the context, and not in ta_tad) */
				const tad_context *t = &ta_ctx->tad;
				hash_tad(t);
				hu((u32)(t->render_passes[9] - t->thd_root));
				hu(ta_ctx->walk_count);
				hb(t->thd_root, t->thd_data - t->thd_root);
			}
			YUV_index = 77;
			pvr_state_before_v21();
			hu(YUV_index);
			hash_front();
			pvr_state_before_v28();
			hash_front();
		}
}

static void bad_states(void)
{
	/* (the first: more frames than there can be; the second and third:
	 * bits that mean nothing; then more data than a frame has; begun
	 * again and drawn; cut short; and no frames after the count) */
	static const u32 words[6] = { 0, 0x40 | 16, 0x40 | 8, 0x7FFFFFE0u, 64 | 3, 128 };
	u8 *p;
	int k;
	for (k = 0; k < 7; k++)
	{
		void *d = state;
		unsigned int used = 0;
		bool ok;
		u32 i;
		p = state;
		memcpy(p, gun, 5);
		p += 5;
		put8(&p, 0x33);
		put8(&p, k == 0 ? 0xF0 : 0x01);
		put8(&p, k == 0 ? 17 : 2);
		put8(&p, 1);
		if (k < 6)
		{
			const u32 n = (words[k] & ~31u) <= 128 ? words[k] & ~31u : 0;
			put8(&p, 0x13);
			put32(&p, words[k]);
			put8(&p, 12);
			for (i = 0; i < 12; i++)
				put32(&p, i * 40 + 3);
			for (i = 0; i < n; i++)
				put8(&p, (u8)(i ^ 0x5A));
			put8(&p, 0x4);
			put32(&p, 64 | 2);
			put8(&p, 0);
			for (i = 0; i < 64; i++)
				put8(&p, (u8)i);
		}
		state_base = state;
		state_limit = (size_t)(p - state);
		if (k == 5)
			state_limit -= 40;
		state_failed = false;
		ok = pvr_unserialize_v21(&d, &used);
		state_base = NULL;
		hu(ok);
		hu(used);
		hu(state_failed);
		hu(YUV_index);
		hash_front();
		if (ok)
		{
			TA_context *c = tactx_Find(3 << 20, false);
			hu(c != NULL);
			if (c != NULL)
				hash_ctx(c);
			c = tactx_Find(4 << 20, false);
			hu(c != NULL);
			if (c != NULL)
				hash_ctx(c);
		}
	}
}

/* The GPU pool's slices: taken, retired, done, short, and let go of with
 * a frame in one */
static u8 *pool_mem;
static void gpu_pool(void)
{
	const u32 size = ta_data_size();
	u8 *base = pool_mem + (32 - ((size_t)pool_mem & 31)) % 32;
	u32 st[2];
	static const u32 serial[3] = { 10, 20, 0x8FFFFFF0u };
	u32 i, k;
	TA_context *ctxs[6];

	tactx_Reset();
	hu(tactx_gpu_pool_attach(base, size - 32, 3));
	hu(tactx_gpu_pool_attach(base, size, 3));
	tactx_gpu_pool_stats(&st[0], &st[1]);
	hu(st[0]);
	hu(st[1]);
	for (k = 0; k < 3; k++)
	{
		for (i = 0; i < 6; i++)
		{
			select_addr(i << 20);
			ta_vtx_ListInit();
			feed(1 + i);
			hu((u32)ta_ctx->pool);
			hu(ctx_id(ta_ctx));
			hu(ta_ctx->gpu_serial);
			hu(ta_tad.thd_root >= base && ta_tad.thd_root < base + 3 * (size_t)size);
		}
		tactx_gpu_pool_stats(&st[0], &st[1]);
		hu(st[0]);
		hu(st[1]);
		for (i = 0; i < 6; i++)
		{
			ctxs[i] = tactx_Get(i << 20);
			hu(ctxs[i] != NULL);
			if (ctxs[i] == NULL)
				continue;
			tactx_Remove(ctxs[i]);
			ctxs[i]->gpu_serial = i == 2 ? 0 : serial[k] + i;
		}
		for (i = 0; i < 6; i++)
			if (ctxs[i] != NULL)
				tactx_Recycle(ctxs[i]);
		/* (the second done just now; and those of no work, of a long time
		 * ago, after the serials pass half their range) */
		tactx_gpu_done(serial[k] + 1);
	}
	/* three taken again, one being written, and the pool let go of */
	for (i = 10; i >= 8; i--)
	{
		select_addr(i << 20);
		ta_vtx_ListInit();
		feed(4);
		ta_vtx_ListCont();
		feed(5);
		ta_vtx_ListCont();
		feed(2);
		hu((u32)ta_ctx->pool);
		hash_ctx(ta_ctx);
	}
	tactx_gpu_pool_detach();
	hu((u32)ta_ctx->pool);
	hu(ta_ctx->gpu_serial);
	hu(ta_tad.thd_root >= base && ta_tad.thd_root < base + 3 * (size_t)size);
	hu(ta_ctx->tad.thd_root == ta_tad.thd_root);
	hash_ctx(ta_ctx);
	feed(5);
	hash_front();
	tactx_gpu_pool_stats(&st[0], &st[1]);
	hu(st[0]);
	hu(st[1]);
	tactx_Reset();
	/* (counted again from a new pool) */
	hu(tactx_gpu_pool_attach(base, size, 1));
	tactx_gpu_pool_stats(&st[0], &st[1]);
	hu(st[0]);
	hu(st[1]);
	tactx_gpu_pool_detach();
}

int main(void)
{
	log_cb = test_log;
	settings.System = DC_PLATFORM_DREAMCAST;
	ta_fsm_fill();
	ta_ctx_log_init();
	hb(ta_fsm, 2048);
	hu(ta_fsm_cl);
	printf("fsm: %08x, the list's warning set %d\n", h, ta_list_warn != NULL);

	state = (u8 *)malloc(4 * (size_t)TA_DATA_SIZE);
	pool_mem = (u8 *)malloc(3 * (size_t)TA_DATA_SIZE + 32);
	if (state == NULL || pool_mem == NULL)
		return 1;

	lists();
	phase("lists");
	run(20000, 4, false);
	phase("run");
	ta_set_walk(true);
	run(20000, 3, true);
	phase("run, walked, with states");
	ta_set_walk(false);
	run(5000, 16, true);
	phase("run, all addresses");
	overflow();
	phase("overflow");
	ta_set_walk(true);
	ages();
	ages_edge();
	state_room();
	phase("ages");
	queue();
	phase("queue");
	old_states();
	phase("old states");
	bad_states();
	phase("bad states");
	gpu_pool();
	phase("gpu pool");

	/* a NAOMI 2's frames are bigger: those of before made again */
	settings.System = DC_PLATFORM_NAOMI2;
	lists();
	select_addr(1 << 20);
	ta_vtx_ListInit();
	hu(ta_ctx->data_size);
	run(3000, 2, true);
	phase("naomi 2");
	tactx_Reset();
	settings.System = DC_PLATFORM_DREAMCAST;
	gpu_pool();
	phase("gpu pool again");
	settings.System = DC_PLATFORM_NAOMI2;
	settings.System = DC_PLATFORM_DREAMCAST;
	select_addr(1 << 20);
	ta_vtx_ListInit();
	hu(ta_ctx->data_size);
	run(2000, 2, false);
	phase("dreamcast again");

	tactx_Reset();
	free(state);
	free(pool_mem);
	return 0;
}
