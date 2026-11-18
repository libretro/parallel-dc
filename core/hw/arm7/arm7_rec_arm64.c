/*
	Copyright 2020 flyinghead

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

#include "build.h"

#if	HOST_CPU == CPU_ARM64 && FEAT_AREC != DYNAREC_NONE

#include "jit/a64_emit.h"
#include "arm7_rec.h"
#include "hw/aica/aica_if.h"
#include "hw/mem/_vmem.h"

/* The ARM7 recompiler's backend for ARM64, behind the same decoder as the
 * x86-64 one (arm7_rec.c): the blocks end at the same places and take the
 * same cycles, and each instruction does what the x86-64 code does for it,
 * flags and all, so that the two kinds of machine run a sound program the
 * same to the cycle.
 *
 * While a block runs: x28 points to the ARM's registers, x26 to the table
 * of entry points, and the ARM's registers the allocator gives a host one
 * are in w19-w25 and w27, all kept across calls. The ARM's flags are kept
 * in its register file, as on x86-64, and taken into the host's NZCV only
 * for the instruction that needs them: the two processors' flags are the
 * same four bits in the same places, with the same carry for subtraction.
 * The main loop's frame holds the sound memory's address and mask for the
 * loads and stores done in line. */

static void (*arm_dispatch)(void);

/* the dispatcher loads the next pc and the interrupt as a pair */
typedef char arm7_next_pc_and_interrupt[INTR_PEND == R15_ARM_NEXT + 1 ? 1 : -1];

#define MAX_REGS 8

/* where the main loop's frame keeps the sound memory's address and mask */
#define RAM_SLOT 96
#define MASK_SLOT 104
#define FRAME_SIZE 112

static const int alloc_regs[MAX_REGS] = { 19, 20, 21, 22, 23, 24, 25, 27 };

typedef struct Arm7Compiler
{
	a64_emit e;
	bool logical_op_set_flags;
	bool set_carry_bit;
	bool set_flags;
	ArmRegAlloc regalloc;
} Arm7Compiler;

static s64 arm_reg_offset(Arm7Reg reg)
{
	return (s64)((u8 *)&arm_Reg[reg].I - (u8 *)&arm_Reg[0].I);
}

static int get_reg(int i)
{
	verify(i >= 0 && i < MAX_REGS);
	return alloc_regs[i];
}

static int map_reg(Arm7Compiler *c, Arm7Reg r)
{
	return get_reg(arm_regalloc_map(&c->regalloc, r));
}

static void load_arm_reg(a64_emit *e, int rt, Arm7Reg reg)
{
	a64_ldst(e, true, false, rt, 28, arm_reg_offset(reg));
}

static void store_arm_reg(a64_emit *e, int rt, Arm7Reg reg)
{
	a64_ldst(e, false, false, rt, 28, arm_reg_offset(reg));
}

static void load_reg(void *ctx, int host_reg, Arm7Reg armreg)
{
	load_arm_reg(&((Arm7Compiler *)ctx)->e, get_reg(host_reg), armreg);
}

static void store_reg(void *ctx, int host_reg, Arm7Reg armreg)
{
	store_arm_reg(&((Arm7Compiler *)ctx)->e, get_reg(host_reg), armreg);
}

/* Where the code being made will run from */
static uintptr_t exec_cursor(const a64_emit *e)
{
	return (uintptr_t)arm7_write_to_exec(e->p);
}

/* A call to @target, in the core's own code. It is out of reach of a
 * branch from here (128 MB) only where the code is not in the core's
 * .text, as on macOS on Apple's processors can be (see
 * vmem_platform_alloc_jit()): the call is then made through a register. */
static void call(a64_emit *e, const void *target)
{
	if (!a64_b_rel(e, true, (s64)((intptr_t)target - (intptr_t)exec_cursor(e))))
	{
		a64_mov_imm(e, true, A64_IP0, (u64)(uintptr_t)target);
		a64_blr(e, A64_IP0);
	}
}

