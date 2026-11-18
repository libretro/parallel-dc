/*
	In case you wonder, the extern "C" stuff are for the assembly code on beagleboard/pandora
*/
#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DynarecCodeEntryPtr)(void);

#ifdef __GNUC__
__attribute__((used))
#endif
DynarecCodeEntryPtr DYNACALL bm_GetCodeByVAddr(u32 addr);
/* What bm_GetCodeByVAddr() keeps of where it found the code for an address
 * with the MMU on is to go: an address may be another page now, or a
 * block is gone. */
#if FEAT_SHREC != DYNAREC_NONE
void bm_ForgetVaddrs(void);
#else
static TYPES_INLINE void bm_ForgetVaddrs(void) {}
#endif

/* The recompiler's once a second: its statistics, and code that keeps
 * being rewritten */
void bm_Periodical_1s(void);

/* The table of blocks' entries, each to the "not compiled yet" stub */
void bm_vmem_pagefill(void **ptr, u32 size_bytes);

#define CODE_SIZE   (16*1024*1024)

/* For the blocks whose code keeps being rewritten (smc_hotspots). A build
 * with NO_MMU has them as well - it used to have no room here at all, and
 * would have been given a block to write into it all the same. */
#define TEMP_CODE_SIZE (1024*1024)

/* The room there has to be in a code cache before a block is compiled into
 * it: more than any one block comes to. A block is up to 511 operations,
 * and with the MMU on a load or store can be over a hundred bytes of code.
 * The largest seen here is 10 KB (Sega Rally 2); it was 16 KB that was
 * asked for, and upstream went to 32 KB after Tomb Raider: The Last
 * Revelation, another Windows CE game, ran past it. 64 KB of 16 MB is
 * nothing to give for not having to wonder. */
#define CODE_MARGIN (64*1024)

extern u8* CodeCache;

/* When NO_RWX is enabled there's two address-spaces, one executable and
 * one writtable. The emitter and most of the code in rec-* will work with
 * the RW pointer. However the fpcb table and other pointers during execution
 * (ie. exceptions) are RX pointers. These two macros convert between them by
 * sub/add the pointer offset. CodeCache will point to the RW pointer for simplicity. */
#ifdef FEAT_NO_RWX_PAGES
	extern uintptr_t cc_rx_offset;
	#define CC_RW2RX(ptr) (void*)(((uintptr_t)(ptr)) + cc_rx_offset)
	#define CC_RX2RW(ptr) (void*)(((uintptr_t)(ptr)) - cc_rx_offset)
#else
	#define CC_RW2RX(ptr) (ptr)
	#define CC_RX2RW(ptr) (ptr)
#endif

/* A write to main memory that faulted at @p, the host's address: the page
 * it is in had code compiled from it, which is thrown away and the page
 * opened (false: not main memory) */
bool bm_RamWriteFault(void *p);

/* The recompiler's (rec-*): an access to the SH4's memory at *@host_pc in
 * compiled code that faulted, written over with a call that does it the
 * slow way; *@host_pc is where to go on from (false: not one of its) */
bool ngen_Rewrite(unat *host_pc);
/* The 32-bit ARM recompiler's: the same, with the host's registers */
u32* ngen_readm_fail_v2(u32* ptr, u32* regs, u32 saddr);

#ifdef __cplusplus
}

#include "decoder.h"
#include "dyna_state.h"
#include "node_pool.h"

/* A block, as it is handed about: by its address. It is the block
 * manager's for as long as it is in its tables, or among the blocks
 * discarded lately (a second at most), and those that jump to it hold
 * it as well (pre_refs): it goes when the last lets go (bm_BlockUnref()). */
typedef RuntimeBlockInfo *RuntimeBlockInfoPtr;

struct RuntimeBlockInfo_Core
{
	u32 addr;
	DynarecCodeEntryPtr code;
	u32 lookups;
};

struct RuntimeBlockInfo;

/* A block's place in the list of a page of main memory its code is in
 * (blockmanager.cpp: the page's blocks, discarded when it is written) */
struct BlockPageLink
{
	RuntimeBlockInfo *block;
	BlockPageLink *prev, *next;
	u32 page;
};
/* the pages a block's code can be in: it is at most BLOCK_MAX_SH_OPS_HARD
 * instructions, a fraction of a page, so two; four with room */
#define BLOCK_PAGES 4

struct RuntimeBlockInfo: RuntimeBlockInfo_Core
{
	bool Setup(u32 pc,fpscr_t fpu_cfg);
	/* who holds it (see RuntimeBlockInfoPtr) */
	u32 refs;
	const char* hash();

	u32 vaddr;

