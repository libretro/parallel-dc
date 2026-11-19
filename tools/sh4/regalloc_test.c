/* The register allocator (core/hw/sh4/dyna/ssa_regalloc.c) over the
 * blocks the SSA optimizer leaves of the decoder's blocks of random code
 * and of random blocks of operations made up whole, with as many host
 * registers as each recompiler has and fewer, pairs in registers and not:
 * where each register is at each operation, and every load and write-back
 * asked for, hashed, against what the C++ allocator it replaced did
 * (sh tools/sh4/regalloc_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/dyna/decoder.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/shil.h"
#include "hw/sh4/dyna/ssa.h"
#include "hw/sh4/dyna/ssa_regalloc.h"
#include "hw/sh4/sh4_rom.h"

static u16 code[0x10000];
static u32 code_base = 0x8C010000;
static Sh4RCB rcb;
Sh4RCB *p_sh4rcb = &rcb;
RegisterStruct CCN[SH4_CCN_REGS];
bool mmu_strict;
settings_t settings;
static ngen_features features;
static u32 rng = 1;
static unsigned h = 2166136261u;
/* the opcodes the decoder does something of its own for */
static u16 handled[0x10000];
static u32 handled_count;

static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static u16 DYNACALL read16(u32 addr) { return code[((addr - code_base) / 2) & 0xffff]; }
ReadMem16Func IReadMem16 = read16;
bool Do_Exception(u32 epc, u32 expEvn, u32 CallVect) { (void)epc; (void)expEvn; (void)CallVect; return true; }
const char *bm_BlockHash(RuntimeBlockInfo *block) { (void)block; return "-"; }
void ngen_GetFeatures(ngen_features *dst) { *dst = features; }
void os_DebugBreak(void) { fprintf(stderr, "verify failed\n"); abort(); }

void bm_OpPush(RuntimeBlockInfo *block, const shil_opcode *op)
{
	if (block->op_count == block->op_room)
	{
		block->op_room = block->op_room ? block->op_room * 2 : 64;
		block->oplist = (shil_opcode *)realloc(block->oplist, block->op_room * sizeof(shil_opcode));
	}
	memcpy(&block->oplist[block->op_count++], op, sizeof(*op));
}



static u8 DYNACALL rd8(u32 a) { return (u8)(a * 2654435761u >> 7); }
static u16 DYNACALL rd16(u32 a) { return (u16)(a * 2654435761u >> 9); }
static u32 DYNACALL rd32(u32 a) { return a * 2654435761u ^ 0x5A5A1234; }
ReadMem8Func ReadMem8 = rd8;
ReadMem16Func ReadMem16 = rd16;
ReadMem32Func ReadMem32 = rd32;
f32_x2 sin_table[0x10000];
retro_log_printf_t log_cb;
u32 mmu_data_translation_r8(u32 va, u32 *rv) { *rv = va; return (va >> 12) & 1 ? MMU_ERROR_NONE : 1; }

void bm_OpInsert(RuntimeBlockInfo *block, u32 at, const shil_opcode *op)
{
	shil_opcode keep = *op;
	bm_OpPush(block, op);
	memmove(&block->oplist[at + 1], &block->oplist[at], (block->op_count - 1 - at) * sizeof(shil_opcode));
	block->oplist[at] = keep;
}
void bm_OpErase(RuntimeBlockInfo *block, u32 at)
{
	memmove(&block->oplist[at], &block->oplist[at + 1], (block->op_count - 1 - at) * sizeof(shil_opcode));
	block->op_count--;
}


/* FNV-1a */
static void mix(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		h ^= (v >> (i * 8)) & 0xFF;
		h *= 16777619u;
	}
}

static void mix_param(const shil_param *p)
{
	int i;
	mix(p->type);
	mix(p->_imm);
	for (i = 0; i < 16; i++)
		mix(p->version[i]);
}

