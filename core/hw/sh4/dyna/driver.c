#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"

/* (the state is a complete type: hash() keeps it on the stack) */
#include <encodings/xxh32.h>
#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_if.h"
#include "hw/sh4/sh4_interrupts.h"

#include "hw/mem/_vmem.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/aica/aica_if.h"
#include "hw/gdrom/gdrom_if.h"

#include <time.h>
#include <float.h>

#include "blockmanager.h"
#include "ngen.h"
#include "decoder.h"
#include "ssa.h"

#if FEAT_SHREC != DYNAREC_NONE

/* (the portable recompiler's code is records, read: plain memory) */
#if defined(HOST_APPLE_JIT) && FEAT_SHREC == DYNAREC_JIT
#define REC_APPLE_JIT
#endif

#ifdef REC_APPLE_JIT
/* (no array: the code goes in MAP_JIT memory, recSh4_HaveCodeCache()) */
#else
u8 SH4_TCB[CODE_SIZE + TEMP_CODE_SIZE + 4096]
#if defined(_WIN32) || FEAT_SHREC != DYNAREC_JIT
	;
#elif defined(__linux__) || defined(__HAIKU__) || \
      defined(__FreeBSD__) || defined(__DragonFly__) || \
	  defined(HAVE_LIBNX)
	__attribute__((section(".text")));
#elif defined(__MACH__)
	__attribute__((section("__TEXT,.text")));
#else
	#error SH4_TCB ALLOC
#endif
#endif

u8* CodeCache;
u8* TempCodeCache;
uintptr_t cc_rx_offset;

u32 LastAddr = 0;
u32 LastAddr_min = 0;
u32 TempLastAddr = 0;
u32* emit_ptr = NULL;
u32* emit_ptr_limit = NULL;

/* The addresses of the blocks whose code keeps being rewritten: compiled
 * into the temporary cache from then on. A set, by open addressing in a
 * table of a power of two, made when the recompiler starts and grown in
 * the rare case it comes to half full; emptied with the cache. */
static u32 *smc_hotspots;		/* 0: empty (no block is compiled at 0) */
static u32 smc_hotspot_room;
static u32 smc_hotspot_count;

static u32 smc_slot(u32 addr, u32 room)
{
	return (addr * 2654435761u >> 7) & (room - 1);
}

static bool smc_hotspot_find(u32 addr)
{
	u32 i;
	if (smc_hotspot_count == 0)
		return false;
	for (i = smc_slot(addr, smc_hotspot_room); smc_hotspots[i] != 0; i = (i + 1) & (smc_hotspot_room - 1))
		if (smc_hotspots[i] == addr)
			return true;
	return false;
}

static void smc_hotspots_make(u32 room)
{
	u32 *had = smc_hotspots, had_room = smc_hotspot_room, i;
	smc_hotspots = (u32 *)calloc(room, sizeof(*smc_hotspots));
	if (smc_hotspots == NULL)
		die("out of memory");
	smc_hotspot_room = room;
	smc_hotspot_count = 0;
	for (i = 0; i < had_room; i++)
		if (had[i] != 0)
		{
			u32 j = smc_slot(had[i], room);
			while (smc_hotspots[j] != 0)
				j = (j + 1) & (room - 1);
			smc_hotspots[j] = had[i];
			smc_hotspot_count++;
		}
	free(had);
}

/* whether @addr was not there before */
static bool smc_hotspot_insert(u32 addr)
{
	u32 i;
	if (addr == 0 || smc_hotspot_find(addr))
		return false;
	if ((smc_hotspot_count + 1) * 2 > smc_hotspot_room)
		smc_hotspots_make(smc_hotspot_room != 0 ? smc_hotspot_room * 2 : 1024);
	i = smc_slot(addr, smc_hotspot_room);
	while (smc_hotspots[i] != 0)
		i = (i + 1) & (smc_hotspot_room - 1);
	smc_hotspots[i] = addr;
	smc_hotspot_count++;
	return true;
}

static void smc_hotspots_clear(void)
{
	if (smc_hotspot_count != 0)
		memset(smc_hotspots, 0, smc_hotspot_room * sizeof(*smc_hotspots));
	smc_hotspot_count = 0;
}


void* emit_GetCCPtr(void)
{
   if (emit_ptr)
      return (void*)emit_ptr;
   return (void*)&CodeCache[LastAddr];
}

void emit_SetBaseAddr(void)
{
   LastAddr_min = LastAddr;
}

