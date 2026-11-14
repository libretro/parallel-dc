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
#pragma once
#include "types.h"
#include "decoder.h"
#include "hw/sh4/modules/mmu.h"
#include "ssa.h"

#define ssa_printf(...) DEBUG_LOG(DYNAREC, __VA_ARGS__)

/* AllocVec2: a pair of the SH4's floating-point registers that an
 * instruction takes as one (DRn and XDn: FMOV with FPSCR.SZ set, FSCA) is
 * given two host registers like any other, in place of being written back
 * before the instruction and read from memory by it. Wider vectors (FVn,
 * XMTRX) are left in memory either way. */
template<typename nreg_t, typename nregf_t, bool AllocVec2 = false>
class RegAlloc
{
public:
	RegAlloc() {}
	virtual ~RegAlloc() {}

	void DoAlloc(RuntimeBlockInfo* block, const nreg_t* regs_avail, const nregf_t* regsf_avail)
	{
		this->block = block;
		SSAOptimizer optim(block);
		optim.AddVersionPass();

		verify(host_gregs.empty());
		while (*regs_avail != (nreg_t)-1)
			host_gregs.push_back(*regs_avail++);

		verify(host_fregs.empty());
		while (*regsf_avail != (nregf_t)-1)
			host_fregs.push_back(*regsf_avail++);
	}

	void OpBegin(shil_opcode* op, int opid)
	{
		opnum = opid;
		if (op->op == shop_ifb)
		{
			FlushAllRegs(true);
		}
		else if (mmu_enabled() && (op->op == shop_readm || op->op == shop_writem || op->op == shop_pref))
		{
			FlushAllRegs(false);
		}
		else if (op->op == shop_sync_sr)
		{
			//FlushReg(reg_sr_T, true);
			FlushReg(reg_sr_status, true);
			FlushReg(reg_old_sr_status, true);
			for (int i = reg_r0; i <= reg_r7; i++)
				FlushReg((Sh4RegType)i, true);
			for (int i = reg_r0_Bank; i <= reg_r7_Bank; i++)
				FlushReg((Sh4RegType)i, true);
		}
		else if (op->op == shop_sync_fpscr)
		{
			FlushReg(reg_fpscr, true);
			FlushReg(reg_old_fpscr, true);
			for (int i = reg_fr_0; i <= reg_xf_15; i++)
				FlushReg((Sh4RegType)i, true);
		}
		else if (op->op == shop_div1)
		{
			// reads and writes Q and M where the status register is kept
			FlushReg(reg_sr_status, true);
		}
		// Flush regs used by vector ops
		if (op->rs1.is_reg() && op->rs1.count() > MaxVecSize)
		{
			for (int i = 0; i < op->rs1.count(); i++)
				FlushReg((Sh4RegType)(op->rs1._reg + i), false);
		}
		if (op->rs2.is_reg() && op->rs2.count() > MaxVecSize)
		{
			for (int i = 0; i < op->rs2.count(); i++)
				FlushReg((Sh4RegType)(op->rs2._reg + i), false);
		}
		if (op->rs3.is_reg() && op->rs3.count() > MaxVecSize)
		{
			for (int i = 0; i < op->rs3.count(); i++)
				FlushReg((Sh4RegType)(op->rs3._reg + i), false);
		}
		if (op->op != shop_ifb)
		{
			AllocSourceReg(op->rs1);
			AllocSourceReg(op->rs2);
			AllocSourceReg(op->rs3);
			// Hard flush vector ops destination regs
			// Note that this is incorrect if a reg is both src (scalar) and dest (vec). However such an op doesn't exist.
			if (op->rd.is_reg() && op->rd.count() > MaxVecSize)
			{
				for (int i = 0; i < op->rd.count(); i++)
				{
					verify(!IsAlloced((Sh4RegType)(op->rd._reg + i)) || !alloced[op->rd._reg + i].write_back);
					FlushReg((Sh4RegType)(op->rd._reg + i), true);
				}
			}
			if (op->rd2.is_reg() && op->rd2.count() > MaxVecSize)
			{
				for (int i = 0; i < op->rd2.count(); i++)
				{
					verify(!IsAlloced((Sh4RegType)(op->rd2._reg + i)) || !alloced[op->rd2._reg + i].write_back);
					FlushReg((Sh4RegType)(op->rd2._reg + i), true);
				}
			}
			AllocDestReg(op->rd);
			AllocDestReg(op->rd2);
		}
		ssa_printf("%08x  %s gregs %zd fregs %zd\n", block->vaddr + op->guest_offs, op->dissasm().c_str(), host_gregs.size(), host_fregs.size());
	}

