/*
	Tiny cute block manager. Doesn't keep block graphs or anything fancy ...
	Its based on a simple hashed-lists idea
*/

#include <stdlib.h>
#include "blockmanager.h"
#include "wait_site.h"
#include "ngen.h"

#include "../sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"


#if defined(__unix__) && defined(DYNA_OPROF)
#include <opagent.h>
op_agent_t          oprofHandle;
#endif

#if FEAT_SHREC != DYNAREC_NONE


/* The blocks there are, by where their code is: one table for the main
 * cache and one for the temporary cache, each in the order its blocks
 * were made, which is the order of their code, for each cache is filled
 * from its start to its end and only ever emptied whole. A block that
 * goes leaves its place (block NULL), until the cache is emptied. Each
 * holds its blocks. */
struct BlockMapEntry
{
	void *code;
	RuntimeBlockInfo *block;
};
struct BlockMap
{
	BlockMapEntry *entries;
	size_t count;
	size_t room;
};
/* [0]: the main cache's, [1]: the temporary cache's */
static BlockMap blkmap[2];

/* The temporary cache's blocks (blkmap has them as well), through their
 * own links: put in and taken out with nothing allocated */
static RuntimeBlockInfo *temp_blocks;

/* The blocks discarded lately, held until bm_CleanupDeletedBlocks() */
static RuntimeBlockInfo **del_blocks;
static size_t del_block_count;
static size_t del_block_room;

/* @room places in @*array of @size bytes each, where there are fewer
 * (false: no memory). Made when the recompiler starts, and grown in the
 * rare case it comes to more. */
static bool bm_grow(void **array, size_t *have, size_t room, size_t size)
{
	void *grown;
	if (*have >= room)
		return true;
	grown = realloc(*array, room * size);
	if (grown == NULL)
		return false;
	*array = grown;
	*have = room;
	return true;
}

static void del_blocks_push(RuntimeBlockInfo *block)
{
	if (del_block_count == del_block_room
			&& !bm_grow((void **)&del_blocks, &del_block_room, del_block_room * 2 + 1024, sizeof(*del_blocks)))
		die("out of memory");
	del_blocks[del_block_count++] = block;
}

