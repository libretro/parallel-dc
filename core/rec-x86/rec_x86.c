/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
/* The SH4 recompiler for 32-bit x86.
 *
 * It does what the x86-64 one (rec-x64/rec_x64.c) does, the same way and
 * in the same places, so that the two run a game the same to the cycle:
 * a block's cycles come off the counter where it ends, the time slice is
 * looked at in the same places, wait loops are found the same way, and the
 * floating point gives the same bits. What differs is what the processor
 * has: seven registers and no room for the SH4's context or the cycle
 * counter in one of them, so both are reached at their addresses, which
 * 32 bits can always give; the calls blocks make are fastcall (ecx, edx),
 * as every handler they reach is on this processor (DYNACALL); and a load
 * or store goes straight to memory only within the 512 MB the host has
 * laid out, the address taken down to it first.
 *
 * Everything the blocks jump to and call that is not C - the main loop,
 * the routines that compile a block or link one to the next, the ones a
 * fast memory access is rewritten to call - is written into the code
 * cache by ngen_init(), ahead of the blocks. So nothing here is assembly
 * linked into the library, which then has no relocations in its code: a
 * library that has them is not loaded by Android from version 6 on. */
#include "build.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#include "types.h"
#include "jit/x64_emit.h"
#include "x86_vector.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/dyna/ssa_regalloc.h"
#include "hw/sh4/modules/ccn.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_interrupts.h"

#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/sh4/dyna/wait_site.h"
#include "hw/holly/sb_mem.h"
#include "hw/mem/_vmem.h"

/* One block goes on to the next with a jump straight to its code, as on
 * x86-64 (see RuntimeBlockInfo::link_at). A link site is six bytes here: a call to the
 * link stub, a jump to the next block, or a jump through the table of
 * blocks (jmp dword [address]), each with what is left over filled. */
#define LINK_SITE_SIZE 6

/* What is left of the time slice: x86-64 has it in r14, this processor has
 * no register to spare and keeps it here. The SH4's context has another
 * copy, for the timers, which is written where the x86-64 recompiler
 * writes it, and only there: the state has it. */
int cycle_counter;

/* Made by ngen_init(), in the code cache */
static u8 *mainloop_code;
static u8 *block_return;	/* a block that does not go on to another jumps here */
static u8 *compile_stub;	/* what the table of blocks has for an address with no block */
static u8 *unwind_stub;		/* back to the main loop from an exception, wherever it is */
static u8 *link_stub;
/* the stack as blocks run on it, which unwind_stub puts back */
static u32 jmp_esp;

/* The four registers a function keeps on 32-bit x86 are handed out; eax,
 * ecx and edx are scratch, for the calls blocks make (fastcall: ecx, edx)
 * and for whatever an operation needs on the side. xmm4 to xmm7 are handed
 * out and xmm0 to xmm3 are scratch: no function keeps any of them, so the
 * ones in use are saved round every call (gen_call_raw()). */
static const u32 alloc_regs[] = { X86_EBX, X86_EBP, X86_ESI, X86_EDI };
static const u32 alloc_fregs[] = { 7, 6, 5, 4 };
/* Pairs of the SH4's floating-point registers (DRn, XDn) stay in memory:
 * four xmm registers are not enough to give them two each. */
#define ALLOC_F64 false

/* Back to the main loop, from code a block called that has raised an
 * SH4 exception and set the context for it (x86-64 has longjmp() for this:
 * the stack as it was is all there is to put back here). */
static void rec_unwind(void)
{
	((void (*)(void))(uintptr_t)unwind_stub)();
#ifdef __GNUC__
	__builtin_unreachable();
#endif
}

void ngen_mainloop(void* v_cntx)
{
	(void)v_cntx;
	((void (*)(void))(uintptr_t)mainloop_code)();
}

/* The kinds of fast memory access there are, as on x86-64: a load or a
 * store, of 1, 2, 4 or 8 bytes, the 4 from or to a general register or a
 * floating-point one. 8 bytes are always in xmm0. */
enum
{
	MEM_LOAD8, MEM_LOAD16, MEM_LOAD32, MEM_LOAD32F, MEM_LOAD64,
	MEM_STORE8, MEM_STORE16, MEM_STORE32, MEM_STORE32F, MEM_STORE64,
	MEM_KINDS
};
#define MEM_KIND_F(kind) ((kind) == MEM_LOAD32F || (kind) == MEM_STORE32F || (kind) == MEM_LOAD64 || (kind) == MEM_STORE64)

/* What a fast access that faulted is written over with a call to
 * (ngen_Rewrite()): a routine that does the access by asking. The access
 * has the address as the SH4 gave it in eax (ecx has it taken down to the
 * 512 MB), and its data in a general register (by number: eax for what is
 * loaded, edx or one of the block's for what is stored) or in an xmm
 * register (one of the block's, or xmm0); there is a routine for each kind
 * and each register. They keep the block's xmm registers, which the
 * function they call need not. */
static u8 *mem_stubs[MEM_KINDS][8];

/* What recompiled code calls (global: tools/sh4/x86_run.sh builds the C++
 * recompiler this replaced against them) */
u32 DYNACALL rec_x86_mem_load8(u32 addr)  { return (u32)(s32)(s8)ReadMem8(addr); }
u32 DYNACALL rec_x86_mem_load16(u32 addr) { return (u32)(s32)(s16)ReadMem16(addr); }
u32 DYNACALL rec_x86_mem_load32(u32 addr) { return ReadMem32(addr); }
u64 DYNACALL rec_x86_mem_load64(u32 addr) { return ReadMem64(addr); }
void DYNACALL rec_x86_mem_store8(u32 addr, u32 data)  { WriteMem8(addr, (u8)data); }
void DYNACALL rec_x86_mem_store16(u32 addr, u32 data) { WriteMem16(addr, (u16)data); }
void DYNACALL rec_x86_mem_store32(u32 addr, u32 data) { WriteMem32(addr, data); }
/* (64 bits from where @data points: they do not fit in the two registers) */
void DYNACALL rec_x86_mem_store64(u32 addr, const u32 *data)
{
	WriteMem64(addr, ((u64)data[1] << 32) | data[0]);
}

static void mem_stub_make(int kind, int data)
{
	static const uintptr_t routines[MEM_KINDS] = {
		(uintptr_t)&rec_x86_mem_load8, (uintptr_t)&rec_x86_mem_load16, (uintptr_t)&rec_x86_mem_load32,
		(uintptr_t)&rec_x86_mem_load32, (uintptr_t)&rec_x86_mem_load64,
		(uintptr_t)&rec_x86_mem_store8, (uintptr_t)&rec_x86_mem_store16, (uintptr_t)&rec_x86_mem_store32,
		(uintptr_t)&rec_x86_mem_store32, (uintptr_t)&rec_x86_mem_store64,
	};
	const bool load = kind < MEM_STORE8;
	x86_emit s;
	int i;

	x86_init(&s, (u8 *)emit_GetCCPtr(), 128);
	mem_stubs[kind][data] = s.p;
	/* Called from a block, whose stack is a multiple of 16 at a call: 12
	 * here, and 28 more make it one again, with room for the block's four
	 * xmm registers and 8 bytes of data. */
	x86_alu_ri(&s, false, X86_SUB, X86_ESP, 28);
	for (i = 0; i < 4; i++)
		x64_movd_mx(&s, x86_m(X86_ESP, i * 4), 4 + i);
	/* what is left of the time slice, for the timers (see cycle_counter in sh4_if.h) */
	x86_load(&s, X86_ECX, x86_abs(&cycle_counter));
	x86_store(&s, x86_abs(&Sh4cntx.cycle_counter), X86_ECX);
	x86_mov_rr(&s, X86_ECX, X86_EAX);
	if (kind == MEM_STORE64)
	{
		x64_movq_store(&s, x86_m(X86_ESP, 16), 0);
		x64_lea(&s, false, X86_EDX, x86_m(X86_ESP, 16));
	}
	else if (kind == MEM_STORE32F)
		x64_movd_rx(&s, X86_EDX, data);
	else if (!load && data != X86_EDX)
		x86_mov_rr(&s, X86_EDX, data);
	x86_call(&s, routines[kind]);
	for (i = 0; i < 4; i++)
		x64_movd_xm(&s, 4 + i, x86_m(X86_ESP, i * 4));
	if (kind == MEM_LOAD64)
	{
		x64_movd_xr(&s, 0, X86_EAX);
		x64_movd_xr(&s, 1, X86_EDX);
		x64_punpckldq(&s, 0, 1);
	}
	else if (kind == MEM_LOAD32F)
		x64_movd_xr(&s, data, X86_EAX);
	else if (load && data != X86_EAX)
		x86_mov_rr(&s, data, X86_EAX);
	x86_alu_ri(&s, false, X86_ADD, X86_ESP, 28);
	x86_ret(&s);
	verify(!s.full);
	emit_Skip(((u32)(s.p - s.start) + 15) & ~15u);
}

static void link_site_call_stub(u8 *site)
{
	const s32 rel = (s32)(link_stub - (site + 5));

	site[0] = 0xE8;		/* call rel32 */
	memcpy(site + 1, &rel, 4);
	site[5] = 0xCC;
}

static void link_site_jump(u8 *site, const u8 *code)
{
	const s32 rel = (s32)(code - (site + 5));

	site[0] = 0xE9;		/* jmp rel32 */
	memcpy(site + 1, &rel, 4);
	site[5] = 0xCC;
}

u32 ngen_Relink(RuntimeBlockInfo *block)
{
	int i;
	for (i = 0; i < 2; i++)
	{
		const RuntimeBlockInfo *to = i == 0 ? block->pBranchBlock : block->pNextBlock;
		u8 *const code = (u8 *)(uintptr_t)block->code;

		if (block->link_at[i] == 0)
			continue;
		if (to != NULL)
			link_site_jump(code + block->link_at[i], (const u8 *)(uintptr_t)to->code + to->linked_entry);
		else
			link_site_call_stub(code + block->link_at[i]);
	}
	return 0;
}

/* From the link stub: the block whose link site returns to @after is
 * leaving for @pc for the first time. As on x86-64 (ngen_link() there). */
#ifdef __GNUC__
__attribute__((used))
#endif
void * DYNACALL ngen_link(u8 *after, u32 pc)
{
	u8 *const site = after - 5;
	RuntimeBlockInfoPtr from = bm_GetBlock2(site);
	const bool mmu = mmu_enabled();
	u32 addr = pc;
	DynarecCodeEntryPtr code;
	RuntimeBlockInfo *block;
	RuntimeBlockInfoPtr to;
	u32 at;
	int which;

	if (mmu)
	{
		if (from == NULL || !rdv_MmuSamePlace(from, pc, &addr))
		{
			next_pc = pc;
			return (void *)block_return;
		}
		code = (DynarecCodeEntryPtr)(uintptr_t)p_sh4rcb->fpcb[(addr >> 1) & FPCB_MASK];
	}
	else
		code = bm_GetCodeByVAddr(pc);
	if (code == ngen_FailedToFindBlock)
	{
		code = rdv_FailedToFindBlock(pc);
		if (mmu && next_pc != pc)
			return (void *)block_return;
	}
	if (code == ngen_FailedToFindBlock)
	{
		next_pc = pc;
		return (void *)block_return;
	}

	if (from == NULL || bm_GetBlock2(site) != from)
		return (void *)(uintptr_t)code;

	block = from;
	at = (u32)(site - (u8 *)(uintptr_t)block->code);
	which = at == block->link_at[0] ? 0 : at == block->link_at[1] ? 1 : -1;
	to = bm_GetBlock(addr);

	if (which < 0 || pc != (which == 0 ? block->BranchBlock : block->NextBlock))
		return (void *)(uintptr_t)code;
	if (to == NULL || to->addr != addr || to->code != code)
		return (void *)(uintptr_t)code;
	if (mmu && to->vaddr != pc)
		return (void *)(uintptr_t)code;

	if (to->temp_block)
	{
		/* Not linked, ever: through the table from now on. jmp dword [address] */
		const u32 entry = (u32)(uintptr_t)&p_sh4rcb->fpcb[(addr >> 1) & FPCB_MASK];

		site[0] = 0xFF;
		site[1] = 0x25;
		memcpy(site + 2, &entry, 4);
		block->link_at[which] = 0;
		return (void *)(uintptr_t)code;
	}

	if (which == 0)
		block->pBranchBlock = to;
	else
		block->pNextBlock = to;
	bm_BlockAddRef(to, from);
	ngen_Relink(block);
	return (void *)(uintptr_t)code;
}

