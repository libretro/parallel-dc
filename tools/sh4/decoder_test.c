/* The SH4 decoder (core/hw/sh4/dyna/decoder.c) on random code, each block
 * and its operations hashed, against what the C++ decoder it replaced made
 * of the same code (sh tools/sh4/decoder_run.sh). */
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
bool bm_BlockIsIdle(RuntimeBlockInfo *block) { (void)block; return false; }
void ngen_GetFeatures(ngen_features *dst) { *dst = features; }
void os_DebugBreak(void) { fprintf(stderr, "verify failed\n"); abort(); }

shil_opcode *bm_OpPush(RuntimeBlockInfo *block)
{
	if (block->op_count == block->op_room)
	{
		block->op_room = block->op_room ? block->op_room * 2 : 64;
		block->oplist = (shil_opcode *)realloc(block->oplist, block->op_room * sizeof(shil_opcode));
	}
	return &block->oplist[block->op_count++];
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

static void mix_block(const RuntimeBlockInfo *b, int r)
{
	u32 i;
	mix((u32)r);
	mix(b->op_count);
	mix(b->guest_cycles);
	mix(b->guest_opcodes);
	mix(b->sh4_code_size);
	mix(b->NextBlock);
	mix(b->BranchBlock);
	mix((u32)b->BlockType);
	mix(b->has_jcond);
	mix(b->has_fpu_op);
	for (i = 0; i < b->op_count; i++)
	{
		const shil_opcode *x = &b->oplist[i];
		mix((u32)x->op);
		/* (what ifb's flags were was never set) */
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

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 20000, t;
	static RuntimeBlockInfo a;
	BuildOpcodeTables();
	for (t = 0; t < 0x10000; t++)
		if (OpDesc[t]->rec_oph != NULL || OpDesc[t]->decode != 0)
			handled[handled_count++] = (u16)t;
	for (t = 0; t < n; t++)
	{
		u32 i, pc;
		int ra;
		fpscr_t cfg;
		/* random code, with runs of the shapes the decoder looks for */
		for (i = 0; i < 0x200; i++)
		{
			u32 r = rnd();
			code[i] = (r >> 20) % 4 ? handled[rnd() % handled_count] : (u16)r;
			if ((r >> 16) % 8 == 0)
				code[i] = (u16)(0x4024 | (rnd() & 0x0F00));	/* rotcl */
			else if ((r >> 16) % 8 == 1)
				code[i] = (u16)(0x3004 | (rnd() & 0x0FF0));	/* div1 */
			else if ((r >> 16) % 16 == 2)
				code[i] = (u16)(rnd() & 1 ? 0x0019 : (0x2007 | (rnd() & 0x0FF0)));	/* div0u, div0s */
			else if ((r >> 16) % 32 == 4)
				code[i] = (u16)(rnd() & 1 ? 0xF3FD : 0xFBFD);	/* fschg, frchg */
			else if ((r >> 16) % 64 == 3 && i < 0x200 - 66)
			{
				/* a whole 32-bit division, as the decoder matches it, or
				 * one of its registers off in a step */
				u32 q = rnd(), k, rot = (q >> 8) & 0xF, m = (q >> 12) & 0xF, nn = (q >> 16) & 0xF;
				u32 off = (q >> 20) % 4 ? 99 : (q >> 24) % 66;
				code[i] = (u16)(q & 1 ? 0x0019 : (0x2007 | (m << 4) | (nn << 8)));
				for (k = 0; k < 32; k++)
				{
					code[i + 1 + 2 * k] = (u16)(0x4024 | (rot << 8));
					code[i + 2 + 2 * k] = (u16)(0x3004 | (m << 4) | (nn << 8));
				}
				if (off < 65)
					code[i + 1 + off] ^= 0x0100;
				i += 65;
			}
		}
		pc = code_base + (rnd() % 64) * 2;
		cfg.full = 0;
		cfg.PR = rnd() & 1;
		cfg.SZ = rnd() & 1;
		cfg.RM = rnd() & 3;
		settings.dynarec.AccurateTiming = (rnd() & 1) != 0;
		settings.dynarec.DisableDivMatching = (rnd() & 3) == 0;
		features.OnlyDynamicEnds = (rnd() & 7) == 0;
		features.InterpreterFallback = (rnd() & 7) == 0;
		p_sh4rcb->cntx.sr.FD = 0;
		setup(&a, pc, cfg);
		ra = dec_DecodeBlock(&a, SH4_TIMESLICE / 2);
		mix_block(&a, ra);
	}
	free(a.oplist);
	printf("%u opcodes decoded, %d blocks, %08x\n", handled_count, n, h);
	return 0;
}