static void mix_block(const RuntimeBlockInfo *b)
{
	u32 i;
	mix(b->op_count);
	mix(b->NextBlock);
	mix(b->BranchBlock);
	mix((u32)b->BlockType);
	mix(b->has_jcond);
	for (i = 0; i < b->op_count; i++)
	{
		const shil_opcode *x = &b->oplist[i];
		mix((u32)x->op);
		mix(x->op == shop_ifb ? 0 : x->flags);
		mix(x->guest_offs);
		mix(x->delay_slot);
		mix_param(&x->rd);
		mix_param(&x->rd2);
		mix_param(&x->rs1);
		mix_param(&x->rs2);
		mix_param(&x->rs3);
	}
}

static void setup(RuntimeBlockInfo *b, u32 pc, fpscr_t cfg)
{
	shil_opcode *keep = b->oplist;
	u32 room = b->op_room;
	memset(b, 0, sizeof(*b));
	b->oplist = keep;
	b->op_room = room;
	b->vaddr = b->addr = pc;
	b->fpu_cfg = cfg;
	b->BranchBlock = b->NextBlock = b->csc_RetCache = 0xFFFFFFFF;
}

static void copy_block(RuntimeBlockInfo *d, const RuntimeBlockInfo *s)
{
	shil_opcode *keep = d->oplist;
	u32 room = d->op_room, i;
	*d = *s;
	d->oplist = keep;
	d->op_room = room;
	d->op_count = 0;
	for (i = 0; i < s->op_count; i++)
		bm_OpPush(d, &s->oplist[i]);
}

/* random code, with runs of the shapes the decoder looks for, constants
 * loaded and constants read */
static void random_code(void)
{
	u32 i;
	for (i = 0; i < 0x200; i++)
	{
		u32 r = rnd();
		code[i] = (r >> 20) % 4 ? handled[rnd() % handled_count] : (u16)r;
		if ((r >> 16) % 8 == 0)
			code[i] = (u16)(0x4024 | (rnd() & 0x0F00));	/* rotcl */
		else if ((r >> 16) % 8 == 1)
			code[i] = (u16)(0xE000 | (rnd() & 0x0FFF));	/* mov #imm,Rn */
		else if ((r >> 16) % 16 == 2)
			code[i] = (u16)(rnd() & 1 ? 0x0019 : (0x2007 | (rnd() & 0x0FF0)));	/* div0u, div0s */
		else if ((r >> 16) % 32 == 4)
			code[i] = (u16)(0x9000 | (rnd() & 0x0FFF));	/* mov.w @(disp,PC),Rn */
		else if ((r >> 16) % 32 == 5)
			code[i] = (u16)(0xD000 | (rnd() & 0x0FFF));	/* mov.l @(disp,PC),Rn */
		else if ((r >> 16) % 64 == 3 && i < 0x200 - 70)
		{
			/* a division of constants */
			u32 q = rnd(), k, rot = (q >> 8) & 0xF, m = (q >> 12) & 0xF, nn = (q >> 16) & 0xF;
			code[i++] = (u16)(0xE000 | (m << 8) | (rnd() & 0xFF));
			code[i++] = (u16)(0xE000 | (nn << 8) | (rnd() & 0xFF));
			code[i++] = (u16)(0xE000 | (rot << 8) | (rnd() & 0xFF));
			code[i] = (u16)(q & 1 ? 0x0019 : (0x2007 | (m << 4) | (nn << 8)));
			for (k = 0; k < 32; k++)
			{
				code[i + 1 + 2 * k] = (u16)(0x4024 | (rot << 8));
				code[i + 2 + 2 * k] = (u16)(0x3004 | (m << 4) | (nn << 8));
			}
			i += 65;
		}
	}
}

