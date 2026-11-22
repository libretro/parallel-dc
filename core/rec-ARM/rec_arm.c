/* The SH4 recompiler for 32-bit ARM (ARMv7 with VFP), on jit/a32_emit.h.
 * C89.
 *
 *	ARM ABI
 *		r0~r1: scratch, params, return
 *		r2~r3: scratch, params
 *		8 regs, v6 is platform dependent
 *			r4~r11
 *		r12 is "The Intra-Procedure-call scratch register"
 *		r13 stack
 *		r14 link
 *		r15 pc
 *
 *		Registers s0-s15 (d0-d7, q0-q3) do not need to be preserved (and can
 *		be used for passing arguments or returning results in standard
 *		procedure-call variants). Registers s16-s31 (d8-d15, q4-q7) must be
 *		preserved across subroutine calls; registers d16-d31 (q8-q15), if
 *		present, do not need to be preserved.
 *
 *	Reg alloc
 *		r0~r4: scratch (r4: the next pc, at the end of a block)
 *		r5,r6,r7,r10,r11: allocated (not r11 on Apple's systems)
 *		r8: the end of the SH4's context
 *		r9: what is left of the time slice (r11 on Apple's systems)
 *
 *	fpu reg alloc
 *		s16:s31, single storage */
#include <unistd.h>
#include <string.h>
#include "types.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_ARM
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/wait_site.h"

#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/dyna/ssa_regalloc.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/mem/_vmem.h"

#define A32_EMIT(w) emit_Write32(w)
#define A32_HERE() emit_GetCCPtr()
#include "jit/a32_emit.h"

#ifdef __MACH__
#include <libkern/OSCacheControl.h>
static void CacheFlush(void* code, void* pEnd)
{
	sys_dcache_flush(code, (u8*)pEnd - (u8*)code + 1);
	sys_icache_invalidate(code, (u8*)pEnd - (u8*)code + 1);
}
#elif !defined(_ANDROID)
static void CacheFlush(void* code, void* pEnd)
{
	__builtin___clear_cache((char *)code, (char *)pEnd);
}
#else
#include <sys/syscall.h>
static void CacheFlush(void* code, void* pEnd)
{
	/* Ideally, syscall(__ARM_NR_cacheflush, start, end, 0); but syscall(int,
	 * ...) is not there on every system, especially not with EABI: the call
	 * is made directly. */
	void* start = code;
	size_t size = (u8*)pEnd - (u8*)start + 4;
	register uint32_t beg __asm__("a1") = (uint32_t)start;
	register uint32_t end __asm__("a2") = (uint32_t)start + size;
	register uint32_t flg __asm__("a3") = 0;
#ifdef __ARM_EABI__
#if defined (__arm__) && !defined(__thumb__)
	/* (__arm__ may be defined in thumb mode) */
	register uint32_t scno __asm__("r7") = __ARM_NR_cacheflush;
	__asm__ volatile(
		"svc 0x0"
		: "=r" (beg)
		: "0" (beg), "r" (end), "r" (flg), "r" (scno));
#else
	/* (r7 is reserved by the EABI in thumb mode) */
	__asm__ volatile(
		"@   Enter ARM Mode  \n\t"
		"adr r3, 1f      \n\t"
		"bx  r3          \n\t"
		".ALIGN 4        \n\t"
		".ARM            \n"
		"1:  push {r7}       \n\t"
		"mov r7, %4      \n\t"
		"svc 0x0         \n\t"
		"pop {r7}        \n\t"
		"@   Enter THUMB Mode\n\t"
		"adr r3, 2f+1    \n\t"
		"bx  r3          \n\t"
		".THUMB          \n"
		"2:                  \n\t"
		: "=r" (beg)
		: "0" (beg), "r" (end), "r" (flg), "r" (__ARM_NR_cacheflush)
		: "r3");
#endif
#else
#if defined (__arm__) && !defined(__thumb__)
	__asm__ volatile(
		"svc %1"
		: "=r" (beg)
		: "i" (__ARM_NR_cacheflush), "0" (beg), "r" (end), "r" (flg));
#else
	/* (not __ARM_NR_cacheflush in the assembly: the thumb mode value would
	 * be used, and the svc is in ARM mode) */
	__asm__ volatile(
		"@   Enter ARM Mode  \n\t"
		"adr r3, 1f      \n\t"
		"bx  r3          \n\t"
		".ALIGN 4        \n\t"
		".ARM            \n"
		"1:  svc 0x9f0002    \n"
		"@   Enter THUMB Mode\n\t"
		"adr r3, 2f+1    \n\t"
		"bx  r3          \n\t"
		".THUMB          \n"
		"2:                  \n\t"
		: "=r" (beg)
		: "0" (beg), "r" (end), "r" (flg)
		: "r3");
#endif
#endif
}
#endif

/* What is left of the time slice */
#ifdef __MACH__
#define CYC A32_R11
#else
#define CYC A32_R9
#endif
/* The end of the SH4's context */
#define CTX A32_R8

#define AL A32_AL

/* where the SH4's register @reg is from r8 */
static s32 sh4_reg_offset(u32 reg)
{
	return (s32)((u8*)GetRegPtr(reg) - sh4_dyna_rcb);
}

/* (s32 by the arithmetic C++'s shil_param::reg_nofs() did it in) */
static s32 reg_nofs(const shil_param *prm)
{
	return (s32)((u8*)GetRegPtr(prm->_imm) - (u8*)GetRegPtr(reg_xf_0) - sizeof(Sh4cntx));
}

static u32 reg_aofs(const shil_param *prm)
{
	return (u32)-reg_nofs(prm);
}

static s32 rcb_noffs(const void *ptr)
{
	return (s32)((const u8*)ptr - (const u8*)p_sh4rcb - sizeof(Sh4RCB));
}

/* you pick reg, loads Base with reg addr, no reg. mapping yet ! */
static void LoadSh4Reg_mem(int Rt, u32 Sh4_Reg, int CC)
{
	a32_ls_imm(A32_LDR, Rt, CTX, sh4_reg_offset(Sh4_Reg), A32_OFFSET, CC);
}

/* you pick regs, loads Base with reg addr, no reg. mapping yet ! data
 * should already exist for Rt ! */
static void StoreSh4Reg_mem(int Rt, u32 Sh4_Reg, int CC)
{
	a32_ls_imm(A32_STR, Rt, CTX, sh4_reg_offset(Sh4_Reg), A32_OFFSET, CC);
}

#ifdef __MACH__
static const u32 alloc_regs[] = { 5, 6, 7, 10 };
#else
static const u32 alloc_regs[] = { 5, 6, 7, 10, 11 };
#endif
static const u32 alloc_fpu[] = { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };

/* A pair of the SH4's floating-point registers (DRn, XDn) is kept in two of alloc_fpu */
#define ALLOC_F64 true

static sh4_regalloc ra;

static void arm_preload(sh4_regalloc *rap, u32 reg, u32 nreg)
{
	(void)rap;
	verify(reg != reg_pc_dyn);
	LoadSh4Reg_mem((int)nreg, reg, AL);
}

static void arm_writeback(sh4_regalloc *rap, u32 reg, u32 nreg)
{
	(void)rap;
	/* reg_pc_dyn has been stored in r4 by the jdyn op implementation: it
	 * is not used past the end of the block */
	if (reg != reg_pc_dyn)
		StoreSh4Reg_mem((int)nreg, reg, AL);
}

static void arm_preload_fpu(sh4_regalloc *rap, u32 reg, u32 nreg)
{
	(void)rap;
	a32_vls(true, false, (int)nreg, CTX, sh4_reg_offset(reg) / 4, AL);
}

static void arm_writeback_fpu(sh4_regalloc *rap, u32 reg, u32 nreg)
{
	(void)rap;
	a32_vls(false, false, (int)nreg, CTX, sh4_reg_offset(reg) / 4, AL);
}

#define IS_ALLOCG(p) sh4_regalloc_is_allocg(&ra, (p))
#define IS_ALLOCF(p) sh4_regalloc_is_allocf(&ra, (p))
#define MAPG(p) ((int)sh4_regalloc_mapg(&ra, (p)))
#define MAPF(p) ((int)sh4_regalloc_mapf(&ra, (p), 0))
#define MAPF_I(p, i) ((int)sh4_regalloc_mapf(&ra, (p), (i)))

void no_update(void);
void intc_sched(void);
void ngen_blockcheckfail(void);
void ngen_LinkBlock_Generic_stub(void);
void ngen_LinkBlock_cond_Branch_stub(void);
void ngen_LinkBlock_cond_Next_stub(void);
void ngen_FailedToFindBlock_(void);

/* The condition the last op of a block leaves T as, set (ccmap) and clear
 * (ccnmap), for the ops that have one */
static bool cond_op(shilop op, int *cc_set, int *cc_clear)
{
	switch (op)
	{
	case shop_test:  *cc_set = A32_EQ; *cc_clear = A32_NE; return true;
	case shop_seteq: *cc_set = A32_EQ; *cc_clear = A32_NE; return true;
	case shop_setge: *cc_set = A32_GE; *cc_clear = A32_LT; return true;
	case shop_setgt: *cc_set = A32_GT; *cc_clear = A32_LE; return true;
	case shop_setae: *cc_set = A32_HS; *cc_clear = A32_LO; return true;
	case shop_setab: *cc_set = A32_HI; *cc_clear = A32_LS; return true;
	default: return false;
	}
}

/* ---- With the MMU on ----
 *
 * A block compiled with the SH4's MMU on does three things another way.
 *
 * Every access to memory is a call (rec_arm_mmu_read8 and the rest below):
 * the address is translated, and where that fails the SH4's exception is
 * taken there and then - registers saved, PC at the handler - and
 * longjmp() goes back to the main loop (ngen_arm.S), which carries on
 * from the handler. So do instructions the interpreter runs and store
 * queue flushes, which can raise exceptions too. There is no reading and
 * writing through the host's own mapping, since a 32-bit host has no room
 * to lay out a translated address space the way the 64-bit ones do.
 *
 * It ends by handing its next PC to no_update_mmu rather than by jumping
 * to another block: which block a virtual address means depends on the
 * mapping in force. Not so for an address in P1 or P2, or in the page the
 * block is in itself (rdv_MmuMayGoOn() in ngen.h), and to those it does
 * jump - with the PC put in the context first, for the check below.
 *
 * And it begins by checking that the PC is its own, for the same reason,
 * and that the FPU is on if it uses it. */
#ifndef NO_MMU
#include <setjmp.h>

static jmp_buf arm_jmp_env;

void no_update_mmu(void);
void intc_sched_mmu(void);

__attribute__((used)) void *rec_arm_jmp_env(void)
{
	return arm_jmp_env;
}

__attribute__((used)) u32 rec_arm_mmu_on(void)
{
	return mmu_enabled();
}

__attribute__((used)) void *rec_arm_code_mmu(u32 pc)
{
	return (void *)(uintptr_t)bm_GetCodeByVAddr(pc);
}

static u32 mmu_read(u32 addr, u32 pc, u32 size)
{
	u32 paddr, rv;
	rv = mmu_data_translation_c(addr, &paddr, MMU_TT_DREAD, size);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(addr, rv, MMU_TT_DREAD);
		spc = pc;
		longjmp(arm_jmp_env, 1);
	}
	/* the next read of the page need not come here: see mmu.h */
	mmu_lut_fill(addr, paddr, false);
	return size == 1 ? (u32)(u8)_vmem_ReadMem8(paddr) : size == 2 ? (u32)(u16)_vmem_ReadMem16(paddr)
			: _vmem_ReadMem32(paddr);
}

/* what a load of 8 or 16 bits gives is sign-extended */
u32 rec_arm_mmu_read8(u32 addr, u32 pc)  { return (u32)(s32)(s8)mmu_read(addr, pc, 1); }
u32 rec_arm_mmu_read16(u32 addr, u32 pc) { return (u32)(s32)(s16)mmu_read(addr, pc, 2); }
u32 rec_arm_mmu_read32(u32 addr, u32 pc) { return mmu_read(addr, pc, 4); }

