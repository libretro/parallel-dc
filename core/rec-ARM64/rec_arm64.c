/*
	Copyright 2019 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
/* The SH4 recompiler for AArch64, on jit/a64_emit.h: each of VIXL's
 * macros it used to be written with expanded as VIXL expanded it. C89. */

#include "types.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_ARM64

#include <setjmp.h>
#include <string.h>

#include "jit/a64_emit.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/wait_site.h"
#include "hw/sh4/dyna/ssa_regalloc.h"

#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_interpreter.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/holly/sb_mem.h"
#include "hw/mem/_vmem.h"

#undef do_sqw_nommu

void ngen_blockcheckfail(u32 pc);
void ngen_LinkBlock_Generic_stub(void);
void ngen_LinkBlock_cond_Branch_stub(void);
void ngen_LinkBlock_cond_Next_stub(void);
void ngen_FailedToFindBlock_mmu(void);
void ngen_FailedToFindBlock_nommu(void);

#ifdef __APPLE__
/* What ngen_arm64.S has elsewhere, for Apple's assembler: a C name has an
 * underscore before it there, and a symbol is kept to the library with
 * .private_extern, not .hidden. Its linker may also move the code after
 * each symbol on its own: a symbol the code before it runs on into is
 * marked as part of that code (.alt_entry). */
__asm__(
		".text\n"
		".p2align 2\n"
		".private_extern _ngen_LinkBlock_cond_Branch_stub\n"
		".globl _ngen_LinkBlock_cond_Branch_stub\n"
	"_ngen_LinkBlock_cond_Branch_stub:\n"
		"mov w1, #1\n"
		"b _ngen_LinkBlock_Shared_stub\n"

		".private_extern _ngen_LinkBlock_cond_Next_stub\n"
		".globl _ngen_LinkBlock_cond_Next_stub\n"
	"_ngen_LinkBlock_cond_Next_stub:\n"
		"mov w1, #0\n"
		"b _ngen_LinkBlock_Shared_stub\n"

		".private_extern _ngen_LinkBlock_Generic_stub\n"
		".globl _ngen_LinkBlock_Generic_stub\n"
	"_ngen_LinkBlock_Generic_stub:\n"
		"mov w1, w29\n"						/* djump/pc, in case it is needed */

		".private_extern _ngen_LinkBlock_Shared_stub\n"
		".alt_entry _ngen_LinkBlock_Shared_stub\n"
		".globl _ngen_LinkBlock_Shared_stub\n"
	"_ngen_LinkBlock_Shared_stub:\n"
		"sub x0, lr, #4\n"					/* the call */
		"bl _rdv_LinkBlock\n"				/* returns an RX address */
		"br x0\n"

		".private_extern _ngen_FailedToFindBlock_nommu\n"
		".globl _ngen_FailedToFindBlock_nommu\n"
	"_ngen_FailedToFindBlock_nommu:\n"
		"mov w0, w29\n"
		"bl _rdv_FailedToFindBlock\n"
		"br x0\n"

		".private_extern _ngen_FailedToFindBlock_mmu\n"
		".globl _ngen_FailedToFindBlock_mmu\n"
	"_ngen_FailedToFindBlock_mmu:\n"
		"bl _rdv_FailedToFindBlock_pc\n"
		"br x0\n"

		".private_extern _ngen_blockcheckfail\n"
		".globl _ngen_blockcheckfail\n"
	"_ngen_blockcheckfail:\n"
		"bl _rdv_BlockCheckFail\n"
		"br x0\n"
);
#endif

static jmp_buf jmp_env;

/* The kinds of fast memory access there are (gen_fast_access()): a load or
 * a store, of 1, 2, 4 or 8 bytes, the 4 from or to a general register or a
 * floating-point one. */
enum
{
	MEM_LOAD8, MEM_LOAD16, MEM_LOAD32, MEM_LOAD32F, MEM_LOAD64,
	MEM_STORE8, MEM_STORE16, MEM_STORE32, MEM_STORE32F, MEM_STORE64,
	MEM_KINDS
};
/* (whether a kind's data is in a floating-point register) */
#define MEM_KIND_F(kind) ((kind) == MEM_LOAD32F || (kind) == MEM_STORE32F)

/* What a fast access that faulted is written over with a call to
 * (ngen_Rewrite()): a routine that does the access by asking. A fast
 * access has its address and its data in whatever registers the block has
 * them in, so there is a routine for each kind of access, each register
 * the address can be in and each the data can be in - 528 of them, a few
 * instructions each, written after the main loop each time that is.
 *
 * The registers, by number: for the address and for data in a general
 * register, 0 is the one a call has it in (w0; w1 for what is stored) and
 * 1 on are the block's own, alloc_regs; for data in a floating-point
 * register, 0 on are the block's, alloc_fregs. 64 bits are always in x0
 * or x1. */
#define MEM_REGS 8
static u8 *mem_stubs[MEM_KINDS][MEM_REGS][MEM_REGS];

/* x26 is not handed out: recompiled code keeps the address of the SH4's
 * memory, as the host has it mapped, in it - a load or store is then one
 * instruction (gen_fast_access()). x27 has what is left of the time slice
 * and x28 the address of the SH4's context. */
static const u32 alloc_regs[] = { 19, 20, 21, 22, 23, 24, 25 };
static const u32 alloc_fregs[] = { 8, 9, 10, 11, 12, 13, 14, 15 };
#define ALLOC_REGS ((int)(sizeof(alloc_regs) / sizeof(alloc_regs[0])))
#define ALLOC_FREGS ((int)(sizeof(alloc_fregs) / sizeof(alloc_fregs[0])))
/* A pair of the SH4's floating-point registers (DRn, XDn) is kept in two of them */
#define ALLOC_F64 true

/* What recompiled code calls (global: tools/sh4/arm64_run.sh builds the
 * C++ recompiler this replaced against them). (signed char and short by
 * name: s8 and s16 are registers as well on this host's other recompilers) */
s32 rec_arm64_mem_load8(u32 addr)  { return (signed char)ReadMem8(addr); }
s32 rec_arm64_mem_load16(u32 addr) { return (short)ReadMem16(addr); }
u32 rec_arm64_mem_load32(u32 addr) { return ReadMem32(addr); }
u64 rec_arm64_mem_load64(u32 addr) { return ReadMem64(addr); }
/* Whether extending a byte or a word that is passed to a function is the
 * caller's job. It is the function's own by this host's rules, except on
 * Apple's systems: a write handler called from compiled code there took the
 * register's other bits along with the value. */
#ifdef __APPLE__
static const bool caller_extends = true;
#else
static const bool caller_extends = false;
#endif
void rec_arm64_mem_store8(u32 addr, u32 data)  { WriteMem8(addr, (u8)data); }
void rec_arm64_mem_store16(u32 addr, u32 data) { WriteMem16(addr, (u16)data); }
void rec_arm64_mem_store32(u32 addr, u32 data) { WriteMem32(addr, data); }
void rec_arm64_mem_store64(u32 addr, u64 data) { WriteMem64(addr, data); }

/* The number a general register goes by here: 0 if it is @first, 1 on for
 * the block's own, -1 for any other. */
static int mem_reg_number(int reg, int first)
{
	int i;
	if (reg == first)
		return 0;
	for (i = 0; i < ALLOC_REGS; i++)
		if ((int)alloc_regs[i] == reg)
			return i + 1;
	return -1;
}

/* The same for a floating-point register, which is one of the block's or nothing */
static int mem_freg_number(int reg)
{
	int i;
	for (i = 0; i < ALLOC_FREGS; i++)
		if ((int)alloc_fregs[i] == reg)
			return i;
	return -1;
}

/* The register of the SH4's memory as the host has it mapped */
#define MEM_BASE 26
/* the context, what is left of the time slice, the next pc */
#define CTX 28
#define CYC 27
#define NPC 29
#define LR 30

typedef void (*a64_func)(void);

static void (*mainloop)(void *context);
/* For the routines that link blocks, where the code cannot reach them with
 * a bl (gen_link_trampolines()): NULL where it can */
static const a64_func link_stubs[3] = { ngen_LinkBlock_Generic_stub, ngen_LinkBlock_cond_Branch_stub, ngen_LinkBlock_cond_Next_stub };
static a64_func link_trampolines[3];
static int (*arm64_intc_sched)(void);
static void (*arm64_no_update)(void);

static bool restarting;
/* (driver.c: where the temporary cache ends while a block is made there) */
extern u32 *emit_ptr_limit;

static void generate_mainloop(void);

void ngen_mainloop(void* v_cntx)
{
	do {
		restarting = false;
		generate_mainloop();

		mainloop(v_cntx);
		if (restarting)
			p_sh4rcb->cntx.CpuRunning = 1;
	} while (restarting);
}

void ngen_init(void)
{
	INFO_LOG(DYNAREC, "Initializing the ARM64 dynarec\n");
	ngen_FailedToFindBlock = &ngen_FailedToFindBlock_nommu;
}

void ngen_ResetBlocks(void)
{
	mainloop = NULL;
	if (mmu_enabled())
		ngen_FailedToFindBlock = &ngen_FailedToFindBlock_mmu;
	else
		ngen_FailedToFindBlock = &ngen_FailedToFindBlock_nommu;
	if (p_sh4rcb->cntx.CpuRunning)
	{
		/* Force the dynarec out of mainloop() to regenerate it */
		p_sh4rcb->cntx.CpuRunning = 0;
		restarting = true;
	}
}

void ngen_GetFeatures(ngen_features* dst)
{
	dst->InterpreterFallback = false;
	dst->OnlyDynamicEnds     = false;
}

static u64 read_noex(u32 addr, u32 pc, u32 size)
{
#ifndef NO_MMU
	u32 ex;
	const u64 rv = mmu_ReadMemNoEx_c(addr, size, &ex);
	if (ex)
	{
		spc = pc;
		longjmp(jmp_env, 1);
	}
	return rv;
#else
	(void)addr;
	(void)pc;
	(void)size;
	return 0;
#endif
}

u8 rec_arm64_read_noex8(u32 addr, u32 unused, u32 pc) { (void)unused; return (u8)read_noex(addr, pc, 1); }
u16 rec_arm64_read_noex16(u32 addr, u32 unused, u32 pc) { (void)unused; return (u16)read_noex(addr, pc, 2); }
u32 rec_arm64_read_noex32(u32 addr, u32 unused, u32 pc) { (void)unused; return (u32)read_noex(addr, pc, 4); }
u64 rec_arm64_read_noex64(u32 addr, u32 unused, u32 pc) { (void)unused; return read_noex(addr, pc, 8); }

static void write_noex(u32 addr, u64 data, u32 pc, u32 size)
{
#ifndef NO_MMU
	const u32 ex = mmu_WriteMemNoEx_c(addr, data, size);
	if (ex)
	{
		spc = pc;
		longjmp(jmp_env, 1);
	}
#else
	(void)addr;
	(void)data;
	(void)pc;
	(void)size;
#endif
}

void rec_arm64_write_noex8(u32 addr, u8 data, u32 pc) { write_noex(addr, data, pc, 1); }
void rec_arm64_write_noex16(u32 addr, u16 data, u32 pc) { write_noex(addr, data, pc, 2); }
void rec_arm64_write_noex32(u32 addr, u32 data, u32 pc) { write_noex(addr, data, pc, 4); }
void rec_arm64_write_noex64(u32 addr, u64 data, u32 pc) { write_noex(addr, data, pc, 8); }

static void sh4_exception_raised(SH4ThrownException *ex, u32 pc)
{
	if (pc & 1)
	{
		/* Delay slot */
		AdjustDelaySlotException(ex);
		pc--;
	}
	Do_Exception(pc, ex->expEvn, ex->callVect);
	longjmp(jmp_env, 1);
}

void rec_arm64_interpreter_fallback(u16 op, OpCallFP *oph, u32 pc)
{
	sh4_catch c;
	SH4_TRY(c)
	{
		oph(op);
		sh4_catch_pop(&c);
	}
	else
		sh4_exception_raised(&c.ex, pc);
}

void rec_arm64_do_sqw_mmu_no_ex(u32 addr, u32 pc)
{
	sh4_catch c;
	SH4_TRY(c)
	{
		do_sqw_mmu(addr);
		sh4_catch_pop(&c);
	}
	else
		sh4_exception_raised(&c.ex, pc);
}

/* The code being made, and the block it is for */
typedef struct
{
	a64_emit e;
	a64_pool pool;
	sh4_regalloc ra;
	RuntimeBlockInfo *block;
	/* the canonical call's parameters (ngen_CC_Param()) */
	struct
	{
		CanonicalParamType type;
		const shil_param *prm;
	} cc_pars[8];
	int cc_count;
} arm64_compiler;

static arm64_compiler *comp;
/* (one at a time: the room, made anew for each thing made, nothing allocated) */
static arm64_compiler room;

static void compiler_start(arm64_compiler *c, void *buffer, size_t size)
{
	a64_init(&c->e, buffer, size);
	c->pool.count = 0;
	c->cc_count = 0;
	c->block = NULL;
}

static u8 *cursor(arm64_compiler *c)
{
	return (u8 *)c->e.p;
}

/* Whether @target, in the core's own code, is out of reach of a branch
 * from here (128 MB). It never is where the code is in the core's .text;
 * on macOS on Apple's processors it can be (see vmem_platform_alloc_jit()),
 * and the call or branch is then made through a register. */
static bool out_of_reach(arm64_compiler *c, uintptr_t target)
{
	const ptrdiff_t offset = (ptrdiff_t)(target - (uintptr_t)CC_RW2RX(cursor(c)));
	return offset < -128 * 1024 * 1024 || offset >= 128 * 1024 * 1024;
}