/* Blocks made up whole: */
static const shilop const_ops[] = {
	shop_mov32, shop_add, shop_sub, shop_adc, shop_sbc, shop_negc, shop_rocl, shop_rocr, shop_shl, shop_shr,
	shop_sar, shop_ror, shop_shld, shop_shad, shop_or, shop_and, shop_xor, shop_not, shop_ext_s16, shop_ext_s8,
	shop_mul_i32, shop_mul_u16, shop_mul_s16, shop_mul_u64, shop_mul_s64, shop_test, shop_neg, shop_swaplb,
	shop_swap, shop_seteq, shop_setgt, shop_setge, shop_setab, shop_setae, shop_setpeq, shop_xtrct,
	shop_div32u, shop_div32s, shop_div32p2, shop_fneg, shop_fadd, shop_fsub, shop_fmul, shop_fdiv,
	shop_cvt_i2f_n, shop_cvt_i2f_z, shop_fsqrt, shop_cvt_f2i_t, shop_fsrra, shop_fsca, shop_fabs, shop_fmac,
	shop_fseteq, shop_fsetgt
};
/* (the operations done here and now, with one result each and both
 * results, as they are: those with two) */
static const shilop other_ops[] = {
	shop_div1, shop_sync_sr, shop_sync_fpscr, shop_ifb, shop_readm, shop_writem, shop_pref, shop_mov64, shop_frswap
};

static Sh4RegType rand_ireg(void)
{
	static const Sh4RegType regs[] = { reg_r0, reg_r1, reg_r2, reg_r3, reg_r7, reg_r8, reg_r0_Bank,
		reg_r7_Bank, reg_sr_T, reg_sr_status, reg_old_sr_status, reg_fpscr, reg_old_fpscr, reg_macl };
	const u32 few = rnd() & 1;
	return regs[rnd() % (few ? 4 : sizeof(regs) / sizeof(regs[0]))];
}

static Sh4RegType rand_freg(void)
{
	static const Sh4RegType regs[] = { reg_fr_0, reg_fr_1, reg_fr_2, reg_fr_3, reg_fr_15, reg_xf_0, reg_xf_15 };
	const u32 few = rnd() & 1;
	return regs[rnd() % (few ? 4 : sizeof(regs) / sizeof(regs[0]))];
}

static u32 rand_imm(void)
{
	static const u32 imms[] = { 0, 1, 2, 3, 16, 31, 32, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFE0u, 0xFFFFFFF0u,
		0x3F800000u, 0xE0000000u, 0x8C010004u, 0x0C010008u };
	return rnd() & 1 ? imms[rnd() % (sizeof(imms) / sizeof(imms[0]))] : rnd();
}

/* a source: a constant, an integer or a floating-point register, now and
 * then a pair or a vector of them */
static shil_param rand_src(int kind)
{
	switch (kind)
	{
	case 0:
		return shil_param_none();
	case 1:
		return shil_param_imm(rand_imm());
	case 2:
		return shil_param_reg(rand_freg());
	case 3:
		return shil_param_reg(rnd() & 1 ? regv_dr_0 : regv_fv_0 + (rnd() & 1) * 4);
	default:
		return shil_param_reg(rand_ireg());
	}
}

static void push_op(RuntimeBlockInfo *b, shilop o, shil_param rd, shil_param rs1, shil_param rs2)
{
	shil_opcode op;
	memset(&op, 0, sizeof(op));
	op.op = o;
	op.guest_offs = (u16)(2 * b->op_count);
	op.rd = rd;
	op.rd2 = op.rs3 = shil_param_none();
	op.rs1 = rs1;
	op.rs2 = rs2;
	bm_OpPush(b, &op);
}

/* Shapes the passes look for, seldom come by at random: copies of copies,
 * a copy read by the operation that writes what it is a copy of, a copy
 * read as part of a pair, constants kept in XF over a change of FPSCR */
