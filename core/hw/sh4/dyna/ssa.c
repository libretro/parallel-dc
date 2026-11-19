/*
	Created on: Jun 5, 2019

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
#include <string.h>
#include <math.h>
#include "types.h"
#include "blockmanager.h"
#include "ssa.h"
#include "decoder.h"
#include "rend/grow_buf.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/sh4/sh4_fmac.h"
#define SHIL_MODE 2
#include "shil_canonical.h"

/* A map of (register, version) to a value, the keys in order, in memory
 * kept from one block compiled to the next: a block has a few hundred
 * operations at most, so a sorted array, searched by halves. A key is the
 * register (signed, as the pair it was compared as) and the version. */
typedef struct
{
	u64 key;
	u64 value;
} reg_value;

typedef struct
{
	grow_buf_t buf;
	u32 count;
} reg_value_map;

/* The SH4's thread compiles one block at a time: the maps are its own */
static reg_value_map writeback_values;	/* (a set) */
static reg_value_map constprop_values;	/* (reg num, version) -> value */
static reg_value_map dead_uses;			/* (a set: the values used) */
static reg_value_map reg_aliases;		/* (dest reg, version) -> (source reg, version), as keys */

static u64 rv_key(u32 reg, u32 version)
{
	return ((u64)(reg ^ 0x80000000u) << 32) | version;
}

static Sh4RegType rv_reg(u64 key)
{
	return (Sh4RegType)(s32)((u32)(key >> 32) ^ 0x80000000u);
}

static u32 rv_version(u64 key)
{
	return (u32)key;
}

/* the value of @param's @index'th register, as a key */
static u64 rv_param(const shil_param *param, int index)
{
	verify(shil_is_reg(param));
	verify(index >= 0 && index < (int)shil_count(param));
	return rv_key(param->_imm + index, param->version[index]);
}

static reg_value *rv_entries(const reg_value_map *m)
{
	return (reg_value *)m->buf.data;
}