/* The main loop, as on x86-64 (ngen_mainloop there), and the routines that
 * go with it. Blocks are jumped to with the stack a multiple of 16, which
 * is what a call wants on Linux (and does no harm on Windows, which only
 * asks for 4), and with the address they are for in edx. */
static void gen_mainloop(void)
{
	x86_emit c;
	x86_label run_loop, slice_loop, ret, update, end;

	x86_init(&c, (u8 *)emit_GetCCPtr(), 1024);
	x86_label_init(&run_loop);
	x86_label_init(&slice_loop);
	x86_label_init(&ret);
	x86_label_init(&update);
	x86_label_init(&end);

	mainloop_code = c.p;
	x86_push(&c, X86_EBP);
	x86_push(&c, X86_EBX);
	x86_push(&c, X86_ESI);
	x86_push(&c, X86_EDI);
	/* (the caller's stack pointer, under the place it is aligned to) */
	x86_mov_rr(&c, X86_EAX, X86_ESP);
	x86_alu_ri(&c, false, X86_AND, X86_ESP, ~15u);
	x86_alu_ri(&c, false, X86_SUB, X86_ESP, 12);
	x86_push(&c, X86_EAX);
	x86_store_imm(&c, x86_abs(&cycle_counter), SH4_TIMESLICE);
	x86_store(&c, x86_abs(&jmp_esp), X86_ESP);

	x86_label_bind(&c, &run_loop);
	x86_alu_mi(&c, X86_CMP, x86_abs(&Sh4cntx.CpuRunning), 0);
	x86_jmp_label(&c, X86_CC_E, &end, true);

	x86_label_bind(&c, &slice_loop);
	x86_load(&c, X86_ECX, x86_abs(&next_pc));
	x86_call(&c, (uintptr_t)&bm_GetCodeByVAddr);
	x86_load(&c, X86_EDX, x86_abs(&next_pc));
	x64_jmp_r(&c, X86_EAX);

	x86_label_bind(&c, &ret);
	x86_alu_mi(&c, X86_CMP, x86_abs(&cycle_counter), 0);
	x86_jmp_label(&c, X86_CC_G, &slice_loop, false);

	x86_alu_mi(&c, X86_ADD, x86_abs(&cycle_counter), SH4_TIMESLICE);
	/* UpdateSystem_INTC(), here, when no event is due and no interrupt waits */
	x86_load(&c, X86_EDX, x86_abs(&Sh4cntx.sh4_sched_next));
	x86_alu_ri(&c, false, X86_SUB, X86_EDX, SH4_TIMESLICE);
	x86_jmp_label(&c, X86_CC_S, &update, false);
	x86_alu_mi(&c, X86_CMP, x86_abs(&Sh4cntx.interrupt_pend), 0);
	x86_jmp_label(&c, X86_CC_NE, &update, false);
	x86_store(&c, x86_abs(&Sh4cntx.sh4_sched_next), X86_EDX);
	x86_jmp_label(&c, -1, &run_loop, false);
	x86_label_bind(&c, &update);
	/* (the timers' time is the end of the slice here: see cycle_counter in sh4_if.h) */
	x86_store_imm(&c, x86_abs(&Sh4cntx.cycle_counter), SH4_TIMESLICE);
	x86_call(&c, (uintptr_t)&UpdateSystem_INTC);
	x86_jmp_label(&c, -1, &run_loop, false);

	x86_label_bind(&c, &end);
	x86_pop(&c, X86_ESP);
	x86_pop(&c, X86_EDI);
	x86_pop(&c, X86_ESI);
	x86_pop(&c, X86_EBX);
	x86_pop(&c, X86_EBP);
	x86_ret(&c);

	/* back from an exception: what the stack was as blocks run, and on */
	x64_align(&c, 16);
	unwind_stub = c.p;
	x86_load(&c, X86_ESP, x86_abs(&jmp_esp));
	x86_jmp_label(&c, -1, &run_loop, false);

	/* What the table of blocks holds for an address with no block yet: it
	 * is jumped to as a block is, with the address in edx, has the block
	 * compiled and goes back to the main loop, which finds it there. */
	x64_align(&c, 16);
	compile_stub = c.p;
	x86_mov_rr(&c, X86_ECX, X86_EDX);
	x86_call(&c, (uintptr_t)&rdv_FailedToFindBlock);
	x86_jmp_label(&c, -1, &ret, false);

	/* The link stub, called from a link site with the address the block is
	 * leaving for in edx: the site's return address and that address are
	 * ngen_link()'s arguments, and what it returns is jumped to. */
	x64_align(&c, 16);
	link_stub = c.p;
	x86_pop(&c, X86_ECX);
	x86_call(&c, (uintptr_t)&ngen_link);
	x64_jmp_r(&c, X86_EAX);

	verify(!c.full);
	block_return = ret.at;
	emit_Skip(((u32)(c.p - c.start) + 15) & ~15u);
}

void ngen_init(void)
{
	/* the routines for the general registers there can be, and the xmm ones */
	static const int gregs[] = { X86_EAX, X86_EDX, X86_EBX, X86_EBP, X86_ESI, X86_EDI };
	static const int fregs[] = { 0, 4, 5, 6, 7 };
	int kind, i;

	gen_mainloop();
	ngen_FailedToFindBlock = (void (*)(void))(uintptr_t)compile_stub;
	for (kind = 0; kind < MEM_KINDS; kind++)
	{
		if (kind == MEM_LOAD64 || kind == MEM_STORE64)
			mem_stub_make(kind, 0);
		else if (MEM_KIND_F(kind))
			for (i = 0; i < (int)(sizeof(fregs) / sizeof(fregs[0])); i++)
				mem_stub_make(kind, fregs[i]);
		else
			for (i = 0; i < (int)(sizeof(gregs) / sizeof(gregs[0])); i++)
			{
				const int data = gregs[i];
				/* (what is loaded is in eax or one of the block's, a byte stored in edx or ebx) */
				if (kind < MEM_STORE8 && data == X86_EDX)
					continue;
				if (kind >= MEM_STORE8 && data == X86_EAX)
					continue;
				if (kind == MEM_STORE8 && data != X86_EDX && data != X86_EBX)
					continue;
				mem_stub_make(kind, data);
			}
	}
	emit_SetBaseAddr();
}

void ngen_ResetBlocks(void)
{
}

void ngen_GetFeatures(ngen_features* dst)
{
	dst->InterpreterFallback = false;
	dst->OnlyDynamicEnds = false;
}

void DYNACALL rec_x86_blockcheckfail(u32 pc)
{
	rdv_BlockCheckFail(pc);
}

static void handle_mem_exception(u32 pc)
{
	/* (an odd pc: in a delay slot) */
	if (pc & 1)
		spc = pc - 1;
	else
		spc = pc;
	cycle_counter += CPU_RATIO * 2;	/* probably more is needed but no easy way to find out */
	rec_unwind();
}

/* The MMU's loads and stores: the address in ecx and what is stored in edx,
 * as for the others, and the pc of the instruction here, which a block
 * stores before the call when the MMU is on. */
static u32 noex_pc;

static u32 read_noex_paddr(u32 addr, u32 pc, u32 size)
{
	u32 paddr = 0;
#ifndef NO_MMU
	const u32 rv = mmu_data_translation_c(addr, &paddr, MMU_TT_DREAD, size);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(addr, rv, MMU_TT_DREAD);
		handle_mem_exception(pc);
	}
	mmu_lut_fill(addr, paddr, false);
#else
	(void)addr;
	(void)pc;
	(void)size;
#endif
	return paddr;
}

static u32 write_noex_paddr(u32 addr, u32 pc, u32 size)
{
	u32 paddr = 0;
#ifndef NO_MMU
	const u32 rv = mmu_data_translation_c(addr, &paddr, MMU_TT_DWRITE, size);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(addr, rv, MMU_TT_DWRITE);
		handle_mem_exception(pc);
	}
	mmu_lut_fill(addr, paddr, true);
#else
	(void)addr;
	(void)pc;
	(void)size;
#endif
	return paddr;
}

u8 DYNACALL rec_x86_read_noex8(u32 addr, u32 pc)
{
#ifndef NO_MMU
	return _vmem_ReadMem8(read_noex_paddr(addr, pc, 1));
#else
	return 0;
#endif
}

u16 DYNACALL rec_x86_read_noex16(u32 addr, u32 pc)
{
#ifndef NO_MMU
	return _vmem_ReadMem16(read_noex_paddr(addr, pc, 2));
#else
	return 0;
#endif
}

u32 DYNACALL rec_x86_read_noex32(u32 addr, u32 pc)
{
#ifndef NO_MMU
	return _vmem_ReadMem32(read_noex_paddr(addr, pc, 4));
#else
	return 0;
#endif
}

u64 DYNACALL rec_x86_read_noex64(u32 addr, u32 pc)
{
#ifndef NO_MMU
	/* (two accesses, each translated: mmu.h) */
	if (mmu_in_two_pages(addr))
	{
		const u32 low = rec_x86_read_noex32(addr, pc);
		return ((u64)rec_x86_read_noex32(addr + 4, pc) << 32) | low;
	}
	return _vmem_ReadMem64(read_noex_paddr(addr, pc, 8));
#else
	return 0;
#endif
}

static void write_noex(u32 addr, u64 data, u32 pc, u32 size)
{
#ifndef NO_MMU
	u32 paddr;
	if (size == 8 && mmu_in_two_pages(addr))
	{
		write_noex(addr, (u32)data, pc, 4);
		write_noex(addr + 4, (u32)(data >> 32), pc, 4);
		return;
	}
	paddr = write_noex_paddr(addr, pc, size);
	switch (size)
	{
	case 1: _vmem_WriteMem8(paddr, (u8)data); break;
	case 2: _vmem_WriteMem16(paddr, (u16)data); break;
	case 4: _vmem_WriteMem32(paddr, (u32)data); break;
	default: _vmem_WriteMem64(paddr, data); break;
	}
#else
	(void)addr;
	(void)data;
	(void)pc;
	(void)size;
#endif
}

void DYNACALL rec_x86_write_noex8(u32 addr, u32 data) { write_noex(addr, (u8)data, noex_pc, 1); }
void DYNACALL rec_x86_write_noex16(u32 addr, u32 data) { write_noex(addr, (u16)data, noex_pc, 2); }
void DYNACALL rec_x86_write_noex32(u32 addr, u32 data) { write_noex(addr, data, noex_pc, 4); }
void DYNACALL rec_x86_write_noex64(u32 addr, const u32 *data)
{
	write_noex(addr, ((u64)data[1] << 32) | data[0], noex_pc, 8);
}

static void handle_sh4_exception(SH4ThrownException *ex, u32 pc)
{
	/* (an odd pc: in a delay slot) */
	if (pc & 1)
	{
		AdjustDelaySlotException(ex);
		pc--;
	}
	Do_Exception(pc, ex->expEvn, ex->callVect);
	cycle_counter += CPU_RATIO * 4;	/* probably more is needed */
	rec_unwind();
}

/* An instruction the interpreter runs, with the MMU on: what raises an
 * exception is caught, and the catch over, before the main loop is gone
 * back to. The handler is set here before the call. */
static OpCallFP *fallback_oph;

void DYNACALL rec_x86_interpreter_fallback(u32 op, u32 pc)
{
	sh4_catch c;
	SH4_TRY(c)
	{
		fallback_oph(op);
		sh4_catch_pop(&c);
		return;
	}
	handle_sh4_exception(&c.ex, pc);
}

void DYNACALL rec_x86_do_sqw_mmu_no_ex(u32 addr, u32 pc)
{
	sh4_catch c;
	SH4_TRY(c)
	{
		do_sqw_mmu(addr);
		sh4_catch_pop(&c);
		return;
	}
	handle_sh4_exception(&c.ex, pc);
}

void DYNACALL rec_x86_do_sqw_nommu_local(u32 addr, u8* sqb)
{
	do_sqw_nommu(addr, sqb);
}

/* Whether the processor has FMA, found once (-1 until then; global, so
 * that tools/sh4/x86_test.c can have both compiled for) */
int rec_x86_cpu = -1;

static bool x86_has_fma(void)
{
	if (rec_x86_cpu < 0)
	{
		u32 a, b, c, d;
		int f = 0;
#if defined(_MSC_VER)
		int r[4];
		__cpuid(r, 1);
		a = (u32)r[0]; b = (u32)r[1]; c = (u32)r[2]; d = (u32)r[3];
#else
		__cpuid(1, a, b, c, d);
#endif
		(void)a;
		(void)b;
		(void)d;
		/* (as Xbyak had it: and the processor's state saved by the system) */
		if (c & (1u << 27))
		{
			u64 bv;
#if defined(_MSC_VER)
			bv = _xgetbv(0);
#else
			u32 lo, hi;
			__asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0));
			bv = ((u64)hi << 32) | lo;
#endif
			if ((bv & 6) == 6 && (c & (1u << 12)))
				f = 2;
		}
		rec_x86_cpu = f;
	}
	return (rec_x86_cpu & 2) != 0;
}

