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

#if (HOST_CPU == CPU_X64 || HOST_CPU == CPU_X86 || HOST_CPU == CPU_ARM64) && FEAT_AREC != DYNAREC_NONE

#include <string.h>
#include "arm7_rec.h"
#include "arm7.h"
#include "hw/aica/aica_if.h"
#include "hw/mem/_vmem.h"
#include "arm_mem.h"

#define arm_printf(...) DEBUG_LOG(AICA_ARM, __VA_ARGS__)

void (*arm_compilecode)(void);
arm_mainloop_t arm_mainloop;

u8 *arm7_ic_ptr;
u8 *arm7_icache;
void (*arm7_entry_points[ARAM_SIZE_MAX / 4])(void);

/* Every block there is: where it starts and how many words of the program
 * it was made from. For arm7_rec_flush(), which puts back only the entry
 * points that were given out, and for arm7_rec_same_code(). A block is
 * never under 16 bytes of code: there is room for one for every 16 bytes
 * of the code buffer, and so for all there can be before it is full. */
typedef struct CompiledBlock
{
	u32 pc;
	u32 words;
} CompiledBlock;
#define MAX_BLOCKS (ARM7_ICACHE_SIZE / 16)
static CompiledBlock *compiled_blocks;
static u32 compiled_block_count;
/* what the entry points that are not a block's are set to, NULL before they have been set at all */
static void (*entry_points_filled)(void);

/* The code this makes: in with the program's own, so that it can reach the
 * emulator's data and functions with 32-bit offsets (on ARM64, its
 * functions with a branch). On macOS on Apple's processors it is MAP_JIT
 * memory instead, mapped in arm7_rec_init(). */
#if defined(HOST_APPLE_JIT)
/* (no array) */
#elif defined(_WIN32)
DECL_ALIGN(4096) static u8 ARM7_TCB[ARM7_ICACHE_SIZE];
#elif defined(__linux__) || defined(HAVE_LIBNX)
DECL_ALIGN(4096) static u8 ARM7_TCB[ARM7_ICACHE_SIZE] __attribute__((section(".text")));
#elif defined(__APPLE__)
DECL_ALIGN(4096) static u8 ARM7_TCB[ARM7_ICACHE_SIZE] __attribute__((section("__TEXT, .text")));
#else
#error ARM7_TCB ALLOC
#endif

ptrdiff_t arm7_rx_offset;

/* An instruction's fields */
#define BITS(v, at, n) (((v) >> (at)) & ((1u << (n)) - 1))
#define B_IMM12(v)      BITS(v, 0, 12)
#define B_RD(v)         BITS(v, 12, 4)
#define B_RN(v)         BITS(v, 16, 4)
#define B_SET_FLAGS(v)  BITS(v, 20, 1)
#define B_OP_TYPE(v)    BITS(v, 21, 4)
#define B_IMM_OP(v)     BITS(v, 25, 1)
#define B_OP_GROUP(v)   BITS(v, 26, 2)
#define B_CONDITION(v)  BITS(v, 28, 4)
#define B_RM(v)         BITS(v, 0, 4)
#define B_SHIFT_BY_REG(v) BITS(v, 4, 1)
#define B_SHIFT_TYPE(v) BITS(v, 5, 2)
#define B_SHIFT_IMM(v)  BITS(v, 7, 5)
#define B_IMM8(v)       BITS(v, 0, 8)
#define B_ROTATE(v)     BITS(v, 8, 4)
#define B_ZERO(v)       BITS(v, 7, 1)
#define B_SHIFT_REG(v)  BITS(v, 8, 4)
#define B_LOAD(v)       BITS(v, 20, 1)
#define B_WRITE_BACK(v) BITS(v, 21, 1)
#define B_BYTE(v)       BITS(v, 22, 1)
#define B_UP(v)         BITS(v, 23, 1)
#define B_PRE_INDEX(v)  BITS(v, 24, 1)
/* An immediate operand: 8 bits rotated right by twice the rotate field */
#define B_ROTATED_IMM(v) (B_ROTATE(v) == 0 ? B_IMM8(v) \
		: (B_IMM8(v) >> (B_ROTATE(v) * 2)) | (B_IMM8(v) << (32 - B_ROTATE(v) * 2)))

static u32 set_bits(u32 v, unsigned at, unsigned n, u32 field)
{
	const u32 mask = ((1u << n) - 1) << at;
	return (v & ~mask) | ((field << at) & mask);
}

/* The block being compiled: at most 32 instructions, each of which can
 * become three, and the three the block's end adds */
#define MAX_BLOCK_OPS 128
static ArmOp block_ops[MAX_BLOCK_OPS];
static u32 block_op_count;

static void block_push(const ArmOp *op)
{
	verify(block_op_count < MAX_BLOCK_OPS);
	block_ops[block_op_count++] = *op;
}

/* @op put in at @at, what was there and after it moved on by one */
static void block_insert(u32 at, const ArmOp *op)
{
	verify(block_op_count < MAX_BLOCK_OPS);
	memmove(&block_ops[at + 1], &block_ops[at], (block_op_count - at) * sizeof(block_ops[0]));
	block_ops[at] = *op;
	block_op_count++;
}

/* findfirstset -- used in LDM/STM handling */
#ifdef _MSC_VER
#include <intrin.h>

static u32 findfirstset(u32 v)
{
	unsigned long rv;
	_BitScanForward(&rv,v);
	return rv+1;
}
#else
#define findfirstset __builtin_ffs
#endif

