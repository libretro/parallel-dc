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
#include "ssa_regalloc.h"

/* The host registers free, the next taken from the back (a ring: one
 * given back goes to the front) */
static bool hr_empty(const sh4_host_regs *h) { return h->n == 0; }
static void hr_push_back(sh4_host_regs *h, u32 v)
{
	verify(h->n < SH4_REGALLOC_HOST_MAX);
	h->regs[(h->head + h->n++) % SH4_REGALLOC_HOST_MAX] = v;
}
static void hr_push_front(sh4_host_regs *h, u32 v)
{
	verify(h->n < SH4_REGALLOC_HOST_MAX);
	h->head = (h->head + SH4_REGALLOC_HOST_MAX - 1) % SH4_REGALLOC_HOST_MAX;
	h->regs[h->head] = v;
	h->n++;
}
static u32 hr_take_back(sh4_host_regs *h)
{
	const u32 v = h->regs[(h->head + h->n - 1) % SH4_REGALLOC_HOST_MAX];
	h->n--;
	return v;
}

static bool is_float(Sh4RegType reg)
{
	return reg >= reg_fr_0 && reg <= reg_xf_15;
}

static bool is_alloced(const sh4_regalloc *ra, Sh4RegType reg)
{
	return (int)reg >= 0 && reg < sh4_reg_count && ((ra->alloced_bits[reg >> 6] >> (reg & 63)) & 1) != 0;
}

/* the first one with a host register after @after (-1: from the start); -1 if none */
static int next_alloced(const sh4_regalloc *ra, int after)
{
	int w;
	for (w = (after + 1) >> 6; w < 2; w++)
	{
		u64 bits = ra->alloced_bits[w];
		if (w == (after + 1) >> 6)
			bits &= ~(u64)0 << ((after + 1) & 63);
		if (bits != 0)
		{
			int b = 0;
			while ((bits & 1) == 0)
			{
				bits >>= 1;
				b++;
			}
			return w * 64 + b;
		}
	}
	return -1;
}

static bool none_alloced(const sh4_regalloc *ra)
{
	return (ra->alloced_bits[0] | ra->alloced_bits[1]) == 0;
}

static void alloc_reg(sh4_regalloc *ra, Sh4RegType reg, u32 host_reg, u16 version, bool write_back, bool dirty)
{
	ra->alloced[reg].host_reg = host_reg;
	ra->alloced[reg].version = version;
	ra->alloced[reg].write_back = write_back;
	ra->alloced[reg].dirty = dirty;
	ra->alloced_bits[reg >> 6] |= (u64)1 << (reg & 63);
}

static void free_reg(sh4_regalloc *ra, Sh4RegType reg)
{
	memset(&ra->alloced[reg], 0, sizeof(ra->alloced[reg]));
	ra->alloced_bits[reg >> 6] &= ~((u64)1 << (reg & 63));
}

static void write_back_reg(sh4_regalloc *ra, Sh4RegType reg_num)
{
	sh4_reg_alloc *a = &ra->alloced[reg_num];
	if (!a->write_back)
		return;
	if (!ra->fast_forwarding)
	{
		if (is_float(reg_num))
			ra->writeback_fpu(ra, reg_num, a->host_reg);
		else
			ra->writeback(ra, reg_num, a->host_reg);
	}
	a->write_back = false;
	a->dirty = false;
}

static void flush_reg(sh4_regalloc *ra, Sh4RegType reg_num, bool hard)
{
	u32 host_reg;
	if (!is_alloced(ra, reg_num))
		return;
	write_back_reg(ra, reg_num);
	if (!hard)
		return;
	host_reg = ra->alloced[reg_num].host_reg;
	free_reg(ra, reg_num);
	hr_push_front(is_float(reg_num) ? &ra->fregs : &ra->gregs, host_reg);
}

static void flush_all_regs(sh4_regalloc *ra, bool hard)
{
	int reg;
	if (hard)
	{
		while (!none_alloced(ra))
			flush_reg(ra, (Sh4RegType)next_alloced(ra, -1), true);
	}
	else
	{
		for (reg = next_alloced(ra, -1); reg >= 0; reg = next_alloced(ra, reg))
			flush_reg(ra, (Sh4RegType)reg, false);
	}
}

/* whether @param is @reg, at @version, in a vector (@vector) or not */
static bool param_uses(const sh4_regalloc *ra, const shil_param *p, Sh4RegType reg, u32 version, bool vector)
{
	return shil_is_reg(p) && reg >= (Sh4RegType)p->_imm && reg < (Sh4RegType)(p->_imm + shil_count(p))
			&& version == p->version[reg - (Sh4RegType)p->_imm]
			&& vector == (shil_count(p) > ra->max_vec);
}

