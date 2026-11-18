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
/* The sound chip's DSP recompiler for AArch64: the 128-step program made
 * into one function that runs a sample. */
#include "build.h"

#if HOST_CPU == CPU_ARM64 && FEAT_DSPREC != DYNAREC_NONE

#include <stdio.h>
#include "dsp.h"
#include "aica_if.h"
#include "a64_emit.h"
#include "hw/mem/_vmem.h"

/* Where the code goes: in the DSP's state, made executable - except on
 * macOS on Apple's processors, where it is MAP_JIT memory, mapped the first
 * time and kept (NULL without it: no DSP then). */
#ifdef HOST_APPLE_JIT
static u8 *dsp_code;
#define DSP_CODE dsp_code
#else
#define DSP_CODE (&dsp.DynCode[0])
#endif

void vmem_platform_flush_cache(void *icache_start, void *icache_end, void *dcache_start, void *dcache_end);

/* x28 points to TEMP, x27 to DSPData */
#define R_INPUTS   25	/* 24 bits */
#define R_ACC      19	/* 26 bits - saved */
#define R_B        26	/* 26 bits - saved */
#define R_X        10	/* 24 bits */
#define R_Y        9	/* 13 bits */
#define R_FRC_REG  20	/* 13 bits - saved */
#define R_Y_REG    21	/* 24 bits - saved */
#define R_SHIFTED  24	/* 24 bits */
#define R_ADRS_REG 22	/* 13 bits unsigned - saved */
#define R_MDEC_CT  23	/* saved */

/* A load or store of a word of the DSP's state (from x28), or of its
 * registers' (from x27), @offset bytes in */
static void ldst_at(a64_emit *e, bool load, int rt, int base, ptrdiff_t offset)
{
	if (offset < 16384)
		a64_ldst_imm(e, load, rt, base, (u32)offset);
	else
	{
		a64_mov_imm(e, true, 0, (u64)offset);
		a64_ldst_reg(e, load, false, rt, base, 0, false);
	}
}

static ptrdiff_t dsp_offset(const struct dsp_t *DSP, const void *data, int index)
{
	return ((const u8 *)data - (const u8 *)DSP) - offsetof(dsp_t, TEMP) + index * 4;
}

static ptrdiff_t dspdata_offset(const void *data, int index)
{
	return ((const u8 *)data - (const u8 *)DSPData) + index * 4;
}

/* TEMP[w1] (TEMP is where x28 points) */
static void ldst_temp(a64_emit *e, bool load, int rt)
{
	a64_ldst_reg(e, load, false, rt, 28, 1, true);
}

/* A call out of the code */
static void call_runtime(a64_emit *e, uintptr_t function)
{
	/* (out of a branch's reach, 128 MB, only where the code is not in
	 * the core's own memory: see vmem_platform_alloc_jit()) */
	if (!a64_bl(e, function))
	{
		a64_mov_imm(e, true, A64_IP0, function);
		a64_blr(e, A64_IP0);
	}
}

static void calculate_addr(a64_emit *e, const struct dsp_t *DSP, int ADDR, const _INST *op)
{
	/* u32 ADDR = DSPData->MADRS[op.MASA]; */
	ldst_at(e, true, ADDR, 27, dspdata_offset(DSPData->MADRS, op->MASA));
	if (op->ADREB)
	{
		/* ADDR += ADRS_REG & 0x0FFF; */
		a64_ubfx(e, 0, R_ADRS_REG, 0, 12);
		a64_addsub_rr(e, false, false, false, ADDR, ADDR, 0, A64_LSL, 0);
	}
	if (op->NXADR)
		/* ADDR++; */
		a64_addsub_imm(e, false, false, false, ADDR, ADDR, 1);
	if (!op->TABLE)
	{
		/* ADDR += DSP->regs.MDEC_CT; */
		a64_addsub_rr(e, false, false, false, ADDR, ADDR, R_MDEC_CT, A64_LSL, 0);
		/* ADDR &= DSP->RBL; (RBL is constant for this program) */
		a64_and_imm(e, false, ADDR, ADDR, DSP->RBL);
	}
	else
		/* ADDR &= 0xFFFF; */
		a64_bfc(e, ADDR, 16, 16);

	/* ADDR <<= 1;	Word -> byte address */
	a64_lsl(e, ADDR, ADDR, 1);
	/* ADDR += DSP->RBP;	RBP is already a byte address, and constant for this program
	 * (kept even: it is a 16-bit word there, and RBP is only as good
	 * as the save state it may have come out of) */
	a64_addsub_imm(e, false, false, false, ADDR, ADDR, (s64)(DSP->RBP & ~1u));
	/* ADDR & ARAM_MASK */
	if (ARAM_SIZE == 2*1024*1024)
		a64_bfc(e, ADDR, 21, 11);
	else if (ARAM_SIZE == 8*1024*1024)
		a64_bfc(e, ADDR, 23, 9);
	else
		die("Unsupported ARAM_SIZE");
}