	void OpEnd(shil_opcode* op)
	{
		for (u32 pi = 0; pi < pending_count; pi++)
		{
			const Sh4RegType reg = pending_flushes[pi];
			verify(!alloced[reg].write_back);
			Free(reg);
		}
		pending_count = 0;

		// Flush normally
		for (int reg = NextAlloced(-1); reg >= 0; reg = NextAlloced(reg))
		{
			FlushReg((Sh4RegType)reg, false);
		}

		// Hard flush all dirty regs. Useful for troubleshooting
//		while (!NoneAlloced())
//		{
//			const Sh4RegType it = (Sh4RegType)NextAlloced(-1);
//
//			if (alloced[it].dirty)
//				alloced[it].write_back = true;
//			FlushReg(it, true);
//		}

		// Final writebacks
		if (op >= &block->oplist.back())
		{
			FlushAllRegs(false);
			final_opend = true;
		}
	}

	void SetOpnum(int num)
	{
		fast_forwarding = true;
		for (int i = 0; i < num; i++)
		{
			shil_opcode* op = &block->oplist[i];
			OpBegin(op, i);
			OpEnd(op);
		}
		OpBegin(&block->oplist[num], num);
		fast_forwarding = false;
	}

	bool IsAllocAny(const shil_param& prm)
	{
		if (prm.is_reg())
		{
			bool rv = IsAllocAny(prm._reg);
			if (prm.count() != 1)
			{
				for (u32 i = 1;i < prm.count(); i++)
					verify(IsAllocAny((Sh4RegType)(prm._reg + i)) == rv);
			}
			return rv;
		}
		else
		{
			return false;
		}
	}

	bool IsAllocg(const shil_param& prm)
	{
		if (prm.is_reg())
		{
			if (prm.count() > MaxVecSize)
				return false;
			return IsAllocg(prm._reg);
		}
		else
		{
			return false;
		}
	}

	bool IsAllocf(const shil_param& prm)
	{
		if (prm.is_reg())
		{
			if (prm.count() > MaxVecSize)
				return false;
			return IsAllocf(prm._reg);
		}
		else
		{
			return false;
		}
	}

	nreg_t mapg(const shil_param& prm)
	{
		verify(IsAllocg(prm));
		verify(prm.count() == 1);
		return mapg(prm._reg);
	}

	// index: which register of a pair
	nregf_t mapf(const shil_param& prm, int index = 0)
	{
		verify(IsAllocf(prm));
		verify(index >= 0 && (u32)index < prm.count() && prm.count() <= MaxVecSize);
		return mapf((Sh4RegType)(prm._reg + index));
	}

	bool reg_used(nreg_t host_reg)
	{
		for (int reg = NextAlloced(-1); reg >= 0; reg = NextAlloced(reg))
			if ((nreg_t)alloced[reg].host_reg == host_reg && !IsFloat((Sh4RegType)reg))
				return true;
		return false;
	}

	bool regf_used(nregf_t host_reg)
	{
		for (int reg = NextAlloced(-1); reg >= 0; reg = NextAlloced(reg))
			if ((nregf_t)alloced[reg].host_reg == host_reg && IsFloat((Sh4RegType)reg))
				return true;
		return false;
	}

	void Cleanup() {
		verify(final_opend || block->oplist.empty());
		final_opend = false;
		FlushAllRegs(true);
		verify(NoneAlloced());
		verify(pending_count == 0);
		block = NULL;
		host_fregs.clear();
		host_gregs.clear();
	}

	virtual void Preload(u32 reg, nreg_t nreg) = 0;
	virtual void Writeback(u32 reg, nreg_t nreg) = 0;

	virtual void Preload_FPU(u32 reg, nregf_t nreg) = 0;
	virtual void Writeback_FPU(u32 reg, nregf_t nreg) = 0;

private:
	struct reg_alloc {
		u32 host_reg;
		u16 version;
		bool write_back;
		bool dirty;
	};
	static constexpr u32 MaxVecSize = AllocVec2 ? 2 : 1;

	bool IsFloat(Sh4RegType reg)
	{
		return reg >= reg_fr_0 && reg <= reg_xf_15;
	}