static bool uses_reg(const sh4_regalloc *ra, const shil_opcode *op, Sh4RegType reg, u32 version, bool vector)
{
	return param_uses(ra, &op->rs1, reg, version, vector) || param_uses(ra, &op->rs2, reg, version, vector)
			|| param_uses(ra, &op->rs3, reg, version, vector);
}

static bool param_defs(const sh4_regalloc *ra, const shil_param *p, Sh4RegType reg, bool vector)
{
	return shil_is_reg(p) && reg >= (Sh4RegType)p->_imm && reg < (Sh4RegType)(p->_imm + shil_count(p))
			&& vector == (shil_count(p) > ra->max_vec);
}

static bool defs_reg(const sh4_regalloc *ra, const shil_opcode *op, Sh4RegType reg, bool vector)
{
	return param_defs(ra, &op->rd, reg, vector) || param_defs(ra, &op->rd2, reg, vector);
}

static void spill_reg(sh4_regalloc *ra, bool freg, bool source)
{
	Sh4RegType spilled_reg = NoReg;
	int latest_use = -1, ri;
	u32 host_reg;

	for (ri = next_alloced(ra, -1); ri >= 0; ri = next_alloced(ra, ri))
	{
		const Sh4RegType reg_first = (Sh4RegType)ri;
		const sh4_reg_alloc *reg_second = &ra->alloced[ri];
		const shil_opcode *op;
		bool pending = false;
		int first_use = -1, i;
		u32 pi;

		if (is_float(reg_first) != freg)
			continue;
		/* Don't spill already spilled regs */
		for (pi = 0; pi < ra->pending_count; pi++)
			if (ra->pending_flushes[pi] == reg_first)
			{
				pending = true;
				break;
			}
		if (pending)
			continue;

		/* Don't spill current op scalar dest regs */
		op = &ra->block->oplist[ra->opnum];
		if (defs_reg(ra, op, reg_first, false))
			continue;

		/* Find the first use, but ignore vec ops (they don't use reg alloc) */
		for (i = ra->opnum + (source ? 0 : 1); i < (int)ra->block->op_count; i++)
		{
			op = &ra->block->oplist[i];
			if (uses_reg(ra, op, reg_first, reg_second->version, false))
			{
				first_use = i;
				break;
			}
		}
		if (first_use == -1)
		{
			latest_use = -1;
			spilled_reg = reg_first;
			break;
		}
		if (first_use > latest_use && first_use > ra->opnum)
		{
			latest_use = first_use;
			spilled_reg = reg_first;
		}
	}
	if (latest_use != -1)
	{
		ra->spills++;
		/* need to write-back if dirty so reload works */
		if (ra->alloced[spilled_reg].dirty)
			ra->alloced[spilled_reg].write_back = true;
	}
	verify(spilled_reg != NoReg);

	if (source)
		flush_reg(ra, spilled_reg, true);
	else
	{
		/* Delay the eviction of spilled regs from the map due to dest
		 * register allocation. It's possible that the same host reg is
		 * allocated to a source operand and to the (future) dest operand.
		 * In this case we want to keep both mappings until the current op
		 * is done. */
		write_back_reg(ra, spilled_reg);
		host_reg = ra->alloced[spilled_reg].host_reg;
		hr_push_front(is_float(spilled_reg) ? &ra->fregs : &ra->gregs, host_reg);
		verify(ra->pending_count < sh4_reg_count);
		ra->pending_flushes[ra->pending_count++] = spilled_reg;
	}
}

/* a host register for @param's kind, a register spilled if there is none */
static u32 take_host_reg(sh4_regalloc *ra, const shil_param *param, bool source)
{
	sh4_host_regs *h = shil_is_r32i(param) ? &ra->gregs : &ra->fregs;
	if (hr_empty(h))
	{
		spill_reg(ra, !shil_is_r32i(param), source);
		verify(!hr_empty(h));
	}
	return hr_take_back(h);
}

static void alloc_source_reg(sh4_regalloc *ra, const shil_param *param)
{
	u32 i;
	if (!shil_is_reg(param) || shil_count(param) > ra->max_vec)
		return;
	for (i = 0; i < shil_count(param); i++)
	{
		const Sh4RegType sh4reg = (Sh4RegType)(param->_imm + i);
		u32 host_reg;
		if (is_alloced(ra, sh4reg))
			continue;
		host_reg = take_host_reg(ra, param, true);
		alloc_reg(ra, sh4reg, host_reg, param->version[i], false, false);
		if (!ra->fast_forwarding)
		{
			if (is_float(sh4reg))
				ra->preload_fpu(ra, sh4reg, host_reg);
			else
				ra->preload(ra, sh4reg, host_reg);
		}
	}
}