/* A call or a branch to C (by the address it runs at) */
static void gen_call_runtime(arm64_compiler *c, uintptr_t function, bool link)
{
	if (out_of_reach(c, function))
	{
		a64_mov_imm(&c->e, true, A64_IP0, function);
		if (link)
			a64_blr(&c->e, A64_IP0);
		else
			a64_br(&c->e, A64_IP0);
		return;
	}
	a64_b_rel(&c->e, link, (s64)(function - (uintptr_t)CC_RW2RX(cursor(c))));
}

/* A call or a branch to code in the cache (by where it is written) */
static void gen_call(arm64_compiler *c, uintptr_t code, bool link)
{
	a64_b_rel(&c->e, link, (s64)(code - (uintptr_t)cursor(c)));
}

/* Something in the SH4's context, from x28 */
static a64_mem ctx_mem(const void *p)
{
	const u32 offset = (u32)((const u8*)p - (const u8*)&p_sh4rcb->cntx);
	verify((offset & 3) == 0 && offset <= 16380);
	return a64_mem_off(CTX, offset);
}

static int map_reg(arm64_compiler *c, const shil_param *p)
{
	const int host = (int)(s8)sh4_regalloc_mapg(&c->ra, p);
	if (host == -1)
		die("Register not allocated");
	return host;
}

static int map_vreg(arm64_compiler *c, const shil_param *p, int index)
{
	const int host = (int)(s8)sh4_regalloc_mapf(&c->ra, p, index);
	if (host == -1)
		die("VRegister not allocated");
	return host;
}

#define IS_ALLOCG(c, p) sh4_regalloc_is_allocg(&(c)->ra, (p))
#define IS_ALLOCF(c, p) sh4_regalloc_is_allocf(&(c)->ra, (p))
#define MAPG(c, p) map_reg(c, p)
#define MAPF(c, p) map_vreg(c, p, 0)
#define MAPF_I(c, p, i) map_vreg(c, p, i)

#define OP_IMM(v) a64_opnd_imm((u64)(v))
#define OP_REG(r) a64_opnd_reg(r, A64_LSL, 0)

/* Mov(wd, imm) / Mov(wd, wn) */
static void mov_wi(arm64_compiler *c, int rd, u32 imm) { a64_mov_imm(&c->e, false, rd, imm); }
static void mov_ww(arm64_compiler *c, int rd, int rn) { a64m_mov(&c->e, false, rd, rn, false, false); }
static void mov_xx(arm64_compiler *c, int rd, int rn) { a64m_mov(&c->e, true, rd, rn, false, false); }

/* What is left of the time slice, where the timers find it, before a call
 * that can reach an on-chip register: see cycle_counter in sh4_if.h. */
static void gen_slice_left(arm64_compiler *c)
{
	a64m_ls(&c->e, A64_STRW, CYC, a64_mem_off(CTX, offsetof(Sh4Context, cycle_counter)));
}

/* A place in memory that is known when the block is compiled, as a load
 * or store is to have it: so far from x26, which has the address of the
 * SH4's memory as the host has it mapped, wherever it is within 4 GB
 * above that - main memory and everything else of the machine's that is
 * laid out there. The distance is put into @scratch with one or two
 * instructions. It is the place's 64-bit address for a place that is not
 * there. */
static a64_mem const_mem(arm64_compiler *c, const void *ptr, int scratch)
{
	const u8 *const base = (const u8 *)&p_sh4rcb->cntx + sizeof(Sh4Context);
	const u64 distance = (u64)((const u8 *)ptr - base);

	if (distance <= 0xFFFFFFFFu)
	{
		mov_wi(c, scratch, (u32)distance);
		return a64_mem_reg(MEM_BASE, scratch, 3, 0);
	}
	a64m_ldr_lit(&c->e, &c->pool, 0x58000000u, scratch, (uintptr_t)ptr);
	return a64_mem_off(scratch, 0);
}

static void param_to_reg(arm64_compiler *c, const shil_param *param, int reg, bool x)
{
	a64_emit *e = &c->e;
	if (shil_is_imm(param))
		a64_mov_imm(e, x, reg, param->_imm);
	else if (shil_is_reg(param))
	{
		if (shil_is_r64f(param) && IS_ALLOCF(c, param))
		{
			/* the first of the pair is the low half, as in memory */
			verify(x && reg != 15);
			a64m_fmov_ws(e, reg, MAPF_I(c, param, 0));
			a64m_fmov_ws(e, 15, MAPF_I(c, param, 1));
			a64_dw(e, 0xB3600000u | (31u << 10) | (15u << 5) | (u32)reg);	/* bfi xd, x15, #32, #32 */
		}
		else if (shil_is_r64f(param))
			a64m_ls(e, x ? A64_LDRX : A64_LDRW, reg, ctx_mem(shil_reg_ptr(param)));
		else if (shil_is_r32f(param))
		{
			if (IS_ALLOCF(c, param))
			{
				/* (fmov xd, dn where reg is x: never) */
				a64m_fmov_ws(e, reg, MAPF(c, param));
			}
			else
				a64m_ls(e, x ? A64_LDRX : A64_LDRW, reg, ctx_mem(shil_reg_ptr(param)));
		}
		else
		{
			if (IS_ALLOCG(c, param))
				a64m_mov(e, x, reg, MAPG(c, param), false, false);
			else
				a64m_ls(e, x ? A64_LDRX : A64_LDRW, reg, ctx_mem(shil_reg_ptr(param)));
		}
	}
	else
	{
		verify(shil_is_null(param));
	}
}

/* A register into a parameter: @kind 0 w, 1 x, 2 s */
static void reg_to_param(arm64_compiler *c, const shil_param *param, int reg, int kind)
{
	a64_emit *e = &c->e;
	if (kind == 1)
	{
		if (shil_is_r64f(param) && IS_ALLOCF(c, param))
		{
			/* (what was in reg is lost) */
			a64m_fmov_sw(e, MAPF_I(c, param, 0), reg);
			a64m_shift_imm(e, true, A64_LSR, reg, reg, 32);
			a64m_fmov_sw(e, MAPF_I(c, param, 1), reg);
		}
		else
			a64m_ls(e, A64_STRX, reg, ctx_mem(shil_reg_ptr(param)));
	}
	else if (IS_ALLOCG(c, param))
	{
		if (kind == 0)
			mov_ww(c, MAPG(c, param), reg);
		else
			a64m_fmov_ws(e, MAPG(c, param), reg);
	}
	else if (IS_ALLOCF(c, param))
	{
		if (kind == 2)
			a64m_fp2(e, A64_FMOV_S, MAPF(c, param), reg);
		else
			a64m_fmov_sw(e, MAPF(c, param), reg);
	}
	else
		a64m_ls(e, kind == 2 ? A64_STRS : A64_STRW, reg, ctx_mem(shil_reg_ptr(param)));
}

static void gen_binary_op(arm64_compiler *c, const shil_opcode *op, int which)
{
	a64_operand op3 = a64_opnd_imm(0);
	if (shil_is_imm(&op->rs2))
		op3 = OP_IMM(op->rs2._imm);
	else if (shil_is_r32i(&op->rs2))
		op3 = OP_REG(MAPG(c, &op->rs2));
	switch (which)
	{
	case 0: a64_logical(&c->e, false, 0, false, MAPG(c, &op->rd), MAPG(c, &op->rs1), op3); break;
	case 1: a64_logical(&c->e, false, 1, false, MAPG(c, &op->rd), MAPG(c, &op->rs1), op3); break;
	case 2: a64_logical(&c->e, false, 2, false, MAPG(c, &op->rd), MAPG(c, &op->rs1), op3); break;
	case 3: a64m_addsub(&c->e, false, false, false, MAPG(c, &op->rd), MAPG(c, &op->rs1), op3, false); break;
	default: a64m_addsub(&c->e, false, true, false, MAPG(c, &op->rd), MAPG(c, &op->rs1), op3, false); break;
	}
}

static void gen_binary_fop(arm64_compiler *c, const shil_opcode *op, u32 fop)
{
	int reg1, reg2;
	if (shil_is_imm(&op->rs1))
	{
		a64m_fmov_imm(&c->e, &c->pool, 0, op->rs1._imm);
		reg1 = 0;
	}
	else
		reg1 = MAPF(c, &op->rs1);
	if (shil_is_imm(&op->rs2))
	{
		a64m_fmov_imm(&c->e, &c->pool, 1, op->rs2._imm);
		reg2 = 1;
	}
	else
		reg2 = MAPF(c, &op->rs2);
	a64m_fp3(&c->e, fop, MAPF(c, &op->rd), reg1, reg2);
}

/* The address of a load or store: in @raddr (-1: w0, or the register the
 * block has it in where there is nothing to add to it); the register it
 * is in */
static int gen_mem_addr(arm64_compiler *c, const shil_opcode *op, int raddr)
{
	a64_emit *e = &c->e;
	int ret_reg = raddr < 0 ? 0 : raddr;

	if (shil_is_imm(&op->rs3))
	{
		if (IS_ALLOCG(c, &op->rs1))
			a64m_addsub(e, false, false, false, ret_reg, MAPG(c, &op->rs1), OP_IMM(op->rs3._imm), false);
		else
		{
			a64m_ls(e, A64_LDRW, ret_reg, ctx_mem(shil_reg_ptr(&op->rs1)));
			a64m_addsub(e, false, false, false, ret_reg, ret_reg, OP_IMM(op->rs3._imm), false);
		}
	}
	else if (shil_is_r32i(&op->rs3))
	{
		if (IS_ALLOCG(c, &op->rs1) && IS_ALLOCG(c, &op->rs3))
			a64m_addsub(e, false, false, false, ret_reg, MAPG(c, &op->rs1), OP_REG(MAPG(c, &op->rs3)), false);
		else
		{
			a64m_ls(e, A64_LDRW, ret_reg, ctx_mem(shil_reg_ptr(&op->rs1)));
			a64m_ls(e, A64_LDRW, 8, ctx_mem(shil_reg_ptr(&op->rs3)));
			a64m_addsub(e, false, false, false, ret_reg, ret_reg, OP_REG(8), false);
		}
	}
	else if (!shil_is_null(&op->rs3))
	{
		die("invalid rs3");
	}
	else if (shil_is_reg(&op->rs1))
	{
		if (IS_ALLOCG(c, &op->rs1))
		{
			if (raddr < 0)
				ret_reg = MAPG(c, &op->rs1);
			else
				mov_ww(c, ret_reg, MAPG(c, &op->rs1));
		}
		else
			a64m_ls(e, A64_LDRW, ret_reg, ctx_mem(shil_reg_ptr(&op->rs1)));
	}
	else
	{
		verify(shil_is_imm(&op->rs1));
		mov_wi(c, ret_reg, op->rs1._imm);
	}
	return ret_reg;
}

static void gen_read_memory_slow(arm64_compiler *c, u32 size)
{
	gen_slice_left(c);
	switch (size)
	{
	case 1:
		if (!mmu_enabled())
			gen_call_runtime(c, (uintptr_t)ReadMem8, true);
		else
			gen_call_runtime(c, (uintptr_t)&rec_arm64_read_noex8, true);
		a64_bfm(&c->e, false, 0, 0, 0, 0, 7);	/* sxtb w0, w0 */
		break;

	case 2:
		if (!mmu_enabled())
			gen_call_runtime(c, (uintptr_t)ReadMem16, true);
		else
			gen_call_runtime(c, (uintptr_t)&rec_arm64_read_noex16, true);
		a64_bfm(&c->e, false, 0, 0, 0, 0, 15);	/* sxth w0, w0 */
		break;

	case 4:
		if (!mmu_enabled())
			gen_call_runtime(c, (uintptr_t)ReadMem32, true);
		else
			gen_call_runtime(c, (uintptr_t)&rec_arm64_read_noex32, true);
		break;

	case 8:
		if (!mmu_enabled())
			gen_call_runtime(c, (uintptr_t)ReadMem64, true);
		else
			gen_call_runtime(c, (uintptr_t)&rec_arm64_read_noex64, true);
		break;

	default:
		die("1..8 bytes");
		break;
	}
}

static void gen_write_memory_slow(arm64_compiler *c, u32 size)
{
	gen_slice_left(c);
	switch (size)
	{
	case 1:
		if (caller_extends)
			a64_bfm(&c->e, false, 2, 1, 1, 0, 7);	/* uxtb w1, w1 */
		if (!mmu_enabled())
			gen_call_runtime(c, (uintptr_t)WriteMem8, true);
		else
			gen_call_runtime(c, (uintptr_t)&rec_arm64_write_noex8, true);
		break;

	case 2:
		if (caller_extends)
			a64_bfm(&c->e, false, 2, 1, 1, 0, 15);	/* uxth w1, w1 */
		if (!mmu_enabled())
			gen_call_runtime(c, (uintptr_t)WriteMem16, true);
		else
			gen_call_runtime(c, (uintptr_t)&rec_arm64_write_noex16, true);
		break;

	case 4:
		if (!mmu_enabled())
			gen_call_runtime(c, (uintptr_t)WriteMem32, true);
		else
			gen_call_runtime(c, (uintptr_t)&rec_arm64_write_noex32, true);
		break;

	case 8:
		if (!mmu_enabled())
			gen_call_runtime(c, (uintptr_t)WriteMem64, true);
		else
			gen_call_runtime(c, (uintptr_t)&rec_arm64_write_noex64, true);
		break;

	default:
		die("1..8 bytes");
		break;
	}
}

/* The end of a block that goes to @target and nowhere else: to the block
 * that is there, @linked, once it is known - until then to @stub, which
 * finds it or has it compiled, and has this written again
 * (rdv_LinkBlock()).
 *
 * With the MMU on that is for a target that is where it is whatever the
 * TLB says (ngen.h); for any other, the main loop, which looks the address
 * up. Either way the context has the address by then: a block begins by
 * seeing that it is the one for it. */
