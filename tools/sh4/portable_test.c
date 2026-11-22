/* The portable recompiler (core/rec-portable/rec_portable.c) against the
 * interpreter: blocks of random code, each run from the same random state
 * by the interpreter's handlers, an instruction at a time, and by the
 * recompiler, by way of its main loop - and the registers and memory each
 * leaves compared (sh tools/sh4/portable_run.sh). Every block is compiled
 * with its checks and without, optimized and not.
 *
 * The code is at 0x8C010000, in the upper half of 128 KB of memory that
 * reads of every address go to; writes go to the lower half, so that no
 * block writes over its own code. Instructions that raise exceptions, put
 * the processor to sleep or need what is not here (the MMU, the store
 * queues, the BIOS) are not in the code; a block in which one is raised
 * anyway is not compared, and counted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_interpreter.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/decoder.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/dyna/ssa.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/sh4/interpr/sh4_opcodes.h"

/* (sh4_core.h's names of the registers: the context's members here) */
#undef r
#undef r_bank
#undef gbr
#undef ssr
#undef spc
#undef sgr
#undef dbr
#undef vbr
#undef mac
#undef pr
#undef fpul
#undef sr
#undef fpscr
#undef old_sr
#undef old_fpscr

#define CODE_BASE 0x8C010000u
#define CODE_WORDS 0x400

static u8 mem[0x20000];
static u8 mem_start[0x20000];
static Sh4RCB rcb_room;
static u32 rng = 1;
/* an exception was raised */
static int raised;

RegisterStruct CCN[SH4_CCN_REGS];
bool mmu_strict;
settings_t settings;
unsigned RAM_MASK = 0x1FFFF;
VArray2 mem_b;
retro_log_printf_t log_cb;
f32_x2 sin_table[0x10000];
u32 CCN_QACR_TR[2];
TLB_Entry UTLB[64];
extern int cycle_counter;

static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* -- The memory --------------------------------------------------------- */

static u32 rd_at(u32 a, u32 size) { return (a & 0x1FFFF) & ~(size - 1); }
static u32 wr_at(u32 a, u32 size) { return (a & 0xFFFF) & ~(size - 1); }

static u8 DYNACALL rd8(u32 a) { return mem[rd_at(a, 1)]; }
static u16 DYNACALL rd16(u32 a) { u16 v; memcpy(&v, &mem[rd_at(a, 2)], 2); return v; }
static u32 DYNACALL rd32(u32 a) { u32 v; memcpy(&v, &mem[rd_at(a, 4)], 4); return v; }
static u64 DYNACALL rd64(u32 a) { u64 v; memcpy(&v, &mem[rd_at(a, 8)], 8); return v; }
static void DYNACALL wr8(u32 a, u8 d) { mem[wr_at(a, 1)] = d; }
static void DYNACALL wr16(u32 a, u16 d) { memcpy(&mem[wr_at(a, 2)], &d, 2); }
static void DYNACALL wr32(u32 a, u32 d) { memcpy(&mem[wr_at(a, 4)], &d, 4); }
static void DYNACALL wr64(u32 a, u64 d) { memcpy(&mem[wr_at(a, 8)], &d, 8); }
ReadMem8Func ReadMem8 = rd8;
ReadMem16Func ReadMem16 = rd16;
ReadMem16Func IReadMem16 = rd16;
ReadMem32Func ReadMem32 = rd32;
ReadMem64Func ReadMem64 = rd64;
WriteMem8Func WriteMem8 = wr8;
WriteMem16Func WriteMem16 = wr16;
WriteMem32Func WriteMem32 = wr32;
WriteMem64Func WriteMem64 = wr64;

u8 *GetMemPtr(u32 addr, u32 size)
{
	return &mem[rd_at(addr, 1) + size <= sizeof(mem) ? rd_at(addr, 1) : 0];
}

/* -- What the interpreter and the recompiler call of the rest ----------- */

static void nothere(const char *what)
{
	printf("FAIL: %s was called\n", what);
	exit(1);
}