static ArmOp fallback_of(u32 opcode)
{
	ArmOp op = armop_make(ARMOP_FALLBACK, ARMCC_AL);
	op.arg[0] = armop_imm(opcode);
	op.cycles = 0;
	return op;
}

static ArmOp decodeArmOp(u32 opcode, u32 arm_pc)
{
	const u32 bits = opcode;
	ArmOp op = armop_make(ARMOP_FALLBACK, ARMCC_AL);

	op.condition = (ArmCondition)B_CONDITION(bits);
	if (op.condition == ARMCC_UC)
	{
		/* NV condition means VFP on newer cores, let interpreter handle it... */
		op.op_type = ARMOP_FALLBACK;
		op.arg[0] = armop_imm(opcode);
		op.cycles = 0;
		return op;
	}
	if (op.condition != ARMCC_AL)
		op.flags |= ARMOP_READS_FLAGS;

	switch (B_OP_GROUP(bits))
	{
	case 0:	/* data processing, Multiply, Swap */
		/* disambiguate */
		if ((B_OP_TYPE(bits) & 4) == 0 && B_IMM_OP(bits) == 0 && B_SHIFT_BY_REG(bits) && B_ZERO(bits) != 0)
		{
			/* MUL, MLA or SWP */
			op.op_type = ARMOP_FALLBACK;
			op.arg[0] = armop_imm(opcode);
			op.cycles = 0;
			return op;
		}
		if (B_OP_TYPE(bits) >= (u32)ARMOP_TST && B_OP_TYPE(bits) <= ARMOP_CMN && B_SET_FLAGS(bits) == 0)
		{
			/* MSR, MRS */
			op.spsr = (B_OP_TYPE(bits) & 2) != 0;
			if ((bits & 0x0FBF0FFF) == 0x010F0000)
			{
				op.op_type = ARMOP_MRS;
				op.rd = armop_reg((Arm7Reg)B_RD(bits));
				verify(B_RD(bits) != 15);
			}
			else if ((bits & 0x0FB0FFF0) == 0x0120F000)
			{
				op.op_type = ARMOP_MSR;
				op.psrMask = (bits >> 16) & 0xF;
				op.arg[0] = armop_reg((Arm7Reg)B_RM(bits));
				op.cycles++;
			}
			else if ((bits & 0x0DB0F000) == 0x0120F000)
			{
				op.op_type = ARMOP_MSR;
				op.psrMask = (bits >> 16) & 0xF;
				if (B_IMM_OP(bits) == 0)
				{
					/* source is reg */
					op.arg[0] = armop_reg((Arm7Reg)B_RM(bits));
					verify(B_RM(bits) != 15);
				}
				else
				{
					op.arg[0] = armop_imm(B_ROTATED_IMM(bits));
				}
			}
			else
			{
				/* Unsupported op */
				op.op_type = ARMOP_MOV;
				op.condition = ARMCC_AL;
				op.flags = 0;
				op.rd = armop_reg((Arm7Reg)0);
				op.arg[0] = op.rd;
			}
			return op;
		}
		{
			int argidx = 0;
			ArmOpOperand *a;

			op.op_type = (ArmOpType)B_OP_TYPE(bits);
			if (!armop_is_comp(&op))
				op.rd = armop_reg((Arm7Reg)B_RD(bits));
			if (op.op_type != ARMOP_MOV && op.op_type != ARMOP_MVN)
			{
				op.arg[0] = armop_reg((Arm7Reg)B_RN(bits));
				if (armop_get_reg(&op.arg[0])->armreg == RN_PC)
					op.arg[0] = armop_imm(arm_pc + (!B_IMM_OP(bits) && B_SHIFT_BY_REG(bits) ? 12 : 8));
				argidx++;
			}
			if (B_SET_FLAGS(bits))
				op.flags |= ARMOP_SETS_FLAGS;
			a = &op.arg[argidx];
			if (B_IMM_OP(bits))
				*a = armop_imm(B_ROTATED_IMM(bits));
			else
			{
				*a = armop_reg((Arm7Reg)B_RM(bits));
				a->shift_type = (ArmShiftOp)B_SHIFT_TYPE(bits);
				a->shift_imm = B_SHIFT_BY_REG(bits) == 0;
				if (a->shift_imm)
				{
					a->s.shift_value = B_SHIFT_IMM(bits);
					if (a->shift_type == ARMSH_RRX && a->s.shift_value == 0)
						op.flags |= ARMOP_READS_FLAGS;
				}
				else
				{
					a->s.shift_reg = armop_register((Arm7Reg)B_SHIFT_REG(bits));
					op.cycles++;
				}
				/* Compute pc-relative addresses */
				if (armop_get_reg(a)->armreg == RN_PC)
				{
					if (a->shift_imm && !((op.flags & ARMOP_SETS_FLAGS) && armop_is_logical(&op)) && a->shift_type != ARMSH_ROR
							/* LSR #0 and ASR #0 are how shifts by 32 are written */
							&& (a->shift_type == ARMSH_LSL || a->s.shift_value != 0))
					{
						const u32 next_pc = arm_pc + 8;
						switch (a->shift_type)
						{
						case ARMSH_LSL:
							*a = armop_imm(next_pc << a->s.shift_value);
							break;
						case ARMSH_LSR:
							*a = armop_imm(next_pc >> a->s.shift_value);
							break;
						case ARMSH_ASR:
							*a = armop_imm((u32)((int)next_pc >> a->s.shift_value));
							break;
						default:
							break;
						}
					}
					else
						armop_set_imm(a, arm_pc + (a->shift_imm ? 8 : 12));
				}
			}
			if (armop_is_reg(&op.rd) && armop_get_reg(&op.rd)->armreg == RN_PC)
			{
				if (op.op_type == ARMOP_MOV && armop_is_reg(&op.arg[0]) && !B_SET_FLAGS(bits) && !armop_is_shifted(&op.arg[0]))
				{
					/* MOVcc pc, rn -> B */
					op.op_type = ARMOP_B;
					op.flags |= ARMOP_SETS_PC;
					op.rd = armop_none();
					op.cycles += 3;
					return op;
				}
				if (op.condition != ARMCC_AL || (op.flags & ARMOP_SETS_FLAGS))
				{
					/* TODO no support for conditional/setflags ops that set pc except the case above */
					op.op_type = ARMOP_FALLBACK;
					op.arg[0] = armop_imm(opcode);
					op.arg[1] = armop_none();
					op.arg[2] = armop_none();
					op.rd = armop_none();
					op.cycles = 0;
				}
				else
				{
					armop_get_reg(&op.rd)->armreg = R15_ARM_NEXT;
					/* (a constant for the program counter goes in without its
					 * low two bits; anything else loses them where it is made) */
					if (op.op_type == ARMOP_MOV && armop_is_imm(&op.arg[0]) && !armop_is_shifted(&op.arg[0]))
						armop_set_imm(&op.arg[0], armop_get_imm(&op.arg[0]) & ~3u);
					op.cycles++;
				}
				op.flags |= ARMOP_SETS_PC;
			}
			if (op.op_type == ARMOP_ADC || op.op_type == ARMOP_SBC || op.op_type == ARMOP_RSC)
				op.flags |= ARMOP_READS_FLAGS;
			if (((!B_IMM_OP(bits) && B_SHIFT_BY_REG(bits))
						|| (!B_IMM_OP(bits) && B_RM(bits) == 15)
						|| (B_IMM_OP(bits) && B_ROTATE(bits) != 0 && B_ROTATED_IMM(bits) <= 255))
					&& B_SET_FLAGS(bits) && armop_is_logical(&op) && !(op.flags & ARMOP_SETS_PC))
			{
				/* A logical instruction that sets the flags takes its carry
				 * from the shift, and a shift by a register has a carry for
				 * every count from 0 to 255. The backends do not work that
				 * out, so the interpreter runs these; they are rare.
				 *
				 * So does it a constant that is rotated and still comes out
				 * under 256: the rotation sets the carry, to 0, and the
				 * backends take a constant that small for one that was not
				 * rotated. No assembler writes a small constant that way.
				 *
				 * And r15 as the second operand, which the backends would
				 * be given as a constant and take for a rotated one. */
				return fallback_of(opcode);
			}
		}
		break;

	case 1: /* LDR/STR */
		op.add_offset = B_UP(bits) != 0;
		op.byte_xfer = B_BYTE(bits) != 0;
		op.pre_index = B_PRE_INDEX(bits) != 0;
		op.write_back = (B_WRITE_BACK(bits) || !B_PRE_INDEX(bits)) && B_RD(bits) != B_RN(bits) && (B_IMM_OP(bits) != 0 || B_IMM12(bits) != 0);
		if (B_LOAD(bits))
		{
			op.op_type = ARMOP_LDR;
			op.rd = armop_reg((Arm7Reg)B_RD(bits));
			if (armop_get_reg(&op.rd)->armreg == RN_PC)			/* LDR w/ rd=pc */
			{
				op.flags |= ARMOP_SETS_PC;
				if (op.condition != ARMCC_AL)
				{
					/* TODO no support for conditional ops */
					op.op_type = ARMOP_FALLBACK;
					op.arg[0] = armop_imm(opcode);
					op.cycles = 0;
					return op;
				}
				armop_set_reg(&op.rd, R15_ARM_NEXT, 0);
				op.cycles++;
			}
			op.cycles += 4;
		}
		else
		{
			op.op_type = ARMOP_STR;
			op.arg[2] = armop_reg((Arm7Reg)B_RD(bits));
			if (armop_get_reg(&op.arg[2])->armreg == RN_PC)
				op.arg[2] = armop_imm(arm_pc + 12);
			op.cycles += 3;
		}
		op.arg[0] = armop_reg((Arm7Reg)B_RN(bits));
		if (armop_get_reg(&op.arg[0])->armreg == RN_PC && op.write_back)
		{
			/* LDR/STR w pc-based offset and write-back
			 * Invalid: Write-back must not be specified if R15 is used as the base register */
			op.flags |= ARMOP_SETS_PC;
			op.op_type = ARMOP_FALLBACK;
			op.arg[0] = armop_imm(opcode);
			op.arg[1] = armop_none();
			op.arg[2] = armop_none();
			op.cycles = 0;
			return op;
		}
		if (B_IMM_OP(bits) == 0)
		{
			/* Immediate offset */
			if (armop_get_reg(&op.arg[0])->armreg == RN_PC)
				/* Compute pc-relative address */
				op.arg[0] = armop_imm(arm_pc + 8 + (op.add_offset ? (int)B_IMM12(bits) : -(int)B_IMM12(bits)));
			else
				op.arg[1] = armop_imm(B_IMM12(bits));
		}
		else
		{
			/* Offset by register, optionally shifted */
			if (armop_get_reg(&op.arg[0])->armreg == RN_PC)
				op.arg[0] = armop_imm(arm_pc + 8);
			op.arg[1] = armop_reg((Arm7Reg)B_RM(bits));
			op.arg[1].shift_type = (ArmShiftOp)B_SHIFT_TYPE(bits);
			op.arg[1].shift_imm = true;
			op.arg[1].s.shift_value = B_SHIFT_IMM(bits);
			if (armop_get_reg(&op.arg[1])->armreg == RN_PC)
			{
				/* Invalid: r15 cannot be used as the offset register */
				op.op_type = ARMOP_FALLBACK;
				op.arg[0] = armop_imm(opcode);
				op.arg[1] = armop_none();
				op.arg[2] = armop_none();
				op.cycles = 0;
				return op;
			}
			if (op.arg[1].shift_type == ARMSH_RRX && op.arg[1].s.shift_value == 0)
				op.flags |= ARMOP_READS_FLAGS;
		}
		break;

	case 2:	/* LDM/STM and B,BL */
		if (B_IMM_OP(bits))
		{
			/* B, BL */
			op.op_type = B_OP_TYPE(bits) & 8 ? ARMOP_BL : ARMOP_B;	/* L bit */
			op.arg[0] = armop_imm(arm_pc + 8 + ((((opcode & 0xffffff) ^ 0x800000u) - 0x800000u) << 2));	/* (sign-extended) */
			op.flags |= ARMOP_SETS_PC;
			op.cycles += 3;
		}
		else
		{
			/* LDM/STM */
			u32 reg_list = opcode & 0xffff;
			/* one register selected and no PSR */
			if (!(opcode & (1 << 22)) && cpuBitsSet[reg_list & 255] + cpuBitsSet[(reg_list >> 8) & 255] == 1)
			{
				/* One register transferred: a load or store of a word.
				 * Immediate offset, the same indexing and direction, write
				 * back (always 0 for post-increment) only when the block
				 * transfer asks for it, Rn as it is, Rd the one register,
				 * an offset of 4. */
				u32 newbits = 0x04000000 | (opcode & 0xf0000000);
				const bool load = (opcode & (1 << 20)) != 0;

				newbits = set_bits(newbits, 24, 1, B_PRE_INDEX(bits));
				newbits = set_bits(newbits, 23, 1, B_UP(bits));
				newbits = set_bits(newbits, 21, 1, B_WRITE_BACK(bits) & B_PRE_INDEX(bits));
				newbits = set_bits(newbits, 20, 1, load ? 1 : 0);
				newbits = set_bits(newbits, 16, 4, B_RN(bits));
				newbits = set_bits(newbits, 12, 4, findfirstset(reg_list) - 1);
				newbits |= 4;

				/* A load that indexes after the access always writes the
				 * base back; a block load does so only when asked. Not
				 * asked, it is a load from the base as it is. (A store as
				 * well: this used to write the base back on every store
				 * that indexes before the access.) */
				if (!B_PRE_INDEX(bits) && !B_WRITE_BACK(bits))
				{
					newbits = set_bits(newbits, 24, 1, 1);
					newbits &= ~0xfffu;
				}

				if (load)
				{
					arm_printf("ARM: MEM TFX R %08X -> %08X\n", opcode, newbits);
					op = decodeArmOp(newbits, arm_pc);
					/* All but one thing: a block load reads from a whole
					 * word's address whatever the base's low two bits. */
					op.aligned = true;
					if (op.op_type == ARMOP_FALLBACK)
						op.arg[0] = armop_imm(opcode);
				}
				else
				{
					arm_printf("ARM: MEM TFX W %08X -> %08X\n", opcode, newbits);
					op = decodeArmOp(newbits, arm_pc);
				}
				op.cycles += 4;
				return op;
			}
			op.op_type = ARMOP_FALLBACK;
			op.arg[0] = armop_imm(opcode);
			op.cycles = 0;

			if ((opcode & 0x8000) && B_LOAD(bits))	/* LDM w/ pc */
				op.flags |= ARMOP_SETS_PC;
		}
		break;

	case 3: /* coproc, SWI */
		op.op_type = ARMOP_FALLBACK;
		op.arg[0] = armop_imm(opcode);
		op.cycles = 0;
		if (B_IMM_OP(bits) == 1 && (B_OP_TYPE(bits) & 8)) /* SWI */
			op.flags |= ARMOP_SETS_PC;
		break;
	}

	return op;
}