static void random_shape(RuntimeBlockInfo *b)
{
	const shil_param none = shil_param_none();
	const shil_param r1 = shil_param_reg(reg_r1), r2 = shil_param_reg(reg_r2), r3 = shil_param_reg(reg_r3);
	const shil_param fr1 = shil_param_reg(reg_fr_1), fr3 = shil_param_reg(reg_fr_3);
	switch (rnd() % 6)
	{
	case 5:
		{
			/* constants kept in R7 or its bank over a change of SR */
			const shil_param r7 = shil_param_reg(rnd() & 1 ? reg_r7 : reg_r7_Bank);
			push_op(b, shop_mov32, r7, shil_param_imm(rnd()), none);
			push_op(b, shop_sync_sr, none, none, none);
			push_op(b, shop_add, shil_param_reg(reg_r0), r7, shil_param_reg(reg_r0));
		}
		break;
	case 0:
		push_op(b, shop_mov32, r2, r1, none);
		push_op(b, shop_mov32, r3, r2, none);
		push_op(b, shop_add, shil_param_reg(reg_r0), r3, shil_param_imm(4));
		if (rnd() & 1)
			push_op(b, shop_mov32, r2, shil_param_imm(rnd()), none);
		push_op(b, shop_mov32, r3, shil_param_imm(rnd()), none);
		break;
	case 1:
		push_op(b, shop_mov32, r2, r1, none);
		push_op(b, shop_add, r1, r2, rnd() & 1 ? shil_param_imm(8) : r1);
		push_op(b, shop_mov32, r2, shil_param_imm(rnd()), none);
		break;
	case 2:
		push_op(b, shop_mov32, fr1, fr3, none);
		push_op(b, shop_fadd, shil_param_reg(reg_fr_2), rnd() & 1 ? shil_param_reg(regv_dr_0) : fr1, fr3);
		push_op(b, shop_mov32, fr1, shil_param_imm(rnd()), none);
		break;
	case 3:
		push_op(b, shop_mov32, shil_param_reg(rnd() & 1 ? reg_xf_0 : reg_fr_0), shil_param_imm(0x3F800000), none);
		push_op(b, shop_sync_fpscr, none, none, none);
		push_op(b, shop_fadd, shil_param_reg(reg_fr_2), shil_param_reg(reg_xf_0), shil_param_reg(reg_fr_0));
		break;
	default:
		{
			/* shifts in a row, as far as 32 together */
			const shilop sh = rnd() & 1 ? shop_shl : shop_sar;
			const u32 by = 1 + rnd() % 31;
			push_op(b, shop_mov32, r2, r1, none);
			push_op(b, sh, r2, r2, shil_param_imm(by));
			push_op(b, sh, r2, r2, shil_param_imm(rnd() & 1 ? 32 - by : rnd() % 32));
		}
		break;
	}
}

