/* The render-to-texture watch (core/rend/rtt_watch.c) by itself, every
 * function it has called at random: pictures of every pixel format,
 * dithered and not, in and out of video memory and of the slots there
 * are, and video memory touched, faulted on, overwritten and replaced over
 * and around them, with the renderer handing its pictures over each way
 * it can (its own memory, the scratch room, none, nothing). Every call it
 * makes - the pages watched and opened, the pictures read, released and
 * begun, the textures made again - and video memory after each step go
 * into hashes; they must be what they always were (rtt_watch_expected.txt).
 * See rtt_watch_run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "rend/rtt_watch.h"
#include "hw/mem/_vmem.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/modules/mmu.h"
#if !defined(TARGET_NO_THREADS)
#include <rthreads/rthreads.h>
#endif

#define STEPS 4000
#define PICTURE_MAX ((720 * 4 + 16) * 576)

unsigned VRAM_SIZE = 8 * 1024 * 1024;
VArray2 vram;
settings_t settings;
bool mmu_strict;
RegisterStruct CCN[SH4_CCN_REGS];
retro_log_printf_t log_cb;

static u8 watched[VRAM_SIZE_MAX / PAGE_SIZE];
static u8 *picture;	/* the renderer's own memory for a picture */
static u32 h = 2166136261u;
static u32 rng = 1;
static int read_way;	/* how the renderer hands its picture over */
static u32 last_addr, last_bytes;	/* the last picture's */

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

static u32 rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

void os_DebugBreak(void)
{
	printf("rtt_watch_rand: died\n");
	exit(1);
}

void _vmem_watch_vram(u32 addr, u32 size)
{
	u32 p;
	mix(0x1000); mix(addr); mix(size);
	for (p = addr / PAGE_SIZE; p <= (addr + size - 1) / PAGE_SIZE; p++)
		watched[p] = 1;
}

void _vmem_unwatch_vram(u32 addr, u32 size)
{
	u32 p;
	mix(0x2000); mix(addr); mix(size);
	for (p = addr / PAGE_SIZE; p <= (addr + size - 1) / PAGE_SIZE; p++)
		watched[p] = 0;
}

void _vmem_open_watched_vram(u32 addr, u32 size)
{
	mix(0x3000); mix(addr); mix(size);
}

bool _vmem_vram_page_watched(u32 page)
{
	return watched[page] != 0;
}

u32 _vmem_get_vram_offset(void *addr)
{
	const u8 *a = (const u8 *)addr;
	if (a < vram.data || a >= vram.data + VRAM_SIZE)
		return (u32)-1;
	return (u32)(a - vram.data);
}

void VramRendered(u32 offset, u32 size, const void *target)
{
	mix(0x4000); mix(offset); mix(size); mix((u32)(uintptr_t)target);
}

void VramLockedWriteRange(u32 offset, u32 size)
{
	mix(0x5000); mix(offset); mix(size);
}

static void watch_mix(const RttWatch *w)
{
	mix(w->addr); mix(w->bytes); mix(w->w); mix(w->h); mix((u32)w->tex);
	mix(w->serial); mix(w->owned); mix(w->age);
}

/* The renderer */
static void backend_read(const RttWatch *w, u8 *scratch, RttWatchTake take)
{
	const size_t bytes = (size_t)w->w * w->h * 4;
	size_t i, pitch;

	mix(0x6000); watch_mix(w);
	switch (read_way++ % 5)
	{
	case 0:	/* its own memory, lines further apart than they need be */
		pitch = (size_t)w->w * 4 + 16;
		for (i = 0; i < pitch * w->h; i++)
			picture[i] = (u8)(i * 31 + w->tex + (i >> 7));
		take(w, picture, pitch);
		break;
	case 1:	/* the scratch room */
		for (i = 0; i < bytes; i++)
			scratch[i] = (u8)(i * 17 + w->serial);
		take(w, scratch, (size_t)w->w * 4);
		break;
	case 2:	/* none */
		take(w, NULL, 0);
		break;
	case 3:	/* nothing at all */
		break;
	default:	/* every value of a byte, the dither's ends */
		for (i = 0; i < bytes; i++)
			scratch[i] = (u8)(rnd() & 1 ? 255 - (i & 7) : i & 7);
		take(w, scratch, (size_t)w->w * 4);
		break;
	}
}

static void backend_release(uintptr_t tex)
{
	mix(0x7000); mix((u32)tex);
}

static void backend_prefetch(const RttWatch *const *watches, unsigned count)
{
	unsigned i;
	mix(0x8000); mix(count);
	for (i = 0; i < count; i++)
		mix(watches[i]->serial);
}

static RttWatchBackend backends[2];

#if !defined(TARGET_NO_THREADS)
static u32 other_offset, other_size;

/* A thread that neither renders nor runs the machine (the frontend saving
 * a state from one of its own) */
static void other_thread(void *arg)
{
	(void)arg;
	rtt_watch_touch(other_offset, other_size);
}
#endif

static u32 offset_any(void)
{
	switch (rnd() % 6)
	{
	case 0:
		return VRAM_SIZE - 1 - rnd() % (3 * PAGE_SIZE);
	case 1:
		return VRAM_SIZE + rnd() % PAGE_SIZE;
	default:
		return rnd() % (VRAM_SIZE / 16) * 4;	/* (where the pictures are) */
	}
}

static u32 size_any(void)
{
	switch (rnd() % 7)
	{
	case 0:
		return 0;
	case 5:	/* past the end of video memory, or round the end of a u32 */
		return rnd() & 1 ? VRAM_SIZE : 0xffffffffu - rnd() % 4096;
	case 6:
		return rnd() % (VRAM_SIZE / 4);
	case 1:
		return 1 + rnd() % 16;
	default:
		return 1 + rnd() % (6 * PAGE_SIZE);
	}
}