static void block_ssa_pass(void)
{
	u32 versions[RN_ARM_REG_SLOTS];
	u32 i;
	int k;

	memset(versions, 0, sizeof(versions));
	for (i = 0; i < block_op_count; i++)
	{
		ArmOp *it = &block_ops[i];
		if (it->op_type == ARMOP_FALLBACK)
			for (k = 0; k < RN_ARM_REG_SLOTS; k++)
				versions[k]++;
		else
		{
			if ((it->op_type == ARMOP_STR || it->op_type == ARMOP_LDR) && it->write_back)
			{
				/* Extract add/sub operation from STR/LDR */
				if (it->op_type == ARMOP_LDR && !it->pre_index && armop_is_reg(&it->arg[1])
						&& armop_get_reg(&it->arg[1])->armreg == armop_get_reg(&it->rd)->armreg)
				{
					/* Edge case where the offset reg is the target register but its value before the op
					 * must be used in post-increment/decrement
					 * Thus we save its value in a scratch register. */
					ArmOp newop = armop_make(ARMOP_MOV, it->condition);
					ArmOp newop2;
					newop.rd = armop_reg(RN_SCRATCH);
					newop.arg[0] = it->rd;
					/* Insert before */
					block_insert(i, &newop);
					i++;
					it = &block_ops[i];
					newop2 = armop_make(it->add_offset ? ARMOP_ADD : ARMOP_SUB, it->condition);
					newop2.rd = it->arg[0];
					newop2.arg[0] = newop2.rd;
					newop2.arg[1] = it->arg[1];
					armop_set_reg(&newop2.arg[1], RN_SCRATCH, 0);
					if (it->arg[1].shift_type == ARMSH_RRX && it->arg[1].s.shift_value == 0)
						newop2.flags |= ARMOP_READS_FLAGS;
					it->flags &= ~ARMOP_READS_FLAGS;
					it->write_back = false;
					it->arg[1] = armop_none();
					/* Insert after */
					block_insert(i + 1, &newop2);
					i--;
					it = &block_ops[i];
				}
				else
				{
					ArmOp newop = armop_make(it->add_offset ? ARMOP_ADD : ARMOP_SUB, it->condition);
					newop.rd = it->arg[0];
					newop.arg[0] = newop.rd;
					newop.arg[1] = it->arg[1];
					if (it->arg[1].shift_type == ARMSH_RRX && it->arg[1].s.shift_value == 0)
						newop.flags |= ARMOP_READS_FLAGS;
					if (it->condition == ARMCC_AL)
						it->flags &= ~ARMOP_READS_FLAGS;
					it->write_back = false;
					it->arg[1] = armop_none();
					if (it->pre_index)
						/* Insert before */
						block_insert(i, &newop);
					else
						/* Insert after */
						block_insert(i + 1, &newop);
					it = &block_ops[i];
				}
			}
			/* Set versions */
			for (k = 0; k < 3; k++)
			{
				ArmOpOperand *arg = &it->arg[k];
				if (armop_is_reg(arg))
					armop_get_reg(arg)->version = (int)versions[(size_t)armop_get_reg(arg)->armreg];
				if (!arg->shift_imm)
					arg->s.shift_reg.version = (int)versions[(size_t)arg->s.shift_reg.armreg];
			}
			if (armop_is_reg(&it->rd))
				armop_get_reg(&it->rd)->version = (int)++versions[(size_t)armop_get_reg(&it->rd)->armreg];
			/* (see arm_regalloc_load()) */
			if (it->op_type == ARMOP_MSR)
				for (k = 0; k < RN_ARM_REG_SLOTS; k++)
					versions[k]++;
		}
	}
}