void os_DebugBreak(void) { nothere("verify"); }
bool Do_Exception(u32 epc, u32 expEvn, u32 CallVect) { (void)epc; (void)expEvn; (void)CallVect; raised = 1; return true; }
void DoMMUException(u32 addr, u32 mmu_error, u32 access_type) { (void)addr; (void)mmu_error; (void)access_type; nothere("DoMMUException"); }
int UpdateINTC(void) { return 0; }
bool SRdecode(void) { return false; }
int UpdateSystem_INTC(void) { return 0; }
u64 sh4_sched_now64(void) { return 0; }
void reios_trap(u32 op) { (void)op; nothere("reios_trap"); }
void DYNACALL TAWriteSQ(u32 address, u8 *sqb) { (void)address; (void)sqb; nothere("TAWriteSQ"); }
void WriteMemBlock_nommu_sq(u32 dst, u32 *src) { (void)dst; (void)src; nothere("WriteMemBlock_nommu_sq"); }
bool UTLB_Sync(u32 entry) { (void)entry; nothere("UTLB_Sync"); return false; }
bool mmu_TranslateSQW(u32 adr, u32 *out) { (void)adr; (void)out; nothere("mmu_TranslateSQW"); return false; }
u32 mmu_data_translation_r8(u32 va, u32 *rv) { (void)va; (void)rv; nothere("the MMU"); return 0; }
u32 mmu_data_translation_r16(u32 va, u32 *rv) { (void)va; (void)rv; nothere("the MMU"); return 0; }
u32 mmu_data_translation_r32(u32 va, u32 *rv) { (void)va; (void)rv; nothere("the MMU"); return 0; }
u32 mmu_data_translation_r64(u32 va, u32 *rv) { (void)va; (void)rv; nothere("the MMU"); return 0; }
u32 mmu_data_translation_w8(u32 va, u32 *rv) { (void)va; (void)rv; nothere("the MMU"); return 0; }
u32 mmu_data_translation_w16(u32 va, u32 *rv) { (void)va; (void)rv; nothere("the MMU"); return 0; }
u32 mmu_data_translation_w32(u32 va, u32 *rv) { (void)va; (void)rv; nothere("the MMU"); return 0; }
u32 mmu_data_translation_w64(u32 va, u32 *rv) { (void)va; (void)rv; nothere("the MMU"); return 0; }
u8 DYNACALL _vmem_ReadMem8(u32 a) { (void)a; nothere("_vmem_ReadMem8"); return 0; }
u16 DYNACALL _vmem_ReadMem16(u32 a) { (void)a; nothere("_vmem_ReadMem16"); return 0; }
u32 DYNACALL _vmem_ReadMem32(u32 a) { (void)a; nothere("_vmem_ReadMem32"); return 0; }
u64 DYNACALL _vmem_ReadMem64(u32 a) { (void)a; nothere("_vmem_ReadMem64"); return 0; }
void DYNACALL _vmem_WriteMem8(u32 a, u8 d) { (void)a; (void)d; nothere("_vmem_WriteMem8"); }
void DYNACALL _vmem_WriteMem16(u32 a, u16 d) { (void)a; (void)d; nothere("_vmem_WriteMem16"); }
void DYNACALL _vmem_WriteMem32(u32 a, u32 d) { (void)a; (void)d; nothere("_vmem_WriteMem32"); }
void DYNACALL _vmem_WriteMem64(u32 a, u64 d) { (void)a; (void)d; nothere("_vmem_WriteMem64"); }

/* The interpreter's (interpr/sh4_interpreter.c): the instruction in the
 * delay slot */
static int slots_run;

void ExecuteDelayslot(void)
{
	slots_run++;
	const u32 op = IReadMem16(next_pc);
	next_pc += 2;
	sh4_in_delay_slot = true;
	OpPtr[op](op);
	sh4_in_delay_slot = false;
}

void ExecuteDelayslot_RTE(void) { nothere("ExecuteDelayslot_RTE"); }

/* -- What the recompiler calls of the driver and the block manager ------ */

static u8 cache[1 << 20];
static u32 cache_used;
static DynarecCodeEntryPtr to_run;
static void missing(void) { }
void (*ngen_FailedToFindBlock)(void) = missing;

void *emit_GetCCPtr(void) { return cache + cache_used; }
u32 emit_FreeSpace(void) { return (u32)sizeof(cache) - cache_used; }
void emit_Skip(u32 sz) { cache_used += sz; }

/* the block to run, once; then no block, and the main loop is to stop */
DynarecCodeEntryPtr DYNACALL bm_GetCodeByVAddr(u32 addr)
{
	DynarecCodeEntryPtr code = to_run;
	(void)addr;
	to_run = NULL;
	return code != NULL ? code : ngen_FailedToFindBlock;
}

DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock(u32 pc)
{
	(void)pc;
	Sh4cntx.CpuRunning = 0;
	cycle_counter = 0;
	return NULL;
}