/* A branch to the main loop's dispatcher, which is in the same code buffer */
static void jump_to_dispatch(a64_emit *e)
{
	if (!a64_b_rel(e, false, (s64)((intptr_t)(uintptr_t)arm_dispatch - (intptr_t)exec_cursor(e))))
		e->full = true;
}

/* The value of an operand: a plain constant as it is, a register as it
 * is, anything shifted worked out into @scratch - with the carry the
 * shift leaves into w14 when a logical instruction that sets the flags
 * wants it, as the x86-64 backend does. */
static a64_operand get_operand(Arm7Compiler *c, const ArmOpOperand *arg, int scratch)
{
	a64_emit *e = &c->e;
	int rm;
	if (armop_is_imm(arg))
	{
		if (!armop_is_shifted(arg))
			return a64_opnd_imm(armop_get_imm(arg));
		/* r15 read as a value, with a shift the decoder left to be done */
		a64_mov_imm(e, false, scratch, armop_get_imm(arg));
		rm = scratch;
	}
	else
		rm = map_reg(c, armop_reg_of(arg)->armreg);
	if (!armop_is_shifted(arg))
		return a64_opnd_reg(rm, A64_LSL, 0);

	if (!arg->shift_imm)
	{
		/* shift by register: the count is the register's low byte */
		const int shift_reg = map_reg(c, arg->s.shift_reg.armreg);
		switch (arg->shift_type)
		{
		case ARMSH_LSL:
		case ARMSH_LSR:
			a64_bfm(e, false, 2, 3, shift_reg, 0, 7);		/* uxtb w3 */
			a64_shift_reg(e, arg->shift_type == ARMSH_LSL ? A64_LSL : A64_LSR, scratch, rm, 3);
			a64_addsub(e, false, true, true, A64_ZR, 3, a64_opnd_imm(32));
			a64_csel(e, scratch, scratch, A64_ZR, A64_LO);	/* LSL and LSR by 32 or more gives 0 */
			break;
		case ARMSH_ASR:
			a64_bfm(e, false, 2, 3, shift_reg, 0, 7);
			a64_asr(e, false, 13, rm, 31);
			a64_shift_reg(e, A64_ASR, scratch, rm, 3);
			a64_addsub(e, false, true, true, A64_ZR, 3, a64_opnd_imm(32));
			a64_csel(e, scratch, 13, scratch, A64_HS);	/* ASR by 32 or more gives 0 or -1 depending on operand sign */
			break;
		case ARMSH_ROR:
			a64_shift_reg(e, A64_ROR, scratch, rm, shift_reg);
			break;
		default:
			die("Invalid shift");
			break;
		}
		return a64_opnd_reg(scratch, A64_LSL, 0);
	}

	/* shift by immediate */
	if (arg->shift_type != ARMSH_ROR && arg->s.shift_value != 0 && !c->logical_op_set_flags)
		return a64_opnd_reg(rm, (int)arg->shift_type, arg->s.shift_value);

	if (arg->s.shift_value == 0)
	{
		/* Shift by 32 */
		if (c->logical_op_set_flags)
			c->set_carry_bit = true;
		if (arg->shift_type == ARMSH_LSR)
		{
			if (c->set_carry_bit)
				a64_ubfx(e, 14, rm, 31, 1);			/* w14 = rm[31] */
			a64_mov_imm(e, false, scratch, 0);		/* scratch = 0 */
		}
		else if (arg->shift_type == ARMSH_ASR)
		{
			if (c->set_carry_bit)
				a64_ubfx(e, 14, rm, 31, 1);			/* w14 = rm[31] */
			a64_asr(e, false, scratch, rm, 31);		/* scratch = rm < 0 ? -1 : 0 */
		}
		else if (arg->shift_type == ARMSH_ROR)
		{
			/* RRX, with the ARM's carry as it is in its register file */
			load_arm_reg(e, 15, RN_PSR_FLAGS);
			a64_ubfx(e, 15, 15, 29, 1);				/* w15 = C */
			if (c->set_carry_bit)
				a64_ubfx(e, 14, rm, 0, 1);			/* w14 = rm[0] (new C) */
			a64_shift_imm(e, A64_LSR, 13, rm, 1);
			a64_logical(e, false, 1, false, scratch, 13, a64_opnd_reg(15, A64_LSL, 31));
		}
		else
			die("Invalid shift");
	}
	else
	{
		/* Carry must be preserved or Ror shift */
		if (c->logical_op_set_flags)
			c->set_carry_bit = true;
		if (c->set_carry_bit)
		{
			if (arg->shift_type == ARMSH_LSL)
				a64_ubfx(e, 14, rm, 32 - arg->s.shift_value, 1);	/* w14 = rm[lsb] */
			else
				a64_ubfx(e, 14, rm, arg->s.shift_value - 1, 1);	/* w14 = rm[msb] */
		}
		a64_shift_imm(e, (int)arg->shift_type, scratch, rm, arg->s.shift_value);
	}
	return a64_opnd_reg(scratch, A64_LSL, 0);
}