/* The register allocator */

void arm_regalloc_init(ArmRegAlloc *ra, int max_regs, const ArmOp *ops, u32 count,
		void (*load_reg)(void *ctx, int host_reg, Arm7Reg armreg),
		void (*store_reg)(void *ctx, int host_reg, Arm7Reg armreg), void *ctx)
{
	int i;

	for (i = 0; i < RN_ARM_REG_SLOTS; i++)
	{
		ra->allocs[i].host_reg = -1;
		ra->allocs[i].version = 0;
		ra->allocs[i].dirty = false;
		ra->allocs[i].temporary = false;
	}
	ra->host_reg_count = 0;
	for (i = 0; i < max_regs; i++)
		ra->host_regs[ra->host_reg_count++] = i;
	ra->block_ops = ops;
	ra->block_op_count = count;
	ra->load_reg = load_reg;
	ra->store_reg = store_reg;
	ra->ctx = ctx;
}

/* Whether a register written at @opidx - 1 has to be written back: not if
 * the rest of the block writes it again before reading it */
static bool needs_writeback(const ArmRegAlloc *ra, const ArmOpRegister *reg, u32 opidx)
{
	for (; opidx < ra->block_op_count; opidx++)
	{
		const ArmOp *it = &ra->block_ops[opidx];
		if (it->op_type == ARMOP_FALLBACK)
			/* assume the value is being used */
			return true;
		if (armop_is_reg(&it->rd) && armop_reg_of(&it->rd)->armreg == reg->armreg)
			/* overwritten so it is not used - unless the write may not
			 * happen, and then it is written back */
			return it->condition != ARMCC_AL;
	}
	/* assume the value will be used */
	return true;
}