	nreg_t mapg(Sh4RegType reg)
	{
		verify(IsAlloced(reg));
		return (nreg_t)alloced[reg].host_reg;
	}

	nregf_t mapf(Sh4RegType reg)
	{
		verify(IsAlloced(reg));
		return (nregf_t)alloced[reg].host_reg;
	}

	bool IsAllocf(Sh4RegType reg)
	{
		if (!IsFloat(reg))
			return false;
		return IsAlloced(reg);
	}

	bool IsAllocg(Sh4RegType reg)
	{
		if (IsFloat(reg))
			return false;
		return IsAlloced(reg);
	}

	bool IsAllocAny(Sh4RegType reg)
	{
		return IsAllocg(reg) || IsAllocf(reg);
	}

	void WriteBackReg(Sh4RegType reg_num, struct reg_alloc& reg_alloc)
	{
		if (reg_alloc.write_back)
		{
			if (!fast_forwarding)
			{
				ssa_printf("WB %s.%d <- %cx\n", name_reg(reg_num).c_str(), reg_alloc.version, 'a' + reg_alloc.host_reg);
				if (IsFloat(reg_num))
					Writeback_FPU(reg_num, (nregf_t)reg_alloc.host_reg);
				else
					Writeback(reg_num, (nreg_t)reg_alloc.host_reg);
			}
			reg_alloc.write_back = false;
			reg_alloc.dirty = false;
		}
	}

	void FlushReg(Sh4RegType reg_num, bool hard)
	{
		if (IsAlloced(reg_num))
		{
			WriteBackReg(reg_num, alloced[reg_num]);
			if (hard)
			{
				u32 host_reg = alloced[reg_num].host_reg;
				Free(reg_num);
				if (IsFloat(reg_num))
					host_fregs.push_front((nregf_t)host_reg);
				else
					host_gregs.push_front((nreg_t)host_reg);
			}
		}
	}

	void FlushAllRegs(bool hard)
	{
		if (hard)
		{
			while (!NoneAlloced())
				FlushReg((Sh4RegType)NextAlloced(-1), true);
		}
		else
		{
			for (int reg = NextAlloced(-1); reg >= 0; reg = NextAlloced(reg))
				FlushReg((Sh4RegType)reg, false);
		}
	}

	void AllocSourceReg(const shil_param& param)
	{
		if (!param.is_reg() || param.count() > MaxVecSize)
			return;
		for (u32 i = 0; i < param.count(); i++)
		{
			const Sh4RegType sh4reg = (Sh4RegType)(param._reg + i);
			if (!IsAlloced(sh4reg))
			{
				u32 host_reg;
				if (param.is_r32i())
				{
					if (host_gregs.empty())
					{
						SpillReg(false, true);
						verify(!host_gregs.empty());
					}
					host_reg = host_gregs.back();
					host_gregs.pop_back();
				}
				else
				{
					if (host_fregs.empty())
					{
						SpillReg(true, true);
						verify(!host_fregs.empty());
					}
					host_reg = host_fregs.back();
					host_fregs.pop_back();
				}
				Alloc(sh4reg, { host_reg, (u16)param.version[i], false, false });
				if (!fast_forwarding)
				{
					ssa_printf("PL %s.%d -> %cx\n", name_reg(sh4reg).c_str(), param.version[i], 'a' + host_reg);
					if (IsFloat(sh4reg))
						Preload_FPU(sh4reg, (nregf_t)host_reg);
					else
						Preload(sh4reg, (nreg_t)host_reg);
				}
			}
		}
	}

	bool NeedsWriteBack(Sh4RegType reg, u32 version)
	{
		for (int i = opnum + 1; i < block->oplist.size(); i++)
		{
			shil_opcode* op = &block->oplist[i];
			// if a subsequent op needs all or some regs flushed to mem
			// TODO we could look at the ifb op to optimize what to flush
			if (op->op == shop_ifb || (mmu_enabled() && (op->op == shop_readm || op->op == shop_writem || op->op == shop_pref)))
				return true;
			if (op->op == shop_sync_sr && (/*reg == reg_sr_T ||*/ reg == reg_sr_status || reg == reg_old_sr_status || (reg >= reg_r0 && reg <= reg_r7)
					|| (reg >= reg_r0_Bank && reg <= reg_r7_Bank)))
				return true;
			if (op->op == shop_sync_fpscr && (reg == reg_fpscr || reg == reg_old_fpscr || (reg >= reg_fr_0 && reg <= reg_xf_15)))
				return true;
			if (op->op == shop_div1 && reg == reg_sr_status)
				return true;
			// if reg is used by a subsequent vector op that doesn't use reg allocation
			if (UsesReg(op, reg, version, true))
				return true;
			// no writeback needed if redefined
			if (DefsReg(op, reg, true))
				return false;
			if (DefsReg(op, reg, false))
				return false;
		}

		return true;
	}