static bool needs_write_back(const sh4_regalloc *ra, Sh4RegType reg, u32 version)
{
	int i;
	for (i = ra->opnum + 1; i < (int)ra->block->op_count; i++)
	{
		const shil_opcode *op = &ra->block->oplist[i];
		/* if a subsequent op needs all or some regs flushed to mem */
		if (op->op == shop_ifb || (mmu_enabled() && (op->op == shop_readm || op->op == shop_writem || op->op == shop_pref)))
			return true;
		if (op->op == shop_sync_sr && (reg == reg_sr_status || reg == reg_old_sr_status || (reg >= reg_r0 && reg <= reg_r7)
				|| (reg >= reg_r0_Bank && reg <= reg_r7_Bank)))
			return true;
		if (op->op == shop_sync_fpscr && (reg == reg_fpscr || reg == reg_old_fpscr || (reg >= reg_fr_0 && reg <= reg_xf_15)))
			return true;
		if (op->op == shop_div1 && reg == reg_sr_status)
			return true;
		/* if reg is used by a subsequent vector op that doesn't use reg allocation */
		if (uses_reg(ra, op, reg, version, true))
			return true;
		/* no writeback needed if redefined */
		if (defs_reg(ra, op, reg, true))
			return false;
		if (defs_reg(ra, op, reg, false))
			return false;
	}
	return true;
}

static void alloc_dest_reg(sh4_regalloc *ra, const shil_param *param)
{
	u32 i;
	if (!shil_is_reg(param) || shil_count(param) > ra->max_vec)
		return;
	for (i = 0; i < shil_count(param); i++)
	{
		const Sh4RegType sh4reg = (Sh4RegType)(param->_imm + i);
		if (!is_alloced(ra, sh4reg))
		{
			const u32 host_reg = take_host_reg(ra, param, false);
			alloc_reg(ra, sh4reg, host_reg, param->version[i], needs_write_back(ra, sh4reg, param->version[i]), true);
		}
		else
		{
			sh4_reg_alloc *reg = &ra->alloced[sh4reg];
			verify(!reg->write_back);
			reg->write_back = needs_write_back(ra, sh4reg, param->version[i]);
			reg->dirty = true;
			reg->version = param->version[i];
		}
		verify(ra->alloced[sh4reg].dirty);
	}
}

/* vectors wider than a pair are left in memory: flushed before */
static void flush_vector(sh4_regalloc *ra, const shil_param *p, bool hard)
{
	u32 i;
	if (shil_is_reg(p) && shil_count(p) > ra->max_vec)
		for (i = 0; i < shil_count(p); i++)
		{
			if (hard)
				verify(!is_alloced(ra, (Sh4RegType)(p->_imm + i)) || !ra->alloced[p->_imm + i].write_back);
			flush_reg(ra, (Sh4RegType)(p->_imm + i), hard);
		}
}

void sh4_regalloc_init(sh4_regalloc *ra, bool alloc_vec2, sh4_regalloc_cb preload, sh4_regalloc_cb writeback,
		sh4_regalloc_cb preload_fpu, sh4_regalloc_cb writeback_fpu, void *user)
{
	memset(ra, 0, sizeof(*ra));
	ra->max_vec = alloc_vec2 ? 2 : 1;
	ra->preload = preload;
	ra->writeback = writeback;
	ra->preload_fpu = preload_fpu;
	ra->writeback_fpu = writeback_fpu;
	ra->user = user;
}

void sh4_regalloc_begin(sh4_regalloc *ra, RuntimeBlockInfo *block, const u32 *gregs, u32 gregs_count,
		const u32 *fregs, u32 fregs_count)
{
	u32 i;
	ra->block = block;
	ssa_add_versions(block);

	verify(hr_empty(&ra->gregs));
	for (i = 0; i < gregs_count; i++)
		hr_push_back(&ra->gregs, gregs[i]);
	verify(hr_empty(&ra->fregs));
	for (i = 0; i < fregs_count; i++)
		hr_push_back(&ra->fregs, fregs[i]);
}