DynarecCodeEntryPtr DYNACALL rdv_BlockCheckFail(u32 addr) { (void)addr; nothere("rdv_BlockCheckFail"); return NULL; }
bool bm_BlockIsIdle(RuntimeBlockInfo *block) { (void)block; return false; }

shil_opcode *bm_OpPush(RuntimeBlockInfo *block)
{
	if (block->op_count == block->op_room)
	{
		block->op_room = block->op_room ? block->op_room * 2 : 64;
		block->oplist = (shil_opcode *)realloc(block->oplist, block->op_room * sizeof(shil_opcode));
	}
	return &block->oplist[block->op_count++];
}

shil_opcode *bm_OpInsert(RuntimeBlockInfo *block, u32 at)
{
	bm_OpPush(block);
	memmove(&block->oplist[at + 1], &block->oplist[at], (block->op_count - 1 - at) * sizeof(shil_opcode));
	return &block->oplist[at];
}

void bm_OpErase(RuntimeBlockInfo *block, u32 at)
{
	memmove(&block->oplist[at], &block->oplist[at + 1], (block->op_count - 1 - at) * sizeof(shil_opcode));
	block->op_count--;
}

/* -- The code and the state --------------------------------------------- */

static u16 allowed[0x10000];
static u32 allowed_count;

static bool is_branch(u32 op)
{
	return OPCODE_SETPC(OpDesc[op]->type);
}

/* What is left out: what raises an exception or stops, what the MMU, the
 * store queues, the caches or the BIOS have to do with, and what writes
 * SR (its bank of registers, its interrupt mask: the interpreter calls
 * UpdateSR() and the recompiler too, but the block ends there) */
static bool left_out(u32 op)
{
	static const char *const names[] =
	{
		"trapa", "sleep", "rte", "ldtlb", "pref", "ocbi", "ocbp", "ocbwb", "movca",
		"ldc ", "ldc.l", "missing", "reios_trap", NULL
	};
	const char *d = OpDesc[op]->diss;
	int i;
	if (OpDesc[op]->oph == iNotImplemented || d == NULL)
		return true;
	for (i = 0; names[i] != NULL; i++)
		if (strncmp(d, names[i], strlen(names[i])) == 0)
			return true;
	return false;
}

/* Whether @op may be in the delay slot of @branch: not a branch, nor what
 * reads or writes PR in that of one that sets it (the interpreter sets it
 * after the slot, the decoder before: which the SH4 does is not known) */
static bool slot_ok(u32 branch, u32 op)
{
	const char *b = OpDesc[branch]->diss, *d = OpDesc[op]->diss;
	if (!is_branch(branch) || !(OpDesc[branch]->type & Delayslot))
		return true;
	if (is_branch(op))
		return false;
	if (strncmp(b, "bsr", 3) == 0 || strncmp(b, "jsr", 3) == 0)
		return strstr(d, "PR") == NULL;
	return true;
}

static void random_code(void)
{
	u32 i;
	for (i = 0; i < CODE_WORDS; i++)
	{
		u32 op;
		do
			op = allowed[rnd() % allowed_count];
		while (i > 0 && !slot_ok(mem[0x10000 + 2 * (i - 1)] | mem[0x10001 + 2 * (i - 1)] << 8, op));
		/* fewer branches: longer blocks */
		if (is_branch(op) && rnd() % 4 != 0)
			op = 0x0009;	/* nop */
		/* a multiplication by a constant, for the optimizer to simplify:
		 * mov #-1..2,Rm, then muls.w, mulu.w or mul.l Rm,Rn */
		if (rnd() % 32 == 0 && i + 1 < CODE_WORDS)
		{
			static const u16 muls[3] = { 0x200F, 0x200E, 0x0007 };
			const u32 m = rnd() & 15;
			op = 0xE000 | m << 8 | ((rnd() % 4 - 1) & 0xFF);
			mem[0x10000 + 2 * i] = (u8)op;
			mem[0x10001 + 2 * i] = (u8)(op >> 8);
			i++;
			op = muls[rnd() % 3] | (rnd() & 15) << 8 | m << 4;
		}
		mem[0x10000 + 2 * i] = (u8)op;
		mem[0x10001 + 2 * i] = (u8)(op >> 8);
	}
}

/* An address in the memory, or anything */
static u32 random_word(void)
{
	const u32 r = rnd();
	switch (r & 3)
	{
	case 0:
		return 0x8C000000u | (rnd() & 0xFFFC);
	case 1:
		return rnd() & 0xFF;
	default:
		return rnd();
	}
}

