/* The wait sites (core/hw/sh4/dyna/wait_site.c): which random blocks get a
 * site and which can write, sites given out until there are none, and
 * sh4_wait_check() on random passes - each answer hashed, against what the
 * C++ it replaced answered (sh tools/sh4/wait_site_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/dyna/shil.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/wait_site.h"

static Sh4RCB rcb;
Sh4RCB *p_sh4rcb = &rcb;
static u64 slice;
static u32 rng = 1;
static unsigned h = 2166136261u;
static shil_opcode ops[16];

#ifdef __cplusplus
extern "C"
#endif
u64 sh4_sched_now64(void) { return slice; }

static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

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

static void param(shil_param *p, u32 kind)
{
	memset(p, 0, sizeof(*p));
	switch (kind % 6)
	{
	case 0:
		p->type = FMT_NULL;
		p->_imm = 0xFFFFFFFF;
		break;
	case 1:
		p->type = FMT_IMM;
		p->_imm = rnd() & 3 ? (rnd() & 7) : 0u - (rnd() & 7);
		break;
	case 2:
		/* (now and then one numbered as an integer register is) */
		p->type = rnd() & 7 ? FMT_F32 : FMT_F64;
		p->_imm = rnd() & 3 ? reg_fr_0 + (rnd() & 15) : reg_r0 + (rnd() & 3);
		break;
	default:
		p->type = FMT_I32;
		p->_imm = rnd() & 3 ? reg_r0 + (rnd() & 3) : rnd() & 1 ? (u32)reg_r15 : (u32)reg_sr_T;
		break;
	}
}

static void random_block(RuntimeBlockInfo *b)
{
	static const shilop kinds[] = { shop_add, shop_add, shop_sub, shop_sub, shop_mov32, shop_and,
		shop_writem, shop_ifb, shop_pref, shop_sync_sr, shop_sync_fpscr, shop_readm };
	u32 i, n = 1 + rnd() % 8;
	memset(b, 0, sizeof(*b));
	b->vaddr = b->addr = 0x8C010000 + (rnd() & 0xFE);
	b->BranchBlock = rnd() & 1 ? b->vaddr : rnd() & 1 ? b->vaddr + 2 : b->vaddr - 4;
	b->oplist = ops;
	b->op_count = n;
	for (i = 0; i < n; i++)
	{
		shil_opcode *op = &ops[i];
		u32 r = rnd();
		memset(op, 0, sizeof(*op));
		/* mostly arithmetic, so that the steps are looked at */
		op->op = (r & 7) ? kinds[(r >> 3) % 6] : kinds[(r >> 3) % (sizeof(kinds) / sizeof(kinds[0]))];
		param(&op->rd, 1 + (r >> 8) % 5);
		param(&op->rd2, (r >> 12) % 16 ? 0 : 3);
		param(&op->rs1, (r >> 16) % 4 ? 3 : (r >> 18) % 6);
		if ((r >> 20) % 2 && op->rd.type == FMT_I32)
			op->rs1 = op->rd;
		param(&op->rs2, (r >> 21) % 3 ? 1 : (r >> 23) % 6);
		param(&op->rs3, 0);
	}
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 100000, t;
	static RuntimeBlockInfo b;
	WaitSite *site;
	u32 given = 0;
	s32 cycles;

	/* blocks: whether they write, and whether they are watched */
	for (t = 0; t < n; t++)
	{
		random_block(&b);
		mix(sh4_block_writes(&b));
		site = sh4_wait_site(&b);
		mix(site != NULL);
		if (site)
		{
			mix(site->skip | site->backoff | site->gen | site->valid | site->cycles);
			given++;
			/* a site used, and given out again after a reset */
			if (rnd() & 1)
			{
				sh4_wait_check(site, 1000);
				sh4_wait_check(site, 900);
				p_sh4rcb->cntx.r[0]++;
				sh4_wait_check(site, 800);
			}
		}
		if (rnd() % 64 == 0)
			sh4_wait_sites_reset();
	}
	/* sites given out until there are no more (65536), then none */
	sh4_wait_sites_reset();
	b.op_count = 0;
	b.BranchBlock = b.vaddr + 2;
	for (t = 0; t < 70000; t++)
		mix(sh4_wait_site(&b) != NULL);
	sh4_wait_sites_reset();

	/* passes through one site: the slice, the generation and the registers
	 * now and then changed, the cycles counted down */
	b.op_count = 0;
	site = sh4_wait_site(&b);
	cycles = 448 * 200;
	for (t = 0; t < n; t++)
	{
		u32 r = rnd();
		/* stretches of a loop with work to do: something changes every pass */
		const int busy = (t / 1000) % 4 == 3;
		s32 cost = 1 + (s32)((r >> 4) % (r & 1 ? 40 : 4000));
		if ((r >> 16) % 32 == 0)
		{
			slice++;
			cycles = 448 * (1 + (s32)(rnd() % 400));
		}
		if ((r >> 21) % 8 == 0)
			sh4_write_gen++;
		if (busy || (r >> 24) % 8 == 0)
			p_sh4rcb->cntx.r[rnd() & 15] ^= 1;
		if ((r >> 27) % 16 == 0)
			p_sh4rcb->cntx.fpscr.full ^= 1u << (rnd() % 22);
		cycles -= (r >> 29) == 0 ? cost * 4 : 0;
		cycles = sh4_wait_check(site, cycles);
		mix((u32)cycles);
		mix(site->skip);
		mix(site->backoff);
		mix(site->valid);
		mix(site->gen);
		mix((u32)site->slice);
		mix((u32)site->cycles);
		cycles -= cost;
		/* (now and then the counter is let run on below 0) */
		if (cycles <= 0 && ((r >> 8) % 8 || cycles < -448 * 50))
		{
			slice++;
			cycles += 448 * 200;
		}
	}
	printf("%d blocks, %u watched, %08x\n", n, given, h);
	return 0;
}
