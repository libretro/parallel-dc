/*
	In case you wonder, the extern "C" stuff are for the assembly code on beagleboard/pandora
*/
#ifndef SH4_DYNA_BLOCKMANAGER_H
#define SH4_DYNA_BLOCKMANAGER_H
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
#endif

#include "decoder.h"
#include "dyna_state.h"
#include "node_pool.h"

/* A block, as it is handed about: by its address. It is the block
 * manager's for as long as it is in its tables, or among the blocks
 * discarded lately (a second at most), and those that jump to it hold
 * it as well (pre_refs): it goes when the last lets go (bm_BlockUnref()). */
typedef RuntimeBlockInfo *RuntimeBlockInfoPtr;

/* A block's place in the list of a page of main memory its code is in
 * (blockmanager.c: the page's blocks, discarded when it is written) */
typedef struct BlockPageLink BlockPageLink;
struct BlockPageLink
{
	RuntimeBlockInfo *block;
	BlockPageLink *prev, *next;
	u32 page;
};
/* the pages a block's code can be in: it is at most BLOCK_MAX_SH_OPS_HARD
 * instructions, a fraction of a page, so two; four with room */
#define BLOCK_PAGES 4

#ifdef __cplusplus
extern "C" {
#endif
/* The block made ready to be decoded at @pc with @fpu_cfg (false: the SH4
 * took an exception on the way) */
bool bm_BlockSetup(RuntimeBlockInfo *block, u32 pc, fpscr_t fpu_cfg);
/* whether it is one of the blocks known to be a wait for something */
bool bm_BlockIsIdle(RuntimeBlockInfo *block);
/* The recompiler's: the block's links made again, for its pNextBlock and
 * pBranchBlock as they are now (none: back to the main loop); the bytes of
 * code that come to */
u32 ngen_Relink(RuntimeBlockInfo *block);
/* @other, which jumps to @block, put in its pre_refs (held), taken out */
void bm_BlockAddRef(RuntimeBlockInfo *block, RuntimeBlockInfo *other);
void bm_BlockRemRef(RuntimeBlockInfo *block, RuntimeBlockInfo *other);
/* the blocks that jump to it sent elsewhere, and let go */
void bm_BlockDiscard(RuntimeBlockInfo *block);
/* read_only, by where its code comes from */
void bm_BlockSetProtectedFlags(RuntimeBlockInfo *block);
#ifdef __cplusplus
}
#endif

struct RuntimeBlockInfo
{
	u32 addr;
	DynarecCodeEntryPtr code;
	u32 lookups;

	/* who holds it (see RuntimeBlockInfoPtr) */
	u32 refs;

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
	/* with the MMU on: may go straight on into another block (rdv_MmuMayGoOn()) */
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
	/* Its operations while it is made: op_count of them, in room for
	 * op_room (bm_OpPush(), bm_OpInsert(), bm_OpErase()), in the list
	 * bm_BlockLendOps() lends it; none once it is made */
	shil_opcode *oplist;
	u32 op_count;
	u32 op_room;

	/* The blocks that jump to this one, each held (see RuntimeBlockInfoPtr):
	 * told when it goes, to jump elsewhere. In the recompiler's pool. */
	RuntimeBlockInfo **pre_refs;
	u32 pre_ref_count;
	u32 pre_ref_room;

	bool read_only;

	/* What the recompiler keeps of the block. x86-64 and x86: where in
	 * its code the link sites are, 0 where there is none - the way out to
	 * BranchBlock, and the one to NextBlock - and where a block linked to
	 * this one comes in (past the test of the address it is run at). */
	u32 link_at[2];
	u32 linked_entry;
	/* 32-bit ARM: the host register T is in at the end (-1: none), and the
	 * condition the tail goes by where it goes by the flags the last
	 * instruction left (-1: it compares T) */
	int t_reg;
	int tail_cc;

	/* in the lists of the pages its code is in (read-only blocks), and in
	 * the temporary blocks' (temp_block): links of their own, nothing
	 * allocated to put it in or take it out; in none after Setup() */
	BlockPageLink page_link[BLOCK_PAGES];
	u32 page_links;
	RuntimeBlockInfo *temp_prev, *temp_next;
	bool temp_listed;

#ifdef __cplusplus
	/* (for the recompilers in C++) */
	bool Setup(u32 pc, fpscr_t cfg) { return bm_BlockSetup(this, pc, cfg); }
	bool contains_code(const u8* ptr) const
	{
		return ((size_t)(ptr-(u8*)code)) < host_code_size;
	}
	u32 Relink();
	void AddRef(RuntimeBlockInfo *other) { bm_BlockAddRef(this, other); }
	void RemRef(RuntimeBlockInfo *other) { bm_BlockRemRef(this, other); }
	void Discard() { bm_BlockDiscard(this); }
	void SetProtectedFlags() { bm_BlockSetProtectedFlags(this); }
#endif
};

/* whether @ptr is in the block's code */
static TYPES_INLINE bool bm_ContainsCode(const RuntimeBlockInfo *block, const u8 *ptr)
{
	return ((size_t)(ptr - (const u8 *)(uintptr_t)block->code)) < block->host_code_size;
}

#ifdef __cplusplus
extern "C" {
#endif
/* Room for an operation at the end of @block's, room for one put in at
 * @at (those from there on moved up), an operation taken out from @at: the
 * room is the caller's to fill, where it is */
shil_opcode *bm_OpPush(RuntimeBlockInfo *block);
shil_opcode *bm_OpInsert(RuntimeBlockInfo *block, u32 at);
void bm_OpErase(RuntimeBlockInfo *block, u32 at);
/* The list of operations lent to @block while it is made, and taken back
 * from it before it is kept or let go: one list for every block, kept */
void bm_BlockLendOps(RuntimeBlockInfo *block);
void bm_BlockTakeOps(RuntimeBlockInfo *block);

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
/* @nothing_since: no block has been compiled since the last time */
void bm_ResetCache(bool nothing_since);
void bm_ResetTempCache(bool full);

void bm_Init(void);
void bm_Term(void);

void bm_RamWriteAccess(u32 addr);
#ifdef __cplusplus
}

/* (a block's operations, for a loop over them in C++) */
struct BlockOps
{
	shil_opcode *first, *last;
	shil_opcode *begin() const { return first; }
	shil_opcode *end() const { return last; }
};
static inline BlockOps bm_Ops(const RuntimeBlockInfo *block)
{
	BlockOps ops = { block->oplist, block->oplist + block->op_count };
	return ops;
}
#endif

#endif