	void AllocDestReg(const shil_param& param)
	{
		if (!param.is_reg() || param.count() > MaxVecSize)
			return;
		for (u32 i = 0; i < param.count(); i++)
		{
			const Sh4RegType sh4reg = (Sh4RegType)(param._reg + i);
			if (!IsAlloced(sh4reg))
			{
				u32 host_reg;
				if (param.is_r32i())
				{
					if (host_gregs.empty())
					{
						SpillReg(false, false);
						verify(!host_gregs.empty());
					}
					host_reg = host_gregs.back();
					host_gregs.pop_back();
				}
				else
				{
					if (host_fregs.empty())
					{
						SpillReg(true, false);
						verify(!host_fregs.empty());
					}
					host_reg = host_fregs.back();
					host_fregs.pop_back();
				}
				Alloc(sh4reg, { host_reg, (u16)param.version[i], NeedsWriteBack(sh4reg, param.version[i]), true });
				ssa_printf("   %s.%d -> %cx %s\n", name_reg(sh4reg).c_str(), param.version[i], 'a' + host_reg, alloced[sh4reg].write_back ? "(wb)" : "");
			}
			else
			{
				reg_alloc& reg = alloced[sh4reg];
				verify(!reg.write_back);
				reg.write_back = NeedsWriteBack(sh4reg, param.version[i]);
				reg.dirty = true;
				reg.version = param.version[i];
			}
			verify(alloced[sh4reg].dirty);
		}
	}