static void gen_static_tail(arm64_compiler *c, RuntimeBlockInfo *block, u32 target, RuntimeBlockInfo *linked, a64_func stub)
{
	int i;
	if (mmu_enabled())
	{
		u32 place;

		mov_wi(c, NPC, target);
		a64m_ls(&c->e, A64_STRW, NPC, ctx_mem(&next_pc));
		if (!block->mmu_go_on || !rdv_MmuSamePlace(block, target, &place))
		{
			gen_call(c, (uintptr_t)arm64_no_update, false);
			return;
		}
	}
	if (linked != NULL)
		gen_call(c, (uintptr_t)linked->code, false);
	else
	{
		for (i = 0; i < 3; i++)
			if (stub == link_stubs[i] && link_trampolines[i] != NULL)
			{
				gen_call(c, (uintptr_t)link_trampolines[i], true);
				return;
			}
		gen_call_runtime(c, (uintptr_t)stub, true);
	}
}

static u32 relink_block(arm64_compiler *c, RuntimeBlockInfo *block)
{
	a64_emit *e = &c->e;
	const u32 *const start = e->p;

	switch (block->BlockType)
	{
	case BET_StaticJump:
	case BET_StaticCall:
		gen_static_tail(c, block, block->BranchBlock, block->pBranchBlock, ngen_LinkBlock_Generic_stub);
		break;

	case BET_Cond_0:
	case BET_Cond_1:
		{
			a64_label branch_not_taken;
			a64_label_init(&branch_not_taken);

			if (block->has_jcond)
				a64m_ls(e, A64_LDRW, 11, ctx_mem(&Sh4cntx.jdyn));
			else
				a64m_ls(e, A64_LDRW, 11, ctx_mem(&sr.T));

			a64m_addsub(e, false, true, true, A64_ZR, 11, OP_IMM(block->BlockType & 1), false);
			a64_b_label(e, A64_NE, 0, &branch_not_taken);
			gen_static_tail(c, block, block->BranchBlock, block->pBranchBlock, ngen_LinkBlock_cond_Branch_stub);

			a64_label_bind(e, &branch_not_taken);

			gen_static_tail(c, block, block->NextBlock, block->pNextBlock, ngen_LinkBlock_cond_Next_stub);
		}
		break;

	case BET_DynamicJump:
	case BET_DynamicCall:
	case BET_DynamicRet:
		a64m_ls(e, A64_STRW, NPC, ctx_mem(&next_pc));
		if (!mmu_enabled())
		{
			a64m_addsub(e, true, true, false, 2, CTX, OP_IMM(offsetof(Sh4RCB, cntx)), false);
#if RAM_SIZE_MAX == 33554432
			a64_ubfx(e, 1, NPC, 1, 24);
#else
			a64_ubfx(e, 1, NPC, 1, 23);
#endif
			a64m_ls(e, A64_LDRX, 15, a64_mem_reg(2, 1, 3, 3));	/* the block's entry point */
			a64_br(e, 15);
		}
		else
			gen_call(c, (uintptr_t)arm64_no_update, false);
		break;

	case BET_DynamicIntr:
	case BET_StaticIntr:
		if (block->BlockType == BET_StaticIntr)
			mov_wi(c, NPC, block->NextBlock);
		/* else next_pc = *jdyn (already in w29) */

		a64m_ls(e, A64_STRW, NPC, ctx_mem(&next_pc));

		gen_call_runtime(c, (uintptr_t)&UpdateINTC, true);

		a64m_ls(e, A64_LDRW, NPC, ctx_mem(&next_pc));
		gen_call(c, (uintptr_t)arm64_no_update, false);
		break;

	default:
		die("Invalid block end type");
	}

	return (u32)((e->p - start) * 4);
}

/* The pool, and the caches made to see the code */
static void finalize(arm64_compiler *c, bool rewrite)
{
	const u32 *const code_end = c->e.p;
	u8 *const start = (u8 *)c->e.start;

	a64m_pool_emit(&c->e, &c->pool);
	if (c->e.full)
		die("arm64 recompiler: out of room");

	if (!rewrite)
	{
		c->block->code = (DynarecCodeEntryPtr)(uintptr_t)start;
		c->block->host_code_size = (u32)((u8 *)c->e.p - start);
		c->block->host_opcodes = (u32)(code_end - c->e.start);

		emit_Skip(c->block->host_code_size);
	}

	/* Flush and invalidate caches */
	vmem_platform_flush_cache(CC_RW2RX(start), CC_RW2RX((u8 *)c->e.p), start, (u8 *)c->e.p);
}

/* With the MMU on an access is a call, which asks the MMU. First, though,
 * the table of translations kept by (mmu.h), as the x86-64 and the 32-bit
 * ARM recompiler do: on a hit, straight to the page on the host - x4 is
 * then what to add to the address, which is in x0, to be there. The call
 * is for a miss, and fills the table in. Not for an address that is not a
 * multiple of the access's size, which is an address error and the call's
 * to raise; 64 bits are two accesses of 32 to the SH4, so a multiple of 4
 * will do for them, but then the second half must not be in the next page.
 * x3 and x4 belong to nobody here. */
static bool gen_mmu_lookup(arm64_compiler *c, const uintptr_t *table, u32 size, a64_label *miss)
{
#ifdef MMU_HOST_PAGE_LUT
	a64_emit *e = &c->e;
	if (!mmu_enabled())
		return false;
	if (size == 2 || size == 4 || size == 8)
	{
		a64_logical(e, false, 3, false, A64_ZR, 0, OP_IMM(size == 2 ? 1 : 3));
		a64_b_label(e, A64_NE, 0, miss);
	}
	if (size == 8)
	{
		a64m_addsub(e, false, false, false, 3, 0, OP_IMM(4), false);
		a64_logical(e, false, 3, false, A64_ZR, 3, OP_IMM(0xFFF));
		a64_b_label(e, A64_EQ, 0, miss);
	}
	a64m_shift_imm(e, false, A64_LSR, 3, 0, 12);
	a64m_ldr_lit(e, &c->pool, 0x58000000u, 4, (uintptr_t)table);
	a64m_ls(e, A64_LDRX, 4, a64_mem_reg(4, 3, 3, 3));
	a64m_cbz(e, false, true, 4, miss);
	return true;
#else
	(void)c;
	(void)table;
	(void)size;
	(void)miss;
	return false;
#endif
}

#ifndef MMU_HOST_PAGE_LUT
/* (no tables: gen_mmu_lookup() is never given one) */
#define mmu_read_lut NULL
#define mmu_write_lut NULL
#endif

static u32 op_pc(const RuntimeBlockInfo *block, const shil_opcode *op)
{
	return block->vaddr + op->guest_offs - (op->delay_slot ? 2 : 0);
}

/* The translation of an address known when the block is compiled, with
 * the MMU on: false where it is not to be done here */
static bool translate_const(const RuntimeBlockInfo *block, u32 *addr, u32 size, u32 tt, bool second_page)
{
#ifndef NO_MMU
	u32 paddr, rv = MMU_ERROR_NONE;
	if (!(mmu_enabled() && mmu_is_translated_c(*addr, size, tt)))
		return true;
	/* When full mmu is on, only consider addresses in the same 4k page */
	if ((*addr >> 12) != (block->vaddr >> 12)
			&& (!second_page || (*addr >> 12) != ((block->vaddr + block->guest_opcodes * 2 - 1) >> 12)))
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
		break;
	}
	if (rv != MMU_ERROR_NONE)
		return false;
	*addr = paddr;
#else
	(void)block;
	(void)addr;
	(void)size;
	(void)tt;
	(void)second_page;
#endif
	return true;
}

/* a register offset from x26 (const_mem()), or an address */
static bool mem_is_reg(a64_mem m)
{
	return m.index >= 0;
}

static bool gen_read_memory_immediate(arm64_compiler *c, const shil_opcode *op)
{
	a64_emit *e = &c->e;
	u32 size, addr;
	bool isram = false;
	void *ptr;

	if (!shil_is_imm(&op->rs1))
		return false;

	size = op->flags & 0x7f;
	addr = op->rs1._imm;
	if (!translate_const(c->block, &addr, size, MMU_TT_DREAD, false))
		return false;
	ptr = _vmem_read_ptr(addr, &isram, size > 4 ? 4 : size);

	if (isram)
	{
		const a64_mem at = const_mem(c, ptr, 1);

		if (sh4_regalloc_is_alloc_any(&c->ra, &op->rd))
		{
			switch (size)
			{
			case 1:
				a64m_ls(e, A64_LDRSB, MAPG(c, &op->rd), at);
				break;

			case 2:
				a64m_ls(e, A64_LDRSH, MAPG(c, &op->rd), at);
				break;

			case 4:
				if (shil_is_r32f(&op->rd))
					a64m_ls(e, A64_LDRS, MAPF(c, &op->rd), at);
				else
					a64m_ls(e, A64_LDRW, MAPG(c, &op->rd), at);
				break;

			case 8:
				/* (a pair load has no register offset) */
				if (mem_is_reg(at))
				{
					a64m_addsub(e, true, false, false, 1, at.base, OP_REG(at.index), false);
					a64m_lsp(e, A64_LDPS, 2, MAPF_I(c, &op->rd, 0), MAPF_I(c, &op->rd, 1), a64_mem_off(1, 0));
				}
				else
					a64m_lsp(e, A64_LDPS, 2, MAPF_I(c, &op->rd, 0), MAPF_I(c, &op->rd, 1), at);
				break;

			default:
				die("Invalid size");
				break;
			}
		}
		else
		{
			switch (size)
			{
			case 1:
				a64m_ls(e, A64_LDRSB, 1, at);
				break;

			case 2:
				a64m_ls(e, A64_LDRSH, 1, at);
				break;

			case 4:
				a64m_ls(e, A64_LDRW, 1, at);
				break;

			case 8:
				a64m_ls(e, A64_LDRX, 1, at);
				break;

			default:
				die("Invalid size");
				break;
			}
			a64m_ls(e, size == 8 ? A64_STRX : A64_STRW, 1, ctx_mem(shil_reg_ptr(&op->rd)));
		}
	}
	else
	{
		/* Not RAM */
		if (sh4_area7(addr))
			gen_slice_left(c);
		if (size == 8)
		{
			/* Need to call the handler twice */
			mov_wi(c, 0, addr);
			gen_call_runtime(c, (uintptr_t)ptr, true);
			if (IS_ALLOCF(c, &op->rd))
				a64m_fmov_sw(e, MAPF_I(c, &op->rd, 0), 0);
			else
				a64m_ls(e, A64_STRW, 0, ctx_mem(shil_reg_ptr(&op->rd)));

			mov_wi(c, 0, addr + 4);
			gen_call_runtime(c, (uintptr_t)ptr, true);
			if (IS_ALLOCF(c, &op->rd))
				a64m_fmov_sw(e, MAPF_I(c, &op->rd, 1), 0);
			else
				a64m_ls(e, A64_STRW, 0, ctx_mem((u8*)shil_reg_ptr(&op->rd) + 4));
		}
		else
		{
			mov_wi(c, 0, addr);

			switch (size)
			{
			case 1:
				gen_call_runtime(c, (uintptr_t)ptr, true);
				a64_bfm(e, false, 0, 0, 0, 0, 7);	/* sxtb w0, w0 */
				break;

			case 2:
				gen_call_runtime(c, (uintptr_t)ptr, true);
				a64_bfm(e, false, 0, 0, 0, 0, 15);	/* sxth w0, w0 */
				break;

			case 4:
				gen_call_runtime(c, (uintptr_t)ptr, true);
				break;

			default:
				die("Invalid size");
				break;
			}

			if (IS_ALLOCG(c, &op->rd))
				mov_ww(c, MAPG(c, &op->rd), 0);
			else
			{
				verify(IS_ALLOCF(c, &op->rd));
				a64m_fmov_sw(e, MAPF(c, &op->rd), 0);
			}
		}
	}

	return true;
}

/* A fast access of @kind: one instruction on the SH4's memory as the host
 * has it mapped, which x26 has the address of. The SH4's address is in the
 * general register @addr, as 32 bits, and the data in, or wanted in,
 * @data - a general register, or a floating-point one for the two kinds
 * that are. (Where the host has only 512 MB of the SH4's addresses mapped,
 * the address is cut to that first: two instructions.)
 *
 * One that faults - an address that is not memory - is written over with
 * a call: ngen_Rewrite(), which knows it by what it is. */
static void gen_fast_access(arm64_compiler *c, int kind, int addr, int data)
{
	static const u32 ops[MEM_KINDS] = {
		A64_LDRSB, A64_LDRSH, A64_LDRW, A64_LDRS, A64_LDRX,
		A64_STRB, A64_STRH, A64_STRW, A64_STRS, A64_STRX
	};
	a64_mem at = a64_mem_reg(MEM_BASE, addr, 2, 0);

	if (!_nvmem_4gb_space())
	{
		a64_bfm(&c->e, true, 2, 9, addr, 0, 28);	/* ubfx x9, x<addr>, #0, #29 */
		at = a64_mem_reg(MEM_BASE, 9, 3, 0);
	}
	if (kind < 0 || kind >= MEM_KINDS)
		die("no such access");
	a64m_ls(&c->e, ops[kind], data, at);
}

/* A load as a fast access, straight into the register the block wants it
 * in. False if that is not to be had - no mapping, or the MMU on - and
 * nothing written. */
