#include "wait_site.h"
#include "shil.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_sched.h"
#include <stdlib.h>
#include <string.h>

#if FEAT_SHREC != DYNAREC_NONE

u32 sh4_write_gen;

/* A block of sites at a time, never moved: recompiled code has their addresses */
#define WAIT_SITES_PER_CHUNK 1024
#define WAIT_SITE_CHUNKS 64
static WaitSite *wait_sites[WAIT_SITE_CHUNKS];
static u32 wait_site_chunks;
static u32 wait_sites_used;

/* Whether a site in @block could never find the processor waiting.
 *
 * A block that stores moves sh4_write_gen on every time it starts, so two
 * passes through its site never have the same generation.
 *
 * A block that goes back to its own start, and in which the only thing
 * done to some register is to add constants that do not cancel out: two
 * passes in a row never have the same registers. If such a loop is left
 * and come back to by a loop around it, that loop has a site of its own. */
static bool never_found_waiting(const RuntimeBlockInfo *block)
{
	const shil_opcode *ops = block->oplist;
	u32 n = block->op_count, i, reg;

	if (sh4_block_writes(block))
		return true;
	if (block->BranchBlock != block->vaddr)
		return false;
	for (i = 0; i < n; i++)
		if (ops[i].op == shop_sync_sr || ops[i].op == shop_sync_fpscr)
			return false;		/* can change registers no parameter names */
	for (reg = reg_r0; reg <= reg_r15; reg++)
	{
		u32 step = 0;
		bool only_steps = true;
		for (i = 0; i < n; i++)
		{
			const shil_opcode *op = &ops[i];
			const bool rd = shil_is_reg(&op->rd) && shil_reg(&op->rd) == reg;
			const bool rd2 = shil_is_reg(&op->rd2) && shil_reg(&op->rd2) == reg;
			if (!rd && !rd2)
				continue;
			if (rd && !rd2 && (op->op == shop_add || op->op == shop_sub)
					&& shil_is_r32i(&op->rd) && shil_is_r32i(&op->rs1) && shil_reg(&op->rs1) == reg && shil_is_imm(&op->rs2))
				step += op->op == shop_add ? op->rs2._imm : 0u - op->rs2._imm;
			else
			{
				only_steps = false;
				break;
			}
		}
		if (only_steps && step != 0)
			return true;
	}
	return false;
}

WaitSite *sh4_wait_site(const RuntimeBlockInfo *block)
{
	WaitSite *site;

	if (never_found_waiting(block))
		return NULL;
	if (wait_sites_used == wait_site_chunks * WAIT_SITES_PER_CHUNK)
	{
		if (wait_site_chunks == WAIT_SITE_CHUNKS)
			return NULL;		/* 65536 loops: the rest go unwatched */
		/* (kept from cache flush to cache flush, and given back never) */
		if (!(wait_sites[wait_site_chunks] = (WaitSite *)malloc(WAIT_SITES_PER_CHUNK * sizeof(WaitSite))))
			return NULL;
		wait_site_chunks++;
	}
	site = &wait_sites[wait_sites_used / WAIT_SITES_PER_CHUNK][wait_sites_used % WAIT_SITES_PER_CHUNK];
	wait_sites_used++;
	memset(site, 0, sizeof(*site));
	return site;
}

void sh4_wait_sites_reset(void)
{
	wait_sites_used = 0;
}

bool sh4_block_writes(const RuntimeBlockInfo *block)
{
	u32 i;
	for (i = 0; i < block->op_count; i++)
	{
		const shil_opcode *op = &block->oplist[i];
		if (op->op == shop_writem || op->op == shop_ifb || op->op == shop_pref)
			return true;
	}
	return false;
}

s32 sh4_wait_check(WaitSite *site, s32 cycles)
{
	const u64 now = sh4_sched_now64();

	if (site->valid && site->slice == now)
	{
		if (site->gen == sh4_write_gen && !memcmp(site->regs, &Sh4cntx, sizeof(site->regs)))
		{
			s32 pass;
			site->backoff = 0;
			site->skip = 0;
			/* What a pass costs. Every pass left would cost the same, and
			 * the counter only falls on the way round, so each of the
			 * passes skipped would have ended with at least one cycle left
			 * and the slice would not have ended in it, whichever way a
			 * recompiler tests the counter against 0. The pass that is left
			 * is run, and ends the slice where it would have ended. */
			pass = site->cycles - cycles;
			if (pass > 0 && cycles > pass)
				cycles -= (cycles - 1) / pass * pass;
			site->cycles = cycles;
			return cycles;
		}
		/* it wrote something, or its registers moved: a loop with work to do */
		site->backoff = site->backoff * 2 + 1 < 1023u ? site->backoff * 2 + 1 : 1023u;
		site->skip = site->backoff;
		site->valid = false;
		return cycles;
	}
	/* nothing from this time slice to compare with: take it, and look at the next pass */
	memcpy(site->regs, &Sh4cntx, sizeof(site->regs));
	site->gen = sh4_write_gen;
	site->slice = now;
	site->cycles = cycles;
	site->valid = true;
	site->skip = 0;
	return cycles;
}

#endif