/* The program made into code at @code; false if it did not fit */
static bool dsp_compile(u8 *code, size_t room, struct dsp_t *DSP)
{
	a64_emit em;
	a64_emit *e = &em;
	int steps, step;

	a64_init(e, code, room);
	DEBUG_LOG(AICA_ARM, "DSPAssembler::DSPCompile recompiling for arm64 at %p\n", (void *)code);

	a64_ldstp(e, false, A64_PRE, 29, 30, A64_SP, -96);
	a64_ldstp(e, false, A64_OFFSET, 21, 22, A64_SP, 16);
	a64_ldstp(e, false, A64_OFFSET, 23, 24, A64_SP, 32);
	a64_ldstp(e, false, A64_OFFSET, 25, 26, A64_SP, 48);
	a64_ldstp(e, false, A64_OFFSET, 27, 28, A64_SP, 64);
	a64_ldstp(e, false, A64_OFFSET, 19, 20, A64_SP, 80);
	a64_mov_imm(e, true, 28, (uintptr_t)&DSP->TEMP[0]);	/* x28 points to TEMP, right after the code */
	a64_mov_imm(e, true, 27, (uintptr_t)DSPData);		/* x27 points to DSPData */

	a64_mov_imm(e, false, R_ACC, 0);
	a64_mov_imm(e, false, R_B, 0);
	a64_mov_imm(e, false, R_FRC_REG, 0);
	a64_mov_imm(e, false, R_Y_REG, 0);
	a64_mov_imm(e, false, R_ADRS_REG, 0);
	ldst_at(e, true, R_MDEC_CT, 28, dsp_offset(DSP, &DSP->regs.MDEC_CT, 0));

	/* The program is 128 steps, and a game's seldom fills them. A
	 * step that is all zeroes writes nothing: it only works out an
	 * accumulator for the step after it to pick up, and at the top of
	 * every sample that accumulator, with the other registers a step
	 * can read, starts from zero again. So nothing can see what the
	 * empty steps after the last one in use work out, and they are
	 * left out. Empty steps between used ones are kept: the one after
	 * may take what they leave. */
	steps = 128;
	while (steps > 0 && DSPData->MPRO[steps * 4 - 4] == 0 && DSPData->MPRO[steps * 4 - 3] == 0
			&& DSPData->MPRO[steps * 4 - 2] == 0 && DSPData->MPRO[steps * 4 - 1] == 0)
		steps--;

	for (step = 0; step < steps; ++step)
	{
		u32 *mpro = &DSPData->MPRO[step * 4];
		_INST op;
		const u32 COEF = step;
		int X_alias;

		DecodeInst(mpro, &op);

		if (op.XSEL || op.YRL || (op.ADRL && op.SHIFT != 3))
		{
			if (op.IRA <= 0x1f)
				/* INPUTS = DSP->MEMS[op.IRA]; */
				ldst_at(e, true, R_INPUTS, 28, dsp_offset(DSP, DSP->MEMS, op.IRA));
			else if (op.IRA <= 0x2F)
			{
				/* INPUTS = DSP->MIXS[op.IRA - 0x20] << 4;	MIXS is 20 bit */
				ldst_at(e, true, R_INPUTS, 28, dsp_offset(DSP, DSP->MIXS, op.IRA - 0x20));
				a64_lsl(e, R_INPUTS, R_INPUTS, 4);
			}
			else if (op.IRA <= 0x31)
			{
				/* INPUTS = DSPData->EXTS[op.IRA - 0x30] << 8;	EXTS is 16 bits */
				ldst_at(e, true, R_INPUTS, 27, dspdata_offset(DSPData->EXTS, op.IRA - 0x30));
				a64_lsl(e, R_INPUTS, R_INPUTS, 8);
			}
			else
				a64_mov_imm(e, false, R_INPUTS, 0);
		}

		if (op.IWT)
		{
			/* DSP->MEMS[op.IWA] = MEMVAL[step & 3];	MEMVAL was selected in previous MRD */
			ldst_at(e, true, 1, 28, dsp_offset(DSP, DSP->MEMVAL, step & 3));
			ldst_at(e, false, 1, 28, dsp_offset(DSP, DSP->MEMS, op.IWA));
		}

		/* Operand sel */
		/* B */
		if (!op.ZERO)
		{
			if (op.BSEL)
				/* B = ACC; */
				a64_mov_rr(e, false, R_B, R_ACC);
			else
			{
				/* B = DSP->TEMP[(TRA + DSP->regs.MDEC_CT) & 0x7F]; */
				if (op.TRA)
					a64_addsub_imm(e, false, false, false, 1, R_MDEC_CT, op.TRA);
				else
					a64_mov_rr(e, false, 1, R_MDEC_CT);
				a64_bfc(e, 1, 7, 25);
				ldst_temp(e, true, R_B);
			}
			if (op.NEGB)
				/* B = 0 - B; */
				a64_neg(e, R_B, R_B);
		}

		/* X */
		X_alias = R_X;
		if (op.XSEL)
			/* X = INPUTS; */
			X_alias = R_INPUTS;
		else
		{
			/* X = DSP->TEMP[(TRA + DSP->regs.MDEC_CT) & 0x7F];
			 * B was read from the same place and will do for X - unless
			 * the step takes B negated, which has been done to it by
			 * now: X is not negated. */
			if (!op.ZERO && !op.BSEL && !op.NEGB)
				X_alias = R_B;
			else
			{
				if (op.TRA)
					a64_addsub_imm(e, false, false, false, 1, R_MDEC_CT, op.TRA);
				else
					a64_mov_rr(e, false, 1, R_MDEC_CT);
				a64_bfc(e, 1, 7, 25);
				ldst_temp(e, true, R_X);
			}
		}

		/* Y */
		if (op.YSEL == 0)
			/* Y = FRC_REG; */
			a64_mov_rr(e, false, R_Y, R_FRC_REG);
		else if (op.YSEL == 1)
		{
			/* Y = DSPData->COEF[COEF] >> 3;	COEF is 16 bits */
			ldst_at(e, true, R_Y, 27, dspdata_offset(DSPData->COEF, COEF));
			a64_sbfx(e, R_Y, R_Y, 3, 13);
		}
		else if (op.YSEL == 2)
			/* Y = Y_REG >> 11; */
			a64_asr(e, false, R_Y, R_Y_REG, 11);
		else if (op.YSEL == 3)
			/* Y = (Y_REG >> 4) & 0x0FFF; */
			a64_ubfx(e, R_Y, R_Y_REG, 4, 12);

		if (op.YRL)
			/* Y_REG = INPUTS; */
			a64_mov_rr(e, false, R_Y_REG, R_INPUTS);

		if (op.TWT || op.FRCL || op.MWT || (op.ADRL && op.SHIFT == 3) || op.EWT)
		{
			/* Shifter
			 * There's a 1-step delay at the output of the X*Y + B adder. So we use the ACC value from the previous step. */
			if (op.SHIFT == 0)
			{
				/* SHIFTED = clamp(ACC, -0x800000, 0x7FFFFF) */
				a64_mov_imm(e, false, 0, 0x800000);
				a64_neg(e, 1, 0);
				a64_cmp_rr(e, R_ACC, 1);
				a64_csel(e, R_SHIFTED, 1, R_ACC, A64_LT);
				a64_addsub_imm(e, false, true, false, 0, 0, 1);
				a64_cmp_rr(e, R_SHIFTED, 0);
				a64_csel(e, R_SHIFTED, 0, R_SHIFTED, A64_GT);
			}
			else if (op.SHIFT == 1)
			{
				/* SHIFTED = ACC << 1;	x2 scale */
				a64_lsl(e, R_SHIFTED, R_ACC, 1);
				/* SHIFTED = clamp(SHIFTED, -0x800000, 0x7FFFFF) */
				a64_mov_imm(e, false, 0, 0x800000);
				a64_neg(e, 1, 0);
				a64_cmp_rr(e, R_SHIFTED, 1);
				a64_csel(e, R_SHIFTED, 1, R_SHIFTED, A64_LT);
				a64_addsub_imm(e, false, true, false, 0, 0, 1);
				a64_cmp_rr(e, R_SHIFTED, 0);
				a64_csel(e, R_SHIFTED, 0, R_SHIFTED, A64_GT);
			}
			else if (op.SHIFT == 2)
				/* SHIFTED = ACC << 1;	x2 scale */
				a64_lsl(e, R_SHIFTED, R_ACC, 1);
			else if (op.SHIFT == 3)
				/* SHIFTED = ACC; */
				a64_mov_rr(e, false, R_SHIFTED, R_ACC);
		}

		/* ACCUM
		 * ACC = (((s64)X * (s64)Y) >> 12) + B; */
		a64_sxtw(e, X_alias, X_alias);
		a64_sxtw(e, R_Y, R_Y);
		a64_mul64(e, 0, X_alias, R_Y);
		a64_asr(e, true, 0, 0, 12);
		if (op.ZERO)
			a64_mov_rr(e, false, R_ACC, 0);
		else
			a64_addsub_rr(e, false, false, false, R_ACC, 0, R_B, A64_LSL, 0);

		if (op.TWT)
		{
			/* DSP->TEMP[(op.TWA + DSP->regs.MDEC_CT) & 0x7F] = SHIFTED; */
			if (op.TWA)
				a64_addsub_imm(e, false, false, false, 1, R_MDEC_CT, op.TWA);
			else
				a64_mov_rr(e, false, 1, R_MDEC_CT);
			a64_bfc(e, 1, 7, 25);
			ldst_temp(e, false, R_SHIFTED);
		}

		if (op.FRCL)
		{
			if (op.SHIFT == 3)
				/* FRC_REG = SHIFTED & 0x0FFF; */
				a64_ubfx(e, R_FRC_REG, R_SHIFTED, 0, 12);
			else
				/* FRC_REG = SHIFTED >> 11; */
				a64_asr(e, false, R_FRC_REG, R_SHIFTED, 11);
		}

		if (step & 1)
		{
			const int ADDR = 11;
			if (op.MRD)			/* memory only allowed on odd. DoA inserts NOPs on even */
			{
				/* MEMVAL[(step + 2) & 3] = UNPACK(*(u16 *)&aica_ram[ADDR & ARAM_MASK]); */
				calculate_addr(e, DSP, ADDR, &op);
				a64_ldr_literal(e, 1, (uintptr_t)&aica_ram.data[0]);
				a64_ldst_reg(e, true, true, 0, 1, ADDR, false);
				call_runtime(e, (uintptr_t)UNPACK);
				a64_mov_rr(e, false, 2, 0);
				ldst_at(e, false, 2, 28, dsp_offset(DSP, DSP->MEMVAL, (step + 2) & 3));
			}
			if (op.MWT)
			{
				/* *(u16 *)&aica_ram[ADDR & ARAM_MASK] = PACK(SHIFTED); */
				a64_mov_rr(e, false, 0, R_SHIFTED);
				call_runtime(e, (uintptr_t)PACK);
				a64_mov_rr(e, false, 2, 0);

				calculate_addr(e, DSP, ADDR, &op);
				a64_ldr_literal(e, 1, (uintptr_t)&aica_ram.data[0]);
				a64_ldst_reg(e, false, true, 2, 1, ADDR, false);
			}
		}

		if (op.ADRL)
		{
			if (op.SHIFT == 3)
				/* ADRS_REG = SHIFTED >> 12; */
				a64_asr(e, false, R_ADRS_REG, R_SHIFTED, 12);
			else
				/* ADRS_REG = INPUTS >> 16; */
				a64_asr(e, false, R_ADRS_REG, R_INPUTS, 16);
		}

		if (op.EWT)
		{
			/* DSPData->EFREG[op.EWA] = SHIFTED >> 8; */
			const ptrdiff_t at = dspdata_offset(DSPData->EFREG, op.EWA);
			if (at >= 16384)
				a64_mov_imm(e, true, 0, (u64)at);
			a64_asr(e, false, 1, R_SHIFTED, 8);
			if (at >= 16384)
				a64_ldst_reg(e, false, false, 1, 27, 0, false);
			else
				a64_ldst_imm(e, false, 1, 27, (u32)at);
		}
	}
	/* DSP->regs.MDEC_CT-- */
	a64_addsub_imm(e, false, true, true, R_MDEC_CT, R_MDEC_CT, 1);
	/* if (dsp.regs.MDEC_CT == 0)
	 *	dsp.regs.MDEC_CT = dsp.RBL + 1;	RBL is ring buffer length - 1 */
	a64_mov_imm(e, false, 0, dsp.RBL + 1);
	a64_csel(e, R_MDEC_CT, 0, R_MDEC_CT, A64_EQ);
	ldst_at(e, false, R_MDEC_CT, 28, dsp_offset(DSP, &DSP->regs.MDEC_CT, 0));

	a64_ldstp(e, true, A64_OFFSET, 21, 22, A64_SP, 16);
	a64_ldstp(e, true, A64_OFFSET, 23, 24, A64_SP, 32);
	a64_ldstp(e, true, A64_OFFSET, 25, 26, A64_SP, 48);
	a64_ldstp(e, true, A64_OFFSET, 27, 28, A64_SP, 64);
	a64_ldstp(e, true, A64_OFFSET, 19, 20, A64_SP, 80);
	a64_ldstp(e, true, A64_POST, 29, 30, A64_SP, 96);
	a64_ret(e);

	a64_finish(e);

	vmem_platform_flush_cache(e->start, e->p, e->start, e->p);
	return !e->full;
}