static bool gen_read_memory_fast(arm64_compiler *c, const shil_opcode *op)
{
	const u32 size = op->flags & 0x7f;
	int addr, kind;

	if (!_nvmem_enabled() || mmu_enabled())
		return false;
	/* the register the block has the address in, or w0 with it worked out */
	addr = gen_mem_addr(c, op, -1);

	if (size == 8)
	{
		gen_fast_access(c, MEM_LOAD64, addr, 0);
		reg_to_param(c, &op->rd, 0, 1);
		return true;
	}
	kind = size == 1 ? MEM_LOAD8 : size == 2 ? MEM_LOAD16 : MEM_LOAD32;
	if (IS_ALLOCG(c, &op->rd))
		gen_fast_access(c, kind, addr, MAPG(c, &op->rd));
	else if (size == 4 && IS_ALLOCF(c, &op->rd))
		gen_fast_access(c, MEM_LOAD32F, addr, MAPF(c, &op->rd));
	else
	{
		gen_fast_access(c, kind, addr, 0);
		reg_to_param(c, &op->rd, 0, 0);
	}
	return true;
}

static void gen_read_memory(arm64_compiler *c, const shil_opcode *op, bool optimise)
{
	a64_emit *e = &c->e;
	u32 size;
	a64_label lut_miss, lut_done;

	if (gen_read_memory_immediate(c, op))
		return;
	/* one instruction, with whatever registers the block has things in; or what follows */
	if (optimise && gen_read_memory_fast(c, op))
		return;

	gen_mem_addr(c, op, 0);
	if (mmu_enabled())
		mov_wi(c, 2, op_pc(c->block, op));	/* pc */

	size = op->flags & 0x7f;
	a64_label_init(&lut_miss);
	a64_label_init(&lut_done);
	if (gen_mmu_lookup(c, mmu_read_lut, size, &lut_miss))
	{
		const a64_mem at = a64_mem_reg(4, 0, 2, 0);
		switch (size)
		{
		case 1:
			a64m_ls(e, A64_LDRSB, 0, at);
			break;
		case 2:
			a64m_ls(e, A64_LDRSH, 0, at);
			break;
		case 8:
			a64m_ls(e, A64_LDRX, 0, at);
			break;
		default:
			a64m_ls(e, A64_LDRW, 0, at);
			break;
		}
		a64_b_label(e, -1, 0, &lut_done);
		a64_label_bind(e, &lut_miss);
		gen_read_memory_slow(c, size);
		a64_label_bind(e, &lut_done);
	}
	else
		gen_read_memory_slow(c, size);

	if (size < 8)
		reg_to_param(c, &op->rd, 0, 0);
	else
		reg_to_param(c, &op->rd, 0, 1);
}

static bool gen_write_memory_immediate(arm64_compiler *c, const shil_opcode *op)
{
	a64_emit *e = &c->e;
	u32 size, addr;
	bool isram = false;
	void *ptr;
	int reg2 = -1;

	if (!shil_is_imm(&op->rs1))
		return false;

	size = op->flags & 0x7f;
	addr = op->rs1._imm;
	if (!translate_const(c->block, &addr, size, MMU_TT_DWRITE, true))
		return false;
	ptr = _vmem_write_ptr(addr, &isram, size > 4 ? 4 : size);

	if (size != 8)
	{
		if (shil_is_imm(&op->rs2))
		{
			mov_wi(c, 1, op->rs2._imm);
			reg2 = 1;
		}
		else if (IS_ALLOCG(c, &op->rs2))
			reg2 = MAPG(c, &op->rs2);
		else if (IS_ALLOCF(c, &op->rs2))
		{
			a64m_fmov_ws(e, 1, MAPF(c, &op->rs2));
			reg2 = 1;
		}
		else
			die("Invalid rs2 param");
	}
	if (isram)
	{
		const a64_mem at = const_mem(c, ptr, 0);

		switch (size)
		{
		case 1:
			a64m_ls(e, A64_STRB, reg2, at);
			break;

		case 2:
			a64m_ls(e, A64_STRH, reg2, at);
			break;

		case 4:
			a64m_ls(e, A64_STRW, reg2, at);
			break;

		case 8:
			if (IS_ALLOCF(c, &op->rs2))
			{
				/* (a pair store has no register offset) */
				if (mem_is_reg(at))
				{
					a64m_addsub(e, true, false, false, 0, at.base, OP_REG(at.index), false);
					a64m_lsp(e, A64_STPS, 2, MAPF_I(c, &op->rs2, 0), MAPF_I(c, &op->rs2, 1), a64_mem_off(0, 0));
				}
				else
					a64m_lsp(e, A64_STPS, 2, MAPF_I(c, &op->rs2, 0), MAPF_I(c, &op->rs2, 1), at);
			}
			else
			{
				param_to_reg(c, &op->rs2, 1, true);
				a64m_ls(e, A64_STRX, 1, at);
			}
			break;

		default:
			die("Invalid size");
			break;
		}
	}
	else
	{
		/* Not RAM */
		if (sh4_area7(addr))
			gen_slice_left(c);
		mov_wi(c, 0, addr);
		if (size == 8)
		{
			/* Need to call the handler twice */
			param_to_reg(c, &op->rs2, 1, true);
			gen_call_runtime(c, (uintptr_t)ptr, true);

			mov_wi(c, 0, addr + 4);
			param_to_reg(c, &op->rs2, 1, true);
			a64m_shift_imm(e, true, A64_LSR, 1, 1, 32);
			gen_call_runtime(c, (uintptr_t)ptr, true);
		}
		else
		{
			if (caller_extends && size == 1)
				a64_bfm(e, false, 2, 1, reg2, 0, 7);	/* uxtb */
			else if (caller_extends && size == 2)
				a64_bfm(e, false, 2, 1, reg2, 0, 15);	/* uxth */
			else
				mov_ww(c, 1, reg2);

			switch (size)
			{
			case 1:
			case 2:
			case 4:
				gen_call_runtime(c, (uintptr_t)ptr, true);
				break;

			default:
				die("Invalid size");
				break;
			}
		}
	}

	return true;
}

/* A store, likewise */
static bool gen_write_memory_fast(arm64_compiler *c, const shil_opcode *op)
{
	const u32 size = op->flags & 0x7f;
	int addr, kind;

	if (!_nvmem_enabled() || mmu_enabled())
		return false;
	addr = gen_mem_addr(c, op, -1);

	if (size == 8)
	{
		param_to_reg(c, &op->rs2, 1, true);
		gen_fast_access(c, MEM_STORE64, addr, 1);
		return true;
	}
	kind = size == 1 ? MEM_STORE8 : size == 2 ? MEM_STORE16 : MEM_STORE32;
	if (shil_is_reg(&op->rs2) && !shil_is_r32f(&op->rs2) && IS_ALLOCG(c, &op->rs2))
		gen_fast_access(c, kind, addr, MAPG(c, &op->rs2));
	else if (size == 4 && shil_is_r32f(&op->rs2) && IS_ALLOCF(c, &op->rs2))
		gen_fast_access(c, MEM_STORE32F, addr, MAPF(c, &op->rs2));
	else
	{
		param_to_reg(c, &op->rs2, 1, false);
		gen_fast_access(c, kind, addr, 1);
	}
	return true;
}

static void gen_write_memory(arm64_compiler *c, const shil_opcode *op, bool optimise)
{
	a64_emit *e = &c->e;
	u32 size;
	a64_label lut_miss, lut_done;

	if (gen_write_memory_immediate(c, op))
		return;
	if (optimise && gen_write_memory_fast(c, op))
		return;

	gen_mem_addr(c, op, 0);
	if (mmu_enabled())
		mov_wi(c, 2, op_pc(c->block, op));	/* pc */

	size = op->flags & 0x7f;
	if (size != 8)
		param_to_reg(c, &op->rs2, 1, false);
	else
		param_to_reg(c, &op->rs2, 1, true);
	a64_label_init(&lut_miss);
	a64_label_init(&lut_done);

	if (gen_mmu_lookup(c, mmu_write_lut, size, &lut_miss))
	{
		const a64_mem at = a64_mem_reg(4, 0, 2, 0);
		switch (size)
		{
		case 1:
			a64m_ls(e, A64_STRB, 1, at);
			break;
		case 2:
			a64m_ls(e, A64_STRH, 1, at);
			break;
		case 8:
			a64m_ls(e, A64_STRX, 1, at);
			break;
		default:
			a64m_ls(e, A64_STRW, 1, at);
			break;
		}
		a64_b_label(e, -1, 0, &lut_done);
		a64_label_bind(e, &lut_miss);
		gen_write_memory_slow(c, size);
		a64_label_bind(e, &lut_done);
	}
	else
		gen_write_memory_slow(c, size);
}

static void check_block(arm64_compiler *c, bool force_checks, RuntimeBlockInfo* block)
{
	a64_emit *e = &c->e;
	a64_label blockcheck_fail, blockcheck_success;

	if (!mmu_enabled() && !force_checks)
		return;

	a64_label_init(&blockcheck_fail);
	a64_label_init(&blockcheck_success);

	if (mmu_enabled())
	{
		a64m_ls(e, A64_LDRW, 10, ctx_mem(&next_pc));
		a64m_ldr_lit(e, &c->pool, 0x18000000u, 11, block->vaddr);
		a64m_addsub(e, false, true, true, A64_ZR, 10, OP_REG(11), false);
		a64_b_label(e, A64_NE, 0, &blockcheck_fail);
	}
	if (force_checks)
	{
		s32 sz = (s32)block->sh4_code_size;
		u8* ptr = GetMemPtr(block->addr, sz);
		if (ptr != NULL)
		{
			a64m_ldr_lit(e, &c->pool, 0x58000000u, 9, (uintptr_t)ptr);

			while (sz > 0)
			{
				if (sz >= 8)
				{
					u64 v;
					memcpy(&v, ptr, 8);
					a64m_ls(e, A64_LDRX, 10, a64_mem_idx(9, 8, A64M_POST));
					a64m_ldr_lit(e, &c->pool, 0x58000000u, 11, v);
					a64m_addsub(e, true, true, true, A64_ZR, 10, OP_REG(11), false);
					sz -= 8;
					ptr += 8;
				}
				else if (sz >= 4)
				{
					u32 v;
					memcpy(&v, ptr, 4);
					a64m_ls(e, A64_LDRW, 10, a64_mem_idx(9, 4, A64M_POST));
					a64m_ldr_lit(e, &c->pool, 0x18000000u, 11, v);
					a64m_addsub(e, false, true, true, A64_ZR, 10, OP_REG(11), false);
					sz -= 4;
					ptr += 4;
				}
				else
				{
					u16 v;
					memcpy(&v, ptr, 2);
					a64m_ls(e, A64_LDRH, 10, a64_mem_idx(9, 2, A64M_POST));
					mov_wi(c, 11, v);
					a64m_addsub(e, false, true, true, A64_ZR, 10, OP_REG(11), false);
					sz -= 2;
					ptr += 2;
				}
				a64_b_label(e, A64_NE, 0, &blockcheck_fail);
			}
		}
	}
	a64_b_label(e, -1, 0, &blockcheck_success);
	a64_label_bind(e, &blockcheck_fail);
	a64m_ldr_lit(e, &c->pool, 0x18000000u, 0, block->addr);
	a64_mov_imm(e, true, A64_IP0, (uintptr_t)&ngen_blockcheckfail);
	a64_br(e, A64_IP0);

	a64_label_bind(e, &blockcheck_success);

	if (mmu_enabled() && block->has_fpu_op)
	{
		a64_label fpu_enabled;
		a64_label_init(&fpu_enabled);
		a64m_ls(e, A64_LDRW, 10, ctx_mem(&sr));
		a64m_tbz(e, false, 10, 15, &fpu_enabled);	/* test SR.FD bit */

		mov_wi(c, 0, block->vaddr);	/* pc */
		mov_wi(c, 1, 0x800);		/* event */
		mov_wi(c, 2, 0x100);		/* vector */
		a64_mov_imm(e, true, A64_IP0, (uintptr_t)&Do_Exception);
		a64_blr(e, A64_IP0);
		a64m_ls(e, A64_LDRW, NPC, ctx_mem(&next_pc));
		gen_call(c, (uintptr_t)arm64_no_update, false);

		a64_label_bind(e, &fpu_enabled);
	}
}

/* The routines a fast access that faulted is written over with a call to:
 * see mem_stubs. Each puts the address and what is stored where a function
 * takes them, calls the one that does the access by asking, and puts what
 * was loaded where the access had it - or, where there is nothing to do
 * afterwards, goes to the function and lets it return for it. */
static void gen_mem_stubs(arm64_compiler *c)
{
	static const uintptr_t routines[MEM_KINDS] = {
		(uintptr_t)&rec_arm64_mem_load8, (uintptr_t)&rec_arm64_mem_load16, (uintptr_t)&rec_arm64_mem_load32,
		(uintptr_t)&rec_arm64_mem_load32, (uintptr_t)&rec_arm64_mem_load64,
		(uintptr_t)&rec_arm64_mem_store8, (uintptr_t)&rec_arm64_mem_store16, (uintptr_t)&rec_arm64_mem_store32,
		(uintptr_t)&rec_arm64_mem_store32, (uintptr_t)&rec_arm64_mem_store64,
	};
	a64_emit *e = &c->e;
	const int regs = 1 + ALLOC_REGS, fregs = ALLOC_FREGS;
	int kind, addr, data;

	verify(regs <= MEM_REGS && fregs <= MEM_REGS);
	memset(mem_stubs, 0, sizeof(mem_stubs));
	for (kind = 0; kind < MEM_KINDS; kind++)
		for (addr = 0; addr < regs; addr++)
		{
			const bool load = kind < MEM_STORE8;
			const int datas = MEM_KIND_F(kind) ? fregs : kind == MEM_LOAD64 || kind == MEM_STORE64 ? 1 : regs;

			for (data = 0; data < datas; data++)
			{
				const bool after = load && (MEM_KIND_F(kind) || data != 0);

				mem_stubs[kind][addr][data] = cursor(c);
				gen_slice_left(c);
				if (after)
					a64m_ls(e, A64_STRX, LR, a64_mem_idx(A64_SP, -16, A64M_PRE));
				/* (neither the address nor what is stored is ever in the other's place) */
				if (addr != 0)
					mov_ww(c, 0, (int)alloc_regs[addr - 1]);
				if (!load && MEM_KIND_F(kind))
					a64m_fmov_ws(e, 1, (int)alloc_fregs[data]);
				else if (!load && data != 0)
					mov_ww(c, 1, (int)alloc_regs[data - 1]);
				if (!after)
				{
					gen_call_runtime(c, routines[kind], false);
					continue;
				}
				gen_call_runtime(c, routines[kind], true);
				if (MEM_KIND_F(kind))
					a64m_fmov_sw(e, (int)alloc_fregs[data], 0);
				else
					mov_ww(c, (int)alloc_regs[data - 1], 0);
				a64m_ls(e, A64_LDRX, LR, a64_mem_idx(A64_SP, 16, A64M_POST));
				a64_ret(e);
			}
		}
}