/* A label that many jumps go to (a block's check of its code) */
#define FAR_LABEL_JUMPS 1024
typedef struct
{
	u8 *at;
	u8 *patch[FAR_LABEL_JUMPS];
	int patches;
} x86_far_label;

/* An argument on the stack */
enum { ARG_IMM, ARG_REG, ARG_MEM, ARG_XMM };
typedef struct
{
	int kind;
	u32 value;		/* the constant, the register's number or the address */
} rec_arg;

static rec_arg arg_i(u32 v) { rec_arg a; a.kind = ARG_IMM; a.value = v; return a; }
static rec_arg arg_r(int reg) { rec_arg a; a.kind = ARG_REG; a.value = (u32)reg; return a; }
static rec_arg arg_m(const void *p) { rec_arg a; a.kind = ARG_MEM; a.value = (u32)(uintptr_t)p; return a; }
static rec_arg arg_x(int reg) { rec_arg a; a.kind = ARG_XMM; a.value = (u32)reg; return a; }

/* The block being compiled */
typedef struct
{
	x86_emit e;
	sh4_regalloc ra;
	size_t current_opid;
	x86_label exit_block;
	x86_label slice_out;
	x86_far_label check_failed;
	x86_label vaddr_failed;
	bool vaddr_checked;
	bool slice_out_used;
	u32 block_cycles;
	bool charge_at_tail;
	/* the canonical call's parameters (ngen_CC_Param()) */
	struct
	{
		CanonicalParamType type;
		const shil_param *prm;
	} cc_pars[8];
	int cc_count;
} x86_compiler;

static x86_compiler *comp;
/* where a float a canonical implementation returns is put on its way to an xmm register */
static f32 f32_rv;

static void far_jcc(x86_emit *e, int cc, x86_far_label *l)
{
	x86_db(e, 0x0F);
	x86_db(e, 0x80 | (u32)cc);
	if (l->patches == FAR_LABEL_JUMPS)
		e->full = true;
	else
		l->patch[l->patches++] = e->p;
	x86_dd(e, 0);
}

static void far_bind(x86_emit *e, x86_far_label *l)
{
	int i;
	l->at = e->p;
	if (e->full)
		return;
	for (i = 0; i < l->patches; i++)
	{
		const u32 v = (u32)(s32)(l->at - (l->patch[i] + 4));
		memcpy(l->patch[i], &v, 4);
	}
	l->patches = 0;
}

/* Something in the SH4's context, at its address */
static x86_mem ctx(const void *p)
{
	const ptrdiff_t at = (const u8*)p - (const u8*)&p_sh4rcb->cntx;
	verify(at >= 0 && at < (ptrdiff_t)sizeof(Sh4Context));
	(void)at;
	return x86_abs(p);
}

static int map_reg(x86_compiler *c, const shil_param *p)
{
	const int host = (int)(s8)sh4_regalloc_mapg(&c->ra, p);
	if (host == -1)
		die("Register not allocated");
	return host;
}

static int map_xreg(x86_compiler *c, const shil_param *p)
{
	const int host = (int)(s8)sh4_regalloc_mapf(&c->ra, p, 0);
	if (host == -1)
		die("VRegister not allocated");
	return host;
}

#define IS_ALLOCG(c, p) sh4_regalloc_is_allocg(&(c)->ra, (p))
#define IS_ALLOCF(c, p) sh4_regalloc_is_allocf(&(c)->ra, (p))
#define MAPG(c, p) map_reg(c, p)
#define MAPF(c, p) map_xreg(c, p)
#define SAME_G(c, a, b) (sh4_regalloc_mapg(&(c)->ra, (a)) == sh4_regalloc_mapg(&(c)->ra, (b)))
#define SAME_F(c, a, b) (sh4_regalloc_mapf(&(c)->ra, (a), 0) == sh4_regalloc_mapf(&(c)->ra, (b), 0))

/* A call, with the stack a multiple of 16 at it, @count arguments pushed
 * (@args the first one first) and taken off again after unless the
 * function does that (@callee_pops). Whatever goes in registers is there
 * already. The block's xmm registers in use are kept: no function has to
 * keep any of them on this processor. */
static void gen_call_raw(x86_compiler *c, uintptr_t function, const rec_arg *args, int count, bool callee_pops)
{
	x86_emit *e = &c->e;
	int saved[4];
	int saved_count = 0, i, pad, cleanup;

	if (c->current_opid != (size_t)-1)
		for (i = 4; i < 8; i++)
			if (sh4_regalloc_reg_used(&c->ra, (u32)i, true))
				saved[saved_count++] = i;
	if (saved_count != 0)
	{
		x86_alu_ri(e, false, X86_SUB, X86_ESP, 16);
		for (i = 0; i < saved_count; i++)
			x64_movd_mx(e, x86_m(X86_ESP, i * 4), saved[i]);
	}
	pad = (16 - (count * 4) % 16) % 16;
	if (pad != 0)
		x86_alu_ri(e, false, X86_SUB, X86_ESP, (u32)pad);
	for (i = count; i-- > 0;)
	{
		switch (args[i].kind)
		{
		case ARG_IMM:
			if (x86_is8((s32)args[i].value))
			{
				x86_db(e, 0x6A);
				x86_db(e, args[i].value);
			}
			else
			{
				x86_db(e, 0x68);
				x86_dd(e, args[i].value);
			}
			break;
		case ARG_REG:
			x86_push(e, (int)args[i].value);
			break;
		case ARG_MEM:
			x86_op_m(e, false, 0xFF, -1, 6, x86_abs((const void *)(uintptr_t)args[i].value), 0, false);
			break;
		case ARG_XMM:
			x86_alu_ri(e, false, X86_SUB, X86_ESP, 4);
			x64_movss_store(e, x86_m(X86_ESP, 0), (int)args[i].value);
			break;
		}
	}
	x86_call(e, function);
	cleanup = pad + (callee_pops ? 0 : count * 4);
	if (cleanup != 0)
		x86_alu_ri(e, false, X86_ADD, X86_ESP, (u32)cleanup);
	if (saved_count != 0)
	{
		for (i = 0; i < saved_count; i++)
			x64_movd_xm(e, saved[i], x86_m(X86_ESP, i * 4));
		x86_alu_ri(e, false, X86_ADD, X86_ESP, 16);
	}
}

/* A fastcall function, whose arguments are all in ecx and edx already */
#define gen_call(c, f) gen_call_raw(c, (uintptr_t)(f), NULL, 0, true)
/* A plain C function, its arguments given */
#define gen_call_c(c, f, args, n) gen_call_raw(c, (uintptr_t)(f), args, n, false)

/* A parameter into a general register (@xmm false) or an xmm one; uses
 * eax for a constant into a floating-point register */
static void param_to_reg(x86_compiler *c, const shil_param *param, int reg, bool xmm)
{
	x86_emit *e = &c->e;
	if (shil_is_imm(param))
	{
		if (!xmm)
			x86_mov_ri(e, reg, param->_imm);
		else
		{
			x86_mov_ri(e, X86_EAX, param->_imm);
			x64_movd_xr(e, reg, X86_EAX);
		}
	}
	else if (shil_is_reg(param))
	{
		if (shil_is_r32f(param))
		{
			if (IS_ALLOCF(c, param))
			{
				const int sreg = MAPF(c, param);
				if (!xmm)
					x64_movd_rx(e, reg, sreg);
				else if (reg != sreg)
					x64_movss_rr(e, reg, sreg);
			}
			else
			{
				if (!xmm)
					x86_load(e, reg, ctx(shil_reg_ptr(param)));
				else
					x64_movss_load(e, reg, ctx(shil_reg_ptr(param)));
			}
		}
		else
		{
			if (IS_ALLOCG(c, param))
			{
				const int sreg = MAPG(c, param);
				if (xmm)
					x64_movd_xr(e, reg, sreg);
				else if (reg != sreg)
					x86_mov_rr(e, reg, sreg);
			}
			else
			{
				if (!xmm)
					x86_load(e, reg, ctx(shil_reg_ptr(param)));
				else
					x64_movss_load(e, reg, ctx(shil_reg_ptr(param)));
			}
		}
	}
	else
	{
		verify(shil_is_null(param));
	}
}

static void reg_to_param(x86_compiler *c, const shil_param *param, int reg, bool xmm)
{
	x86_emit *e = &c->e;
	if (IS_ALLOCG(c, param))
	{
		const int sreg = MAPG(c, param);
		if (!xmm)
			x86_mov_rr(e, sreg, reg);
		else if (reg != sreg)
			x64_movd_rx(e, sreg, reg);
	}
	else if (IS_ALLOCF(c, param))
	{
		const int sreg = MAPF(c, param);
		if (!xmm)
			x64_movd_xr(e, sreg, reg);
		else if (reg != sreg)
			x64_movss_rr(e, sreg, reg);
	}
	else
	{
		if (!xmm)
			x86_store(e, ctx(shil_reg_ptr(param)), reg);
		else
			x64_movss_store(e, ctx(shil_reg_ptr(param)), reg);
	}
}

/* Takes what the block costs off the cycle counter. (See x86-64.) */
static void gen_charge(x86_compiler *c)
{
	x86_alu_mi(&c->e, X86_SUB, x86_abs(&cycle_counter), c->block_cycles);
}

/* To the main loop if the time slice is used up, by way of slice_out; otherwise on */
static void gen_slice_check(x86_compiler *c)
{
	if (c->charge_at_tail)
		gen_charge(c);		/* leaves the flags of the counter against 0 */
	else
		x86_alu_mi(&c->e, X86_CMP, x86_abs(&cycle_counter), 0);
	x86_jmp_label(&c->e, X86_CC_LE, &c->slice_out, true);
	c->slice_out_used = true;
}

/* What is left of the time slice, where the timers find it, before a call
 * that can reach an on-chip register: see cycle_counter in sh4_if.h. Uses
 * eax. */
static void gen_slice_left(x86_compiler *c)
{
	x86_load(&c->e, X86_EAX, x86_abs(&cycle_counter));
	x86_store(&c->e, ctx(&Sh4cntx.cycle_counter), X86_EAX);
}

/* A block going back: see wait_site.h */
static void gen_wait_check(x86_compiler *c, const RuntimeBlockInfo *block)
{
	x86_emit *e = &c->e;
	WaitSite *site = sh4_wait_site(block);
	x86_label over;
	rec_arg args[2];
	if (site == NULL)
		return;
	x86_label_init(&over);

	x64_dec_m(e, x86_abs(&site->skip));
	x86_jmp_label(e, X86_CC_NS, &over, true);
	args[0] = arg_i((u32)(uintptr_t)site);
	args[1] = arg_m(&cycle_counter);
	gen_call_c(c, &sh4_wait_check, args, 2);
	x86_store(e, x86_abs(&cycle_counter), X86_EAX);
	x86_label_bind(e, &over);
}

/* On to the block at @target: by a link site (see RuntimeBlockInfo::link_at), which is
 * the block's way out to its BranchBlock (@which 0) or its NextBlock
 * (1). As on x86-64. */
static void gen_go_on(x86_compiler *c, RuntimeBlockInfo *block, int which)
{
	x86_emit *e = &c->e;
	const u32 target = which == 0 ? block->BranchBlock : block->NextBlock;
	u32 place = target;

	x86_mov_ri(e, X86_EDX, target);
	if (mmu_enabled())
	{
		x86_store(e, ctx(&next_pc), X86_EDX);
		rdv_MmuSamePlace(block, target, &place);
	}
	gen_slice_check(c);
	if (block->temp_block || target == block->vaddr || link_stub == NULL)
	{
		x86_jmp_m(e, x86_abs(&p_sh4rcb->fpcb[(place >> 1) & FPCB_MASK]));
		return;
	}
	block->link_at[which] = (u32)(e->p - e->start);
	x86_call(e, (uintptr_t)link_stub);
	x86_db(e, 0xCC);
}

/* With the MMU on an access is a call. First, though, the table of
 * translations (mmu.h): on a hit, eax is what to add to the address to
 * be at the page on the host. The address is in ecx. As on x86-64. */