static void random_ops(RuntimeBlockInfo *b)
{
	static const u32 bases[] = { 0x8C010000, 0x0C010000, 0x04011000, 0x0C011000 };
	u32 n = 1 + rnd() % 24, i, base;
	fpscr_t cfg;
	cfg.full = 0;
	/* (one call of rnd() an expression, so that every compiler draws the
	 * same block) */
	base = bases[rnd() % 4];
	setup(b, base + (rnd() & 0x7FE), cfg);
	b->sh4_code_size = 2 * n;
	switch (rnd() % 4)
	{
	case 0: b->BlockType = BET_DynamicJump; break;
	case 1: b->BlockType = (BlockEndType)(BET_DynamicJump + 2 * (1 + rnd() % 3)); break;
	case 2: b->BlockType = rnd() & 1 ? BET_Cond_0 : BET_Cond_1; b->has_jcond = true; break;
	default: b->BlockType = BET_StaticJump; break;
	}
	b->NextBlock = b->vaddr + 2 * n;
	b->BranchBlock = 0x8C020000;
	for (i = 0; i < n; i++)
	{
		shil_opcode op;
		u32 r = rnd();
		memset(&op, 0, sizeof(op));
		op.guest_offs = (u16)(2 * i);
		op.rd = op.rd2 = op.rs1 = op.rs2 = op.rs3 = shil_param_none();
		if (r % 8 < 6)
		{
			op.op = const_ops[(r >> 3) % (sizeof(const_ops) / sizeof(const_ops[0]))];
			op.rd = shil_param_reg(rnd() & 3 ? rand_ireg() : rand_freg());
			op.rs1 = rand_src((r >> 9) % 13 == 0 ? 3 : (r >> 9) % 5 == 0 ? 2 : (r >> 12) % 3 ? 1 : 4);
			op.rs2 = rand_src((r >> 14) % 4 == 0 ? 0 : (r >> 14) % 11 == 1 ? 3 : (r >> 16) % 3 ? 1 : 4);
			op.rs3 = rand_src((r >> 18) % 4 ? 0 : (r >> 20) % 2 ? 1 : 4);
			if ((r >> 21) % 4 == 0)
				op.rs2 = op.rs1;		/* a op a */
			if (op.op == shop_adc || op.op == shop_sbc || op.op == shop_negc || op.op == shop_rocl
					|| op.op == shop_rocr || op.op == shop_mul_u64 || op.op == shop_mul_s64
					|| op.op == shop_div32u || op.op == shop_div32s)
				op.rd2 = shil_param_reg(rand_ireg());
			if (op.op == shop_fsca)
				op.rd = shil_param_typed(FMT_V2, reg_fr_0 + 2 * (rnd() & 1));
			if (op.op == shop_mov32 && !shil_is_imm(&op.rs1))
			{
				/* a copy: from one of the same kind, now and then of what was just written */
				op.rs1 = shil_param_reg(shil_is_r32i(&op.rd) ? rand_ireg() : rand_freg());
				op.rs2 = op.rs3 = shil_param_none();
			}
			/* shifts by a constant below 32, rotations by 1 or 31, as the
			 * decoder has them */
			if (op.op == shop_shl || op.op == shop_shr || op.op == shop_sar)
				op.rs2 = shil_param_imm(rnd() % 32);
			else if (op.op == shop_ror)
				op.rs2 = shil_param_imm(rnd() & 1 ? 1 : 31);
			if ((op.op == shop_shl || op.op == shop_shr || op.op == shop_sar) && (r >> 23) % 2)
			{
				/* shifts in a row */
				op.rs1 = op.rd = shil_param_reg(reg_r1);
				op.rs2 = shil_param_imm(rnd() % 20);
			}
		}
		else
		{
			op.op = other_ops[(r >> 3) % (sizeof(other_ops) / sizeof(other_ops[0]))];
			switch (op.op)
			{
			case shop_readm:
			case shop_writem:
				op.flags = 1u << ((r >> 8) % 4);
				/* (mostly the block's own page) */
				op.rs1 = (r >> 10) % 2 ? shil_param_imm(b->vaddr + (rnd() & 7 ? rnd() & 0x3C : (rnd() & 0x1FFC) - 0x800))
						: shil_param_reg(rand_ireg());
				op.rs3 = rand_src((r >> 11) % 3 == 0 ? 1 : (r >> 13) % 2 ? 0 : 4);
				if (op.op == shop_readm)
					op.rd = shil_param_reg(op.flags == 8 ? regv_dr_0 : rand_ireg());
				else
					op.rs2 = shil_param_reg(rand_ireg());
				break;
			case shop_pref:
				op.rs1 = (r >> 10) % 2 ? shil_param_imm(rand_imm()) : shil_param_reg(rand_ireg());
				break;
			case shop_div1:
				op.rd = op.rs1 = shil_param_reg(rand_ireg());
				op.rs2 = shil_param_reg(rand_ireg());
				break;
			case shop_mov64:
				op.rd = shil_param_reg(regv_dr_0 + (rnd() & 1));
				op.rs1 = shil_param_reg(regv_xd_0);
				break;
			case shop_frswap:
				op.rd = shil_param_reg(regv_xmtrx);
				op.rs1 = shil_param_reg(regv_fmtrx);
				break;
			default:
				break;
			}
		}
		if (i == n - 1 && BET_GET_CLS(b->BlockType) == BET_CLS_Dynamic && (r >> 24) % 2)
		{
			op.op = shop_jdyn;
			op.rd = shil_param_reg(reg_pc_dyn);
			op.rs1 = (r >> 25) % 2 ? shil_param_imm(rand_imm()) : shil_param_reg(rand_ireg());
			op.rs2 = shil_param_imm(rnd() & 2);
			op.rs3 = op.rd2 = shil_param_none();
		}
		else if (i == n - 1 && b->has_jcond && (r >> 24) % 2)
		{
			op.op = shop_jcond;
			op.rd = shil_param_reg(reg_pc_dyn);
			op.rs1 = (r >> 25) % 2 ? shil_param_imm(rnd() & 1) : shil_param_reg(reg_sr_T);
			op.rs2 = op.rs3 = op.rd2 = shil_param_none();
		}
		if ((r >> 26) % 8 == 0 && i < n - 1)
			random_shape(b);
		op.guest_offs = (u16)(2 * b->op_count);
		bm_OpPush(b, &op);
	}
	b->sh4_code_size = 2 * b->op_count;
	b->read_only = (rnd() & 1) != 0;
}