/* The index of the next instruction that uses the given version of a
 * register, -1 if none does in the rest of the block or a fallback comes
 * first */
static int next_use(const ArmRegAlloc *ra, Arm7Reg reg, int version, u32 opidx)
{
	for (; opidx < ra->block_op_count; opidx++)
	{
		const ArmOp *it = &ra->block_ops[opidx];
		int k;
		if (it->op_type == ARMOP_FALLBACK)
			return -1;
		for (k = 0; k < 3; k++)
		{
			const ArmOpOperand *arg = &it->arg[k];
			if (armop_is_reg(arg) && armop_reg_of(arg)->armreg == reg)
				return armop_reg_of(arg)->version == version ? (int)opidx : -1;
			if (!arg->shift_imm && arg->s.shift_reg.armreg == reg)
				return arg->s.shift_reg.version == version ? (int)opidx : -1;
		}
		if (armop_is_reg(&it->rd) && armop_reg_of(&it->rd)->armreg == reg)
			return -1;
	}
	return -1;
}

static void flush_reg(ArmRegAlloc *ra, ArmRegAllocSlot *alloc)
{
	if (alloc->dirty)
	{
		ra->store_reg(ra->ctx, alloc->host_reg, (Arm7Reg)(alloc - ra->allocs));
		alloc->dirty = false;
	}
	ra->host_regs[ra->host_reg_count++] = alloc->host_reg;
	alloc->host_reg = -1;
}