static bool gen_mmu_lookup(x86_compiler *c, const uintptr_t *table, u32 size, x86_label *miss)
{
#ifdef MMU_HOST_PAGE_LUT
	x86_emit *e = &c->e;
	if (!mmu_enabled())
		return false;
	if (size == 2 || size == 4 || size == 8)
	{
		x86_test_ri(e, X86_ECX, size == 2 ? 1 : 3);
		x86_jmp_label(e, X86_CC_NE, miss, true);
	}
	if (size == 8)
	{
		x64_lea(e, false, X86_EAX, x86_m(X86_ECX, 4));
		x86_test_ri(e, X86_EAX, 0xFFF);
		x86_jmp_label(e, X86_CC_E, miss, true);
	}
	x86_mov_rr(e, X86_EAX, X86_ECX);
	x86_shift_ri(e, X86_SHR, X86_EAX, 12);
	x86_load(e, X86_EAX, x86_mi(-1, X86_EAX, 4, (s32)(u32)(uintptr_t)table));
	x86_test_rr(e, X86_EAX, X86_EAX);
	x86_jmp_label(e, X86_CC_E, miss, true);
	return true;
#else
	(void)c;
	(void)table;
	(void)size;
	(void)miss;
	return false;
#endif
}

static u32 op_pc(const RuntimeBlockInfo *block, const shil_opcode *op)
{
	return block->vaddr + op->guest_offs - (op->delay_slot ? 1 : 0);
}

/* A load by asking, the address in ecx: what is loaded comes back in eax,
 * and 64 bits in edx and eax. */
static void gen_read_memory_slow(x86_compiler *c, const shil_opcode *op, RuntimeBlockInfo *block)
{
	x86_emit *e = &c->e;
	const u32 size = op->flags & 0x7f;
	x86_label lut_miss, lut_done;
	bool lut;
	x86_label_init(&lut_miss);
	x86_label_init(&lut_done);
#ifdef MMU_HOST_PAGE_LUT
	lut = gen_mmu_lookup(c, mmu_read_lut, size, &lut_miss);
#else
	lut = false;
#endif
	if (lut)
	{
		switch (size)
		{
		case 1:
			x64_movsx8_m(e, X86_EAX, x86_mi(X86_EAX, X86_ECX, 1, 0));
			break;
		case 2:
			x86_movsx16(e, X86_EAX, x86_mi(X86_EAX, X86_ECX, 1, 0));
			break;
		case 8:
			x86_load(e, X86_EDX, x86_mi(X86_EAX, X86_ECX, 1, 4));
			x86_load(e, X86_EAX, x86_mi(X86_EAX, X86_ECX, 1, 0));
			break;
		default:
			x86_load(e, X86_EAX, x86_mi(X86_EAX, X86_ECX, 1, 0));
			break;
		}
		x86_jmp_label(e, -1, &lut_done, true);
		x86_label_bind(e, &lut_miss);
	}
	gen_slice_left(c);
	if (mmu_enabled())
		x86_mov_ri(e, X86_EDX, op_pc(block, op));	/* pc */

	switch (size)
	{
	case 1:
		if (!mmu_enabled())
			gen_call(c, ReadMem8);
		else
			gen_call(c, &rec_x86_read_noex8);
		x64_movsx8_rr(e, X86_EAX, X86_EAX);
		break;
	case 2:
		if (!mmu_enabled())
			gen_call(c, ReadMem16);
		else
			gen_call(c, &rec_x86_read_noex16);
		x64_movsx16_rr(e, X86_EAX, X86_EAX);
		break;
	case 4:
		if (!mmu_enabled())
			gen_call(c, ReadMem32);
		else
			gen_call(c, &rec_x86_read_noex32);
		break;
	case 8:
		if (!mmu_enabled())
			gen_call(c, ReadMem64);
		else
			gen_call(c, &rec_x86_read_noex64);
		break;
	default:
		die("1..8 bytes");
	}
	if (lut)
		x86_label_bind(e, &lut_done);
}

/* A store by asking, the address in ecx and the data in edx - or for 64
 * bits, where they are. */
static void gen_write_memory_slow(x86_compiler *c, const shil_opcode *op, RuntimeBlockInfo *block)
{
	x86_emit *e = &c->e;
	const u32 size = op->flags & 0x7f;
	x86_label lut_miss, lut_done;
	bool lut;
	x86_label_init(&lut_miss);
	x86_label_init(&lut_done);
#ifdef MMU_HOST_PAGE_LUT
	lut = gen_mmu_lookup(c, mmu_write_lut, size, &lut_miss);
#else
	lut = false;
#endif
	if (lut)
	{
		const x86_mem at = x86_mi(X86_EAX, X86_ECX, 1, 0);
		switch (size)
		{
		case 1:
			x86_store8(e, at, X86_EDX);
			break;
		case 2:
			x64_store16(e, at, X86_EDX);
			break;
		case 8:
			x64_movq_load(e, 0, x86_m(X86_EDX, 0));
			x64_movq_store(e, at, 0);
			break;
		default:
			x86_store(e, at, X86_EDX);
			break;
		}
		x86_jmp_label(e, -1, &lut_done, true);
		x86_label_bind(e, &lut_miss);
	}
	gen_slice_left(c);
	if (mmu_enabled())
		x86_store_imm(e, x86_abs(&noex_pc), op_pc(block, op));	/* pc */

	switch (size)
	{
	case 1:
		if (!mmu_enabled())
			gen_call(c, WriteMem8);
		else
			gen_call(c, &rec_x86_write_noex8);
		break;
	case 2:
		if (!mmu_enabled())
			gen_call(c, WriteMem16);
		else
			gen_call(c, &rec_x86_write_noex16);
		break;
	case 4:
		if (!mmu_enabled())
			gen_call(c, WriteMem32);
		else
			gen_call(c, &rec_x86_write_noex32);
		break;
	case 8:
		if (!mmu_enabled())
			gen_call(c, &rec_x86_mem_store64);
		else
			gen_call(c, &rec_x86_write_noex64);
		break;
	default:
		die("1..8 bytes");
	}
	if (lut)
		x86_label_bind(e, &lut_done);
}

/* A store queue written out, its address in ecx */
static void gen_store_queue(x86_compiler *c, const shil_opcode *op, RuntimeBlockInfo *block)
{
	if (mmu_enabled())
	{
		x86_mov_ri(&c->e, X86_EDX, op_pc(block, op));	/* pc */
		gen_call(c, &rec_x86_do_sqw_mmu_no_ex);
	}
	else if (CCN_MMUCR.AT == 1)
		gen_call(c, &do_sqw_mmu);
	else
	{
		x86_mov_ri(&c->e, X86_EDX, (u32)(uintptr_t)sq_both);
		gen_call(c, &rec_x86_do_sqw_nommu_local);
	}
}

/* Whether a load or store can be one move from or to the guest's memory
 * as the host has it mapped: with the MMU off, and the 512 MB layout
 * there, the address taken down to it (in P0 to P3 every area is where
 * it is in P0: what is not memory there faults, and is rewritten). */
static bool fast_memory(void)
{
	return !mmu_enabled() && _nvmem_enabled() && !_nvmem_4gb_space();
}

/* Whether what is at @code is a fast access as gen_fast_access() writes
 * them, and if so which kind, with which register, and how long it is: a
 * move to or from [ecx + where the memory is], which nothing else a block
 * has. */
static bool fast_access_at(const u8 *code, int *kind, int *data, int *length)
{
	const u8 *p = code;
	u8 prefix = 0, modrm;
	u32 distance;

	if (*p == 0x66 || *p == 0xF3)
		prefix = *p++;
	if (p[0] == 0x0F)
	{
		if (p[1] == 0xBE && prefix == 0)
			*kind = MEM_LOAD8;
		else if (p[1] == 0xBF && prefix == 0)
			*kind = MEM_LOAD16;
		else if (p[1] == 0x10 && prefix == 0xF3)
			*kind = MEM_LOAD32F;
		else if (p[1] == 0x11 && prefix == 0xF3)
			*kind = MEM_STORE32F;
		else if (p[1] == 0x7E && prefix == 0xF3)
			*kind = MEM_LOAD64;
		else if (p[1] == 0xD6 && prefix == 0x66)
			*kind = MEM_STORE64;
		else
			return false;
		p += 2;
	}
	else if (*p == 0x8B && prefix == 0)
	{
		*kind = MEM_LOAD32;
		p++;
	}
	else if (*p == 0x88 && prefix == 0)
	{
		*kind = MEM_STORE8;
		p++;
	}
	else if (*p == 0x89 && prefix != 0xF3)
	{
		*kind = prefix != 0 ? MEM_STORE16 : MEM_STORE32;
		p++;
	}
	else
		return false;

	/* [ecx + a distance] */
	modrm = *p++;
	if ((modrm & 0xC7) != 0x81)
		return false;
	memcpy(&distance, p, 4);
	if (distance != (u32)(uintptr_t)virt_ram_base)
		return false;
	p += 4;

	*data = (modrm >> 3) & 7;
	if ((*kind == MEM_LOAD64 || *kind == MEM_STORE64) && *data != 0)
		return false;
	*length = (int)(p - code);
	return true;
}

/* A fast access of @kind, the SH4's address in eax: the data in, or wanted
 * in, register @data - a general one, or an xmm one for the kinds that
 * are. */
static void gen_fast_access(x86_compiler *c, int kind, int data)
{
	x86_emit *e = &c->e;
	const u8 *start_addr;
	x86_mem at;
	int k, d, length;

	x86_mov_rr(e, X86_ECX, X86_EAX);
	x86_alu_ri(e, false, X86_AND, X86_ECX, 0x1FFFFFFF);
	start_addr = e->p;
	at = x86_m(X86_ECX, (s32)(u32)(uintptr_t)virt_ram_base);

	switch (kind)
	{
	case MEM_LOAD8:    x64_movsx8_m(e, data, at); break;
	case MEM_LOAD16:   x86_movsx16(e, data, at); break;
	case MEM_LOAD32:   x86_load(e, data, at); break;
	case MEM_LOAD32F:  x64_movss_load(e, data, at); break;
	case MEM_LOAD64:   x64_movq_load(e, data, at); break;
	case MEM_STORE8:   x86_store8(e, at, data); break;
	case MEM_STORE16:  x64_store16(e, at, data); break;
	case MEM_STORE32:  x86_store(e, at, data); break;
	case MEM_STORE32F: x64_movss_store(e, at, data); break;
	case MEM_STORE64:  x64_movq_store(e, at, data); break;
	default:           die("no such access");
	}
	/* ngen_Rewrite() has to know it again, and needs five bytes for its call */
	verify(fast_access_at(start_addr, &k, &d, &length) && k == kind && d == data
			&& length == (int)(e->p - start_addr) && length >= 5 && mem_stubs[kind][data] != NULL);
	(void)start_addr;
	(void)k;
	(void)d;
	(void)length;
}

/* The address of a load or store, into @reg */
static void gen_mem_addr(x86_compiler *c, const shil_opcode *op, int reg)
{
	x86_emit *e = &c->e;
	if (!shil_is_null(&op->rs3) && shil_is_reg(&op->rs1) && IS_ALLOCG(c, &op->rs1))
	{
		/* one instruction for the two of them, where both are to hand */
		const int base = MAPG(c, &op->rs1);

		if (shil_is_imm(&op->rs3))
		{
			x64_lea(e, false, reg, x86_m(base, (s32)op->rs3._imm));
			return;
		}
		if (IS_ALLOCG(c, &op->rs3))
		{
			x64_lea(e, false, reg, x86_mi(base, MAPG(c, &op->rs3), 1, 0));
			return;
		}
	}
	param_to_reg(c, &op->rs1, reg, false);
	if (!shil_is_null(&op->rs3))
	{
		if (shil_is_imm(&op->rs3))
			x86_alu_ri(e, false, X86_ADD, reg, op->rs3._imm);
		else if (IS_ALLOCG(c, &op->rs3))
			x86_add_rr(e, reg, MAPG(c, &op->rs3));
		else
			x86_add_rm(e, reg, ctx(shil_reg_ptr(&op->rs3)));
	}
}

/* A load as one instruction, from the SH4's memory as the host has it
 * mapped straight into the register the block wants it in. False if that
 * is not to be had (fast_memory()), and nothing written. */