static u32 rv_lower_bound(const reg_value_map *m, u64 key)
{
	const reg_value *e = rv_entries(m);
	u32 lo = 0, hi = m->count;
	while (lo < hi)
	{
		const u32 mid = (lo + hi) / 2;
		if (e[mid].key < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* where @key is, or -1 */
static int rv_find(const reg_value_map *m, u64 key)
{
	const u32 i = rv_lower_bound(m, key);
	return i < m->count && rv_entries(m)[i].key == key ? (int)i : -1;
}

/* @key's value, made (0) if it had none */
static u64 *rv_set(reg_value_map *m, u64 key)
{
	const u32 i = rv_lower_bound(m, key);
	reg_value *e;
	if (i < m->count && rv_entries(m)[i].key == key)
		return &rv_entries(m)[i].value;
	e = (reg_value *)grow_buf_reserve_keep(&m->buf, (m->count + 1) * sizeof(reg_value));
	if (e == NULL)
		die("Out of memory");
	memmove(&e[i + 1], &e[i], (m->count - i) * sizeof(reg_value));
	e[i].key = key;
	e[i].value = 0;
	m->count++;
	return &e[i].value;
}

/* those of a register @drop says to, gone; the others in their order */
static void rv_erase_regs(reg_value_map *m, bool (*drop)(Sh4RegType reg))
{
	reg_value *e = rv_entries(m);
	u32 kept = 0, i;
	for (i = 0; i < m->count; i++)
		if (!drop(rv_reg(e[i].key)))
			e[kept++] = e[i];
	m->count = kept;
}

static bool sync_sr_drops(Sh4RegType reg)
{
	return reg == reg_sr_status || reg == reg_old_sr_status || (reg >= reg_r0 && reg <= reg_r7)
			|| (reg >= reg_r0_Bank && reg <= reg_r7_Bank);
}

static bool div1_drops(Sh4RegType reg)
{
	return reg == reg_sr_status;
}

static bool sync_fpscr_drops(Sh4RegType reg)
{
	return reg == reg_fpscr || reg == reg_old_fpscr || (reg >= reg_fr_0 && reg <= reg_xf_15);
}

typedef struct
{
	RuntimeBlockInfo *block;
	int opnum;
	u32 reg_versions[sh4_reg_count];
	struct
	{
		u32 prop_constants;
		u32 constant_ops_replaced;
		u32 dead_code_ops;
		u32 dead_registers;
		u32 dyn_to_stat_blocks;
		u32 waw_blocks;
		u32 combined_shifts;
	} stats;
} ssa_state;

static void add_version_to_operand(u32 *reg_versions, shil_param *param, bool define)
{
	u32 i;
	if (!shil_is_reg(param))
		return;
	if (define)
		for (i = 0; i < shil_count(param); i++)
			reg_versions[param->_imm + i]++;
	for (i = 0; i < shil_count(param); i++)
		param->version[i] = (u16)reg_versions[param->_imm + i];
}

void ssa_add_versions(RuntimeBlockInfo *block)
{
	static u32 reg_versions[sh4_reg_count];
	u32 i;

	memset(reg_versions, 0, sizeof(reg_versions));
	for (i = 0; i < block->op_count; i++)
	{
		shil_opcode *op = &block->oplist[i];
		add_version_to_operand(reg_versions, &op->rs1, false);
		add_version_to_operand(reg_versions, &op->rs2, false);
		add_version_to_operand(reg_versions, &op->rs3, false);
		add_version_to_operand(reg_versions, &op->rd, true);
		add_version_to_operand(reg_versions, &op->rd2, true);
	}
}

#if DEBUG
static void print_block(const RuntimeBlockInfo *block)
{
	char buf[256];
	u32 i;
	for (i = 0; i < block->op_count; i++)
	{
		shil_disasm(buf, sizeof(buf), &block->oplist[i]);
		INFO_LOG(DYNAREC, "%08x  %s\n", block->vaddr + block->oplist[i].guest_offs, buf);
	}
}
#endif

/* mov rd, #v */
static void replace_by_mov32_imm(ssa_state *s, shil_opcode *op, u32 v)
{
	verify(shil_is_null(&op->rd2));
	op->op = shop_mov32;
	op->rs1 = shil_param_imm(v);
	op->rs2.type = FMT_NULL;
	op->rs3.type = FMT_NULL;
	s->stats.constant_ops_replaced++;
}

/* mov rd, rs1 */
static void replace_by_mov32(shil_opcode *op)
{
	verify(shil_is_null(&op->rd2));
	op->op = shop_mov32;
	op->rs2.type = FMT_NULL;
	op->rs3.type = FMT_NULL;
}

/* mov rd, rs, after the operation at s->opnum */
static void insert_mov32(ssa_state *s, const shil_param *rd, const shil_param *rs)
{
	shil_opcode op2;
	memcpy(&op2, &s->block->oplist[s->opnum], sizeof(op2));
	op2.op = shop_mov32;
	op2.rd = *rd;
	op2.rd2 = shil_param_none();
	op2.rs1 = *rs;
	op2.rs2 = shil_param_none();
	op2.rs3 = shil_param_none();

	bm_OpInsert(s->block, s->opnum + 1, &op2);
	s->opnum++;
}

/* @op's @which'th result, a constant: a move of it after @op, @op left
 * pointing where it went */
static shil_opcode *insert_const_result(ssa_state *s, shil_opcode *op, Sh4RegType reg, u32 version, u32 v)
{
	shil_param rd = shil_param_reg(reg);
	shil_param imm = shil_param_imm(v);
	rd.version[0] = (u16)version;
	insert_mov32(s, &rd, &imm);
	(void)op;
	/* the insert may have moved the operations */
	return &s->block->oplist[s->opnum - 1];
}

static void const_prop_operand(ssa_state *s, shil_param *param)
{
	if (shil_is_r32(param))
	{
		const int it = rv_find(&constprop_values, rv_param(param, 0));
		if (it >= 0)
		{
			param->type = FMT_IMM;
			param->_imm = (u32)rv_entries(&constprop_values)[it].value;
			s->stats.prop_constants++;
		}
	}
}

static f32 as_f32(u32 v)
{
	f32 f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

static u32 as_u32(f32 f)
{
	u32 v;
	memcpy(&v, &f, sizeof(v));
	return v;
}

/* Only immediate operands: the operation done here and now */
static bool execute_const_op(ssa_state *s, shil_opcode *op)
{
	RuntimeBlockInfo *block = s->block;
	u32 rs1, rs2, rs3, rd, rd2;

	if (shil_is_reg(&op->rs1) || shil_is_reg(&op->rs2) || shil_is_reg(&op->rs3)
			|| !(shil_is_imm(&op->rs1) || shil_is_imm(&op->rs2) || shil_is_imm(&op->rs3))
			|| !shil_is_reg(&op->rd))
		return false;

	rs1 = shil_is_imm(&op->rs1) ? shil_imm_value(&op->rs1) : 0;
	rs2 = shil_is_imm(&op->rs2) ? shil_imm_value(&op->rs2) : 0;
	rs3 = shil_is_imm(&op->rs3) ? shil_imm_value(&op->rs3) : 0;

	switch (op->op)
	{
	case shop_mov32:
		rd = rs1;
		break;
	case shop_add:
		rd = SHIL_FN(add,f1)(rs1, rs2);
		break;
	case shop_sub:
		rd = SHIL_FN(sub,f1)(rs1, rs2);
		break;
	case shop_adc:
	case shop_sbc:
	case shop_negc:
	case shop_rocl:
	case shop_rocr:
	case shop_mul_u64:
	case shop_mul_s64:
		{
			u64 v;
			if (op->op == shop_adc)
				v = SHIL_FN(adc,f1)(rs1, rs2, rs3);
			else if (op->op == shop_sbc)
				v = SHIL_FN(sbc,f1)(rs1, rs2, rs3);
			else if (op->op == shop_rocl)
				v = SHIL_FN(rocl,f1)(rs1, rs2);
			else if (op->op == shop_rocr)
				v = SHIL_FN(rocr,f1)(rs1, rs2);
			else if (op->op == shop_negc)
				v = SHIL_FN(negc,f1)(rs1, rs2);
			else if (op->op == shop_mul_u64)
				v = SHIL_FN(mul_u64,f1)(rs1, rs2);
			else
				v = SHIL_FN(mul_s64,f1)(rs1, rs2);
			rd = (u32)v;
			rd2 = (u32)(v >> 32);
			op = insert_const_result(s, op, shil_reg(&op->rd2), op->rd2.version[0], rd2);
			op->rd2 = shil_param_none();
		}
		break;
	case shop_shl:
		rd = SHIL_FN(shl,f1)(rs1, rs2);
		break;
	case shop_shr:
		rd = SHIL_FN(shr,f1)(rs1, rs2);
		break;
	case shop_sar:
		rd = SHIL_FN(sar,f1)(rs1, rs2);
		break;
	case shop_ror:
		rd = SHIL_FN(ror,f1)(rs1, rs2);
		break;
	case shop_shld:
		rd = SHIL_FN(shld,f1)(rs1, rs2);
		break;
	case shop_shad:
		rd = SHIL_FN(shad,f1)(rs1, rs2);
		break;
	case shop_or:
		rd = SHIL_FN(or,f1)(rs1, rs2);
		break;
	case shop_and:
		rd = SHIL_FN(and,f1)(rs1, rs2);
		break;
	case shop_xor:
		rd = SHIL_FN(xor,f1)(rs1, rs2);
		break;
	case shop_not:
		rd = SHIL_FN(not,f1)(rs1);
		break;
	case shop_ext_s16:
		rd = SHIL_FN(ext_s16,f1)(rs1);
		break;
	case shop_ext_s8:
		rd = SHIL_FN(ext_s8,f1)(rs1);
		break;
	case shop_mul_i32:
		rd = SHIL_FN(mul_i32,f1)(rs1, rs2);
		break;
	case shop_mul_u16:
		rd = SHIL_FN(mul_u16,f1)(rs1, rs2);
		break;
	case shop_mul_s16:
		rd = SHIL_FN(mul_s16,f1)(rs1, rs2);
		break;
	case shop_test:
		rd = SHIL_FN(test,f1)(rs1, rs2);
		break;
	case shop_neg:
		rd = SHIL_FN(neg,f1)(rs1);
		break;
	case shop_swaplb:
		rd = SHIL_FN(swaplb,f1)(rs1);
		break;
	case shop_swap:
		rd = SHIL_FN(swap,f1)(rs1);
		break;
	case shop_seteq:
		rd = SHIL_FN(seteq,f1)(rs1, rs2);
		break;
	case shop_setgt:
		rd = SHIL_FN(setgt,f1)(rs1, rs2);
		break;
	case shop_setge:
		rd = SHIL_FN(setge,f1)(rs1, rs2);
		break;
	case shop_setab:
		rd = SHIL_FN(setab,f1)(rs1, rs2);
		break;
	case shop_setae:
		rd = SHIL_FN(setae,f1)(rs1, rs2);
		break;
	case shop_setpeq:
		rd = SHIL_FN(setpeq,f1)(rs1, rs2);
		break;
	case shop_xtrct:
		rd = SHIL_FN(xtrct,f1)(rs1, rs2);
		break;

	case shop_div32u:
	case shop_div32s:
		{
			/* the quotient to rd, the remainder to rd2 (rs3's register) */
			const u64 res = op->op == shop_div32u ? SHIL_FN(div32u,f1)(rs1, rs2, rs3) : SHIL_FN(div32s,f1)(rs1, rs2, rs3);
			rd = (u32)res;
			rd2 = (u32)(res >> 32);
			*rv_set(&constprop_values, rv_param(&op->rd2, 0)) = rd2;
			op = insert_const_result(s, op, shil_reg(&op->rd2), op->rd2.version[0], rd2);
			op->rd2 = shil_param_none();
		}
		break;
	case shop_div32p2:
		rd = SHIL_FN(div32p2,f1)(rs1, rs2, rs3);
		break;

	case shop_jdyn:
		verify(BET_GET_CLS(block->BlockType) == BET_CLS_Dynamic);
		rs1 += rs2;
		switch ((block->BlockType >> 1) & 3)
		{
		case BET_SCL_Jump:
		case BET_SCL_Ret:
			block->BlockType = BET_StaticJump;
			block->BranchBlock = rs1;
			break;
		case BET_SCL_Call:
			block->BlockType = BET_StaticCall;
			block->BranchBlock = rs1;
			break;
		case BET_SCL_Intr:
			block->BlockType = BET_StaticIntr;
			block->NextBlock = rs1;
			break;
		default:
			die("Unexpected block end type\n");
		}
		/* rd (that is jdyn) won't be updated but since it's not a real
		 * register it shouldn't be a problem */
		bm_OpErase(block, s->opnum);
		s->opnum--;
		s->stats.dyn_to_stat_blocks++;
		return true;

	case shop_jcond:
		if (rs1 != (block->BlockType & 1))
			block->BranchBlock = block->NextBlock;
		block->BlockType = BET_StaticJump;
		block->NextBlock = 0xFFFFFFFF;
		block->has_jcond = false;
		/* same remark regarding jdyn as in the previous case */
		bm_OpErase(block, s->opnum);
		s->opnum--;
		s->stats.dyn_to_stat_blocks++;
		return true;

	case shop_fneg:
		rd = as_u32(SHIL_FN(fneg,f1)(as_f32(rs1)));
		break;
	case shop_fadd:
		rd = as_u32(SHIL_FN(fadd,f1)(as_f32(rs1), as_f32(rs2)));
		break;
	case shop_fsub:
		rd = as_u32(SHIL_FN(fsub,f1)(as_f32(rs1), as_f32(rs2)));
		break;
	case shop_fmul:
		rd = as_u32(SHIL_FN(fmul,f1)(as_f32(rs1), as_f32(rs2)));
		break;
	case shop_fdiv:
		rd = as_u32(SHIL_FN(fdiv,f1)(as_f32(rs1), as_f32(rs2)));
		break;
	case shop_cvt_i2f_n:
		rd = as_u32(SHIL_FN(cvt_i2f_n,f1)(rs1));
		break;
	case shop_cvt_i2f_z:
		rd = as_u32(SHIL_FN(cvt_i2f_z,f1)(rs1));
		break;
	case shop_fsqrt:
		rd = as_u32(SHIL_FN(fsqrt,f1)(as_f32(rs1)));
		break;
	case shop_cvt_f2i_t:
		rd = SHIL_FN(cvt_f2i_t,f1)(as_f32(rs1));
		break;
	case shop_fsrra:
		rd = as_u32(SHIL_FN(fsrra,f1)(as_f32(rs1)));
		break;
	case shop_fsca:
		{
			f32 tmp[2];
			u32 rd_1;
			SHIL_FN(fsca,fsca_table)(tmp, rs1);
			rd = as_u32(tmp[0]);
			rd_1 = as_u32(tmp[1]);
			*rv_set(&constprop_values, rv_param(&op->rd, 1)) = rd_1;
			op = insert_const_result(s, op, (Sh4RegType)(op->rd._imm + 1), op->rd.version[1], rd_1);
			op->rd.type = FMT_F32;
		}
		break;
	case shop_fabs:
		rd = as_u32(SHIL_FN(fabs,f1)(as_f32(rs1)));
		break;
	case shop_fmac:
		rd = as_u32(SHIL_FN(fmac,f1)(as_f32(rs1), as_f32(rs2), as_f32(rs3)));
		break;
	case shop_fseteq:
		rd = SHIL_FN(fseteq,f1)(as_f32(rs1), as_f32(rs2));
		break;
	case shop_fsetgt:
		rd = SHIL_FN(fsetgt,f1)(as_f32(rs1), as_f32(rs2));
		break;

	default:
		ERROR_LOG(DYNAREC, "unhandled constant op %d\n", op->op);
		die("unhandled constant op");
		rd = 0;
		break;
	}

	/* (a second result has been moved to a mov32 of its own) */
	*rv_set(&constprop_values, rv_param(&op->rd, 0)) = rd;
	replace_by_mov32_imm(s, op, rd);
	return true;
}

/* A read of the block's own pages, read-only: the value is known now */
static void const_read(ssa_state *s, shil_opcode *op)
{
	const RuntimeBlockInfo *block = s->block;
	bool doit = false;
	u32 v;

	if (!(shil_is_imm(&op->rs1) && op->op == shop_readm && block->read_only
			&& (op->rs1._imm >> 12) >= (block->vaddr >> 12)
			&& (op->rs1._imm >> 12) <= ((block->vaddr + block->sh4_code_size - 1) >> 12)
			&& (op->flags & 0x7f) <= 4))
		return;
	if (mmu_enabled())
	{
#ifndef FAST_MMU
		/* It is possible to get a tlb miss on data if the page is only in
		 * the itlb table. It won't happen with the fast mmu implementation
		 * though. */
		u32 paddr;
		doit = mmu_data_translation_r8(op->rs1._imm, &paddr) == MMU_ERROR_NONE;
#else
		doit = true;
#endif
		/* Only for user space addresses, and kernel RAM addresses */
		if (doit && (op->rs1._imm & 0x80000000) != 0)
			doit = IsOnRam(op->rs1._imm);
	}
	else
		doit = IsOnRam(op->rs1._imm);
	if (!doit)
		return;
	switch (op->flags & 0x7f)
	{
	case 1:
		v = (u32)(s32)(s8)ReadMem8(op->rs1._imm);
		break;
	case 2:
		v = (u32)(s32)(s16)ReadMem16(op->rs1._imm);
		break;
	case 4:
		v = ReadMem32(op->rs1._imm);
		break;
	default:
		die("invalid size");
		v = 0;
		break;
	}
	replace_by_mov32_imm(s, op, v);
	*rv_set(&constprop_values, rv_param(&op->rd, 0)) = v;
}

static void const_prop_pass(ssa_state *s)
{
	RuntimeBlockInfo *block = s->block;
	for (s->opnum = 0; s->opnum < (int)block->op_count; s->opnum++)
	{
		shil_opcode *op = &block->oplist[s->opnum];
		const shilop o = op->op;

		/* TODO do shop_sub and others */
		if (o != shop_setab && o != shop_setae && o != shop_setgt && o != shop_setge && o != shop_sub && o != shop_fsetgt
				&& o != shop_fseteq && o != shop_fdiv && o != shop_fsub && o != shop_fmac)
			const_prop_operand(s, &op->rs1);
		if (o != shop_rocr && o != shop_rocl && o != shop_fsetgt && o != shop_fseteq && o != shop_fmac)
			const_prop_operand(s, &op->rs2);
		if (o != shop_fmac && o != shop_adc)
			const_prop_operand(s, &op->rs3);

		if (o == shop_ifb)
			constprop_values.count = 0;
		else if (o == shop_sync_sr)
			rv_erase_regs(&constprop_values, sync_sr_drops);
		else if (o == shop_div1)
			/* it changes the status register: what was known of it is not any more */
			rv_erase_regs(&constprop_values, div1_drops);
		else if (o == shop_sync_fpscr)
			rv_erase_regs(&constprop_values, sync_fpscr_drops);
		else if (o == shop_readm || o == shop_writem)
		{
			if (shil_is_imm(&op->rs1))
			{
				if (shil_is_imm(&op->rs3))
				{
					/* Merge base addr and offset */
					op->rs1._imm += shil_imm_value(&op->rs3);
					op->rs3.type = FMT_NULL;
				}
				else if (shil_is_reg(&op->rs3))
				{
					/* Swap rs1 and rs3 so that rs1 is never an immediate operand */
					shil_param t = op->rs1;
					op->rs1 = op->rs3;
					op->rs3 = t;
				}
				/* If we know the address to read and it's in the same
				 * memory page(s) as the block and if those pages are
				 * read-only, then we can directly read the memory at
				 * compile time and propagate the read value as a constant. */
				const_read(s, op);
			}
		}
		else if (execute_const_op(s, op))
		{
		}
		else if (o == shop_and || o == shop_or || o == shop_xor || o == shop_add || o == shop_mul_s16 || o == shop_mul_u16
				|| o == shop_mul_i32 || o == shop_test || o == shop_seteq || o == shop_fseteq || o == shop_fadd || o == shop_fmul
				|| o == shop_mul_u64 || o == shop_mul_s64 || o == shop_adc || o == shop_setpeq)
		{
			if (shil_is_imm(&op->rs1) && shil_is_reg(&op->rs2))
			{
				/* Swap rs1 and rs2 so that rs1 is never an immediate operand */
				shil_param t = op->rs1;
				op->rs1 = op->rs2;
				op->rs2 = t;
			}
		}
		else if ((o == shop_shld || o == shop_shad) && shil_is_imm(&op->rs2))
		{
			/* Replace shld/shad with shl/shr/sar */
			const u32 r2 = shil_imm_value(&op->rs2);
			if ((r2 & 0x80000000) == 0)
			{
				/* rd = r1 << (r2 & 0x1F) */
				op->op = shop_shl;
				op->rs2._imm = r2 & 0x1F;
				s->stats.constant_ops_replaced++;
			}
			else if ((r2 & 0x1F) == 0)
			{
				/* To the right by 32: a logical shift leaves 0, an
				 * arithmetic one the sign in every bit */
				if (o == shop_shld)
					replace_by_mov32_imm(s, op, 0);
				else
				{
					op->op = shop_sar;
					op->rs2._imm = 31;
					s->stats.constant_ops_replaced++;
				}
			}
			else
			{
				/* rd = r1 >> ((~r2 & 0x1F) + 1) */
				op->op = o == shop_shad ? shop_sar : shop_shr;
				op->rs2._imm = (~r2 & 0x1F) + 1;
				s->stats.constant_ops_replaced++;
			}
		}
	}
}

/* @param's registers that are written for the last time here kept in
 * @last_versions; whether none of them is used again */
static bool dest_unused(u32 *last_versions, const shil_param *param)
{
	bool unused_rd = true;
	u32 i;
	for (i = 0; i < shil_count(param); i++)
	{
		const u32 reg = param->_imm + i;
		if (last_versions[reg] == (u32)-1)
		{
			last_versions[reg] = param->version[i];
			unused_rd = false;
			rv_set(&writeback_values, rv_param(param, (int)i));
		}
		else
		{
			verify(param->version[i] < last_versions[reg]);
			if (rv_find(&dead_uses, rv_param(param, (int)i)) >= 0)
				unused_rd = false;
		}
	}
	return unused_rd;
}

static void mark_used(const shil_param *param)
{
	u32 i;
	if (shil_is_reg(param))
		for (i = 0; i < shil_count(param); i++)
			rv_set(&dead_uses, rv_param(param, (int)i));
}

static void dead_code_removal_pass(ssa_state *s)
{
	RuntimeBlockInfo *block = s->block;
	u32 last_versions[sh4_reg_count];
	int opnum, i;

	dead_uses.count = 0;
	memset(last_versions, -1, sizeof(last_versions));
	for (opnum = (int)block->op_count - 1; opnum >= 0; opnum--)
	{
		shil_opcode *op = &block->oplist[opnum];
		bool dead_code = false;

		if (op->op == shop_ifb || (mmu_enabled() && (op->op == shop_readm || op->op == shop_writem)))
		{
			/* if mmu enabled, mem accesses can throw an exception so
			 * last_versions must be reset so the regs are correctly saved
			 * beforehand */
			memset(last_versions, -1, sizeof(last_versions));
			continue;
		}
		if (op->op == shop_pref)
		{
			if (shil_is_imm(&op->rs1) && (shil_imm_value(&op->rs1) & 0xFC000000) != 0xE0000000)
				dead_code = true;
			else if (mmu_enabled())
			{
				memset(last_versions, -1, sizeof(last_versions));
				continue;
			}
		}
		if (op->op == shop_sync_sr)
		{
			last_versions[reg_sr_T] = (u32)-1;
			last_versions[reg_sr_status] = (u32)-1;
			last_versions[reg_old_sr_status] = (u32)-1;
			for (i = reg_r0; i <= reg_r7; i++)
				last_versions[i] = (u32)-1;
			for (i = reg_r0_Bank; i <= reg_r7_Bank; i++)
				last_versions[i] = (u32)-1;
			continue;
		}
		if (op->op == shop_div1)
			last_versions[reg_sr_status] = (u32)-1;	/* read by it: an earlier write is not dead */

		if (op->op == shop_sync_fpscr)
		{
			last_versions[reg_fpscr] = (u32)-1;
			last_versions[reg_old_fpscr] = (u32)-1;
			for (i = reg_fr_0; i <= reg_xf_15; i++)
				last_versions[i] = (u32)-1;
			continue;
		}

		if (shil_is_reg(&op->rd))
			dead_code = dest_unused(last_versions, &op->rd) || dead_code;
		if (shil_is_reg(&op->rd2))
			dead_code = dest_unused(last_versions, &op->rd2) && dead_code;
		if (dead_code && op->op != shop_readm)	/* memory read on registers can have side effects */
		{
			bm_OpErase(block, (u32)opnum);
			s->stats.dead_code_ops++;
		}
		else
		{
			mark_used(&op->rs1);
			mark_used(&op->rs2);
			mark_used(&op->rs3);
		}
	}
}

static void simplify_expression_pass(ssa_state *s)
{
	RuntimeBlockInfo *block = s->block;
	u32 opnum;
	for (opnum = 0; opnum < block->op_count; opnum++)
	{
		shil_opcode *op = &block->oplist[opnum];
		const shilop o = op->op;
		if (shil_is_imm(&op->rs2))
		{
			if (shil_imm_value(&op->rs2) == 0)
			{
				/* a & 0 == 0, a * 0 == 0 (not true for FPU ops because of
				 * Inf and NaN) */
				if (o == shop_and || o == shop_mul_i32 || o == shop_mul_s16 || o == shop_mul_u16)
					replace_by_mov32_imm(s, op, 0);
				/* a + 0, a - 0, a | 0, a ^ 0, a >> 0, a << 0 == a */
				else if (o == shop_add || o == shop_sub || o == shop_or || o == shop_xor
						|| o == shop_shl || o == shop_shr || o == shop_sar || o == shop_shad || o == shop_shld)
					replace_by_mov32(op);
			}
			/* a * 1 == a */
			else if (shil_imm_value(&op->rs2) == 1
					&& (o == shop_mul_i32 || o == shop_mul_s16 || o == shop_mul_u16))
				replace_by_mov32(op);
		}
		/* Not sure it's worth the trouble, except for the 'and' and 'xor' */
		else if (shil_is_r32i(&op->rs1) && op->rs1._imm == op->rs2._imm)
		{
			/* a + a == a * 2 == a << 1 (there's quite a few of these) */
			if (o == shop_add)
			{
				op->op = shop_shl;
				op->rs2 = shil_param_imm(1);
			}
			/* a ^ a == 0, a - a == 0 */
			else if (o == shop_xor || o == shop_sub)
				replace_by_mov32_imm(s, op, 0);
			/* SBC a, a == SBC 0,0 */
			else if (o == shop_sbc)
			{
				op->rs1 = shil_param_imm(0);
				op->rs2 = shil_param_imm(0);
				s->stats.prop_constants += 2;
			}
			/* a & a == a, a | a == a */
			else if (o == shop_and || o == shop_or)
				replace_by_mov32(op);
		}
	}
}

/* whether @param is @key's register, at a version later than (@higher),
 * or the same as, @key's */
static bool param_has_version(const shil_param *param, u64 key, bool higher)
{
	const Sh4RegType reg = rv_reg(key);
	u32 version;
	if (!shil_is_reg(param) || reg < (Sh4RegType)param->_imm
			|| reg >= (Sh4RegType)(param->_imm + shil_count(param)))
		return false;
	version = param->version[reg - (Sh4RegType)param->_imm];
	return higher ? version > rv_version(key) : version == rv_version(key);
}

static void replace_by_alias(shil_param *param, u64 from, u64 to)
{
	if (shil_is_r32(param) && (Sh4RegType)param->_imm == rv_reg(from))
	{
		verify(param->version[0] == rv_version(from));
		param->_imm = (u32)rv_reg(to);
		param->version[0] = (u16)rv_version(to);
	}
}

static void dead_register_pass(ssa_state *s)
{
	RuntimeBlockInfo *block = s->block;
	u32 a, opnum;

	/* Find aliases */
	reg_aliases.count = 0;
	for (opnum = 0; opnum < block->op_count; opnum++)
	{
		const shil_opcode *op = &block->oplist[opnum];
		/* ignore moves from/to int regs to/from fpu regs */
		if (op->op == shop_mov32 && shil_is_reg(&op->rs1) && shil_is_r32i(&op->rd) == shil_is_r32i(&op->rs1))
		{
			const u64 dest = rv_param(&op->rd, 0);
			const u64 src = rv_param(&op->rs1, 0);
			const int it = rv_find(&reg_aliases, src);
			/* use the final value if the src is itself aliased */
			const u64 final_src = it >= 0 ? rv_entries(&reg_aliases)[it].value : src;
			*rv_set(&reg_aliases, dest) = final_src;
		}
	}

	/* Attempt to eliminate them */
	for (a = 0; a < reg_aliases.count; a++)
	{
		const u64 alias_from = rv_entries(&reg_aliases)[a].key;
		const u64 alias_to = rv_entries(&reg_aliases)[a].value;
		size_t defnum = (size_t)-1, usenum = (size_t)-1, aliasdef = (size_t)-1, n;

		if (rv_find(&writeback_values, alias_from) >= 0)
			continue;

		/* Do a first pass to check that we can replace the value */
		for (opnum = 0; opnum < block->op_count; opnum++)
		{
			const shil_opcode *op = &block->oplist[opnum];
			/* find def */
			if (shil_is_r32(&op->rd) && rv_param(&op->rd, 0) == alias_from)
				defnum = opnum;
			else if (shil_is_r32(&op->rd2) && rv_param(&op->rd2, 0) == alias_from)
				defnum = opnum;

			/* find alias redef */
			if (aliasdef == (size_t)-1)
			{
				if (param_has_version(&op->rd, alias_to, true))
					aliasdef = opnum;
				else if (param_has_version(&op->rd2, alias_to, true))
					aliasdef = opnum;
				/* An instruction left to the interpreter can change any
				 * register, and none of them is down as written by it: the
				 * register this one is a copy of may not be what it was
				 * afterwards (upstream ec3ad9b32). */
				else if (op->op == shop_ifb)
					aliasdef = opnum;
			}

			/* find last use; values used by vectors can't be aliased, they
			 * need adjacent regs */
			if (param_has_version(&op->rs1, alias_from, false))
			{
				if (shil_count(&op->rs1) == 1)
					usenum = opnum;
				else
				{
					usenum = 0xFFFF;
					aliasdef = 0;
					break;
				}
			}
			else if (param_has_version(&op->rs2, alias_from, false))
			{
				if (shil_count(&op->rs2) == 1)
					usenum = opnum;
				else
				{
					usenum = 0xFFFF;
					aliasdef = 0;
					break;
				}
			}
			else if (param_has_version(&op->rs3, alias_from, false))
			{
				if (shil_count(&op->rs3) == 1)
					usenum = opnum;
				else
				{
					usenum = 0xFFFF;
					aliasdef = 0;
					break;
				}
			}
		}
		verify(defnum != (size_t)-1);
		/* If the alias is redefined before any use we can't use it */
		if (aliasdef != (size_t)-1 && usenum != (size_t)-1 && aliasdef < usenum)
			continue;

		for (n = defnum + 1; n <= usenum && usenum != (size_t)-1; n++)
		{
			shil_opcode *op = &block->oplist[n];
			replace_by_alias(&op->rs1, alias_from, alias_to);
			replace_by_alias(&op->rs2, alias_from, alias_to);
			replace_by_alias(&op->rs3, alias_from, alias_to);
		}
		s->stats.dead_registers++;
		bm_OpErase(block, (u32)defnum);
	}
}

static void identity_move_pass(ssa_state *s)
{
	RuntimeBlockInfo *block = s->block;
	int opnum;
	/* This pass creates holes in reg versions and should be run last. The
	 * versioning pass must be re-run if needed. */
	for (opnum = 0; opnum < (int)block->op_count; opnum++)
	{
		const shil_opcode *op = &block->oplist[opnum];
		if (op->op == shop_mov32 && shil_is_reg(&op->rs1) && op->rd._imm == op->rs1._imm)
		{
			bm_OpErase(block, (u32)opnum);
			opnum--;
			s->stats.dead_code_ops++;
		}
	}
}

static void combine_shifts_pass(ssa_state *s)
{
	RuntimeBlockInfo *block = s->block;
	int opnum;
	for (opnum = 0; opnum < (int)block->op_count - 1; opnum++)
	{
		shil_opcode *op = &block->oplist[opnum];
		shil_opcode *next_op = &block->oplist[opnum + 1];
		if (op->op == next_op->op && (op->op == shop_shl || op->op == shop_shr || op->op == shop_sar)
				&& shil_is_r32i(&next_op->rs1) && op->rd._imm == next_op->rs1._imm
				&& next_op->rs2._imm + op->rs2._imm <= 31)
		{
			next_op->rs2._imm += op->rs2._imm;
			replace_by_mov32(op);
			s->stats.combined_shifts++;
		}
	}
}

void AnalyseBlock(RuntimeBlockInfo *block)
{
	static ssa_state s;

	memset(&s, 0, sizeof(s));
	s.block = block;
	writeback_values.count = 0;
	constprop_values.count = 0;

	ssa_add_versions(block);
#if DEBUG
	INFO_LOG(DYNAREC, "BEFORE\n");
	print_block(block);
#endif
	const_prop_pass(&s);
	/* (A pass dropping the first of two writes to one address, for RAM
	 * and video memory only, is not worth the trouble) */
	dead_code_removal_pass(&s);
	simplify_expression_pass(&s);
	combine_shifts_pass(&s);
	dead_register_pass(&s);
	identity_move_pass(&s);

#if DEBUG
	if (s.stats.prop_constants > 0 || s.stats.dead_code_ops > 0 || s.stats.constant_ops_replaced > 0
			|| s.stats.dead_registers > 0 || s.stats.dyn_to_stat_blocks > 0 || s.stats.waw_blocks > 0 || s.stats.combined_shifts > 0)
		INFO_LOG(DYNAREC, "STATS: %08x ops %u constants %u constops replaced %u dead code %u dead regs %u dyn2stat blks %u waw %u shifts %u\n",
				block->vaddr, block->op_count, s.stats.prop_constants, s.stats.constant_ops_replaced,
				s.stats.dead_code_ops, s.stats.dead_registers, s.stats.dyn_to_stat_blocks, s.stats.waw_blocks, s.stats.combined_shifts);
#endif
}