static void cb_preload(sh4_regalloc *ra, u32 reg, u32 host) { (void)ra; mix(0x504C0000u | reg); mix(host); }
static void cb_writeback(sh4_regalloc *ra, u32 reg, u32 host) { (void)ra; mix(0x57420000u | reg); mix(host); }
static void cb_preload_fpu(sh4_regalloc *ra, u32 reg, u32 host) { (void)ra; mix(0x50460000u | reg); mix(host); }
static void cb_writeback_fpu(sh4_regalloc *ra, u32 reg, u32 host) { (void)ra; mix(0x57460000u | reg); mix(host); }

static void mix_map(const sh4_regalloc *ra, const shil_param *p)
{
	const bool g = sh4_regalloc_is_allocg(ra, p), f = sh4_regalloc_is_allocf(ra, p);
	mix(g | f << 1 | sh4_regalloc_is_alloc_any(ra, p) << 2);
	if (g)
		mix(sh4_regalloc_mapg(ra, p));
	if (f)
	{
		u32 i;
		for (i = 0; i < shil_count(p) && i < ra->max_vec; i++)
			mix(sh4_regalloc_mapf(ra, p, (int)i));
	}
}

/* the host registers of each recompiler (x86-64, on Windows, x86, ARM64,
 * 32-bit ARM, on Apple's), and whether pairs are kept in them there: the
 * other way too where there are floating-point ones enough */
static const u32 gsets[][8] = {
	{ 3, 5, 12, 13 },
	{ 3, 5, 7, 6, 12, 13 },
	{ 3, 5, 6, 7 },
	{ 19, 20, 21, 22, 23, 24, 25 },
	{ 5, 6, 7, 10, 11 },
	{ 5, 6, 7, 10 }
};
static const u32 gcounts[] = { 4, 6, 4, 7, 5, 4 };
static const u32 fcounts[] = { 8, 10, 4, 8, 16, 16 };
static const bool vec2s[] = { true, true, false, true, true, true };

