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
#ifndef SH4_DYNA_SSA_REGALLOC_H
#define SH4_DYNA_SSA_REGALLOC_H
#include "types.h"
#include "decoder.h"
#include "hw/sh4/modules/mmu.h"
#include "ssa.h"

/* The register allocator: the SH4's registers a block uses are kept in
 * host registers from where they are first read or written, written back
 * where something after needs them in memory, spilled when there are no
 * more host registers. The recompiler is called back to load a register
 * into a host one and to write one back (an integer register or a
 * floating-point one). Host registers are numbered as the recompiler has
 * them.
 *
 * With alloc_vec2, a pair of the SH4's floating-point registers that an
 * instruction takes as one (DRn and XDn: FMOV with FPSCR.SZ set, FSCA) is
 * given two host registers like any other, in place of being written back
 * before the instruction and read from memory by it. Wider vectors (FVn,
 * XMTRX) are left in memory either way. */

#define SH4_REGALLOC_HOST_MAX 64

typedef struct sh4_regalloc sh4_regalloc;
typedef void (*sh4_regalloc_cb)(sh4_regalloc *ra, u32 reg, u32 host_reg);

typedef struct sh4_reg_alloc
{
	u32 host_reg;
	u16 version;
	bool write_back;
	bool dirty;
} sh4_reg_alloc;

/* the host registers free, the next taken from the back (a ring: one
 * given back goes to the front) */
typedef struct sh4_host_regs
{
	u32 regs[SH4_REGALLOC_HOST_MAX];
	u32 head, n;
} sh4_host_regs;

struct sh4_regalloc
{
	sh4_regalloc_cb preload, writeback, preload_fpu, writeback_fpu;
	void *user;
	u32 max_vec;
	RuntimeBlockInfo *block;
	sh4_host_regs gregs, fregs;
	/* registers spilled for a destination, let go at the end of the op */
	Sh4RegType pending_flushes[sh4_reg_count];
	u32 pending_count;
	/* the guest registers in host ones: a register's entry, and a bit each
	 * for those that have one (in register order, as they are gone through) */
	sh4_reg_alloc alloced[sh4_reg_count];
	u64 alloced_bits[2];
	int opnum;
	bool final_opend;
	bool fast_forwarding;
	u32 spills;
};

#ifdef __cplusplus
extern "C" {
#endif

void sh4_regalloc_init(sh4_regalloc *ra, bool alloc_vec2, sh4_regalloc_cb preload, sh4_regalloc_cb writeback,
		sh4_regalloc_cb preload_fpu, sh4_regalloc_cb writeback_fpu, void *user);
/* A block to be compiled, with the host registers there are for it */
void sh4_regalloc_begin(sh4_regalloc *ra, RuntimeBlockInfo *block, const u32 *gregs, u32 gregs_count,
		const u32 *fregs, u32 fregs_count);
/* Around each operation, in order (@opid its place) */
void sh4_regalloc_op_begin(sh4_regalloc *ra, const shil_opcode *op, int opid);
void sh4_regalloc_op_end(sh4_regalloc *ra, const shil_opcode *op);
/* as it is at operation @num, nothing loaded or written back on the way */
void sh4_regalloc_set_opnum(sh4_regalloc *ra, int num);
/* whether a parameter is in host registers: any, integer, floating-point */
bool sh4_regalloc_is_alloc_any(const sh4_regalloc *ra, const shil_param *prm);
bool sh4_regalloc_is_allocg(const sh4_regalloc *ra, const shil_param *prm);
bool sh4_regalloc_is_allocf(const sh4_regalloc *ra, const shil_param *prm);
/* the host register it is in; of a pair, the @index'th */
u32 sh4_regalloc_mapg(const sh4_regalloc *ra, const shil_param *prm);
u32 sh4_regalloc_mapf(const sh4_regalloc *ra, const shil_param *prm, int index);
/* whether a host register (@fpu: a floating-point one) holds a register */
bool sh4_regalloc_reg_used(const sh4_regalloc *ra, u32 host_reg, bool fpu);
/* The block done: everything written back, the host registers let go */
void sh4_regalloc_cleanup(sh4_regalloc *ra);

#ifdef __cplusplus
}
#endif

#endif