/* An operand in a register: a constant is put in @scratch */
static int to_register(Arm7Compiler *c, a64_operand op, int scratch)
{
	if (op.is_imm)
	{
		a64_mov_imm(&c->e, false, scratch, op.imm);
		return scratch;
	}
	if (op.amount == 0)
		return op.reg;
	a64_mov_op(&c->e, false, scratch, op);
	return scratch;
}

/* The code that skips what follows unless @cc holds; false for none */
static bool start_conditional(Arm7Compiler *c, ArmCondition cc, a64_label *label)
{
	if (cc == ARMCC_AL)
		return false;
	verify(cc <= ARMCC_LE);
	a64_label_init(label);
	/* The ARM's conditions are encoded as the host's */
	load_arm_reg(&c->e, 0, RN_PSR_FLAGS);
	a64_msr_nzcv(&c->e, 0);
	a64_b_label(&c->e, (int)((u32)cc ^ 1), 0, label);
	return true;
}

static void end_conditional(Arm7Compiler *c, bool *active, a64_label *label)
{
	if (*active)
	{
		a64_label_bind(&c->e, label);
		*active = false;
	}
}

/* The flags an instruction sets go back to the ARM's register file at
 * once. A logical instruction sets N and Z from its result @result, C
 * from the shift or the constant when there is one (w14), and leaves V;
 * an arithmetic one sets all four as the host's NZCV has them. The bits
 * below the flags are kept. */
static void store_logical_flags(Arm7Compiler *c, int result)
{
	a64_emit *e = &c->e;
	load_arm_reg(e, 3, RN_PSR_FLAGS);
	a64_logical(e, false, 3, false, A64_ZR, result, a64_opnd_reg(result, A64_LSL, 0));
	a64_mrs_nzcv(e, 0);
	a64_shift_imm(e, A64_LSR, 0, 0, 30);
	a64_bfi(e, 3, 0, 30, 2);			/* N, Z */
	if (c->set_carry_bit)
		a64_bfi(e, 3, 14, 29, 1);		/* C */
	store_arm_reg(e, 3, RN_PSR_FLAGS);
}

static void store_arith_flags(Arm7Compiler *c)
{
	a64_emit *e = &c->e;
	a64_mrs_nzcv(e, 0);
	load_arm_reg(e, 3, RN_PSR_FLAGS);
	a64_shift_imm(e, A64_LSR, 0, 0, 28);
	a64_bfi(e, 3, 0, 28, 4);			/* N, Z, C, V */
	store_arm_reg(e, 3, RN_PSR_FLAGS);
}

