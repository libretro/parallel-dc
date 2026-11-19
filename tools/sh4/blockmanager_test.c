/* The block manager (core/hw/sh4/dyna/blockmanager.c) driven by itself:
 * blocks made, added, linked to each other, run into, discarded, written
 * over, protected and unprotected, the caches emptied, states loaded -
 * at random - and what it does hashed: the table of entries, the pages
 * locked, the relinks asked for, the blocks found, held and let go,
 * against what the C++ it replaced did (sh tools/sh4/blockmanager_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/mem/_vmem.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/dyna_state.h"
#include "hw/sh4/dyna/ngen.h"
#include "node_pool.h"

/* What the block manager calls of the rest of the core */
Sh4RCB *p_sh4rcb;
RegisterStruct CCN[SH4_CCN_REGS];
settings_t settings;
bool mmu_strict;
unsigned RAM_SIZE = 16 * 1024 * 1024;
unsigned RAM_MASK = 16 * 1024 * 1024 - 1;
VArray2 mem_b;
u8 *virt_ram_base;
bool vmem_4gb_space;
const u32 fast_reg_lut[8] = { 0, 0, 0, 0, 1, 1, 0, 0 };
retro_log_printf_t log_cb;

static u32 rng = 1;
static unsigned h = 2166136261u;
static u32 slice;

static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* FNV-1a */
static void mix(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		h ^= (v >> (i * 8)) & 0xFF;
		h *= 16777619u;
	}
}

static void failed_to_find(void) { }
void (*ngen_FailedToFindBlock)(void) = failed_to_find;

/* code at fake addresses, no further than the start of the caches: the
 * main cache's then the temporary one's, each filled in order */
#define MAIN_BASE ((uintptr_t)0x10000000u)
#define TEMP_BASE ((uintptr_t)0x20000000u)
static uintptr_t main_next = MAIN_BASE, temp_next = TEMP_BASE;

/* the block's address as it is known here: where its code is */
static u32 code_of(const RuntimeBlockInfo *b)
{
	return b == NULL ? 0xFFFFFFFFu : (u32)(uintptr_t)b->code;
}

/* an entry of the table: a block's code, or "not compiled" (whose
 * address is the binary's) */
static u32 entry_of(DynarecCodeEntryPtr p)
{
	return p == ngen_FailedToFindBlock ? 0xFFFFFFFEu : (u32)(uintptr_t)p;
}

u32 ngen_Relink(RuntimeBlockInfo *block)
{
	mix(0x52454C4Bu);
	mix(code_of(block));
	/* (what a recompiler keeps of its links: set again here, and gone
	 * where the block manager says so) */
	mix(block->relink_data);
	block->relink_data = 1 + (code_of(block->pNextBlock) & 0xFF);
	mix(code_of(block->pNextBlock));
	mix(code_of(block->pBranchBlock));
	return 0;
}
void ngen_ResetBlocks(void) { mix(0x52455342u); }
void sh4_wait_sites_reset(void) { mix(0x57414954u); }
/* The table of entries, emptied as _vmem.c does: here only where there
 * were entries since the last time (the whole of it is 16 million) */
static u32 *entries_set;
static size_t entries_set_count, entries_set_room;
static void note_entry(u32 addr)
{
	if (entries_set_count == entries_set_room)
	{
		entries_set_room = entries_set_room * 2 + 1024;
		entries_set = (u32 *)realloc(entries_set, entries_set_room * sizeof(*entries_set));
		if (entries_set == NULL)
			exit(2);
	}
	entries_set[entries_set_count++] = addr;
}
void _vmem_bm_reset(void)
{
	size_t i;
	mix(0x564D454Du);
	for (i = 0; i < entries_set_count; i++)
		p_sh4rcb->fpcb[(entries_set[i] >> 1) & FPCB_MASK] = (void *)(uintptr_t)ngen_FailedToFindBlock;
	entries_set_count = 0;
}
bool mem_region_lock(void *start, size_t len)
{
	mix(1);
	mix((u32)((uintptr_t)start - (virt_ram_base ? (uintptr_t)virt_ram_base : (uintptr_t)mem_b.data)));
	mix((u32)len);
	return true;
}
bool mem_region_unlock(void *start, size_t len)
{
	mix(2);
	mix((u32)((uintptr_t)start - (virt_ram_base ? (uintptr_t)virt_ram_base : (uintptr_t)mem_b.data)));
	mix((u32)len);
	return true;
}
u64 sh4_sched_now64(void) { return slice; }
bool Do_Exception(u32 epc, u32 expEvn, u32 CallVect) { mix(epc); mix(expEvn); mix(CallVect); return true; }
void DoMMUException(u32 addr, u32 mmu_error, u32 access_type) { mix(addr); mix(mmu_error); mix(access_type); }
u32 mmu_full_lookup_c(u32 va, const TLB_Entry **entry, u32 *rv)
{
	*entry = NULL;
	*rv = va & 0x1FFFFFFF;
	return (va >> 12) % 8 == 7 ? MMU_ERROR_TLB_MISS : MMU_ERROR_NONE;
}
u32 mmu_data_translation_w64(u32 va, u32 *rv) { *rv = va; return MMU_ERROR_NONE; }
void DYNACALL _vmem_WriteMem64(u32 addr, u64 data) { mix(addr); mix((u32)data); }
void os_DebugBreak(void) { printf("blockmanager_test: a check in the block manager failed\n"); exit(1); }