/* A routine that links blocks finds the block that called it by the
 * address after the call, and that has to be in the block's code as it is
 * when linked: where it cannot be reached with one bl (only on macOS, see
 * out_of_reach()), a block calls one of these, beside the main loop,
 * which goes on to the routine. */
static void gen_link_trampolines(arm64_compiler *c)
{
	int i;
	for (i = 0; i < 3; i++)
	{
		link_trampolines[i] = NULL;
		if (!out_of_reach(c, (uintptr_t)link_stubs[i]))
			continue;
		link_trampolines[i] = (a64_func)(uintptr_t)cursor(c);
		a64_mov_imm(&c->e, true, A64_IP0, (uintptr_t)link_stubs[i]);
		a64_br(&c->e, A64_IP0);
	}
}

static void gen_mainloop(arm64_compiler *c)
{
	a64_emit *e = &c->e;
	a64_label no_update, intc_sched, end_mainloop, do_interrupts;

	a64_label_init(&no_update);
	a64_label_init(&intc_sched);
	a64_label_init(&end_mainloop);
	a64_label_init(&do_interrupts);

	/* int intc_sched() */
	arm64_intc_sched = (int (*)(void))(uintptr_t)cursor(c);
	a64_b_label(e, -1, 0, &intc_sched);

	/* void no_update() */
	a64_label_bind(e, &no_update);				/* next_pc _MUST_ be on w29 */

	a64m_ls(e, A64_LDRW, 0, a64_mem_off(CTX, offsetof(Sh4Context, CpuRunning)));
	a64m_cbz(e, false, false, 0, &end_mainloop);
	if (!mmu_enabled())
	{
		a64m_addsub(e, true, true, false, 2, CTX, OP_IMM(offsetof(Sh4RCB, cntx)), false);
		if (RAM_SIZE == 32 * 1024 * 1024)
			a64_ubfx(e, 1, NPC, 1, 24);	/* 24+1 bits: 32 MB */
		else if (RAM_SIZE == 16 * 1024 * 1024)
			a64_ubfx(e, 1, NPC, 1, 23);	/* 23+1 bits: 16 MB */
		else
			die("Unsupported RAM_SIZE");
		a64m_ls(e, A64_LDRX, 0, a64_mem_reg(2, 1, 3, 3));
	}
	else
	{
		mov_ww(c, 0, NPC);
		gen_call_runtime(c, (uintptr_t)&bm_GetCodeByVAddr, true);
	}
	a64_br(e, 0);

	/* void mainloop(void *context) */
	mainloop = (void (*)(void *))(uintptr_t)CC_RW2RX(cursor(c));

	/* Save registers */
	a64m_lsp(e, A64_STPX, 3, 19, 20, a64_mem_idx(A64_SP, -160, A64M_PRE));
	a64m_lsp(e, A64_STPX, 3, 21, 22, a64_mem_off(A64_SP, 16));
	a64m_lsp(e, A64_STPX, 3, 23, 24, a64_mem_off(A64_SP, 32));
	a64m_lsp(e, A64_STPX, 3, 25, 26, a64_mem_off(A64_SP, 48));
	a64m_lsp(e, A64_STPX, 3, 27, 28, a64_mem_off(A64_SP, 64));
	a64m_lsp(e, A64_STPS, 2, 14, 15, a64_mem_off(A64_SP, 80));
	a64m_lsp(e, A64_STPS, 2, 8, 9, a64_mem_off(A64_SP, 96));
	a64m_lsp(e, A64_STPS, 2, 10, 11, a64_mem_off(A64_SP, 112));
	a64m_lsp(e, A64_STPS, 2, 12, 13, a64_mem_off(A64_SP, 128));
	a64m_lsp(e, A64_STPX, 3, 29, 30, a64_mem_off(A64_SP, 144));

	a64m_addsub(e, true, true, false, 0, 0, OP_IMM(sizeof(Sh4Context)), false);
	/* Push context, and what is left of the time slice (see ngen_Compile()):
	 * what a block compiled with the MMU on goes by. Whether the MMU is on
	 * or not, so that the stack is the same either way - the MMU can come
	 * on while this is running, and what is compiled and run before this is
	 * left then writes to that place, and may leave by the end of a main
	 * loop made after this one. */
	a64_mov_imm(e, true, 1, SH4_TIMESLICE);
	a64m_lsp(e, A64_STPX, 3, 0, 1, a64_mem_idx(A64_SP, -16, A64M_PRE));
	if (mmu_enabled())
	{
		a64m_ldr_lit(e, &c->pool, 0x58000000u, 0, (uintptr_t)jmp_env);
		a64m_ldr_lit(e, &c->pool, 0x58000000u, 1, (uintptr_t)&setjmp);
		a64_blr(e, 1);

		/* (here at the start, and again after every longjmp) */
		a64m_ls(e, A64_LDRX, CTX, a64_mem_off(A64_SP, 0));	/* Set context */
		a64m_ls(e, A64_LDRW, CYC, a64_mem_off(A64_SP, 8));
	}
	else
	{
		/* Use x28 as sh4 context pointer */
		mov_xx(c, CTX, 0);
		/* Use x27 as cycle_counter */
		mov_wi(c, CYC, SH4_TIMESLICE);
	}
	/* the SH4's memory, where the host has it mapped, comes straight after its context */
	a64m_addsub(e, true, false, false, MEM_BASE, CTX, OP_IMM(sizeof(Sh4Context)), false);

	/* w29 is next_pc */
	a64m_ls(e, A64_LDRW, NPC, a64_mem_off(CTX, offsetof(Sh4Context, pc)));
	a64_b_label(e, -1, 0, &no_update);

	a64_label_bind(e, &intc_sched);

	/* Add timeslice to cycle counter, and to the copy of it if there is one */
	a64m_addsub(e, false, false, false, CYC, CYC, OP_IMM(SH4_TIMESLICE), false);
	if (mmu_enabled())
		a64m_ls(e, A64_STRW, CYC, a64_mem_off(A64_SP, 8));
	mov_xx(c, NPC, LR);		/* Trashing pc here but it will be reset at the end of the block or in DoInterrupts */
	/* (the timers' time is the end of the slice here: see cycle_counter in sh4_if.h) */
	mov_wi(c, 0, SH4_TIMESLICE);
	a64m_ls(e, A64_STRW, 0, a64_mem_off(CTX, offsetof(Sh4Context, cycle_counter)));
	gen_call_runtime(c, (uintptr_t)&UpdateSystem, true);
	mov_xx(c, LR, NPC);
	a64m_cbz(e, true, false, 0, &do_interrupts);
	a64m_ls(e, A64_LDRW, 0, a64_mem_off(CTX, offsetof(Sh4Context, CpuRunning)));
	a64_ret(e);

	a64_label_bind(e, &do_interrupts);
	mov_xx(c, 0, NPC);
	gen_call_runtime(c, (uintptr_t)&rdv_DoInterrupts, true);	/* Updates next_pc based on host pc */
	mov_ww(c, NPC, 0);

	a64_b_label(e, -1, 0, &no_update);

	a64_label_bind(e, &end_mainloop);
	/* Pop context */
	a64m_addsub(e, true, false, false, A64_SP, A64_SP, OP_IMM(16), true);
	/* Restore registers */
	a64m_lsp(e, A64_LDPX, 3, 29, 30, a64_mem_off(A64_SP, 144));
	a64m_lsp(e, A64_LDPS, 2, 12, 13, a64_mem_off(A64_SP, 128));
	a64m_lsp(e, A64_LDPS, 2, 10, 11, a64_mem_off(A64_SP, 112));
	a64m_lsp(e, A64_LDPS, 2, 8, 9, a64_mem_off(A64_SP, 96));
	a64m_lsp(e, A64_LDPS, 2, 14, 15, a64_mem_off(A64_SP, 80));
	a64m_lsp(e, A64_LDPX, 3, 27, 28, a64_mem_off(A64_SP, 64));
	a64m_lsp(e, A64_LDPX, 3, 25, 26, a64_mem_off(A64_SP, 48));
	a64m_lsp(e, A64_LDPX, 3, 23, 24, a64_mem_off(A64_SP, 32));
	a64m_lsp(e, A64_LDPX, 3, 21, 22, a64_mem_off(A64_SP, 16));
	a64m_lsp(e, A64_LDPX, 3, 19, 20, a64_mem_idx(A64_SP, 160, A64M_POST));
	a64_ret(e);

	gen_mem_stubs(c);
	gen_link_trampolines(c);

	a64m_pool_emit(e, &c->pool);
	if (e->full)
		die("arm64 recompiler: out of room");
	emit_Skip((u32)((u8 *)e->p - (u8 *)e->start));

	arm64_no_update = (void (*)(void))(uintptr_t)no_update.at;

	/* Flush and invalidate caches */
	vmem_platform_flush_cache(CC_RW2RX((u8 *)e->start), CC_RW2RX((u8 *)e->p), e->start, e->p);
}