void clear_temp_cache(bool full)
{
	TempLastAddr = 0;
	bm_ResetTempCache(full);
}

static void recSh4_ClearCache(void)
{
	/* (see bm_ResetCache()) */
	const bool nothing_since = LastAddr == LastAddr_min && TempLastAddr == 0;
	INFO_LOG(DYNAREC, "recSh4:Dynarec Cache clear at %08X free space %d\n", next_pc, emit_FreeSpace());
	LastAddr=LastAddr_min;
	bm_ResetCache(nothing_since);
	smc_hotspots_clear();
	clear_temp_cache(true);
}

static void recSh4_Run(void)
{
   sh4_int_bCpuRun = true;
	HostFloatModeSave();
	RestoreHostRoundingMode();

	/* (nothing left of a run that ended in the middle of one) */
	sh4_catch_top = NULL;
	sh4_in_delay_slot = false;
	sh4_dyna_rcb=(u8*)&Sh4cntx + sizeof(Sh4cntx);
	INFO_LOG(DYNAREC, "cntx, fpcb offset: %ld, pc offset: %ld, pc %08X\n", (long)((u8*)&sh4rcb.fpcb - sh4_dyna_rcb), (long)((u8*)&sh4rcb.cntx.pc - sh4_dyna_rcb), sh4rcb.cntx.pc);
	ngen_mainloop(sh4_dyna_rcb);

	sh4_int_bCpuRun=false;
	HostFloatModeBack();
}

void emit_Write32(u32 data)
{
	if (emit_ptr)
	{
		*emit_ptr=data;
		emit_ptr++;
	}
	else
	{
		*(u32*)&CodeCache[LastAddr]=data;
		LastAddr+=4;
	}
}

void emit_Skip(u32 sz)
{
	if (emit_ptr)
		emit_ptr = (u32*)((u8*)emit_ptr + sz);
	else
		LastAddr+=sz;
}
u32 emit_FreeSpace(void)
{
	if (emit_ptr)
		return (emit_ptr_limit - emit_ptr) * sizeof(u32);
	else
		return CODE_SIZE-LastAddr;
}


/* The blocks known to be a wait for something, by how many instructions
 * they have and the hash of their code (bm_BlockIsIdle()) */
static const struct
{
	u32 opcodes;
	u32 hash;
} idle_blocks[] =
{
	/* BIOS */
	{ 0x05, 0x13B23363 },
	{ 0x04, 0x2E23A33B },
	{ 0x04, 0xFB498832 },
	{ 0x0A, 0x50A249F9 },

	/* SC */
	{ 0x0A, 0xB4E90338 },
	{ 0x04, 0x11578A16 },
	{ 0x04, 0xC281CC52 },

	/* HH */
	{ 0x07, 0x0757DC10 },
	{ 0x04, 0x1476CC5E },

	/* these look very suspicious, but I'm not sure about any of them */
	/* cross testing w/ IKA makes them more suspects than not */
	{ 0x0D, 0x8C2921FF },
	{ 0x04, 0xB806EEE4 },

	/*  Dead or Alive 2 */
	{ 0x08, 0x0A37187A }
};

bool bm_BlockIsIdle(RuntimeBlockInfo *block)
{
	/* (on the stack: nothing allocated) */
	xxh32_state_t state_room;
	xxh32_state_t *state = &state_room;
	const u8 *ptr;
	u32 hash, i;
	bool listed = false;

	for (i = 0; i < sizeof(idle_blocks) / sizeof(idle_blocks[0]); i++)
		if (idle_blocks[i].opcodes == block->guest_opcodes)
			listed = true;
	if (!listed)
		return false;
	ptr = GetMemPtr(block->addr, block->sh4_code_size);
	if (ptr == NULL)
		return false;
	/* the first guest_opcodes bytes of its code, each as 16 bits */
	xxh32_reset(state, 7);
	for (i = 0; i < block->guest_opcodes; i++)
	{
		const u16 data = ptr[i];
		xxh32_update(state, &data, 2);
	}
	hash = xxh32_digest(state);
	for (i = 0; i < sizeof(idle_blocks) / sizeof(idle_blocks[0]); i++)
		if (idle_blocks[i].opcodes == block->guest_opcodes && idle_blocks[i].hash == hash)
			return true;
	return false;
}