/* The last place in @map whose code is at @code or before (-1: none) */
static ptrdiff_t blkmap_find(const BlockMap *map, const void *code)
{
	size_t lo = 0, hi = map->count;
	while (lo < hi)
	{
		const size_t mid = lo + (hi - lo) / 2;
		if ((const u8 *)map->entries[mid].code <= (const u8 *)code)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (ptrdiff_t)lo - 1;
}

/* (the hold its maker had is the table's now) */
static void blkmap_add(RuntimeBlockInfo *block)
{
	BlockMap *map = &blkmap[block->temp_block ? 1 : 0];
	if (map->count != 0 && (u8 *)map->entries[map->count - 1].code >= (u8 *)block->code)
	{
		INFO_LOG(DYNAREC, "DUP: %p %08X %p\n", map->entries[map->count - 1].code, block->addr, block->code);
		verify(false);
	}
	if (map->count == map->room
			&& !bm_grow((void **)&map->entries, &map->room, map->room * 2 + 1024, sizeof(*map->entries)))
		die("out of memory");
	map->entries[map->count].code = (void *)block->code;
	map->entries[map->count].block = block;
	map->count++;
}

/* The block taken out (it was there): its place left, and the table's
 * hold on it the caller's */
static void blkmap_remove(RuntimeBlockInfo *block)
{
	BlockMap *map = &blkmap[block->temp_block ? 1 : 0];
	const ptrdiff_t i = blkmap_find(map, (void *)block->code);
	verify(i >= 0 && map->entries[i].block == block);
	map->entries[i].block = NULL;
}

/* Every block there is to @f, in the order of their code (the two tables
 * merged), and the tables emptied: their holds @f's */
static void blkmap_take_all(void (*f)(RuntimeBlockInfo *block))
{
	size_t at[2] = { 0, 0 };
	for (;;)
	{
		int which;
		while (at[0] < blkmap[0].count && blkmap[0].entries[at[0]].block == NULL)
			at[0]++;
		while (at[1] < blkmap[1].count && blkmap[1].entries[at[1]].block == NULL)
			at[1]++;
		if (at[0] == blkmap[0].count && at[1] == blkmap[1].count)
			break;
		if (at[0] == blkmap[0].count)
			which = 1;
		else if (at[1] == blkmap[1].count)
			which = 0;
		else
			which = (u8 *)blkmap[1].entries[at[1]].code < (u8 *)blkmap[0].entries[at[0]].code ? 1 : 0;
		f(blkmap[which].entries[at[which]++].block);
	}
	blkmap[0].count = 0;
	blkmap[1].count = 0;
}

static bool unprotected_pages[RAM_SIZE_MAX/PAGE_SIZE];
/* The pages of main memory that are locked against writing now: those code
 * has been compiled from, and those it was compiled from once and that
 * nothing has written to since. */
static bool locked_pages[RAM_SIZE_MAX/PAGE_SIZE];
/* The read-only blocks whose code is in each page, through their links
 * (RuntimeBlockInfo::page_link): discarded when the page is written */
static BlockPageLink *blocks_per_page[RAM_SIZE_MAX/PAGE_SIZE];

static void temp_list_add(RuntimeBlockInfo *block)
{
	block->temp_prev = NULL;
	block->temp_next = temp_blocks;
	if (temp_blocks != NULL)
		temp_blocks->temp_prev = block;
	temp_blocks = block;
	block->temp_listed = true;
}

static void temp_list_remove(RuntimeBlockInfo *block)
{
	if (!block->temp_listed)
		return;
	if (block->temp_prev != NULL)
		block->temp_prev->temp_next = block->temp_next;
	else
		temp_blocks = block->temp_next;
	if (block->temp_next != NULL)
		block->temp_next->temp_prev = block->temp_prev;
	block->temp_prev = block->temp_next = NULL;
	block->temp_listed = false;
}

/* every list let go: the blocks' links too, so that a block discarded
 * after takes itself out of nothing */
static void lists_clear(void)
{
	size_t page;
	for (page = 0; page < sizeof(blocks_per_page) / sizeof(blocks_per_page[0]); page++)
	{
		while (blocks_per_page[page] != NULL)
		{
			BlockPageLink *link = blocks_per_page[page];
			blocks_per_page[page] = link->next;
			link->prev = link->next = NULL;
			link->block->page_links = 0;
		}
	}
	while (temp_blocks != NULL)
		temp_list_remove(temp_blocks);
}

// Stats
u32 protected_blocks;
u32 unprotected_blocks;

#define FPCA(x) ((DynarecCodeEntryPtr&)sh4rcb.fpcb[(x>>1)&FPCB_MASK])

// addr must be a physical address
// This returns an executable address
static DynarecCodeEntryPtr DYNACALL bm_GetCode(u32 addr)
{
	DynarecCodeEntryPtr rv = FPCA(addr);

	return rv;
}

/* With the MMU on a block goes straight on into the next only where the
 * next one's address is where it is whatever the TLB says (ngen.h). From
 * the others it is back to the main loop, which asks here for the code of
 * the address that is next - 200 to 250 thousand times a frame in Sega
 * Rally 2, when no block went straight on - and each time the address was
 * translated and the code for where it is looked up.
 *
 * So where the code for an address was found is kept: by the address and
 * the address space, so that going over to another address space and
 * back, which such a game does many times a frame, loses nothing. All of
 * it is forgotten whenever an address may have come to mean another page -
 * the MMU says when (bm_ForgetVaddrs()) - and whenever a block is made or
 * goes. */
struct VaddrCode
{
	u32 vaddr;
	u32 when;		// bm_vaddr_time, and the address space in the low 8 bits
	DynarecCodeEntryPtr code;
};
#define VADDRS_KEPT 4096
static VaddrCode vaddrs_kept[VADDRS_KEPT];
static u32 bm_vaddr_time = 256;

void bm_ForgetVaddrs()
{
	bm_vaddr_time += 256;
	if (bm_vaddr_time == 0)
	{
		// (once in sixteen million times)
		memset(vaddrs_kept, 0, sizeof(vaddrs_kept));
		bm_vaddr_time = 256;
	}
}

// addr must be a virtual address
// This returns an executable address
DynarecCodeEntryPtr DYNACALL bm_GetCodeByVAddr(u32 addr)
{
#ifndef NO_MMU
	if (!mmu_enabled())
#endif
		return bm_GetCode(addr);
#ifndef NO_MMU
	else
	{
		const u32 asid = CCN_PTEH.ASID;
		const u32 when = bm_vaddr_time | asid;
		VaddrCode& kept = vaddrs_kept[((addr >> 1) ^ (asid << 7)) & (VADDRS_KEPT - 1)];

		// (an odd address is never kept: see below)
		if (kept.vaddr == addr && kept.when == when)
			return kept.code;

		const u32 kept_addr = addr;

		if (addr & 1)
		{
			switch (addr)
			{
#ifdef USE_WINCE_HACK
			case 0xfffffde7: // GetTickCount
				// This should make this syscall faster
				r[0] = sh4_sched_now64() * 1000 / SH4_MAIN_CLOCK;
				next_pc = pr;
				break;

			case 0xfffffd05: // QueryPerformanceCounter(u64 *)
				{
					u32 paddr;
					if (mmu_data_translation<MMU_TT_DWRITE, u64>(r[4], paddr) == MMU_ERROR_NONE)
					{
						_vmem_WriteMem64(paddr, sh4_sched_now64() >> 4);
						r[0] = 1;
						next_pc = pr;
					}
					else
					{
						Do_Exception(addr, 0xE0, 0x100);
					}
				}
				break;
#endif

			default:
				Do_Exception(addr, 0xE0, 0x100);
				break;
			}
			addr = next_pc;
		}

		u32 paddr;
		u32 rv = mmu_instruction_translation(addr, paddr);
		if (rv != MMU_ERROR_NONE)
		{
			DoMMUException(addr, rv, MMU_TT_IREAD);
			mmu_instruction_translation(next_pc, paddr);
			return bm_GetCode(paddr);
		}

		const DynarecCodeEntryPtr code = bm_GetCode(paddr);

		/* Kept if there is code, the address is the one that was asked for
		 * (not where a system call or an exception went instead), and - the
		 * strict way with MMUCR.SV on - the translation does not hang on
		 * the mode the SH4 is in, which is not part of what it is kept by. */
		if (code != ngen_FailedToFindBlock && kept_addr == addr && bm_vaddr_time == (when & ~0xFFu)
				&& !(mmu_strict && CCN_MMUCR.SV))
		{
			kept.vaddr = addr;
			kept.when = when;
			kept.code = code;
		}
		return code;
	}
#endif
}

// addr must be a physical address
// This returns an executable address
RuntimeBlockInfoPtr DYNACALL bm_GetBlock(u32 addr)
{
	DynarecCodeEntryPtr cde = bm_GetCode(addr);  // Returns RX ptr

	if (cde == ngen_FailedToFindBlock)
		return NULL;
	else
		return bm_GetBlock2((void*)cde);  // Returns RX ptr
}

// This takes a RX address and returns the info block ptr (RW space)
RuntimeBlockInfoPtr bm_GetBlock2(void* dynarec_code)
{
	void *dynarecrw = CC_RX2RW(dynarec_code);
	int m;

	for (m = 0; m < 2; m++)
	{
		const ptrdiff_t i = blkmap_find(&blkmap[m], dynarecrw);
		RuntimeBlockInfo *block;
		if (i < 0)
			continue;
		// the block whose code starts there or before: it may not reach
		block = blkmap[m].entries[i].block;
		if (block == NULL || (u8*)block->code + block->host_code_size < (u8*)dynarecrw)
			continue;
		verify(block->contains_code((u8*)dynarecrw));
		return block;
	}
	return NULL;
}

/* Which of the blocks in del_blocks still have their code to themselves,
 * and can be relinked if one of them turns out to be the block that is
 * running (bm_ResetCache()):
 *  - where the blocks thrown out by the last bm_ResetCache() start, if
 *    they are still there. Theirs until something is compiled;
 *  - where they end. What comes after was discarded on its own since, and
 *    its place in the main cache is not given to anyone before the cache
 *    is emptied again;
 *  - where the list ended when the temporary cache was last started again
 *    from its beginning. A temporary block before that may have been
 *    written over. */
static size_t reset_batch = (size_t)-1;
static size_t reset_end;
static size_t temp_reset_end;

static void bm_CleanupDeletedBlocks()
{
	size_t i;
	for (i = 0; i < del_block_count; i++)
		bm_BlockUnref(del_blocks[i]);
	del_block_count = 0;
	reset_batch = (size_t)-1;
	reset_end = 0;
	temp_reset_end = 0;
}

// Takes RX pointer and returns a RW pointer
RuntimeBlockInfoPtr bm_GetStaleBlock(void* dynarec_code)
{
	void *dynarecrw = CC_RX2RW(dynarec_code);
	size_t i;
	// Start from the end to get the youngest one
	for (i = del_block_count; i-- > 0; )
		if (del_blocks[i]->contains_code((u8*)dynarecrw))
			return del_blocks[i];

	return NULL;
}

/* (the hold its maker had is the table's now) */
void bm_AddBlock(RuntimeBlockInfo* block)
{
	if (block->temp_block)
		temp_list_add(block);
	blkmap_add(block);

	verify((void*)bm_GetCode(block->addr) == (void*)ngen_FailedToFindBlock);
	FPCA(block->addr) = (DynarecCodeEntryPtr)CC_RW2RX(block->code);
	bm_ForgetVaddrs();

#ifdef DYNA_OPROF
	if (oprofHandle)
	{
		char fname[512];

		sprintf(fname,"sh4:%08X,c:%d,s:%d,h:%d", block->addr, block->guest_cycles, block->guest_opcodes, block->host_opcodes);

		if (op_write_native_code(oprofHandle, fname, (uint64_t)block->code, (void*)block->code, block->host_code_size) != 0)
		{
			INFO_LOG(DYNAREC, "op_write_native_code error\n");
		}
	}
#endif

}

void bm_DiscardBlock(RuntimeBlockInfo* block)
{
	RuntimeBlockInfo *const block_ptr = block;

	// Remove from block map (its hold, del_blocks' below)
	blkmap_remove(block);

	/* It jumps to its neighbours no longer, and they are told: a block
	 * that goes has those that are linked to it linked again (Discard(),
	 * below), and this one must not be among them when its neighbour goes.
	 * It was left there. Its code would then be patched - "unlinked" - long
	 * after it was gone: no harm where its code is still its own, in the
	 * main cache, which is only ever emptied whole; but the temporary cache
	 * is filled again from the start, and the patch landed in the code of
	 * whatever block was there by then. (And the neighbour kept it from
	 * being freed for as long as the neighbour lived.) */
	if (block_ptr->pNextBlock != NULL)
		bm_BlockRemRef(block_ptr->pNextBlock, block_ptr);
	if (block_ptr->pBranchBlock != NULL)
		bm_BlockRemRef(block_ptr->pBranchBlock, block_ptr);
	block_ptr->pNextBlock = NULL;
	block_ptr->pBranchBlock = NULL;
	ngen_Relink(block_ptr);

	// Remove from jump table
	verify((void*)bm_GetCode(block_ptr->addr) == CC_RW2RX((void*)block_ptr->code));
	FPCA(block_ptr->addr) = ngen_FailedToFindBlock;
	bm_ForgetVaddrs();

	if (block_ptr->temp_block)
		temp_list_remove(block_ptr);

	del_blocks_push(block_ptr);
	bm_BlockDiscard(block_ptr);
}

void bm_Periodical_1s()
{
	bm_CleanupDeletedBlocks();
}


void bm_vmem_pagefill(void** ptr, u32 size_bytes)
{
	for (size_t i = 0; i < size_bytes / sizeof(ptr[0]); i++)
	{
		ptr[i]=(void*)ngen_FailedToFindBlock;
	}
}

void bm_Reset(void)
{
	bm_CleanupDeletedBlocks();
	protected_blocks = 0;
	unprotected_blocks = 0;
	memset(locked_pages, 0, sizeof(locked_pages));

	if (_nvmem_enabled())
	{
		/* Windows cannot lock/unlock a region spanning more than one
		 * VirtualAlloc or MapViewOfFile, so each of main memory's mirrors
		 * is unlocked on its own: in P0, and in P1, P2 and P3 as well where
		 * the whole 4 GB is laid out (which it is on Windows too now). */
		static const u32 regions[] = { 0x0C000000, 0x8C000000, 0xAC000000, 0xCC000000 };
		const u32 count = _nvmem_4gb_space() ? 4 : 1;

		for (u32 i = 0; i < count; i++)
			for (u32 mirror = 0; mirror < 0x04000000; mirror += RAM_SIZE)
				mem_region_unlock(virt_ram_base + regions[i] + mirror, RAM_SIZE);
	}
	else
	{
		mem_region_unlock(&mem_b[0], RAM_SIZE);
	}
}

/* A page of main memory is to be written to or not, at every address the
 * host has it at. Main memory answers all over area 3 - at 0x0C000000 and
 * at 0x0D000000, 0x0E000000 and 0x0F000000 as well (twice over where there
 * are 32 megabytes of it) - and that in each of the SH4's regions, and
 * where the recompiled code reads and writes memory without asking, every
 * one of those is there for it. Only the first of each region was
 * protected ("TODO wraps"): code written over at another was not found to
 * have been, and what had been compiled from it went on being run. */
static void bm_ProtectPage(u32 addr, bool protect)
{
	// P0, P1, P2, P3
	static const u32 regions[] = { 0x0C000000, 0x8C000000, 0xAC000000, 0xCC000000 };
	u32 count, i, mirror;

	addr = addr & (RAM_MASK - PAGE_MASK);
	locked_pages[addr / PAGE_SIZE] = protect;
	if (!_nvmem_enabled())
	{
		if (protect)
			mem_region_lock(&mem_b[addr], PAGE_SIZE);
		else
			mem_region_unlock(&mem_b[addr], PAGE_SIZE);
		return;
	}
	count = _nvmem_4gb_space() ? ARRAY_SIZE(regions) : 1;
	for (i = 0; i < count; i++)
	{
		/* with the MMU on, P0 and P3 are what it says they are, and
		 * nothing reads or writes them as the host has them: recompiled
		 * code asks the table of pages or the MMU, and both go by where
		 * memory is in P1 */
		if ((i == 0 || i == 3) && mmu_enabled() && _nvmem_4gb_space())
			continue;
		for (mirror = 0; mirror < 0x04000000; mirror += RAM_SIZE)
		{
			u8 *page = virt_ram_base + regions[i] + mirror + addr;

			if (protect)
				mem_region_lock(page, PAGE_SIZE);
			else
				mem_region_unlock(page, PAGE_SIZE);
		}
	}
}

static void bm_LockPage(u32 addr)
{
	bm_ProtectPage(addr, true);
}

static void bm_UnlockPage(u32 addr)
{
	bm_ProtectPage(addr, false);
}

/* (a block of the tables, for bm_ResetCache(): its hold del_blocks') */
static void reset_one(RuntimeBlockInfo *block)
{
	block->relink_data = 0;
	block->pNextBlock = 0;
	block->pBranchBlock = 0;
	// needed for the transition to full mmu. Could perhaps limit it to the current block.
	ngen_Relink(block);
	// Avoid circular references
	bm_BlockDiscard(block);
	del_blocks_push(block);
}

void bm_ResetCache(bool nothing_since)
{
	bm_ForgetVaddrs();
	ngen_ResetBlocks();
	sh4_wait_sites_reset();
	_vmem_bm_reset();

	/* Emptied again with nothing compiled since the last time (@nothing_since):
	 * whatever block is still running is one of those thrown out then. It
	 * is not in blkmap to be relinked below, but nothing has been written
	 * over it, and it has to leave by the rules as they are now - back to
	 * the main loop, if the MMU has just come on - like any block that is
	 * running when this happens. Left as it was it went on by the rules of
	 * the first time: into a block compiled for the MMU, under a main loop
	 * made for a machine without one. (A program that turns translation
	 * on and loads its TLB after does both in one block: two of these.)
	 *
	 * The same goes for a block that was discarded on its own since the
	 * last time and may be running still: one that wrote to the page its
	 * own code is in - a variable next to it will do - and then did what
	 * brings this about. */
	const bool again = nothing_since && reset_batch <= del_block_count;

	for (size_t i = again ? reset_batch : reset_end; i < del_block_count; i++)
	{
		RuntimeBlockInfo *block = del_blocks[i];

		if (!again && block->temp_block && i < temp_reset_end)
			continue;
		block->relink_data = 0;
		block->pNextBlock = 0;
		block->pBranchBlock = 0;
		ngen_Relink(block);
	}
	if (!again)
		reset_batch = del_block_count;

	// blkmap includes temp blocks as well
	blkmap_take_all(reset_one);

	reset_end = del_block_count;

	lists_clear();

	// every page may be protected again, when next something is compiled from it
	memset(unprotected_pages, 0, sizeof(unprotected_pages));

#ifdef DYNA_OPROF
	if (oprofHandle)
	{
		for (size_t i=0;i<del_block_count;i++)
		{
			if (op_unload_native_code(oprofHandle, (uint64_t)del_blocks[i]->code) != 0)
			{
				INFO_LOG(DYNAREC, "op_unload_native_code error\n");
			}
		}
	}
#endif
}

/* The temporary cache is emptied: because it is full (@full false: its
 * blocks go, the others stay), or with everything else (@full true: after
 * bm_ResetCache(), which has dealt with every block there is).
 *
 * When only the temporary blocks go they go as any block does, one by
 * one: unlinked from the blocks they jump to and from the blocks that
 * jump to them. They used to be taken out of the tables and no more. A
 * block of the main cache that was linked to one went on jumping to where
 * it had been, which is where the next blocks to be compiled are put: it
 * ran another block's code, or the middle of one. That was of no
 * consequence while the temporary cache was only used with the MMU on,
 * when nothing is linked; since 355f8028 it is used without, for code
 * that a game keeps rewriting. */
void bm_ResetTempCache(bool full)
{
	/* (bm_DiscardBlock() takes each out of the list; with @full,
	 * bm_ResetCache() has let the list go already) */
	if (!full)
		while (temp_blocks != NULL)
			bm_DiscardBlock(temp_blocks);
	// (the temporary blocks there are now may be written over from here on)
	temp_reset_end = del_block_count;
}

static void bm_RanOut(size_t made)
{
	WARN_LOG(DYNAREC, "The recompiler's room: %u MB was not enough, more made\n", (unsigned)(made >> 20));
}

void bm_Init()
{
	/* (a block discarded is put here, from the signal handler too: its
	 * room made now, and kept when it is emptied) */
	if (!bm_grow((void **)&del_blocks, &del_block_room, CODE_SIZE / 1024, sizeof(*del_blocks))
			|| !bm_grow((void **)&blkmap[0].entries, &blkmap[0].room, CODE_SIZE / 1024, sizeof(*blkmap[0].entries))
			|| !bm_grow((void **)&blkmap[1].entries, &blkmap[1].room, TEMP_CODE_SIZE / 1024, sizeof(*blkmap[1].entries)))
		die("out of memory");
	/* the room the blocks and what they are kept in take, made now
	 * (node_pool.h): more is made if a game needs it, and that is said */
	node_pool_reserve(NODE_POOL_CHUNK);
	node_pool_set_ran_out(bm_RanOut);

#ifdef DYNA_OPROF
	oprofHandle=op_open_agent();
	if (oprofHandle==0)
		INFO_LOG(DYNAREC, "bm: Failed to open oprofile\n");
	else
		INFO_LOG(DYNAREC, "bm: Oprofile integration enabled !\n");
#endif
	bm_Reset();
}

/* Every block let go, and with it what it holds. A block that another
 * jumps to holds that one (pre_refs), so two that jump to each other hold
 * each other: dropped from the tables as they are, neither was ever freed.
 * The tables are the core's statics, and the blocks in them went when the
 * core was unloaded - unloaded by a frontend, with its memory lost for as
 * long as the frontend ran - or at the process's exit. Nothing runs
 * recompiled code now, so the code is not touched: the links are cut and
 * the blocks freed as their last holder lets go. */
static void release_refs(RuntimeBlockInfo *block)
{
	while (block->pre_ref_count != 0)
		bm_BlockUnref(block->pre_refs[--block->pre_ref_count]);
	block->pNextBlock = NULL;
	block->pBranchBlock = NULL;
}

static void release_one(RuntimeBlockInfo *block)
{
	bm_BlockUnref(block);
}

static void bm_ReleaseBlocks()
{
	size_t i;
	int m;
	for (m = 0; m < 2; m++)
		for (i = 0; i < blkmap[m].count; i++)
			if (blkmap[m].entries[i].block != NULL)
				release_refs(blkmap[m].entries[i].block);
	for (i = 0; i < del_block_count; i++)
		release_refs(del_blocks[i]);
	bm_ForgetVaddrs();
	lists_clear();
	blkmap_take_all(release_one);
}


void bm_Term()
{
#ifdef DYNA_OPROF
	if (oprofHandle) op_close_agent(oprofHandle);
	
	oprofHandle=0;
#endif
	bm_ReleaseBlocks();
	bm_Reset();
}

#if 0
void sh4_jitsym(FILE* out)
{
	for (auto& it : blkmap)
	{
		RuntimeBlockInfoPtr& block = it.second;
		fprintf(out, "%p %d %08X\n", block->code, block->host_code_size, block->addr);
	}
}

u32 GetLookup(RuntimeBlockInfo* elem)
{
	return elem->lookups;
}

bool UDgreater ( RuntimeBlockInfo* elem1, RuntimeBlockInfo* elem2 )
{
	return elem1->runs > elem2->runs;
}

bool UDgreater2 ( RuntimeBlockInfo* elem1, RuntimeBlockInfo* elem2 )
{
	return elem1->runs*elem1->host_opcodes > elem2->runs*elem2->host_opcodes;
}

bool UDgreater3 ( RuntimeBlockInfo* elem1, RuntimeBlockInfo* elem2 )
{
	return elem1->runs*elem1->host_opcodes/elem1->guest_cycles > elem2->runs*elem2->host_opcodes/elem2->guest_cycles;
}

void bm_PrintTopBlocks()
{
	double total_lups=0;
	double total_runs=0;
	double total_cycles=0;
	double total_hops=0;
	double total_sops=0;

	for (size_t i=0;i<all_blocks.size();i++)
	{
		total_lups+=GetLookup(all_blocks[i]);
		total_cycles+=all_blocks[i]->runs*all_blocks[i]->guest_cycles;
		total_hops+=all_blocks[i]->runs*all_blocks[i]->host_opcodes;
		total_sops+=all_blocks[i]->runs*all_blocks[i]->guest_opcodes;
		total_runs+=all_blocks[i]->runs;
	}

	INFO_LOG(DYNAREC, "Total lookups:  %.0fKRuns, %.0fKLuops, Total cycles: %.0fMhz, Total Hops: %.0fMips, Total Sops: %.0fMips!\n",total_runs/1000,total_lups/1000,total_cycles/1000/1000,total_hops/1000/1000,total_sops/1000/1000);
	total_hops/=100;
	total_cycles/=100;
	total_runs/=100;

	double sel_hops=0;
	for (size_t i=0;i<(all_blocks.size()/100);i++)
	{
		INFO_LOG(DYNAREC, "Block %08X: %p, r: %d (c: %d, s: %d, h: %d) (r: %.2f%%, c: %.2f%%, h: %.2f%%)\n",
			all_blocks[i]->addr, all_blocks[i]->code,all_blocks[i]->runs,
			all_blocks[i]->guest_cycles,all_blocks[i]->guest_opcodes,all_blocks[i]->host_opcodes,

			all_blocks[i]->runs/total_runs,
			all_blocks[i]->guest_cycles*all_blocks[i]->runs/total_cycles,
			all_blocks[i]->host_opcodes*all_blocks[i]->runs/total_hops);
		
		sel_hops+=all_blocks[i]->host_opcodes*all_blocks[i]->runs;
	}

	INFO_LOG(DYNAREC, " >-< %.2f%% covered in top 1%% blocks\n",sel_hops/total_hops);

	size_t i;
	for (i=all_blocks.size()/100;sel_hops/total_hops<50;i++)
	{
		INFO_LOG(DYNAREC, "Block %08X: %p, r: %d (c: %d, s: %d, h: %d) (r: %.2f%%, c: %.2f%%, h: %.2f%%)\n",
			all_blocks[i]->addr, all_blocks[i]->code,all_blocks[i]->runs,
			all_blocks[i]->guest_cycles,all_blocks[i]->guest_opcodes,all_blocks[i]->host_opcodes,

			all_blocks[i]->runs/total_runs,
			all_blocks[i]->guest_cycles*all_blocks[i]->runs/total_cycles,
			all_blocks[i]->host_opcodes*all_blocks[i]->runs/total_hops);
		
		sel_hops+=all_blocks[i]->host_opcodes*all_blocks[i]->runs;
	}

	INFO_LOG(DYNAREC, " >-< %.2f%% covered in top %.2f%% blocks\n",sel_hops/total_hops,i*100.0/all_blocks.size());

}

void bm_Sort()
{
	INFO_LOG(DYNAREC, "!!!!!!!!!!!!!!!!!!! BLK REPORT !!!!!!!!!!!!!!!!!!!!\n");

	INFO_LOG(DYNAREC, "     ---- Blocks: Sorted based on Runs ! ----     \n");
	std::sort(all_blocks.begin(),all_blocks.end(),UDgreater);
	bm_PrintTopBlocks();

	INFO_LOG(DYNAREC, "<><><><><><><><><><><><><><><><><><><><><><><><><>\n");

	INFO_LOG(DYNAREC, "     ---- Blocks: Sorted based on hops ! ----     \n");
	std::sort(all_blocks.begin(),all_blocks.end(),UDgreater2);
	bm_PrintTopBlocks();

	INFO_LOG(DYNAREC, "<><><><><><><><><><><><><><><><><><><><><><><><><>\n");

	INFO_LOG(DYNAREC, "     ---- Blocks: Sorted based on wefs ! ----     \n");
	std::sort(all_blocks.begin(),all_blocks.end(),UDgreater3);
	bm_PrintTopBlocks();

	INFO_LOG(DYNAREC, "^^^^^^^^^^^^^^^^^^^ END REPORT ^^^^^^^^^^^^^^^^^^^\n");

	for (size_t i=0;i<all_blocks.size();i++)
	{
		all_blocks[i]->runs=0;
	}
}
#endif

RuntimeBlockInfo *bm_NewBlock(void)
{
	void *room = node_pool_alloc(sizeof(RuntimeBlockInfo));
	RuntimeBlockInfo *block;
	if (room == NULL)
		die("out of memory");
	block = new (room) RuntimeBlockInfo();
	block->refs = 1;
	block->t_reg = -1;
	return block;
}

/* Room for @count operations in @block's list */
static void op_room(RuntimeBlockInfo *block, u32 count)
{
	shil_opcode *grown;
	u32 room;
	if (count <= block->op_room)
		return;
	room = block->op_room == 0 ? 64 : block->op_room * 2;
	while (room < count)
		room *= 2;
	grown = (shil_opcode *)node_pool_alloc(room * sizeof(*grown));
	if (grown == NULL)
		die("out of memory");
	if (block->op_count != 0)
		memcpy((void *)grown, block->oplist, block->op_count * sizeof(*grown));
	node_pool_free(block->oplist, block->op_room * sizeof(*grown));
	block->oplist = grown;
	block->op_room = room;
}

void bm_OpPush(RuntimeBlockInfo *block, const shil_opcode *op)
{
	op_room(block, block->op_count + 1);
	memcpy((void *)&block->oplist[block->op_count++], op, sizeof(*op));
}

void bm_OpInsert(RuntimeBlockInfo *block, u32 at, const shil_opcode *op)
{
	op_room(block, block->op_count + 1);
	memmove((void *)&block->oplist[at + 1], &block->oplist[at], (block->op_count - at) * sizeof(*op));
	memcpy((void *)&block->oplist[at], op, sizeof(*op));
	block->op_count++;
}

void bm_OpErase(RuntimeBlockInfo *block, u32 at)
{
	memmove((void *)&block->oplist[at], &block->oplist[at + 1], (block->op_count - at - 1) * sizeof(*block->oplist));
	block->op_count--;
}

void bm_BlockRef(RuntimeBlockInfo *block)
{
	block->refs++;
}

void bm_BlockUnref(RuntimeBlockInfo *block)
{
	verify(block->refs != 0);
	if (--block->refs != 0)
		return;
	if (block->sh4_code_size != 0)
	{
		if (block->read_only)
			protected_blocks--;
		else
			unprotected_blocks--;
	}
	while (block->pre_ref_count != 0)
		bm_BlockUnref(block->pre_refs[--block->pre_ref_count]);
	node_pool_free(block->pre_refs, block->pre_ref_room * sizeof(*block->pre_refs));
	node_pool_free(block->oplist, block->op_room * sizeof(*block->oplist));
	block->~RuntimeBlockInfo();
	node_pool_free(block, sizeof(RuntimeBlockInfo));
}

void bm_BlockAddRef(RuntimeBlockInfo *block, RuntimeBlockInfo *other)
{
	if (block->pre_ref_count == block->pre_ref_room)
	{
		const u32 room = block->pre_ref_room == 0 ? 4 : block->pre_ref_room * 2;
		RuntimeBlockInfo **grown = (RuntimeBlockInfo **)node_pool_alloc(room * sizeof(*grown));
		if (grown == NULL)
			die("out of memory");
		if (block->pre_ref_count != 0)
			memcpy(grown, block->pre_refs, block->pre_ref_count * sizeof(*grown));
		node_pool_free(block->pre_refs, block->pre_ref_room * sizeof(*block->pre_refs));
		block->pre_refs = grown;
		block->pre_ref_room = room;
	}
	bm_BlockRef(other);
	block->pre_refs[block->pre_ref_count++] = other;
}

void bm_BlockRemRef(RuntimeBlockInfo *block, RuntimeBlockInfo *other)
{
	u32 i;
	for (i = 0; i < block->pre_ref_count; i++)
		if (block->pre_refs[i] == other)
		{
			memmove(&block->pre_refs[i], &block->pre_refs[i + 1],
					(block->pre_ref_count - i - 1) * sizeof(*block->pre_refs));
			block->pre_ref_count--;
			bm_BlockUnref(other);
			return;
		}
}

void bm_BlockDiscard(RuntimeBlockInfo *block)
{
	u32 k, i;
	/* Update references */
	for (k = 0; k < block->pre_ref_count; k++)
	{
		RuntimeBlockInfo *ref = block->pre_refs[k];

		/* (By what it is linked to, not by the address it goes to: that
		 * need not be the address this was compiled for - the same code
		 * through another of memory's mirrors - and a link left standing
		 * goes to code that is no longer anybody's.) */
		if (ref->pNextBlock == block)
			ref->pNextBlock = NULL;
		if (ref->pBranchBlock == block)
			ref->pBranchBlock = NULL;
		ref->relink_data = 0;
		ngen_Relink(ref);
	}
	while (block->pre_ref_count != 0)
		bm_BlockUnref(block->pre_refs[--block->pre_ref_count]);

	/* Remove this block from the per-page block lists */
	for (i = 0; i < block->page_links; i++)
	{
		BlockPageLink *link = &block->page_link[i];
		if (link->prev != NULL)
			link->prev->next = link->next;
		else
			blocks_per_page[link->page] = link->next;
		if (link->next != NULL)
			link->next->prev = link->prev;
		link->prev = link->next = NULL;
	}
	block->page_links = 0;
}

void bm_BlockSetProtectedFlags(RuntimeBlockInfo *block)
{
	u32 addr;
#ifndef TARGET_NO_EXCEPTIONS
	/* Don't write protect rom and BIOS/IP.BIN (Grandia II) */
	if (!IsOnRam(block->addr) || (block->addr & 0x1FFF0000) == 0x0c000000)
#endif
	{
		block->read_only = false;
		unprotected_blocks++;
		return;
	}
	for (addr = block->addr & ~PAGE_MASK; addr < block->addr + block->sh4_code_size; addr += PAGE_SIZE)
	{
		if (unprotected_pages[(addr & RAM_MASK) / PAGE_SIZE])
		{
			block->read_only = false;
			unprotected_blocks++;
			return;
		}
	}
	block->read_only = true;
	protected_blocks++;
	verify(block->page_links == 0);
	for (addr = block->addr & ~PAGE_MASK; addr < block->addr + block->sh4_code_size; addr += PAGE_SIZE)
	{
		const u32 page = (addr & RAM_MASK) / PAGE_SIZE;
		BlockPageLink *link = &block->page_link[block->page_links++];
		verify(block->page_links <= BLOCK_PAGES);
		if (blocks_per_page[page] == NULL)
			bm_LockPage(addr);
		link->block = block;
		link->page = page;
		link->prev = NULL;
		link->next = blocks_per_page[page];
		if (link->next != NULL)
			link->next->prev = link;
		blocks_per_page[page] = link;
	}
}

void bm_RamWriteAccess(u32 addr)
{
	addr &= RAM_MASK;
	if (unprotected_pages[addr / PAGE_SIZE])
	{
		ERROR_LOG(DYNAREC, "Page %08x already unprotected\n", addr);
		die("Fatal error");
	}
	unprotected_pages[addr / PAGE_SIZE] = true;
	bm_UnlockPage(addr);
	/* (from the signal handler: nothing allocated. A block's Discard()
	 * takes it out of the page's list.) */
	while (blocks_per_page[addr / PAGE_SIZE] != NULL)
		bm_DiscardBlock(blocks_per_page[addr / PAGE_SIZE]->block);
}

/* A save state's main memory, @from, is put in place of the machine's.
 *
 * What has been compiled stays, where the code it was compiled from is
 * the same in the state: a page that is locked - one code was compiled
 * from - is held against the state's, and is left as it is, lock and all,
 * when they are the same. One that differs is written to as a game writes
 * to it, except that this is not taken for a page the game keeps writing
 * to: its blocks go, it is unlocked, and the next block compiled from it
 * locks it again. A block that checks its own code each time it is run -
 * one in a page that is written to all the time - needs nothing done.
 *
 * Every block used to be thrown away at every load, and the whole of
 * memory unlocked first so that it could be copied over: with a frontend
 * running frames ahead, which loads a state for every frame shown, all
 * that a frame runs was compiled again for every frame. */
void bm_LoadRam(const u8 *from)
{
	u8 * const ram = &mem_b[0];
	u32 run = 0;		// where the pages to be copied in one go start

	for (u32 addr = 0; addr < RAM_SIZE; addr += PAGE_SIZE)
	{
		if (!locked_pages[addr / PAGE_SIZE])
			continue;
		if (addr > run)
			memcpy(ram + run, from + run, addr - run);
		run = addr + PAGE_SIZE;
		if (memcmp(ram + addr, from + addr, PAGE_SIZE) == 0)
			continue;

		while (blocks_per_page[addr / PAGE_SIZE] != NULL)
			bm_DiscardBlock(blocks_per_page[addr / PAGE_SIZE]->block);
		bm_UnlockPage(addr);
		memcpy(ram + addr, from + addr, PAGE_SIZE);
	}
	if (RAM_SIZE > run)
		memcpy(ram + run, from + run, RAM_SIZE - run);
}

bool bm_RamWriteFault(void *p)
{
	if (_nvmem_enabled())
	{
		if (_nvmem_4gb_space())
		{
			if ((u8 *)p < virt_ram_base || (u8 *)p >= virt_ram_base + 0x100000000L)
				return false;
		}
		else
		{
			if ((u8 *)p < virt_ram_base || (u8 *)p >= virt_ram_base + 0x20000000)
				return false;
		}
		u32 addr = (u8*)p - virt_ram_base;
		if (mmu_enabled() && _nvmem_4gb_space() && ((addr & 0x80000000) == 0 || (addr >> 29) == 6))
			// (nothing is protected there with the MMU on: bm_ProtectPage())
			return false;
		if (!IsOnRam(addr) || ((addr >> 29) > 0 && (addr >> 29) < 4))	// system RAM is not mapped to 20, 40 and 60 because of laziness
			return false;
		bm_RamWriteAccess(addr);
	}
	else
	{
		uintptr_t offset = (uintptr_t)p - (uintptr_t)&mem_b[0];
#if defined(__aarch64__)
		/* Memory from malloc() can have a tag in its address's top byte
		 * (Android on ARM64), and the address of a fault comes without it. */
		offset = ((uintptr_t)p & 0x00FFFFFFFFFFFFFFull) - ((uintptr_t)&mem_b[0] & 0x00FFFFFFFFFFFFFFull);
#endif
		if (offset >= RAM_SIZE)
			return false;
		bm_RamWriteAccess((u32)offset);
	}

	return true;
}

bool print_stats;
#if 0
void fprint_hex(FILE* d,const char* init,u8* ptr, u32& ofs, u32 limit)
{
	int base=ofs;
	int cnt=0;
	while(ofs<limit)
	{
		if (cnt==32)
		{
			fputs("\n",d);
			cnt=0;
		}

		if (cnt==0)
			fprintf(d,"%s:%d:",init,ofs-base);

		fprintf(d," %02X",ptr[ofs++]);
		cnt++;
	}
	fputs("\n",d);
}



void print_blocks()
{
	FILE* f=0;

	if (print_stats)
	{
		f=fopen(get_writable_data_path("/blkmap.lst").c_str(),"w");
		print_stats=false;

		INFO_LOG(DYNAREC, "Writing blocks to %p\n", f);
	}

	for (auto it : blkmap)
	{
		RuntimeBlockInfoPtr blk = it.second;
		if (f)
		{
			fprintf(f,"block: %p\n",blk.get());
			fprintf(f,"vaddr: %08X\n",blk->vaddr);
			fprintf(f,"paddr: %08X\n",blk->addr);
			fprintf(f,"hash: %s\n",bm_BlockHash(blk));
			fprintf(f,"hash_rloc: %s\n",bm_BlockHash(blk));
			fprintf(f,"code: %p\n",blk->code);
			fprintf(f,"runs: %d\n",blk->runs);
			fprintf(f,"BlockType: %d\n",blk->BlockType);
			fprintf(f,"NextBlock: %08X\n",blk->NextBlock);
			fprintf(f,"BranchBlock: %08X\n",blk->BranchBlock);
			fprintf(f,"pNextBlock: %p\n",blk->pNextBlock);
			fprintf(f,"pBranchBlock: %p\n",blk->pBranchBlock);
			fprintf(f,"guest_cycles: %d\n",blk->guest_cycles);
			fprintf(f,"guest_opcodes: %d\n",blk->guest_opcodes);
			fprintf(f,"host_opcodes: %d\n",blk->host_opcodes);
			fprintf(f,"il_opcodes: %zd\n",blk->op_count);

			u32 hcode=0;
			s32 gcode=-1;
			u8* pucode=(u8*)blk->code;

			size_t j=0;
			
			fprintf(f,"{\n");
			for (;j<blk->op_count;j++)
			{
				shil_opcode* op = &blk->oplist[j];
				//fprint_hex(f,"//h:",pucode,hcode,op->host_offs);

				if (gcode!=op->guest_offs)
				{
					gcode=op->guest_offs;
					u32 rpc=blk->vaddr+gcode;
#ifndef NO_MMU
					sh4_catch c;
					SH4_TRY(c)
					{
#endif
						u16 op=IReadMem16(rpc);
#ifndef NO_MMU
						sh4_catch_pop(&c);
#endif
						char temp[128];
						OpDesc[op]->Disassemble(temp,rpc,op);

						fprintf(f,"//g: %04X %s\n", op, temp);
#ifndef NO_MMU
					}
					else
						fprintf(f,"//g: ???? (page fault)\n");
#endif
				}

            std::string s=op->dissasm();
				fprintf(f,"//il:%d:%d:%s\n",op->guest_offs,op->host_offs,s.c_str());
			}
			
			//fprint_hex(f,"//h:",pucode,hcode,blk->host_code_size);

			fprintf(f,"}\n");
		}

		blk->runs=0;
	}

	if (f) fclose(f);
}
#endif
#endif