static void gen_op(arm64_compiler *c, shil_opcode *op, RuntimeBlockInfo *block, bool optimise)
{
	a64_emit *e = &c->e;
	switch (op->op)
	{
	case shop_ifb:	/* Interpreter fallback */
		if (op->rs1._imm)	/* if NeedPC() */
		{
			mov_wi(c, 10, op->rs2._imm);
			a64m_ls(e, A64_STRW, 10, ctx_mem(&next_pc));
		}
		mov_wi(c, 0, op->rs3._imm);
		gen_slice_left(c);

		if (!mmu_enabled())
			gen_call_runtime(c, (uintptr_t)OpDesc[op->rs3._imm]->oph, true);
		else
		{
			a64_mov_imm(e, true, 1, (uintptr_t)OpDesc[op->rs3._imm]->oph);	/* op handler */
			mov_wi(c, 2, block->vaddr + op->guest_offs - (op->delay_slot ? 1 : 0));	/* pc */

			gen_call_runtime(c, (uintptr_t)&rec_arm64_interpreter_fallback, true);
		}
		break;

	case shop_jcond:
	case shop_jdyn:
		{
			const int rd = MAPG(c, &op->rd);
			if (shil_is_imm(&op->rs2))
				a64m_addsub(e, false, false, false, rd, MAPG(c, &op->rs1), OP_IMM(op->rs2._imm), false);
			else
				mov_ww(c, rd, MAPG(c, &op->rs1));
			/* Save it for the branching at the end of the block */
			mov_ww(c, NPC, rd);
		}
		break;

	case shop_mov32:
		verify(shil_is_reg(&op->rd));
		verify(shil_is_reg(&op->rs1) || shil_is_imm(&op->rs1));

		if (IS_ALLOCF(c, &op->rd))
		{
			const int rd = MAPF(c, &op->rd);
			if (shil_is_imm(&op->rs1))
				a64m_fmov_imm(e, &c->pool, rd, op->rs1._imm);
			else if (IS_ALLOCF(c, &op->rs1))
				a64m_fp2(e, A64_FMOV_S, rd, MAPF(c, &op->rs1));
			else
				a64m_fmov_sw(e, rd, MAPG(c, &op->rs1));
		}
		else
		{
			const int rd = MAPG(c, &op->rd);
			if (shil_is_imm(&op->rs1))
				mov_wi(c, rd, op->rs1._imm);
			else if (IS_ALLOCG(c, &op->rs1))
				mov_ww(c, rd, MAPG(c, &op->rs1));
			else
				a64m_fmov_ws(e, rd, MAPF(c, &op->rs1));
		}
		break;

	case shop_mov64:
		verify(shil_is_reg(&op->rd));
		verify(shil_is_reg(&op->rs1) || shil_is_imm(&op->rs1));

		if (IS_ALLOCF(c, &op->rd))
		{
			const int rd0 = MAPF_I(c, &op->rd, 0);
			const int rd1 = MAPF_I(c, &op->rd, 1);
			const int rs0 = MAPF_I(c, &op->rs1, 0);
			const int rs1 = MAPF_I(c, &op->rs1, 1);
			/* The allocator can give the first of the destination the
			 * register the second of the source is in (a source used for
			 * the last time gives its register up): that one is moved out
			 * of the way first. */
			if (rd0 == rs1)
			{
				a64m_fp2(e, A64_FMOV_S, 0, rs1);
				a64m_fp2(e, A64_FMOV_S, rd0, rs0);
				a64m_fp2(e, A64_FMOV_S, rd1, 0);
			}
			else
			{
				if (rd0 != rs0)
					a64m_fp2(e, A64_FMOV_S, rd0, rs0);
				if (rd1 != rs1)
					a64m_fp2(e, A64_FMOV_S, rd1, rs1);
			}
		}
		else
		{
			param_to_reg(c, &op->rs1, 15, true);
			reg_to_param(c, &op->rd, 15, 1);
		}
		break;

	case shop_readm:
		gen_read_memory(c, op, optimise);
		break;

	case shop_writem:
		gen_write_memory(c, op, optimise);
		break;

	case shop_sync_sr:
		gen_call_runtime(c, (uintptr_t)&UpdateSR, true);
		break;
	case shop_sync_fpscr:
		gen_call_runtime(c, (uintptr_t)&UpdateFPSCR, true);
		break;

	case shop_swaplb:
		{
			const int rs1 = MAPG(c, &op->rs1);
			const int rd = MAPG(c, &op->rd);
			a64m_shift_imm(e, false, A64_LSR, 9, rs1, 16);
			a64m_rev16(e, rd, rs1);
			a64_bfi(e, rd, 9, 16, 16);
		}
		break;

	case shop_neg:
		a64m_addsub(e, false, true, false, MAPG(c, &op->rd), A64_ZR, OP_REG(MAPG(c, &op->rs1)), false);
		break;
	case shop_not:
		a64_mvn_op(e, false, MAPG(c, &op->rd), OP_REG(MAPG(c, &op->rs1)));
		break;

	case shop_and:
		gen_binary_op(c, op, 0);
		break;
	case shop_or:
		gen_binary_op(c, op, 1);
		break;
	case shop_xor:
		gen_binary_op(c, op, 2);
		break;
	case shop_add:
		gen_binary_op(c, op, 3);
		break;
	case shop_sub:
		gen_binary_op(c, op, 4);
		break;
	case shop_shl:
	case shop_shr:
	case shop_sar:
	case shop_ror:
		{
			const int shift = op->op == shop_shl ? A64_LSL : op->op == shop_shr ? A64_LSR
					: op->op == shop_sar ? A64_ASR : A64_ROR;
			if (shil_is_imm(&op->rs2))
				a64m_shift_imm(e, false, shift, MAPG(c, &op->rd), MAPG(c, &op->rs1), op->rs2._imm);
			else if (shil_is_reg(&op->rs2))
				a64m_shift_reg(e, false, shift, MAPG(c, &op->rd), MAPG(c, &op->rs1), MAPG(c, &op->rs2));
		}
		break;

	case shop_adc:
		{
			int reg1, reg3;
			a64_operand op2;
			if (shil_is_imm(&op->rs1))
			{
				mov_wi(c, 0, shil_imm_value(&op->rs1));
				reg1 = 0;
			}
			else
				reg1 = MAPG(c, &op->rs1);
			if (shil_is_imm(&op->rs2))
				op2 = OP_IMM(shil_imm_value(&op->rs2));
			else
				op2 = OP_REG(MAPG(c, &op->rs2));
			if (shil_is_imm(&op->rs3))
			{
				mov_wi(c, 1, shil_imm_value(&op->rs3));
				reg3 = 1;
			}
			else
				reg3 = MAPG(c, &op->rs3);
			a64m_addsub(e, false, true, true, A64_ZR, reg3, OP_IMM(1), false);	/* C = rs3 */
			a64m_addsub_carry(e, false, true, MAPG(c, &op->rd), reg1, op2);	/* (C,rd)=rs1+rs2+rs3(C) */
			a64m_cset(e, MAPG(c, &op->rd2), A64_HS);	/* rd2 = C */
		}
		break;
	case shop_sbc:
		{
			int reg1;
			a64_operand op2, op3;
			if (shil_is_imm(&op->rs1))
			{
				mov_wi(c, 0, shil_imm_value(&op->rs1));
				reg1 = 0;
			}
			else
				reg1 = MAPG(c, &op->rs1);
			if (shil_is_imm(&op->rs2))
				op2 = OP_IMM(shil_imm_value(&op->rs2));
			else
				op2 = OP_REG(MAPG(c, &op->rs2));
			if (shil_is_imm(&op->rs3))
				op3 = OP_IMM(shil_imm_value(&op->rs3));
			else
				op3 = OP_REG(MAPG(c, &op->rs3));
			a64m_addsub(e, false, true, true, A64_ZR, A64_ZR, op3, false);	/* C = ~rs3 */
			a64m_addsub_carry(e, true, true, MAPG(c, &op->rd), reg1, op2);	/* (C,rd) = rs1 - rs2 - ~rs3(C) */
			a64m_cset(e, MAPG(c, &op->rd2), A64_LO);	/* rd2 = ~C */
		}
		break;
	case shop_negc:
		{
			a64_operand op1, op2;
			if (shil_is_imm(&op->rs1))
				op1 = OP_IMM(shil_imm_value(&op->rs1));
			else
				op1 = OP_REG(MAPG(c, &op->rs1));
			if (shil_is_imm(&op->rs2))
				op2 = OP_IMM(shil_imm_value(&op->rs2));
			else
				op2 = OP_REG(MAPG(c, &op->rs2));
			a64m_addsub(e, false, true, true, A64_ZR, A64_ZR, op2, false);	/* C = ~rs2 */
			a64m_addsub_carry(e, true, true, MAPG(c, &op->rd), A64_ZR, op1);	/* (C,rd) = 0 - rs1 - ~rs2(C) */
			a64m_cset(e, MAPG(c, &op->rd2), A64_LO);	/* rd2 = ~C */
		}
		break;

	case shop_rocr:
		{
			int reg1, reg2, rd;
			if (shil_is_imm(&op->rs1))
			{
				mov_wi(c, 1, shil_imm_value(&op->rs1));
				reg1 = 1;
			}
			else
				reg1 = MAPG(c, &op->rs1);
			if (shil_is_imm(&op->rs2))
			{
				mov_wi(c, 2, shil_imm_value(&op->rs2));
				reg2 = 2;
			}
			else
				reg2 = MAPG(c, &op->rs2);
			a64_ubfx(e, 0, reg1, 0, 1);		/* w0 = rs1[0] (new C) */
			rd = MAPG(c, &op->rd);
			a64m_shift_imm(e, false, A64_LSR, rd, reg1, 1);	/* rd = rs1 >> 1 */
			a64_bfi(e, rd, reg2, 31, 1);		/* rd |= C << 31 */
			mov_ww(c, MAPG(c, &op->rd2), 0);	/* rd2 = w0 (new C) */
		}
		break;
	case shop_rocl:
		{
			int reg1, reg2;
			if (shil_is_imm(&op->rs1))
			{
				mov_wi(c, 0, shil_imm_value(&op->rs1));
				reg1 = 0;
			}
			else
				reg1 = MAPG(c, &op->rs1);
			if (shil_is_imm(&op->rs2))
			{
				mov_wi(c, 1, shil_imm_value(&op->rs2));
				reg2 = 1;
			}
			else
				reg2 = MAPG(c, &op->rs2);
			a64_logical(e, false, 3, false, A64_ZR, reg1, OP_IMM(0x80000000u));	/* Z = ~rs1[31] */
			a64_logical(e, false, 1, false, MAPG(c, &op->rd), reg2, a64_opnd_reg(reg1, A64_LSL, 1));	/* rd = rs1 << 1 | rs2(C) */
			a64m_cset(e, MAPG(c, &op->rd2), A64_NE);	/* rd2 = ~Z(C) */
		}
		break;

	case shop_shld:
	case shop_shad:
		{
			int reg1, rs2, rd;
			a64_label positive_shift, negative_shift, end;
			a64_label_init(&positive_shift);
			a64_label_init(&negative_shift);
			a64_label_init(&end);
			if (shil_is_imm(&op->rs1))
			{
				mov_wi(c, 0, shil_imm_value(&op->rs1));
				reg1 = 0;
			}
			else
				reg1 = MAPG(c, &op->rs1);
			rs2 = MAPG(c, &op->rs2);
			a64m_tbz(e, false, rs2, 31, &positive_shift);
			a64m_addsub(e, false, false, true, A64_ZR, rs2, OP_IMM(32), false);
			a64_b_label(e, A64_NE, 0, &negative_shift);
			rd = MAPG(c, &op->rd);
			/* rs2 == -32 => rd = 0 (logical) or 0/-1 (arith) */
			if (op->op == shop_shld)
				mov_ww(c, rd, A64_ZR);
			else
				a64m_shift_imm(e, false, A64_ASR, rd, reg1, 31);
			a64_b_label(e, -1, 0, &end);

			a64_label_bind(e, &positive_shift);
			/* rs2 >= 0 => left shift */
			a64m_shift_reg(e, false, A64_LSL, rd, reg1, rs2);
			a64_b_label(e, -1, 0, &end);

			a64_label_bind(e, &negative_shift);
			/* rs2 < 0 => right shift */
			a64m_addsub(e, false, true, false, 1, A64_ZR, OP_REG(rs2), false);
			a64m_shift_reg(e, false, op->op == shop_shld ? A64_LSR : A64_ASR, rd, reg1, 1);
			a64_label_bind(e, &end);
		}
		break;

	case shop_test:
	case shop_seteq:
	case shop_setge:
	case shop_setgt:
	case shop_setae:
	case shop_setab:
		{
			static const u32 conditions[] = { 0 /* eq */, 0 /* eq */, 10 /* ge */, 12 /* gt */, 2 /* hs */, 8 /* hi */ };
			const int rs1 = MAPG(c, &op->rs1);
			if (op->op == shop_test)
			{
				if (shil_is_imm(&op->rs2))
					a64_logical(e, false, 3, false, A64_ZR, rs1, OP_IMM(op->rs2._imm));
				else
					a64_logical(e, false, 3, false, A64_ZR, rs1, OP_REG(MAPG(c, &op->rs2)));
			}
			else
			{
				if (shil_is_imm(&op->rs2))
					a64m_addsub(e, false, true, true, A64_ZR, rs1, OP_IMM(op->rs2._imm), false);
				else
					a64m_addsub(e, false, true, true, A64_ZR, rs1, OP_REG(MAPG(c, &op->rs2)), false);
			}

			a64m_cset(e, MAPG(c, &op->rd), conditions[op->op - shop_test]);
		}
		break;
	case shop_setpeq:
		{
			int reg1, reg2, rd;
			if (shil_is_imm(&op->rs1))
			{
				mov_wi(c, 0, shil_imm_value(&op->rs1));
				reg1 = 0;
			}
			else
				reg1 = MAPG(c, &op->rs1);
			if (shil_is_imm(&op->rs2))
			{
				mov_wi(c, 1, shil_imm_value(&op->rs2));
				reg2 = 1;
			}
			else
				reg2 = MAPG(c, &op->rs2);
			a64_logical(e, false, 2, false, 1, reg1, OP_REG(reg2));
			rd = MAPG(c, &op->rd);
			mov_ww(c, rd, A64_ZR);
			mov_ww(c, 2, A64_ZR);	/* wzr not supported by csinc (?!) */
			a64_logical(e, false, 3, false, A64_ZR, 1, OP_IMM(0xFF000000u));
			a64m_csinc(e, rd, rd, 2, A64_NE);
			a64_logical(e, false, 3, false, A64_ZR, 1, OP_IMM(0x00FF0000u));
			a64m_csinc(e, rd, rd, 2, A64_NE);
			a64_logical(e, false, 3, false, A64_ZR, 1, OP_IMM(0x0000FF00u));
			a64m_csinc(e, rd, rd, 2, A64_NE);
			a64_logical(e, false, 3, false, A64_ZR, 1, OP_IMM(0x000000FFu));
			a64m_csinc(e, rd, rd, 2, A64_NE);
		}
		break;

	case shop_mul_u16:
	case shop_mul_s16:
	case shop_mul_i32:
		{
			int reg2;
			if (shil_is_imm(&op->rs2))
			{
				mov_wi(c, 0, shil_imm_value(&op->rs2));
				reg2 = 0;
			}
			else
				reg2 = MAPG(c, &op->rs2);
			if (op->op == shop_mul_i32)
				a64m_mul(e, MAPG(c, &op->rd), MAPG(c, &op->rs1), reg2);
			else
			{
				/* uxth / sxth */
				const u32 opc = op->op == shop_mul_u16 ? 2 : 0;
				a64_bfm(e, false, opc, 10, MAPG(c, &op->rs1), 0, 15);
				a64_bfm(e, false, opc, 11, reg2, 0, 15);
				a64m_mul(e, MAPG(c, &op->rd), 10, 11);
			}
		}
		break;
	case shop_mul_u64:
	case shop_mul_s64:
		{
			int reg2, rd;
			if (shil_is_imm(&op->rs2))
			{
				mov_wi(c, 0, shil_imm_value(&op->rs2));
				reg2 = 0;
			}
			else
				reg2 = MAPG(c, &op->rs2);
			rd = MAPG(c, &op->rd);
			a64m_mull(e, op->op == shop_mul_s64, rd, MAPG(c, &op->rs1), reg2);
			a64m_shift_imm(e, true, A64_LSR, MAPG(c, &op->rd2), rd, 32);
		}
		break;

	case shop_pref:
		{
			a64_label not_sqw;
			a64_label_init(&not_sqw);
			if (shil_is_imm(&op->rs1))
				mov_wi(c, 0, op->rs1._imm);
			else
			{
				if (IS_ALLOCG(c, &op->rs1))
					a64m_shift_imm(e, false, A64_LSR, 1, MAPG(c, &op->rs1), 26);
				else
				{
					a64m_ls(e, A64_LDRW, 0, ctx_mem(shil_reg_ptr(&op->rs1)));
					a64m_shift_imm(e, false, A64_LSR, 1, 0, 26);
				}
				a64m_addsub(e, false, true, true, A64_ZR, 1, OP_IMM(0x38), false);
				a64_b_label(e, A64_NE, 0, &not_sqw);
				if (IS_ALLOCG(c, &op->rs1))
					mov_ww(c, 0, MAPG(c, &op->rs1));
			}

			if (mmu_enabled())
			{
				mov_wi(c, 1, block->vaddr + op->guest_offs - (op->delay_slot ? 1 : 0));	/* pc */

				gen_call_runtime(c, (uintptr_t)&rec_arm64_do_sqw_mmu_no_ex, true);
			}
			else
			{
				if (CCN_MMUCR.AT)
					a64m_ldr_lit(e, &c->pool, 0x58000000u, 9, (uintptr_t)&do_sqw_mmu);
				else
				{
					a64m_addsub(e, true, true, false, 9, CTX, OP_IMM(offsetof(Sh4RCB, cntx) - offsetof(Sh4RCB, do_sqw_nommu)), false);
					a64m_ls(e, A64_LDRX, 9, a64_mem_off(9, 0));
					a64m_addsub(e, true, true, false, 1, CTX, OP_IMM(offsetof(Sh4RCB, cntx) - offsetof(Sh4RCB, sq_buffer)), false);
				}
				a64_blr(e, 9);
			}
			a64_label_bind(e, &not_sqw);
		}
		break;

	case shop_ext_s8:
		a64_bfm(e, false, 0, MAPG(c, &op->rd), MAPG(c, &op->rs1), 0, 7);
		break;
	case shop_ext_s16:
		a64_bfm(e, false, 0, MAPG(c, &op->rd), MAPG(c, &op->rs1), 0, 15);
		break;

	case shop_xtrct:
		{
			/* The low half of the one and the high half of the other.
			 * Either may be a number the optimiser found, which is no
			 * register to look up: its half is put in w0 as it is. */
			const int rd = MAPG(c, &op->rd);
			if (shil_is_imm(&op->rs1) && shil_is_imm(&op->rs2))
				mov_wi(c, rd, (op->rs1._imm >> 16) | (op->rs2._imm << 16));
			else if (shil_is_imm(&op->rs1))
			{
				mov_wi(c, 0, op->rs1._imm >> 16);
				a64_logical(e, false, 1, false, rd, 0, a64_opnd_reg(MAPG(c, &op->rs2), A64_LSL, 16));
			}
			else if (shil_is_imm(&op->rs2))
			{
				mov_wi(c, 0, op->rs2._imm << 16);
				a64_logical(e, false, 1, false, rd, 0, a64_opnd_reg(MAPG(c, &op->rs1), A64_LSR, 16));
			}
			else
			{
				const int rs1 = MAPG(c, &op->rs1);
				const int rs2 = MAPG(c, &op->rs2);
				if (op->rs1._imm == op->rd._imm)
				{
					verify(op->rs2._imm != op->rd._imm);
					a64m_shift_imm(e, false, A64_LSR, rd, rs1, 16);
					a64m_shift_imm(e, false, A64_LSL, 0, rs2, 16);
				}
				else
				{
					a64m_shift_imm(e, false, A64_LSL, rd, rs2, 16);
					a64m_shift_imm(e, false, A64_LSR, 0, rs1, 16);
				}
				a64_logical(e, false, 1, false, rd, rd, OP_REG(0));
			}
		}
		break;

	/* FPU */

	case shop_fadd:
		gen_binary_fop(c, op, A64_FADD_S);
		break;
	case shop_fsub:
		gen_binary_fop(c, op, A64_FSUB_S);
		break;
	case shop_fmul:
		gen_binary_fop(c, op, A64_FMUL_S);
		break;
	case shop_fdiv:
		gen_binary_fop(c, op, A64_FDIV_S);
		break;

	case shop_fabs:
		a64m_fp2(e, A64_FABS_S, MAPF(c, &op->rd), MAPF(c, &op->rs1));
		break;
	case shop_fneg:
		a64m_fp2(e, A64_FNEG_S, MAPF(c, &op->rd), MAPF(c, &op->rs1));
		break;
	case shop_fsqrt:
		a64m_fp2(e, A64_FSQRT_S, MAPF(c, &op->rd), MAPF(c, &op->rs1));
		break;

	case shop_fmac:
		a64m_fmadd(e, MAPF(c, &op->rd), MAPF(c, &op->rs3), MAPF(c, &op->rs2), MAPF(c, &op->rs1));
		break;

	case shop_fsrra:
		a64m_fp2(e, A64_FSQRT_S, 0, MAPF(c, &op->rs1));
		a64m_fmov_imm(e, &c->pool, 1, 0x3f800000u);
		a64m_fp3(e, A64_FDIV_S, MAPF(c, &op->rd), 1, 0);
		break;

	case shop_fsetgt:
	case shop_fseteq:
		a64m_fcmp(e, MAPF(c, &op->rs1), MAPF(c, &op->rs2));
		a64m_cset(e, MAPG(c, &op->rd), op->op == shop_fsetgt ? A64_GT : A64_EQ);
		break;

	case shop_fsca:
		a64_mov_imm(e, true, 1, (uintptr_t)&sin_table);
		if (shil_is_reg(&op->rs1))
			a64m_addsub_ext(e, true, false, false, 1, 1, MAPG(c, &op->rs1), 1 /* uxth */, 3);
		else
			a64m_addsub(e, true, false, false, 1, 1, OP_IMM((u32)(shil_imm_value(&op->rs1) << 3)), false);
		if (IS_ALLOCF(c, &op->rd))
			a64m_lsp(e, A64_LDPS, 2, MAPF_I(c, &op->rd, 0), MAPF_I(c, &op->rd, 1), a64_mem_off(1, 0));
		else
		{
			a64m_ls(e, A64_LDRX, 2, a64_mem_off(1, 0));
			a64m_ls(e, A64_STRX, 2, ctx_mem(shil_reg_ptr(&op->rd)));
		}
		break;

	/* FIPR and FTRV in doubles, the sums in the order the reference
	 * implementations in hw/sh4/dyna/shil_canonical.h take them, as the
	 * interpreter and the x86-64 recompiler do: a float times a float is
	 * exact in a double, so the only roundings are those of the sums and of
	 * the result, and the result is theirs to the last bit. (Fused in
	 * doubles is the same as not: the product has nothing to round.) */
	case shop_fipr:
		a64m_addsub(e, true, false, false, 9, CTX, OP_IMM(ctx_mem(shil_reg_ptr(&op->rs1)).offset), false);
		a64_dw(e, 0x4C407800u | (9u << 5) | 0);		/* ld1 {v0.4s}, [x9] */
		a64m_addsub(e, true, false, false, 9, CTX, OP_IMM(ctx_mem(shil_reg_ptr(&op->rs2)).offset), false);
		a64_dw(e, 0x4C407800u | (9u << 5) | 1);		/* ld1 {v1.4s}, [x9] */
		a64_dw(e, 0x0E617800u | (0u << 5) | 16);	/* fcvtl v16.2d, v0.2s */
		a64_dw(e, 0x4E617800u | (0u << 5) | 17);	/* fcvtl2 v17.2d, v0.4s */
		a64_dw(e, 0x0E617800u | (1u << 5) | 18);	/* fcvtl v18.2d, v1.2s */
		a64_dw(e, 0x4E617800u | (1u << 5) | 19);	/* fcvtl2 v19.2d, v1.4s */
		a64_dw(e, 0x6E60DC00u | (18u << 16) | (16u << 5) | 16);	/* fmul v16.2d, v16.2d, v18.2d: the first two products */
		a64_dw(e, 0x6E60DC00u | (19u << 16) | (17u << 5) | 17);	/* fmul v17.2d, v17.2d, v19.2d: and the last two */
		a64_dw(e, 0x7E70D800u | (16u << 5) | 16);	/* faddp d16, v16.2d: first + second */
		a64m_fp3(e, 0x1E602800u, 16, 16, 17);		/* fadd d16, d16, d17: + third */
		a64_dw(e, 0x5E180400u | (17u << 5) | 17);	/* mov d17, v17.d[1] */
		a64m_fp3(e, 0x1E602800u, 16, 16, 17);		/* + fourth */
		a64m_fp2(e, A64_FCVT_SD, MAPF(c, &op->rd), 16);
		break;

	case shop_ftrv:
		{
			int col;
			a64m_addsub(e, true, false, false, 9, CTX, OP_IMM(ctx_mem(shil_reg_ptr(&op->rs1)).offset), false);
			a64_dw(e, 0x4C407800u | (9u << 5) | 0);		/* ld1 {v0.4s}, [x9] */
			a64m_addsub(e, true, false, false, 9, CTX, OP_IMM(ctx_mem(shil_reg_ptr(&op->rs2)).offset), false);
			for (col = 1; col <= 4; col++)
				a64_dw(e, 0x4CDF7800u | (9u << 5) | (u32)col);	/* ld1 {v<col>.4s}, [x9], #16 */
			/* the vector's elements, and each column times its element, two rows to a register */
			a64_dw(e, 0x0E617800u | (0u << 5) | 6);		/* fcvtl v6.2d, v0.2s */
			a64_dw(e, 0x4E617800u | (0u << 5) | 7);		/* fcvtl2 v7.2d, v0.4s */
			for (col = 0; col < 4; col++)
			{
				const u32 element = col < 2 ? 6 : 7, h = (u32)(col & 1) << 11;
				const u32 column = 1 + (u32)col;
				a64_dw(e, 0x0E617800u | (column << 5) | 18);	/* fcvtl v18.2d: rows 0 and 1 */
				a64_dw(e, 0x4E617800u | (column << 5) | 19);	/* fcvtl2 v19.2d: rows 2 and 3 */
				if (col == 0)
				{
					a64_dw(e, 0x4FC09000u | h | (element << 16) | (18u << 5) | 16);	/* fmul v16.2d, v18.2d, v<element>.d[] */
					a64_dw(e, 0x4FC09000u | h | (element << 16) | (19u << 5) | 17);
				}
				else
				{
					a64_dw(e, 0x4FC01000u | h | (element << 16) | (18u << 5) | 16);	/* fmla */
					a64_dw(e, 0x4FC01000u | h | (element << 16) | (19u << 5) | 17);
				}
			}
			a64_dw(e, 0x0E616800u | (16u << 5) | 5);	/* fcvtn v5.2s, v16.2d */
			a64_dw(e, 0x4E616800u | (17u << 5) | 5);	/* fcvtn2 v5.4s, v17.2d */
			a64m_addsub(e, true, false, false, 9, CTX, OP_IMM(ctx_mem(shil_reg_ptr(&op->rd)).offset), false);
			a64_dw(e, 0x4C007800u | (9u << 5) | 5);		/* st1 {v5.4s}, [x9] */
		}
		break;

	case shop_frswap:
		a64m_addsub(e, true, false, false, 9, CTX, OP_IMM(ctx_mem(shil_reg_ptr(&op->rs1)).offset), false);
		a64m_addsub(e, true, false, false, 10, CTX, OP_IMM(ctx_mem(shil_reg_ptr(&op->rd)).offset), false);
		a64_dw(e, 0x4C400C00u | (9u << 5) | 0);		/* ld4 {v0.2d-v3.2d}, [x9] */
		a64_dw(e, 0x4C400C00u | (10u << 5) | 4);	/* ld4 {v4.2d-v7.2d}, [x10] */
		a64_dw(e, 0x4C000C00u | (9u << 5) | 4);		/* st4 {v4.2d-v7.2d}, [x9] */
		a64_dw(e, 0x4C000C00u | (10u << 5) | 0);	/* st4 {v0.2d-v3.2d}, [x10] */
		break;

	case shop_cvt_f2i_t:
		{
			/* The host's conversion gives the largest or smallest integer
			 * for a value too large, as the SH4 does, but 0 for a NaN,
			 * where the SH4 gives 0x80000000. A NaN is the one value that
			 * does not compare with itself. */
			const int from = MAPF(c, &op->rs1);
			const int to = MAPG(c, &op->rd);
			a64m_fcvtzs(e, to, from);
			a64m_fcmp(e, from, from);
			mov_wi(c, 0, 0x80000000u);
			a64_csel(e, to, to, 0, 7 /* vc */);
		}
		break;
	case shop_cvt_i2f_n:
	case shop_cvt_i2f_z:
		a64m_scvtf(e, MAPF(c, &op->rd), MAPG(c, &op->rs1));
		break;

	default:
		shil_chf[op->op](op);
		break;
	}
}