static void emit_data_proc_op(Arm7Compiler *c, const ArmOp *op)
{
	a64_emit *e = &c->e;
	const bool mov_op = op->op_type == ARMOP_MOV || op->op_type == ARMOP_MVN;
	a64_operand arg0 = get_operand(c, &op->arg[0], 1);
	a64_operand arg1 = a64_opnd_imm(0);
	int rd = A64_ZR;
	int rn = A64_ZR;

	if (!mov_op)
		arg1 = get_operand(c, &op->arg[1], 2);
	if (armop_is_reg(&op->rd))
		rd = map_reg(c, armop_reg_of(&op->rd)->armreg);
	if (c->logical_op_set_flags)
	{
		/* When an Operand2 constant is used with the instructions MOVS, MVNS, ANDS, ORRS, ORNS, EORS, BICS, TEQ or TST,
		 * the carry flag is updated to bit[31] of the constant,
		 * if the constant is greater than 255 and can be produced by shifting an 8-bit value.
		 * The constant is the second operand, or the only one of MOV and
		 * MVN. The first operand of the others can be a constant too,
		 * r15 read as a value, and has nothing to do with the carry. */
		const ArmOpOperand *constant = mov_op ? &op->arg[0] : &op->arg[1];
		if (armop_is_imm(constant) && !armop_is_shifted(constant) && armop_get_imm(constant) > 255)
		{
			c->set_carry_bit = true;
			a64_mov_imm(e, false, 14, (armop_get_imm(constant) & 0x80000000) >> 31);
		}
	}
	/* the first operand of the others, in a register */
	if (!mov_op)
		rn = to_register(c, arg0, 1);
	if (op->op_type == ARMOP_ADC || op->op_type == ARMOP_SBC || op->op_type == ARMOP_RSC)
	{
		/* the ARM's carry into the host's: the same bit, the same sense */
		load_arm_reg(e, 0, RN_PSR_FLAGS);
		a64_msr_nzcv(e, 0);
	}

	switch (op->op_type)
	{
	case ARMOP_AND:
		a64_logical(e, false, 0, false, rd, rn, arg1);
		break;
	case ARMOP_ORR:
		a64_logical(e, false, 1, false, rd, rn, arg1);
		break;
	case ARMOP_EOR:
		a64_logical(e, false, 2, false, rd, rn, arg1);
		break;
	case ARMOP_BIC:
		a64_logical(e, false, 0, true, rd, rn, arg1);
		break;
	case ARMOP_TST:
		a64_logical(e, false, 0, false, 0, rn, arg1);
		break;
	case ARMOP_TEQ:
		a64_logical(e, false, 2, false, 0, rn, arg1);
		break;
	case ARMOP_MOV:
		a64_mov_op(e, false, rd, arg0);
		break;
	case ARMOP_MVN:
		if (arg0.is_imm)
			a64_mov_imm(e, false, rd, (u32)~arg0.imm);
		else
			a64_mvn_op(e, false, rd, arg0);
		break;

	case ARMOP_CMP:
		a64_addsub(e, false, true, true, A64_ZR, rn, arg1);
		break;
	case ARMOP_CMN:
		a64_addsub(e, false, false, true, A64_ZR, rn, arg1);
		break;
	case ARMOP_SUB:
		a64_addsub(e, false, true, c->set_flags, rd, rn, arg1);
		break;
	case ARMOP_SBC:
		a64_addsub_carry(e, true, c->set_flags, rd, rn, arg1);
		break;
	case ARMOP_RSB:
		/* rd = op2 - rn, with the flags of that subtraction */
		a64_addsub(e, false, true, c->set_flags, rd, to_register(c, arg1, 2), a64_opnd_reg(rn, A64_LSL, 0));
		break;
	case ARMOP_RSC:
		a64_addsub_carry(e, true, c->set_flags, rd, to_register(c, arg1, 2), a64_opnd_reg(rn, A64_LSL, 0));
		break;
	case ARMOP_ADD:
		a64_addsub(e, false, false, c->set_flags, rd, rn, arg1);
		break;
	case ARMOP_ADC:
		a64_addsub_carry(e, false, c->set_flags, rd, rn, arg1);
		break;
	default:
		die("invalid");
		break;
	}
	if (c->set_flags)
	{
		if (armop_is_logical(op))
			store_logical_flags(c, op->op_type == ARMOP_TST || op->op_type == ARMOP_TEQ ? 0 : rd);
		else
			store_arith_flags(c);
	}
	/* A result that goes to the program counter loses its low two bits:
	 * the ARM takes instructions from whole words only. (A constant put
	 * there is the address of an instruction, and has none.) */
	if (armop_is_reg(&op->rd) && armop_reg_of(&op->rd)->armreg == R15_ARM_NEXT
			&& !(op->op_type == ARMOP_MOV && armop_is_imm(&op->arg[0]) && !armop_is_shifted(&op->arg[0])))
		a64_logical(e, false, 0, false, rd, rd, a64_opnd_imm(0xfffffffc));
}