bool bm_BlockSetup(RuntimeBlockInfo *block, u32 rpc, fpscr_t rfpu_cfg)
{
#if !defined(NO_MMU)
	sh4_catch c;
#endif
	block->staging_runs = 0;
	block->addr = block->lookups = block->runs = block->host_code_size = 0;
	block->guest_cycles = block->guest_opcodes = block->host_opcodes = 0;
	block->sh4_code_size = 0;
	block->pBranchBlock = block->pNextBlock = 0;
	block->code = 0;
	block->has_jcond = false;
	block->BranchBlock = block->NextBlock = block->csc_RetCache = 0xFFFFFFFF;
	block->BlockType = (BlockEndType)BET_SCL_Intr;
	block->has_fpu_op = false;
	block->temp_block = false;
	block->mmu_go_on = false;
	block->page_links = 0;
	block->temp_prev = block->temp_next = NULL;
	block->temp_listed = false;

	block->vaddr = rpc;
#ifndef NO_MMU
	if (mmu_enabled())
	{
		u32 rv = mmu_instruction_translation_c(block->vaddr, &block->addr);
		if (rv != MMU_ERROR_NONE)
		{
			DoMMUException(block->vaddr, rv, MMU_TT_IREAD);
			return false;
		}
	}
	else
#endif /* NO_MMU */
	if (block->vaddr & 1)
	{
		/* An instruction is at an even address: fetching one from an odd
		 * one is an address error, raised here as the MMU's are above -
		 * the caller goes on from where the exception leads. (It was
		 * compiled, from between two instructions.) */
		CCN_TEA = block->vaddr;
		Do_Exception(block->vaddr, 0xE0, 0x100);
		return false;
	}
	else
		block->addr = block->vaddr;
	block->fpu_cfg = rfpu_cfg;

	block->op_count = 0;

#if !defined(NO_MMU)
	SH4_TRY(c)
	{
		const bool decoded = dec_DecodeBlock(block, SH4_TIMESLICE / 2);
		sh4_catch_pop(&c);
		if (!decoded)
			return false;
	}
	else
	{
		Do_Exception(rpc, c.ex.expEvn, c.ex.callVect);
		return false;
	}
#else
	if (!dec_DecodeBlock(block, SH4_TIMESLICE / 2))
		return false;
#endif
	bm_BlockSetProtectedFlags(block);

	AnalyseBlock(block);

	return true;
}

DynarecCodeEntryPtr rdv_CompilePC(u32 blockcheck_failures)
{
	u32 pc=next_pc;
	RuntimeBlockInfo* rbi;
	bool do_opts, block_check;

	if (emit_FreeSpace() < CODE_MARGIN || pc==0x8c0000e0 || pc==0xac010000 || pc==0xac008300)
		recSh4_ClearCache();

	rbi = bm_NewBlock();
	bm_BlockLendOps(rbi);

	if (!bm_BlockSetup(rbi, pc,fpscr))
	{
		bm_BlockTakeOps(rbi);
		bm_BlockUnref(rbi);
		return NULL;
	}
	rbi->blockcheck_failures = blockcheck_failures;
	rbi->mmu_go_on = rdv_MmuMayGoOn(rbi);
	if (smc_hotspot_find(rbi->addr))
	{
		if (TEMP_CODE_SIZE - TempLastAddr < CODE_MARGIN)
			clear_temp_cache(false);
		emit_ptr = (u32 *)(TempCodeCache + TempLastAddr);
		emit_ptr_limit = (u32 *)(TempCodeCache + TEMP_CODE_SIZE);
		rbi->temp_block = true;
		if (rbi->read_only)
			INFO_LOG(DYNAREC, "WARNING: temp block %x (%x) is protected!\n", rbi->vaddr, rbi->addr);
	}
	do_opts = !rbi->temp_block;
	rbi->staging_runs=do_opts?100:-100;
	block_check = rbi->read_only ? false : IsOnRam(rbi->addr);
	ngen_Compile(rbi, block_check, (pc & 0xFFFFFF) == 0x08300 || (pc & 0xFFFFFF) == 0x10000, false, do_opts);
	verify(rbi->code!=0);
	bm_BlockTakeOps(rbi);

	bm_AddBlock(rbi);

	if (emit_ptr != NULL)
	{
		TempLastAddr = (u8*)emit_ptr - TempCodeCache;
		emit_ptr = NULL;
		emit_ptr_limit = NULL;
	}

	return rbi->code;
}

DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock_pc(void)
{
	return rdv_FailedToFindBlock(next_pc);
}

DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock(u32 pc)
{
	DynarecCodeEntryPtr code;
	next_pc=pc;
	code = rdv_CompilePC(0);
	if (code == NULL)
		code = bm_GetCodeByVAddr(next_pc);
	else
		code = (DynarecCodeEntryPtr)(uintptr_t)CC_RW2RX((void *)(uintptr_t)code);
	return code;
}

u32 DYNACALL rdv_DoInterrupts_pc(u32 pc) {
	next_pc = pc;
	UpdateINTC();

	return next_pc;
}

static void ngen_FailedToFindBlock_internal(void) {
	rdv_FailedToFindBlock(Sh4cntx.pc);
}

void (*ngen_FailedToFindBlock)(void) = &ngen_FailedToFindBlock_internal;

u32 DYNACALL rdv_DoInterrupts(void* block_cpde)
{
	RuntimeBlockInfoPtr rbi = bm_GetBlock2(block_cpde);
	/* The block may have been discarded while it ran (it wrote over its own
	 * code, or the cache was emptied): it is among those waiting to be
	 * freed then, and still knows its address. */
	if (!rbi)
		rbi = bm_GetStaleBlock(block_cpde);
	return rdv_DoInterrupts_pc(rbi ? rbi->vaddr : next_pc);
}

/* addr must be the physical address of the start of the block */
DynarecCodeEntryPtr DYNACALL rdv_BlockCheckFail(u32 addr)
{
	u32 blockcheck_failures = 0;
	if (mmu_enabled())
	{
		RuntimeBlockInfoPtr block = bm_GetBlock(addr);
		/* (gone already, if what failed was run once more after it was discarded) */
		if (block)
		{
			blockcheck_failures = block->blockcheck_failures + 1;
			if (blockcheck_failures > 5)
			{
				if (smc_hotspot_insert(addr))
					DEBUG_LOG(DYNAREC, "rdv_BlockCheckFail SMC hotspot @ %08x fails %d\n", addr, blockcheck_failures);
			}
			bm_DiscardBlock(block);
		}
	}
	else
	{
		/* Discard only the stale block instead of flushing the whole code
		 * cache. The full flush forced every block in the working set to be
		 * recompiled -- tens of milliseconds of host time in a single frame,
		 * re-triggered every time a game overwrote any code (level loads,
		 * overlays, SMC) -- measured as frame-time spikes of up to hundreds of
		 * ms. bm_DiscardBlock unlinks incoming direct links via pre_refs and
		 * removes the block from the dispatch table, so execution safely falls
		 * back to rdv_CompilePC for just this block; it is the exact mechanism
		 * the MMU path has always used on block-check failure. The SMC hotspot
		 * tracking likewise now applies to non-MMU titles, moving repeat
		 * offenders to the temp cache so they stop paying the block-check. */
		RuntimeBlockInfoPtr block = bm_GetBlock(addr);
		if (block)
		{
			blockcheck_failures = block->blockcheck_failures + 1;
			if (blockcheck_failures > 5)
			{
				if (smc_hotspot_insert(addr))
					DEBUG_LOG(DYNAREC, "rdv_BlockCheckFail SMC hotspot @ %08x fails %d\n", addr, blockcheck_failures);
			}
			bm_DiscardBlock(block);
		}
		next_pc = addr;
	}
	return (DynarecCodeEntryPtr)(uintptr_t)CC_RW2RX((void *)(uintptr_t)rdv_CompilePC(blockcheck_failures));
}

DynarecCodeEntryPtr rdv_FindOrCompile(void)
{
	DynarecCodeEntryPtr rv = bm_GetCodeByVAddr(next_pc);	/* Returns exec addr */
	/* (Compiling can be an exception instead - an odd address is one: what
	 * comes back is the handler's code then, and the context has its
	 * address.) */
	if (rv == ngen_FailedToFindBlock)
		rv = rdv_FailedToFindBlock(next_pc);

	return rv;
}

bool rdv_MmuMayGoOn(const RuntimeBlockInfo *block)
{
#ifndef NO_MMU
	const TLB_Entry *entry;
	u32 addr;

	if (!mmu_enabled() || mmu_strict)
		return false;
	if ((block->vaddr >> 29) == 4 || (block->vaddr >> 29) == 5)
		return true;
	if (mmu_full_lookup_c(block->vaddr, &entry, &addr) != MMU_ERROR_NONE)
		return false;
	return entry->Data.SZ1 != 0 || entry->Data.SZ0 != 0;
#else
	return false;
#endif
}

