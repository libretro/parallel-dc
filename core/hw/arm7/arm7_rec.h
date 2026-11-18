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
/* The ARM7 recompiler's decoded instructions, their register allocation and
 * the code buffer, shared by its decoder (arm7_rec.c) and its backends
 * (arm7_rec_x64.c, arm7_rec_x86.c, arm7_rec_arm64.c). C89. */
#ifndef ARM7_REC_H
#define ARM7_REC_H

#include "types.h"
#include "arm7.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ArmOpType
{
	ARMOP_AND, ARMOP_EOR, ARMOP_SUB, ARMOP_RSB, ARMOP_ADD, ARMOP_ADC, ARMOP_SBC, ARMOP_RSC,
	ARMOP_TST, ARMOP_TEQ, ARMOP_CMP, ARMOP_CMN, ARMOP_ORR, ARMOP_MOV, ARMOP_BIC, ARMOP_MVN,
	ARMOP_LDR, ARMOP_STR, ARMOP_B, ARMOP_BL, ARMOP_MSR, ARMOP_MRS, ARMOP_FALLBACK
} ArmOpType;

typedef enum ArmCondition
{
	ARMCC_EQ, ARMCC_NE, ARMCC_CS, ARMCC_CC, ARMCC_MI, ARMCC_PL, ARMCC_VS, ARMCC_VC,
	ARMCC_HI, ARMCC_LS, ARMCC_GE, ARMCC_LT, ARMCC_GT, ARMCC_LE, ARMCC_AL, ARMCC_UC
} ArmCondition;

typedef enum ArmShiftOp
{
	ARMSH_LSL, ARMSH_LSR, ARMSH_ASR, ARMSH_ROR, ARMSH_RRX = ARMSH_ROR
} ArmShiftOp;

#define ARMOP_READS_FLAGS 1
#define ARMOP_SETS_FLAGS  4
#define ARMOP_SETS_PC     8

typedef struct ArmOpRegister
{
	Arm7Reg armreg;
	int version;
} ArmOpRegister;

/* An operand: none, a register or a constant, and for the flexible operand
 * the shift it goes through, by a constant or by a register */
enum { ARMOPND_NONE, ARMOPND_REG, ARMOPND_IMM };
typedef struct ArmOpOperand
{
	int type;
	union
	{
		u32 imm_value;
		ArmOpRegister reg_value;
	} v;
	ArmShiftOp shift_type;
	bool shift_imm;
	union
	{
		u32 shift_value;
		ArmOpRegister shift_reg;
	} s;
} ArmOpOperand;

typedef struct ArmOp
{
	ArmOpType op_type;
	ArmOpOperand rd;
	ArmOpOperand arg[3];
	/* For LDR/STR */
	bool pre_index;
	bool add_offset;
	bool byte_xfer;
	bool write_back;
	/* A load that takes its word from the address rounded down to a word,
	 * as a block load does, where a plain load of a word turns what it reads
	 * by the address's low two bits */
	bool aligned;
	ArmCondition condition;
	u8 flags;
	u8 cycles;
	bool spsr;
	/* For MSR */
	u8 psrMask;
} ArmOp;

/* What a default operand, or one made of a constant or a register, is */
static INLINE ArmOpOperand armop_none(void)
{
	ArmOpOperand o;
	memset(&o, 0, sizeof(o));
	o.type = ARMOPND_NONE;
	o.shift_type = ARMSH_LSL;
	o.shift_imm = true;
	return o;
}

static INLINE ArmOpOperand armop_imm(u32 v)
{
	ArmOpOperand o = armop_none();
	o.type = ARMOPND_IMM;
	o.v.imm_value = v;
	return o;
}

static INLINE ArmOpOperand armop_reg(Arm7Reg r)
{
	ArmOpOperand o = armop_none();
	o.type = ARMOPND_REG;
	o.v.reg_value.armreg = r;
	o.v.reg_value.version = 0;
	return o;
}

static INLINE ArmOpRegister armop_register(Arm7Reg r)
{
	ArmOpRegister reg;
	reg.armreg = r;
	reg.version = 0;
	return reg;
}

static INLINE bool armop_is_imm(const ArmOpOperand *o) { return o->type == ARMOPND_IMM; }
static INLINE bool armop_is_reg(const ArmOpOperand *o) { return o->type == ARMOPND_REG; }
static INLINE bool armop_is_none(const ArmOpOperand *o) { return o->type == ARMOPND_NONE; }

static INLINE u32 armop_get_imm(const ArmOpOperand *o)
{
	verify(armop_is_imm(o));
	return o->v.imm_value;
}

static INLINE void armop_set_imm(ArmOpOperand *o, u32 v)
{
	o->type = ARMOPND_IMM;
	o->v.imm_value = v;
}

static INLINE ArmOpRegister *armop_get_reg(ArmOpOperand *o)
{
	verify(armop_is_reg(o));
	return &o->v.reg_value;
}

static INLINE const ArmOpRegister *armop_reg_of(const ArmOpOperand *o)
{
	verify(armop_is_reg(o));
	return &o->v.reg_value;
}