static void emit_mem_op(Arm7Compiler *c, const ArmOp *op)
{
	a64_emit *e = &c->e;
	a64_label slow, done;
	/* the address in w0, what is stored in w1: the memory handlers' arguments */
	a64_operand base = get_operand(c, &op->arg[0], 0);
	if (base.is_imm)
		a64_mov_imm(e, false, 0, base.imm);
	else if (base.reg != 0)
		a64_mov_op(e, false, 0, a64_opnd_reg(base.reg, A64_LSL, 0));
	if (op->pre_index)
	{
		const ArmOpOperand *offset = &op->arg[1];
		if (armop_is_reg(offset))
			a64_addsub(e, false, !op->add_offset, false, 0, 0, get_operand(c, offset, 2));
		else if (armop_is_imm(offset) && armop_get_imm(offset) != 0)
			a64_addsub(e, false, !op->add_offset, false, 0, 0, a64_opnd_imm(armop_get_imm(offset)));
	}
	if (op->aligned)
		a64_logical(e, false, 0, false, 0, 0, a64_opnd_imm(0xfffffffc));
	if (op->op_type == ARMOP_STR)
	{
		if (armop_is_imm(&op->arg[2]))
			a64_mov_imm(e, false, 1, armop_get_imm(&op->arg[2]));
		else
			a64_mov_op(e, false, 1, a64_opnd_reg(map_reg(c, armop_reg_of(&op->arg[2])->armreg), A64_LSL, 0));
	}

	/* Nearly every access is to the ARM's memory, and that is one load
	 * or store of the host's: it is done here. What is left for the call
	 * is the sound chip's registers, above 0x800000, and a word read
	 * from an address that is not a word's, which comes back rotated. */
	a64_label_init(&slow);
	a64_label_init(&done);
	a64_logical(e, false, 0, false, 2, 0, a64_opnd_imm(0x00ffffff));
	a64_addsub(e, false, true, true, A64_ZR, 2, a64_opnd_imm(0x00800000));
	a64_b_label(e, A64_HS, 0, &slow);
	if (!op->byte_xfer)
	{
		if (op->op_type == ARMOP_LDR)
		{
			a64_logical(e, false, 3, false, A64_ZR, 0, a64_opnd_imm(3));
			a64_b_label(e, A64_NE, 0, &slow);
		}
		else
			a64_logical(e, false, 0, false, 2, 2, a64_opnd_imm(0xfffffffc));	/* a word is stored at a word's address */
	}
	a64_ldst(e, true, false, 4, A64_SP, MASK_SLOT);
	a64_ldst(e, true, true, 3, A64_SP, RAM_SLOT);
	a64_logical(e, false, 0, false, 2, 2, a64_opnd_reg(4, A64_LSL, 0));
	a64_ldst_rr(e, op->op_type == ARMOP_LDR, op->byte_xfer ? 1 : 4, op->op_type == ARMOP_LDR ? 0 : 1, 3, 2, false);
	a64_b_label(e, -1, 0, &done);
	a64_label_bind(e, &slow);
	call(e, arm7_rec_mem_op(op->op_type == ARMOP_LDR, op->byte_xfer));
	a64_label_bind(e, &done);

	if (op->op_type == ARMOP_LDR)
	{
		/* the program counter has no low two bits */
		if (armop_reg_of(&op->rd)->armreg == R15_ARM_NEXT)
			a64_logical(e, false, 0, false, 0, 0, a64_opnd_imm(0xfffffffc));
		a64_mov_op(e, false, map_reg(c, armop_reg_of(&op->rd)->armreg), a64_opnd_reg(0, A64_LSL, 0));
	}
}

