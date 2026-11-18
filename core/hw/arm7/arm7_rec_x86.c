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

/* The sound processor's recompiler for 32-bit x86.
 *
 * It is arm7_rec_x64.c made for a processor with eight registers, and
 * does what that one does for every instruction, in the same order and
 * with the same results, flags and memory accesses: the blocks, their
 * cycles and the register allocation are arm7_rec.c's, shared by every
 * backend. What differs is what the processor has:
 *
 * - four registers for the ARM's (ebx, ebp, esi, edi, which the C code
 *   called keeps), and eax, ecx and edx for everything else. The two
 *   operands of an instruction are taken into edx and eax, and the
 *   shifter's carry, where it is wanted, into ecx. Where x86-64 had a
 *   spare register for a value to use in place of a shift's, the value is
 *   in memory (cmov takes it from there), and a rotate through the carry
 *   (RRX) is the host's own rcr, with the ARM's carry put in the host's
 *   by bt;
 * - the emulator's data reached at its address, which 32 bits can always
 *   give, where x86-64 reaches it relative to the instruction;
 * - calls made fastcall, the first two arguments in ecx and edx, as every
 *   handler called from here is declared (DYNACALL), with the stack a
 *   multiple of 16 at each one, as Linux and Android want;
 * - four bytes to an entry point.
 *
 * Everything blocks jump to and call that is not C - the main loop, the
 * routine that has a block compiled - is written into the code buffer by
 * arm7backend_flush(), so nothing here is assembly linked into the
 * library, which keeps it free of relocations in its code (Android refuses
 * a library that has them). */

#include "build.h"

#if	HOST_CPU == CPU_X86 && FEAT_AREC != DYNAREC_NONE

#include "jit/x86_emit.h"
#include "arm7_rec.h"
#include "hw/aica/aica_if.h"

static void (*arm_dispatch)(void);

/* fastcall */
static const int call_regs[] = { X86_ECX, X86_EDX };
static void (**entry_points)(void);

/* What a shift by a register gives in place of its own result for a count
 * of 32 or more: 0 for the logical shifts, and the count an arithmetic
 * shift by 31 is the same as. */
static const u32 shift_zero = 0;
static const u32 shift_asr_max = 31;

#define ALLOC_REGS 4
static const int alloc_regs[ALLOC_REGS] = {
		X86_EBX, X86_EBP, X86_ESI, X86_EDI
};

#define N_FLAG (1u << 31)
#define Z_FLAG (1u << 30)
#define C_FLAG (1u << 29)
#define V_FLAG (1u << 28)

/* (none: a plain constant, left to the caller) */
#define NO_REG (-1)

typedef struct Arm7Compiler
{
	x86_emit e;
	bool logical_op_set_flags;
	bool set_carry_bit;
	bool set_flags;
	ArmRegAlloc regalloc;
} Arm7Compiler;

static x86_mem arm_reg(Arm7Reg r)
{
	return x86_abs(&arm_Reg[r].I);
}

static int get_reg32(int i)
{
	verify(i >= 0 && i < ALLOC_REGS);
	return alloc_regs[i];
}

static int map_reg(Arm7Compiler *c, Arm7Reg r)
{
	return get_reg32(arm_regalloc_map(&c->regalloc, r));
}

static void load_reg(void *ctx, int host_reg, Arm7Reg armreg)
{
	Arm7Compiler *c = (Arm7Compiler *)ctx;
	x86_load(&c->e, get_reg32(host_reg), arm_reg(armreg));
}

static void store_reg(void *ctx, int host_reg, Arm7Reg armreg)
{
	Arm7Compiler *c = (Arm7Compiler *)ctx;
	x86_store(&c->e, arm_reg(armreg), get_reg32(host_reg));
}

/* The value of an operand: a register, or NO_REG for a plain constant.
 * The shifter's carry goes in ecx (bit 0, the rest 0, or bit 0 of cl
 * with the rest of ecx anything above bit 7: save_flags() only keeps
 * bit 0's place). */