static INLINE void armop_set_reg(ArmOpOperand *o, Arm7Reg armreg, int version)
{
	o->type = ARMOPND_REG;
	o->v.reg_value.armreg = armreg;
	o->v.reg_value.version = version;
}

static INLINE bool armop_is_shifted(const ArmOpOperand *o)
{
	return !o->shift_imm || o->s.shift_value != 0 || o->shift_type != ARMSH_LSL;
}

/* An instruction of @type: under @condition it reads the flags */
static INLINE ArmOp armop_make(ArmOpType type, ArmCondition condition)
{
	ArmOp op;
	int i;
	memset(&op, 0, sizeof(op));
	op.op_type = type;
	op.condition = condition;
	op.rd = armop_none();
	for (i = 0; i < 3; i++)
		op.arg[i] = armop_none();
	op.cycles = 6;
	if (condition != ARMCC_AL)
		op.flags |= ARMOP_READS_FLAGS;
	return op;
}

static INLINE bool armop_is_logical(const ArmOp *op)
{
	return op->op_type == ARMOP_AND || op->op_type == ARMOP_EOR || op->op_type == ARMOP_TST
			|| op->op_type == ARMOP_TEQ || (op->op_type >= ARMOP_ORR && op->op_type <= ARMOP_MVN);
}

static INLINE bool armop_is_comp(const ArmOp *op)
{
	return op->op_type >= ARMOP_TST && op->op_type <= ARMOP_CMN;
}

/* The register allocator: the ARM's registers a block's instructions use
 * given host registers (by number, 0 to max_regs - 1, which the backend
 * maps to its own), loaded and stored by the backend's functions */
typedef struct ArmRegAllocSlot
{
	int host_reg;
	u16 version;
	bool dirty;
	bool temporary;
} ArmRegAllocSlot;

typedef struct ArmRegAlloc
{
	ArmRegAllocSlot allocs[RN_ARM_REG_SLOTS];
	/* the free host registers, the one given next last */
	int host_regs[16];
	int host_reg_count;
	const ArmOp *block_ops;
	u32 block_op_count;
	void (*load_reg)(void *ctx, int host_reg, Arm7Reg armreg);
	void (*store_reg)(void *ctx, int host_reg, Arm7Reg armreg);
	void *ctx;
} ArmRegAlloc;

void arm_regalloc_init(ArmRegAlloc *ra, int max_regs, const ArmOp *block_ops, u32 count,
		void (*load_reg)(void *ctx, int host_reg, Arm7Reg armreg),
		void (*store_reg)(void *ctx, int host_reg, Arm7Reg armreg), void *ctx);
/* The registers of instruction @opidx in host registers */
void arm_regalloc_load(ArmRegAlloc *ra, u32 opidx);
/* ... and what it wrote written back where it has to be */
void arm_regalloc_store(ArmRegAlloc *ra, u32 opidx);
/* Writes back every register whose latest value is only in a host
 * register. For the backend to call before code that may be skipped: a
 * conditional instruction that runs short of host registers pushes one
 * out, and if the one pushed out still had to be written back, the
 * write would be in the skipped code - written when the condition
 * holds and lost when it does not, with nothing to tell which. */
void arm_regalloc_store_dirty(ArmRegAlloc *ra);
static INLINE int arm_regalloc_map(const ArmRegAlloc *ra, Arm7Reg r)
{
	return ra->allocs[(int)r].host_reg;
}

/* The code buffer */
#define ARM7_ICACHE_SIZE (4 * 1024 * 1024)
/* Where the code for each address of the ARM's memory is; the compiler's
 * own entry where there is none yet */
extern void (*arm7_entry_points[ARAM_SIZE_MAX / 4])(void);
extern u8 *arm7_ic_ptr;
extern u8 *arm7_icache;
extern ptrdiff_t arm7_rx_offset;

static INLINE void *arm7_current_code(void) { return arm7_ic_ptr; }
static INLINE u32 arm7_space_left(void) { return ARM7_ICACHE_SIZE - (u32)(arm7_ic_ptr - arm7_icache); }
static INLINE bool arm7_code_empty(void) { return arm7_ic_ptr == arm7_icache; }
static INLINE void arm7_advance(u32 size) { arm7_ic_ptr += size; }
static INLINE void *arm7_write_to_exec(void *addr) { return (char *)addr + arm7_rx_offset; }
static INLINE void *arm7_exec_to_write(void *addr) { return (char *)addr - arm7_rx_offset; }

/* Has the block at the ARM's program counter compiled (the code's own
 * "compile this") */
void arm7_rec_compile(void);
/* The memory access a backend calls for what it does not do in line */
void *arm7_rec_mem_op(bool load, bool byte);

/* The backend's: a block's code made, and the main loop made again */
void arm7backend_compile(const ArmOp *block_ops, u32 count, u32 cycles);
void arm7backend_flush(void);

extern void (*arm_compilecode)(void);
typedef void (*arm_mainloop_t)(reg_pair *arm_regs, void (*entrypoints[])(void));
extern arm_mainloop_t arm_mainloop;

#ifdef __cplusplus
}
#endif

#endif