bool rdv_MmuSamePlace(const RuntimeBlockInfo *block, u32 target, u32 *addr)
{
	if ((target >> 29) == 4 || (target >> 29) == 5)
	{
		*addr = target;
		return true;
	}
	if ((target >> 12) != (block->vaddr >> 12))
		return false;
	*addr = (block->addr & ~0xFFFu) | (target & 0xFFF);
	return true;
}

void* DYNACALL rdv_LinkBlock(u8* code,u32 dpc)
{
	/* code is the RX addr to return after, however bm_GetBlock returns RW */
	RuntimeBlockInfoPtr rbi=bm_GetBlock2((void*)code);
	bool stale_block = false;
	u32 bcls, target, place;
	bool mmu, link;
	DynarecCodeEntryPtr rv;

	if (!rbi)
	{
		stale_block = true;
		rbi = bm_GetStaleBlock(code);
	}
	
	verify(rbi != NULL);

	bcls = BET_GET_CLS(rbi->BlockType);

	if (bcls == BET_CLS_Static)
	{
		if (rbi->BlockType == BET_StaticIntr)
			next_pc = rbi->NextBlock;
		else
			next_pc=rbi->BranchBlock;
	}
	else if (bcls == BET_CLS_Dynamic)
	{
		next_pc=dpc;
	}
	else if (bcls == BET_CLS_COND)
	{
		if (dpc)
			next_pc=rbi->BranchBlock;
		else
			next_pc=rbi->NextBlock;
	}

	mmu = mmu_enabled();
	target = next_pc;
	/* where the target is, for the block manager */
	place = target;

	if (!mmu)
		rv = rdv_FindOrCompile();	/* Returns rx ptr */
	else
	{
		/* (Looking the address up can raise an exception, and so can
		 * compiling what is there: what comes back is the handler's code
		 * then, or the routine that compiles it, and the context has its
		 * address.) */
		rv = bm_GetCodeByVAddr(target);
		if (rv == ngen_FailedToFindBlock)
			rv = rdv_FailedToFindBlock(next_pc);
	}

	/* Compiling may have emptied the cache the block was in - the whole
	 * one, or the temporary one if it was there. It is gone then, and is
	 * not to be linked: its code is no longer its own, and what would be
	 * patched is the block just compiled, if that is where it was put. */
	if (!stale_block && bm_GetBlock2((void*)code) != rbi)
		stale_block = true;

	/* With the MMU on a block asks to be linked only to an address that
	 * is where it is whatever the TLB says (ngen.h). It is linked to the
	 * block that was compiled for that very address - not to one for
	 * another address of the same memory, which would send it back - and
	 * not at all if an exception was raised instead. */
	link = !stale_block && next_pc == target;
	if (mmu && link)
	{
		link = false;
		if (bcls != BET_CLS_Dynamic && rbi->mmu_go_on && next_pc == target
				&& rdv_MmuSamePlace(rbi, target, &place))
		{
			RuntimeBlockInfoPtr nxt = bm_GetBlock(place);

			link = nxt != NULL && nxt->addr == place && nxt->vaddr == target
				&& (DynarecCodeEntryPtr)(uintptr_t)CC_RW2RX((void *)(uintptr_t)nxt->code) == rv;
		}
	}

	if (link)
	{
		u32 ncs;
		if (bcls == BET_CLS_Dynamic)
		{
			verify(rbi->relink_data == 0 || rbi->pBranchBlock == NULL);

			if (rbi->pBranchBlock!= NULL)
			{
				bm_BlockRemRef(rbi->pBranchBlock, rbi);
				rbi->pBranchBlock = NULL;
				rbi->relink_data = 1;
			}
			else if (rbi->relink_data == 0)
			{
				rbi->pBranchBlock = bm_GetBlock(place);
				bm_BlockAddRef(rbi->pBranchBlock, rbi);
			}
		}
		else
		{
			RuntimeBlockInfo* nxt = bm_GetBlock(place);

			if (rbi->BranchBlock == target)
				rbi->pBranchBlock = nxt;
			if (rbi->NextBlock == target)
				rbi->pNextBlock = nxt;

			bm_BlockAddRef(nxt, rbi);
		}
		ncs = rbi->relink_offset + ngen_Relink(rbi);
		verify(rbi->host_code_size >= ncs);
		rbi->host_code_size = ncs;
	}
	else if (!mmu && stale_block)
	{
		INFO_LOG(DYNAREC, "null RBI: from %08X to %08X -- unlinked stale block -- code %p next %p\n", rbi->vaddr, next_pc, (void *)code, (void *)(uintptr_t)rv);
	}
	
	return (void*)(uintptr_t)rv;
}

