/*
	Copyright 2021 flyinghead

	This file is part of flycast.

    flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
/* The sound chip's DSP recompiler for 32-bit x86. It is the x86-64 one
 * (dsp_x64.c) step for step, so that the two leave the same state and
 * the same sound to the bit - a game played over netplay between the two
 * kinds of machine has to - with what this processor has: four registers
 * for the DSP's (INPUTS, ACC, B and Y), and the rest of what x86-64 keeps
 * in registers in words of this file's (Y_REG, ADRS_REG, MDEC_CT and
 * SHIFTED). Those are worked out again at the top of every sample, as
 * they are there, and are not in the state. */
#include "build.h"

#if HOST_CPU == CPU_X86 && FEAT_DSPREC != DYNAREC_NONE

#include "dsp.h"
#include "aica_if.h"
#include "x86_emit.h"
#include "hw/mem/_vmem.h"

DECL_ALIGN(4096) static u8 CodeBuffer[32 * 1024]
#if defined(_WIN32) || defined(__HAIKU__)
	;
#elif defined(__linux__)
	__attribute__((section(".text")));
#elif defined(__MACH__)
	__attribute__((section("__TEXT,.text")));
#else
	#error CodeBuffer code section unknown
#endif
static u8 *pCodeBuffer;

/* what x86-64 has in r13, r14 and r15, and in edx between the shifter and its uses */
static s32 y_reg;
static s32 adrs_reg;
static u32 mdec_ct;
static s32 shifted;

#define R_INPUTS X86_ESI	/* 24 bits */
#define R_ACC    X86_EDI	/* 26 bits */
#define R_B      X86_EBP	/* 26 bits */
#define R_Y      X86_EBX	/* 13 bits */

/* A word of memory at its address */
static x86_mem at(const void *p)
{
	return x86_m(-1, (s32)(uintptr_t)p);
}

/* Uses ecx */
static void calculate_addr(x86_emit *e, const struct dsp_t *DSP, int ADDR, const _INST *op)
{
	/* u32 ADDR = DSPData->MADRS[op.MASA]; */
	x86_load(e, ADDR, at(&DSPData->MADRS[op->MASA]));
	if (op->ADREB)
	{
		/* ADDR += ADRS_REG & 0x0FFF; */
		x86_load(e, X86_ECX, at(&adrs_reg));
		x86_alu_ri(e, false, X86_AND, X86_ECX, 0x0FFF);
		x86_add_rr(e, ADDR, X86_ECX);
	}
	if (op->NXADR)
		/* ADDR++; */
		x86_alu_ri(e, false, X86_ADD, ADDR, 1);
	if (!op->TABLE)
	{
		/* ADDR += DSP->regs.MDEC_CT; */
		x86_add_rm(e, ADDR, at(&mdec_ct));
		/* ADDR &= DSP->RBL; (RBL is constant for this program) */
		x86_alu_ri(e, false, X86_AND, ADDR, DSP->RBL);
	}
	else
		/* ADDR &= 0xFFFF; */
		x86_alu_ri(e, false, X86_AND, ADDR, 0xFFFF);

	/* ADDR <<= 1;	Word -> byte address */
	x86_shift_ri(e, X86_SHL, ADDR, 1);
	/* ADDR += DSP->RBP;	RBP is already a byte address, and constant for this program */
	x86_alu_ri(e, false, X86_ADD, ADDR, DSP->RBP);
	/* ADDR & ARAM_MASK, and even: see dsp_x64.c */
	x86_alu_ri(e, false, X86_AND, ADDR, ARAM_MASK & ~1u);
}