/* The program the code was made from. Asked to make it again from the
 * same - a game that writes its program again, or a save state loaded that
 * has the one that is running, which with run-ahead is every frame -
 * nothing is made. */
static u32 compiled_mpro[128 * 4];
static bool compiled;
/* What else the code has in it as constants: the ring buffer's length and
 * place, which a game sets with the program left as it is, and where sound
 * memory is and its size, which another game may have otherwise. The same
 * program with any of them changed is made again. */
static u32 compiled_rbl, compiled_rbp, compiled_mask;
static u8 *compiled_ram;

static void dsp_recompile(void)
{
	int i;

	dsp.Stopped = true;
	for (i = 127; i >= 0; --i)
	{
		u32 *IPtr = DSPData->MPRO + i * 4;

		if (IPtr[0] != 0 || IPtr[1] != 0 || IPtr[2 ]!= 0 || IPtr[3] != 0)
		{
			dsp.Stopped = false;
			break;
		}
	}
	if (compiled && memcmp(compiled_mpro, DSPData->MPRO, sizeof(compiled_mpro)) == 0
			&& compiled_rbl == dsp.RBL && compiled_rbp == dsp.RBP
			&& compiled_mask == ARAM_MASK && compiled_ram == &aica_ram.data[0])
		return;
	memcpy(compiled_mpro, DSPData->MPRO, sizeof(compiled_mpro));
	compiled_rbl = dsp.RBL;
	compiled_rbp = dsp.RBP;
	compiled_mask = ARAM_MASK;
	compiled_ram = &aica_ram.data[0];
	compiled = true;
#ifdef HOST_APPLE_JIT
	if (DSP_CODE == NULL)
		return;
#endif
	vmem_jit_write_begin();
	if (!dsp_compile(DSP_CODE, sizeof(dsp.DynCode), &dsp))
	{
		/* (no program is that long: a sample with it does nothing) */
		ERROR_LOG(AICA_ARM, "The DSP's program did not fit its code room\n");
		dsp.Stopped = true;
		compiled = false;
	}
	vmem_jit_write_end();
}