static void arm64_preload(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	a64m_ls(&comp->e, A64_LDRW, (int)nreg, ctx_mem(GetRegPtr(reg)));
}

static void arm64_writeback(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	a64m_ls(&comp->e, A64_STRW, (int)nreg, ctx_mem(GetRegPtr(reg)));
}

static void arm64_preload_fpu(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	a64m_ls(&comp->e, A64_LDRS, (int)nreg, ctx_mem(GetRegPtr(reg)));
}

static void arm64_writeback_fpu(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	a64m_ls(&comp->e, A64_STRS, (int)nreg, ctx_mem(GetRegPtr(reg)));
}

static void compile_block(arm64_compiler *c, RuntimeBlockInfo* block, bool force_checks, bool optimise)
{
	a64_emit *e = &c->e;
	a64_label cycles_remaining, cpu_running;
	bool accurate;
	size_t i;

	c->block = block;
	check_block(c, force_checks, block);

	/* run register allocator */
	sh4_regalloc_begin(&c->ra, block, alloc_regs, ALLOC_REGS, alloc_fregs, ALLOC_FREGS);

	/* scheduler: what is left of the time slice is w27. The slice ends
	 * after the block that uses it up - that leaves the counter at 0 or
	 * below - which is here, at the start of the next one; and a block
	 * pays for itself where it ends, so that a timer read in the middle of
	 * it counts the time up to the block and not the block's own. The
	 * x86-64 recompiler has it so, and the interpreter as near as an
	 * instruction at a time can: the same game runs the same on both.
	 *
	 * With the MMU on a block can be left by an exception, which comes
	 * back to the main loop by longjmp() with w27 as it was at the
	 * setjmp(): there the block pays at its start, as on x86-64, and there
	 * is a copy in the main loop's stack frame, written here, for it to
	 * start again from. */
	a64_label_init(&cycles_remaining);
	a64_label_init(&cpu_running);
	a64m_addsub(e, false, true, true, A64_ZR, CYC, OP_IMM(0), false);
	a64_b_label(e, A64_GT, 0, &cycles_remaining);
	gen_call(c, (uintptr_t)arm64_intc_sched, true);
	a64m_cbz(e, true, false, 0, &cpu_running);
	mov_wi(c, NPC, block->vaddr);
	a64m_ls(e, A64_STRW, NPC, ctx_mem(&next_pc));
	gen_call(c, (uintptr_t)arm64_no_update, false);
	a64_label_bind(e, &cpu_running);
	a64_label_bind(e, &cycles_remaining);
	if (mmu_enabled())
	{
		a64m_addsub(e, false, true, false, CYC, CYC, OP_IMM(block->guest_cycles), false);
		a64m_ls(e, A64_STRW, CYC, a64_mem_off(A64_SP, 8));
	}

	accurate = settings.dynarec.AccurateTiming && !mmu_enabled();
	if (accurate && sh4_block_writes(block))
	{
		/* a block that can change something other than a register says so: see wait_site.h */
		a64_mov_imm(e, true, 1, (uintptr_t)&sh4_write_gen);
		a64m_ls(e, A64_LDRW, 0, a64_mem_off(1, 0));
		a64m_addsub(e, false, false, false, 0, 0, OP_IMM(1), false);
		a64m_ls(e, A64_STRW, 0, a64_mem_off(1, 0));
	}

	for (i = 0; i < block->op_count; i++)
	{
		shil_opcode *op = &block->oplist[i];
		sh4_regalloc_op_begin(&c->ra, op, (int)i);
		gen_op(c, op, block, optimise);
		sh4_regalloc_op_end(&c->ra, op);
	}
	sh4_regalloc_cleanup(&c->ra);

	/* what the block costs, where it ends (see above) */
	if (!mmu_enabled())
		a64m_addsub(e, false, true, false, CYC, CYC, OP_IMM(block->guest_cycles), false);

	if (accurate && block->BranchBlock <= block->vaddr
			&& (block->BlockType == BET_StaticJump || block->BlockType == BET_StaticCall
				|| block->BlockType == BET_Cond_0 || block->BlockType == BET_Cond_1))
	{
		/* A block that can go back: every so often, see whether the game
		 * is only waiting, and take the passes it would still make in the
		 * time slice off the cycle counter, w27, if it is. See wait_site.h.
		 * This is ahead of the branch, so it is also passed on the way out
		 * of a loop; that changes nothing, since a pass that finds the
		 * registers as they were is one that goes round again. */
		WaitSite *site = sh4_wait_site(block);
		if (site != NULL)
		{
			a64_label over;
			a64_label_init(&over);

			a64_mov_imm(e, true, 1, (uintptr_t)&site->skip);
			a64m_ls(e, A64_LDRW, 0, a64_mem_off(1, 0));
			a64m_addsub(e, false, true, true, 0, 0, OP_IMM(1), false);
			a64m_ls(e, A64_STRW, 0, a64_mem_off(1, 0));
			a64_b_label(e, 5 /* pl */, 0, &over);
			a64_mov_imm(e, true, 0, (uintptr_t)site);
			mov_ww(c, 1, CYC);
			gen_call_runtime(c, (uintptr_t)&sh4_wait_check, true);
			mov_ww(c, CYC, 0);
			a64_label_bind(e, &over);
		}
	}

	block->relink_offset = (u32)((u8 *)e->p - (u8 *)e->start);
	block->relink_data = 0;

	relink_block(c, block);

	finalize(c, false);
}