static void recSh4_Stop(void)
{
	Sh4_int_Stop();
}
static void recSh4_Start(void)
{
	Sh4_int_Start();
}

static void recSh4_Step(void)
{
	Sh4_int_Step();
}

static void recSh4_Skip(void)
{
	Sh4_int_Skip();
}

static void recSh4_Reset(bool hard)
{
	Sh4_int_Reset(hard);
	recSh4_ClearCache();
}

#ifdef REC_APPLE_JIT
static u8 *apple_code_cache;

/* Whether there is memory for the code (see vmem_platform_alloc_jit()):
 * mapped the first time, and kept for every game after that. Without it
 * the interpreter is used (LoadSpecialSettingsCPU()). */
bool recSh4_HaveCodeCache(void)
{
	if (apple_code_cache == NULL)
		apple_code_cache = (u8 *)vmem_platform_alloc_jit(CODE_SIZE + TEMP_CODE_SIZE);
	return apple_code_cache != NULL;
}
#endif

/* where @ptr is in the SH4's block, from its end */
#define RCB_NOFFS(ptr) ((s32)((u8 *)(ptr) - (u8 *)p_sh4rcb - (s32)sizeof(Sh4RCB)))

static void recSh4_Init(void)
{
#ifndef REC_APPLE_JIT
	void *candidate_ptr;
#endif
	INFO_LOG(DYNAREC, "recSh4 Init\n");
	Sh4_int_Init();
	bm_Init();
	if (smc_hotspots == NULL)
		smc_hotspots_make(1024);

	verify(RCB_NOFFS(p_sh4rcb->sq_buffer) == -512);

	verify(RCB_NOFFS(&p_sh4rcb->cntx.sh4_sched_next) == -152);
	verify(RCB_NOFFS(&p_sh4rcb->cntx.interrupt_pend) == -148);
	
	if (_nvmem_enabled())
	{
		if (!_nvmem_4gb_space())
		{
			verify(mem_b.data==((u8*)p_sh4rcb->sq_buffer+512+0x0C000000));
		}
		else
		{
			verify(mem_b.data==((u8*)p_sh4rcb->sq_buffer+512+0x8C000000));
		}
	}
	
#ifdef REC_APPLE_JIT
	recSh4_HaveCodeCache();
	CodeCache = apple_code_cache;
#else
	/* Prepare some pointer to the pre-allocated code cache: */
	candidate_ptr = (void*)(((unat)SH4_TCB + 4095) & ~4095);

	/* Call the platform-specific magic to make the pages RWX */
	CodeCache = NULL;
	#if FEAT_SHREC == DYNAREC_CPP
	CodeCache = (u8 *)candidate_ptr;
	#elif defined(FEAT_NO_RWX_PAGES)
	verify(vmem_platform_prepare_jit_block_rw(candidate_ptr, CODE_SIZE + TEMP_CODE_SIZE, (void**)&CodeCache, &cc_rx_offset));
	#else
	verify(vmem_platform_prepare_jit_block(candidate_ptr, CODE_SIZE + TEMP_CODE_SIZE, (void**)&CodeCache));
	#endif
#endif
	/* Ensure the pointer returned is non-null */
	verify(CodeCache != NULL);

	vmem_jit_write_begin();
	memset(CodeCache, 0xFF, CODE_SIZE + TEMP_CODE_SIZE);
	vmem_jit_write_end();
	TempCodeCache = CodeCache + CODE_SIZE;
	ngen_init();
	bm_ResetCache(false);
}

static void recSh4_Term(void)
{
	INFO_LOG(DYNAREC, "recSh4 Term\n");
	bm_Term();
	Sh4_int_Term();
}

static bool recSh4_IsCpuRunning(void)
{
	return Sh4_int_IsCpuRunning();
}

void Get_Sh4Recompiler(sh4_if* cpu)
{
	cpu->Run = recSh4_Run;
	cpu->Stop = recSh4_Stop;
	cpu->Start = recSh4_Start;
	cpu->Step = recSh4_Step;
	cpu->Skip = recSh4_Skip;
	cpu->Reset = recSh4_Reset;
	cpu->Init = recSh4_Init;
	cpu->Term = recSh4_Term;
	cpu->IsCpuRunning = recSh4_IsCpuRunning;
	cpu->ResetCache = recSh4_ClearCache;
}
#endif	/* FEAT_SHREC != DYNAREC_NONE */