u64 rec_arm_mmu_read64(u32 addr, u32 pc)
{
	u32 paddr, rv;
	/* (two accesses, each translated: mmu.h) */
	if (mmu_in_two_pages(addr))
	{
		const u32 low = mmu_read(addr, pc, 4);
		return ((u64)mmu_read(addr + 4, pc, 4) << 32) | low;
	}
	rv = mmu_data_translation_c(addr, &paddr, MMU_TT_DREAD, 8);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(addr, rv, MMU_TT_DREAD);
		spc = pc;
		longjmp(arm_jmp_env, 1);
	}
	mmu_lut_fill(addr, paddr, false);
	return _vmem_ReadMem64(paddr);
}

static void mmu_write(u32 addr, u64 data, u32 pc, u32 size)
{
	u32 paddr, rv;
	if (size == 8 && mmu_in_two_pages(addr))
	{
		mmu_write(addr, (u32)data, pc, 4);
		mmu_write(addr + 4, (u32)(data >> 32), pc, 4);
		return;
	}
	rv = mmu_data_translation_c(addr, &paddr, MMU_TT_DWRITE, size);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(addr, rv, MMU_TT_DWRITE);
		spc = pc;
		longjmp(arm_jmp_env, 1);
	}
	mmu_lut_fill(addr, paddr, true);
	if (size == 1)
		_vmem_WriteMem8(paddr, (u8)data);
	else if (size == 2)
		_vmem_WriteMem16(paddr, (u16)data);
	else if (size == 4)
		_vmem_WriteMem32(paddr, (u32)data);
	else
		_vmem_WriteMem64(paddr, data);
}

void rec_arm_mmu_write8(u32 addr, u32 data, u32 pc)  { mmu_write(addr, (u8)data, pc, 1); }
void rec_arm_mmu_write16(u32 addr, u32 data, u32 pc) { mmu_write(addr, (u16)data, pc, 2); }
void rec_arm_mmu_write32(u32 addr, u32 data, u32 pc) { mmu_write(addr, data, pc, 4); }
/* the PC ahead of the data: a 64-bit argument goes in r2 and r3 either way */
void rec_arm_mmu_write64(u32 addr, u32 pc, u64 data) { mmu_write(addr, data, pc, 8); }

/* @pc is odd in a delay slot, where it is the address before the instruction's */
static void mmu_exception(SH4ThrownException *ex, u32 pc)
{
	if (pc & 1)
	{
		AdjustDelaySlotException(ex);
		pc--;
	}
	Do_Exception(pc, ex->expEvn, ex->callVect);
	longjmp(arm_jmp_env, 1);
}

void rec_arm_mmu_fallback(u32 op, OpCallFP *oph, u32 pc)
{
	sh4_catch c;
	SH4_TRY(c)
	{
		oph((u16)op);
		sh4_catch_pop(&c);
	}
	else
		mmu_exception(&c.ex, pc);
}

void rec_arm_mmu_sqw(u32 addr, u32 pc)
{
	sh4_catch c;
	SH4_TRY(c)
	{
		do_sqw_mmu(addr);
		sh4_catch_pop(&c);
	}
	else
		mmu_exception(&c.ex, pc);
}
#endif

/* The end of the block, written where the block's end is, with the emitter
 * pointed there for the while - and pointed back afterwards to wherever it
 * was. (A block of the temporary cache is compiled with the emitter
 * pointing into that cache, and this is called as the last of compiling
 * it.) */
u32 ngen_Relink(RuntimeBlockInfo *block)
{
	u32 *const emit_ptr_before = emit_ptr;
	u8* code_start = (u8*)(uintptr_t)block->code + block->relink_offset;
	u32 sz;
	emit_ptr = (u32*)code_start;

	switch (block->BlockType)
	{
	case BET_Cond_0:
	case BET_Cond_1:
		{
			int CC = A32_EQ;

			if (block->tail_cc != -1)
				CC = block->tail_cc;
			else
			{
				if (!block->has_jcond)
				{
					if (block->t_reg != -1)
						a32_mov(A32_R4, block->t_reg, AL);
					else
					{
						INFO_LOG(DYNAREC, "SLOW COND PATH %08x\n", block->vaddr);
						LoadSh4Reg_mem(A32_R4, reg_sr_T, AL);
					}
				}
				a32_cmp_imm(A32_R4, block->BlockType & 1, AL);
			}

#ifndef NO_MMU
			if (mmu_enabled())
			{
				/* The next PC to the main loop, either way - but for one that
				 * is where it is whatever the TLB says, which is gone straight
				 * on to like any other time: see "With the MMU on". */
				u32 place;

				a32_mov32(A32_R4, block->BranchBlock, CC);
				if (block->mmu_go_on && rdv_MmuSamePlace(block, block->BranchBlock, &place))
				{
					StoreSh4Reg_mem(A32_R4, reg_nextpc, CC);
					if (block->pBranchBlock)
						a32_jump((uintptr_t)block->pBranchBlock->code, CC);
					else
						a32_call((uintptr_t)ngen_LinkBlock_cond_Branch_stub, CC);
				}
				else
					a32_jump((uintptr_t)no_update_mmu, CC);
				a32_mov32(A32_R4, block->NextBlock, AL);
				if (block->mmu_go_on && rdv_MmuSamePlace(block, block->NextBlock, &place))
				{
					StoreSh4Reg_mem(A32_R4, reg_nextpc, AL);
					if (block->pNextBlock)
						a32_jump((uintptr_t)block->pNextBlock->code, AL);
					else
						a32_call((uintptr_t)ngen_LinkBlock_cond_Next_stub, AL);
				}
				else
					a32_jump((uintptr_t)no_update_mmu, AL);
				break;
			}
#endif
			if (block->pBranchBlock)
				a32_jump((uintptr_t)block->pBranchBlock->code, CC);
			else
				a32_call((uintptr_t)ngen_LinkBlock_cond_Branch_stub, CC);

			if (block->pNextBlock)
				a32_jump((uintptr_t)block->pNextBlock->code, AL);
			else
				a32_call((uintptr_t)ngen_LinkBlock_cond_Next_stub, AL);
			break;
		}

	case BET_DynamicRet:
	case BET_DynamicCall:
	case BET_DynamicJump:
		{
#ifndef NO_MMU
			if (mmu_enabled())
			{
				/* r4 has it */
				a32_jump((uintptr_t)no_update_mmu, AL);
				break;
			}
#endif
			if (block->relink_data != 0)
				verify(block->pBranchBlock == 0);
			a32_op_imm(A32_SUB, A32_R2, CTX, (u32)-FPCB_OFFSET, AL);
#if RAM_SIZE_MAX == 33554432
			a32_ubfx(A32_R1, A32_R4, 1, 24, AL);
#else
			a32_ubfx(A32_R1, A32_R4, 1, 23, AL);
#endif
			a32_ls_reg(A32_LDR, A32_PC, A32_R2, A32_R1, A32_OFFSET, true, A32_LSL, 2, AL);
			break;
		}

	case BET_StaticCall:
	case BET_StaticJump:
		{
#ifndef NO_MMU
			if (mmu_enabled())
			{
				u32 place;

				a32_mov32(A32_R4, block->BranchBlock, AL);
				if (block->mmu_go_on && rdv_MmuSamePlace(block, block->BranchBlock, &place))
				{
					StoreSh4Reg_mem(A32_R4, reg_nextpc, AL);
					if (block->pBranchBlock == 0)
						a32_call((uintptr_t)ngen_LinkBlock_Generic_stub, AL);
					else
						a32_jump((uintptr_t)block->pBranchBlock->code, AL);
				}
				else
					a32_jump((uintptr_t)no_update_mmu, AL);
				break;
			}
#endif
			if (block->pBranchBlock == 0)
				a32_call((uintptr_t)ngen_LinkBlock_Generic_stub, AL);
			else
				a32_jump((uintptr_t)block->pBranchBlock->code, AL);
			break;
		}

	case BET_StaticIntr:
	case BET_DynamicIntr:
		{
			if (block->BlockType == BET_StaticIntr)
				a32_mov32(A32_R4, block->NextBlock, AL);
			/* else -> already in r4 djump ! */

			StoreSh4Reg_mem(A32_R4, reg_nextpc, AL);

			a32_call((uintptr_t)UpdateINTC, AL);
			LoadSh4Reg_mem(A32_R4, reg_nextpc, AL);
#ifndef NO_MMU
			if (mmu_enabled())
				a32_jump((uintptr_t)no_update_mmu, AL);
			else
#endif
			a32_jump((uintptr_t)no_update, AL);
			break;
		}

	default:
		ERROR_LOG(DYNAREC, "Error, Relink() Block Type: %X\n", block->BlockType);
		verify(false);
		break;
	}

	CacheFlush(code_start, emit_ptr);

	sz = (u32)((u8*)emit_ptr - code_start);

	emit_ptr = emit_ptr_before;
	return sz;
}

static int GetParam(const shil_param *param, int raddr)
{
	if (shil_is_imm(param))
	{
		a32_mov32(raddr, param->_imm, AL);
		return raddr;
	}
	else if (shil_is_r32i(param))
		return MAPG(param);
	die("Invalid parameter");
	return -1;
}

/* rd = rs1 op rs2, rs2 an immediate where the instruction can have it */
static void ngen_Binary(const shil_opcode* op, int dpop)
{
	int rs1 = GetParam(&op->rs1, A32_R0);
	int rs2 = A32_R1;
	if (shil_is_imm(&op->rs2))
	{
		if (a32_is_imm(op->rs2._imm))
		{
			a32_op_imm(dpop, MAPG(&op->rd), rs1, op->rs2._imm, AL);
			return;
		}
		a32_mov32(rs2, op->rs2._imm, AL);
	}
	else if (shil_is_r32i(&op->rs2))
		rs2 = MAPG(&op->rs2);
	else
	{
		ERROR_LOG(DYNAREC, "ngen_Bin ??? %d\n", op->rs2.type);
		verify(false);
	}
	a32_op(dpop, MAPG(&op->rd), rs1, rs2, AL);
}

/* rd = rs1 shifted by rs2 (an immediate where the instruction can have it) */
static void ngen_Shift(const shil_opcode* op, int shift)
{
	int rs1 = GetParam(&op->rs1, A32_R0);
	int rs2 = A32_R1;
	if (shil_is_imm(&op->rs2))
	{
		if (a32_is_imm(op->rs2._imm))
		{
			a32_shift_imm(shift, false, MAPG(&op->rd), rs1, op->rs2._imm, AL);
			return;
		}
		a32_mov32(rs2, op->rs2._imm, AL);
	}
	else if (shil_is_r32i(&op->rs2))
		rs2 = MAPG(&op->rs2);
	else
	{
		ERROR_LOG(DYNAREC, "ngen_Bin ??? %d\n", op->rs2.type);
		verify(false);
	}
	a32_shift_reg(shift, MAPG(&op->rd), rs1, rs2, AL);
}

static void ngen_fp_bin(const shil_opcode* op, u32 fpop)
{
	int rs1 = 0, rs2 = 1;
	if (shil_is_imm(&op->rs1))
	{
		a32_mov32(A32_R0, op->rs1._imm, AL);
		a32_vmov_sr(rs1, A32_R0, AL);
	}
	else
		rs1 = MAPF(&op->rs1);

	if (shil_is_imm(&op->rs2))
	{
		a32_mov32(A32_R0, op->rs2._imm, AL);
		a32_vmov_sr(rs2, A32_R0, AL);
	}
	else
		rs2 = MAPF(&op->rs2);

	a32_vfp3(fpop, MAPF(&op->rd), rs1, rs2, AL);
}