static void flush_all_regs(ArmRegAlloc *ra)
{
	int i;
	for (i = 0; i < RN_ARM_REG_SLOTS; i++)
		if (ra->allocs[i].host_reg != -1)
			flush_reg(ra, &ra->allocs[i]);
}

static void alloc_reg(ArmRegAlloc *ra, const ArmOpRegister *reg, bool write, bool temporary, u32 opidx)
{
	ArmRegAllocSlot *alloc = &ra->allocs[(int)reg->armreg];
	/* Another value of a register that is in a host register: the
	 * register file has the new one (written there by the interpreter or
	 * swapped in by a change of mode, which come after everything has
	 * been written back). It is read into the same host register. This
	 * used to take another host register and leave the first one with
	 * nothing, gone for the rest of the block: on 32-bit x86, which has
	 * four, a block could run out of them in the middle of an
	 * instruction. */
	if (alloc->host_reg != -1 && alloc->version != reg->version && !write)
	{
		alloc->version = (u16)reg->version;
		alloc->dirty = false;
		alloc->temporary = temporary;
		ra->load_reg(ra->ctx, alloc->host_reg, reg->armreg);
		return;
	}
	if (alloc->host_reg == -1)
	{
		if (ra->host_reg_count == 0)
		{
			/* Need to flush a reg */
			Arm7Reg bestReg = (Arm7Reg)-1;
			int bestRegUse = -1;
			u32 i;
			for (i = 0; i < RN_ARM_REG_SLOTS; i++)
			{
				ArmRegAllocSlot *other = &ra->allocs[i];
				if (other->host_reg == -1)
					continue;
				if (!other->dirty)
				{
					int nextUse_ = next_use(ra, (Arm7Reg)i, other->version, opidx);
					if (nextUse_ == -1)
					{
						ra->host_regs[ra->host_reg_count++] = other->host_reg;
						other->host_reg = -1;
						break;
					}
					if (nextUse_ != (int)opidx && nextUse_ > bestRegUse)
					{
						bestRegUse = nextUse_;
						bestReg = (Arm7Reg)i;
					}
				}
			}
			if (ra->host_reg_count == 0)
			{
				if (bestReg == (Arm7Reg)-1)
				{
					for (i = 0; i < RN_ARM_REG_SLOTS; i++)
					{
						ArmRegAllocSlot *other = &ra->allocs[i];
						int nextUse_;
						if (other->host_reg == -1)
							continue;
						nextUse_ = next_use(ra, (Arm7Reg)i, other->version, opidx);
						if (nextUse_ == -1)
						{
							bestReg = (Arm7Reg)i;
							break;
						}
						if (nextUse_ != (int)opidx && nextUse_ > bestRegUse)
						{
							bestRegUse = nextUse_;
							bestReg = (Arm7Reg)i;
						}
					}
					verify(bestReg != (Arm7Reg)-1);
					DEBUG_LOG(AICA_ARM, "Flushing dirty register r%d\n", (int)bestReg);
				}
				flush_reg(ra, &ra->allocs[bestReg]);
			}
			verify(ra->host_reg_count != 0);
		}
		alloc->host_reg = ra->host_regs[--ra->host_reg_count];
		alloc->version = (u16)reg->version;
		alloc->dirty = write;
		alloc->temporary = temporary;
		if (!write)
			ra->load_reg(ra->ctx, alloc->host_reg, reg->armreg);
	}
	if (write)
	{
		alloc->dirty = true;
		alloc->version = (u16)reg->version;
		alloc->temporary = temporary;
	}
}

void arm_regalloc_load(ArmRegAlloc *ra, u32 opidx)
{
	const ArmOp *op = &ra->block_ops[opidx];
	if (op->op_type == ARMOP_FALLBACK)
		flush_all_regs(ra);
	else
	{
		bool conditional = op->condition != ARMCC_AL;
		int k;
		/* Writing the status register can change the mode, and with it
		 * which of the banked registers r8 to r14 are: arm7_msr_cpsr() swaps
		 * them in the register file. Every register is written back
		 * before and read again after (block_ssa_pass() gives them all
		 * a new version), as around the interpreter. Which values were
		 * still in host registers used to depend on how many there are
		 * on the host. */
		if (op->op_type == ARMOP_MSR)
			flush_all_regs(ra);
		for (k = 0; k < 3; k++)
		{
			const ArmOpOperand *arg = &op->arg[k];
			if (armop_is_reg(arg))
				alloc_reg(ra, armop_reg_of(arg), false, conditional, opidx);
			if (!arg->shift_imm)
				alloc_reg(ra, &arg->s.shift_reg, false, conditional, opidx);
		}
		if (armop_is_reg(&op->rd))
			alloc_reg(ra, armop_reg_of(&op->rd), true, conditional, opidx);
	}
}