/* the blocks there are: each held once more here, as the recompilers' code
 * would be running them */
#define MAX_BLOCKS 512
static RuntimeBlockInfo *live[MAX_BLOCKS];
static u32 live_count;
/* the addresses of the blocks made lately, gone or not */
static u32 seen[64];
static u32 seen_at;

static RuntimeBlockInfo *make_block(void)
{
	RuntimeBlockInfo *b = bm_NewBlock();
	const u32 pick = rnd();
	u32 n, i;
	fpscr_t cfg;
	shil_opcode op;

	mix((u32)b->t_reg);
	mix(b->refs);

	cfg.full = 0;
	/* (addresses in main memory and its mirrors, in the BIOS, now and
	 * then where another block is) */
	b->addr = pick % 16 == 0 ? (rnd() & 0x1FFFE) : (pick & 0xF00000) | 0x8C000000 | (rnd() & 0xFFFE) | ((rnd() & 3) << 16);
	if (live_count != 0 && pick % 13 == 0)
		b->addr = live[rnd() % live_count]->addr + 2;
	b->vaddr = b->addr;
	b->fpu_cfg = cfg;
	b->sh4_code_size = 2 + 2 * (rnd() % 600);
	b->temp_block = (pick >> 8) % 5 == 0;
	b->host_code_size = 16 + 4 * (rnd() % 256);
	if (b->temp_block)
	{
		b->code = (DynarecCodeEntryPtr)temp_next;
		temp_next += b->host_code_size + 4 * (rnd() % 4);
	}
	else
	{
		b->code = (DynarecCodeEntryPtr)main_next;
		main_next += b->host_code_size + 4 * (rnd() % 4);
	}
	b->BranchBlock = 0xFFFFFFFF;
	b->NextBlock = 0xFFFFFFFF;
	/* operations pushed, put in, taken out */
	memset(&op, 0, sizeof(op));
	n = rnd() % 70;
	for (i = 0; i < n; i++)
	{
		op.guest_offs = (u16)i;
		op.op = (shilop)(rnd() % 8);
		if (b->op_count != 0 && rnd() % 4 == 0)
			bm_OpInsert(b, rnd() % b->op_count, &op);
		else
			bm_OpPush(b, &op);
		if (b->op_count != 0 && rnd() % 6 == 0)
			bm_OpErase(b, rnd() % b->op_count);
	}
	mix(b->op_count);
	for (i = 0; i < b->op_count; i++)
		mix(b->oplist[i].guest_offs | (b->oplist[i].op << 16));
	return b;
}

static void forget(RuntimeBlockInfo *b)
{
	u32 i;
	for (i = 0; i < live_count; i++)
		if (live[i] == b)
		{
			live[i] = live[--live_count];
			bm_BlockUnref(b);
			return;
		}
}

static void add_block(void)
{
	RuntimeBlockInfo *b;
	if (live_count == MAX_BLOCKS)
		return;
	b = make_block();
	/* (one at an address that has a block is not added: the main loop
	 * looks first) */
	if (bm_GetBlock(b->addr) != NULL)
	{
		mix(0x444F5542u);
		bm_BlockUnref(b);
		return;
	}
	bm_BlockSetProtectedFlags(b);
	mix(b->read_only);
	mix(b->page_links);
	bm_BlockRef(b);
	live[live_count++] = b;
	note_entry(b->addr);
	bm_AddBlock(b);
	seen[seen_at++ % 64] = b->vaddr;
}

/* whether @b is in the tables still */
static bool in_tables(RuntimeBlockInfo *b)
{
	return bm_GetBlock2((void *)(uintptr_t)b->code) == b;
}