static int get_operand(Arm7Compiler *c, const ArmOpOperand *arg, int scratch_reg)
{
	x86_emit *e = &c->e;
	int r;
	if (!armop_is_reg(arg))
	{
		/* A plain constant is left to the caller. One that still has
		 * a shift to go through is r15 read as a value, with a shift
		 * the decoder could not work out ahead. */
		if (armop_is_none(arg) || !armop_is_shifted(arg))
			return NO_REG;
		x86_mov_ri(e, scratch_reg, armop_get_imm(arg));
		r = scratch_reg;
	}
	else
		r = map_reg(c, armop_reg_of(arg)->armreg);
	if (armop_is_shifted(arg))
	{
		if (r != scratch_reg)
		{
			x86_mov_rr(e, scratch_reg, r);
			r = scratch_reg;
		}
		if (arg->shift_imm)
		{
			const u32 shift_value = arg->s.shift_value;
			/* shift by immediate */
			if (arg->shift_type != ARMSH_ROR && shift_value != 0 && !c->logical_op_set_flags)
			{
				switch (arg->shift_type)
				{
				case ARMSH_LSL:
					x86_shift_ri(e, X86_SHL, r, shift_value);
					break;
				case ARMSH_LSR:
					x86_shift_ri(e, X86_SHR, r, shift_value);
					break;
				case ARMSH_ASR:
					x86_shift_ri(e, X86_SAR, r, shift_value);
					break;
				default:
					die("invalid");
					break;
				}
			}
			else if (shift_value == 0)
			{
				/* Shift by 32 */
				if (c->logical_op_set_flags)
					c->set_carry_bit = true;
				if (arg->shift_type == ARMSH_LSR)
				{
					if (c->set_carry_bit)
					{
						x86_mov_rr(e, X86_ECX, r);			/* ecx = rm[31] */
						x86_shift_ri(e, X86_SHR, X86_ECX, 31);
					}
					x86_mov_ri(e, r, 0);					/* r = 0 */
				}
				else if (arg->shift_type == ARMSH_ASR)
				{
					if (c->set_carry_bit)
					{
						x86_mov_rr(e, X86_ECX, r);			/* ecx = rm[31] */
						x86_shift_ri(e, X86_SHR, X86_ECX, 31);
					}
					x86_shift_ri(e, X86_SAR, r, 31);		/* r = rm < 0 ? -1 : 0 */
				}
				else if (arg->shift_type == ARMSH_ROR)
				{
					/* RRX: the ARM's carry into the top, bit 0 out to the host's carry */
					x86_bt_mi(e, arm_reg(RN_PSR_FLAGS), 29);
					x86_shift_ri(e, X86_RCR, r, 1);
					if (c->set_carry_bit)
						x86_setcc(e, X86_CC_B, X86_ECX);	/* cl = rm[0] (new C) */
				}
				else
					die("Invalid shift");
			}
			else
			{
				/* Carry must be preserved or Ror shift */
				if (c->logical_op_set_flags)
					c->set_carry_bit = true;
				if (arg->shift_type == ARMSH_LSL)
				{
					if (c->set_carry_bit)
					{
						x86_mov_rr(e, X86_ECX, r);
						x86_shift_ri(e, X86_SHR, X86_ECX, 32 - shift_value);
					}
					x86_shift_ri(e, X86_SHL, r, shift_value);	/* r <<= shift */
					if (c->set_carry_bit)
						x86_alu_ri(e, false, X86_AND, X86_ECX, 1);	/* ecx = rm[lsb] */
				}
				else
				{
					if (c->set_carry_bit)
					{
						x86_mov_rr(e, X86_ECX, r);
						x86_shift_ri(e, X86_SHR, X86_ECX, shift_value - 1);
						x86_alu_ri(e, false, X86_AND, X86_ECX, 1);	/* ecx = rm[msb] */
					}

					if (arg->shift_type == ARMSH_LSR)
						x86_shift_ri(e, X86_SHR, r, shift_value);	/* r >>= shift */
					else if (arg->shift_type == ARMSH_ASR)
						x86_shift_ri(e, X86_SAR, r, shift_value);
					else if (arg->shift_type == ARMSH_ROR)
						x86_shift_ri(e, X86_ROR, r, shift_value);
					else
						die("Invalid shift");
				}
			}
		}
		else
		{
			/* shift by register */
			const int shift_reg = map_reg(c, arg->s.shift_reg.armreg);
			/* The count is the low byte of the register, whatever the
			 * rest of it holds. (esi, edi and ebp have no byte register
			 * of their own here: the whole register is taken first.) */
			switch (arg->shift_type)
			{
			case ARMSH_LSL:
			case ARMSH_LSR:
				x86_mov_rr(e, X86_ECX, shift_reg);
				x86_movzx8r(e, X86_ECX, X86_ECX);
				x86_shift_cl(e, arg->shift_type == ARMSH_LSL ? X86_SHL : X86_SHR, r);
				x86_alu_ri(e, false, X86_CMP, X86_ECX, 32);
				x86_cmov_m(e, X86_CC_NB, r, x86_abs(&shift_zero));	/* LSL and LSR by 32 or more gives 0 */
				break;
			case ARMSH_ASR:
				x86_mov_rr(e, X86_ECX, shift_reg);
				x86_movzx8r(e, X86_ECX, X86_ECX);
				x86_alu_ri(e, false, X86_CMP, X86_ECX, 32);
				x86_cmov_m(e, X86_CC_NB, X86_ECX, x86_abs(&shift_asr_max));	/* ASR by 32 or more gives 0 or -1 depending on operand sign, as by 31 */
				x86_shift_cl(e, X86_SAR, r);
				break;
			case ARMSH_ROR:
				x86_mov_rr(e, X86_ECX, shift_reg);
				x86_shift_cl(e, X86_ROR, r);
				break;
			default:
				die("Invalid shift");
				break;
			}
		}
	}
	return r;
}