static bool gen_read_memory_fast(x86_compiler *c, const shil_opcode *op)
{
	const u32 size = op->flags & 0x7f;
	int kind;

	if (!fast_memory())
		return false;
	gen_mem_addr(c, op, X86_EAX);

	if (size == 8)
	{
		gen_fast_access(c, MEM_LOAD64, 0);
		x64_movq_store(&c->e, x86_abs(shil_reg_ptr(&op->rd)), 0);
		return true;
	}
	kind = size == 1 ? MEM_LOAD8 : size == 2 ? MEM_LOAD16 : MEM_LOAD32;
	if (IS_ALLOCG(c, &op->rd))
		gen_fast_access(c, kind, MAPG(c, &op->rd));
	else if (size == 4 && IS_ALLOCF(c, &op->rd))
		gen_fast_access(c, MEM_LOAD32F, MAPF(c, &op->rd));
	else
	{
		gen_fast_access(c, kind, X86_EAX);
		reg_to_param(c, &op->rd, X86_EAX, false);
	}
	return true;
}

/* A store, likewise */
static bool gen_write_memory_fast(x86_compiler *c, const shil_opcode *op)
{
	const u32 size = op->flags & 0x7f;
	int kind;

	if (!fast_memory())
		return false;
	gen_mem_addr(c, op, X86_EAX);

	if (size == 8)
	{
		x64_movq_load(&c->e, 0, x86_abs(shil_reg_ptr(&op->rs2)));
		gen_fast_access(c, MEM_STORE64, 0);
		return true;
	}
	kind = size == 1 ? MEM_STORE8 : size == 2 ? MEM_STORE16 : MEM_STORE32;
	if (shil_is_reg(&op->rs2) && IS_ALLOCG(c, &op->rs2)
			&& (kind != MEM_STORE8 || MAPG(c, &op->rs2) == X86_EBX))
		gen_fast_access(c, kind, MAPG(c, &op->rs2));
	else if (size == 4 && shil_is_reg(&op->rs2) && IS_ALLOCF(c, &op->rs2))
		gen_fast_access(c, MEM_STORE32F, MAPF(c, &op->rs2));
	else
	{
		param_to_reg(c, &op->rs2, X86_EDX, false);
		gen_fast_access(c, kind, X86_EDX);
	}
	return true;
}

static void check_block(x86_compiler *c, RuntimeBlockInfo* block)
{
	x86_emit *e = &c->e;
	s32 sz = (s32)block->sh4_code_size;
	u32 sa = block->addr;

	/* (ecx is the block's address at check_failed) */
	x86_mov_ri(e, X86_ECX, block->addr);

	/* FIXME This test shouldn't be necessary: see x86-64 */
	if (mmu_enabled())
	{
		x86_alu_mi(e, X86_CMP, x86_abs(&next_pc), block->vaddr);
		far_jcc(e, X86_CC_NE, &c->check_failed);
	}

	while (sz > 0)
	{
		void* ptr = (void*)GetMemPtr(sa, sz > 4 ? 4 : sz);
		if (ptr)
		{
			const u32 at = (u32)(uintptr_t)ptr;

			if (sz >= 4 && !(at & 3))
			{
				u32 v;
				memcpy(&v, ptr, 4);
				x86_alu_mi(e, X86_CMP, x86_abs(ptr), v);
				sz -= 4;
				sa += 4;
			}
			else
			{
				/* (a 16-bit constant, signed: one byte where it will do) */
				s16 v;
				memcpy(&v, ptr, 2);
				x86_db(e, 0x66);
				if (x86_is8(v))
				{
					x86_op_m(e, false, 0x83, -1, X86_CMP, x86_abs(ptr), 1, false);
					x86_db(e, (u32)(s32)v);
				}
				else
				{
					x86_op_m(e, false, 0x81, -1, X86_CMP, x86_abs(ptr), 2, false);
					x86_db(e, (u32)(u16)v);
					x86_db(e, (u32)(u16)v >> 8);
				}
				sz -= 2;
				sa += 2;
			}
			far_jcc(e, X86_CC_NE, &c->check_failed);
		}
		else
			break;
	}
}

/* The translation of an address known when the block is compiled, with
 * the MMU on: false where it is not to be done here */
static bool translate_const(const RuntimeBlockInfo *block, u32 *addr, u32 size, u32 tt)
{
	u32 paddr, rv;
	if (!(mmu_enabled() && mmu_is_translated_c(*addr, size, tt)))
		return true;
	/* When full mmu is on, only consider addresses in the same 4k page */
	if ((*addr >> 12) != (block->vaddr >> 12))
		return false;
	switch (size)
	{
	case 1:
	case 2:
		rv = mmu_data_translation_c(*addr, &paddr, tt, size);
		break;
	case 4:
	case 8:
		rv = mmu_data_translation_c(*addr, &paddr, tt, 4);
		break;
	default:
		die("Invalid immediate size");
		return false;
	}
	if (rv != MMU_ERROR_NONE)
		return false;
	*addr = paddr;
	return true;
}

static bool gen_read_mem_immediate(x86_compiler *c, const shil_opcode *op, RuntimeBlockInfo *block)
{
	x86_emit *e = &c->e;
	u32 size, addr;
	bool isram = false;
	void *ptr;

	if (!shil_is_imm(&op->rs1))
		return false;
	size = op->flags & 0x7f;
	addr = op->rs1._imm;
	if (!translate_const(block, &addr, size, MMU_TT_DREAD))
		return false;
	ptr = _vmem_read_ptr(addr, &isram, size > 4 ? 4 : size);
	if (!isram && size <= 4)
	{
		/* a system bus register that is only storage is read as memory */
		const void *reg = area0_read_const(ptr, addr, size);
		if (reg != NULL)
		{
			ptr = (void *)reg;
			isram = true;
		}
	}

	if (isram)
	{
		const x86_mem at = x86_abs(ptr);
		switch (size)
		{
		case 1:
			if (IS_ALLOCG(c, &op->rd))
				x64_movsx8_m(e, MAPG(c, &op->rd), at);
			else
			{
				x64_movsx8_m(e, X86_EAX, at);
				x86_store(e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);
			}
			break;

		case 2:
			if (IS_ALLOCG(c, &op->rd))
				x86_movsx16(e, MAPG(c, &op->rd), at);
			else
			{
				x86_movsx16(e, X86_EAX, at);
				x86_store(e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);
			}
			break;

		case 4:
			if (IS_ALLOCG(c, &op->rd))
				x86_load(e, MAPG(c, &op->rd), at);
			else if (IS_ALLOCF(c, &op->rd))
				x64_movd_xm(e, MAPF(c, &op->rd), at);
			else
			{
				x86_load(e, X86_EAX, at);
				x86_store(e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);
			}
			break;

		case 8:
			x64_movq_load(e, 0, at);
			x64_movq_store(e, x86_abs(shil_reg_ptr(&op->rd)), 0);
			break;

		default:
			die("Invalid immediate size");
			break;
		}
	}
	else
	{
		/* Not RAM: the returned pointer is a memory handler */
		if (sh4_area7(addr))
			gen_slice_left(c);
		if (size == 8)
		{
			/* Need to call the handler twice */
			x86_mov_ri(e, X86_ECX, addr);
			gen_call(c, ptr);
			x86_store(e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);

			x86_mov_ri(e, X86_ECX, addr + 4);
			gen_call(c, ptr);
			x86_store(e, ctx(shil_reg_ptr(&op->rd) + 1), X86_EAX);
		}
		else
		{
			x86_mov_ri(e, X86_ECX, addr);
			gen_call(c, ptr);
			switch (size)
			{
			case 1:
				x64_movsx8_rr(e, X86_EAX, X86_EAX);
				break;
			case 2:
				x64_movsx16_rr(e, X86_EAX, X86_EAX);
				break;
			case 4:
				break;
			default:
				die("Invalid immediate size");
				break;
			}
			reg_to_param(c, &op->rd, X86_EAX, false);
		}
	}

	return true;
}

static bool gen_write_mem_immediate(x86_compiler *c, const shil_opcode *op, RuntimeBlockInfo *block)
{
	x86_emit *e = &c->e;
	u32 size, addr;
	bool isram = false;
	void *ptr;

	if (!shil_is_imm(&op->rs1))
		return false;
	size = op->flags & 0x7f;
	addr = op->rs1._imm;
	if (!translate_const(block, &addr, size, MMU_TT_DWRITE))
		return false;
	ptr = _vmem_write_ptr(addr, &isram, size > 4 ? 4 : size);

	if (isram)
	{
		const x86_mem at = x86_abs(ptr);
		switch (size)
		{
		case 1:
			if (IS_ALLOCG(c, &op->rs2) && MAPG(c, &op->rs2) == X86_EBX)
				x86_store8(e, at, X86_EBX);
			else if (shil_is_imm(&op->rs2))
				x64_store8_imm(e, at, (u8)op->rs2._imm);
			else
			{
				param_to_reg(c, &op->rs2, X86_ECX, false);
				x86_store8(e, at, X86_ECX);
			}
			break;

		case 2:
			if (IS_ALLOCG(c, &op->rs2))
				x64_store16(e, at, MAPG(c, &op->rs2));
			else if (shil_is_imm(&op->rs2))
				x64_store16_imm(e, at, (u16)op->rs2._imm);
			else
			{
				x86_load(e, X86_ECX, ctx(shil_reg_ptr(&op->rs2)));
				x64_store16(e, at, X86_ECX);
			}
			break;

		case 4:
			if (IS_ALLOCG(c, &op->rs2))
				x86_store(e, at, MAPG(c, &op->rs2));
			else if (IS_ALLOCF(c, &op->rs2))
				x64_movd_mx(e, at, MAPF(c, &op->rs2));
			else if (shil_is_imm(&op->rs2))
				x86_store_imm(e, at, op->rs2._imm);
			else
			{
				x86_load(e, X86_ECX, ctx(shil_reg_ptr(&op->rs2)));
				x86_store(e, at, X86_ECX);
			}
			break;

		case 8:
			x64_movq_load(e, 0, x86_abs(shil_reg_ptr(&op->rs2)));
			x64_movq_store(e, at, 0);
			break;

		default:
			die("Invalid immediate size");
			break;
		}
	}
	else
	{
		/* Not RAM: the returned pointer is a memory handler */
		if (sh4_area7(addr))
			gen_slice_left(c);
		if (size == 8)
		{
			/* a 32-bit handler: called for each half, as for a read */
			x86_mov_ri(e, X86_ECX, addr);
			x86_load(e, X86_EDX, ctx(shil_reg_ptr(&op->rs2)));
			gen_call(c, ptr);

			x86_mov_ri(e, X86_ECX, addr + 4);
			x86_load(e, X86_EDX, ctx(shil_reg_ptr(&op->rs2) + 1));
			gen_call(c, ptr);
		}
		else
		{
			x86_mov_ri(e, X86_ECX, addr);
			param_to_reg(c, &op->rs2, X86_EDX, false);
			gen_call(c, ptr);
		}
	}

	return true;
}

static void gen_binary_op(x86_compiler *c, const shil_opcode *op, int alu)
{
	x86_emit *e = &c->e;
	const int rd = MAPG(c, &op->rd);
	const shil_param *rs2 = &op->rs2;
	if (!SAME_G(c, &op->rd, &op->rs1))
	{
		if (shil_is_reg(&op->rs2) && SAME_G(c, &op->rd, &op->rs2))
		{
			if (op->op == shop_sub)
			{
				/* This op isn't commutative */
				x86_neg(e, rd);
				x86_alu_rr(e, X86_ADD, rd, MAPG(c, &op->rs1));
				return;
			}
			/* otherwise just swap the operands */
			rs2 = &op->rs1;
		}
		else
			x86_mov_rr(e, rd, MAPG(c, &op->rs1));
	}
	if (shil_is_imm(&op->rs2))
		/* The constant goes in the instruction, not through a register */
		x86_alu_ri(e, false, alu, rd, op->rs2._imm);
	else
		x86_alu_rr(e, alu, rd, MAPG(c, rs2));
}

/* A NaN comes out as the one NaN an ARM host gives, 0x7fc00000: see
 * x86-64. */
static void gen_nan_fix(x86_compiler *c, int rd)
{
	x86_emit *e = &c->e;
	x86_label ok;
	x86_label_init(&ok);
	x64_ucomiss(e, rd, rd);
	x86_jmp_label(e, 0xB /* NP */, &ok, false);
	x86_mov_ri(e, X86_EAX, 0x7fc00000);
	x64_movd_xr(e, rd, X86_EAX);
	x86_label_bind(e, &ok);
}