void ngen_Compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
{
	(void)reset;
	(void)staging;
	/* The main loop and its routines first, as ngen_AllocateBlock() had them
	 * made before the C rewrite: a cache clear (recSh4_ClearCache(), which
	 * puts the code back at its start) has ngen_ResetBlocks() drop them, and
	 * a block made before they are made again would call where they were,
	 * which the block itself is now written over. They go in the main cache
	 * even when the block is for the temporary one. */
	if (mainloop == NULL)
	{
		u32 *const temp_ptr = emit_ptr;
		u32 *const temp_limit = emit_ptr_limit;
		emit_ptr = NULL;
		emit_ptr_limit = NULL;
		generate_mainloop();
		emit_ptr = temp_ptr;
		emit_ptr_limit = temp_limit;
	}
	verify(emit_FreeSpace() >= CODE_MARGIN);

	vmem_jit_write_begin();
	comp = &room;
	compiler_start(&room, emit_GetCCPtr(), emit_FreeSpace());
	sh4_regalloc_init(&room.ra, ALLOC_F64, arm64_preload, arm64_writeback, arm64_preload_fpu, arm64_writeback_fpu, &room);

	compile_block(&room, block, force_checks, optimise);

	comp = NULL;
	vmem_jit_write_end();
}

void ngen_CC_Start(shil_opcode* op)
{
	(void)op;
	comp->cc_count = 0;
}

void ngen_CC_Param(shil_opcode* op, shil_param* prm, CanonicalParamType tp)
{
	arm64_compiler *c = comp;
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

	case CPT_u64rvL:
	case CPT_u32rv:
		reg_to_param(c, prm, 0, 0);
		break;

	case CPT_u64rvH:
		a64m_shift_imm(&c->e, true, A64_LSR, 10, 0, 32);
		reg_to_param(c, prm, 10, 0);
		break;

	case CPT_f32rv:
		reg_to_param(c, prm, 0, 2);
		break;
	}
}

void ngen_CC_Call(shil_opcode* op, void* function)
{
	arm64_compiler *c = comp;
	int regused = 0, fregused = 0, i;

	/* Args are pushed in reverse order by shil_canonical */
	for (i = c->cc_count; i-- > 0;)
	{
		const shil_param *prm = c->cc_pars[i].prm;
		verify(fregused < 8 && regused < 8);
		switch (c->cc_pars[i].type)
		{
		/* push the params */

		case CPT_u32:
			param_to_reg(c, prm, regused++, false);
			break;

		case CPT_f32:
			if (shil_is_reg(prm))
				a64m_fp2(&c->e, A64_FMOV_S, fregused, MAPF(c, prm));
			else if (shil_is_imm(prm))
				a64m_fmov_imm(&c->e, &c->pool, fregused, prm->_imm);
			else
				verify(shil_is_null(prm));
			fregused++;
			break;

		case CPT_ptr:
			verify(shil_is_reg(prm));
			/* push the ptr itself */
			a64_mov_imm(&c->e, true, regused++, (uintptr_t)shil_reg_ptr(prm));
			break;

		default:
			/* return values are handled in ngen_CC_Param() */
			break;
		}
	}
	gen_call_runtime(c, (uintptr_t)function, true);
	/* A pair the function was given the address of to write (FSCA's, where
	 * it is a call) is read again into the registers the block has it in. */
	for (i = 0; i < c->cc_count; i++)
	{
		const shil_param *prm = c->cc_pars[i].prm;
		if (c->cc_pars[i].type == CPT_ptr && shil_count(prm) == 2 && IS_ALLOCF(c, prm)
				&& ((shil_is_reg(&op->rd) && op->rd._imm == prm->_imm) || (shil_is_reg(&op->rd2) && op->rd2._imm == prm->_imm)))
		{
			a64m_ls(&c->e, A64_LDRS, MAPF_I(c, prm, 0), ctx_mem(shil_reg_ptr(prm)));
			a64m_ls(&c->e, A64_LDRS, MAPF_I(c, prm, 1), ctx_mem((u8 *)shil_reg_ptr(prm) + 4));
		}
	}
}

void ngen_CC_Finish(shil_opcode* op)
{
	(void)op;
}

/* Whether what is at @site is a fast access as gen_fast_access() writes
 * them, and if so which kind and with which registers (as mem_stubs
 * numbers them). A fast access is told by what it is: a load or store with
 * a register for its offset and x26 for its base, which nothing else a
 * block has. Where the host has 512 MB mapped the offset is x9, which the
 * instruction before made from the register the address is in. */
static bool mem_access_at(const u32 *site, int *kind, int *addr, int *data)
{
	const u32 insn = *site;
	u32 size, opc, option;
	bool fp;
	int reg;

	/* size 111 V 00 opc 1 Rm option S 10 Rn Rt, with x26 the base and nothing shifted */
	if ((insn & 0x3B200C00) != 0x38200800 || ((insn >> 5) & 31) != MEM_BASE || (insn & 0x1000) != 0)
		return false;
	size = insn >> 30;
	opc = (insn >> 22) & 3;
	option = (insn >> 13) & 7;
	fp = (insn & 0x04000000) != 0;
	reg = (insn >> 16) & 31;

	if (_nvmem_4gb_space())
	{
		/* the address as it is, 32 bits of it */
		if (option != 2)
			return false;
	}
	else
	{
		/* ubfx x9, <the address>, #0, #29 before it */
		if (option != 3 || reg != 9 || (site[-1] & 0xFFFFFC1F) != 0xD3407009)
			return false;
		reg = (site[-1] >> 5) & 31;
	}
	if (fp)
	{
		if (size != 2 || opc > 1)
			return false;
		*kind = opc == 0 ? MEM_STORE32F : MEM_LOAD32F;
	}
	else if (opc == 0)
		*kind = size == 0 ? MEM_STORE8 : size == 1 ? MEM_STORE16 : size == 2 ? MEM_STORE32 : MEM_STORE64;
	else if (opc == 1 && size >= 2)
		*kind = size == 2 ? MEM_LOAD32 : MEM_LOAD64;
	else if (opc == 3 && size <= 1)
		*kind = size == 0 ? MEM_LOAD8 : MEM_LOAD16;
	else
		return false;

	*addr = mem_reg_number(reg, 0);
	*data = fp ? mem_freg_number(insn & 31) : mem_reg_number(insn & 31, *kind < MEM_STORE8 ? 0 : 1);
	if ((*kind == MEM_LOAD64 || *kind == MEM_STORE64) && *data != 0)
		return false;
	return *addr >= 0 && *data >= 0;
}

/* A fast memory access faulted: it is written over with a call to the
 * routine that does such an access by asking, with the registers this one
 * has (mem_stubs), and run again. */
bool ngen_Rewrite(unat *host_pc_at)
{
	const unat host_pc = *host_pc_at;
	u32 *const site = (u32 *)CC_RX2RW(host_pc);
	int kind, addr, data;
	ptrdiff_t offset;

	if (!_nvmem_enabled() || mmu_enabled())
		return false;
	if (!mem_access_at(site, &kind, &addr, &data) || mem_stubs[kind][addr][data] == NULL)
		return false;

	/* bl, to where the routine is from where this is run */
	offset = mem_stubs[kind][addr][data] - (u8 *)site;
	verify(offset >= -128 * 1024 * 1024 && offset < 128 * 1024 * 1024 && (offset & 3) == 0);
	vmem_jit_write_begin();
	*site = 0x94000000 | (u32)((offset >> 2) & 0x03FFFFFF);
	vmem_jit_write_end();
	vmem_platform_flush_cache((void *)host_pc, (void *)(host_pc + 4), site, site + 1);
	return true;
}

static void generate_mainloop(void)
{
	if (mainloop != NULL)
		return;
	vmem_jit_write_begin();
	comp = &room;
	compiler_start(&room, emit_GetCCPtr(), emit_FreeSpace());

	gen_mainloop(&room);

	comp = NULL;
	vmem_jit_write_end();
}

void ngen_HandleException(void)
{
	longjmp(jmp_env, 1);
}

u32 ngen_Relink(RuntimeBlockInfo *block)
{
	u8 *const at = (u8 *)(uintptr_t)block->code + block->relink_offset;
	u32 code_size;

	vmem_jit_write_begin();
	compiler_start(&room, at, emit_FreeSpace());
	room.block = block;
	code_size = relink_block(&room, block);
	finalize(&room, true);
	vmem_jit_write_end();

	return code_size;
}

/* The store queue written to main memory, with no MMU (an assembler routine
 * of its own: nothing of C's around it) */
#ifdef __APPLE__
#define SQW_NAME "_do_sqw_nommu_area_3"
#define SQW_HIDDEN ".private_extern " SQW_NAME "\n"
#else
#define SQW_NAME "do_sqw_nommu_area_3"
#define SQW_HIDDEN ".hidden " SQW_NAME "\n.type " SQW_NAME ", %function\n"
#endif
__asm__
(
	".text\n"
	".p2align 2\n"
	SQW_HIDDEN
	".globl " SQW_NAME "\n"
	SQW_NAME ":\n"
	"and x12, x0, #0x20\n"			/* SQ# selection, isolate */
	"add x12, x12, x1\n"			/* SQ# selection, add to SQ ptr */
	"ld2 { v0.2D, v1.2D }, [x12]\n"
	"movz x11, #0x0C00, lsl #16\n"
	"add x11, x1, x11\n"			/* get ram ptr from x1, part 1 */
	"ubfx x0, x0, #5, #20\n"		/* get ram offset */
	"add x11, x11, #512\n"			/* get ram ptr from x1, part 2 */
	"add x11, x11, x0, lsl #5\n"	/* ram + offset */
	"st2 { v0.2D, v1.2D }, [x11]\n"
	"ret\n"
);
#else
/* (for the other processors: C has no empty file) */
typedef int rec_arm64_not_here;
#endif
