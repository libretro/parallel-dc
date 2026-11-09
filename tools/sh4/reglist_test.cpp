/* The register allocator's lists of the registers an op writes and reads
 * (core/hw/sh4/dyna/regalloc.h: ShilRegList) against what they replace, a
 * std::set<shil_param> ordered by the key type + 32 * register, largest
 * first: random params put in, looked up and taken out, and the two
 * walked in the same order after each step, so that the allocator sees
 * the registers as it did and the code it makes is the same. (The
 * allocator is the recompilers' with OLD_REGALLOC; the headers before it
 * are theirs.) */
#include "types.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/dyna/regalloc.h"
#include <set>
#include <stdio.h>
#include <stdlib.h>

/* (verify()'s, which the core has) */
void os_DebugBreak()
{
	abort();
}

struct ByKey
{
	bool operator()(const shil_param &lhs, const shil_param &rhs) const
	{
		const u32 rhsv = rhs.type + rhs._reg * 32;
		const u32 lhsv = lhs.type + lhs._reg * 32;
		return rhsv < lhsv;
	}
};

static u32 seed = 12345;
static u32 rnd(u32 n)
{
	seed = seed * 1103515245u + 12345u;
	return (seed >> 8) % n;
}

static shil_param random_param(void)
{
	static const u32 types[] = { FMT_NULL, FMT_IMM, FMT_I32, FMT_F32, FMT_F64, FMT_V2, FMT_V4, FMT_V16 };
	shil_param p;
	p.type = types[rnd(8)];
	/* (FMT_NULL's register is its immediate's 0xFFFFFFFF, as made) */
	p._imm = p.type == FMT_NULL ? 0xFFFFFFFFu : rnd(4) == 0 ? rnd(0x100) : rnd(80);
	return p;
}

static bool same(const ShilRegList &l, const std::set<shil_param, ByKey> &s)
{
	u32 i = 0;
	if (s.size() != l.n)
		return false;
	for (std::set<shil_param, ByKey>::const_iterator it = s.begin(); it != s.end(); ++it, i++)
		if (it->type != l.v[i].type || it->_imm != l.v[i]._imm)
			return false;
	return true;
}

int main(int argc, char **argv)
{
	const long trials = argc > 1 ? atol(argv[1]) : 2000000;
	long t;
	for (t = 0; t < trials; t++)
	{
		ShilRegList l;
		std::set<shil_param, ByKey> s;
		const u32 inserts = 1 + rnd(SHIL_REG_LIST_MAX);
		u32 i;
		l.n = 0;
		for (i = 0; i < inserts; i++)
		{
			const shil_param p = random_param();
			if (s.size() == SHIL_REG_LIST_MAX && s.find(p) == s.end())
				break;
			s.insert(p);
			shil_reg_list_insert(&l, &p);
			if (!same(l, s))
			{
				printf("reglist_test: FAIL: trial %ld, insert %u\n", t, i);
				return 1;
			}
		}
		for (i = 0; i < 6; i++)
		{
			const shil_param p = rnd(2) && l.n ? l.v[rnd(l.n)] : random_param();
			const int at = shil_reg_list_find(&l, &p);
			if ((at >= 0) != (s.count(p) != 0))
			{
				printf("reglist_test: FAIL: trial %ld, a lookup\n", t);
				return 1;
			}
			if (at >= 0)
			{
				s.erase(p);
				shil_reg_list_erase(&l, (u32)at);
				if (!same(l, s))
				{
					printf("reglist_test: FAIL: trial %ld, an erase\n", t);
					return 1;
				}
			}
		}
	}
	printf("reglist_test: %ld trials, the same as std::set's: PASS\n", trials);
	return 0;
}