/* The same for FTRV's four results, in xmm0. Uses xmm1 and xmm2. */
static void gen_nan_fix4(x86_compiler *c)
{
	x86_emit *e = &c->e;
	x86_label ok;
	x86_label_init(&ok);
	x64_movaps_rr(e, 1, 0);
	x64_cmpunordps(e, 1, 1);		/* all ones where a NaN is */
	x64_movmskps(e, X86_EAX, 1);
	x86_test_rr(e, X86_EAX, X86_EAX);
	x86_jmp_label(e, X86_CC_E, &ok, false);
	x86_mov_ri(e, X86_EAX, 0x7fc00000);
	x64_movd_xr(e, 2, X86_EAX);
	x64_shufps(e, 2, 2, 0);
	x64_andps(e, 2, 1);
	x64_andnps(e, 1, 0);
	x64_orps(e, 1, 2);
	x64_movaps_rr(e, 0, 1);
	x86_label_bind(e, &ok);
}

static void gen_binary_fop(x86_compiler *c, const shil_opcode *op, u32 natop)
{
	x86_emit *e = &c->e;
	const int rd = MAPF(c, &op->rd);
	const shil_param *rs2 = &op->rs2;
	if (!SAME_F(c, &op->rd, &op->rs1))
	{
		if (shil_is_reg(&op->rs2) && SAME_F(c, &op->rd, &op->rs2))
		{
			if (op->op == shop_fsub || op->op == shop_fdiv)
			{
				/* these ops aren't commutative so we need a scratch reg */
				x64_movss_rr(e, 0, MAPF(c, &op->rs2));
				x64_movss_rr(e, rd, MAPF(c, &op->rs1));
				x64_ss_rr(e, natop, rd, 0);
				return;
			}
			/* otherwise just swap the operands */
			rs2 = &op->rs1;
		}
		else
			x64_movss_rr(e, rd, MAPF(c, &op->rs1));
	}
	if (shil_is_imm(&op->rs2))
	{
		x86_mov_ri(e, X86_EAX, op->rs2._imm);
		x64_movd_xr(e, 0, X86_EAX);
		x64_ss_rr(e, natop, rd, 0);
	}
	else
		x64_ss_rr(e, natop, rd, MAPF(c, rs2));
}

/* the destination given the first source, if it is not in it already */
static void mov_rd_rs1(x86_compiler *c, const shil_opcode *op)
{
	if (!SAME_G(c, &op->rd, &op->rs1))
		x86_mov_rr(&c->e, MAPG(c, &op->rd), MAPG(c, &op->rs1));
}

/* the destination given the first source, a constant or a register */
static void rd_from_rs1(x86_compiler *c, const shil_opcode *op, int rd)
{
	if (shil_is_imm(&op->rs1))
		x86_mov_ri(&c->e, rd, shil_imm_value(&op->rs1));
	else if (!SAME_G(c, &op->rd, &op->rs1))
		x86_mov_rr(&c->e, rd, MAPG(c, &op->rs1));
}