/* The code that skips what follows unless @cc holds; false for none */
static bool start_conditional(Arm7Compiler *c, ArmCondition cc, x86_label *label)
{
	x86_emit *e = &c->e;
	if (cc == ARMCC_AL)
		return false;
	x86_label_init(label);
	cc = (ArmCondition)((u32)cc ^ 1);	/* invert the condition */
	x86_load(e, X86_EAX, arm_reg(RN_PSR_FLAGS));
	switch (cc)
	{
	case ARMCC_EQ:	/* Z==1 */
		x86_alu_ri(e, false, X86_AND, X86_EAX, Z_FLAG);
		x86_jmp_label(e, X86_CC_NE, label, true);
		break;
	case ARMCC_NE:	/* Z==0 */
		x86_alu_ri(e, false, X86_AND, X86_EAX, Z_FLAG);
		x86_jmp_label(e, X86_CC_E, label, true);
		break;
	case ARMCC_CS:	/* C==1 */
		x86_alu_ri(e, false, X86_AND, X86_EAX, C_FLAG);
		x86_jmp_label(e, X86_CC_NE, label, true);
		break;
	case ARMCC_CC:	/* C==0 */
		x86_alu_ri(e, false, X86_AND, X86_EAX, C_FLAG);
		x86_jmp_label(e, X86_CC_E, label, true);
		break;
	case ARMCC_MI:	/* N==1 */
		x86_alu_ri(e, false, X86_AND, X86_EAX, N_FLAG);
		x86_jmp_label(e, X86_CC_NE, label, true);
		break;
	case ARMCC_PL:	/* N==0 */
		x86_alu_ri(e, false, X86_AND, X86_EAX, N_FLAG);
		x86_jmp_label(e, X86_CC_E, label, true);
		break;
	case ARMCC_VS:	/* V==1 */
		x86_alu_ri(e, false, X86_AND, X86_EAX, V_FLAG);
		x86_jmp_label(e, X86_CC_NE, label, true);
		break;
	case ARMCC_VC:	/* V==0 */
		x86_alu_ri(e, false, X86_AND, X86_EAX, V_FLAG);
		x86_jmp_label(e, X86_CC_E, label, true);
		break;
	case ARMCC_HI:	/* (C==1) && (Z==0) */
		x86_alu_ri(e, false, X86_AND, X86_EAX, C_FLAG | Z_FLAG);
		x86_alu_ri(e, false, X86_CMP, X86_EAX, C_FLAG);
		x86_jmp_label(e, X86_CC_E, label, true);
		break;
	case ARMCC_LS:	/* (C==0) || (Z==1) */
		x86_alu_ri(e, false, X86_AND, X86_EAX, C_FLAG | Z_FLAG);
		x86_alu_ri(e, false, X86_CMP, X86_EAX, C_FLAG);
		x86_jmp_label(e, X86_CC_NE, label, true);
		break;
	case ARMCC_GE:	/* N==V */
		x86_mov_rr(e, X86_ECX, X86_EAX);
		x86_shift_ri(e, X86_SHL, X86_ECX, 3);
		x86_alu_rr(e, X86_XOR, X86_EAX, X86_ECX);
		x86_alu_ri(e, false, X86_AND, X86_EAX, N_FLAG);
		x86_jmp_label(e, X86_CC_E, label, true);
		break;
	case ARMCC_LT:	/* N!=V */
		x86_mov_rr(e, X86_ECX, X86_EAX);
		x86_shift_ri(e, X86_SHL, X86_ECX, 3);
		x86_alu_rr(e, X86_XOR, X86_EAX, X86_ECX);
		x86_alu_ri(e, false, X86_AND, X86_EAX, N_FLAG);
		x86_jmp_label(e, X86_CC_NE, label, true);
		break;
	case ARMCC_GT:	/* (Z==0) && (N==V) */
		x86_mov_rr(e, X86_ECX, X86_EAX);
		x86_mov_rr(e, X86_EDX, X86_EAX);
		x86_shift_ri(e, X86_SHL, X86_ECX, 3);
		x86_shift_ri(e, X86_SHL, X86_EDX, 1);
		x86_alu_rr(e, X86_XOR, X86_EAX, X86_ECX);
		x86_alu_rr(e, X86_OR, X86_EAX, X86_EDX);
		x86_alu_ri(e, false, X86_AND, X86_EAX, N_FLAG);
		x86_jmp_label(e, X86_CC_E, label, true);
		break;
	case ARMCC_LE:	/* (Z==1) || (N!=V) */
		x86_mov_rr(e, X86_ECX, X86_EAX);
		x86_mov_rr(e, X86_EDX, X86_EAX);
		x86_shift_ri(e, X86_SHL, X86_ECX, 3);
		x86_shift_ri(e, X86_SHL, X86_EDX, 1);
		x86_alu_rr(e, X86_XOR, X86_EAX, X86_ECX);
		x86_alu_rr(e, X86_OR, X86_EAX, X86_EDX);
		x86_alu_ri(e, false, X86_AND, X86_EAX, N_FLAG);
		x86_jmp_label(e, X86_CC_NE, label, true);
		break;
	default:
		die("Invalid condition code");
		break;
	}
	return true;
}