void dsp_init(void)
{
#ifdef HOST_APPLE_JIT
	static bool asked;
#endif

	memset(&dsp, 0, sizeof(dsp));
	compiled = false;	/* (the code was in there) */
	dsp.RBL = 0x8000 - 1;
	dsp.RBP=0;
	dsp.regs.MDEC_CT = 1;
	dsp.dyndirty = true;

#ifdef HOST_APPLE_JIT
	if (!asked)
	{
		dsp_code = (u8 *)vmem_platform_alloc_jit(sizeof(dsp.DynCode));
		asked = true;
	}
#else
	if (!mem_region_set_exec(dsp.DynCode, sizeof(dsp.DynCode)))
	{
		perror("Couldn't mprotect DSP code");
		die("mprotect failed in arm64 dsp");
	}
#endif
}

void dsp_step(void)
{
	if (dsp.dyndirty)
	{
		dsp.dyndirty = false;
		dsp_recompile();
	}
	if (dsp.Stopped)
		return;
#ifdef HOST_APPLE_JIT
	if (DSP_CODE == NULL)
		return;
#endif
	((void (*)(void))(uintptr_t)DSP_CODE)();
}

void dsp_writenmem(u32 addr)
{
	if (addr >= 0x3400 && addr < 0x3C00)
	{
		dsp.dyndirty = true;
	}
	/* (TEMP and MEMS are written in aica_mem.c: they are the DSP's own) */
}

void dsp_term(void)
{
}
#else
/* (for the other processors: C has no empty file) */
typedef int dsp_arm64_not_here;
#endif