static void gen_op(x86_compiler *c, shil_opcode *op, RuntimeBlockInfo *block, bool optimise)
{
	x86_emit *e = &c->e;
	switch (op->op)
	{
	case shop_ifb:
		gen_slice_left(c);
		if (op->rs1._imm)
			x86_store_imm(e, ctx(&next_pc), op->rs2._imm);
		x86_mov_ri(e, X86_ECX, op->rs3._imm);
		if (!mmu_enabled())
			gen_call(c, OpDesc[op->rs3._imm]->oph);
		else
		{
			x86_store_imm(e, x86_abs(&fallback_oph), (u32)(uintptr_t)OpDesc[op->rs3._imm]->oph);
			x86_mov_ri(e, X86_EDX, op_pc(block, op));	/* pc */
			gen_call(c, &rec_x86_interpreter_fallback);
		}
		break;

	case shop_jcond:
	case shop_jdyn:
		{
			const int rd = MAPG(c, &op->rd);
			if (shil_is_imm(&op->rs1))
			{
				if (shil_is_imm(&op->rs2))
					x86_mov_ri(e, rd, op->rs1._imm + op->rs2._imm);
				else
				{
					x86_mov_ri(e, rd, op->rs1._imm);
					verify(shil_is_null(&op->rs2));
				}
			}
			else
			{
				const int rs1 = MAPG(c, &op->rs1);
				if (rd != rs1)
					x86_mov_rr(e, rd, rs1);
				if (shil_is_imm(&op->rs2))
					x86_alu_ri(e, false, X86_ADD, rd, op->rs2._imm);
			}
		}
		break;

	case shop_mov32:
		verify(shil_is_reg(&op->rd));
		verify(shil_is_reg(&op->rs1) || shil_is_imm(&op->rs1));
		if (IS_ALLOCF(c, &op->rd))
			param_to_reg(c, &op->rs1, MAPF(c, &op->rd), true);
		else
			param_to_reg(c, &op->rs1, MAPG(c, &op->rd), false);
		break;

	case shop_mov64:
		verify(shil_is_r64f(&op->rd));
		verify(shil_is_r64f(&op->rs1));
		/* (pairs are never in registers here: ALLOC_F64) */
		x64_movq_load(e, 0, x86_abs(shil_reg_ptr(&op->rs1)));
		x64_movq_store(e, x86_abs(shil_reg_ptr(&op->rd)), 0);
		break;

	case shop_readm:
		if (!gen_read_mem_immediate(c, op, block) && (!optimise || !gen_read_memory_fast(c, op)))
		{
			u32 size;
			gen_mem_addr(c, op, X86_ECX);
			gen_read_memory_slow(c, op, block);

			size = op->flags & 0x7f;
			if (size != 8)
				reg_to_param(c, &op->rd, X86_EAX, false);
			else
			{
				x86_store(e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);
				x86_store(e, ctx(shil_reg_ptr(&op->rd) + 1), X86_EDX);
			}
		}
		break;

	case shop_writem:
		if (!gen_write_mem_immediate(c, op, block) && (!optimise || !gen_write_memory_fast(c, op)))
		{
			u32 size;
			gen_mem_addr(c, op, X86_ECX);
			size = op->flags & 0x7f;
			if (size != 8)
				param_to_reg(c, &op->rs2, X86_EDX, false);
			else
				x86_mov_ri(e, X86_EDX, (u32)(uintptr_t)shil_reg_ptr(&op->rs2));
			gen_write_memory_slow(c, op, block);
		}
		break;

	case shop_sync_sr:
		gen_call_c(c, &UpdateSR, NULL, 0);
		break;
	case shop_sync_fpscr:
		gen_call_c(c, &UpdateFPSCR, NULL, 0);
		break;

	case shop_swaplb:
		mov_rd_rs1(c, op);
		x64_shift_ri16(e, X86_ROR, MAPG(c, &op->rd), 8);
		break;

	case shop_neg:
		mov_rd_rs1(c, op);
		x86_neg(e, MAPG(c, &op->rd));
		break;
	case shop_not:
		mov_rd_rs1(c, op);
		x86_not(e, MAPG(c, &op->rd));
		break;

	case shop_and:
		gen_binary_op(c, op, X86_AND);
		break;
	case shop_or:
		gen_binary_op(c, op, X86_OR);
		break;
	case shop_xor:
		gen_binary_op(c, op, X86_XOR);
		break;
	case shop_add:
		gen_binary_op(c, op, X86_ADD);
		break;
	case shop_sub:
		gen_binary_op(c, op, X86_SUB);
		break;

	case shop_shl:
	case shop_shr:
	case shop_sar:
	case shop_ror:
		mov_rd_rs1(c, op);
		if (shil_is_imm(&op->rs2))
			x86_shift_ri(e, op->op == shop_shl ? X86_SHL : op->op == shop_shr ? X86_SHR
					: op->op == shop_sar ? X86_SAR : X86_ROR, MAPG(c, &op->rd), op->rs2._imm);
		else
			die("Unsupported operand");
		break;

	case shop_adc:
		{
			int rs2 = 0;
			int rd;
			x86_alu_ri(e, false, X86_CMP, MAPG(c, &op->rs3), 1);	/* C = ~rs3 */
			rd = MAPG(c, &op->rd);
			if (shil_is_reg(&op->rs2))
			{
				rs2 = MAPG(c, &op->rs2);
				if (SAME_G(c, &op->rd, &op->rs2))
				{
					x86_mov_rr(e, X86_ECX, rs2);
					rs2 = X86_ECX;
				}
			}
			rd_from_rs1(c, op, rd);
			x86_cmc(e);		/* C = rs3 */
			if (shil_is_reg(&op->rs2))
				x86_alu_rr(e, X86_ADC, rd, rs2);	/* (C,rd)=rs1+rs2+rs3(C) */
			else
				x86_alu_ri(e, false, X86_ADC, rd, shil_imm_value(&op->rs2));
			x86_setcc(e, X86_CC_B, X86_EAX);
			x64_movzx8_rr(e, MAPG(c, &op->rd2), X86_EAX);	/* rd2 = C */
		}
		break;

	case shop_negc:
		/* rd = -(rs1 + rs2), and rd2 the borrow out of the 64-bit
		 * subtraction x86-64 does: 1 unless both are 0. */
		if (shil_is_imm(&op->rs1))
			x86_mov_ri(e, X86_ECX, shil_imm_value(&op->rs1));
		else
			x86_mov_rr(e, X86_ECX, MAPG(c, &op->rs1));
		x86_mov_rr(e, X86_EAX, X86_ECX);
		if (shil_is_imm(&op->rs2))
			x86_alu_ri(e, false, X86_ADD, X86_ECX, shil_imm_value(&op->rs2));
		else
			x86_alu_rr(e, X86_ADD, X86_ECX, MAPG(c, &op->rs2));
		x86_neg(e, X86_ECX);
		if (shil_is_imm(&op->rs2))
			x86_alu_ri(e, false, X86_OR, X86_EAX, shil_imm_value(&op->rs2));
		else
			x86_alu_rr(e, X86_OR, X86_EAX, MAPG(c, &op->rs2));
		x86_setcc(e, X86_CC_NE, X86_EAX);
		x64_movzx8_rr(e, X86_EAX, X86_EAX);
		x86_mov_rr(e, MAPG(c, &op->rd), X86_ECX);
		x86_mov_rr(e, MAPG(c, &op->rd2), X86_EAX);
		break;

	case shop_rocr:
	case shop_rocl:
		{
			const int rd = MAPG(c, &op->rd);
			x86_alu_ri(e, false, X86_CMP, MAPG(c, &op->rs2), 1);	/* C = ~rs2 */
			rd_from_rs1(c, op, rd);
			x86_cmc(e);		/* C = rs2 */
			x86_shift_ri(e, op->op == shop_rocr ? X86_RCR : X86_RCL, rd, 1);
			x86_setcc(e, X86_CC_B, X86_EAX);
			x64_movzx8_rr(e, MAPG(c, &op->rd2), X86_EAX);	/* rd2 = C */
		}
		break;

	case shop_shld:
	case shop_shad:
		{
			int rd;
			x86_label negative_shift, non_zero, exit;
			x86_label_init(&negative_shift);
			x86_label_init(&non_zero);
			x86_label_init(&exit);
			if (shil_is_reg(&op->rs2))
				x86_mov_rr(e, X86_ECX, MAPG(c, &op->rs2));
			else
				/* This shouldn't happen. If arg is imm -> shop_shl/shr/sar */
				x86_mov_ri(e, X86_ECX, shil_imm_value(&op->rs2));
			rd = MAPG(c, &op->rd);
			rd_from_rs1(c, op, rd);

			x86_alu_ri(e, false, X86_CMP, X86_ECX, 0);
			x86_jmp_label(e, X86_CC_S, &negative_shift, false);
			x86_shift_cl(e, X86_SHL, rd);
			x86_jmp_label(e, -1, &exit, false);

			x86_label_bind(e, &negative_shift);
			x86_test_ri(e, X86_ECX, 0x1f);
			x86_jmp_label(e, X86_CC_NE, &non_zero, false);
			if (op->op == shop_shld)
				x86_xor_rr(e, rd, rd);
			else
				x86_shift_ri(e, X86_SAR, rd, 31);
			x86_jmp_label(e, -1, &exit, false);

			x86_label_bind(e, &non_zero);
			x86_neg(e, X86_ECX);
			x86_shift_cl(e, op->op == shop_shld ? X86_SHR : X86_SAR, rd);
			x86_label_bind(e, &exit);
		}
		break;

	case shop_test:
	case shop_seteq:
	case shop_setge:
	case shop_setgt:
	case shop_setae:
	case shop_setab:
		if (op->op == shop_test)
		{
			if (shil_is_imm(&op->rs2))
				x86_test_ri(e, MAPG(c, &op->rs1), op->rs2._imm);
			else
				x86_test_rr(e, MAPG(c, &op->rs1), MAPG(c, &op->rs2));
		}
		else
		{
			if (shil_is_imm(&op->rs2))
				x86_alu_ri(e, false, X86_CMP, MAPG(c, &op->rs1), op->rs2._imm);
			else
				x86_alu_rr(e, X86_CMP, MAPG(c, &op->rs1), MAPG(c, &op->rs2));
		}
		x86_setcc(e, op->op == shop_setge ? X86_CC_GE : op->op == shop_setgt ? X86_CC_G
				: op->op == shop_setae ? X86_CC_NB : op->op == shop_setab ? X86_CC_A : X86_CC_E, X86_EAX);
		x64_movzx8_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;

	case shop_setpeq:
		{
			x86_label end;
			x86_label_init(&end);
			x86_mov_rr(e, X86_ECX, MAPG(c, &op->rs1));
			if (shil_is_r32i(&op->rs2))
				x86_xor_rr(e, X86_ECX, MAPG(c, &op->rs2));
			else
				x86_alu_ri(e, false, X86_XOR, X86_ECX, op->rs2._imm);

			x86_mov_ri(e, X86_EAX, 1);
			x86_test_ri(e, X86_ECX, 0xFF000000);
			x86_jmp_label(e, X86_CC_E, &end, false);
			x86_test_ri(e, X86_ECX, 0x00FF0000);
			x86_jmp_label(e, X86_CC_E, &end, false);
			x86_test_ri(e, X86_ECX, 0x0000FF00);
			x86_jmp_label(e, X86_CC_E, &end, false);
			x86_xor_rr(e, X86_EAX, X86_EAX);
			x64_test_rr8(e, X86_ECX, X86_ECX);
			x86_setcc(e, X86_CC_E, X86_EAX);
			x86_label_bind(e, &end);
			x86_mov_rr(e, MAPG(c, &op->rd), X86_EAX);
		}
		break;

	case shop_mul_u16:
		x64_movzx16_rr(e, X86_EAX, MAPG(c, &op->rs1));
		if (shil_is_reg(&op->rs2))
			x64_movzx16_rr(e, X86_ECX, MAPG(c, &op->rs2));
		else
			x86_mov_ri(e, X86_ECX, op->rs2._imm & 0xFFFF);
		x64_f7_w(e, false, 4, X86_ECX);
		x86_mov_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;
	case shop_mul_s16:
		x64_movsx16_rr(e, X86_EAX, MAPG(c, &op->rs1));
		if (shil_is_reg(&op->rs2))
			x64_movsx16_rr(e, X86_ECX, MAPG(c, &op->rs2));
		else
			x86_mov_ri(e, X86_ECX, (u32)(s32)(s16)op->rs2._imm);
		x64_f7_w(e, false, 4, X86_ECX);
		x86_mov_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;
	case shop_mul_i32:
		x86_mov_rr(e, X86_EAX, MAPG(c, &op->rs1));
		if (shil_is_reg(&op->rs2))
			x64_f7_w(e, false, 4, MAPG(c, &op->rs2));
		else
		{
			x86_mov_ri(e, X86_ECX, op->rs2._imm);
			x64_f7_w(e, false, 4, X86_ECX);
		}
		x86_mov_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;
	case shop_mul_u64:
	case shop_mul_s64:
		x86_mov_rr(e, X86_EAX, MAPG(c, &op->rs1));
		if (shil_is_reg(&op->rs2))
			x86_mov_rr(e, X86_ECX, MAPG(c, &op->rs2));
		else
			x86_mov_ri(e, X86_ECX, op->rs2._imm);
		x64_f7_w(e, false, op->op == shop_mul_u64 ? 4 : 5, X86_ECX);
		x86_mov_rr(e, MAPG(c, &op->rd), X86_EAX);
		x86_mov_rr(e, MAPG(c, &op->rd2), X86_EDX);
		break;

	case shop_pref:
		if (shil_is_imm(&op->rs1))
		{
			/* this test shouldn't be necessary */
			if ((op->rs1._imm & 0xFC000000) == 0xE0000000)
			{
				x86_mov_ri(e, X86_ECX, op->rs1._imm);
				gen_store_queue(c, op, block);
			}
		}
		else
		{
			int rn;
			x86_label no_sqw;
			x86_label_init(&no_sqw);
			if (IS_ALLOCG(c, &op->rs1))
				rn = MAPG(c, &op->rs1);
			else
			{
				x86_load(e, X86_EAX, ctx(shil_reg_ptr(&op->rs1)));
				rn = X86_EAX;
			}
			x86_mov_rr(e, X86_ECX, rn);
			x86_shift_ri(e, X86_SHR, X86_ECX, 26);
			x86_alu_ri(e, false, X86_CMP, X86_ECX, 0x38);
			x86_jmp_label(e, X86_CC_NE, &no_sqw, true);

			x86_mov_rr(e, X86_ECX, rn);
			gen_store_queue(c, op, block);
			x86_label_bind(e, &no_sqw);
		}
		break;

	case shop_ext_s8:
		x86_mov_rr(e, X86_EAX, MAPG(c, &op->rs1));
		x64_movsx8_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;
	case shop_ext_s16:
		x64_movsx16_rr(e, MAPG(c, &op->rd), MAPG(c, &op->rs1));
		break;

	case shop_xtrct:
		{
			const int rd = MAPG(c, &op->rd);
			int rs1 = X86_ECX, rs2 = X86_EAX;
			if (shil_is_reg(&op->rs1))
				rs1 = MAPG(c, &op->rs1);
			else
				x86_mov_ri(e, rs1, shil_imm_value(&op->rs1));
			if (shil_is_reg(&op->rs2))
				rs2 = MAPG(c, &op->rs2);
			else
				x86_mov_ri(e, rs2, shil_imm_value(&op->rs2));
			if (rd == rs2)
			{
				x86_shift_ri(e, X86_SHL, rd, 16);
				x86_mov_rr(e, X86_EAX, rs1);
				x86_shift_ri(e, X86_SHR, X86_EAX, 16);
				x86_alu_rr(e, X86_OR, rd, X86_EAX);
				break;
			}
			else if (rd != rs1)
				x86_mov_rr(e, rd, rs1);
			x86_shift_ri(e, X86_SHR, rd, 16);
			x86_mov_rr(e, X86_EAX, rs2);
			x86_shift_ri(e, X86_SHL, X86_EAX, 16);
			x86_alu_rr(e, X86_OR, rd, X86_EAX);
		}
		break;

	/* FPU */

	case shop_fadd:
		gen_binary_fop(c, op, X64_ADDSS);
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;
	case shop_fsub:
		gen_binary_fop(c, op, X64_SUBSS);
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;
	case shop_fmul:
		gen_binary_fop(c, op, X64_MULSS);
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;
	case shop_fdiv:
		gen_binary_fop(c, op, X64_DIVSS);
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;

	case shop_fabs:
		x64_movd_rx(e, X86_EAX, MAPF(c, &op->rs1));
		x86_alu_ri(e, false, X86_AND, X86_EAX, 0x7FFFFFFF);
		x64_movd_xr(e, MAPF(c, &op->rd), X86_EAX);
		break;
	case shop_fneg:
		x64_movd_rx(e, X86_EAX, MAPF(c, &op->rs1));
		x86_alu_ri(e, false, X86_XOR, X86_EAX, 0x80000000);
		x64_movd_xr(e, MAPF(c, &op->rd), X86_EAX);
		break;

	case shop_fsqrt:
		x64_ss_rr(e, X64_SQRTSS, MAPF(c, &op->rd), MAPF(c, &op->rs1));
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;

	case shop_fmac:
		/* One rounding, of the sum, as everywhere else: see x86-64 */
		if (!x86_has_fma())
		{
			shil_chf[op->op](op);
			break;
		}
		{
			const int rs1 = MAPF(c, &op->rs1);
			int rs2 = MAPF(c, &op->rs2);
			int rs3 = MAPF(c, &op->rs3);
			const int rd = MAPF(c, &op->rd);
			if (rd == rs2)
			{
				x64_movss_rr(e, 1, rs2);
				rs2 = 1;
			}
			if (rd == rs3)
			{
				x64_movss_rr(e, 2, rs3);
				rs3 = 2;
			}
			if (shil_is_imm(&op->rs1))
			{
				x86_mov_ri(e, X86_EAX, op->rs1._imm);
				x64_movd_xr(e, rd, X86_EAX);
			}
			else if (rd != rs1)
				x64_movss_rr(e, rd, rs1);
			x64_vfmadd231ss(e, rd, rs2, rs3);
			gen_nan_fix(c, rd);
		}
		break;

	case shop_fsrra:
		/* RSQRTSS has an |error| <= 1.5*2^-12 where the SH4 FSRRA needs |error| <= 2^-21 */
		x64_ss_rr(e, X64_SQRTSS, 0, MAPF(c, &op->rs1));
		x86_mov_ri(e, X86_EAX, 0x3f800000);	/* 1.0 */
		x64_movd_xr(e, MAPF(c, &op->rd), X86_EAX);
		x64_ss_rr(e, X64_DIVSS, MAPF(c, &op->rd), 0);
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;

	case shop_fsetgt:
	case shop_fseteq:
		x64_ucomiss(e, MAPF(c, &op->rs1), MAPF(c, &op->rs2));
		if (op->op == shop_fsetgt)
			x86_setcc(e, X86_CC_A, X86_EAX);
		else
		{
			/* the unordered case taken into account */
			x64_lahf(e);
			x64_test_ah_imm(e, 0x44);
			x86_setcc(e, 0xB /* NP */, X86_EAX);
		}
		x64_movzx8_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;

	case shop_fsca:
		if (shil_is_imm(&op->rs1))
			x86_mov_ri(e, X86_EAX, op->rs1._imm & 0xFFFF);
		else
			x64_movzx16_rr(e, X86_EAX, MAPG(c, &op->rs1));
		/* (a pair, which is never in registers here) */
		x64_movq_load(e, 0, x86_mi(-1, X86_EAX, 8, (s32)(u32)(uintptr_t)&sin_table));
		x64_movq_store(e, x86_abs(shil_reg_ptr(&op->rd)), 0);
		break;

	case shop_fipr:
		x86_mov_ri(e, X86_EAX, (u32)(uintptr_t)shil_reg_ptr(&op->rs1));
		x86_mov_ri(e, X86_ECX, (u32)(uintptr_t)shil_reg_ptr(&op->rs2));
		x86_emit_fipr(e, MAPF(c, &op->rd));
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;

	case shop_ftrv:
		x86_mov_ri(e, X86_EAX, (u32)(uintptr_t)shil_reg_ptr(&op->rs1));		/* the vector */
		x86_mov_ri(e, X86_ECX, (u32)(uintptr_t)shil_reg_ptr(&op->rs2));		/* the matrix */
		x86_emit_ftrv(e);
		gen_nan_fix4(c);
		x64_sse_rm(e, 0, false, 0x11, 0, x86_abs(shil_reg_ptr(&op->rd)), 0);	/* movups */
		break;

	case shop_frswap:
		{
			int i;
			x86_mov_ri(e, X86_EAX, (u32)(uintptr_t)shil_reg_ptr(&op->rs1));
			x86_mov_ri(e, X86_ECX, (u32)(uintptr_t)shil_reg_ptr(&op->rd));
			for (i = 0; i < 4; i++)
			{
				x64_movaps_load(e, 0, x86_m(X86_EAX, i * 16));
				x64_movaps_load(e, 1, x86_m(X86_ECX, i * 16));
				x64_movaps_store(e, x86_m(X86_EAX, i * 16), 1);
				x64_movaps_store(e, x86_m(X86_ECX, i * 16), 0);
			}
		}
		break;

	case shop_cvt_f2i_t:
		{
			/* As on x86-64: see there */
			const int rd = MAPG(c, &op->rd);
			x86_label done;
			x86_label_init(&done);

			x64_cvttss2si(e, X86_EDX, MAPF(c, &op->rs1));
			x86_mov_ri(e, rd, 0x7fffffff);
			x86_alu_ri(e, false, X86_CMP, X86_EDX, 0x7fffff80);	/* 2147483520.0f */
			x86_jmp_label(e, X86_CC_G, &done, false);
			x86_mov_rr(e, rd, X86_EDX);
			x86_alu_ri(e, false, X86_CMP, rd, 0x80000000);	/* indefinite integer */
			x86_jmp_label(e, X86_CC_NE, &done, false);
			x86_xor_rr(e, X86_EAX, X86_EAX);
			x64_pxor(e, 0, 0);
			x64_ucomiss(e, MAPF(c, &op->rs1), 0);
			x86_setcc(e, X86_CC_B, X86_EAX);
			x86_alu_ri(e, false, X86_ADD, X86_EAX, 0x7fffffff);
			x86_mov_rr(e, rd, X86_EAX);
			x86_label_bind(e, &done);
		}
		break;
	case shop_cvt_i2f_n:
	case shop_cvt_i2f_z:
		x64_cvtsi2ss(e, MAPF(c, &op->rd), MAPG(c, &op->rs1));
		break;

	default:
		shil_chf[op->op](op);
		break;
	}
}