void sh4_regalloc_op_begin(sh4_regalloc *ra, const shil_opcode *op, int opid)
{
	int i;
	ra->opnum = opid;
	if (op->op == shop_ifb)
		flush_all_regs(ra, true);
	else if (mmu_enabled() && (op->op == shop_readm || op->op == shop_writem || op->op == shop_pref))
		flush_all_regs(ra, false);
	else if (op->op == shop_sync_sr)
	{
		flush_reg(ra, reg_sr_status, true);
		flush_reg(ra, reg_old_sr_status, true);
		for (i = reg_r0; i <= reg_r7; i++)
			flush_reg(ra, (Sh4RegType)i, true);
		for (i = reg_r0_Bank; i <= reg_r7_Bank; i++)
			flush_reg(ra, (Sh4RegType)i, true);
	}
	else if (op->op == shop_sync_fpscr)
	{
		flush_reg(ra, reg_fpscr, true);
		flush_reg(ra, reg_old_fpscr, true);
		for (i = reg_fr_0; i <= reg_xf_15; i++)
			flush_reg(ra, (Sh4RegType)i, true);
	}
	else if (op->op == shop_div1)
		/* reads and writes Q and M where the status register is kept */
		flush_reg(ra, reg_sr_status, true);
	/* Flush regs used by vector ops */
	flush_vector(ra, &op->rs1, false);
	flush_vector(ra, &op->rs2, false);
	flush_vector(ra, &op->rs3, false);
	if (op->op != shop_ifb)
	{
		alloc_source_reg(ra, &op->rs1);
		alloc_source_reg(ra, &op->rs2);
		alloc_source_reg(ra, &op->rs3);
		/* Hard flush vector ops destination regs. Note that this is
		 * incorrect if a reg is both src (scalar) and dest (vec). However
		 * such an op doesn't exist. */
		flush_vector(ra, &op->rd, true);
		flush_vector(ra, &op->rd2, true);
		alloc_dest_reg(ra, &op->rd);
		alloc_dest_reg(ra, &op->rd2);
	}
}

void sh4_regalloc_op_end(sh4_regalloc *ra, const shil_opcode *op)
{
	u32 pi;
	int reg;
	for (pi = 0; pi < ra->pending_count; pi++)
	{
		const Sh4RegType r = ra->pending_flushes[pi];
		verify(!ra->alloced[r].write_back);
		free_reg(ra, r);
	}
	ra->pending_count = 0;

	/* Flush normally */
	for (reg = next_alloced(ra, -1); reg >= 0; reg = next_alloced(ra, reg))
		flush_reg(ra, (Sh4RegType)reg, false);

	/* Final writebacks */
	if (op >= &ra->block->oplist[ra->block->op_count - 1])
	{
		flush_all_regs(ra, false);
		ra->final_opend = true;
	}
}

void sh4_regalloc_set_opnum(sh4_regalloc *ra, int num)
{
	int i;
	ra->fast_forwarding = true;
	for (i = 0; i < num; i++)
	{
		const shil_opcode *op = &ra->block->oplist[i];
		sh4_regalloc_op_begin(ra, op, i);
		sh4_regalloc_op_end(ra, op);
	}
	sh4_regalloc_op_begin(ra, &ra->block->oplist[num], num);
	ra->fast_forwarding = false;
}

bool sh4_regalloc_is_alloc_any(const sh4_regalloc *ra, const shil_param *prm)
{
	bool rv;
	u32 i;
	if (!shil_is_reg(prm))
		return false;
	rv = is_alloced(ra, shil_reg(prm));
	for (i = 1; i < shil_count(prm); i++)
		verify(is_alloced(ra, (Sh4RegType)(prm->_imm + i)) == rv);
	return rv;
}

bool sh4_regalloc_is_allocg(const sh4_regalloc *ra, const shil_param *prm)
{
	if (!shil_is_reg(prm) || shil_count(prm) > ra->max_vec)
		return false;
	return !is_float(shil_reg(prm)) && is_alloced(ra, shil_reg(prm));
}

bool sh4_regalloc_is_allocf(const sh4_regalloc *ra, const shil_param *prm)
{
	if (!shil_is_reg(prm) || shil_count(prm) > ra->max_vec)
		return false;
	return is_float(shil_reg(prm)) && is_alloced(ra, shil_reg(prm));
}

u32 sh4_regalloc_mapg(const sh4_regalloc *ra, const shil_param *prm)
{
	verify(sh4_regalloc_is_allocg(ra, prm));
	verify(shil_count(prm) == 1);
	return ra->alloced[prm->_imm].host_reg;
}

u32 sh4_regalloc_mapf(const sh4_regalloc *ra, const shil_param *prm, int index)
{
	verify(sh4_regalloc_is_allocf(ra, prm));
	verify(index >= 0 && (u32)index < shil_count(prm) && shil_count(prm) <= ra->max_vec);
	return ra->alloced[prm->_imm + index].host_reg;
}

bool sh4_regalloc_reg_used(const sh4_regalloc *ra, u32 host_reg, bool fpu)
{
	int reg;
	for (reg = next_alloced(ra, -1); reg >= 0; reg = next_alloced(ra, reg))
		if (ra->alloced[reg].host_reg == host_reg && is_float((Sh4RegType)reg) == fpu)
			return true;
	return false;
}

void sh4_regalloc_cleanup(sh4_regalloc *ra)
{
	verify(ra->final_opend || ra->block->op_count == 0);
	ra->final_opend = false;
	flush_all_regs(ra, true);
	verify(none_alloced(ra));
	verify(ra->pending_count == 0);
	ra->block = NULL;
	ra->gregs.head = ra->gregs.n = 0;
	ra->fregs.head = ra->fregs.n = 0;
}