static void end_conditional(Arm7Compiler *c, bool *active, x86_label *label)
{
	if (*active)
	{
		x86_label_bind(&c->e, label);
		*active = false;
	}
}

static bool emit_data_proc_op(Arm7Compiler *c, const ArmOp *op)
{
	x86_emit *e = &c->e;
	bool save_v_flag = true;
	int arg0 = get_operand(c, &op->arg[0], X86_EDX);
	int arg1 = get_operand(c, &op->arg[1], X86_EAX);
	int rd = NO_REG;

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
		const ArmOpOperand *constant = (op->op_type == ARMOP_MOV || op->op_type == ARMOP_MVN) ? &op->arg[0] : &op->arg[1];
		if (armop_is_imm(constant) && !armop_is_shifted(constant) && armop_get_imm(constant) > 255)
		{
			c->set_carry_bit = true;
			x86_mov_ri(e, X86_ECX, (armop_get_imm(constant) & 0x80000000) >> 31);
		}
	}

	/* Every instruction here but MOV and MVN has two operands, and they
	 * are taken by value, into edx and eax, before anything is written:
	 * the result may be going to the register one of them came from. */
	if (op->op_type != ARMOP_MOV && op->op_type != ARMOP_MVN)
	{
		if (arg0 == NO_REG)
			x86_mov_ri(e, X86_EDX, armop_get_imm(&op->arg[0]));
		else if (arg0 != X86_EDX)
			x86_mov_rr(e, X86_EDX, arg0);
		if (arg1 == NO_REG)
			x86_mov_ri(e, X86_EAX, armop_get_imm(&op->arg[1]));
		else if (arg1 != X86_EAX)
			x86_mov_rr(e, X86_EAX, arg1);
	}
	if (op->op_type == ARMOP_ADC || op->op_type == ARMOP_SBC || op->op_type == ARMOP_RSC)
	{
		/* the ARM's carry into the host's */
		x86_bt_mi(e, arm_reg(RN_PSR_FLAGS), 29);
		if (op->op_type != ARMOP_ADC)
			x86_cmc(e);		/* subtracting, the ARM's carry is "no borrow" */
	}

	switch (op->op_type)
	{
	case ARMOP_AND:
		x86_alu_rr(e, X86_AND, X86_EDX, X86_EAX);
		x86_mov_rr(e, rd, X86_EDX);
		save_v_flag = false;
		break;
	case ARMOP_ORR:
		x86_alu_rr(e, X86_OR, X86_EDX, X86_EAX);
		x86_mov_rr(e, rd, X86_EDX);
		save_v_flag = false;
		break;
	case ARMOP_EOR:
		x86_alu_rr(e, X86_XOR, X86_EDX, X86_EAX);
		x86_mov_rr(e, rd, X86_EDX);
		save_v_flag = false;
		break;
	case ARMOP_BIC:
		x86_not(e, X86_EAX);
		x86_alu_rr(e, X86_AND, X86_EDX, X86_EAX);
		x86_mov_rr(e, rd, X86_EDX);
		save_v_flag = false;
		break;

	case ARMOP_TST:
		x86_test_rr(e, X86_EDX, X86_EAX);
		save_v_flag = false;
		break;
	case ARMOP_TEQ:
		x86_alu_rr(e, X86_XOR, X86_EDX, X86_EAX);
		save_v_flag = false;
		break;
	case ARMOP_CMP:
		x86_alu_rr(e, X86_CMP, X86_EDX, X86_EAX);
		if (c->set_flags)
		{
			x86_setcc(e, X86_CC_NB, X86_ECX);
			c->set_carry_bit = true;
		}
		break;
	case ARMOP_CMN:
		x86_alu_rr(e, X86_ADD, X86_EDX, X86_EAX);
		if (c->set_flags)
		{
			x86_setcc(e, X86_CC_B, X86_ECX);
			c->set_carry_bit = true;
		}
		break;

	case ARMOP_MOV:
		if (arg0 == NO_REG)
			x86_mov_ri(e, rd, armop_get_imm(&op->arg[0]));
		else if (arg0 != rd)
			x86_mov_rr(e, rd, arg0);
		if (c->set_flags)
		{
			x86_test_rr(e, rd, rd);
			save_v_flag = false;
		}
		break;
	case ARMOP_MVN:
		if (arg0 == NO_REG)
			x86_mov_ri(e, rd, ~armop_get_imm(&op->arg[0]));
		else
		{
			if (arg0 != rd)
				x86_mov_rr(e, rd, arg0);
			x86_not(e, rd);
		}
		if (c->set_flags)
		{
			x86_test_rr(e, rd, rd);
			save_v_flag = false;
		}
		break;

	case ARMOP_SUB:
	case ARMOP_SBC:
		/* rd = rn - op2 (- !C) */
		x86_alu_rr(e, op->op_type == ARMOP_SUB ? X86_SUB : X86_SBB, X86_EDX, X86_EAX);
		if (c->set_flags)
		{
			x86_setcc(e, X86_CC_NB, X86_ECX);
			c->set_carry_bit = true;
		}
		x86_mov_rr(e, rd, X86_EDX);
		break;
	case ARMOP_RSB:
	case ARMOP_RSC:
		/* rd = op2 - rn (- !C) */
		x86_alu_rr(e, op->op_type == ARMOP_RSB ? X86_SUB : X86_SBB, X86_EAX, X86_EDX);
		if (c->set_flags)
		{
			x86_setcc(e, X86_CC_NB, X86_ECX);
			c->set_carry_bit = true;
		}
		x86_mov_rr(e, rd, X86_EAX);
		break;
	case ARMOP_ADD:
	case ARMOP_ADC:
		x86_alu_rr(e, op->op_type == ARMOP_ADD ? X86_ADD : X86_ADC, X86_EDX, X86_EAX);
		if (c->set_flags)
		{
			x86_setcc(e, X86_CC_B, X86_ECX);
			c->set_carry_bit = true;
		}
		x86_mov_rr(e, rd, X86_EDX);
		break;
	default:
		die("invalid");
		break;
	}
	/* A result that goes to the program counter loses its low two bits:
	 * the ARM takes instructions from whole words only. (A constant put
	 * there is the address of an instruction, and has none. None of
	 * these sets the flags, so the host's are free.) */
	if (armop_is_reg(&op->rd) && armop_reg_of(&op->rd)->armreg == R15_ARM_NEXT
			&& !(op->op_type == ARMOP_MOV && arg0 == NO_REG))
		x86_alu_ri(e, false, X86_AND, rd, 0xfffffffc);

	return save_v_flag;
}