static void ngen_fp_una(const shil_opcode* op, u32 fpop)
{
	verify(shil_is_r32f(&op->rd));
	verify(shil_is_r32f(&op->rs1));

	a32_vfp2(fpop, MAPF(&op->rd), MAPF(&op->rs1), AL);
}

/* The canonical call's parameters (ngen_CC_Param()) */
static struct
{
	CanonicalParamType type;
	shil_param* par;
} CC_pars[8];
static int CC_count;

void ngen_CC_Start(shil_opcode* op)
{
	(void)op;
	CC_count = 0;
}

void ngen_CC_Param(shil_opcode* op, shil_param* par, CanonicalParamType tp)
{
	(void)op;
	switch (tp)
	{
	case CPT_f32rv:
	case CPT_u32rv:
	case CPT_u64rvL:
		if (IS_ALLOCG(par))
			a32_mov(MAPG(par), A32_R0, AL);
		else if (IS_ALLOCF(par))
			a32_vmov_sr(MAPF(par), A32_R0, AL);
		else
			die("unhandled param");
		break;

	case CPT_u64rvH:
		verify(IS_ALLOCG(par));
		a32_mov(MAPG(par), A32_R1, AL);
		break;

	case CPT_u32:
	case CPT_ptr:
	case CPT_f32:
		verify(CC_count < 8);
		CC_pars[CC_count].type = tp;
		CC_pars[CC_count].par = par;
		CC_count++;
		break;

	default:
		die("invalid tp");
	}
}

void ngen_CC_Call(shil_opcode* op, void* function)
{
	u32 rd = A32_R0;
	int i;

	for (i = CC_count; i-- > 0;)
	{
		const shil_param *par = CC_pars[i].par;
		if (CC_pars[i].type == CPT_ptr)
			a32_mov32((int)rd, (u32)(uintptr_t)shil_reg_ptr(par), AL);
		else if (shil_is_reg(par))
		{
			if (IS_ALLOCG(par))
				a32_mov((int)rd, MAPG(par), AL);
			else if (IS_ALLOCF(par))
				a32_vmov_rs((int)rd, MAPF(par), AL);
			else
				die("Must not happen!\n");
		}
		else
		{
			verify(shil_is_imm(par));
			a32_mov32((int)rd, par->_imm, AL);
		}
		rd++;
	}
	a32_call((uintptr_t)function, AL);

	/* A pair the function was given the address of to write (FSCA's, where
	 * it is a call) is read again into the registers the block has it in. */
	for (i = 0; i < CC_count; i++)
	{
		const shil_param *par = CC_pars[i].par;
		if (CC_pars[i].type == CPT_ptr && shil_count(par) == 2 && IS_ALLOCF(par)
				&& ((shil_is_reg(&op->rd) && op->rd._imm == par->_imm) || (shil_is_reg(&op->rd2) && op->rd2._imm == par->_imm)))
		{
			a32_vls(true, false, MAPF_I(par, 0), CTX, reg_nofs(par) / 4, AL);
			a32_vls(true, false, MAPF_I(par, 1), CTX, reg_nofs(par) / 4 + 1, AL);
		}
	}
}

void ngen_CC_Finish(shil_opcode* op)
{
	(void)op;
	CC_count = 0;
}

enum mem_op_type
{
	SZ_8,
	SZ_16,
	SZ_32I,
	SZ_32F,
	SZ_64F
};

static enum mem_op_type memop_type(const shil_opcode* op)
{
	int Lsz = -1;
	const u32 sz = op->flags & 0x7f;
	const bool fp32 = shil_is_r32f(&op->rs2) || shil_is_r32f(&op->rd);

	if (sz == 1) Lsz = SZ_8;
	if (sz == 2) Lsz = SZ_16;
	if (sz == 4 && !fp32) Lsz = SZ_32I;
	if (sz == 4 && fp32) Lsz = SZ_32F;
	if (sz == 8) Lsz = SZ_64F;

	verify(Lsz != -1);
	return (enum mem_op_type)Lsz;
}

static u32 memop_bytes(enum mem_op_type tp)
{
	static const u32 rv[] = { 1, 2, 4, 4, 8 };
	return rv[tp];
}

/*
	8/16/32 I R/W B
	bic r1,raddr,..
	ldr(sh/sb)/str(h/b) rd/s,[r1+r8]

	32/64 F R/W B
	bic r1,raddr,..
	add r1,r1,r8
	vldr/vstr rd/s,[r1] {32 or 64 bit forms}

	8/16/32 I R/W M
	mov r0,raddr
	call MEMHANDLER<rd/s>

	32/64 F R M
	mov r0,raddr
	vmov r1,rs // vmov.d r3:r2,rs
	call MEMHANDER<r1> // call MEMHANDLER64

	32/64 F W M
	mov r0,raddr
	call MEMHANDER<r1> // call MEMHANDLER64
	vmov rd,r0 // vmov.d rd,r3:r2
*/

/* What is left of the time slice, where the timers find it, before a call
 * that can reach an on-chip register: see cycle_counter in sh4_if.h. (r8
 * is the end of the context.) */
typedef char cycle_counter_where_ngen_arm_S_writes_it[offsetof(Sh4Context, cycle_counter) - sizeof(Sh4Context) == (size_t)-136 ? 1 : -1];

static void slice_left(int CC)
{
	a32_ls_imm(A32_STR, CYC, CTX, -136, A32_OFFSET, CC);
}

/* The stubs ngen_init() makes: by whether a read, the size and the
 * register the address is in; and for the floating-point ones, by whether
 * a read and the size */
static uintptr_t _mem_hndl[2][3][14];
static uintptr_t _mem_func[2][5];

static const struct
{
	u32 mask;
	u32 key;
	bool read;
	enum mem_op_type optp;
	s32 offs;
}
op_table[] =
{
	/* LDRSB */
	{ 0x0E500FF0, 0x001000D0, true, SZ_8, 1 },
	/* LDRSH */
	{ 0x0E500FF0, 0x001000F0, true, SZ_16, 1 },
	/* LDR */
	{ 0x0E500010, 0x06100000, true, SZ_32I, 1 },
	/* VLDR.32 */
	{ 0x0F300F00, 0x0D100A00, true, SZ_32F, 2 },
	/* VLDR.64 */
	{ 0x0F300F00, 0x0D100B00, true, SZ_64F, 2 },

	/* STRB */
	{ 0x0FF00010, 0x07C00000, false, SZ_8, 1 },
	/* STRH */
	{ 0x0FF00FF0, 0x018000B0, false, SZ_16, 1 },
	/* STR */
	{ 0x0E500010, 0x06000000, false, SZ_32I, 1 },
	/* VSTR.32 */
	{ 0x0F300F00, 0x0D000A00, false, SZ_32F, 2 },
	/* VSTR.64 */
	{ 0x0F300F00, 0x0D000B00, false, SZ_64F, 2 },

	{ 0, 0, false, SZ_8, 0 }
};

/* The fields of a load or store */
#define MEM_RA(w) ((w) & 15)
#define MEM_RT(w) (((w) >> 12) & 15)
#define MEM_RN(w) (((w) >> 16) & 15)
#define MEM_D(w) (((w) >> 22) & 1)

/* A branch, at @at, to where the code is now */
static void patch_branch(u32 *at, u32 cond_op)
{
	*at = cond_op | ((u32)((u32 *)A32_HERE() - at - 2) & 0x00FFFFFF);
}

#ifndef NO_MMU
/* The address of the instruction @op is for, as an exception has to have
 * it: in a delay slot, the branch's */
static u32 mmu_op_pc(const RuntimeBlockInfo *block, const shil_opcode *op)
{
	return block->vaddr + op->guest_offs - (op->delay_slot ? 2 : 0);
}

/* A load or store with the MMU on: a call, with the instruction's address
 * for the exception it may raise. Otherwise as vmem_slowpath(). */
static void mmu_slowpath(const RuntimeBlockInfo *block, const shil_opcode *op, int raddr, int rt, int ft, int fd,
		enum mem_op_type optp, bool read)
{
	const u32 pc = mmu_op_pc(block, op);
	u32 *miss = NULL, *hit = NULL, *unaligned = NULL, *crosses = NULL;

	/* First the table of translations kept by (mmu.h): on a hit, straight
	 * to the page on the host. The call below is for a miss, and fills the
	 * table in. r1 to r3 are free here but for the data of a store, which
	 * can be in r2.
	 *
	 * Not for an address that is not a multiple of the access's size: that
	 * is an address error, which the call raises. 64 bits are two accesses
	 * of 32 to the SH4, so a multiple of 4 will do for them - but then the
	 * second half must not be in the next page, which is another page
	 * altogether: the call's again. */
	if (optp != SZ_8)
	{
		a32_tst_imm(raddr, optp == SZ_16 ? 1 : 3, AL);
		unaligned = (u32 *)A32_HERE();
		a32_mov(A32_R0, A32_R0, AL);		/* "bne" to the call */
	}
	if (optp == SZ_64F)
	{
		a32_op_imm(A32_ADD, A32_R1, raddr, 4, AL);
		a32_ubfx(A32_R1, A32_R1, 0, 12, AL);
		a32_cmp_imm(A32_R1, 0, AL);
		crosses = (u32 *)A32_HERE();
		a32_mov(A32_R0, A32_R0, AL);		/* "beq" to the call */
	}
	a32_mov32(A32_R3, (u32)(uintptr_t)(read ? mmu_read_lut : mmu_write_lut), AL);
	a32_shift_imm(A32_LSR, false, A32_R1, raddr, 12, AL);
	a32_ls_reg(A32_LDR, A32_R1, A32_R3, A32_R1, A32_OFFSET, true, A32_LSL, 2, AL);
	a32_cmp_imm(A32_R1, 0, AL);
	miss = (u32 *)A32_HERE();
	a32_mov(A32_R0, A32_R0, AL);			/* "beq" to the call, once it is known where that is */
	/* (r1: what to add to the address to be at the page on the host) */
	if (read)
	{
		switch (optp)
		{
		case SZ_8:   a32_lsh_reg(A32_LDRSB, rt, A32_R1, raddr, true, AL); break;
		case SZ_16:  a32_lsh_reg(A32_LDRSH, rt, A32_R1, raddr, true, AL); break;
		case SZ_32I: a32_ls_reg(A32_LDR, rt, A32_R1, raddr, A32_OFFSET, true, A32_LSL, 0, AL); break;
		case SZ_64F: a32_op(A32_ADD, A32_R1, A32_R1, raddr, AL); a32_vls(true, true, fd, A32_R1, 0, AL); break;
		default:     a32_op(A32_ADD, A32_R1, A32_R1, raddr, AL); a32_vls(true, false, ft, A32_R1, 0, AL); break;
		}
	}
	else
	{
		switch (optp)
		{
		case SZ_8:   a32_ls_reg(A32_STRB, rt, A32_R1, raddr, A32_OFFSET, true, A32_LSL, 0, AL); break;
		case SZ_16:  a32_lsh_reg(A32_STRH, rt, A32_R1, raddr, true, AL); break;
		case SZ_32I: a32_ls_reg(A32_STR, rt, A32_R1, raddr, A32_OFFSET, true, A32_LSL, 0, AL); break;
		case SZ_64F: a32_op(A32_ADD, A32_R1, A32_R1, raddr, AL); a32_vls(false, true, fd, A32_R1, 0, AL); break;
		default:     a32_op(A32_ADD, A32_R1, A32_R1, raddr, AL); a32_vls(false, false, ft, A32_R1, 0, AL); break;
		}
	}
	hit = (u32 *)A32_HERE();
	a32_mov(A32_R0, A32_R0, AL);			/* "b" over the call */
	patch_branch(miss, 0x0A000000);
	if (unaligned)
		patch_branch(unaligned, 0x1A000000);
	if (crosses)
		patch_branch(crosses, 0x0A000000);

