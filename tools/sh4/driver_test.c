/* The recompiler's driver (core/hw/sh4/dyna/driver.c) with the block
 * manager, everything else stubbed: random code compiled, looked up, run
 * into, linked, failing its checks until it is taken for code that keeps
 * being rewritten and compiled into the temporary cache, the caches
 * emptied - and what comes of it hashed (where each block's code is, in
 * which cache, what was asked of the recompiler), against what the C++
 * driver it replaced did (sh tools/sh4/driver_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/mem/_vmem.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/dyna/ssa.h"
#include "encodings/xxh32.h"

/* What the driver and the block manager call of the rest of the core */
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
/* (the driver's, where it is compiling into the main cache) */
extern u32 LastAddr, TempLastAddr;
u8 *sh4_dyna_rcb;
bool sh4_in_delay_slot;
sh4_catch *sh4_catch_top;

static u32 rng = 1;
static unsigned h = 2166136261u;
static u8 *code_room;

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

/* a host address as it is known here: where in the code caches */
static u32 offs(const void *p)
{
	if (p == NULL)
		return 0xFFFFFFFFu;
	if ((uintptr_t)p == (uintptr_t)ngen_FailedToFindBlock)
		return 0xFFFFFFFEu;
	return (u32)((const u8 *)p - CodeCache);
}

static u32 code_offs(DynarecCodeEntryPtr p)
{
	return offs((const void *)(uintptr_t)p);
}

bool vmem_platform_prepare_jit_block(void *code_area, unsigned size, void **code_area_rwx)
{
	(void)code_area;
	(void)size;
	*code_area_rwx = code_room;
	return true;
}
bool mem_region_lock(void *start, size_t len) { mix(1); mix((u32)((u8 *)start - mem_b.data)); mix((u32)len); return true; }
bool mem_region_unlock(void *start, size_t len) { mix(2); mix((u32)((u8 *)start - mem_b.data)); mix((u32)len); return true; }
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
void sh4_wait_sites_reset(void) { mix(0x57414954u); }
u64 sh4_sched_now64(void) { return 0; }
bool Do_Exception(u32 epc, u32 expEvn, u32 CallVect) { mix(epc); mix(expEvn); mix(CallVect); return true; }
void DoMMUException(u32 addr, u32 mmu_error, u32 access_type) { mix(addr); mix(mmu_error); mix(access_type); }
u32 mmu_full_lookup_c(u32 va, const TLB_Entry **entry, u32 *rv) { *entry = NULL; *rv = va; return MMU_ERROR_NONE; }
u32 mmu_data_translation_w64(u32 va, u32 *rv) { *rv = va; return MMU_ERROR_NONE; }
void DYNACALL _vmem_WriteMem64(u32 addr, u64 data) { (void)addr; (void)data; }
u8 *GetMemPtr(u32 addr, u32 size) { (void)size; return mem_b.data + (addr & RAM_MASK & ~1u); }
int UpdateINTC(void) { mix(0x494E5443u); return 0; }
void HostFloatModeSave(void) { }
void HostFloatModeBack(void) { }
void RestoreHostRoundingMode(void) { }
void Sh4_int_Init(void) { }
void Sh4_int_Term(void) { }
void Sh4_int_Stop(void) { }
void Sh4_int_Start(void) { }
void Sh4_int_Step(void) { }
void Sh4_int_Skip(void) { }
void Sh4_int_Reset(bool hard) { (void)hard; }
bool Sh4_int_IsCpuRunning(void) { return false; }
void ngen_init(void) { }
void ngen_mainloop(void *cntx) { (void)cntx; }
void ngen_ResetBlocks(void) { mix(0x52455342u); }
void os_DebugBreak(void) { printf("driver_test: a check failed\n"); exit(1); }

/* The decoder, by the address: a few instructions, ending in each way a
 * block ends, to addresses among those compiled */
static const u32 block_ends[] = { BET_StaticJump, BET_StaticCall, BET_StaticIntr, BET_DynamicJump,
	BET_DynamicCall, BET_DynamicRet, BET_Cond_0, BET_Cond_1 };
static u32 pc_of(u32 i)
{
	/* (one of them the BIOS's start, where the cache is emptied) */
	if (i == 95)
		return 0x8C0000E0;
	return 0x8C010000 + (i % 64) * 0x40 + (i / 64) * 0x10000;
}
/* The lists of operations the blocks were made in (the one list lent to
 * each, bm_BlockLendOps(), grown once to what these need), and whether a
 * block was kept or relinked with one still. Not hashed. */