static void emit_branch(Arm7Compiler *c, const ArmOp *op)
{
	a64_emit *e = &c->e;
	if (armop_is_imm(&op->arg[0]) && !armop_is_shifted(&op->arg[0]))
		a64_mov_imm(e, false, 0, armop_get_imm(&op->arg[0]));
	else
	{
		int target = to_register(c, get_operand(c, &op->arg[0], 0), 0);
		a64_logical(e, false, 0, false, 0, target, a64_opnd_imm(0xfffffffc));
	}
	store_arm_reg(e, 0, R15_ARM_NEXT);
}

static void emit_msr(Arm7Compiler *c, const ArmOp *op)
{
	a64_emit *e = &c->e;
	if (armop_is_imm(&op->arg[0]))
		a64_mov_imm(e, false, 0, armop_get_imm(&op->arg[0]));
	else
		a64_mov_op(e, false, 0, a64_opnd_reg(map_reg(c, armop_reg_of(&op->arg[0])->armreg), A64_LSL, 0));
	a64_mov_imm(e, false, 1, op->psrMask);
	if (op->spsr)
		call(e, (const void *)(uintptr_t)arm7_msr_spsr);
	else
		call(e, (const void *)(uintptr_t)arm7_msr_cpsr);
}

static void emit_mrs(Arm7Compiler *c, const ArmOp *op)
{
	a64_emit *e = &c->e;
	call(e, (const void *)(uintptr_t)CPUUpdateCPSR);
	load_arm_reg(e, map_reg(c, armop_reg_of(&op->rd)->armreg), op->spsr ? RN_SPSR : RN_CPSR);
}

static void emit_fallback(Arm7Compiler *c, const ArmOp *op)
{
	c->set_flags = false;
	a64_mov_imm(&c->e, false, 0, armop_get_imm(&op->arg[0]));
	call(&c->e, (const void *)(uintptr_t)arm7_interpret);
}

/* On to the next block, as the x86-64 backend does it: the block's
 * cycles come off here, and the shared dispatcher takes over when the
 * time slice is used up (none left or less) or an interrupt is waiting;
 * otherwise the block jumps to the next itself, through the slot of the
 * address it goes to when that is fixed. */
static void emit_dispatch(Arm7Compiler *c, const ArmOp *block_ops, u32 count, u32 cycles)
{
	a64_emit *e = &c->e;
	const ArmOp *last = count == 0 ? NULL : &block_ops[count - 1];
	a64_label to_dispatch;

	a64_label_init(&to_dispatch);
	load_arm_reg(e, 0, CYCL_CNT);
	a64_addsub(e, false, true, true, 0, 0, a64_opnd_imm(cycles));
	store_arm_reg(e, 0, CYCL_CNT);
	a64_b_label(e, A64_LE, 0, &to_dispatch);
	load_arm_reg(e, 1, INTR_PEND);
	a64_b_label(e, -2, 1, &to_dispatch);

	if (last != NULL && last->condition == ARMCC_AL && armop_is_imm(&last->arg[0]) && !armop_is_shifted(&last->arg[0])
			&& ((last->op_type == ARMOP_B || last->op_type == ARMOP_BL)
				|| (last->op_type == ARMOP_MOV && armop_is_reg(&last->rd) && armop_reg_of(&last->rd)->armreg == R15_ARM_NEXT)))
	{
		const u32 target = armop_get_imm(&last->arg[0]);
		a64_ldst(e, true, true, 3, 26, (s64)((target & (ARAM_SIZE_MAX - 1)) / 4) * 8);
	}
	else
	{
		load_arm_reg(e, 0, R15_ARM_NEXT);
		a64_ubfx(e, 2, 0, 2, 21);					/* pc & 0x7ffffc, in entries */
		a64_ldst_rr(e, true, 8, 3, 26, 2, true);
	}
	a64_br(e, 3);

	a64_label_bind(e, &to_dispatch);
	jump_to_dispatch(e);
}