	slice_left(AL);
	if (raddr != A32_R0)
		a32_mov(A32_R0, raddr, AL);

	if (read)
	{
		a32_mov32(A32_R1, pc, AL);
		switch (optp)
		{
		case SZ_8:  a32_call((uintptr_t)rec_arm_mmu_read8, AL); break;
		case SZ_16: a32_call((uintptr_t)rec_arm_mmu_read16, AL); break;
		case SZ_32I:
		case SZ_32F: a32_call((uintptr_t)rec_arm_mmu_read32, AL); break;
		default:    a32_call((uintptr_t)rec_arm_mmu_read64, AL); break;
		}
		if (optp <= SZ_32I) a32_mov(rt, A32_R0, AL);
		else if (optp == SZ_32F) a32_vmov_sr(ft, A32_R0, AL);
		else a32_vmov_drr(fd, A32_R0, A32_R1, AL);
	}
	else
	{
		if (optp == SZ_64F)
		{
			a32_vmov_rrd(A32_R2, A32_R3, fd, AL);
			a32_mov32(A32_R1, pc, AL);
			a32_call((uintptr_t)rec_arm_mmu_write64, AL);
		}
		else
		{
			if (optp == SZ_32F) a32_vmov_rs(A32_R1, ft, AL);
			else a32_mov(A32_R1, rt, AL);
			a32_mov32(A32_R2, pc, AL);
			switch (optp)
			{
			case SZ_8:  a32_call((uintptr_t)rec_arm_mmu_write8, AL); break;
			case SZ_16: a32_call((uintptr_t)rec_arm_mmu_write16, AL); break;
			default:    a32_call((uintptr_t)rec_arm_mmu_write32, AL); break;
			}
		}
	}
	patch_branch(hit, 0xEA000000);
}
#endif

/* The call that does a load or store, with the address in @raddr and what
 * is stored in, or what is loaded wanted in, @rt, @ft or @fd */
static void mem_call(int raddr, int rt, int ft, int fd, enum mem_op_type optp, bool read, uintptr_t funct)
{
	if (!read)
	{
		if (optp <= SZ_32I) a32_mov(A32_R1, rt, AL);
		else if (optp == SZ_32F) a32_vmov_rs(A32_R1, ft, AL);
		else if (optp == SZ_64F) a32_vmov_rrd(A32_R2, A32_R3, fd, AL);
	}

	if (fd != 0 && optp == SZ_64F)
		die("BLAH");

	verify(funct != 0);
	a32_call(funct, AL);

	if (read)
	{
		if (optp <= SZ_32I) a32_mov(rt, A32_R0, AL);
		else if (optp == SZ_32F) a32_vmov_sr(ft, A32_R0, AL);
		else if (optp == SZ_64F) a32_vmov_drr(fd, A32_R0, A32_R1, AL);
	}
	(void)raddr;
}

static void vmem_slowpath(int raddr, int rt, int ft, int fd, enum mem_op_type optp, bool read)
{
	if (raddr != A32_R0)
		a32_mov(A32_R0, raddr, AL);
	mem_call(raddr, rt, ft, fd, optp, read,
			optp <= SZ_32I ? _mem_hndl[read][optp][raddr] : _mem_func[read][optp]);
}

/* A fast access (a load or store through the host's mapping of the SH4's
 * memory) faulted: it is written over with the call that does it by
 * asking, and run again. */
u32* ngen_readm_fail_v2(u32* ptrv, u32* regs, u32 fault_addr)
{
	u32 *ptr = ptrv;
	enum mem_op_type optp = SZ_8;
	u32 read = 0;
	s32 offs = -1;
	u32 fop = ptr[0];
	int i, raddr, rt, ft, fd;
	u32 sh4_addr, fault_offs;

	for (i = 0; op_table[i].mask; i++)
	{
		if ((fop & op_table[i].mask) == op_table[i].key)
		{
			optp = op_table[i].optp;
			read = op_table[i].read;
			offs = op_table[i].offs;
		}
	}

	if (offs == -1)
	{
		ERROR_LOG(DYNAREC, "%08X : invalid size\n", fop);
		die("can't decode opcode\n");
	}

	ptr -= offs;

	/* the registers, from the instructions */
	if ((ptr[0] & 0x0FE00070) == 0x07E00050)
		raddr = MEM_RA(ptr[0]);		/* from ubfx ! */
	else if ((ptr[0] & 0x0FE00000) == 0x03C00000)
		raddr = MEM_RN(ptr[0]);
	else
	{
		ERROR_LOG(DYNAREC, "fail raddr %08X {@%08X}:(\n", ptr[0], regs[1]);
		die("Invalid opcode: vmem fixup\n");
		return ptr;
	}
	/* from mem op */
	rt = MEM_RT(ptr[offs]);
	ft = MEM_RT(ptr[offs]) * 2 + MEM_D(ptr[offs]);
	fd = MEM_D(ptr[offs]) * 16 + MEM_RT(ptr[offs]);

	sh4_addr = regs[raddr];
	fault_offs = fault_addr - regs[8];

	/* fault offset must always be the addr from ubfx (sanity check) */
	verify((fault_offs == 0) || fault_offs == (0x1FFFFFFF & sh4_addr));
	(void)sh4_addr;
	(void)fault_offs;

	verify(emit_ptr == 0);
	emit_ptr = ptr;

	/*
			mov r0,raddr

		8/16/32I:
			call _mem_hdlp[read][optp][rt]
		32F/64F:
	32F,r:	vmov r1,ft
	64F,r:	vmov [r3:r2],fd
			call _mem_hdlp[read][optp][0]
	32F,w:	vmov ft,r0
	64F,w:	vmov fd,[r1:r0]
	*/
	if (offs == 2)
	{
		if (raddr != A32_R0)
			a32_mov(A32_R0, raddr, AL);
		else
			a32_nop();
	}

	mem_call(raddr, rt, ft, fd, optp, read != 0,
			offs == 1 ? _mem_hndl[read][optp][raddr] : offs == 2 ? _mem_func[read][optp] : 0);

	CacheFlush((void*)ptr, (void*)emit_ptr);
	emit_ptr = 0;

	return ptr;
}

static int GenMemAddr(const shil_opcode* op, int raddr)
{
	if (shil_is_imm(&op->rs3))
	{
		if (a32_is_imm(op->rs3._imm))
			a32_op_imm(A32_ADD, raddr, MAPG(&op->rs1), op->rs3._imm, AL);
		else
		{
			a32_mov32(A32_R1, op->rs3._imm, AL);
			a32_op(A32_ADD, raddr, MAPG(&op->rs1), A32_R1, AL);
		}
	}
	else if (shil_is_r32i(&op->rs3))
		a32_op(A32_ADD, raddr, MAPG(&op->rs1), MAPG(&op->rs3), AL);
	else if (!shil_is_null(&op->rs3))
	{
		ERROR_LOG(DYNAREC, "rs3: %08X\n", op->rs3.type);
		die("invalid rs3");
	}
	else if (shil_is_imm(&op->rs1))
		a32_mov32(raddr, op->rs1._imm, AL);
	else
		raddr = MAPG(&op->rs1);

	return raddr;
}

/* A pair of the SH4's floating-point registers (DRn, XDn) to and from d0,
 * where 64 bits are loaded, stored and handed to and from functions: s0 is
 * the low half of d0, as the first of the pair is in memory. Or to and
 * from the context, where the block does not have the pair. */
static void D0ToPair(const shil_param *prm)
{
	if (IS_ALLOCF(prm))
	{
		a32_vmov_ss(MAPF_I(prm, 0), 0, AL);
		a32_vmov_ss(MAPF_I(prm, 1), 1, AL);
	}
	else
		a32_vls(false, true, 0, CTX, reg_nofs(prm) / 4, AL);
}

static void PairToD0(const shil_param *prm)
{
	if (IS_ALLOCF(prm))
	{
		a32_vmov_ss(0, MAPF_I(prm, 0), AL);
		a32_vmov_ss(1, MAPF_I(prm, 1), AL);
	}
	else
		a32_vls(true, true, 0, CTX, reg_nofs(prm) / 4, AL);
}

static bool ngen_readm_immediate(const shil_opcode* op)
{
	enum mem_op_type optp;
	bool isram = false;
	void* ptr;
	int rd;

	if (!shil_is_imm(&op->rs1))
		return false;

	optp = memop_type(op);
#ifndef NO_MMU
	/* an address the MMU translates is not known to be anywhere until it is used */
	if (mmu_enabled() && mmu_is_translated_c(op->rs1._imm, memop_bytes(optp), MMU_TT_DREAD))
		return false;
#endif
	ptr = _vmem_read_ptr(op->rs1._imm, &isram, memop_bytes(optp) < 4 ? memop_bytes(optp) : 4);
	rd = (optp != SZ_32F && optp != SZ_64F) ? MAPG(&op->rd) : A32_R0;

	if (isram)
	{
		a32_mov32(A32_R0, (u32)(uintptr_t)ptr, AL);
		switch (optp)
		{
		case SZ_8:
			a32_lsh_imm(A32_LDRSB, rd, A32_R0, 0, AL);
			break;

		case SZ_16:
			a32_lsh_imm(A32_LDRSH, rd, A32_R0, 0, AL);
			break;

		case SZ_32I:
			a32_ls_imm(A32_LDR, rd, A32_R0, 0, A32_OFFSET, AL);
			break;

		case SZ_32F:
			a32_vls(true, false, MAPF(&op->rd), A32_R0, 0, AL);
			break;

		case SZ_64F:
			if (IS_ALLOCF(&op->rd))
			{
				a32_vls(true, false, MAPF_I(&op->rd, 0), A32_R0, 0, AL);
				a32_vls(true, false, MAPF_I(&op->rd, 1), A32_R0, 1, AL);
			}
			else
			{
				a32_vls(true, true, 0, A32_R0, 0, AL);
				a32_vls(false, true, 0, CTX, reg_nofs(&op->rd) / 4, AL);
			}
			break;
		}
	}
	else
	{
		/* Not RAM */
		if (sh4_area7(op->rs1._imm))
			slice_left(AL);
		if (optp == SZ_64F)
		{
			/* Need to call the handler twice */
			a32_mov32(A32_R0, op->rs1._imm, AL);
			a32_call((uintptr_t)ptr, AL);
			if (IS_ALLOCF(&op->rd))
				a32_vmov_sr(MAPF_I(&op->rd, 0), A32_R0, AL);
			else
				a32_ls_imm(A32_STR, A32_R0, CTX, reg_nofs(&op->rd), A32_OFFSET, AL);

			a32_mov32(A32_R0, op->rs1._imm + 4, AL);
			a32_call((uintptr_t)ptr, AL);
			if (IS_ALLOCF(&op->rd))
				a32_vmov_sr(MAPF_I(&op->rd, 1), A32_R0, AL);
			else
				a32_ls_imm(A32_STR, A32_R0, CTX, reg_nofs(&op->rd) + 4, A32_OFFSET, AL);
		}
		else
		{
			a32_mov32(A32_R0, op->rs1._imm, AL);
			a32_call((uintptr_t)ptr, AL);

			switch (optp)
			{
			case SZ_8:
				a32_extend(A32_SXTB, A32_R0, A32_R0, AL);
				break;

			case SZ_16:
				a32_extend(A32_SXTH, A32_R0, A32_R0, AL);
				break;

			case SZ_32I:
			case SZ_32F:
				break;

			default:
				die("Invalid size");
				break;
			}

			if (IS_ALLOCG(&op->rd))
				a32_mov(rd, A32_R0, AL);
			else if (IS_ALLOCF(&op->rd))
				a32_vmov_sr(MAPF(&op->rd), A32_R0, AL);
			else
				die("Unsupported");
		}
	}

	return true;
}