static f32 random_float(void)
{
	static const f32 nice[] = { 0.0f, 1.0f, -1.0f, 0.5f, 2.0f, 3.0f, -0.25f, 100.0f, 1e-3f, 65536.0f };
	const u32 r = rnd();
	f32 v;
	if (r & 1)
		return nice[(r >> 1) % (sizeof(nice) / sizeof(nice[0]))];
	v = (f32)(s32)(rnd() & 0xFFFFF) / (f32)(1 + (rnd() & 0xFFF));
	return (r & 2) ? -v : v;
}

static void random_state(void)
{
	u32 i, fp;
	memset(&Sh4cntx, 0, sizeof(Sh4cntx));
	for (i = 0; i < 16; i++)
		Sh4cntx.r[i] = random_word();
	for (i = 0; i < 8; i++)
		Sh4cntx.r_bank[i] = random_word();
	for (i = 0; i < 32; i++)
		Sh4cntx.xffr[i] = random_float();
	Sh4cntx.mac.l = rnd();
	Sh4cntx.mac.h = rnd();
	Sh4cntx.gbr = 0x8C000000u | (rnd() & 0xFF00);
	Sh4cntx.ssr = rnd();
	Sh4cntx.spc = rnd();
	Sh4cntx.sgr = rnd();
	Sh4cntx.dbr = rnd();
	Sh4cntx.vbr = rnd();
	Sh4cntx.pr = random_word();
	Sh4cntx.fpul = rnd() & 1 ? rnd() : rnd() & 0xFFFF;
	/* MD and BL set, FD clear, any T, S, Q, M and RB */
	sh4_sr_SetFull(0x500000F0u | (rnd() & 0x20000303u));
	Sh4cntx.old_sr.status = Sh4cntx.sr.status;
	/* not PR and SZ together */
	fp = rnd();
	Sh4cntx.fpscr.full = 0x00040000u | (fp & 0x00200001u) | ((fp & 2) ? 0x00080000u : (fp & 4) ? 0x00100000u : 0);
	Sh4cntx.old_fpscr = Sh4cntx.fpscr;
	UpdateFPSCR();
	next_pc = CODE_BASE;
}

/* -- Running ------------------------------------------------------------ */

/* The interpreter, from the block's start to its last instruction (of
 * those not in a delay slot) */
static void interpret_steps(u32 end, u32 last)
{
	u32 pc;
	slots_run = 0;
	do
	{
		u32 op;
		pc = next_pc;
		op = IReadMem16(pc);
		next_pc += 2;
		OpPtr[op](op);
	} while (pc != last && !raised);
	/* (a conditional branch with a delay slot, not taken, leaves the slot
	 * to be run as the next instruction) */
	if (last == end - 4 && slots_run == 0 && !raised)
	{
		const u32 op = IReadMem16(next_pc);
		next_pc += 2;
		OpPtr[op](op);
	}
}

static void interpret(const RuntimeBlockInfo *b)
{
	const u32 end = CODE_BASE + b->sh4_code_size;
	u32 last = end - 2;
	sh4_catch c;
	if (b->sh4_code_size >= 4)
	{
		const u32 op = IReadMem16(end - 4);
		if (is_branch(op) && (OpDesc[op]->type & Delayslot))
			last = end - 4;
	}
	/* (as the interpreter: an odd address jumped to raises an exception) */
	settings.dynarec.Enable = 0;
	SH4_TRY(c)
	{
		interpret_steps(end, last);
		sh4_catch_pop(&c);
	}
	else
		raised = 1;
	settings.dynarec.Enable = 1;
}

/* The recompiler's main loop, with the block for the first lookup */
static void recompiled(RuntimeBlockInfo *b, bool checks, bool optimise)
{
	cache_used = 0;
	ngen_Compile(b, checks, false, false, optimise);
	to_run = b->code;
	Sh4cntx.CpuRunning = 1;
	Sh4cntx.sh4_sched_next = 1 << 30;
	ngen_mainloop(&Sh4cntx);
}

static void setup(RuntimeBlockInfo *b)
{
	shil_opcode *keep = b->oplist;
	u32 room = b->op_room;
	memset(b, 0, sizeof(*b));
	b->oplist = keep;
	b->op_room = room;
	b->vaddr = CODE_BASE;
	b->addr = CODE_BASE & 0x1FFFFFFF;
	b->fpu_cfg = Sh4cntx.fpscr;
	b->BranchBlock = b->NextBlock = b->csc_RetCache = 0xFFFFFFFF;
}

/* What is compared: the registers, as the programs see them, and the
 * memory written */