static void emit_mem_op(Arm7Compiler *c, const ArmOp *op)
{
	x86_emit *e = &c->e;
	x86_label slow, done;
	int addr_reg = get_operand(c, &op->arg[0], call_regs[0]);

	if (addr_reg != call_regs[0])
	{
		if (addr_reg == NO_REG)
			x86_mov_ri(e, call_regs[0], armop_get_imm(&op->arg[0]));
		else
			x86_mov_rr(e, call_regs[0], addr_reg);
		addr_reg = call_regs[0];
	}
	if (op->pre_index)
	{
		const ArmOpOperand *offset = &op->arg[1];
		int offset_reg = get_operand(c, offset, X86_EAX);
		if (offset_reg != NO_REG)
			x86_alu_rr(e, op->add_offset ? X86_ADD : X86_SUB, addr_reg, offset_reg);
		else if (armop_is_imm(offset) && armop_get_imm(offset) != 0)
			x86_alu_ri(e, false, op->add_offset ? X86_ADD : X86_SUB, addr_reg, armop_get_imm(offset));
	}
	if (op->aligned)
		x86_alu_ri(e, false, X86_AND, addr_reg, 0xfffffffc);
	if (op->op_type == ARMOP_STR)
	{
		if (armop_is_imm(&op->arg[2]))
			x86_mov_ri(e, call_regs[1], armop_get_imm(&op->arg[2]));
		else
			x86_mov_rr(e, call_regs[1], map_reg(c, armop_reg_of(&op->arg[2])->armreg));
	}

	/* Nearly every access is to the ARM's memory, and that is one move
	 * to or from the host's: it is done here. What is left for the call
	 * is the sound chip's registers, above 0x800000, and a word read
	 * from an address that is not a word's, which comes back rotated. */
	x86_label_init(&slow);
	x86_label_init(&done);
	x86_mov_rr(e, X86_EAX, addr_reg);
	x86_alu_ri(e, false, X86_AND, X86_EAX, 0x00ffffff);
	x86_alu_ri(e, false, X86_CMP, X86_EAX, 0x00800000);
	x86_jmp_label(e, X86_CC_NB, &slow, false);
	if (!op->byte_xfer)
	{
		if (op->op_type == ARMOP_LDR)
		{
			x86_test_ri(e, X86_EAX, 3);
			x86_jmp_label(e, X86_CC_NE, &slow, false);
		}
		else
			x86_alu_ri(e, false, X86_AND, X86_EAX, 0xfffffffc);		/* a word is stored at a word's address */
	}
	x86_alu_rm(e, X86_AND, X86_EAX, x86_abs(&ARAM_MASK));
	x86_alu_rm(e, X86_ADD, X86_EAX, x86_abs(&aica_ram.data));
	if (op->op_type == ARMOP_LDR)
	{
		if (op->byte_xfer)
			x86_movzx8m(e, X86_EAX, x86_m(X86_EAX, 0));
		else
			x86_load(e, X86_EAX, x86_m(X86_EAX, 0));
	}
	else
	{
		if (op->byte_xfer)
			x86_store8(e, x86_m(X86_EAX, 0), X86_EDX);
		else
			x86_store(e, x86_m(X86_EAX, 0), call_regs[1]);
	}
	x86_jmp_label(e, -1, &done, false);
	x86_label_bind(e, &slow);
	x86_call(e, (uintptr_t)arm7_rec_mem_op(op->op_type == ARMOP_LDR, op->byte_xfer));
	x86_label_bind(e, &done);

	if (op->op_type == ARMOP_LDR)
	{
		/* the program counter has no low two bits */
		if (armop_reg_of(&op->rd)->armreg == R15_ARM_NEXT)
			x86_alu_ri(e, false, X86_AND, X86_EAX, 0xfffffffc);
		x86_mov_rr(e, map_reg(c, armop_reg_of(&op->rd)->armreg), X86_EAX);
	}
}