static bool ngen_writemem_immediate(const shil_opcode* op)
{
	enum mem_op_type optp;
	bool isram = false;
	void* ptr;
	int rs2 = A32_R1, rs2f = 0;

	if (!shil_is_imm(&op->rs1))
		return false;

	optp = memop_type(op);
#ifndef NO_MMU
	if (mmu_enabled() && mmu_is_translated_c(op->rs1._imm, memop_bytes(optp), MMU_TT_DWRITE))
		return false;
#endif
	/* (the handler for the size of the store: a byte or a word got the 32-bit one's) */
	ptr = _vmem_write_ptr(op->rs1._imm, &isram, memop_bytes(optp) < 4 ? memop_bytes(optp) : 4);
	/* 64 bits to a handler: the way for any address does it, as two writes */
	if (!isram && optp == SZ_64F)
		return false;

	if (shil_is_imm(&op->rs2))
		a32_mov32(rs2, op->rs2._imm, AL);
	else if (optp == SZ_32F)
		rs2f = MAPF(&op->rs2);
	else if (optp != SZ_64F)
		rs2 = MAPG(&op->rs2);

	if (isram)
	{
		a32_mov32(A32_R0, (u32)(uintptr_t)ptr, AL);
		switch (optp)
		{
		case SZ_8:
			a32_ls_imm(A32_STRB, rs2, A32_R0, 0, A32_OFFSET, AL);
			break;

		case SZ_16:
			a32_lsh_imm(A32_STRH, rs2, A32_R0, 0, AL);
			break;

		case SZ_32I:
			a32_ls_imm(A32_STR, rs2, A32_R0, 0, A32_OFFSET, AL);
			break;

		case SZ_32F:
			a32_vls(false, false, rs2f, A32_R0, 0, AL);
			break;

		case SZ_64F:
			if (IS_ALLOCF(&op->rs2))
			{
				a32_vls(false, false, MAPF_I(&op->rs2, 0), A32_R0, 0, AL);
				a32_vls(false, false, MAPF_I(&op->rs2, 1), A32_R0, 1, AL);
			}
			else
			{
				a32_vls(true, true, 0, CTX, reg_nofs(&op->rs2) / 4, AL);
				a32_vls(false, true, 0, A32_R0, 0, AL);
			}
			break;

		default:
			die("Invalid size");
			break;
		}
	}
	else
	{
		if (sh4_area7(op->rs1._imm))
			slice_left(AL);
		a32_mov32(A32_R0, op->rs1._imm, AL);
		/* (A handler takes the byte or the word it is given as it comes:
		 * extending a small argument is the caller's job on this host, and a
		 * compiler that holds its callers to that stored the register's
		 * other bits in whatever the handler keeps.) */
		if (optp == SZ_8)
			a32_extend(A32_UXTB, A32_R1, rs2, AL);
		else if (optp == SZ_16)
			a32_extend(A32_UXTH, A32_R1, rs2, AL);
		else if (optp == SZ_32F)
			a32_vmov_rs(A32_R1, rs2f, AL);
		else if (A32_R1 != rs2)
			a32_mov(A32_R1, rs2, AL);

		a32_call((uintptr_t)ptr, AL);
	}
	return true;
}

/* rd = 0, and 1 where the flags say @cc */
static void set_on(int rd, int cc)
{
	a32_movw(rd, 0, AL);
	a32_movw(rd, 1, cc);
}