void arm_regalloc_store_dirty(ArmRegAlloc *ra)
{
	int i;
	for (i = 0; i < RN_ARM_REG_SLOTS; i++)
	{
		ArmRegAllocSlot *alloc = &ra->allocs[i];
		if (alloc->host_reg != -1 && alloc->dirty)
		{
			ra->store_reg(ra->ctx, alloc->host_reg, (Arm7Reg)i);
			alloc->dirty = false;
		}
	}
}

void arm_regalloc_store(ArmRegAlloc *ra, u32 opidx)
{
	const ArmOp *op = &ra->block_ops[opidx];
	if (op->op_type == ARMOP_FALLBACK)
		return;
	if (op->condition != ARMCC_AL)
	{
		int i;
		for (i = 0; i < RN_ARM_REG_SLOTS; i++)
			if (ra->allocs[i].host_reg != -1 && ra->allocs[i].temporary)
				flush_reg(ra, &ra->allocs[i]);
	}
	else if (armop_is_reg(&op->rd) && needs_writeback(ra, armop_reg_of(&op->rd), opidx + 1))
	{
		ArmRegAllocSlot *alloc = &ra->allocs[(int)armop_reg_of(&op->rd)->armreg];
		ra->store_reg(ra->ctx, alloc->host_reg, armop_reg_of(&op->rd)->armreg);
		alloc->dirty = false;
	}
}

void arm7_rec_flush(void);

void arm7_rec_compile(void)
{
	void *rv;
	u32 pc, cycles, ops;

	/* Out of room: start over. Everything is compiled again as it is
	 * reached, the main loop first and in the same place, so the code that
	 * called this is still there to go back to. A sound driver is a few
	 * kilobytes and this is megabytes; without this, running out threw. */
	if (arm7_space_left() < 64 * 1024 || compiled_block_count == MAX_BLOCKS)
		arm7_rec_flush();

	/* Get the code ptr */
	rv = arm7_ic_ptr;

	/* setup local pc counter
	 * (of whole words: the block is found by its address without the low
	 * two bits, and has to be the code for that address) */
	pc = arm_Reg[R15_ARM_NEXT].I & ~3u;

	/* update the block table
	 * Note that we mask with the max aica size (8 MB), which is
	 * also the size of the EntryPoints table. This way the dynarec
	 * main loop doesn't have to worry about the actual aica
	 * ram size. The aica ram always wraps to 8 MB anyway. */
	arm7_entry_points[(pc & (ARAM_SIZE_MAX - 1)) / 4] = (void (*)(void))(uintptr_t)arm7_write_to_exec(rv);

	block_op_count = 0;
	cycles = 0;

	arm_printf("ARM7 Block %x\n", pc);
	/* the ops counter is used to terminate the block (max op count for a single block is 32 currently)
	 * We don't want too long blocks for timing accuracy */
	for (ops = 0; ops < 32; ops++)
	{
		/* Read opcode ...
		 * (a whole word inside memory, whatever the low bits of pc are) */
		u32 opcd = *(u32*)&aica_ram.data[pc & ARAM_MASK & ~3u];
		ArmOp last_op = decodeArmOp(opcd, pc);
		cycles += last_op.cycles;

		/* Goto next opcode */
		pc += 4;

		if (opcd != 0 && opcd != 0x41474553)	/* andeq r0, r0, r0 (== NOP) and 'SEGA' */
		{										/* ARAM is filled with these at start up */
			if (last_op.op_type == ARMOP_FALLBACK)
			{
				/* Interpreter needs pc + 8 in r15 */
				ArmOp armop = armop_make(ARMOP_MOV, ARMCC_AL);
				armop.rd = armop_reg(RN_PC);
				armop.arg[0] = armop_imm(pc + 4);
				block_push(&armop);
			}
			/* Branch ? */
			if (last_op.flags & ARMOP_SETS_PC)
			{
				if (last_op.condition != ARMCC_AL)
				{
					/* insert a "mov armNextPC, pc + 4" before the jump if not taken */
					ArmOp armop = armop_make(ARMOP_MOV, ARMCC_AL);
					armop.rd = armop_reg(R15_ARM_NEXT);
					armop.arg[0] = armop_imm(pc);
					block_push(&armop);
				}
				if (last_op.op_type == ARMOP_BL)
				{
					/* Save pc+4 into r14 */
					ArmOp armop = armop_make(ARMOP_MOV, last_op.condition);
					armop.rd = armop_reg(RN_LR);
					armop.arg[0] = armop_imm(pc);
					block_push(&armop);
				}
				block_push(&last_op);
				arm_printf("ARM: %06X: Block End %d\n", pc, ops);
				break;
			}
			block_push(&last_op);
		}

		/* block size limit ? */
		if (ops == 31)
		{
			/* Update armNextPC */
			ArmOp armop = armop_make(ARMOP_MOV, ARMCC_AL);
			armop.rd = armop_reg(R15_ARM_NEXT);
			armop.arg[0] = armop_imm(pc);
			block_push(&armop);
			arm_printf("ARM: %06X: Block split\n", pc);
		}
	}

	{
		const u32 start = arm_Reg[R15_ARM_NEXT].I & ~3u;
		compiled_blocks[compiled_block_count].pc = start;
		compiled_blocks[compiled_block_count].words = (pc - start) / 4;
		compiled_block_count++;
	}

	block_ssa_pass();

	arm7backend_compile(block_ops, block_op_count, cycles);

	arm_printf("arm7rec_compile done: %p,%p\n", rv, (void *)arm7_ic_ptr);
}