static void save_flags(Arm7Compiler *c, bool save_v_flag)
{
	x86_emit *e = &c->e;
	if (!c->set_flags)
		return;

	x86_pushf(e);
	x86_pop(e, X86_EAX);

	if (save_v_flag)
	{
		x86_mov_rr(e, X86_EDX, X86_EAX);
		x86_shift_ri(e, X86_SHL, X86_EDX, 28 - 11);		/* V */
	}
	x86_shift_ri(e, X86_SHL, X86_EAX, 30 - 6);			/* Z,N */
	if (save_v_flag)
		x86_alu_ri(e, false, X86_AND, X86_EDX, V_FLAG);	/* V */
	x86_alu_ri(e, false, X86_AND, X86_EAX, Z_FLAG | N_FLAG);	/* Z,N */
	if (save_v_flag)
		x86_alu_rr(e, X86_OR, X86_EAX, X86_EDX);

	x86_load(e, X86_EDX, arm_reg(RN_PSR_FLAGS));
	if (c->set_carry_bit)
	{
		if (save_v_flag)
			x86_alu_ri(e, false, X86_AND, X86_EDX, ~(Z_FLAG | N_FLAG | C_FLAG | V_FLAG));
		else
			x86_alu_ri(e, false, X86_AND, X86_EDX, ~(Z_FLAG | N_FLAG | C_FLAG));
		x86_shift_ri(e, X86_SHL, X86_ECX, 29);
		x86_alu_rr(e, X86_OR, X86_EDX, X86_ECX);
	}
	else
	{
		if (save_v_flag)
			x86_alu_ri(e, false, X86_AND, X86_EDX, ~(Z_FLAG | N_FLAG | V_FLAG));
		else
			x86_alu_ri(e, false, X86_AND, X86_EDX, ~(Z_FLAG | N_FLAG));
	}
	x86_alu_rr(e, X86_OR, X86_EDX, X86_EAX);
	x86_store(e, arm_reg(RN_PSR_FLAGS), X86_EDX);
}