static const shil_opcode *ops_seen;
static u32 ops_lists, ops_kept;
bool dec_DecodeBlock(RuntimeBlockInfo *rbi, u32 max_cycles)
{
	const u32 k = (rbi->addr * 2654435761u) >> 8;
	shil_opcode op;
	u32 i;
	(void)max_cycles;
	rbi->guest_opcodes = 1 + k % 20;
	memset(&op, 0, sizeof(op));
	for (i = 0; i < rbi->guest_opcodes; i++)
	{
		op.guest_offs = (u16)(i * 2);
		*bm_OpPush(rbi) = op;
	}
	rbi->sh4_code_size = rbi->guest_opcodes * 2;
	rbi->guest_cycles = rbi->guest_opcodes;
	rbi->BlockType = (BlockEndType)block_ends[(k >> 8) % 8];
	/* (what the decoder sets, as it does: the rest as bm_BlockSetup()
	 * left it) */
	if (BET_GET_CLS(rbi->BlockType) == BET_CLS_COND)
		rbi->has_jcond = true;
	if (BET_GET_CLS(rbi->BlockType) != BET_CLS_Dynamic)
		rbi->BranchBlock = pc_of((k >> 12) % 96);
	if (rbi->BlockType != BET_StaticJump)
		rbi->NextBlock = rbi->addr + rbi->sh4_code_size;
	mix(rbi->has_jcond);
	mix(rbi->BranchBlock);
	mix(rbi->NextBlock);
	mix(rbi->page_links);
	return true;
}
void AnalyseBlock(RuntimeBlockInfo *block) { mix(block->addr); }

/* The recompiler: code of its size written where the driver says */
void ngen_Compile(RuntimeBlockInfo *block, bool force_checks, bool reset, bool staging, bool optimise)
{
	u32 n = 8 + (block->addr >> 2) % 64, i;
	if (block->op_count != block->guest_opcodes || block->oplist[block->op_count - 1].guest_offs != (block->op_count - 1) * 2)
		ops_kept++;
	if (block->oplist != ops_seen)
	{
		ops_seen = block->oplist;
		ops_lists++;
	}
	note_entry(block->addr);
	block->code = (DynarecCodeEntryPtr)(uintptr_t)emit_GetCCPtr();
	for (i = 0; i < n; i++)
		emit_Write32(block->addr ^ i);
	block->host_code_size = n * 4;
	block->relink_offset = n * 2;
	mix(0x434F4D50u);
	mix(block->addr);
	mix(code_offs(block->code));
	mix(force_checks | reset << 1 | staging << 2 | optimise << 3 | block->temp_block << 4 | block->read_only << 5);
	mix(block->blockcheck_failures);
	mix(block->staging_runs);
}
u32 ngen_Relink(RuntimeBlockInfo *block)
{
	if (block->oplist != NULL || block->op_count != 0 || block->op_room != 0)
		ops_kept++;
	mix(0x52454C4Bu);
	mix(code_offs(block->code));
	mix(block->pNextBlock != NULL ? code_offs(block->pNextBlock->code) : 0);
	mix(block->pBranchBlock != NULL ? code_offs(block->pBranchBlock->code) : 0);
	mix(block->relink_data);
	return 16;
}