	void SpillReg(bool freg, bool source)
	{
		Sh4RegType spilled_reg = Sh4RegType::NoReg;
		int latest_use = -1;

		for (int ri = NextAlloced(-1); ri >= 0; ri = NextAlloced(ri))
		{
			const Sh4RegType reg_first = (Sh4RegType)ri;
			const reg_alloc& reg_second = alloced[ri];
			if (IsFloat(reg_first) != freg)
				continue;
			// Don't spill already spilled regs
			bool pending = false;
			for (u32 pi = 0; pi < pending_count; pi++)
				if (pending_flushes[pi] == reg_first)
				{
					pending = true;
					break;
				}
			if (pending)
				continue;

			// Don't spill current op scalar dest regs
			shil_opcode* op = &block->oplist[opnum];
			if (DefsReg(op, reg_first, false))
				continue;

			// Find the first use, but ignore vec ops
			int first_use = -1;
			for (int i = opnum + (source ? 0 : 1); i < block->oplist.size(); i++)
			{
				op = &block->oplist[i];
				// Vector ops don't use reg alloc
				if (UsesReg(op, reg_first, reg_second.version, false))
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
			if (first_use > latest_use && first_use > opnum)
			{
				latest_use = first_use;
				spilled_reg = reg_first;
			}
		}
		if (latest_use != -1)
		{
			ssa_printf("RegAlloc: non optimal alloc? reg %s used in op %d\n", name_reg(spilled_reg).c_str(), latest_use);
			spills++;
			// need to write-back if dirty so reload works
			if (alloced[spilled_reg].dirty)
				alloced[spilled_reg].write_back = true;
		}
		verify(spilled_reg != Sh4RegType::NoReg);

		if (source)
			FlushReg(spilled_reg, true);
		else
		{
			// Delay the eviction of spilled regs from the map due to dest register allocation.
			// It's possible that the same host reg is allocated to a source operand
			// and to the (future) dest operand. In this case we want to keep both mappings
			// until the current op is done.
			WriteBackReg(spilled_reg, alloced[spilled_reg]);
			u32 host_reg = alloced[spilled_reg].host_reg;
			if (IsFloat(spilled_reg))
				host_fregs.push_front((nregf_t)host_reg);
			else
				host_gregs.push_front((nreg_t)host_reg);
			verify(pending_count < sh4_reg_count);
			pending_flushes[pending_count++] = spilled_reg;
		}
	}

	bool IsVectorOp(shil_opcode* op)
	{
		return op->rs1.count() > MaxVecSize || op->rs2.count() > MaxVecSize || op->rs3.count() > MaxVecSize
				|| op->rd.count() > MaxVecSize || op->rd2.count() > MaxVecSize;
	}

	bool UsesReg(shil_opcode* op, Sh4RegType reg, u32 version, bool vector)
	{
		if (op->rs1.is_reg() && reg >= op->rs1._reg && reg < (Sh4RegType)(op->rs1._reg + op->rs1.count())
				&& version == op->rs1.version[reg - op->rs1._reg]
				&& vector == (op->rs1.count() > MaxVecSize))
			return true;
		if (op->rs2.is_reg() && reg >= op->rs2._reg && reg < (Sh4RegType)(op->rs2._reg + op->rs2.count())
				&& version == op->rs2.version[reg - op->rs2._reg]
				&& vector == (op->rs2.count() > MaxVecSize))
			return true;
		if (op->rs3.is_reg() && reg >= op->rs3._reg && reg < (Sh4RegType)(op->rs3._reg + op->rs3.count())
				&& version == op->rs3.version[reg - op->rs3._reg]
				&& vector == (op->rs3.count() > MaxVecSize))
			return true;

		return false;
	}

	bool DefsReg(shil_opcode* op, Sh4RegType reg, bool vector)
	{
		if (op->rd.is_reg() && reg >= op->rd._reg && reg < (Sh4RegType)(op->rd._reg + op->rd.count())
				&& vector == (op->rd.count() > MaxVecSize))
			return true;
		if (op->rd2.is_reg() && reg >= op->rd2._reg && reg < (Sh4RegType)(op->rd2._reg + op->rd2.count())
				&& vector == (op->rd2.count() > MaxVecSize))
			return true;
		return false;
	}

	RuntimeBlockInfo* block = NULL;
	/* The host registers free, the next taken from the back (a ring: one
	 * given back goes to the front) */
	template<typename T>
	struct HostRegs
	{
		enum { Max = 64 };
		T regs[Max];
		u32 head = 0, n = 0;
		bool empty() const { return n == 0; }
		size_t size() const { return n; }
		void clear() { head = n = 0; }
		void push_back(T v) { verify(n < Max); regs[(head + n++) % Max] = v; }
		void push_front(T v) { verify(n < Max); head = (head + Max - 1) % Max; regs[head] = v; n++; }
		T back() const { return regs[(head + n - 1) % Max]; }
		void pop_back() { n--; }
	};
	HostRegs<nreg_t> host_gregs;
	HostRegs<nregf_t> host_fregs;
	/* Registers spilled for a destination, let go at the end of the op */
	Sh4RegType pending_flushes[sh4_reg_count];
	u32 pending_count = 0;
	/* The guest registers in host ones: a register's entry, and a bit each
	 * for those that have one (in register order, as they are gone through) */
	static_assert(sh4_reg_count <= 128, "two words of bits");
	reg_alloc alloced[sh4_reg_count] = {};
	u64 alloced_bits[2] = {};
	bool IsAlloced(Sh4RegType reg) const
	{
		return reg >= 0 && reg < sh4_reg_count && ((alloced_bits[reg >> 6] >> (reg & 63)) & 1) != 0;
	}
	/* the first one with a host register after @after (-1: from the start); -1 if none */
	int NextAlloced(int after) const
	{
		for (int w = (after + 1) >> 6; w < 2; w++)
		{
			u64 bits = alloced_bits[w];
			if (w == (after + 1) >> 6)
				bits &= ~0ull << ((after + 1) & 63);
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
	bool NoneAlloced() const { return (alloced_bits[0] | alloced_bits[1]) == 0; }
	void Alloc(Sh4RegType reg, const reg_alloc& a)
	{
		alloced[reg] = a;
		alloced_bits[reg >> 6] |= 1ull << (reg & 63);
	}
	void Free(Sh4RegType reg)
	{
		alloced[reg] = reg_alloc();
		alloced_bits[reg >> 6] &= ~(1ull << (reg & 63));
	}
	int opnum = 0;

	bool final_opend = false;
	bool fast_forwarding = false;
public:
	u32 spills = 0;
};

#undef printf