static void emit_branch(Arm7Compiler *c, const ArmOp *op)
{
	x86_emit *e = &c->e;
	int addr_reg = get_operand(c, &op->arg[0], X86_EAX);
	if (addr_reg == NO_REG)
		x86_mov_ri(e, X86_EAX, armop_get_imm(&op->arg[0]));
	else
	{
		if (addr_reg != X86_EAX)
			x86_mov_rr(e, X86_EAX, addr_reg);
		x86_alu_ri(e, false, X86_AND, X86_EAX, 0xfffffffc);
	}
	x86_store(e, arm_reg(R15_ARM_NEXT), X86_EAX);
}

static void emit_msr(Arm7Compiler *c, const ArmOp *op)
{
	x86_emit *e = &c->e;
	if (armop_is_imm(&op->arg[0]))
		x86_mov_ri(e, call_regs[0], armop_get_imm(&op->arg[0]));
	else
		x86_mov_rr(e, call_regs[0], map_reg(c, armop_reg_of(&op->arg[0])->armreg));
	x86_mov_ri(e, call_regs[1], op->psrMask);
	if (op->spsr)
		x86_call(e, (uintptr_t)arm7_msr_spsr);
	else
		x86_call(e, (uintptr_t)arm7_msr_cpsr);
}

static void emit_mrs(Arm7Compiler *c, const ArmOp *op)
{
	x86_emit *e = &c->e;
	x86_call(e, (uintptr_t)CPUUpdateCPSR);

	if (op->spsr)
		x86_load(e, map_reg(c, armop_reg_of(&op->rd)->armreg), arm_reg(RN_SPSR));
	else
		x86_load(e, map_reg(c, armop_reg_of(&op->rd)->armreg), arm_reg(RN_CPSR));
}

/* On to the next block, by a jump of the block's own: through the
 * address's entry point when the block ends by going to a fixed address,
 * else through the table. As on x86-64: every block used to jump to one dispatcher,
 * whose one indirect jump then had to serve every block in the program,
 * and the host has no way to guess where a jump like that goes next.
 * Each block now ends in its own: a block mostly goes on to the same
 * one or two, and its own jump learns them. Where the block ends by
 * going to a fixed address there is nothing to look up at all.
 *
 * The shared dispatcher still takes over when the time slice is used
 * up or an interrupt is waiting, which it checks for again itself. */
static void emit_dispatch(Arm7Compiler *c, const ArmOp *block_ops, u32 count, u32 cycles)
{
	x86_emit *e = &c->e;
	const ArmOp *last = count == 0 ? NULL : &block_ops[count - 1];

	/* The block's cycles come off here, at its end, where what is left
	 * of the time slice is looked at: the subtraction leaves the flags
	 * the jump wants. They used to come off at the top of the block,
	 * with a compare against zero here. Nothing in between reads the
	 * count; the interpreter, when a block calls it, only takes its
	 * own cycles off. */
	x86_alu_mi(e, X86_SUB, arm_reg(CYCL_CNT), cycles);
	x86_jmp_abs(e, X86_CC_LE, (uintptr_t)arm_dispatch);
	x86_alu_mi(e, X86_CMP, arm_reg(INTR_PEND), 0);
	x86_jmp_abs(e, X86_CC_NE, (uintptr_t)arm_dispatch);

	if (last != NULL && last->condition == ARMCC_AL && armop_is_imm(&last->arg[0]) && !armop_is_shifted(&last->arg[0])
			&& ((last->op_type == ARMOP_B || last->op_type == ARMOP_BL)
				|| (last->op_type == ARMOP_MOV && armop_is_reg(&last->rd) && armop_reg_of(&last->rd)->armreg == R15_ARM_NEXT)))
	{
		const u32 target = armop_get_imm(&last->arg[0]);
		x86_jmp_m(e, x86_abs(&arm7_entry_points[(target & (ARAM_SIZE_MAX - 1)) / 4]));
	}
	else
	{
		x86_load(e, X86_ECX, arm_reg(R15_ARM_NEXT));
		x86_load(e, X86_EDX, x86_abs(&entry_points));
		x86_alu_ri(e, false, X86_AND, X86_ECX, 0x7ffffc);
		x86_jmp_m(e, x86_mi(X86_EDX, X86_ECX, 1, 0));		/* four bytes to an entry point, one for each word */
	}
}

static void emit_fallback(Arm7Compiler *c, const ArmOp *op)
{
	c->set_flags = false;
	x86_mov_ri(&c->e, call_regs[0], armop_get_imm(&op->arg[0]));
	x86_call(&c->e, (uintptr_t)arm7_interpret);
}