static void finish(a64_emit *e)
{
	verify(!e->full);
	vmem_platform_flush_cache(
			arm7_write_to_exec(e->start), arm7_write_to_exec(e->p),
			e->start, e->p);
	arm7_advance((u32)((u8 *)e->p - (u8 *)e->start));
}

void arm7backend_compile(const ArmOp *block_ops, u32 count, u32 cycles)
{
	Arm7Compiler compiler;
	Arm7Compiler *c = &compiler;
	a64_emit *e = &c->e;
	ArmCondition currentCondition = ARMCC_AL;
	a64_label condLabel;
	bool condActive = false;
	u32 i;

	vmem_jit_write_begin();
	a64_init(e, arm7_current_code(), arm7_space_left());
	c->logical_op_set_flags = false;
	c->set_carry_bit = false;
	c->set_flags = false;
	arm_regalloc_init(&c->regalloc, MAX_REGS, block_ops, count, load_reg, store_reg, c);

	for (i = 0; i < count; i++)
	{
		const ArmOp *op = &block_ops[i];

		DEBUG_LOG(AICA_ARM, "-> op %d cc %d\n", (int)op->op_type, (int)op->condition);

		c->set_flags = (op->flags & ARMOP_SETS_FLAGS) != 0;
		c->logical_op_set_flags = armop_is_logical(op) && c->set_flags;
		c->set_carry_bit = false;

		if (op->op_type == ARMOP_FALLBACK)
		{
			end_conditional(c, &condActive, &condLabel);
			currentCondition = ARMCC_AL;
		}
		else if (op->condition != currentCondition)
		{
			end_conditional(c, &condActive, &condLabel);
			currentCondition = op->condition;
			if (op->condition != ARMCC_AL)
				arm_regalloc_store_dirty(&c->regalloc);
			condActive = start_conditional(c, op->condition, &condLabel);
		}

		arm_regalloc_load(&c->regalloc, i);

		if (op->op_type <= ARMOP_MVN)
			/* data processing op */
			emit_data_proc_op(c, op);
		else if (op->op_type <= ARMOP_STR)
			/* memory load/store */
			emit_mem_op(c, op);
		else if (op->op_type <= ARMOP_BL)
			/* branch */
			emit_branch(c, op);
		else if (op->op_type == ARMOP_MRS)
			emit_mrs(c, op);
		else if (op->op_type == ARMOP_MSR)
			emit_msr(c, op);
		else if (op->op_type == ARMOP_FALLBACK)
			emit_fallback(c, op);
		else
			die("invalid");

		arm_regalloc_store(&c->regalloc, i);

		/* The next instruction's condition is for the flags as this one
		 * leaves them, and writing the status register changes them as
		 * surely as an instruction that sets them. */
		if (c->set_flags || op->op_type == ARMOP_MSR)
		{
			currentCondition = ARMCC_AL;
			end_conditional(c, &condActive, &condLabel);
		}
	}
	end_conditional(c, &condActive, &condLabel);

	emit_dispatch(c, block_ops, count, cycles);

	finish(e);
	vmem_jit_write_end();
}