	u32 host_code_size;	   /* in bytes */
	u32 sh4_code_size;      /* in bytes */

	u32 runs;
	s32 staging_runs;

	fpscr_t fpu_cfg;
	u32 guest_cycles;
	u32 guest_opcodes;
	u32 host_opcodes;
	bool has_fpu_op;
	u32 blockcheck_failures;
	bool temp_block;
	// with the MMU on: may go straight on into another block (rdv_MmuMayGoOn())
	bool mmu_go_on;

	u32 BranchBlock; /* if not 0xFFFFFFFF then jump target */
	u32 NextBlock;   /* if not 0xFFFFFFFF then next block (by position) */

	/* 0 if not available */
	RuntimeBlockInfo* pBranchBlock;
	RuntimeBlockInfo* pNextBlock; 

	u32 relink_offset;
	u32 relink_data;
	u32 csc_RetCache; /* only for stats for now */

	BlockEndType BlockType;
	bool has_jcond;

	/* (it, its lists and the lists blocks are kept in take their room from
	 * the recompiler's pool, node_pool.h: compiling a block allocates
	 * nothing once the pool is large enough) */
	/* Its operations: op_count of them, in room for op_room, in the
	 * recompiler's pool (bm_OpPush(), bm_OpInsert(), bm_OpErase()) */
	shil_opcode *oplist;
	u32 op_count;
	u32 op_room;

	bool contains_code(const u8* ptr)
	{
		return ((size_t)(ptr-(u8*)code)) < host_code_size;
	}

	/* The recompiler's: its links made again, for pNextBlock and
	 * pBranchBlock as they are now (none: back to the main loop); the
	 * bytes of code that come to */
	u32 Relink();

	/* The blocks that jump to this one, each held (see RuntimeBlockInfoPtr):
	 * told when it goes, to jump elsewhere. In the recompiler's pool. */
	RuntimeBlockInfo **pre_refs;
	u32 pre_ref_count;
	u32 pre_ref_room;

	void AddRef(RuntimeBlockInfo *other);
	void RemRef(RuntimeBlockInfo *other);

	void Discard();
	void SetProtectedFlags();

	bool read_only;

	/* What the recompiler keeps of the block. x86-64 and x86: where in
	 * its code the link sites are, 0 where there is none - the way out to
	 * BranchBlock, and the one to NextBlock - and where a block linked to
	 * this one comes in (past the test of the address it is run at). */
	u32 link_at[2];
	u32 linked_entry;
	/* 32-bit ARM: the host register T is in at the end (-1: none), and
	 * whether the tail cannot go by the flags the last instruction left
	 * (something that changes them is emitted in between) */
	int t_reg;
	bool tail_flags_lost;

	/* in the lists of the pages its code is in (read-only blocks), and in
	 * the temporary blocks' (temp_block): links of their own, nothing
	 * allocated to put it in or take it out; in none after Setup() */
	BlockPageLink page_link[BLOCK_PAGES];
	u32 page_links;
	RuntimeBlockInfo *temp_prev, *temp_next;
	bool temp_listed;
};

/* An operation added at the end of @block's, put in at @at, taken out
 * from @at */
void bm_OpPush(RuntimeBlockInfo *block, const shil_opcode *op);
void bm_OpInsert(RuntimeBlockInfo *block, u32 at, const shil_opcode *op);
void bm_OpErase(RuntimeBlockInfo *block, u32 at);

/* (a block's operations, for a loop over them) */
struct BlockOps
{
	shil_opcode *first, *last;
	shil_opcode *begin() const { return first; }
	shil_opcode *end() const { return last; }
};
static inline BlockOps bm_Ops(RuntimeBlockInfo *block)
{
	BlockOps ops = { block->oplist, block->oplist + block->op_count };
	return ops;
}

/* A new block, held once (by whoever makes it): in the recompiler's pool */
RuntimeBlockInfo *bm_NewBlock(void);
void bm_BlockRef(RuntimeBlockInfo *block);
/* let go: the block goes if nothing holds it now */
void bm_BlockUnref(RuntimeBlockInfo *block);

RuntimeBlockInfoPtr bm_GetBlock2(void* dynarec_code);
RuntimeBlockInfoPtr bm_GetStaleBlock(void* dynarec_code);
RuntimeBlockInfoPtr DYNACALL bm_GetBlock(u32 addr);

void bm_AddBlock(RuntimeBlockInfo* blk);
void bm_DiscardBlock(RuntimeBlockInfo* block);
// @nothing_since: no block has been compiled since the last time
void bm_ResetCache(bool nothing_since = false);
void bm_ResetTempCache(bool full);

void bm_Init();
void bm_Term();

void bm_RamWriteAccess(u32 addr);
#endif