static void step(sh4_if *cpu)
{
	const u32 roll = rnd() % 100;
	const u32 pc = pc_of(rnd() % 96);
	RuntimeBlockInfo *b;
	DynarecCodeEntryPtr code;

	if (roll < 35)
	{
		/* the main loop: the code for an address, compiled if there is none */
		next_pc = pc;
		mix(code_offs(rdv_FindOrCompile()));
	}
	else if (roll < 60)
	{
		/* a block that failed its check (its code was written over), now
		 * and then again and again */
		int k = rnd() % 4 == 0 ? 1 + rnd() % 8 : 1;
		while (k-- > 0)
		{
			b = bm_GetBlock(pc);
			if (b == NULL)
				return;
			mix(0x46414C4Cu);
			code = rdv_BlockCheckFail(pc);
			mix(code_offs(code));
			b = bm_GetBlock(pc);
			if (b != NULL)
			{
				mix(b->temp_block);
				mix(b->blockcheck_failures);
			}
		}
	}
	else if (roll < 85)
	{
		/* a block asking to be linked to where it goes */
		b = bm_GetBlock(pc);
		if (b == NULL)
			return;
		mix(0x4C494E4Bu);
		mix(offs(rdv_LinkBlock((u8 *)(uintptr_t)b->code, BET_GET_CLS(b->BlockType) == BET_CLS_Dynamic ? pc_of(rnd() % 96) : rnd() & 1)));
		mix(b->host_code_size);
	}
	else if (roll < 90)
	{
		b = bm_GetBlock(pc);
		mix(rdv_DoInterrupts(b != NULL ? (void *)((u8 *)(uintptr_t)b->code + 4) : (void *)CodeCache));
	}
	else if (roll < 92)
	{
		/* a write to code: the page's blocks go */
		b = bm_GetBlock(pc);
		if (b != NULL && b->read_only && b->page_links != 0)
			bm_RamWriteAccess(pc);
	}
	else if (roll < 94)
	{
		if (rnd() % 8 != 0)
			return;
		cpu->ResetCache();
		mix(LastAddr);
	}
	else if (roll < 95)
	{
		/* a place compiled at that empties the cache (the BIOS's start) */
		next_pc = 0x8c0000e0;
		mix(code_offs(rdv_FindOrCompile()));
	}
	else
		bm_Periodical_1s();
	mix(LastAddr);
	mix(TempLastAddr);
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 200000, t;
	sh4_if cpu;

	p_sh4rcb = (Sh4RCB *)calloc(1, sizeof(Sh4RCB));
	mem_b.data = (u8 *)calloc(1, RAM_SIZE);
	mem_b.size = RAM_SIZE;
	code_room = (u8 *)malloc(CODE_SIZE + TEMP_CODE_SIZE);
	if (p_sh4rcb == NULL || mem_b.data == NULL || code_room == NULL)
		return 2;
	bm_vmem_pagefill(p_sh4rcb->fpcb, sizeof(p_sh4rcb->fpcb));
	memset(&cpu, 0, sizeof(cpu));
	Get_Sh4Recompiler(&cpu);
	cpu.Init();
	for (t = 0; t < n; t++)
	{
		step(&cpu);
		/* (the caches emptied, now and then, by their filling up: smaller
		 * here than they are, by starting further on) */
		if (t % 50000 == 49999)
			LastAddr = CODE_SIZE - CODE_MARGIN - 4096;
	}
	/* many blocks that keep being rewritten: each compiled into the
	 * temporary cache after its sixth failure, and none before */
	cpu.ResetCache();
	for (t = 0; t < n / 50; t++)
	{
		const u32 pc = 0x8C100000 + t * 0x40;
		int k;
		for (k = 0; k < 7; k++)
		{
			next_pc = pc;
			rdv_FindOrCompile();
			mix(bm_GetBlock(pc)->temp_block);
			rdv_BlockCheckFail(pc);
		}
	}
	/* A block on the list of waits (four instructions, its code's hash
	 * 0xB806EEE4), and not when a byte of it differs or it is longer. Not
	 * hashed. */
	{
		static const u8 wait_code[8] = { 0x08, 0x7F, 0x26, 0x4F, 0x09, 0x00, 0x09, 0x00 };
		RuntimeBlockInfo w;
		bool listed, changed, longer;
		memset(&w, 0, sizeof(w));
		w.addr = 0x8C200000;
		w.guest_opcodes = 4;
		w.sh4_code_size = 8;
		memcpy(GetMemPtr(w.addr, 8), wait_code, sizeof(wait_code));
		listed = bm_BlockIsIdle(&w);
		GetMemPtr(w.addr, 8)[2] ^= 1;
		changed = bm_BlockIsIdle(&w);
		GetMemPtr(w.addr, 8)[2] ^= 1;
		w.guest_opcodes = 5;
		w.sh4_code_size = 10;
		longer = bm_BlockIsIdle(&w);
		if (!listed || changed || longer)
			printf("FAIL: the waits' list: %d for the one on it, %d changed, %d longer\n", listed, changed, longer);
	}
	cpu.Term();
	if (ops_kept != 0 || ops_lists != 1)
		printf("FAIL: the blocks' operations: %u blocks held them, %u lists made\n", ops_kept, ops_lists);
	printf("%d steps, %08x\n", n, h);
	free(code_room);
	free(entries_set);
	free(mem_b.data);
	free(p_sh4rcb);
	return 0;
}
