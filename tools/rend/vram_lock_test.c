/* The locks on video memory (core/rend/vram_lock.c) by themselves: owners
 * locking ranges of it at random - small ones, ones over many pages, ones
 * out of it and backwards - and writes coming in every way there is (a
 * fault at an offset and at an address, a run of pages, a block copy, a
 * render over them, a save state), taken in at random times. Every change
 * of protection, every lock dropped, every warning and what is left after
 * each step goes into hashes; they must be what they always were
 * (vram_lock_expected.txt). See vram_lock_run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "rend/vram_lock.h"
#include "hw/pvr/pvr_mem.h"

#ifdef VRAM_LOCK_TEST_OLD
/* (the old code's C++ functions, through a shim) */
#define vramlock_reserve old_vramlock_reserve
#define vramlock_unlock old_vramlock_unlock
#define vramlock_collect old_vramlock_collect
#define vramlock_pending old_vramlock_pending
#define VramLockedWriteOffset old_VramLockedWriteOffset
#define VramLockedWrite old_VramLockedWrite
#define VramLockedWriteRange old_VramLockedWriteRange
#define VramRendered old_VramRendered
#define VramBlockWrite old_VramBlockWrite
#define vram_load old_vram_load
void vramlock_reserve(u32 textures);
void vramlock_unlock(vram_block *block);
void vramlock_collect(void);
bool vramlock_pending(void);
bool VramLockedWriteOffset(size_t offset);
bool VramLockedWrite(u8 *address);
void VramLockedWriteRange(u32 offset, u32 size);
void VramRendered(u32 offset, u32 size, const void *target);
void VramBlockWrite(u32 offset, u32 size);
void vram_load(const u8 *from);
u32 old_vram_write_epoch(void);
#define EPOCH() old_vram_write_epoch()
#else
#define EPOCH() vram_write_epoch
#endif

#define OWNERS 96
#define ROUNDS 40
#define STEPS 300

/* What the old code's textures had (the old shim's BaseTextureCacheData) */
typedef struct owner_t
{
	u32 dirty;
	vram_block *lock_block;
	u32 id;
} owner_t;

unsigned VRAM_SIZE;
u32 FrameCount;
retro_log_printf_t log_cb;

static owner_t owners[OWNERS];
static u8 *protect_bits;	/* a byte for each page */
static u8 *state;		/* a save state's video memory */
static u32 h = 2166136261u;	/* what happened, so far */
static u32 rng = 12345;
static bool small_locks;	/* each lock on a page: the locks run out first */
static bool small_first;	/* small_locks for the first round */

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

static void test_log(enum retro_log_level level, const char *fmt, ...)
{
	(void)fmt;
	mix(0x10000000u | (u32)level);
}

void os_DebugBreak(void)
{
	printf("vram_lock_test: died\n");
	exit(1);
}

/* Video memory's pages protected, as _vmem.c does it */
void _vmem_protect_vram(u32 addr, u32 size)
{
	u32 p;
	mix(0x20000000u); mix(addr); mix(size);
	for (p = addr / PAGE_SIZE; p < (addr + size) / PAGE_SIZE; p++)
		protect_bits[p] = 1;
}

void _vmem_unprotect_vram(u32 addr, u32 size)
{
	u32 p;
	mix(0x30000000u); mix(addr); mix(size);
	for (p = addr / PAGE_SIZE; p < (addr + size) / PAGE_SIZE; p++)
		protect_bits[p] = 0;
}

bool _vmem_vram_page_protected(u32 page)
{
	return protect_bits[page] != 0;
}

u32 _vmem_get_vram_offset(void *addr)
{
	const u8 *a = (const u8 *)addr;
	if (a < vram.data || a >= vram.data + VRAM_SIZE)
		return (u32)-1;
	return (u32)(a - vram.data);
}

void rtt_watch_overwrite(u32 offset, u32 size)
{
	mix(0x40000000u); mix(offset); mix(size);
}

void vramlock_dropped(void *owner)
{
	owner_t *o = (owner_t *)owner;
	mix(0x50000000u | o->id);
	o->dirty = FrameCount;
	o->lock_block = NULL;
}

/* An offset in video memory, now and then out of it */
static u32 offset_any(void)
{
	switch (rnd() % 8)
	{
	case 0:
		return VRAM_SIZE + (rnd() & 1) * (rnd() % (2 * PAGE_SIZE));
	case 1:
		return (rnd() % (VRAM_SIZE / PAGE_SIZE)) * PAGE_SIZE + PAGE_SIZE - 1;
	default:
		return rnd() % VRAM_SIZE;
	}
}

/* A size: mostly within a few pages, at times many, or none */
static u32 size_any(void)
{
	switch (rnd() % 10)
	{
	case 0:
		return 0;
	case 1:
		return rnd() % (VRAM_SIZE / 2);
	default:
		return 1 + rnd() % (4 * PAGE_SIZE);
	}
}

static void lock_one(owner_t *o)
{
	u32 start, end;
	if (small_locks)
	{
		start = rnd() % VRAM_SIZE;
		end = start | (PAGE_SIZE - 1);
		vramlock_lock(start, end, o, &o->lock_block);
		mix(0x60000000u | o->id);
		return;
	}
	switch (rnd() % 12)
	{
	case 0:	/* backwards */
		start = rnd() % VRAM_SIZE;
		end = start - 1 - rnd() % 64;
		if (end > start)
			end = start / 2;
		break;
	case 1:	/* past the end */
		start = VRAM_SIZE - 1 - rnd() % (3 * PAGE_SIZE);
		end = start + rnd() % (6 * PAGE_SIZE);
		break;
	case 2:	/* large */
		start = rnd() % VRAM_SIZE;
		end = start + rnd() % (64 * PAGE_SIZE);
		break;
	default:
		start = rnd() % VRAM_SIZE;
		end = start + rnd() % (3 * PAGE_SIZE);
		break;
	}
	vramlock_lock(start, end, o, &o->lock_block);
	mix(0x60000000u | o->id);
	mix(o->lock_block != NULL ? o->lock_block->start : 0xffffffffu);
	mix(o->lock_block != NULL ? o->lock_block->end : 0xffffffffu);
	mix(o->lock_block != NULL ? o->lock_block->len : 0xffffffffu);
}