void arm7_rec_flush(void)
{
	u32 i;
#ifdef HOST_APPLE_JIT
	if (arm7_icache == NULL)
		return;
#endif
	arm7_ic_ptr = arm7_icache;
	arm7backend_flush();
	verify(arm_compilecode != NULL);
	/* There are two million entry points, 16 megabytes of them, and a few
	 * hundred blocks: only the ones that were given out are put back. All
	 * of them the first time, and if "compile this" has moved. */
	if (entry_points_filled != arm_compilecode)
	{
		for (i = 0; i < (sizeof(arm7_entry_points) / sizeof(arm7_entry_points[0])); i++)
			arm7_entry_points[i] = arm_compilecode;
		entry_points_filled = arm_compilecode;
	}
	else
	{
		for (i = 0; i < compiled_block_count; i++)
			arm7_entry_points[(compiled_blocks[i].pc & (ARAM_SIZE_MAX - 1)) / 4] = arm_compilecode;
	}
	compiled_block_count = 0;
}

/* Whether everything that has been compiled was compiled from what @aram -
 * the sound memory of a save state about to be loaded - has in the same
 * places: then it is all still good after the load. */
bool arm7_rec_same_code(const u8 *aram)
{
	u32 b, i;
	for (b = 0; b < compiled_block_count; b++)
		for (i = 0; i < compiled_blocks[b].words; i++)
		{
			const u32 at = (compiled_blocks[b].pc + i * 4) & ARAM_MASK & ~3u;

			if (memcmp(&aica_ram.data[at], aram + at, 4) != 0)
				return false;
		}
	return true;
}

void arm7_rec_init(void)
{
	/* Once: this is called for every game that is loaded, and on Windows
	 * the code does not go in ARM7_TCB but in memory allocated here, which
	 * nothing gave back. The same block serves until the program ends, and
	 * so does the list of blocks. */
#ifdef HOST_APPLE_JIT
	/* Mapped the first time and kept: without it (no JIT entitlement) the
	 * interpreter runs, aicaarm_run() */
	static bool asked;

	if (!asked)
	{
		arm7_icache = (u8 *)vmem_platform_alloc_jit(ARM7_ICACHE_SIZE);
		asked = true;
	}
	if (arm7_icache == NULL)
		return;
#else
	if (arm7_icache == NULL)
	{
		bool rc = vmem_platform_prepare_jit_block(ARM7_TCB, ARM7_ICACHE_SIZE, (void**)&arm7_icache);
		verify(rc);
	}
#endif
	if (compiled_blocks == NULL)
	{
		compiled_blocks = (CompiledBlock *)malloc(MAX_BLOCKS * sizeof(*compiled_blocks));
		verify(compiled_blocks != NULL);
	}

	arm7_rec_flush();
}

static u32 DYNACALL mem_load8(u32 addr, u32 data)
{
	(void)data;
	return arm_ReadMem8(addr);
}

static u32 DYNACALL mem_load32(u32 addr, u32 data)
{
	(void)data;
	return arm_ReadMem32(addr);
}

static u32 DYNACALL mem_store8(u32 addr, u32 data)
{
	arm_WriteMem8(addr, (u8)data);
	return 0;
}

static u32 DYNACALL mem_store32(u32 addr, u32 data)
{
	arm_WriteMem32(addr, data);
	return 0;
}

void *arm7_rec_mem_op(bool load, bool byte)
{
	if (load)
		return byte ? (void *)(uintptr_t)mem_load8 : (void *)(uintptr_t)mem_load32;
	return byte ? (void *)(uintptr_t)mem_store8 : (void *)(uintptr_t)mem_store32;
}

/* Run a timeslice of arm7 (what arm7.h has the sound chip run the ARM7 by) */
void aicaarm_run(u32 samples)
{
	/* The interpreter instead, when the core option asks for it: for
	 * telling whether a sound problem is the recompiler's. The two keep the
	 * ARM's state in the same place and can change over between any two
	 * samples. Coming back, everything compiled is thrown away: the
	 * recompiler was not there to see what ran in the meantime, and the
	 * program in the ARM's memory may be another by now. */
	static bool interpreting;
	u32 i;

#ifdef HOST_APPLE_JIT
	if (settings.dynarec.Arm7Interpreter || arm7_icache == NULL)
#else
	if (settings.dynarec.Arm7Interpreter)
#endif
	{
		if (!interpreting)
			NOTICE_LOG(AICA_ARM, "ARM7: interpreter\n");
		interpreting = true;
		for (i = 0; i < samples; i++)
		{
			arm_Run_(ARM_CYCLES_PER_SAMPLE);
			libAICA_TimeStep();
		}
		return;
	}
	if (interpreting)
	{
		NOTICE_LOG(AICA_ARM, "ARM7: recompiler\n");
		interpreting = false;
		arm7_rec_flush();
	}

	for (i = 0; i < samples; i++)
	{
		if (Arm7Enabled)
		{
			arm_Reg[CYCL_CNT].I += ARM_CYCLES_PER_SAMPLE;
			arm_mainloop(arm_Reg, arm7_entry_points);
		}
		libAICA_TimeStep();
	}
}

#else
/* (for the other processors: C has no empty file) */
typedef int arm7_rec_not_here;
#endif