static void ngen_compile_opcode(RuntimeBlockInfo* block, shil_opcode* op)
{
	switch (op->op)
	{
	case shop_readm:
		if (!ngen_readm_immediate(op))
		{
			const enum mem_op_type optp = memop_type(op);
			const int raddr = GenMemAddr(op, A32_R0);

#ifndef NO_MMU
			if (mmu_enabled())
			{
				switch (optp)
				{
				case SZ_8:
				case SZ_16:
				case SZ_32I:
					mmu_slowpath(block, op, raddr, MAPG(&op->rd), 0, 0, optp, true);
					break;

				case SZ_32F:
					mmu_slowpath(block, op, raddr, A32_R0, MAPF(&op->rd), 0, optp, true);
					break;

				case SZ_64F:
					mmu_slowpath(block, op, raddr, A32_R0, 0, 0, optp, true);
					D0ToPair(&op->rd);
					break;
				}
			}
			else
#endif
			if (_nvmem_enabled())
			{
				a32_op_imm(A32_BIC, A32_R1, raddr, 0xE0000000u, AL);

				switch (optp)
				{
				case SZ_8:
					a32_lsh_reg(A32_LDRSB, MAPG(&op->rd), A32_R1, CTX, true, AL);
					break;

				case SZ_16:
					a32_lsh_reg(A32_LDRSH, MAPG(&op->rd), A32_R1, CTX, true, AL);
					break;

				case SZ_32I:
					a32_ls_reg(A32_LDR, MAPG(&op->rd), A32_R1, CTX, A32_OFFSET, true, A32_LSL, 0, AL);
					break;

				case SZ_32F:
					a32_op(A32_ADD, A32_R1, A32_R1, CTX, AL);	/* there's no [REG+REG] VLDR */
					a32_vls(true, false, MAPF(&op->rd), A32_R1, 0, AL);
					break;

				case SZ_64F:
					a32_op(A32_ADD, A32_R1, A32_R1, CTX, AL);
					/* (into d0, which a rewrite of the load knows) */
					a32_vls(true, true, 0, A32_R1, 0, AL);
					D0ToPair(&op->rd);
					break;
				}
			}
			else
			{
				switch (optp)
				{
				case SZ_8:
				case SZ_16:
				case SZ_32I:
					vmem_slowpath(raddr, MAPG(&op->rd), 0, 0, optp, true);
					break;

				case SZ_32F:
					vmem_slowpath(raddr, A32_R0, MAPF(&op->rd), 0, optp, true);
					break;

				case SZ_64F:
					vmem_slowpath(raddr, A32_R0, 0, 0, optp, true);
					D0ToPair(&op->rd);
					break;
				}
			}
		}
		break;

	case shop_writem:
		if (!ngen_writemem_immediate(op))
		{
			const enum mem_op_type optp = memop_type(op);
			const int raddr = GenMemAddr(op, A32_R0);
			int rs2 = A32_R2, rs2f = 2;

			if (optp == SZ_64F)
				PairToD0(&op->rs2);
			else if (shil_is_imm(&op->rs2))
			{
				a32_mov32(rs2, op->rs2._imm, AL);
				if (optp == SZ_32F)
					a32_vmov_sr(rs2f, rs2, AL);
			}
			else
			{
				if (optp == SZ_32F)
					rs2f = MAPF(&op->rs2);
				else
					rs2 = MAPG(&op->rs2);
			}
#ifndef NO_MMU
			if (mmu_enabled())
			{
				if (optp == SZ_32F)
					mmu_slowpath(block, op, raddr, A32_R0, rs2f, 0, optp, false);
				else
					mmu_slowpath(block, op, raddr, rs2, 0, 0, optp, false);
			}
			else
#endif
			if (_nvmem_enabled())
			{
				a32_op_imm(A32_BIC, A32_R1, raddr, 0xE0000000u, AL);

				switch (optp)
				{
				case SZ_8:
					a32_ls_reg(A32_STRB, rs2, A32_R1, CTX, A32_OFFSET, true, A32_LSL, 0, AL);
					break;

				case SZ_16:
					a32_lsh_reg(A32_STRH, rs2, A32_R1, CTX, true, AL);
					break;

				case SZ_32I:
					a32_ls_reg(A32_STR, rs2, A32_R1, CTX, A32_OFFSET, true, A32_LSL, 0, AL);
					break;

				case SZ_32F:
					a32_op(A32_ADD, A32_R1, A32_R1, CTX, AL);	/* there's no [REG+REG] VSTR, also required for SQ */
					a32_vls(false, false, rs2f, A32_R1, 0, AL);
					break;

				case SZ_64F:
					a32_op(A32_ADD, A32_R1, A32_R1, CTX, AL);
					a32_vls(false, true, 0, A32_R1, 0, AL);
					break;
				}
			}
			else
			{
				switch (optp)
				{
				case SZ_8:
				case SZ_16:
				case SZ_32I:
					vmem_slowpath(raddr, rs2, 0, 0, optp, false);
					break;

				case SZ_32F:
					vmem_slowpath(raddr, A32_R0, rs2f, 0, optp, false);
					break;

				case SZ_64F:
					vmem_slowpath(raddr, A32_R0, 0, 0, optp, false);
					break;
				}
			}
		}
		break;

	/* dynamic jump, r+imm32. This will be at the end of the block, but
	 * doesn't -have- to be the last opcode */
	case shop_jdyn:
		verify(shil_is_reg(&op->rd) && op->rd._imm == reg_pc_dyn);
		if (shil_is_imm(&op->rs2))
		{
			a32_mov32(A32_R2, shil_imm_value(&op->rs2), AL);
			a32_op(A32_ADD, A32_R4, MAPG(&op->rs1), A32_R2, AL);
		}
		else
			a32_mov(A32_R4, MAPG(&op->rs1), AL);
		break;

	case shop_mov32:
		verify(shil_is_r32(&op->rd));

		if (shil_is_imm(&op->rs1))
		{
			if (shil_is_r32i(&op->rd))
				a32_mov32(MAPG(&op->rd), op->rs1._imm, AL);
			else if (op->rs1._imm == 0)
			{
				/* 1 - 1: vmov has no 0 */
				a32_vmov_imm(MAPF(&op->rd), A32_FPU_IMM_1, AL);
				a32_vfp3(A32_VSUB, MAPF(&op->rd), MAPF(&op->rd), MAPF(&op->rd), AL);
			}
			else if (op->rs1._imm == 0x3F800000)
				a32_vmov_imm(MAPF(&op->rd), A32_FPU_IMM_1, AL);
			else
			{
				a32_mov32(A32_R0, op->rs1._imm, AL);
				a32_vmov_sr(MAPF(&op->rd), A32_R0, AL);
			}
		}
		else if (shil_is_r32(&op->rs1))
		{
			u32 type = 0;

			if (IS_ALLOCF(&op->rd))
				type |= 1;
			if (IS_ALLOCF(&op->rs1))
				type |= 2;

			switch (type)
			{
			case 0: /* reg=reg */
				if (MAPG(&op->rd) != MAPG(&op->rs1))
					a32_mov(MAPG(&op->rd), MAPG(&op->rs1), AL);
				break;

			case 1: /* vfp=reg */
				a32_vmov_sr(MAPF(&op->rd), MAPG(&op->rs1), AL);
				break;

			case 2: /* reg=vfp */
				a32_vmov_rs(MAPG(&op->rd), MAPF(&op->rs1), AL);
				break;

			case 3: /* vfp=vfp */
				a32_vmov_ss(MAPF(&op->rd), MAPF(&op->rs1), AL);
				break;
			}
		}
		else
			die("Invalid mov32 size");
		break;

	case shop_mov64:
		verify(shil_is_r64f(&op->rs1) && shil_is_r64f(&op->rd));

		if (IS_ALLOCF(&op->rd))
		{
			const int rd0 = MAPF_I(&op->rd, 0);
			const int rd1 = MAPF_I(&op->rd, 1);
			const int rs0 = MAPF_I(&op->rs1, 0);
			const int rs1 = MAPF_I(&op->rs1, 1);
			/* The allocator can give the first of the destination the
			 * register the second of the source is in (a source used for
			 * the last time gives its register up): that one is moved out
			 * of the way first. */
			if (rd0 == rs1)
			{
				a32_vmov_ss(0, rs1, AL);
				a32_vmov_ss(rd0, rs0, AL);
				a32_vmov_ss(rd1, 0, AL);
			}
			else
			{
				if (rd0 != rs0)
					a32_vmov_ss(rd0, rs0, AL);
				if (rd1 != rs1)
					a32_vmov_ss(rd1, rs1, AL);
			}
		}
		else
		{
			a32_vls(true, true, 0, CTX, reg_nofs(&op->rs1) / 4, AL);
			a32_vls(false, true, 0, CTX, reg_nofs(&op->rd) / 4, AL);
		}
		break;

	case shop_jcond:
		verify(shil_is_reg(&op->rd) && op->rd._imm == reg_pc_dyn);
		a32_mov(A32_R4, MAPG(&op->rs1), AL);
		break;

	case shop_ifb:
		if (op->rs1._imm)
		{
			a32_mov32(A32_R1, op->rs2._imm, AL);
			StoreSh4Reg_mem(A32_R1, reg_nextpc, AL);
		}

		a32_mov32(A32_R0, op->rs3._imm, AL);
		slice_left(AL);
#ifndef NO_MMU
		if (mmu_enabled())
		{
			/* it may raise an exception: see "With the MMU on" */
			a32_mov32(A32_R1, (u32)(uintptr_t)OpPtr[op->rs3._imm], AL);
			a32_mov32(A32_R2, block->vaddr + op->guest_offs - (op->delay_slot ? 1 : 0), AL);
			a32_call((uintptr_t)rec_arm_mmu_fallback, AL);
			break;
		}
#endif
		a32_call((uintptr_t)OpPtr[op->rs3._imm], AL);
		break;

	case shop_neg:
		a32_op_imm(A32_RSB, MAPG(&op->rd), MAPG(&op->rs1), 0, AL);
		break;
	case shop_not:
		a32_dp_reg(A32_MVN, false, MAPG(&op->rd), 0, MAPG(&op->rs1), A32_LSL, 0, AL);
		break;

	case shop_shl: ngen_Shift(op, A32_LSL); break;
	case shop_shr: ngen_Shift(op, A32_LSR); break;
	case shop_sar: ngen_Shift(op, A32_ASR); break;
	case shop_ror: ngen_Shift(op, A32_ROR); break;

	case shop_and: ngen_Binary(op, A32_AND); break;
	case shop_or:  ngen_Binary(op, A32_ORR); break;
	case shop_xor: ngen_Binary(op, A32_EOR); break;

	case shop_add: ngen_Binary(op, A32_ADD); break;
	case shop_sub: ngen_Binary(op, A32_SUB); break;

	case shop_adc:
		{
			const int rs1 = GetParam(&op->rs1, A32_R1);
			const int rs2 = GetParam(&op->rs2, A32_R2);
			const int rs3 = GetParam(&op->rs3, A32_R3);

			a32_shift_imm(A32_LSR, true, A32_R0, rs3, 1, AL);	/* C=rs3, r0=0 */
			a32_dp_reg(A32_ADC, true, MAPG(&op->rd), rs1, rs2, A32_LSL, 0, AL);	/* (C,rd)=rs1+rs2+rs3(C) */
			a32_op_imm(A32_ADC, MAPG(&op->rd2), A32_R0, 0, AL);	/* rd2=C */
		}
		break;

	case shop_rocr:
		{
			const int rd2 = MAPG(&op->rd2);
			const int rs1 = GetParam(&op->rs1, A32_R1);
			const int rs2 = GetParam(&op->rs2, A32_R2);
			if (rd2 != rs1)
			{
				a32_shift_imm(A32_LSR, true, rd2, rs2, 1, AL);	/* C=rs2, rd2=0 */
				a32_op_imm(A32_AND, rd2, rs1, 1, AL);	/* get new carry */
			}
			else
			{
				a32_shift_imm(A32_LSR, true, A32_R0, rs2, 1, AL);	/* C=rs2, rd2=0 */
				a32_op_imm(A32_AND, A32_R0, rs1, 1, AL);	/* get new carry */
			}
			a32_shift_imm(A32_ROR, false, MAPG(&op->rd), rs1, 0, AL);	/* rrx: with the carry */
			if (rd2 == rs1)
				a32_mov(rd2, A32_R0, AL);
		}
		break;

	case shop_rocl:
		{
			const int rs1 = GetParam(&op->rs1, A32_R1);
			const int rs2 = GetParam(&op->rs2, A32_R2);
			a32_dp_reg(A32_ORR, true, MAPG(&op->rd), rs2, rs1, A32_LSL, 1, AL);	/* (C,rd)= rs1<<1 + (|) rs2 */
			a32_movw(MAPG(&op->rd2), 0, AL);	/* clear rd2 (for ADC/MOVCS) */
			a32_op_imm(A32_ADC, MAPG(&op->rd2), MAPG(&op->rd2), 0, AL);	/* rd2=C */
		}
		break;

	case shop_sbc:
		{
			const int rd2 = MAPG(&op->rd2);
			int rs1 = GetParam(&op->rs1, A32_R1), rs2, rs3;
			if (rs1 == rd2)
			{
				a32_mov(A32_R1, rs1, AL);
				rs1 = A32_R1;
			}
			rs2 = GetParam(&op->rs2, A32_R2);
			if (rs2 == rd2)
			{
				a32_mov(A32_R2, rs2, AL);
				rs2 = A32_R2;
			}
			rs3 = GetParam(&op->rs3, A32_R3);
			a32_op_imm(A32_EOR, rd2, rs3, 1, AL);
			a32_shift_imm(A32_LSR, true, rd2, rd2, 1, AL);	/* C=rs3, rd2=0 */
			a32_dp_reg(A32_SBC, true, MAPG(&op->rd), rs1, rs2, A32_LSL, 0, AL);
			a32_mov_imm(rd2, 1, A32_CC);
		}
		break;

	case shop_negc:
		{
			const int rd2 = MAPG(&op->rd2);
			int rs1 = GetParam(&op->rs1, A32_R1), rs2;
			if (rs1 == rd2)
			{
				a32_mov(A32_R1, rs1, AL);
				rs1 = A32_R1;
			}
			rs2 = GetParam(&op->rs2, A32_R2);
			a32_op_imm(A32_EOR, rd2, rs2, 1, AL);
			a32_shift_imm(A32_LSR, true, rd2, rd2, 1, AL);	/* C=rs3, rd2=0 */
			a32_dp_reg(A32_SBC, true, MAPG(&op->rd), rd2, rs1, A32_LSL, 0, AL);	/* rd2 == 0 */
			a32_mov_imm(rd2, 1, A32_CC);
		}
		break;

	case shop_shld:
	case shop_shad:
		{
			int rs1;
			verify(!shil_is_imm(&op->rs2));
			a32_dp_imm(A32_AND, true, A32_R0, MAPG(&op->rs2), 0x8000001Fu, AL);
			a32_op_imm(A32_RSB, A32_R0, A32_R0, 0x80000020u, A32_MI);
			rs1 = GetParam(&op->rs1, A32_R1);
			a32_shift_reg(op->op == shop_shld ? A32_LSR : A32_ASR, MAPG(&op->rd), rs1, A32_R0, A32_MI);
			a32_shift_reg(A32_LSL, MAPG(&op->rd), rs1, A32_R0, A32_PL);
		}
		break;

	case shop_sync_sr:
		/* must flush: SRS, SRT, r0-r7, r0b-r7b */
		a32_call((uintptr_t)UpdateSR, AL);
		break;

	case shop_test:
	case shop_seteq:
	case shop_setge:
	case shop_setgt:
	case shop_setae:
	case shop_setab:
		{
			static const int opcls2[] = { A32_EQ, A32_EQ, A32_GE, A32_GT, A32_HS, A32_HI };
			const int rd = MAPG(&op->rd);
			const int rs1 = GetParam(&op->rs1, A32_R0);
			int rs2 = A32_R1;
			bool is_imm = false;

			if (shil_is_imm(&op->rs2))
			{
				if (!a32_is_imm(op->rs2._imm))
					a32_mov32(rs2, op->rs2._imm, AL);
				else
					is_imm = true;
			}
			else if (shil_is_r32i(&op->rs2))
				rs2 = MAPG(&op->rs2);
			else
			{
				ERROR_LOG(DYNAREC, "ngen_Bin ??? %d\n", op->rs2.type);
				verify(false);
			}

			if (op->op == shop_test)
			{
				if (is_imm)
					a32_tst_imm(rs1, op->rs2._imm, AL);
				else
					a32_tst(rs1, rs2, AL);
			}
			else
			{
				if (is_imm)
					a32_cmp_imm(rs1, op->rs2._imm, AL);
				else
					a32_cmp(rs1, rs2, AL);
			}
			set_on(rd, opcls2[op->op - shop_test]);
		}
		break;

	case shop_setpeq:
		{
			const int rs1 = GetParam(&op->rs1, A32_R1);
			const int rs2 = GetParam(&op->rs2, A32_R2);
			a32_op(A32_EOR, A32_R1, rs1, rs2, AL);
			a32_movw(MAPG(&op->rd), 0, AL);

			a32_tst_imm(A32_R1, 0xFF000000u, AL);
			a32_tst_imm(A32_R1, 0x00FF0000u, A32_NE);
			a32_tst_imm(A32_R1, 0x0000FF00u, A32_NE);
			a32_tst_imm(A32_R1, 0x000000FFu, A32_NE);
			a32_movw(MAPG(&op->rd), 1, A32_EQ);
		}
		break;

	case shop_mul_u16:
	case shop_mul_s16:
		{
			const u32 ext = op->op == shop_mul_u16 ? A32_UXTH : A32_SXTH;
			const int rs2 = GetParam(&op->rs2, A32_R2);
			a32_extend(ext, A32_R1, MAPG(&op->rs1), AL);
			a32_extend(ext, A32_R2, rs2, AL);
			a32_mul(MAPG(&op->rd), A32_R1, A32_R2, AL);
		}
		break;
	case shop_mul_i32:
		{
			/* only the top 32 bits are different on signed vs unsigned */
			const int rs2 = GetParam(&op->rs2, A32_R2);
			a32_mul(MAPG(&op->rd), MAPG(&op->rs1), rs2, AL);
		}
		break;
	case shop_mul_u64:
	case shop_mul_s64:
		{
			const int rs2 = GetParam(&op->rs2, A32_R2);
			a32_mull(op->op == shop_mul_s64, MAPG(&op->rd2), MAPG(&op->rd), MAPG(&op->rs1), rs2, AL);
		}
		break;

	case shop_pref:
		{
			int cc = A32_EQ;
			if (!shil_is_imm(&op->rs1))
			{
				a32_shift_imm(A32_LSR, false, A32_R1, MAPG(&op->rs1), 26, AL);
				a32_mov(A32_R0, MAPG(&op->rs1), AL);
				a32_cmp_imm(A32_R1, 0x38, AL);
			}
			else
			{
				/* The SSA pass has already checked that the destination is a
				 * store queue so no need to check */
				a32_mov32(A32_R0, shil_imm_value(&op->rs1), AL);
				cc = AL;
			}

#ifndef NO_MMU
			if (mmu_enabled())
			{
				/* neither instruction of the move touches the flags */
				a32_mov32(A32_R1, block->vaddr + op->guest_offs - (op->delay_slot ? 1 : 0), AL);
				a32_call((uintptr_t)&rec_arm_mmu_sqw, cc);
			}
			else
#endif
			if (CCN_MMUCR.AT)
				a32_call((uintptr_t)&do_sqw_mmu, cc);
			else
			{
				a32_ls_imm(A32_LDR, A32_R2, CTX, rcb_noffs(&do_sqw_nommu), A32_OFFSET, AL);
				a32_op_imm(A32_SUB, A32_R1, CTX, (u32)-rcb_noffs(sq_both), AL);
				a32_blx(A32_R2, cc);
			}
		}
		break;

	case shop_ext_s8:
	case shop_ext_s16:
		verify(shil_is_r32i(&op->rd));
		verify(shil_is_r32i(&op->rs1));
		a32_extend(op->op == shop_ext_s8 ? A32_SXTB : A32_SXTH, MAPG(&op->rd), MAPG(&op->rs1), AL);
		break;

	case shop_xtrct:
		{
			/* The low half of the one and the high half of the other.
			 * Either may be a number the optimiser found, which is no
			 * register to look up: its half is put in r0 as it is. */
			const int rd = MAPG(&op->rd);
			if (shil_is_imm(&op->rs1) && shil_is_imm(&op->rs2))
				a32_mov32(rd, (op->rs1._imm >> 16) | (op->rs2._imm << 16), AL);
			else if (shil_is_imm(&op->rs1))
			{
				a32_mov32(A32_R0, op->rs1._imm >> 16, AL);
				a32_dp_reg(A32_ORR, false, rd, A32_R0, MAPG(&op->rs2), A32_LSL, 16, AL);
			}
			else if (shil_is_imm(&op->rs2))
			{
				a32_mov32(A32_R0, op->rs2._imm << 16, AL);
				a32_dp_reg(A32_ORR, false, rd, A32_R0, MAPG(&op->rs1), A32_LSR, 16, AL);
			}
			else
			{
				const int rs1 = MAPG(&op->rs1);
				const int rs2 = MAPG(&op->rs2);
				if (rd == rs1)
				{
					verify(rd != rs2);
					a32_shift_imm(A32_LSR, false, rd, rs1, 16, AL);
					a32_shift_imm(A32_LSL, false, A32_R0, rs2, 16, AL);
				}
				else
				{
					a32_shift_imm(A32_LSL, false, rd, rs2, 16, AL);
					a32_shift_imm(A32_LSR, false, A32_R0, rs1, 16, AL);
				}
				a32_op(A32_ORR, rd, rd, A32_R0, AL);
			}
		}
		break;

	/* FPU */

	case shop_fadd: ngen_fp_bin(op, A32_VADD); break;
	case shop_fsub: ngen_fp_bin(op, A32_VSUB); break;
	case shop_fmul: ngen_fp_bin(op, A32_VMUL); break;
	case shop_fdiv: ngen_fp_bin(op, A32_VDIV); break;

	case shop_fabs: ngen_fp_una(op, A32_VABS); break;
	case shop_fneg: ngen_fp_una(op, A32_VNEG); break;
	case shop_fsqrt: ngen_fp_una(op, A32_VSQRT); break;

	case shop_fmac:
		{
			const int rd = MAPF(&op->rd);
			int rs1 = 1, rs2 = 2, rs3 = 3;
			if (shil_is_imm(&op->rs1))
			{
				a32_mov32(A32_R0, shil_imm_value(&op->rs1), AL);
				a32_vmov_sr(rs1, A32_R0, AL);
			}
			else
				rs1 = MAPF(&op->rs1);
			if (shil_is_imm(&op->rs2))
			{
				a32_mov32(A32_R1, shil_imm_value(&op->rs2), AL);
				a32_vmov_sr(rs2, A32_R1, AL);
			}
			else
			{
				rs2 = MAPF(&op->rs2);
				if (rs2 == rd)
				{
					a32_vmov_ss(2, rs2, AL);
					rs2 = 2;
				}
			}
			if (shil_is_imm(&op->rs3))
			{
				a32_mov32(A32_R2, shil_imm_value(&op->rs3), AL);
				a32_vmov_sr(rs3, A32_R2, AL);
			}
			else
			{
				rs3 = MAPF(&op->rs3);
				if (rs3 == rd)
				{
					a32_vmov_ss(3, rs3, AL);
					rs3 = 3;
				}
			}
			if (rd != rs1)
				a32_vmov_ss(rd, rs1, AL);
			a32_vfp3(A32_VMLA, rd, rs2, rs3, AL);
		}
		break;

	case shop_fsrra:
		a32_vmov_imm(1, A32_FPU_IMM_1, AL);
		a32_vfp2(A32_VSQRT, 0, MAPF(&op->rs1), AL);
		a32_vfp3(A32_VDIV, MAPF(&op->rd), 1, 0, AL);
		break;

	case shop_fsetgt:
	case shop_fseteq:
		a32_movw(MAPG(&op->rd), 0, AL);
		a32_vfp2(A32_VCMP, MAPF(&op->rs1), MAPF(&op->rs2), AL);
		a32_vmrs(A32_PC, AL);
		a32_movw(MAPG(&op->rd), 1, op->op == shop_fsetgt ? A32_GT : A32_EQ);
		break;

	case shop_fsca:
		/* r1: base ptr */
		a32_movw(A32_R1, (u32)(uintptr_t)sin_table & 0xFFFF, AL);
		/* (the angle may be a number the optimiser found) */
		if (shil_is_imm(&op->rs1))
			a32_movw(A32_R0, op->rs1._imm & 0xFFFF, AL);
		else
			a32_extend(A32_UXTH, A32_R0, MAPG(&op->rs1), AL);
		a32_movt(A32_R1, (u32)(uintptr_t)sin_table >> 16, AL);
		a32_dp_reg(A32_ADD, false, A32_R0, A32_R1, A32_R0, A32_LSL, 3, AL);

		if (IS_ALLOCF(&op->rd))
		{
			a32_vls(true, false, MAPF_I(&op->rd, 0), A32_R0, 0, AL);
			a32_vls(true, false, MAPF_I(&op->rd, 1), A32_R0, 1, AL);
		}
		else
		{
			a32_vls(true, true, 0, A32_R0, 0, AL);
			a32_vls(false, true, 0, CTX, reg_nofs(&op->rd) / 4, AL);
		}
		break;

	case shop_fipr:
		{
			int fs2 = 0, rd = MAPF(&op->rd);
			a32_op_imm(A32_SUB, A32_R0, CTX, reg_aofs(&op->rs1), AL);
			if (reg_aofs(&op->rs2) == reg_aofs(&op->rs1))
				a32_vlsm(true, 0, A32_R0, 2, false, AL);
			else
			{
				a32_op_imm(A32_SUB, A32_R1, CTX, reg_aofs(&op->rs2), AL);
				a32_vlsm(true, 0, A32_R0, 2, false, AL);
				a32_vlsm(true, 2, A32_R1, 2, false, AL);
				fs2 = 4;
			}
			a32_vfp3(A32_VMUL, rd, 0, fs2 + 0, AL);
			a32_vfp3(A32_VMLA, rd, 1, fs2 + 1, AL);
			a32_vfp3(A32_VMLA, rd, 2, fs2 + 2, AL);
			a32_vfp3(A32_VMLA, rd, 3, fs2 + 3, AL);
		}
		break;

	case shop_ftrv:
		{
			/* s0-s3: the vector; s4-s7: what comes out; s8-s15: the
			 * matrix, two columns at a time (faster than NEON) */
			int rdp = A32_R1;
			a32_op_imm(A32_SUB, A32_R2, CTX, reg_aofs(&op->rs2), AL);
			a32_op_imm(A32_SUB, A32_R1, CTX, reg_aofs(&op->rs1), AL);
			if (reg_aofs(&op->rs1) != reg_aofs(&op->rd))
			{
				rdp = A32_R0;
				a32_op_imm(A32_SUB, A32_R0, CTX, reg_aofs(&op->rd), AL);
			}

			a32_vlsm(true, 4, A32_R2, 2, true, AL);
			a32_vlsm(true, 0, A32_R1, 2, false, AL);

			a32_vfp3(A32_VMUL, 4, 8, 0, AL);
			a32_vfp3(A32_VMUL, 5, 9, 0, AL);
			a32_vfp3(A32_VMUL, 6, 10, 0, AL);
			a32_vfp3(A32_VMUL, 7, 11, 0, AL);

			a32_vlsm(true, 6, A32_R2, 2, true, AL);

			a32_vfp3(A32_VMLA, 4, 12, 1, AL);
			a32_vfp3(A32_VMLA, 5, 13, 1, AL);
			a32_vfp3(A32_VMLA, 6, 14, 1, AL);
			a32_vfp3(A32_VMLA, 7, 15, 1, AL);

			a32_vlsm(true, 4, A32_R2, 2, true, AL);

			a32_vfp3(A32_VMLA, 4, 8, 2, AL);
			a32_vfp3(A32_VMLA, 5, 9, 2, AL);
			a32_vfp3(A32_VMLA, 6, 10, 2, AL);
			a32_vfp3(A32_VMLA, 7, 11, 2, AL);

			a32_vlsm(true, 6, A32_R2, 2, false, AL);

			a32_vfp3(A32_VMLA, 4, 12, 3, AL);
			a32_vfp3(A32_VMLA, 5, 13, 3, AL);
			a32_vfp3(A32_VMLA, 6, 14, 3, AL);
			a32_vfp3(A32_VMLA, 7, 15, 3, AL);

			a32_vlsm(false, 2, rdp, 2, false, AL);
		}
		break;

	case shop_frswap:
		verify(op->rd._imm == op->rs2._imm);
		verify(op->rd2._imm == op->rs1._imm);

		verify(shil_count(&op->rs1) == 16 && shil_count(&op->rs2) == 16);
		verify(shil_count(&op->rd2) == 16 && shil_count(&op->rd) == 16);

		a32_op_imm(A32_SUB, A32_R0, CTX, reg_aofs(&op->rs1), AL);
		a32_op_imm(A32_SUB, A32_R1, CTX, reg_aofs(&op->rd), AL);
		/* Assumes no FPU reg alloc here: frswap touches all FPU regs, so
		 * all spans should be clear here */
		a32_vlsm(true, 0, A32_R1, 8, false, AL);
		a32_vlsm(true, 8, A32_R0, 8, false, AL);
		a32_vlsm(false, 0, A32_R0, 8, false, AL);
		a32_vlsm(false, 8, A32_R1, 8, false, AL);
		break;

	case shop_cvt_f2i_t:
		a32_vfp2(A32_VCVT_S32_F32, 0, MAPF(&op->rs1), AL);
		a32_vmov_rs(MAPG(&op->rd), 0, AL);
		/* The host's conversion gives the largest or smallest integer for a
		 * value too large, as the SH4 does, but 0 for a NaN, where the SH4
		 * gives 0x80000000. A NaN is the one value that does not compare
		 * with itself. */
		a32_vfp2(A32_VCMP, MAPF(&op->rs1), MAPF(&op->rs1), AL);
		a32_vmrs(A32_PC, AL);
		a32_movw(MAPG(&op->rd), 0, A32_VS);
		a32_movt(MAPG(&op->rd), 0x8000, A32_VS);
		break;

	case shop_cvt_i2f_n:
	case shop_cvt_i2f_z:
		a32_vmov_sr(0, MAPG(&op->rs1), AL);
		a32_vfp2(A32_VCVT_F32_S32, MAPF(&op->rd), 0, AL);
		break;

	default:
		shil_chf[op->op](op);
		break;
	}
}