static void step(void)
{
	owner_t *o = &owners[rnd() % OWNERS];
	u32 off, size, i;

	switch (rnd() % 16)
	{
	case 0: case 1: case 2: case 3: case 4:
		lock_one(o);
		break;
	case 5:	/* a texture thrown away */
		if (o->lock_block != NULL)
		{
			vramlock_unlock(o->lock_block);
			o->lock_block = NULL;
			mix(0x70000000u | o->id);
		}
		break;
	case 6: case 7:
		off = offset_any();
		mix(VramLockedWriteOffset(off) ? 1 : 2);
		break;
	case 8:
		off = offset_any();
		mix(VramLockedWrite(vram.data + off) ? 3 : 4);
		break;
	case 9:
		VramLockedWriteRange(offset_any(), size_any());
		break;
	case 10:
		VramBlockWrite(offset_any(), size_any());
		break;
	case 11:
		VramRendered(offset_any(), size_any(), (rnd() & 1) ? (const void *)o : NULL);
		break;
	case 12:
		vramlock_collect();
		break;
	case 13:
		mix(vramlock_pending() ? 5 : 6);
		break;
	case 14:	/* a save state, some of its pages not as video memory has them */
		if (rnd() % 4 == 0)
		{
			memcpy(state, vram.data, VRAM_SIZE);
			for (i = rnd() % 8; i-- > 0; )
				state[rnd() % VRAM_SIZE] ^= (u8)(1 + rnd() % 255);
			/* (the last page's and the one before's, the end of a run) */
			for (i = rnd() % 3; i-- > 0; )
				state[VRAM_SIZE - 1 - rnd() % (2 * PAGE_SIZE)] ^= (u8)(1 + rnd() % 255);
			vram_load(state);
		}
		break;
	default:
		FrameCount++;
		for (i = 0; i < 64; i++)
			vram.data[rnd() % VRAM_SIZE] = (u8)rnd();
		break;
	}
	mix(EPOCH());
}

/* What is left: the owners' locks, the pages protected, video memory */
static u32 left(void)
{
	u32 s = 2166136261u, i;
	for (i = 0; i < OWNERS; i++)
	{
		const owner_t *o = &owners[i];
		s = (s ^ o->dirty) * 16777619u;
		s = (s ^ (o->lock_block != NULL ? o->lock_block->start ^ (o->lock_block->end << 1) : 0x9e3779b9u)) * 16777619u;
		if (o->lock_block != NULL && o->lock_block->userdata != o)
		{
			printf("vram_lock_test: owner %u's lock not its\n", (unsigned)i);
			exit(1);
		}
	}
	for (i = 0; i < VRAM_SIZE / PAGE_SIZE; i++)
		s = (s ^ protect_bits[i]) * 16777619u;
	for (i = 0; i < VRAM_SIZE; i++)
		s = (s ^ vram.data[i]) * 16777619u;
	return s;
}

static void run(unsigned vram_size, u32 reserve, u32 seed)
{
	int r, n;
	u32 i;

	VRAM_SIZE = vram_size;
	rng = seed;
	h = 2166136261u;
	for (i = 0; i < VRAM_SIZE; i++)
		vram.data[i] = (u8)(i * 7 + (i >> 9));
	vram.size = VRAM_SIZE;
	vramlock_reserve(reserve);
	for (r = 0; r < ROUNDS; r++)
	{
		small_locks = small_first && reserve == 4 && r == 0;
		for (n = 0; n < STEPS; n++)
			step();
		printf("%u %u %u: %08x %08x %u\n", vram_size >> 20, (unsigned)reserve,
				(unsigned)r, (unsigned)h, (unsigned)left(), (unsigned)EPOCH());
	}
	/* Everything let go: no page is left with a lock, each written page
	 * taken in, and what is protected stays so */
	for (i = 0; i < OWNERS; i++)
		if (owners[i].lock_block != NULL)
		{
			vramlock_unlock(owners[i].lock_block);
			owners[i].lock_block = NULL;
		}
	vramlock_collect();
	printf("%u %u end: %08x %u\n", vram_size >> 20, (unsigned)reserve,
			(unsigned)left(), vramlock_pending() ? 1u : 0u);
}

/* Twice, a process each: the arenas' warning is said once in a process,
 * with locks run out first (1) and room for them on the pages (0) */
int main(int argc, char **argv)
{
	u32 i;
	small_first = argc > 1 && argv[1][0] == '1';
	printf("small locks first: %d\n", small_first ? 1 : 0);
	log_cb = test_log;
	vram.data = (u8 *)malloc(VRAM_SIZE_MAX);
	state = (u8 *)malloc(VRAM_SIZE_MAX);
	protect_bits = (u8 *)calloc(VRAM_SIZE_MAX / PAGE_SIZE, 1);
	if (vram.data == NULL || state == NULL || protect_bits == NULL)
		return 1;
	for (i = 0; i < OWNERS; i++)
		owners[i].id = i;
	/* (the arenas stay between runs, as they do between games) */
	run(8 << 20, 4, 1);
	run(16 << 20, 32, 2);
	run(8 << 20, 512, 3);
	free(protect_bits);
	free(state);
	free(vram.data);
	return 0;
}