typedef struct Seen
{
	u32 regs[(offsetof(Sh4Context, jdyn)) / 4];
	u32 t_bit, sr_status, fpscr_full;
	u8 written[0x10000];
} Seen;

static void seen(Seen *s)
{
	memcpy(s->regs, &Sh4cntx, sizeof(s->regs));
	s->t_bit = Sh4cntx.sr.T;
	s->sr_status = Sh4cntx.sr.status;
	s->fpscr_full = Sh4cntx.fpscr.full;
	memcpy(s->written, mem, sizeof(s->written));
}

static Sh4Context start;
static Seen by_interpreter, by_recompiler;
static RuntimeBlockInfo block;

static void report(int n, const Seen *a, const Seen *b)
{
	u32 i;
	char buf[256];
	printf("FAIL: block %d: the recompiler and the interpreter differ\n", n);
	for (i = 0; i < sizeof(a->regs) / 4; i++)
		if (a->regs[i] != b->regs[i])
			printf("  context word %u: %08x (interpreter), %08x (recompiler)\n", i, a->regs[i], b->regs[i]);
	if (a->t_bit != b->t_bit)
		printf("  T: %u, %u\n", a->t_bit, b->t_bit);
	if (a->sr_status != b->sr_status)
		printf("  SR: %08x, %08x\n", a->sr_status, b->sr_status);
	if (a->fpscr_full != b->fpscr_full)
		printf("  FPSCR: %08x, %08x\n", a->fpscr_full, b->fpscr_full);
	for (i = 0; i < sizeof(a->written); i++)
		if (a->written[i] != b->written[i])
		{
			printf("  memory at %05x: %02x, %02x\n", i, a->written[i], b->written[i]);
			break;
		}
	for (i = 0; i < block.sh4_code_size; i += 2)
	{
		const u32 op = IReadMem16(CODE_BASE + i);
		printf("  %08x %04x %s\n", CODE_BASE + i, op, OpDesc[op]->diss);
	}
	for (i = 0; i < block.op_count; i++)
	{
		shil_disasm(buf, sizeof(buf), &block.oplist[i]);
		printf("  shil: %s\n", buf);
	}
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 20000, t, skipped = 0, compared = 0;
	u32 ops = 0;

	p_sh4rcb = &rcb_room;
	mem_b.data = mem;
	mem_b.size = sizeof(mem);
	for (t = 0; t < 0x10000; t++)
	{
		sin_table[t].u[0] = (f32)(t - 0x8000) / 32768.0f;
		sin_table[t].u[1] = (f32)(0x8000 - t) / 65536.0f;
	}
	BuildOpcodeTables();
	for (t = 0; t < 0x10000; t++)
		if (!left_out((u32)t))
			allowed[allowed_count++] = (u16)t;
	ngen_init();

	for (t = 0; t < n; t++)
	{
		int run;
		for (run = 0; run < 4; run++)
		{
			u32 i;
			if (run == 0)
			{
				for (i = 0; i < 0x10000; i++)
					mem[i] = (u8)rnd();
				random_code();
				memcpy(mem_start, mem, sizeof(mem));
				settings.dynarec.AccurateTiming = (rnd() & 1) != 0;
				random_state();
				start = Sh4cntx;
			}

			/* decoded from the state it starts from */
			memcpy(mem, mem_start, sizeof(mem));
			Sh4cntx = start;
			UpdateFPSCR();
			raised = 0;
			setup(&block);
			if (!dec_DecodeBlock(&block, SH4_TIMESLICE / 2))
			{
				skipped++;
				break;
			}
			AnalyseBlock(&block);

			memcpy(mem, mem_start, sizeof(mem));
			Sh4cntx = start;
			UpdateFPSCR();
			interpret(&block);
			seen(&by_interpreter);
			if (raised)
			{
				skipped++;
				break;
			}

			memcpy(mem, mem_start, sizeof(mem));
			Sh4cntx = start;
			UpdateFPSCR();
			recompiled(&block, (run & 1) != 0, run < 2);
			seen(&by_recompiler);
			if (raised)
			{
				skipped++;
				break;
			}
			if (memcmp(&by_interpreter, &by_recompiler, sizeof(Seen)) != 0)
			{
				report(t, &by_interpreter, &by_recompiler);
				return 1;
			}
			compared++;
			ops += block.guest_opcodes;
		}
	}
	printf("portable: %d blocks run both ways alike (%u instructions), %d left with an exception\n",
			compared, ops, skipped);
	return 0;
}