void ngen_Compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
{
	u8* blk_start;
	u8* pEnd;
	u32 cyc;
	bool accurate;
	/* (whether something that changes the flags is emitted after the last op) */
	bool tail_flags_lost;
	size_t i;

	(void)reset;
	(void)optimise;
	block->code = (DynarecCodeEntryPtr)(uintptr_t)A32_HERE();

	sh4_regalloc_begin(&ra, block, alloc_regs, sizeof(alloc_regs) / sizeof(alloc_regs[0]),
			alloc_fpu, sizeof(alloc_fpu) / sizeof(alloc_fpu[0]));

	blk_start = (u8*)A32_HERE();

	if (staging)
	{
		a32_mov32(A32_R0, (u32)(uintptr_t)&block->staging_runs, AL);
		a32_ls_imm(A32_LDR, A32_R1, A32_R0, 0, A32_OFFSET, AL);
		a32_op_imm(A32_SUB, A32_R1, A32_R1, 1, AL);
		a32_ls_imm(A32_STR, A32_R1, A32_R0, 0, A32_OFFSET, AL);
	}
	/* pre-load the first reg alloc operations, for better efficiency */
	if (block->op_count != 0)
		sh4_regalloc_op_begin(&ra, &block->oplist[0], 0);

#ifndef NO_MMU
	if (mmu_enabled())
	{
		/* is this the block for the PC? The table it was found through goes by address alone */
		LoadSh4Reg_mem(A32_R1, reg_nextpc, AL);
		a32_mov32(A32_R2, block->vaddr, AL);
		a32_cmp(A32_R1, A32_R2, AL);
		a32_mov32(A32_R0, block->addr, AL);
		a32_jump((uintptr_t)ngen_blockcheckfail, A32_NE);
	}
#endif

	if (force_checks)
	{
		s32 sz = (s32)block->sh4_code_size;
		u32 addr = block->addr;
		a32_mov32(A32_R0, addr, AL);

		while (sz > 0)
		{
			if (sz > 2)
			{
				const u32* ptr = (const u32*)GetMemPtr(addr, 4);
				if (ptr != NULL)
				{
					a32_mov32(A32_R2, (u32)(uintptr_t)ptr, AL);
					a32_ls_imm(A32_LDR, A32_R2, A32_R2, 0, A32_OFFSET, AL);
					a32_mov32(A32_R1, *ptr, AL);
					a32_cmp(A32_R1, A32_R2, AL);

					a32_jump((uintptr_t)ngen_blockcheckfail, A32_NE);
				}
				addr += 4;
				sz -= 4;
			}
			else
			{
				const u16* ptr = (const u16 *)GetMemPtr(addr, 2);
				if (ptr != NULL)
				{
					a32_mov32(A32_R2, (u32)(uintptr_t)ptr, AL);
					a32_lsh_imm(A32_LDRH, A32_R2, A32_R2, 0, AL);
					a32_movw(A32_R1, *ptr, AL);
					a32_cmp(A32_R1, A32_R2, AL);

					a32_jump((uintptr_t)ngen_blockcheckfail, A32_NE);
				}
				addr += 2;
				sz -= 2;
			}
		}
	}

#ifndef NO_MMU
	/* After the checks, not before: a block that is not the one for this
	 * address, or whose code has been written over, is not there to raise
	 * anything (upstream 6f58d204b: Sega Rally 2 hung leaving its records
	 * menu). */
	if (mmu_enabled() && block->has_fpu_op)
	{
		/* with the FPU off (SR.FD), using it is an exception */
		u32 *fpu_on;
		LoadSh4Reg_mem(A32_R1, reg_sr_status, AL);
		a32_tst_imm(A32_R1, 1 << 15, AL);
		fpu_on = (u32 *)A32_HERE();
		a32_mov(A32_R0, A32_R0, AL);		/* "beq" over what follows, once its length is known */
		a32_mov32(A32_R0, block->vaddr, AL);
		a32_mov32(A32_R1, 0x800, AL);
		a32_mov32(A32_R2, 0x100, AL);
		a32_call((uintptr_t)Do_Exception, AL);
		LoadSh4Reg_mem(A32_R4, reg_nextpc, AL);
		a32_jump((uintptr_t)no_update_mmu, AL);
		patch_branch(fpu_on, 0x0A000000);
	}
#endif

	cyc = block->guest_cycles;
	if (!a32_is_imm(cyc))
		cyc &= ~3u;

	a32_dp_imm(A32_SUB, true, CYC, CYC, cyc, AL);
#ifndef NO_MMU
	if (mmu_enabled())
		a32_call((uintptr_t)intc_sched_mmu, A32_LE);
	else
#endif
	a32_call((uintptr_t)intc_sched, A32_LE);

	accurate = settings.dynarec.AccurateTiming && !mmu_enabled();
	if (accurate && sh4_block_writes(block))
	{
		/* a block that can change something other than a register says so: see wait_site.h */
		a32_mov32(A32_R0, (u32)(uintptr_t)&sh4_write_gen, AL);
		a32_ls_imm(A32_LDR, A32_R1, A32_R0, 0, A32_OFFSET, AL);
		a32_op_imm(A32_ADD, A32_R1, A32_R1, 1, AL);
		a32_ls_imm(A32_STR, A32_R1, A32_R0, 0, A32_OFFSET, AL);
	}

	/* compile the block's opcodes */
	for (i = 0; i < block->op_count; i++)
	{
		shil_opcode* op = &block->oplist[i];

		op->host_offs = (u16)((u8*)A32_HERE() - blk_start);

		if (i != 0)
			sh4_regalloc_op_begin(&ra, op, (int)i);

		ngen_compile_opcode(block, op);

		sh4_regalloc_op_end(&ra, op);
	}
	if (block->BlockType == BET_Cond_0 || block->BlockType == BET_Cond_1)
	{
		/* The arm reg containing sr.T, for the block's end when it is (re)linked */
		const shil_param param = shil_param_reg(reg_sr_T);
		if (IS_ALLOCG(&param))
			block->t_reg = MAPG(&param);
		else
			block->t_reg = -1;
	}
	sh4_regalloc_cleanup(&ra);

	tail_flags_lost = false;
	if (accurate && block->BranchBlock <= block->vaddr
			&& (block->BlockType == BET_StaticJump || block->BlockType == BET_StaticCall
				|| block->BlockType == BET_Cond_0 || block->BlockType == BET_Cond_1))
	{
		/* A block that can go back: every so often, see whether the game is
		 * only waiting, and take the passes it would still make in the time
		 * slice off the cycle counter, which is a register, if it is. See
		 * wait_site.h. This is ahead of the branch, so it is also passed on
		 * the way out of a loop; that changes nothing, since a pass that
		 * finds the registers as they were is one that goes round again. */
		WaitSite *site = sh4_wait_site(block);
		if (site != NULL)
		{
			u32 *branch;
			/* This changes the flags, and a conditional block's tail goes by
			 * the flags of its last instruction when it can: it has to be
			 * told to take T from where it is kept instead. (The registers
			 * that can hold T, and r4 with a delayed branch's condition in
			 * it, are ones a call leaves alone.) */
			tail_flags_lost = true;
			a32_mov32(A32_R0, (u32)(uintptr_t)&site->skip, AL);
			a32_ls_imm(A32_LDR, A32_R1, A32_R0, 0, A32_OFFSET, AL);
			a32_dp_imm(A32_SUB, true, A32_R1, A32_R1, 1, AL);
			a32_ls_imm(A32_STR, A32_R1, A32_R0, 0, A32_OFFSET, AL);
			/* "bpl over", written once the length of what it jumps is known */
			branch = (u32 *)A32_HERE();
			a32_mov(A32_R0, A32_R0, AL);
			a32_mov32(A32_R0, (u32)(uintptr_t)site, AL);
			a32_mov(A32_R1, CYC, AL);
			a32_call((uintptr_t)sh4_wait_check, AL);
			a32_mov(CYC, A32_R0, AL);
			patch_branch(branch, 0x5A000000);
		}
	}

	/* The condition the tail goes by: the one the last op leaves the flags
	 * as, where it is the one that sets T and nothing has changed them */
	block->tail_cc = -1;
	if ((block->BlockType == BET_Cond_0 || block->BlockType == BET_Cond_1) && !block->has_jcond
			&& !tail_flags_lost && block->op_count > 0
			&& block->oplist[block->op_count - 1].rd._imm == reg_sr_T)
	{
		int cc_set, cc_clear;
		if (cond_op(block->oplist[block->op_count - 1].op, &cc_set, &cc_clear))
			block->tail_cc = (block->BlockType & 1) == 1 ? cc_set : cc_clear;
	}

	/* Relink written bytes must be added to the count ! */
	block->relink_offset = (u32)((u8*)A32_HERE() - (u8*)(uintptr_t)block->code);
	block->relink_data = 0;

	emit_Skip(ngen_Relink(block));
	pEnd = (u8*)A32_HERE();

	/* Clear the area we've written to for cache */
	CacheFlush((void*)(uintptr_t)block->code, pEnd);

	/* blk_start might not be the same, due to profiling counters */
	block->host_opcodes = (u32)(pEnd - blk_start) / 4;

	/* host code size needs to cover the entire range of the block */
	block->host_code_size = (u32)(pEnd - (u8*)(uintptr_t)block->code);
}