static void add_one(void)
{
	RttWatch w;
	memset(&w, 0, sizeof(w));
	w.w = 1 + rnd() % 200;
	w.h = 1 + rnd() % 160;
	if (rnd() % 3 == 0)
		w.w = w.w / 8 + 1;
	else if (rnd() % 64 == 0)
	{
		/* larger than the room made for one of the screen's size */
		w.w = 640 + rnd() % 81;
		w.h = 480 + rnd() % 97;
	}
	w.stride = w.w * 2 + (rnd() % 3) * 2 * (rnd() % 64);
	w.addr = rnd() % (VRAM_SIZE / 16) * 4;
	if (rnd() % 16 == 0)
		w.addr = (VRAM_SIZE - rnd() % (w.stride * w.h + 64)) & ~3u;
	w.bytes = rnd() % 32 == 0 ? 0 : w.stride * (w.h - 1) + w.w * 2;
	w.x0 = rnd() % 8;
	w.y0 = rnd() % 8;
	w.packmode = rnd() % 4;
	w.kval_bit = rnd() & 1 ? 0x8000 : 0;
	w.alpha_threshold = rnd() % 256;
	w.dither = rnd() % 3 == 0;
	w.tex = 100 + rnd() % 40;
	w.scale = 1;
	w.owned = rnd() % 4 == 0;
	w.age = rnd() % 4;
	w.serial = rnd();
	w.used = rnd() & 1;
	w.target = (const BaseTextureCacheData *)(uintptr_t)(rnd() % 3 * 16);
	mmu_strict = rnd() % 24 == 0;
	last_addr = w.addr;
	last_bytes = w.bytes;
	rtt_watch_add(&w, &backends[rnd() % 8 == 0]);
	mmu_strict = false;
}

static void step(void)
{
	RttWatch got;
	u32 i, off;

	switch (rnd() % 24)
	{
	case 0: case 1: case 2: case 3: case 4: case 5:
		add_one();
		break;
	case 6:
		if (rnd() & 1)
			rtt_watch_supersede(offset_any(), size_any());
		else	/* over a picture, or a part of it, from its start or before */
		{
			const u32 before = rnd() % 3 == 0 ? rnd() % 64 * 4 : 0;
			if (last_addr >= before)
				rtt_watch_supersede(last_addr - before, before + (rnd() & 1 ? last_bytes : 1 + rnd() % (last_bytes + 1)));
		}
		break;
	case 7:
		mix(rtt_watch_take(100 + rnd() % 40) ? 1 : 2);
		break;
	case 8: case 9:
		off = offset_any();
		mix(rtt_watch_fault(vram.data + off) ? 3 : 4);
		break;
	case 10: case 11:
		rtt_watch_touch(offset_any(), size_any());
		break;
	case 12: case 13:
		rtt_watch_overwrite(offset_any(), size_any());
		break;
	case 14:
		rtt_watch_flush();
		break;
	case 15:
		memset(&got, 0, sizeof(got));
		if (rtt_watch_pending(offset_any(), size_any(), &got))
		{
			mix(5);
			watch_mix(&got);
		}
		else
			mix(6);
		break;
	case 16: case 17: case 18:
		rtt_watch_frame();
		break;
	case 19:
		if (rnd() % 4 == 0)
			rtt_watch_forget();
		break;
	case 20:
		if (rnd() % 8 == 0)
			rtt_watch_term();
#if !defined(TARGET_NO_THREADS)
		else
		{
			sthread_t *t;
			other_offset = offset_any();
			other_size = size_any();
			t = sthread_create(other_thread, NULL);
			if (t != NULL)
				sthread_join(t);
		}
#endif
		break;
	case 21:
		rtt_watch_reserve();
		break;
	case 22:	/* (rtt_watch_serve() is only for what is handed over) */
		rtt_watch_touch(offset_any() & ~(u32)PAGE_MASK, PAGE_SIZE);
		break;
	default:	/* the game writes */
		for (i = 0; i < 16; i++)
			vram.data[rnd() % VRAM_SIZE] = (u8)rnd();
		break;
	}
}

static u32 left(void)
{
	u32 s = 2166136261u, i;
	for (i = 0; i < VRAM_SIZE / PAGE_SIZE; i++)
		s = (s ^ watched[i]) * 16777619u;
	for (i = 0; i < VRAM_SIZE; i++)
		s = (s ^ vram.data[i]) * 16777619u;
	return s;
}

int main(void)
{
	int r, n;
	vram.data = (u8 *)calloc(VRAM_SIZE_MAX, 1);
	vram.size = VRAM_SIZE;
	picture = (u8 *)malloc(PICTURE_MAX);
	if (vram.data == NULL || picture == NULL)
		return 1;
	backends[0].read = backend_read;
	backends[0].release = backend_release;
	backends[0].prefetch = backend_prefetch;
	backends[1] = backends[0];
	backends[1].prefetch = NULL;
	rtt_watch_emu_thread();
	/* (before any picture: the thread that renders is not known yet) */
	rtt_watch_touch(0, VRAM_SIZE);
	rtt_watch_reserve();
	for (r = 0; r < 40; r++)
	{
		for (n = 0; n < STEPS / 40; n++)
			step();
		printf("%d: %08x %08x\n", r, (unsigned)h, (unsigned)left());
	}
	rtt_watch_flush();
	rtt_watch_term();
	printf("end: %08x %08x\n", (unsigned)h, (unsigned)left());
	free(picture);
	free(vram.data);
	return 0;
}