/* @from jumps to @to, as a recompiler links them (both in the tables) */
static void link_blocks(void)
{
	RuntimeBlockInfo *from, *to;
	if (live_count < 2)
		return;
	from = live[rnd() % live_count];
	to = live[rnd() % live_count];
	if (!in_tables(from) || !in_tables(to))
		return;
	if (rnd() & 1)
	{
		if (from->pNextBlock != NULL)
			return;
		from->pNextBlock = to;
	}
	else
	{
		if (from->pBranchBlock != NULL)
			return;
		from->pBranchBlock = to;
	}
	bm_BlockAddRef(to, from);
	mix(to->pre_ref_count);
}

static void unlink_blocks(void)
{
	RuntimeBlockInfo *from;
	if (live_count == 0)
		return;
	from = live[rnd() % live_count];
	if (from->pNextBlock != NULL)
	{
		bm_BlockRemRef(from->pNextBlock, from);
		from->pNextBlock = NULL;
	}
	if (from->pBranchBlock != NULL)
	{
		bm_BlockRemRef(from->pBranchBlock, from);
		from->pBranchBlock = NULL;
	}
}

/* what the tables say of a host address: the block it is in, now or
 * among those discarded */
static void look_up(void)
{
	uintptr_t at;
	RuntimeBlockInfo *b;
	int i;
	switch (rnd() % 3)
	{
	case 0:
		at = MAIN_BASE + rnd() % (u32)(main_next - MAIN_BASE + 64);
		break;
	case 1:
		at = TEMP_BASE + rnd() % (u32)(temp_next - TEMP_BASE + 64);
		break;
	default:
		if (live_count == 0)
			return;
		b = live[rnd() % live_count];
		at = (uintptr_t)b->code + rnd() % (b->host_code_size + 8);
		break;
	}
	mix(0x4C4F4F4Bu);
	mix(code_of(bm_GetBlock2((void *)at)));
	mix(code_of(bm_GetStaleBlock((void *)at)));
	for (i = 0; i < 4; i++)
	{
		const u32 vaddr = seen[rnd() % 64];
		mix(code_of(bm_GetBlock(vaddr)));
		mix(entry_of(bm_GetCodeByVAddr(vaddr + (rnd() % 8 == 0 ? 1 : 0))));
	}
}

/* the state of every block still held here, and of the table of entries
 * where they are */
static void mix_live(void)
{
	u32 i;
	mix(live_count);
	for (i = 0; i < live_count; i++)
	{
		const RuntimeBlockInfo *b = live[i];
		mix(code_of(b));
		mix(b->refs);
		mix(b->pre_ref_count);
		mix(b->read_only);
		mix(b->page_links);
		mix(b->relink_data);
		mix(code_of(b->pNextBlock));
		mix(code_of(b->pBranchBlock));
		mix(entry_of((DynarecCodeEntryPtr)(uintptr_t)p_sh4rcb->fpcb[(b->addr >> 1) & FPCB_MASK]));
	}
}