void ngen_ResetBlocks(void)
{
}

void ngen_init(void)
{
	int s, i, read, sz;

	INFO_LOG(DYNAREC, "Initializing the ARM32 dynarec\n");
	verify((s32)FPCB_OFFSET == -0x2100000 || (s32)FPCB_OFFSET == -0x4100000);
	verify(rcb_noffs(p_sh4rcb->fpcb) == (s32)FPCB_OFFSET);

	sh4_regalloc_init(&ra, ALLOC_F64, arm_preload, arm_writeback, arm_preload_fpu, arm_writeback_fpu, NULL);
	ngen_FailedToFindBlock = &ngen_FailedToFindBlock_;

	for (s = 0; s < 6; s++)
	{
		const uintptr_t fn = s == 0 ? (uintptr_t)_vmem_ReadMem8SX32 : s == 1 ? (uintptr_t)_vmem_ReadMem16SX32
				: s == 2 ? (uintptr_t)_vmem_ReadMem32 : s == 3 ? (uintptr_t)_vmem_WriteMem8
				: s == 4 ? (uintptr_t)_vmem_WriteMem16 : (uintptr_t)_vmem_WriteMem32;
		const bool is_read = s <= 2;

		/* r0 to r13 */
		for (i = 0; i <= 13; i++)
		{
			uintptr_t v;
			if (i == 1 || i == 2 || i == 3 || i == 4 || i == 12 || i == 13)
				continue;

			/* (every one writes what is left of the time slice first: see slice_left()) */
			v = (uintptr_t)A32_HERE();
			slice_left(AL);
			if (i != 0)
				a32_mov(A32_R0, i, AL);
			/* (a byte or a word is extended for the handler, as in
			 * ngen_writemem_immediate(): the value is in r1) */
			if (s == 3)
				a32_extend(A32_UXTB, A32_R1, A32_R1, AL);
			else if (s == 4)
				a32_extend(A32_UXTH, A32_R1, A32_R1, AL);
			a32_jump(fn, AL);

			_mem_hndl[is_read][s % 3][i] = v;
		}
	}

	/* (the store queue's: made, and used by nothing) */
	for (i = 0; i <= 13; i++)
	{
		if (i == 1 || i == 2 || i == 3 || i == 4 || i == 12 || i == 13)
			continue;

		a32_op_imm(A32_AND, A32_R3, i, 0x3F, AL);
		a32_shift_imm(A32_LSR, false, A32_R2, i, 26, AL);
		a32_mov(A32_R0, i, AL);
		a32_op(A32_ADD, A32_R3, A32_R3, CTX, AL);
		a32_cmp_imm(A32_R2, 0x38, AL);
		slice_left(A32_NE);
		a32_jump((uintptr_t)&_vmem_WriteMem32, A32_NE);
		a32_ls_imm(A32_STR, A32_R1, A32_R3, rcb_noffs(sq_both), A32_OFFSET, AL);
		a32_bx(A32_LR, AL);
	}

	/* the floating-point ones, which take the address in r0 */
	for (read = 0; read < 2; read++)
		for (sz = 3; sz < 5; sz++)
		{
			const uintptr_t fn = sz == 3 ? (read ? (uintptr_t)_vmem_ReadMem32 : (uintptr_t)_vmem_WriteMem32)
					: (read ? (uintptr_t)_vmem_ReadMem64 : (uintptr_t)_vmem_WriteMem64);
			_mem_func[read][sz] = (uintptr_t)A32_HERE();
			slice_left(AL);
			a32_jump(fn, AL);
		}

	INFO_LOG(DYNAREC, "readm helpers: up to %p\n", A32_HERE());
	emit_SetBaseAddr();
}

void ngen_GetFeatures(ngen_features* dst)
{
	dst->InterpreterFallback = false;
	dst->OnlyDynamicEnds     = false;
}

#else
/* (for the other processors: C has no empty file) */
typedef int rec_arm_not_here;
#endif