void arm7backend_compile(const ArmOp *block_ops, u32 count, u32 cycles)
{
	Arm7Compiler compiler;
	Arm7Compiler *c = &compiler;
	x86_emit *e = &c->e;
	ArmCondition currentCondition = ARMCC_AL;
	x86_label condLabel;
	bool condActive = false;
	u32 i;

	x86_init(e, (u8 *)arm7_current_code(), arm7_space_left());
	c->logical_op_set_flags = false;
	c->set_carry_bit = false;
	c->set_flags = false;
	arm_regalloc_init(&c->regalloc, ALLOC_REGS, block_ops, count, load_reg, store_reg, c);

	for (i = 0; i < count; i++)
	{
		const ArmOp *op = &block_ops[i];
		bool save_v_flag = true;

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
			save_v_flag = emit_data_proc_op(c, op);
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

		save_flags(c, save_v_flag);

		arm_regalloc_store(&c->regalloc, i);

		/* The next instruction's condition is for the flags as this one
		 * leaves them - and writing the status register changes them
		 * as surely as an instruction that sets them, which this did
		 * not count: an instruction after it with the same condition
		 * was run or skipped on the flags from before. */
		if (c->set_flags || op->op_type == ARMOP_MSR)
		{
			currentCondition = ARMCC_AL;
			end_conditional(c, &condActive, &condLabel);
		}
	}
	end_conditional(c, &condActive, &condLabel);

	emit_dispatch(c, block_ops, count, cycles);

	verify(!e->full);
	arm7_advance((u32)(e->p - e->start));
}

void arm7backend_flush(void)
{
	x86_emit em;
	x86_emit *e = &em;
	x86_label arm_dispatch_label, arm_exit, arm_dofiq;
	u8 *arm_mainloop_at;

	if (!arm7_code_empty())
	{
		verify(arm_mainloop != NULL);
		verify(arm_compilecode != NULL);
		return;
	}
	x86_init(e, (u8 *)arm7_current_code(), arm7_space_left());
	x86_label_init(&arm_dispatch_label);
	x86_label_init(&arm_exit);
	x86_label_init(&arm_dofiq);

	/* arm_compilecode: */
	x86_call(e, (uintptr_t)arm7_rec_compile);
	x86_jmp_label(e, -1, &arm_dispatch_label, true);

	/* arm_mainloop: */
	arm_mainloop_at = e->p;
	x86_load(e, X86_EAX, x86_m(X86_ESP, 8));		/* entrypoints */
	x86_store(e, x86_abs(&entry_points), X86_EAX);
	x86_push(e, X86_EBP);
	x86_push(e, X86_EBX);
	x86_push(e, X86_ESI);
	x86_push(e, X86_EDI);
	x86_mov_rr(e, X86_EAX, X86_ESP);
	x86_alu_ri(e, false, X86_AND, X86_ESP, ~15u);
	x86_alu_ri(e, false, X86_SUB, X86_ESP, 12);
	x86_push(e, X86_EAX);

	/* arm_dispatch: */
	x86_label_bind(e, &arm_dispatch_label);
	x86_load(e, X86_EDX, x86_abs(&entry_points));
	x86_load(e, X86_ECX, arm_reg(R15_ARM_NEXT));
	x86_load(e, X86_EAX, arm_reg(INTR_PEND));
	x86_alu_mi(e, X86_CMP, arm_reg(CYCL_CNT), 0);
	x86_jmp_label(e, X86_CC_LE, &arm_exit, true);		/* timeslice is over */
	x86_test_rr(e, X86_EAX, X86_EAX);
	x86_jmp_label(e, X86_CC_NE, &arm_dofiq, true);		/* if interrupt pending, handle it */

	x86_alu_ri(e, false, X86_AND, X86_ECX, 0x7ffffc);
	x86_jmp_m(e, x86_mi(X86_EDX, X86_ECX, 1, 0));

	/* arm_dofiq: */
	x86_label_bind(e, &arm_dofiq);
	x86_call(e, (uintptr_t)CPUFiq);
	x86_jmp_label(e, -1, &arm_dispatch_label, true);

	/* arm_exit: */
	x86_label_bind(e, &arm_exit);
	x86_pop(e, X86_ESP);
	x86_pop(e, X86_EDI);
	x86_pop(e, X86_ESI);
	x86_pop(e, X86_EBX);
	x86_pop(e, X86_EBP);
	x86_ret(e);

	verify(!e->full);
	arm_compilecode = (void (*)(void))(uintptr_t)e->start;
	arm_mainloop = (arm_mainloop_t)(uintptr_t)arm_mainloop_at;
	arm_dispatch = (void (*)(void))(uintptr_t)arm_dispatch_label.at;

	arm7_advance((u32)(e->p - e->start));
}

#else
/* (for the other processors: C has no empty file) */
typedef int arm7_rec_x86_not_here;
#endif