/* The program made into code at @code; false if it did not fit */
static bool dsp_compile(u8 *code, size_t room, struct dsp_t *DSP)
{
	x86_emit em;
	x86_emit *e = &em;
	int steps, step;
	const s32 temp = (s32)(uintptr_t)&DSP->TEMP[0];

	x86_init(e, code, room);
	DEBUG_LOG(AICA_ARM, "DSPAssembler::DSPCompile recompiling for x86 at %p\n", (void *)code);

	if (DSP->Stopped)
	{
		/* (nothing to do, and EFREG is left as it is: see dsp_interp.c) */
		x86_ret(e);
		return !e->full;
	}

	x86_push(e, X86_EBX);
	x86_push(e, X86_EBP);
	x86_push(e, X86_ESI);
	x86_push(e, X86_EDI);
	x86_alu_ri(e, false, X86_SUB, X86_ESP, 12);	/* 16-byte stack alignment */
	x86_xor_rr(e, R_ACC, R_ACC);
	x86_store_imm(e, at(&DSP->FRC_REG), 0);
	x86_store_imm(e, at(&y_reg), 0);
	x86_store_imm(e, at(&adrs_reg), 0);
	x86_load(e, X86_EAX, at(&DSP->regs.MDEC_CT));
	x86_store(e, at(&mdec_ct), X86_EAX);

	/* The empty steps after the last one in use are left out: see dsp_x64.c */
	steps = 128;
	while (steps > 0 && DSPData->MPRO[steps * 4 - 4] == 0 && DSPData->MPRO[steps * 4 - 3] == 0
			&& DSPData->MPRO[steps * 4 - 2] == 0 && DSPData->MPRO[steps * 4 - 1] == 0)
		steps--;

	for (step = 0; step < steps; ++step)
	{
		u32 *mpro = &DSPData->MPRO[step * 4];
		_INST op;
		const u32 COEF = step;
		int X;

		DecodeInst(mpro, &op);

		if (op.XSEL || op.YRL || (op.ADRL && op.SHIFT != 3))
		{
			/* (IRA is six bits of the program's: any value gets here,
			 * and the ones that select nothing read as 0) */
			if (op.IRA <= 0x1f)
				/* INPUTS = DSP->MEMS[op.IRA]; */
				x86_load(e, R_INPUTS, at(&DSP->MEMS[op.IRA]));
			else if (op.IRA <= 0x2F)
			{
				/* INPUTS = DSP->MIXS[op.IRA - 0x20] << 4;	MIXS is 20 bit */
				x86_load(e, R_INPUTS, at(&DSP->MIXS[op.IRA - 0x20]));
				x86_shift_ri(e, X86_SHL, R_INPUTS, 4);
			}
			else if (op.IRA <= 0x31)
			{
				/* INPUTS = DSPData->EXTS[op.IRA - 0x30] << 8;	EXTS is 16 bits */
				x86_load(e, R_INPUTS, at(&DSPData->EXTS[op.IRA - 0x30]));
				x86_shift_ri(e, X86_SHL, R_INPUTS, 8);
			}
			else
				x86_xor_rr(e, R_INPUTS, R_INPUTS);
		}

		if (op.IWT)
		{
			/* DSP->MEMS[op.IWA] = MEMVAL[step & 3];	MEMVAL was selected in previous MRD */
			x86_load(e, X86_EAX, at(&DSP->MEMVAL[step & 3]));
			x86_store(e, at(&DSP->MEMS[op.IWA]), X86_EAX);
		}

		/* Operand sel */
		/* B */
		if (!op.ZERO)
		{
			if (op.BSEL)
				/* B = ACC; */
				x86_mov_rr(e, R_B, R_ACC);
			else
			{
				/* B = DSP->TEMP[(TRA + DSP->regs.MDEC_CT) & 0x7F]; */
				x86_load(e, X86_EAX, at(&mdec_ct));
				if (op.TRA)
					x86_alu_ri(e, false, X86_ADD, X86_EAX, op.TRA);
				x86_alu_ri(e, false, X86_AND, X86_EAX, 0x7f);
				x86_load(e, R_B, x86_mi(-1, X86_EAX, 4, temp));
			}
			if (op.NEGB)
				/* B = 0 - B; */
				x86_neg(e, R_B);
		}

		if (op.TWT || op.FRCL || op.MWT || (op.ADRL && op.SHIFT == 3) || op.EWT)
		{
			/* Shifter
			 * There's a 1-step delay at the output of the X*Y + B adder. So we use the ACC value from the previous step.
			 * (worked out here, before X: it only takes ACC, and X may want ecx) */
			if (op.SHIFT == 0)
			{
				/* SHIFTED = clamp(ACC, -0x80000, 0x7FFFF) */
				x86_alu_ri(e, false, X86_CMP, R_ACC, 0xFF800000);
				x86_mov_ri(e, X86_EDX, 0xFF800000);
				x86_mov_ri(e, X86_EAX, 0x007FFFFF);
				x86_cmov(e, X86_CC_GE, X86_EDX, R_ACC);
				x86_alu_ri(e, false, X86_CMP, X86_EDX, 0x007FFFFF);
				x86_cmov(e, X86_CC_G, X86_EDX, X86_EAX);
			}
			else if (op.SHIFT == 1)
			{
				/* SHIFTED = ACC << 1;	x2 scale */
				x86_mov_rr(e, X86_ECX, R_ACC);
				x86_shift_ri(e, X86_SHL, X86_ECX, 1);
				/* SHIFTED = clamp(SHIFTED, -0x80000, 0x7FFFF) */
				x86_alu_ri(e, false, X86_CMP, X86_ECX, 0xFF800000);
				x86_mov_ri(e, X86_EDX, 0xFF800000);
				x86_mov_ri(e, X86_EAX, 0x007FFFFF);
				x86_cmov(e, X86_CC_GE, X86_EDX, X86_ECX);
				x86_alu_ri(e, false, X86_CMP, X86_EDX, 0x007FFFFF);
				x86_cmov(e, X86_CC_G, X86_EDX, X86_EAX);
			}
			else if (op.SHIFT == 2)
			{
				/* SHIFTED = ACC << 1;	x2 scale */
				x86_mov_rr(e, X86_EDX, R_ACC);
				x86_shift_ri(e, X86_SHL, X86_EDX, 1);
			}
			else if (op.SHIFT == 3)
				/* SHIFTED = ACC; */
				x86_mov_rr(e, X86_EDX, R_ACC);
			x86_store(e, at(&shifted), X86_EDX);
		}

		/* X */
		X = X86_ECX;
		if (op.XSEL)
			/* X = INPUTS; */
			X = R_INPUTS;
		else
		{
			/* X = DSP->TEMP[(TRA + DSP->regs.MDEC_CT) & 0x7F];
			 * (B was read from there, and will do unless it is negated: see dsp_x64.c) */
			if (!op.ZERO && !op.BSEL && !op.NEGB)
				X = R_B;
			else
			{
				x86_load(e, X86_EAX, at(&mdec_ct));
				if (op.TRA)
					x86_alu_ri(e, false, X86_ADD, X86_EAX, op.TRA);
				x86_alu_ri(e, false, X86_AND, X86_EAX, 0x7f);
				x86_load(e, X86_ECX, x86_mi(-1, X86_EAX, 4, temp));
			}
		}

		/* Y */
		if (op.YSEL == 0)
			/* Y = FRC_REG; */
			x86_load(e, R_Y, at(&DSP->FRC_REG));
		else if (op.YSEL == 1)
		{
			/* Y = DSPData->COEF[COEF] >> 3;	COEF is 16 bits */
			x86_movsx16(e, R_Y, at(&DSPData->COEF[COEF]));
			x86_shift_ri(e, X86_SAR, R_Y, 3);
		}
		else if (op.YSEL == 2)
		{
			/* Y = Y_REG >> 11; */
			x86_load(e, R_Y, at(&y_reg));
			x86_shift_ri(e, X86_SAR, R_Y, 11);
		}
		else if (op.YSEL == 3)
		{
			/* Y = (Y_REG >> 4) & 0x0FFF; */
			x86_load(e, R_Y, at(&y_reg));
			x86_shift_ri(e, X86_SAR, R_Y, 4);
			x86_alu_ri(e, false, X86_AND, R_Y, 0x0fff);
		}

		if (op.YRL)
			/* Y_REG = INPUTS; */
			x86_store(e, at(&y_reg), R_INPUTS);

		/* ACCUM
		 * ACC = (((s64)X * (s64)Y) >> 12) + B; */
		x86_mov_rr(e, X86_EAX, X);
		x86_imul1(e, R_Y);
		x86_shrd(e, X86_EAX, X86_EDX, 12);
		x86_mov_rr(e, R_ACC, X86_EAX);
		if (!op.ZERO)
			x86_add_rr(e, R_ACC, R_B);

		if (op.TWT)
		{
			/* DSP->TEMP[(op.TWA + DSP->regs.MDEC_CT) & 0x7F] = SHIFTED; */
			x86_load(e, X86_ECX, at(&mdec_ct));
			if (op.TWA)
				x86_alu_ri(e, false, X86_ADD, X86_ECX, op.TWA);
			x86_alu_ri(e, false, X86_AND, X86_ECX, 0x7f);
			x86_load(e, X86_EDX, at(&shifted));
			x86_store(e, x86_mi(-1, X86_ECX, 4, temp), X86_EDX);
		}

		if (op.FRCL)
		{
			x86_load(e, X86_ECX, at(&shifted));
			if (op.SHIFT == 3)
				/* FRC_REG = SHIFTED & 0x0FFF; */
				x86_alu_ri(e, false, X86_AND, X86_ECX, 0xFFF);
			else
				/* FRC_REG = SHIFTED >> 11; */
				x86_shift_ri(e, X86_SAR, X86_ECX, 11);
			x86_store(e, at(&DSP->FRC_REG), X86_ECX);
		}

		if (step & 1)
		{
			/* (what x86-64 keeps round the calls here is in memory, or in a register a call keeps) */
			const int ADDR = R_Y;
			const s32 ram = (s32)(uintptr_t)&aica_ram.data[0];
			if (op.MRD)			/* memory only allowed on odd. DoA inserts NOPs on even */
			{
				/* MEMVAL[(step + 2) & 3] = UNPACK(*(u16 *)&aica_ram[ADDR & ARAM_MASK]); */
				calculate_addr(e, DSP, ADDR, &op);
				x86_movzx16(e, X86_ECX, x86_m(ADDR, ram));
				x86_call(e, (uintptr_t)UNPACK);
				x86_store(e, at(&DSP->MEMVAL[(step + 2) & 3]), X86_EAX);
			}
			if (op.MWT)
			{
				/* *(u16 *)&aica_ram[ADDR & ARAM_MASK] = PACK(SHIFTED); */
				x86_load(e, X86_ECX, at(&shifted));
				x86_call(e, (uintptr_t)PACK);

				calculate_addr(e, DSP, ADDR, &op);
				x86_store16(e, x86_m(ADDR, ram), X86_EAX);
			}
		}

		if (op.ADRL)
		{
			if (op.SHIFT == 3)
			{
				/* ADRS_REG = SHIFTED >> 12; */
				x86_load(e, X86_EAX, at(&shifted));
				x86_shift_ri(e, X86_SAR, X86_EAX, 12);
			}
			else
			{
				/* ADRS_REG = INPUTS >> 16; */
				x86_mov_rr(e, X86_EAX, R_INPUTS);
				x86_shift_ri(e, X86_SAR, X86_EAX, 16);
			}
			x86_store(e, at(&adrs_reg), X86_EAX);
		}

		if (op.EWT)
		{
			/* DSPData->EFREG[op.EWA] = SHIFTED >> 8; */
			x86_load(e, X86_EDX, at(&shifted));
			x86_shift_ri(e, X86_SAR, X86_EDX, 8);
			x86_store(e, at(&DSPData->EFREG[op.EWA]), X86_EDX);
		}
	}
	/* DSP->regs.MDEC_CT-- */
	x86_mov_ri(e, X86_EAX, dsp.RBL + 1);
	x86_load(e, X86_ECX, at(&mdec_ct));
	x86_alu_ri(e, false, X86_SUB, X86_ECX, 1);
	/* if (dsp.regs.MDEC_CT == 0)
	 *	dsp.regs.MDEC_CT = dsp.RBL + 1;	RBL is ring buffer length - 1 */
	x86_cmov(e, X86_CC_E, X86_ECX, X86_EAX);
	x86_store(e, at(&DSP->regs.MDEC_CT), X86_ECX);

	x86_alu_ri(e, false, X86_ADD, X86_ESP, 12);
	x86_pop(e, X86_EDI);
	x86_pop(e, X86_ESI);
	x86_pop(e, X86_EBP);
	x86_pop(e, X86_EBX);

	x86_ret(e);
	return !e->full;
}

/* What the code was made from: see dsp_x64.c */
static u32 compiled_mpro[128 * 4];
static bool compiled;
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
	if (!dsp_compile(pCodeBuffer, sizeof(CodeBuffer), &dsp))
	{
		/* (no program is that long: a sample with it does nothing) */
		ERROR_LOG(AICA_ARM, "The DSP's program did not fit its code room\n");
		pCodeBuffer[0] = 0xC3;
		compiled = false;
	}
}

void dsp_init(void)
{
	memset(&dsp, 0, sizeof(dsp));
	dsp.RBL = 0x8000 - 1;
	dsp.RBP = 0;
	dsp.regs.MDEC_CT = 1;
	dsp.dyndirty = true;

	/* Once: see dsp_x64.c */
	if (pCodeBuffer == NULL
			&& !vmem_platform_prepare_jit_block(CodeBuffer, sizeof(CodeBuffer), (void**)&pCodeBuffer))
		die("mprotect failed in x86 dsp");
}

void dsp_step(void)
{
	if (dsp.dyndirty)
	{
		dsp.dyndirty = false;
		dsp_recompile();
	}

	((void (*)(void))(uintptr_t)pCodeBuffer)();
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
typedef int dsp_x86_not_here;
#endif