void arm7backend_flush(void)
{
	a64_emit em;
	a64_emit *e = &em;
	a64_label arm_dispatch_label, arm_dofiq, arm_exit;

	if (!arm7_code_empty())
	{
		verify(arm_mainloop != NULL);
		verify(arm_compilecode != NULL);
		return;
	}
	vmem_jit_write_begin();
	a64_init(e, arm7_current_code(), arm7_space_left());
	a64_label_init(&arm_dispatch_label);
	a64_label_init(&arm_dofiq);
	a64_label_init(&arm_exit);

	/* arm_compilecode: */
	arm_compilecode = (void (*)(void))exec_cursor(e);
	call(e, (const void *)(uintptr_t)arm7_rec_compile);
	a64_b_label(e, -1, 0, &arm_dispatch_label);

	/* arm_mainloop(regs, entry points) */
	arm_mainloop = (arm_mainloop_t)exec_cursor(e);
	a64_ldstp(e, false, A64_PRE, 19, 20, A64_SP, -FRAME_SIZE);
	a64_ldstp(e, false, A64_OFFSET, 21, 22, A64_SP, 16);
	a64_ldstp(e, false, A64_OFFSET, 23, 24, A64_SP, 32);
	a64_ldstp(e, false, A64_OFFSET, 25, 26, A64_SP, 48);
	a64_ldstp(e, false, A64_OFFSET, 27, 28, A64_SP, 64);
	a64_ldstp(e, false, A64_OFFSET, 29, 30, A64_SP, 80);

	a64_mov_rr(e, true, 28, 0);		/* arm7 registers */
	a64_mov_rr(e, true, 26, 1);		/* lookup base */
	/* the sound memory, for the loads and stores done in line */
	a64_mov_imm(e, true, 9, (u64)(uintptr_t)&aica_ram.data);
	a64_ldst(e, true, true, 9, 9, 0);
	a64_ldst(e, false, true, 9, A64_SP, RAM_SLOT);
	a64_mov_imm(e, true, 9, (u64)(uintptr_t)&ARAM_MASK);
	a64_ldst(e, true, false, 9, 9, 0);
	a64_ldst(e, false, false, 9, A64_SP, MASK_SLOT);

	/* arm_dispatch: */
	a64_label_bind(e, &arm_dispatch_label);
	arm_dispatch = (void (*)(void))exec_cursor(e);
	load_arm_reg(e, 3, CYCL_CNT);
	a64_ldstp_w(e, true, 0, 1, 28, (int)arm_reg_offset(R15_ARM_NEXT));	/* load Next PC, interrupt */
	a64_addsub(e, false, true, true, A64_ZR, 3, a64_opnd_imm(0));
	a64_b_label(e, A64_LE, 0, &arm_exit);		/* timeslice is over */
	a64_b_label(e, -2, 1, &arm_dofiq);			/* if interrupt pending, handle it */

	a64_ubfx(e, 2, 0, 2, 21);					/* pc & 0x7ffffc, in entries */
	a64_ldst_rr(e, true, 8, 3, 26, 2, true);
	a64_br(e, 3);

	/* arm_dofiq: */
	a64_label_bind(e, &arm_dofiq);
	call(e, (const void *)(uintptr_t)CPUFiq);
	a64_b_label(e, -1, 0, &arm_dispatch_label);

	/* arm_exit: */
	a64_label_bind(e, &arm_exit);
	a64_ldstp(e, true, A64_OFFSET, 29, 30, A64_SP, 80);
	a64_ldstp(e, true, A64_OFFSET, 27, 28, A64_SP, 64);
	a64_ldstp(e, true, A64_OFFSET, 25, 26, A64_SP, 48);
	a64_ldstp(e, true, A64_OFFSET, 23, 24, A64_SP, 32);
	a64_ldstp(e, true, A64_OFFSET, 21, 22, A64_SP, 16);
	a64_ldstp(e, true, A64_POST, 19, 20, A64_SP, FRAME_SIZE);
	a64_ret(e);

	finish(e);
	vmem_jit_write_end();
}

#else
/* (for the other processors: C has no empty file) */
typedef int arm7_rec_arm64_not_here;
#endif