static void step(void)
{
	const u32 r = rnd() % 100;
	RuntimeBlockInfo *b;
	u32 i, addr;

	if (r < 30)
		add_block();
	else if (r < 42)
		link_blocks();
	else if (r < 47)
		unlink_blocks();
	else if (r < 62)
		look_up();
	else if (r < 70)
	{
		/* a block discarded by the recompiler (one that checks its code) */
		if (live_count == 0)
			return;
		b = live[rnd() % live_count];
		if (in_tables(b))
			bm_DiscardBlock(b);
		forget(b);
	}
	else if (r < 78)
	{
		/* a write to main memory that faulted: in a page locked (one with
		 * a read-only block's code), by its address or the host's, at one
		 * of the mirrors - or somewhere that is not main memory */
		mix(0x57524954u);
		addr = 0xFFFFFFFF;
		for (i = rnd() % (live_count + 1); i < live_count; i++)
			if (live[i]->read_only && live[i]->page_links != 0)
			{
				addr = (live[i]->addr & RAM_MASK & ~PAGE_MASK) + (rnd() & 0x3F);
				break;
			}
		if (rnd() % 8 == 0)
		{
			/* (not main memory: left alone) */
			mix(bm_RamWriteFault(mem_b.data + RAM_SIZE + (rnd() % 4 == 0 ? 0 : rnd() & 0xFFFF)));
			if (virt_ram_base != NULL)
				mix(bm_RamWriteFault(virt_ram_base + 0x04000000 + (rnd() & 0xFFFF)));
		}
		else if (addr == 0xFFFFFFFF)
			;
		else if (virt_ram_base != NULL && rnd() % 2)
		{
			static const u32 regions[] = { 0x0C000000, 0x8C000000, 0xAC000000, 0xCC000000 };
			const u32 at = vmem_4gb_space ? regions[rnd() % 4] : 0x0C000000;
			mix(bm_RamWriteFault(virt_ram_base + at + ((rnd() & 1) << 24) + addr));
		}
		else if (virt_ram_base == NULL && rnd() % 2)
			mix(bm_RamWriteFault(mem_b.data + addr));
		else
			bm_RamWriteAccess(addr);
	}
	else if (r < 81)
	{
		/* a state loaded, now and then: some pages as they are, some not
		 * (the state is memory as it is after, kept so) */
		static u8 state[16 * 1024 * 1024];
		if (rnd() % 16 != 0)
			return;
		for (i = rnd() % 6; i != 0; i--)
			state[rnd() & RAM_MASK] ^= 0x5A;
		bm_LoadRam(state);
		mix(memcmp(state, mem_b.data, RAM_SIZE) == 0);
	}
	else if (r < 82)
	{
		bm_ResetCache((rnd() & 3) == 0);
		bm_ResetTempCache(true);
		temp_next = TEMP_BASE;
		main_next = MAIN_BASE;
	}
	else if (r < 87)
	{
		bm_ResetTempCache(false);
		temp_next = TEMP_BASE;
	}
	else if (r < 92)
		bm_Periodical_1s();
	else if (r < 94)
	{
		/* the MMU turned on or off, the address space changed */
		settings.dreamcast.FullMMU = !settings.dreamcast.FullMMU;
		CCN_MMUCR.AT = settings.dreamcast.FullMMU;
		CCN_PTEH.ASID = rnd() & 0xFF;
		bm_ForgetVaddrs();
	}
	else if (r < 95)
	{
		bm_Reset();
		mix(0x52535432u);
	}
	else if (live_count != 0)
		/* let go here, as the code running it moves on */
		forget(live[rnd() % live_count]);

	/* the blocks gone from the tables let go of here, but now and then
	 * one, as the code that was running it is still */
	for (i = live_count; i-- > 0; )
		if (!in_tables(live[i]) && rnd() % 4 != 0)
			forget(live[i]);
}

/* The temporary cache started again: a block compiled into it after is
 * found by its code */
static int temp_cache_restarted(void)
{
	RuntimeBlockInfo *b;
	int i, found;
	fpscr_t cfg;
	cfg.full = 0;
	for (i = 0; i < 4; i++)
	{
		b = bm_NewBlock();
		b->addr = b->vaddr = 0x8C010000 + 0x100 * i;
		b->fpu_cfg = cfg;
		b->temp_block = true;
		b->host_code_size = 64;
		b->code = (DynarecCodeEntryPtr)(TEMP_BASE + 0x1000 + 0x100 * i);
		bm_AddBlock(b);
	}
	bm_ResetTempCache(false);
	b = bm_NewBlock();
	b->addr = b->vaddr = 0x8C020000;
	b->fpu_cfg = cfg;
	b->temp_block = true;
	b->host_code_size = 64;
	b->code = (DynarecCodeEntryPtr)TEMP_BASE;
	bm_AddBlock(b);
	found = bm_GetBlock2((void *)(TEMP_BASE + 8)) == b;
	if (!found)
		printf("blockmanager_test: a block compiled after the temporary cache was started again is not found\n");
	bm_ResetCache(false);
	bm_ResetTempCache(true);
	bm_Periodical_1s();
	return found;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 200000, t, mode;

	p_sh4rcb = (Sh4RCB *)calloc(1, sizeof(Sh4RCB));
	mem_b.data = (u8 *)calloc(1, RAM_SIZE);
	mem_b.size = RAM_SIZE;
	if (p_sh4rcb == NULL || mem_b.data == NULL)
		return 2;
	for (mode = 0; mode < 3; mode++)
	{
		/* main memory locked as itself; within the 512 MB of the address
		 * space laid out; within the 4 GB of it */
		/* (an address space that is not there: only its addresses are
		 * used) */
		virt_ram_base = mode == 0 || sizeof(void *) < 8 ? NULL : (u8 *)((uintptr_t)0x4000000 << 20);
		vmem_4gb_space = mode == 2;
		bm_vmem_pagefill(p_sh4rcb->fpcb, sizeof(p_sh4rcb->fpcb));
		bm_Init();
		if (!temp_cache_restarted())
			return 1;
		for (t = 0; t < n; t++)
		{
			step();
			if (t % 1000 == 999)
				mix_live();
		}
		mix_live();
		while (live_count != 0)
			forget(live[live_count - 1]);
		bm_Term();
		main_next = MAIN_BASE;
		temp_next = TEMP_BASE;
		printf("%d steps, mode %d, %08x\n", n, mode, h);
	}
	free(entries_set);
	free(mem_b.data);
	free(p_sh4rcb);
	return 0;
}