static void x86_preload(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	x86_load(&comp->e, (int)(s8)nreg, ctx(GetRegPtr(reg)));
}

static void x86_writeback(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	x86_store(&comp->e, ctx(GetRegPtr(reg)), (int)(s8)nreg);
}

static void x86_preload_fpu(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	x64_movss_load(&comp->e, (int)(s8)nreg, ctx(GetRegPtr(reg)));
}

static void x86_writeback_fpu(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	x64_movss_store(&comp->e, ctx(GetRegPtr(reg)), (int)(s8)nreg);
}

static void compile_block(x86_compiler *c, RuntimeBlockInfo* block, bool force_checks, bool optimise)
{
	x86_emit *e = &c->e;
	bool go_on, mmu_go_on, go_on_branch, go_on_next, watch;
	u32 place;

	c->current_opid = (size_t)-1;
	if (force_checks)
		check_block(c, block);
	else if (mmu_enabled())
	{
		/* Is this the address the block was compiled for? See x86-64. */
		x86_alu_mi(e, X86_CMP, ctx(&next_pc), block->vaddr);
		x86_jmp_label(e, X86_CC_NE, &c->vaddr_failed, true);
		c->vaddr_checked = true;
		block->linked_entry = (u32)(e->p - e->start);
	}

	if (mmu_enabled() && block->has_fpu_op)
	{
		x86_label fpu_enabled;
		rec_arg args[3];
		x86_label_init(&fpu_enabled);
		x64_test_mi(e, x86_abs(&sr), 0x8000);	/* test SR.FD bit */
		x86_jmp_label(e, X86_CC_E, &fpu_enabled, true);
		args[0] = arg_i(block->vaddr);
		args[1] = arg_i(0x800);
		args[2] = arg_i(0x100);
		gen_call_c(c, &Do_Exception, args, 3);
		x86_jmp_label(e, -1, &c->exit_block, true);
		x86_label_bind(e, &fpu_enabled);
	}
	c->block_cycles = block->guest_cycles;
	c->charge_at_tail = !mmu_enabled();
	if (!c->charge_at_tail)
		gen_charge(c);
	if (settings.dynarec.AccurateTiming && !mmu_enabled())
	{
		/* a block that can change something other than a register says so: see wait_site.h */
		if (sh4_block_writes(block))
			x64_inc_m(e, x86_abs(&sh4_write_gen));
	}
	sh4_regalloc_begin(&c->ra, block, alloc_regs, (int)(sizeof(alloc_regs) / sizeof(alloc_regs[0])),
			alloc_fregs, (int)(sizeof(alloc_fregs) / sizeof(alloc_fregs[0])));

	for (c->current_opid = 0; c->current_opid < block->op_count; c->current_opid++)
	{
		shil_opcode *op = &block->oplist[c->current_opid];
		sh4_regalloc_op_begin(&c->ra, op, (int)c->current_opid);
		gen_op(c, op, block, optimise);
		sh4_regalloc_op_end(&c->ra, op);
	}
	sh4_regalloc_cleanup(&c->ra);
	c->current_opid = (size_t)-1;

	/* How a block ends, as on x86-64: see there */
	go_on = !mmu_enabled();
	mmu_go_on = !go_on && block->mmu_go_on;
	go_on_branch = go_on || (mmu_go_on && rdv_MmuSamePlace(block, block->BranchBlock, &place));
	go_on_next = go_on || (mmu_go_on && rdv_MmuSamePlace(block, block->NextBlock, &place));
	/* see wait_site.h */
	watch = go_on && settings.dynarec.AccurateTiming && block->BranchBlock <= block->vaddr;

	switch (block->BlockType)
	{
	case BET_StaticJump:
	case BET_StaticCall:
		if (go_on_branch)
		{
			if (watch)
				gen_wait_check(c, block);
			gen_go_on(c, block, 0);
		}
		else
			x86_store_imm(e, ctx(&next_pc), block->BranchBlock);
		break;

	case BET_Cond_0:
	case BET_Cond_1:
		{
			x86_label branch_not_taken;
			x86_label_init(&branch_not_taken);
			x86_alu_mi(e, X86_CMP, block->has_jcond ? ctx(&Sh4cntx.jdyn) : ctx(&sr.T), block->BlockType & 1);

			x86_jmp_label(e, X86_CC_NE, &branch_not_taken, true);
			if (go_on_branch)
			{
				if (watch)
					gen_wait_check(c, block);
				gen_go_on(c, block, 0);
			}
			else
			{
				x86_store_imm(e, ctx(&next_pc), block->BranchBlock);
				x86_jmp_label(e, -1, &c->exit_block, true);
			}
			x86_label_bind(e, &branch_not_taken);
			if (go_on_next)
				gen_go_on(c, block, 1);
			else
				x86_store_imm(e, ctx(&next_pc), block->NextBlock);
		}
		break;

	case BET_DynamicJump:
	case BET_DynamicCall:
	case BET_DynamicRet:
		x86_load(e, X86_EDX, ctx(&Sh4cntx.jdyn));
		if (go_on)
		{
			/* the address is only known now: its place in the table is worked out here */
			gen_slice_check(c);
			x86_mov_rr(e, X86_EAX, X86_EDX);
			x86_shift_ri(e, X86_SHR, X86_EAX, 1);
			x86_alu_ri(e, false, X86_AND, X86_EAX, FPCB_MASK);
			x86_jmp_m(e, x86_mi(-1, X86_EAX, 4, (s32)(u32)(uintptr_t)&p_sh4rcb->fpcb[0]));
		}
		else
			x86_store(e, ctx(&next_pc), X86_EDX);
		break;

	case BET_DynamicIntr:
	case BET_StaticIntr:
		if (block->BlockType == BET_DynamicIntr)
		{
			x86_load(e, X86_EDX, ctx(&Sh4cntx.jdyn));
			x86_store(e, ctx(&next_pc), X86_EDX);
		}
		else
			x86_store_imm(e, ctx(&next_pc), block->NextBlock);

		if (c->charge_at_tail)
			gen_charge(c);
		gen_call_c(c, &UpdateINTC, NULL, 0);
		break;

	default:
		die("Invalid block end type");
	}

	/* Back to the main loop */
	x86_label_bind(e, &c->exit_block);
	x86_jmp_abs(e, -1, (uintptr_t)block_return);

	/* The time slice is used up where the block would have gone on to
	 * another: the address it was going to is in edx. */
	if (c->slice_out_used)
	{
		x86_label_bind(e, &c->slice_out);
		x86_store(e, ctx(&next_pc), X86_EDX);
		x86_jmp_label(e, -1, &c->exit_block, true);
	}
	/* The block's code is no longer what it was compiled from (ecx has
	 * its address from check_block()) */
	if (force_checks)
	{
		far_bind(e, &c->check_failed);
		x86_call(e, (uintptr_t)&rec_x86_blockcheckfail);
		x86_jmp_label(e, -1, &c->exit_block, true);
	}
	/* Or it is not the block for this address */
	if (c->vaddr_checked)
	{
		x86_label_bind(e, &c->vaddr_failed);
		x86_mov_ri(e, X86_ECX, block->addr);
		x86_call(e, (uintptr_t)&rec_x86_blockcheckfail);
		x86_jmp_label(e, -1, &c->exit_block, true);
	}

	if (e->full)
		die("x86 recompiler: block too large");

	block->code = (DynarecCodeEntryPtr)(uintptr_t)e->start;
	block->host_code_size = (u32)(e->p - e->start);

	emit_Skip(block->host_code_size);
}

void ngen_Compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
{
	/* (in room of its own, made anew for each block: nothing allocated) */
	static x86_compiler room;
	(void)reset;
	(void)staging;

	verify(emit_FreeSpace() >= CODE_MARGIN);

	comp = &room;
	x86_init(&room.e, (u8 *)emit_GetCCPtr(), emit_FreeSpace());
	sh4_regalloc_init(&room.ra, ALLOC_F64, x86_preload, x86_writeback, x86_preload_fpu, x86_writeback_fpu, &room);
	x86_label_init(&room.exit_block);
	x86_label_init(&room.slice_out);
	x86_label_init(&room.vaddr_failed);
	room.check_failed.patches = 0;
	room.vaddr_checked = false;
	room.slice_out_used = false;
	room.block_cycles = 0;
	room.charge_at_tail = false;
	room.cc_count = 0;

	compile_block(&room, block, force_checks, optimise);

	comp = NULL;
}

void ngen_CC_Start(shil_opcode* op)
{
	(void)op;
	comp->cc_count = 0;
}

void ngen_CC_Param(shil_opcode* op, shil_param* prm, CanonicalParamType tp)
{
	x86_compiler *c = comp;
	(void)op;
	switch (tp)
	{
	case CPT_u32:
	case CPT_ptr:
	case CPT_f32:
		verify(c->cc_count < 8);
		c->cc_pars[c->cc_count].type = tp;
		c->cc_pars[c->cc_count].prm = prm;
		c->cc_count++;
		break;

	/* from eax */
	case CPT_u64rvL:
	case CPT_u32rv:
		reg_to_param(c, prm, X86_EAX, false);
		break;

	/* from edx, after the low half */
	case CPT_u64rvH:
		reg_to_param(c, prm, X86_EDX, false);
		break;

	/* from the x87's top of stack, where a float is returned */
	case CPT_f32rv:
		x64_fstp_m32(&c->e, x86_abs(&f32_rv));
		x64_movss_load(&c->e, 0, x86_abs(&f32_rv));
		reg_to_param(c, prm, 0, true);
		break;
	}
}

/* A canonical implementation, which is a plain C function: everything on the stack */
void ngen_CC_Call(shil_opcode* op, void* function)
{
	x86_compiler *c = comp;
	rec_arg args[8];
	const int count = c->cc_count;
	int i;
	(void)op;

	verify(count <= 8);
	/* (they came last argument first) */
	for (i = 0; i < count; i++)
	{
		const shil_param *prm = c->cc_pars[count - 1 - i].prm;
		switch (c->cc_pars[count - 1 - i].type)
		{
		case CPT_ptr:
			verify(shil_is_reg(prm));
			args[i] = arg_i((u32)(uintptr_t)shil_reg_ptr(prm));
			break;
		default:
			if (shil_is_imm(prm))
				args[i] = arg_i(prm->_imm);
			else if (IS_ALLOCG(c, prm))
				args[i] = arg_r(MAPG(c, prm));
			else if (IS_ALLOCF(c, prm))
				args[i] = arg_x(MAPF(c, prm));
			else
			{
				verify(shil_is_reg(prm));
				args[i] = arg_m(shil_reg_ptr(prm));
			}
			break;
		}
	}
	gen_call_raw(c, (uintptr_t)function, args, count, false);
}

void ngen_CC_Finish(shil_opcode* op)
{
	(void)op;
}

/* A fast memory access (fast_memory()) faulted: it is written over with a
 * call to the routine that does such an access by asking (mem_stubs), and
 * run again from where it was. It is six bytes or more, and a call takes
 * five; what is left of it is filled in. */
bool ngen_Rewrite(unat *host_pc_at)
{
	static const u8 fill[4][3] = { { 0 }, { 0x90 }, { 0x66, 0x90 }, { 0x0F, 0x1F, 0x00 } };
	unat host_pc = *host_pc_at;
	int kind, data, length;
	u8 *site;
	s32 rel;

	if (!fast_memory())
		return false;
	if (bm_GetBlock2((void *)host_pc) == NULL)
	{
		WARN_LOG(DYNAREC, "ngen_Rewrite: Block at %p not found\n", (void *)host_pc);
		return false;
	}
	if (!fast_access_at((const u8 *)host_pc, &kind, &data, &length)
			|| mem_stubs[kind][data] == NULL)
	{
		WARN_LOG(DYNAREC, "ngen_Rewrite: no memory access at %p\n", (void *)host_pc);
		return false;
	}

	site = (u8 *)host_pc;
	rel = (s32)(mem_stubs[kind][data] - (site + 5));

	verify(length >= 5 && length <= 8);
	site[0] = 0xE8;
	memcpy(site + 1, &rel, 4);
	memcpy(site + 5, fill[length - 5], length - 5);
	return true;
}

void ngen_HandleException(void)
{
	rec_unwind();
}
#else
/* (for the other processors: C has no empty file) */
typedef int rec_x86_not_here;
#endif