static void allocate(RuntimeBlockInfo *b, int t)
{
	static sh4_regalloc ra;
	u32 fregs[32], i, j;
	const u32 set = (u32)t % 6;
	int opid;
	for (i = 0; i < 32; i++)
		fregs[i] = 8 + i;
	for (j = 0; j < (fcounts[set] >= 8 ? 2u : 1u); j++)
	{
		const bool vec2 = j == 0 ? vec2s[set] : !vec2s[set];
		sh4_regalloc_init(&ra, vec2, cb_preload, cb_writeback, cb_preload_fpu, cb_writeback_fpu, NULL);
		sh4_regalloc_begin(&ra, b, gsets[set], gcounts[set], fregs, fcounts[set]);
		mix(0x424C4B00u | j);
		for (opid = 0; opid < (int)b->op_count; opid++)
		{
			shil_opcode *op = &b->oplist[opid];
			sh4_regalloc_op_begin(&ra, op, opid);
			mix_map(&ra, &op->rd);
			mix_map(&ra, &op->rd2);
			mix_map(&ra, &op->rs1);
			mix_map(&ra, &op->rs2);
			mix_map(&ra, &op->rs3);
			for (i = 0; i < 8; i++)
				mix(sh4_regalloc_reg_used(&ra, i < gcounts[set] ? gsets[set][i] : 8 + i, i >= gcounts[set]));
			sh4_regalloc_op_end(&ra, op);
		}
		/* (as a recompiler that starts again at an operation has it) */
		if (b->op_count > 2 && rnd() % 4 == 0)
		{
			sh4_regalloc_cleanup(&ra);
			sh4_regalloc_begin(&ra, b, gsets[set], gcounts[set], fregs, fcounts[set]);
			opid = (int)(rnd() % b->op_count);
			sh4_regalloc_set_opnum(&ra, opid);
			mix_map(&ra, &b->oplist[opid].rs1);
			mix_map(&ra, &b->oplist[opid].rd);
			for (; opid < (int)b->op_count; opid++)
			{
				sh4_regalloc_op_end(&ra, &b->oplist[opid]);
				if (opid + 1 < (int)b->op_count)
					sh4_regalloc_op_begin(&ra, &b->oplist[opid + 1], opid + 1);
			}
		}
		sh4_regalloc_cleanup(&ra);
	}
}

static void analyse(RuntimeBlockInfo *b, const RuntimeBlockInfo *a, int t)
{
	copy_block(b, a);
	AnalyseBlock(b);
	mix(b->op_count);
	allocate(b, t);
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 20000, t, changed = 0;
	static RuntimeBlockInfo a, b;
	for (t = 0; t < 0x10000; t++)
	{
		sin_table[t].u[0] = rnd();
		sin_table[t].u[1] = rnd();
	}
	BuildOpcodeTables();
	for (t = 0; t < 0x10000; t++)
		if (OpDesc[t]->rec_oph != NULL || OpDesc[t]->decode != 0)
			handled[handled_count++] = (u16)t;
	for (t = 0; t < n; t++)
	{
		u32 pc;
		fpscr_t cfg;
		settings.dreamcast.FullMMU = (rnd() & 7) == 0;
		CCN_MMUCR.AT = settings.dreamcast.FullMMU;
		if (t & 1)
		{
			random_ops(&a);
			analyse(&b, &a, t);
			changed += b.op_count != a.op_count;
			continue;
		}
		random_code();
		pc = code_base + (rnd() % 64) * 2;
		cfg.full = 0;
		cfg.PR = rnd() & 1;
		cfg.SZ = rnd() & 1;
		cfg.RM = rnd() & 3;
		settings.dynarec.AccurateTiming = (rnd() & 1) != 0;
		settings.dynarec.DisableDivMatching = (rnd() & 7) == 0;
		features.OnlyDynamicEnds = (rnd() & 15) == 0;
		features.InterpreterFallback = (rnd() & 15) == 0;
		p_sh4rcb->cntx.sr.FD = 0;
		setup(&a, pc, cfg);
		if (!dec_DecodeBlock(&a, SH4_TIMESLICE / 2))
			continue;
		a.read_only = (rnd() & 1) != 0;
		analyse(&b, &a, t);
		changed += b.op_count != a.op_count;
	}
	free(a.oplist);
	free(b.oplist);
	printf("%d blocks, %d made shorter, %08x\n", n, changed, h);
	return 0;
}
