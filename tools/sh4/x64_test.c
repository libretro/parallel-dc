/* The x86-64 recompiler (core/rec-x64/rec_x64.c), or built for 32-bit x86
 * the x86 one (core/rec-x86/rec_x86.c), over the blocks the SSA optimizer
 * leaves of the decoder's blocks of random code, with the MMU on and off,
 * the guest's memory mapped every way, blocks checked and not, and each set
 * of the processor's features it looks for: the code it makes for each,
 * its link sites linked and unlinked, and every fast memory access in it
 * rewritten as a call, hashed, against what the C++ recompiler it replaced
 * made (sh tools/sh4/x64_run.sh, tools/sh4/x86_run.sh).
 *
 * Addresses outside the code cache - functions called, data reached - are
 * hashed as which thing they are, and where in it, rather than as numbers,
 * so that the hash is the same in every build of this. On 32-bit x86, where
 * any constant could be taken for one, the SH4's context is put at an
 * address of its own instead, and only what can only be an address is.
 *
 * Built for AArch64 it is the AArch64 one (core/rec-ARM64/rec_arm64.c,
 * sh tools/sh4/arm64_run.sh), which puts addresses in its code as numbers
 * that take one to four instructions: there everything it reaches but
 * code it calls - the SH4's context, its memory, the code cache and the
 * functions it only takes the address of - is at an address of its own.
 * And built for 32-bit ARM it is the 32-bit ARM one (core/rec-ARM/rec_arm.c,
 * sh tools/sh4/arm_run.sh), likewise. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "types.h"
#if HOST_CPU == CPU_X86 || HOST_CPU == CPU_ARM64 || HOST_CPU == CPU_ARM
#include <sys/mman.h>
#endif
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/dyna/decoder.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/dyna/shil.h"
#include "hw/sh4/dyna/ssa.h"
#include "hw/sh4/dyna/wait_site.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/mem/_vmem.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/sh4/sh4_interpreter.h"
#include "hw/holly/sb_mem.h"

#if HOST_CPU == CPU_ARM64 || HOST_CPU == CPU_ARM
/* (at an address of its own: see CODE_AT) */
static u16 *const code = (u16 *)(uintptr_t)0x6B100000u;
#else
static DECL_ALIGN(16) u16 code[0x10000];
#endif
static u32 code_base = 0x8C010000;
static Sh4RCB *rcb;
RegisterStruct CCN[SH4_CCN_REGS];
bool mmu_strict;
settings_t settings;
static u32 rng = 1;
static unsigned h = 2166136261u;
/* the opcodes the decoder does something of its own for */
static u16 handled[0x10000];
static u32 handled_count;

static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static u16 DYNACALL read16(u32 addr) { return code[((addr - code_base) / 2) & 0xffff]; }
ReadMem16Func IReadMem16 = read16;
const char *bm_BlockHash(RuntimeBlockInfo *block) { (void)block; return "-"; }
void os_DebugBreak(void) { fprintf(stderr, "verify failed %p\n", __builtin_return_address(0)); abort(); }

void bm_OpPush(RuntimeBlockInfo *block, const shil_opcode *op)
{
	if (block->op_count == block->op_room)
	{
		block->op_room = block->op_room ? block->op_room * 2 : 64;
		block->oplist = (shil_opcode *)realloc(block->oplist, block->op_room * sizeof(shil_opcode));
	}
	memcpy(&block->oplist[block->op_count++], op, sizeof(*op));
}



/* (each function its own, so that none is folded into another) */
static volatile u32 sink;
static u8 DYNACALL rd8(u32 a) { return (u8)(a * 2654435761u >> 7); }
static u16 DYNACALL rd16(u32 a) { return (u16)(a * 2654435761u >> 9); }
static u32 DYNACALL rd32(u32 a) { return a * 2654435761u ^ 0x5A5A1234; }
static u64 DYNACALL rd64(u32 a) { return a * 0x9E3779B97F4A7C15ull; }
static void DYNACALL wr8(u32 a, u8 d) { sink = a + d + 5; }
static void DYNACALL wr16(u32 a, u16 d) { sink = a + d + 6; }
static void DYNACALL wr32(u32 a, u32 d) { sink = a + d + 7; }
static void DYNACALL wr64(u32 a, u64 d) { sink = a + (u32)d + 8; }
ReadMem8Func ReadMem8 = rd8;
ReadMem16Func ReadMem16 = rd16;
ReadMem32Func ReadMem32 = rd32;
ReadMem64Func ReadMem64 = rd64;
WriteMem8Func WriteMem8 = wr8;
WriteMem16Func WriteMem16 = wr16;
WriteMem32Func WriteMem32 = wr32;
WriteMem64Func WriteMem64 = wr64;
#if HOST_CPU != CPU_ARM64 && HOST_CPU != CPU_ARM
f32_x2 sin_table[0x10000];
#endif
retro_log_printf_t log_cb;

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

/* ---- what the recompiler calls and reaches, as this test has it ---- */

/* the recompiler's */
#if HOST_CPU == CPU_X86
#define REC_X86 1
#define rec_cpu rec_x86_cpu
extern int rec_x86_cpu;
u32 DYNACALL rec_x86_mem_load8(u32 addr);
u32 DYNACALL rec_x86_mem_load16(u32 addr);
u32 DYNACALL rec_x86_mem_load32(u32 addr);
u64 DYNACALL rec_x86_mem_load64(u32 addr);
void DYNACALL rec_x86_mem_store8(u32 addr, u32 data);
void DYNACALL rec_x86_mem_store16(u32 addr, u32 data);
void DYNACALL rec_x86_mem_store32(u32 addr, u32 data);
void DYNACALL rec_x86_mem_store64(u32 addr, const u32 *data);
u8 DYNACALL rec_x86_read_noex8(u32 addr, u32 pc);
u16 DYNACALL rec_x86_read_noex16(u32 addr, u32 pc);
u32 DYNACALL rec_x86_read_noex32(u32 addr, u32 pc);
u64 DYNACALL rec_x86_read_noex64(u32 addr, u32 pc);
void DYNACALL rec_x86_write_noex8(u32 addr, u32 data);
void DYNACALL rec_x86_write_noex16(u32 addr, u32 data);
void DYNACALL rec_x86_write_noex32(u32 addr, u32 data);
void DYNACALL rec_x86_write_noex64(u32 addr, const u32 *data);
void DYNACALL rec_x86_interpreter_fallback(u32 op, u32 pc);
void DYNACALL rec_x86_do_sqw_mmu_no_ex(u32 addr, u32 pc);
void DYNACALL rec_x86_do_sqw_nommu_local(u32 addr, u8 *sqb);
void DYNACALL rec_x86_blockcheckfail(u32 pc);
void * DYNACALL ngen_link(u8 *after, u32 pc);
#define LINK_SITE 6
#define REC_ARM64 0
#elif HOST_CPU == CPU_ARM64
#define REC_X86 0
#define REC_ARM64 1
static int rec_cpu;
s32 rec_arm64_mem_load8(u32 addr);
s32 rec_arm64_mem_load16(u32 addr);
u32 rec_arm64_mem_load32(u32 addr);
u64 rec_arm64_mem_load64(u32 addr);
void rec_arm64_mem_store8(u32 addr, u32 data);
void rec_arm64_mem_store16(u32 addr, u32 data);
void rec_arm64_mem_store32(u32 addr, u32 data);
void rec_arm64_mem_store64(u32 addr, u64 data);
u8 rec_arm64_read_noex8(u32 addr, u32 unused, u32 pc);
u16 rec_arm64_read_noex16(u32 addr, u32 unused, u32 pc);
u32 rec_arm64_read_noex32(u32 addr, u32 unused, u32 pc);
u64 rec_arm64_read_noex64(u32 addr, u32 unused, u32 pc);
void rec_arm64_write_noex8(u32 addr, u8 data, u32 pc);
void rec_arm64_write_noex16(u32 addr, u16 data, u32 pc);
void rec_arm64_write_noex32(u32 addr, u32 data, u32 pc);
void rec_arm64_write_noex64(u32 addr, u64 data, u32 pc);
void rec_arm64_interpreter_fallback(u16 op, OpCallFP *oph, u32 pc);
void rec_arm64_do_sqw_mmu_no_ex(u32 addr, u32 pc);
void do_sqw_nommu_area_3(u32 dst, u8 *sqb);
/* (arm64_run.sh's, at addresses of their own: never run) */
void ngen_blockcheckfail(u32 pc);
void ngen_LinkBlock_Generic_stub(void);
void ngen_LinkBlock_cond_Branch_stub(void);
void ngen_LinkBlock_cond_Next_stub(void);
void ngen_FailedToFindBlock_mmu(void);
void ngen_FailedToFindBlock_nommu(void);
#define LINK_SITE 4
#elif HOST_CPU == CPU_ARM
#define REC_X86 0
#define REC_ARM64 0
#define REC_ARM 1
static int rec_cpu;
extern u32 *emit_ptr;
/* (sh4_dyna_rcb is sh4_core_regs.c's) */
/* (arm_run.sh's, at addresses of their own: never run) */
void no_update(void);
void no_update_mmu(void);
void intc_sched(void);
void intc_sched_mmu(void);
void ngen_blockcheckfail(void);
void ngen_LinkBlock_Generic_stub(void);
void ngen_LinkBlock_cond_Branch_stub(void);
void ngen_LinkBlock_cond_Next_stub(void);
void ngen_FailedToFindBlock_(void);
#define LINK_SITE 4
#else
#define REC_X86 0
#define REC_ARM64 0
#define rec_cpu rec_x64_cpu
extern int rec_x64_cpu;
s32 rec_x64_mem_load8(u32 addr);
s32 rec_x64_mem_load16(u32 addr);
u32 rec_x64_mem_load32(u32 addr);
u64 rec_x64_mem_load64(u32 addr);
void rec_x64_mem_store8(u32 addr, u32 data);
void rec_x64_mem_store16(u32 addr, u32 data);
void rec_x64_mem_store32(u32 addr, u32 data);
void rec_x64_mem_store64(u32 addr, u64 data);
u8 rec_x64_read_noex8(u32 addr, u32 pc);
u16 rec_x64_read_noex16(u32 addr, u32 pc);
u32 rec_x64_read_noex32(u32 addr, u32 pc);
u64 rec_x64_read_noex64(u32 addr, u32 pc);
void rec_x64_write_noex8(u32 addr, u8 data, u32 pc);
void rec_x64_write_noex16(u32 addr, u16 data, u32 pc);
void rec_x64_write_noex32(u32 addr, u32 data, u32 pc);
void rec_x64_write_noex64(u32 addr, u64 data, u32 pc);
void rec_x64_interpreter_fallback(u16 op, OpCallFP *oph, u32 pc);
void rec_x64_do_sqw_mmu_no_ex(u32 addr, u32 pc);
void rec_x64_do_sqw_nommu_local(u32 addr, u8 *sqb);
void ngen_block_return(void);
void ngen_compile_stub(void);
void ngen_blockcheckfail(u32 pc);
void *ngen_link(u8 *after, u32 pc);
#ifndef _WIN32
void ngen_call_keep_xmm(void);
#endif
#define LINK_SITE 7
#endif
#ifndef REC_ARM
#define REC_ARM 0
#endif
/* (both ARMs: things at addresses of their own, code of 4-byte words) */
#define REC_A (REC_ARM64 || REC_ARM)
#if !REC_A
extern int cycle_counter;
#endif
/* (sh4_core.h's: which has macros for the registers' names) */
bool UpdateSR(void);
void UpdateFPSCR(void);

/* the code cache */
#define CACHE_SIZE (4 << 20)
#if REC_A
/* (run, for the main loop: within a branch of the functions called) */
#if REC_ARM
#define CACHE_AT 0x01000000u
#else
#define CACHE_AT 0x04000000u
#endif
static u8 *cache, *cache_at, *cache_base;
#else
static DECL_ALIGN(16) u8 cache[CACHE_SIZE];
static u8 *cache_at = cache, *cache_base = cache;
#endif
#if REC_ARM
/* (the 32-bit ARM one writes where emit_ptr says, where that is set) */
u32 *emit_ptr;
void *emit_GetCCPtr(void) { return emit_ptr != NULL ? (void *)emit_ptr : (void *)cache_at; }
void emit_Skip(u32 sz)
{
	if (emit_ptr != NULL)
		emit_ptr = (u32 *)((u8 *)emit_ptr + sz);
	else
		cache_at += sz;
}
void emit_Write32(u32 data)
{
	if (emit_ptr != NULL)
		*emit_ptr++ = data;
	else
	{
		memcpy(cache_at, &data, 4);
		cache_at += 4;
	}
}
#else
void *emit_GetCCPtr(void) { return cache_at; }
void emit_Skip(u32 sz) { cache_at += sz; }
#endif
u32 emit_FreeSpace(void) { return (u32)(cache + CACHE_SIZE - cache_at); }
void emit_SetBaseAddr(void) { cache_base = cache_at; }
void (*ngen_FailedToFindBlock)(void);

/* the guest's memory, a few of its registers and the tables of pages */
#define RAM_BYTES 0x1000000
#define CODE_BYTES 0x20000
#if REC_A
/* (the SH4's context, the wait sites, its memory, the system bus's
 * registers, sin_table and sh4_write_gen, at addresses that are the same in
 * every build: arm64_run.sh puts the last two there) */
#define FIXED_AT 0x60000000u
#define FIXED_SIZE 0x10000000u
#define SITES_AT 0x69000000u
#define RAM_AT 0x6A000000u
#define AREA0_REGS ((uintptr_t)0x6B000000u)
#define CODE_AT 0x6B100000u
static u8 *const ram = (u8 *)(uintptr_t)RAM_AT;
#else
static DECL_ALIGN(16) u8 ram[RAM_BYTES];
#endif
/* where the system bus's registers are said to be */
#if REC_A
#elif REC_X86
/* (the SH4's context, at an address that is the same in every build, the
 * wait sites after it; the guest's memory as the 512 MB layout says it is
 * mapped, never read) */
#define FIXED_AT 0x60000000u
#define FIXED_SIZE (sizeof(Sh4RCB) + 0x10000)
#define AREA0_REGS ((uintptr_t)FIXED_AT + FIXED_SIZE - 0x1000)
#define FAKE_RAM_BASE ((u8 *)(uintptr_t)0x48000000u)
#else
#define AREA0_REGS ((uintptr_t)ram + ((uintptr_t)1 << 32))
#endif
#if !REC_ARM
uintptr_t mmu_read_lut[0x100000];
uintptr_t mmu_write_lut[0x100000];
#endif
u8 *virt_ram_base;
bool vmem_4gb_space;
const u32 fast_reg_lut[8] = { 0, 0, 0, 0, 1, 1, 1, 1 };
#if !REC_A
u32 sh4_write_gen;
#endif
static WaitSite *sites;
#if !REC_X86 && !REC_A
static Sh4RCB rcb_room;
static WaitSite sites_room[16];
#endif

static u32 DYNACALL hrd8(u32 a) { return a + 1; }
static u32 DYNACALL hrd16(u32 a) { return a + 2; }
static u32 DYNACALL hrd32(u32 a) { return a + 3; }
static u32 DYNACALL hrd64(u32 a) { return a + 4; }
static void DYNACALL hwr8(u32 a, u32 d) { sink = a + d + 1; }
static void DYNACALL hwr16(u32 a, u32 d) { sink = a + d + 2; }
static void DYNACALL hwr32(u32 a, u32 d) { sink = a + d + 3; }
static void DYNACALL hwr64(u32 a, u32 d) { sink = a + d + 4; }

void *_vmem_read_ptr(u32 addr, bool *isram, u32 size)
{
	if ((addr & 0x1C000000) == 0x0C000000)
	{
		*isram = true;
		return &ram[addr & 0xFFFFFF];
	}
	*isram = false;
	return size == 1 ? (void *)(uintptr_t)&hrd8 : size == 2 ? (void *)(uintptr_t)&hrd16
			: size == 4 ? (void *)(uintptr_t)&hrd32 : (void *)(uintptr_t)&hrd64;
}

void *_vmem_write_ptr(u32 addr, bool *isram, u32 size)
{
	if ((addr & 0x1C000000) == 0x0C000000)
	{
		*isram = true;
		return &ram[addr & 0xFFFFFF];
	}
	*isram = false;
	return size == 1 ? (void *)(uintptr_t)&hwr8 : size == 2 ? (void *)(uintptr_t)&hwr16
			: size == 4 ? (void *)(uintptr_t)&hwr32 : (void *)(uintptr_t)&hwr64;
}

const void *area0_read_const(void *handler, u32 addr, u32 size)
{
	(void)handler;
	/* (as far from the guest's memory as the core's own are: never read) */
	if ((addr & 0x1FFFFF00) == 0x005F6800 && (addr & 0x10) != 0)
		return (const u8 *)AREA0_REGS + 4 * (((addr >> 2) & 0x3F) + (size == 4 ? 0 : 0x40));
	return NULL;
}

u8 *GetMemPtr(u32 addr, u32 size)
{
	(void)size;
	return (u8 *)code + (addr & 0x1FFFE);
}

u8 DYNACALL _vmem_ReadMem8(u32 addr) { return (u8)addr; }
u16 DYNACALL _vmem_ReadMem16(u32 addr) { return (u16)addr; }
u32 DYNACALL _vmem_ReadMem32(u32 addr) { return addr; }
u64 DYNACALL _vmem_ReadMem64(u32 addr) { return addr; }
void DYNACALL _vmem_WriteMem8(u32 addr, u8 data) { (void)addr; (void)data; }
void DYNACALL _vmem_WriteMem16(u32 addr, u16 data) { (void)addr; (void)data; }
void DYNACALL _vmem_WriteMem32(u32 addr, u32 data) { (void)addr; (void)data; }
void DYNACALL _vmem_WriteMem64(u32 addr, u64 data) { (void)addr; (void)data; }
u32 mmu_data_translation_r8(u32 va, u32 *rv) { *rv = va; return MMU_ERROR_NONE; }
u32 mmu_data_translation_r16(u32 va, u32 *rv) { *rv = va; return MMU_ERROR_NONE; }
u32 mmu_data_translation_r32(u32 va, u32 *rv) { *rv = va; return (va >> 12) & 1 ? MMU_ERROR_NONE : 1; }
u32 mmu_data_translation_r64(u32 va, u32 *rv) { *rv = va; return MMU_ERROR_NONE; }
u32 mmu_data_translation_w8(u32 va, u32 *rv) { *rv = va; return MMU_ERROR_NONE; }
u32 mmu_data_translation_w16(u32 va, u32 *rv) { *rv = va; return (va >> 13) & 1 ? MMU_ERROR_NONE : 1; }
u32 mmu_data_translation_w32(u32 va, u32 *rv) { *rv = va ^ 0x1000; return MMU_ERROR_NONE; }
u32 mmu_data_translation_w64(u32 va, u32 *rv) { *rv = va; return MMU_ERROR_NONE; }
void mmu_lut_fill(u32 va, u32 pa, bool write) { (void)va; (void)pa; (void)write; }
void DoMMUException(u32 addr, u32 error, u32 type) { (void)addr; (void)error; (void)type; }
#if REC_ARM
bool Do_Exception(u32 epc, u32 expEvn, u32 CallVect);
u32 DYNACALL _vmem_ReadMem8SX32(u32 addr) { return (u32)(s32)(s8)addr; }
u32 DYNACALL _vmem_ReadMem16SX32(u32 addr) { return (u32)(s32)(s16)addr; }
#elif !REC_ARM64
bool Do_Exception(u32 epc, u32 expEvn, u32 CallVect) { (void)epc; (void)expEvn; (void)CallVect; return true; }
#else
bool Do_Exception(u32 epc, u32 expEvn, u32 CallVect);
unsigned RAM_SIZE = 16 * 1024 * 1024;
u64 DYNACALL mmu_ReadMemNoEx_c(u32 adr, u32 size, u32 *ex) { *ex = 0; return adr + size; }
u32 DYNACALL mmu_WriteMemNoEx_c(u32 adr, u64 data, u32 size) { sink = adr + (u32)data + size; return 0; }
void vmem_platform_flush_cache(void *icache_start, void *icache_end, void *dcache_start, void *dcache_end)
{
	(void)icache_start; (void)icache_end; (void)dcache_start; (void)dcache_end;
}
u32 DYNACALL rdv_DoInterrupts(void *block_code) { (void)block_code; return 0; }
#endif
int UpdateINTC(void) { return 5; }
bool SRdecode(void) { return false; }
int UpdateSystem_INTC(void) { return 0; }
void DYNACALL do_sqw_mmu(u32 dst) { sink = dst + 9; }
s32 sh4_wait_check(WaitSite *site, s32 cycles) { (void)site; return cycles + 10; }
WaitSite *sh4_wait_site(const RuntimeBlockInfo *block)
{
	return (block->vaddr >> 4) & 1 ? &sites[(block->vaddr >> 1) & 15] : NULL;
}
bool sh4_block_writes(const RuntimeBlockInfo *block)
{
	return (((block->vaddr >> 3) ^ block->op_count) & 1) != 0;
}

/* the block manager, for linking: what the test has set up for it */
static RuntimeBlockInfo *link_from;		/* the block a site is found in */
static RuntimeBlockInfo link_to;		/* the block to link to */
static bool link_to_known;				/* bm_GetBlock() has it */
static DynarecCodeEntryPtr link_code;	/* bm_GetCodeByVAddr()'s answer */
static DynarecCodeEntryPtr link_compiled;	/* rdv_FailedToFindBlock()'s */
static bool link_raises;				/* compiling raises an exception */
static bool same_place;					/* rdv_MmuSamePlace() for anywhere */

RuntimeBlockInfoPtr bm_GetBlock2(void *dynarec_code)
{
	const u8 *p = (const u8 *)dynarec_code;
	if (link_from != NULL && p >= (const u8 *)(uintptr_t)link_from->code
			&& p < (const u8 *)(uintptr_t)link_from->code + link_from->host_code_size)
		return link_from;
	return NULL;
}
RuntimeBlockInfoPtr DYNACALL bm_GetBlock(u32 addr) { return link_to_known && addr == link_to.addr ? &link_to : NULL; }
DynarecCodeEntryPtr DYNACALL bm_GetCodeByVAddr(u32 addr) { (void)addr; return link_code; }
DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock(u32 pc)
{
	if (link_raises)
		p_sh4rcb->cntx.pc = pc ^ 0x100;
	return link_compiled;
}
void bm_BlockAddRef(RuntimeBlockInfo *block, RuntimeBlockInfo *other)
{
	mix(0x41520000u | (block == &link_to) | (other == link_from) << 1);
}
DynarecCodeEntryPtr DYNACALL rdv_BlockCheckFail(u32 addr) { (void)addr; return NULL; }
bool rdv_MmuSamePlace(const RuntimeBlockInfo *block, u32 target, u32 *addr)
{
	if (!same_place && (target >> 12) != (block->vaddr >> 12))
		return false;
	*addr = target & 0x1FFFFFFF;
	return true;
}

/* ---- the hash, with addresses as what they are ---- */

typedef struct
{
	uintptr_t at;
	size_t size;
	u32 id;
} known;
static known knowns[0x10000];
static u32 known_count;
/* (whatever else is reached: numbered as it comes) */
static uintptr_t others[1024];
static u32 other_count;

static void know(const void *at, size_t size, u32 id)
{
	knowns[known_count].at = (uintptr_t)at;
	knowns[known_count].size = size;
	knowns[known_count].id = id;
	known_count++;
}

static int known_cmp(const void *a, const void *b)
{
	const known *x = (const known *)a, *y = (const known *)b;
	return x->at < y->at ? -1 : x->at > y->at ? 1 : 0;
}

/* an address outside the code cache, as what it is */
static void mix_addr(uintptr_t v)
{
	u32 lo = 0, hi = known_count, i;
	while (lo < hi)
	{
		const u32 mid = (lo + hi) / 2;
		if (knowns[mid].at <= v)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo > 0 && v - knowns[lo - 1].at < knowns[lo - 1].size)
	{
		mix(0xADD00000u | knowns[lo - 1].id);
		mix((u32)(v - knowns[lo - 1].at));
		return;
	}
	for (i = 0; i < other_count; i++)
		if (others[i] == v)
			break;
	if (i == other_count)
	{
		if (other_count == 1024)
		{
			printf("FAIL: too many things reached\n");
			exit(1);
		}
		others[other_count++] = v;
	}
	mix(0x07E00000u | i);
}

/* An address that is a number in the code: hashed as what it is if it is
 * one of the known things, as it is if not (a constant) */
static void mix_imm64(u64 v)
{
	u32 lo = 0, hi = known_count;
	while (lo < hi)
	{
		const u32 mid = (lo + hi) / 2;
		if (knowns[mid].at <= v)
			lo = mid + 1;
		else
			hi = mid;
	}
	if ((lo > 0 && v - knowns[lo - 1].at < knowns[lo - 1].size)
			|| (v >= (uintptr_t)cache && v < (uintptr_t)cache + CACHE_SIZE))
	{
		if (v >= (uintptr_t)cache && v < (uintptr_t)cache + CACHE_SIZE)
		{
			mix(0xCAC00000u);
			mix((u32)(v - (uintptr_t)cache));
		}
		else
			mix_addr((uintptr_t)v);
		return;
	}
	mix((u32)v);
	mix((u32)(v >> 32));
}

static void mix_target(uintptr_t v)
{
	if (v >= (uintptr_t)cache && v < (uintptr_t)cache + CACHE_SIZE)
	{
		mix(0xCAC00000u);
		mix((u32)(v - (uintptr_t)cache));
	}
	else
		mix_addr(v);
}

/* A constant that may be a function's address (32-bit x86): only exactly
 * one is taken for it */
static void mix_imm32(u32 v)
{
	u32 lo = 0, hi = known_count;
	while (lo < hi)
	{
		const u32 mid = (lo + hi) / 2;
		if (knowns[mid].at <= v)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo > 0 && knowns[lo - 1].at == v && knowns[lo - 1].size == 1)
	{
		mix(0xADD00000u | knowns[lo - 1].id);
		return;
	}
	mix(v);
}

static u32 rd32le(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

#if !REC_A
/* The instruction at @p: its length, 0 if it is none the recompiler
 * makes. Hashed, with what in it is an address as what that is. */
static u32 mix_insn(const u8 *p)
{
	const u8 *s = p;
	u32 rex = 0, op, map = 0, modrm = 0, imm = 0, rel = 0;
	bool has_modrm = false, pfx66 = false, vex = false;
	const u8 *rip_disp = NULL, *abs_disp = NULL;
	u8 copy[16];
	u32 len, i;

	while (*p == 0x66 || *p == 0xF2 || *p == 0xF3)
		pfx66 |= *p++ == 0x66;
	if (!REC_X86 && (*p & 0xF0) == 0x40)
		rex = *p++;
	op = *p++;
	if (op == 0xC4)
	{
		map = p[0] & 0x1F;
		p += 2;
		op = *p++;
		vex = true;
	}
	else if (op == 0xC5)
	{
		map = 1;
		p += 1;
		op = *p++;
		vex = true;
	}
	else if (op == 0x62)
	{
		map = p[0] & 3;
		p += 3;
		op = *p++;
		vex = true;
	}
	else if (op == 0x0F)
	{
		map = 1;
		op = *p++;
		if (op == 0x38 || op == 0x3A)
		{
			map = op == 0x38 ? 2 : 3;
			op = *p++;
		}
	}

	if (map == 0)
	{
		if (op < 0x40 && (op & 7) < 4)
			has_modrm = true;
		else if (op < 0x40 && (op & 7) == 4)
			imm = 1;
		else if (op < 0x40 && (op & 7) == 5)
			imm = pfx66 ? 2 : 4;
		else if (op >= 0x50 && op <= 0x5F)
			;
		else if (op == 0x63 || (op >= 0x84 && op <= 0x8F) || (op >= 0xD0 && op <= 0xD3) || op == 0xFE || op == 0xFF)
			has_modrm = true;
		else if (op >= 0x70 && op <= 0x7F)
			rel = 1;
		else if (op == 0x80 || op == 0x83 || op == 0xC0 || op == 0xC1 || op == 0xC6)
			has_modrm = true, imm = 1;
		else if (op == 0x81 || op == 0xC7)
			has_modrm = true, imm = pfx66 ? 2 : 4;
		else if (op == 0x90 || op == 0x9C || op == 0x9F || op == 0xC3 || op == 0xCC || op == 0xF5)
			;
		else if (REC_X86 && (op == 0x68 || op == 0x6A))
			imm = op == 0x68 ? 4 : 1;
		else if (REC_X86 && op == 0xD9)
			has_modrm = true;
		else if (REC_X86 && (op == 0xA1 || op == 0xA3))
		{
			/* mov eax, [address] / [address], eax */
			abs_disp = p;
			p += 4;
		}
		else if (op == 0xA8)
			imm = 1;
		else if (op == 0xA9)
			imm = pfx66 ? 2 : 4;
		else if (op >= 0xB8 && op <= 0xBF)
			imm = rex & 8 ? 8 : 4;
		else if (op == 0xE8 || op == 0xE9)
			rel = 4;
		else if (op == 0xEB)
			rel = 1;
		else if (op == 0xF6 || op == 0xF7)
		{
			has_modrm = true;
			if (((*p >> 3) & 7) == 0)
				imm = op == 0xF6 ? 1 : pfx66 ? 2 : 4;
		}
		else
			return 0;
	}
	else if (map == 1)
	{
		if (op >= 0x80 && op <= 0x8F && !vex)
			rel = 4;
		else if (op == 0x70 || op == 0xBA || op == 0xA4 || op == 0xAC || op == 0xC2 || op == 0xC6)
			has_modrm = true, imm = 1;
		else if ((op >= 0x10 && op <= 0x17) || op == 0x1F || (op >= 0x28 && op <= 0x2F) || (op >= 0x40 && op <= 0x6F)
				|| op == 0x7E || op == 0x7F || (op >= 0x90 && op <= 0x9F) || op == 0xA3 || op == 0xAB || op == 0xAF
				|| op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF || op >= 0xC0)
			has_modrm = true;
		else if (op == 0x77)
			;
		else
			return 0;
	}
	else if (map == 2)
		has_modrm = true;
	else
		has_modrm = true, imm = 1;

	if (has_modrm)
	{
		const u32 mod = *p >> 6, rm = *p & 7;
		modrm = *p++;
		if (mod != 3 && rm == 4)
		{
			const u32 sib = *p++;
			if (mod == 0 && (sib & 7) == 5)
			{
				/* (no base: on x86 an address, as it is) */
				if (REC_X86)
					abs_disp = p;
				p += 4;
			}
		}
		if (mod == 0 && rm == 5)
		{
			if (REC_X86)
				abs_disp = p;
			else
				rip_disp = p;
			p += 4;
		}
		else if (mod == 1)
			p += 1;
		else if (mod == 2)
			p += 4;
	}
	(void)modrm;
	p += imm + rel;
	len = (u32)(p - s);
	if (len > sizeof(copy))
		return 0;

	/* the bytes, but for those that are an address */
	memcpy(copy, s, len);
	if (rip_disp != NULL)
		memset(copy + (rip_disp - s), 0, 4);
	if (abs_disp != NULL)
		memset(copy + (abs_disp - s), 0, 4);
	if (REC_X86 && imm == 4)
		memset(copy + len - 4, 0, 4);
	if (imm == 8)
		memset(copy + len - 8, 0, 8);
	if (rel == 4)
		memset(copy + len - 4, 0, 4);
	mix(len);
	for (i = 0; i < len; i++)
		mix(copy[i]);
	if (rip_disp != NULL)
		mix_target((uintptr_t)(p + (s32)rd32le(rip_disp)));
	if (abs_disp != NULL)
		mix_target((uintptr_t)rd32le(abs_disp));
	if (REC_X86 && imm == 4)
		mix_imm32(rd32le(p - 4));
	if (imm == 8)
		mix_imm64((u64)rd32le(p - 8) | (u64)rd32le(p - 4) << 32);
	if (rel == 4)
		mix_target((uintptr_t)(p + (s32)rd32le(p - 4)));
	return len;
}

#endif

#if REC_A
/* The literals the code before has loads of, by where they are (as words
 * from the start of what is hashed): 4 or 8 bytes, or an address */
#define POOL_MAX 0x4000
static u8 literal_kind[POOL_MAX];
static intptr_t literal_words;

/* The instruction at @p: 4 bytes, with a branch out of the code cache
 * hashed as where it goes. A literal load notes what it loads. */
static u32 mix_insn_at(const u8 *p, const u8 *start)
{
	const u32 insn = rd32le(p);
#if REC_ARM
	/* b, bl (with a condition), blx */
	if ((insn & 0x0E000000u) == 0x0A000000u)
	{
		const s32 words = (s32)(insn << 8) >> 8;
		const uintptr_t to = (uintptr_t)(p + 8 + (intptr_t)words * 4);
		if (to < (uintptr_t)cache || to >= (uintptr_t)cache + CACHE_SIZE)
		{
			mix(insn & 0xFF000000u);
			mix_target(to);
			return 4;
		}
	}
	(void)start;
	mix(insn);
	return 4;
#else
	if ((insn & 0x7C000000u) == 0x14000000u)
	{
		const s32 words = (s32)(insn << 6) >> 6;
		const uintptr_t to = (uintptr_t)(p + (intptr_t)words * 4);
		if (to < (uintptr_t)cache || to >= (uintptr_t)cache + CACHE_SIZE)
		{
			mix(insn & 0xFC000000u);
			mix_target(to);
			return 4;
		}
	}
	if (start != NULL && ((insn & 0xBF000000u) == 0x18000000u || (insn & 0xFF000000u) == 0x1C000000u))
	{
		/* ldr wt / xt / st, literal */
		const s32 words = (s32)(insn << 8) >> 13;
		const intptr_t at = (p - start) / 4 + words;
		if ((insn & 31) != 31 && at >= 0 && at < literal_words)
			literal_kind[at] = (insn & 0x40000000u) ? 8 : 4;
	}
	mix(insn);
	return 4;
#endif
}

static u32 mix_insn(const u8 *p)
{
	return mix_insn_at(p, NULL);
}

/* (SH4_TEST_TRACE set: what is hashed, as code, on stderr) */
static bool trace;

static void mix_code(const u8 *code_at, u32 size)
{
	u32 at;
	if ((size & 3) != 0 || size / 4 > POOL_MAX)
	{
		printf("FAIL: %u bytes of code\n", size);
		exit(1);
	}
	memset(literal_kind, 0, size / 4);
	literal_words = size / 4;
	for (at = 0; at < size;)
	{
		const u32 kind = literal_kind[at / 4];
		if (kind == 8 && at + 8 <= size)
		{
			/* a 64-bit literal: maybe an address */
			mix(0x11780000u);
			mix_imm64((u64)rd32le(code_at + at) | (u64)rd32le(code_at + at + 4) << 32);
			at += 8;
		}
		else if (kind == 4)
		{
			mix(0x11740000u);
			mix(rd32le(code_at + at));
			at += 4;
		}
		else
			at += mix_insn_at(code_at + at, code_at);
		if (trace)
			fprintf(stderr, "%08x %08x %08x\n", at, rd32le(code_at + at - 4), h);
	}
	if (trace)
		fprintf(stderr, "--\n");
}
#else
static void mix_code(const u8 *code_at, u32 size)
{
	u32 at = 0;
	while (at < size)
	{
		u32 n;
		/* (what is between the stubs ngen_init() makes: no instruction made starts with it) */
		if (code_at[at] == 0)
		{
			mix(0xF111u);
			at++;
			continue;
		}
		n = mix_insn(code_at + at);
		if (n == 0)
		{
			u32 k;
			printf("FAIL: no instruction made by the recompiler at +%u:", at);
			for (k = 0; k < 8 && at + k < size; k++)
				printf(" %02x", code_at[at + k]);
			printf("\n");
			exit(1);
		}
		at += n;
	}
	if (at != size)
	{
		printf("FAIL: an instruction runs past the end\n");
		exit(1);
	}
}
#endif

static void know_all(void)
{
	static const void *const functions[] = {
#if REC_X86
		(const void *)(uintptr_t)&rec_x86_mem_load8, (const void *)(uintptr_t)&rec_x86_mem_load16,
		(const void *)(uintptr_t)&rec_x86_mem_load32, (const void *)(uintptr_t)&rec_x86_mem_load64,
		(const void *)(uintptr_t)&rec_x86_mem_store8, (const void *)(uintptr_t)&rec_x86_mem_store16,
		(const void *)(uintptr_t)&rec_x86_mem_store32, (const void *)(uintptr_t)&rec_x86_mem_store64,
		(const void *)(uintptr_t)&rec_x86_read_noex8, (const void *)(uintptr_t)&rec_x86_read_noex16,
		(const void *)(uintptr_t)&rec_x86_read_noex32, (const void *)(uintptr_t)&rec_x86_read_noex64,
		(const void *)(uintptr_t)&rec_x86_write_noex8, (const void *)(uintptr_t)&rec_x86_write_noex16,
		(const void *)(uintptr_t)&rec_x86_write_noex32, (const void *)(uintptr_t)&rec_x86_write_noex64,
		(const void *)(uintptr_t)&rec_x86_interpreter_fallback, (const void *)(uintptr_t)&rec_x86_do_sqw_mmu_no_ex,
		(const void *)(uintptr_t)&rec_x86_do_sqw_nommu_local, (const void *)(uintptr_t)&rec_x86_blockcheckfail,
		(const void *)(uintptr_t)&ngen_link, (const void *)(uintptr_t)&bm_GetCodeByVAddr,
		(const void *)(uintptr_t)&rdv_FailedToFindBlock, (const void *)(uintptr_t)&UpdateSystem_INTC,
#elif REC_ARM64
		(const void *)(uintptr_t)&rec_arm64_mem_load8, (const void *)(uintptr_t)&rec_arm64_mem_load16,
		(const void *)(uintptr_t)&rec_arm64_mem_load32, (const void *)(uintptr_t)&rec_arm64_mem_load64,
		(const void *)(uintptr_t)&rec_arm64_mem_store8, (const void *)(uintptr_t)&rec_arm64_mem_store16,
		(const void *)(uintptr_t)&rec_arm64_mem_store32, (const void *)(uintptr_t)&rec_arm64_mem_store64,
		(const void *)(uintptr_t)&rec_arm64_read_noex8, (const void *)(uintptr_t)&rec_arm64_read_noex16,
		(const void *)(uintptr_t)&rec_arm64_read_noex32, (const void *)(uintptr_t)&rec_arm64_read_noex64,
		(const void *)(uintptr_t)&rec_arm64_write_noex8, (const void *)(uintptr_t)&rec_arm64_write_noex16,
		(const void *)(uintptr_t)&rec_arm64_write_noex32, (const void *)(uintptr_t)&rec_arm64_write_noex64,
		(const void *)(uintptr_t)&rec_arm64_interpreter_fallback, (const void *)(uintptr_t)&rec_arm64_do_sqw_mmu_no_ex,
		(const void *)(uintptr_t)&do_sqw_nommu_area_3, (const void *)(uintptr_t)&ngen_blockcheckfail,
		(const void *)(uintptr_t)&ngen_LinkBlock_Generic_stub, (const void *)(uintptr_t)&ngen_LinkBlock_cond_Branch_stub,
		(const void *)(uintptr_t)&ngen_LinkBlock_cond_Next_stub, (const void *)(uintptr_t)&ngen_FailedToFindBlock_mmu,
		(const void *)(uintptr_t)&ngen_FailedToFindBlock_nommu, (const void *)(uintptr_t)&UpdateSystem,
		(const void *)(uintptr_t)&bm_GetCodeByVAddr, (const void *)(uintptr_t)&rdv_DoInterrupts,
#elif REC_ARM
		(const void *)(uintptr_t)&no_update, (const void *)(uintptr_t)&no_update_mmu,
		(const void *)(uintptr_t)&intc_sched, (const void *)(uintptr_t)&intc_sched_mmu,
		(const void *)(uintptr_t)&ngen_blockcheckfail, (const void *)(uintptr_t)&ngen_LinkBlock_Generic_stub,
		(const void *)(uintptr_t)&ngen_LinkBlock_cond_Branch_stub, (const void *)(uintptr_t)&ngen_LinkBlock_cond_Next_stub,
		(const void *)(uintptr_t)&ngen_FailedToFindBlock_, (const void *)(uintptr_t)&_vmem_ReadMem8SX32,
		(const void *)(uintptr_t)&_vmem_ReadMem16SX32, (const void *)(uintptr_t)&_vmem_ReadMem32,
		(const void *)(uintptr_t)&_vmem_ReadMem64, (const void *)(uintptr_t)&_vmem_WriteMem8,
		(const void *)(uintptr_t)&_vmem_WriteMem16, (const void *)(uintptr_t)&_vmem_WriteMem32,
		(const void *)(uintptr_t)&_vmem_WriteMem64,
#else
		(const void *)(uintptr_t)&rec_x64_mem_load8, (const void *)(uintptr_t)&rec_x64_mem_load16,
		(const void *)(uintptr_t)&rec_x64_mem_load32, (const void *)(uintptr_t)&rec_x64_mem_load64,
		(const void *)(uintptr_t)&rec_x64_mem_store8, (const void *)(uintptr_t)&rec_x64_mem_store16,
		(const void *)(uintptr_t)&rec_x64_mem_store32, (const void *)(uintptr_t)&rec_x64_mem_store64,
		(const void *)(uintptr_t)&rec_x64_read_noex8, (const void *)(uintptr_t)&rec_x64_read_noex16,
		(const void *)(uintptr_t)&rec_x64_read_noex32, (const void *)(uintptr_t)&rec_x64_read_noex64,
		(const void *)(uintptr_t)&rec_x64_write_noex8, (const void *)(uintptr_t)&rec_x64_write_noex16,
		(const void *)(uintptr_t)&rec_x64_write_noex32, (const void *)(uintptr_t)&rec_x64_write_noex64,
		(const void *)(uintptr_t)&rec_x64_interpreter_fallback, (const void *)(uintptr_t)&rec_x64_do_sqw_mmu_no_ex,
		(const void *)(uintptr_t)&rec_x64_do_sqw_nommu_local, (const void *)(uintptr_t)&ngen_block_return,
		(const void *)(uintptr_t)&ngen_compile_stub, (const void *)(uintptr_t)&ngen_blockcheckfail,
		(const void *)(uintptr_t)&ngen_link,
#ifndef _WIN32
		(const void *)(uintptr_t)&ngen_call_keep_xmm,
#else
		NULL,
#endif
#endif
		(const void *)(uintptr_t)&rd8, (const void *)(uintptr_t)&rd16, (const void *)(uintptr_t)&rd32,
		(const void *)(uintptr_t)&rd64, (const void *)(uintptr_t)&wr8, (const void *)(uintptr_t)&wr16,
		(const void *)(uintptr_t)&wr32, (const void *)(uintptr_t)&wr64,
		(const void *)(uintptr_t)&hrd8, (const void *)(uintptr_t)&hrd16, (const void *)(uintptr_t)&hrd32,
		(const void *)(uintptr_t)&hrd64, (const void *)(uintptr_t)&hwr8, (const void *)(uintptr_t)&hwr16,
		(const void *)(uintptr_t)&hwr32, (const void *)(uintptr_t)&hwr64,
		(const void *)(uintptr_t)&UpdateSR, (const void *)(uintptr_t)&UpdateFPSCR,
		(const void *)(uintptr_t)&UpdateINTC, (const void *)(uintptr_t)&Do_Exception,
		(const void *)(uintptr_t)&do_sqw_mmu, (const void *)(uintptr_t)&sh4_wait_check,
	};
	u32 i, j;
	for (i = 0; i < sizeof(functions) / sizeof(functions[0]); i++)
		if (functions[i] != NULL)
			know(functions[i], 1, 1 + i);
	know(rcb, sizeof(*rcb), 100);
	know(code, CODE_BYTES, 101);
	know(ram, RAM_BYTES, 102);
	know((const void *)AREA0_REGS, 0x400, 103);
	know(mmu_read_lut, sizeof(mmu_read_lut), 104);
	know(mmu_write_lut, sizeof(mmu_write_lut), 105);
	know(sites, 16 * sizeof(WaitSite), 106);
	know(sin_table, sizeof(sin_table), 107);
	know(&sh4_write_gen, sizeof(sh4_write_gen), 108);
#if !REC_A
	know(&cycle_counter, sizeof(cycle_counter), 109);
#endif
	/* the interpreter's handlers, by the first opcode that has each */
	for (i = 0; i < 0x10000; i++)
	{
		const uintptr_t f = (uintptr_t)OpDesc[i]->oph;
		for (j = 0; j < known_count; j++)
			if (knowns[j].at == f)
				break;
		if (j == known_count)
			know((const void *)f, 1, 0x10000 + i);
	}
	qsort(knowns, known_count, sizeof(knowns[0]), known_cmp);
}

/* ---- the tests ---- */

/* Instructions of every kind the recompiler has code of its own for, each
 * as likely as the next: random opcodes have the ones with registers to
 * name many times as often as those with none (an opcode and the bits of
 * it that name registers) */
static const u16 kinds[][2] = {
	{ 0xFBFD, 0 }, { 0xF3FD, 0 }, { 0xF0FD, 0x0E00 }, { 0xF1FD, 0x0C00 }, { 0xF0ED, 0x0F00 },
	{ 0xF00E, 0x0FF0 }, { 0xF07D, 0x0F00 }, { 0xF06D, 0x0F00 }, { 0xF000, 0x0FF0 }, { 0xF001, 0x0FF0 },
	{ 0xF002, 0x0FF0 }, { 0xF003, 0x0FF0 }, { 0xF004, 0x0FF0 }, { 0xF005, 0x0FF0 }, { 0xF03D, 0x0F00 },
	{ 0xF02D, 0x0F00 }, { 0xF04D, 0x0F00 }, { 0xF05D, 0x0F00 }, { 0xF008, 0x0FF0 }, { 0xF00A, 0x0FF0 },
	{ 0xF00C, 0x0FF0 }, { 0xF006, 0x0FF0 }, { 0xF007, 0x0FF0 }, { 0xF009, 0x0FF0 }, { 0xF00B, 0x0FF0 },
	{ 0xF01D, 0x0F00 }, { 0xF00D, 0x0F00 }, { 0x6002, 0x0FF0 }, { 0x2002, 0x0FF0 }, { 0x6000, 0x0FF0 },
	{ 0x2000, 0x0FF0 }, { 0x6001, 0x0FF0 }, { 0x2001, 0x0FF0 }, { 0x5000, 0x0FFF }, { 0x1000, 0x0FFF },
	{ 0x000C, 0x0FF0 }, { 0x000D, 0x0FF0 }, { 0x000E, 0x0FF0 }, { 0x0004, 0x0FF0 }, { 0x0005, 0x0FF0 },
	{ 0x0006, 0x0FF0 }, { 0xC600, 0x00FF }, { 0xC200, 0x00FF }, { 0x0083, 0x0F00 }, { 0x400C, 0x0FF0 },
	{ 0x400D, 0x0FF0 }, { 0x300E, 0x0FF0 }, { 0x600A, 0x0FF0 }, { 0x300A, 0x0FF0 }, { 0x4024, 0x0F00 },
	{ 0x4025, 0x0F00 }, { 0x200D, 0x0FF0 }, { 0x6008, 0x0FF0 }, { 0x6009, 0x0FF0 }, { 0x3005, 0x0FF0 },
	{ 0x300D, 0x0FF0 }, { 0x200E, 0x0FF0 }, { 0x200F, 0x0FF0 }, { 0x0007, 0x0FF0 }, { 0x600E, 0x0FF0 },
	{ 0x600F, 0x0FF0 }, { 0x200C, 0x0FF0 }, { 0x2008, 0x0FF0 }, { 0xC800, 0x00FF }, { 0x3000, 0x0FF0 },
	{ 0x3002, 0x0FF0 }, { 0x3003, 0x0FF0 }, { 0x3006, 0x0FF0 }, { 0x3007, 0x0FF0 }, { 0x4011, 0x0F00 },
	{ 0x4015, 0x0F00 }, { 0x4000, 0x0F00 }, { 0x4001, 0x0F00 }, { 0x4021, 0x0F00 }, { 0x4004, 0x0F00 },
	{ 0x4005, 0x0F00 }, { 0x4008, 0x0F00 }, { 0x4009, 0x0F00 }, { 0x4018, 0x0F00 }, { 0x4019, 0x0F00 },
	{ 0x4028, 0x0F00 }, { 0x4029, 0x0F00 }, { 0x6003, 0x0FF0 }, { 0x6007, 0x0FF0 }, { 0x600B, 0x0FF0 },
	{ 0x600C, 0x0FF0 }, { 0x600D, 0x0FF0 }, { 0x7000, 0x0FFF }, { 0x2009, 0x0FF0 }, { 0x200A, 0x0FF0 },
	{ 0x200B, 0x0FF0 }, { 0x400E, 0x0F00 }, { 0x4007, 0x0F00 }, { 0x406A, 0x0F00 }, { 0x4066, 0x0F00 },
	{ 0x0002, 0x0F00 }, { 0x006A, 0x0F00 }, { 0x005A, 0x0F00 }, { 0x0029, 0x0F00 }, { 0x0019, 0 },
	{ 0x2007, 0x0FF0 }, { 0x3004, 0x0FF0 }, { 0x0008, 0 }, { 0x0018, 0 }, { 0x0009, 0 }
};

/* Runs of instructions that make constants for others to use: addresses
 * in memory, in the system bus's registers and in the processor's own
 * area, a store queue's, an angle and FR0 */
static u32 random_shape_at(u32 i)
{
	const u32 m = rnd() & 0xF, d = rnd() & 0xF;
	u32 n = rnd() & 0xF;
	const u32 hi = n << 8;
	switch (rnd() % 7)
	{
	case 0:	/* 0x8C0000xx, and a load or store of 1 to 8 bytes there */
		code[i++] = (u16)(0xE08C | hi);
		code[i++] = (u16)(0x4028 | hi);
		code[i++] = (u16)(0x4018 | hi);
		code[i++] = (u16)(0x7000 | hi | (rnd() & 0x7C));
		break;
	case 1:	/* 0xE0000000 and up: a store queue's */
		code[i++] = (u16)(0xE0E0 | hi);
		code[i++] = (u16)(0x4028 | hi);
		code[i++] = (u16)(0x4018 | hi);
		code[i++] = (u16)(0x0083 | hi);
		return i;
	case 2:	/* 0xFFFF00xx: the processor's own */
		code[i++] = (u16)(0xE0FF | hi);
		code[i++] = (u16)(0x4028 | hi);
		code[i++] = (u16)(0x7000 | hi | (rnd() & 0x1C));
		break;
	case 3:	/* 0x005F68xx: the system bus's */
		code[i++] = 0xE05F;
		code[i++] = 0x4018;
		code[i++] = 0xCB68;
		code[i++] = 0x4018;
		code[i++] = (u16)(0xCB00 | (rnd() & 0x1C));
		n = 0;
		break;
	case 4:	/* an angle */
		code[i++] = (u16)(0xE000 | hi | (rnd() & 0xFF));
		code[i++] = (u16)(0x405A | hi);
		code[i++] = (u16)(0xF0FD | (d & 7) << 9);
		return i;
	case 5:	/* FR0 a constant, for FMAC */
		code[i++] = rnd() & 1 ? 0xF09D : 0xF08D;
		code[i++] = (u16)(0xF00E | hi | m << 4);
		return i;
	default:	/* back from an exception */
		code[i++] = 0x002B;
		code[i++] = 0x0009;
		return i;
	}
	/* the access, through Rn (R0 for the bus's) */
	switch (rnd() % 8)
	{
	case 0: code[i++] = (u16)(0x6002 | m << 8 | n << 4); break;
	case 1: code[i++] = (u16)(0x2002 | n << 8 | m << 4); break;
	case 2: code[i++] = (u16)(0x6000 | m << 8 | n << 4); break;
	case 3: code[i++] = (u16)(0x2001 | n << 8 | m << 4); break;
	case 4: code[i++] = (u16)(0xF008 | m << 8 | n << 4); break;	/* fmov @Rn,FRm (DRm with SZ) */
	case 5: code[i++] = (u16)(0xF00A | n << 8 | m << 4); break;	/* fmov FRm,@Rn */
	case 6: code[i++] = (u16)(0x6001 | m << 8 | n << 4); break;
	default: code[i++] = (u16)(0x2000 | n << 8 | m << 4); break;
	}
	return i;
}

/* random code, and then instructions of each kind put in */
static void random_kinds(void)
{
	u32 i;
	random_code();
	for (i = 0; i < 0x200; i++)
	{
		const u32 r = rnd();
		if (r % 3 == 0)
		{
			const u32 k = (r >> 8) % (sizeof(kinds) / sizeof(kinds[0]));
			code[i] = (u16)(kinds[k][0] | (rnd() & kinds[k][1]));
		}
	}
	for (i = 0; i < 0x1F0; i++)
		if (rnd() % 8 == 0)
			i = random_shape_at(i);
}

static RuntimeBlockInfo compiled;

#if REC_A
/* The block's end written again, the way RuntimeBlockInfo::Relink() has
 * it: to the blocks it goes to, as known, and to none */
static void links(RuntimeBlockInfo *b)
{
	u8 *const code_at = (u8 *)(uintptr_t)b->code;
	const u32 size = b->host_code_size;
	static u8 keep[0x10000];
	int which;
	if (size > sizeof(keep))
		return;
	memcpy(keep, code_at, size);
	for (which = 0; which < 3; which++)
	{
		u32 n;
		link_to.code = (DynarecCodeEntryPtr)(uintptr_t)(cache_at + 64 * (rnd() & 7));
		b->pBranchBlock = which != 1 ? &link_to : NULL;
		b->pNextBlock = which != 0 ? &link_to : NULL;
		same_place = (rnd() & 1) != 0;
		n = ngen_Relink(b);
		mix(0x4C490000u | (u32)which);
		mix(n);
		/* (nothing it writes has a literal) */
		mix_code(code_at + b->relink_offset, n);
	}
	b->pBranchBlock = b->pNextBlock = NULL;
	memcpy(code_at, keep, size);
}
#else
/* Every way out of the block linked and unlinked, the way ngen_link() and
 * ngen_Relink() do it */
static void links(RuntimeBlockInfo *b)
{
	int which;
	u8 *const code_at = (u8 *)(uintptr_t)b->code;
	for (which = 0; which < 2; which++)
	{
		const u32 site = b->link_at[which];
		u8 keep[LINK_SITE];
		void *to, **place;
		u32 pc;
		if (site == 0)
			continue;
		mix(0x4C490000u | (u32)which);
		memcpy(keep, code_at + site, LINK_SITE);

		/* what the block manager has */
		link_from = (rnd() & 7) ? b : NULL;
		link_to.addr = rnd() & 3 ? (which == 0 ? b->BranchBlock : b->NextBlock) : rnd();
		link_to.vaddr = rnd() & 3 ? link_to.addr : rnd();
		if (mmu_enabled())
			link_to.addr &= 0x1FFFFFFF;
		link_to.temp_block = (rnd() & 3) == 0;
		link_to.code = (DynarecCodeEntryPtr)(uintptr_t)(cache_at + 64 * (rnd() & 7));
		link_to.linked_entry = rnd() & 1 ? 0 : 12;
		link_to_known = (rnd() & 7) != 0;
		link_code = rnd() & 3 ? link_to.code : ngen_FailedToFindBlock;
		link_compiled = rnd() & 1 ? link_to.code : ngen_FailedToFindBlock;
		link_raises = (rnd() & 3) == 0;
		same_place = (rnd() & 1) != 0;
		b->pBranchBlock = b->pNextBlock = NULL;
		p_sh4rcb->cntx.pc = 0x1234;

		pc = rnd() & 7 ? (which == 0 ? b->BranchBlock : b->NextBlock) : rnd();
		/* (with the MMU on the table is looked in itself, where the place is) */
		place = &rcb->fpcb[((pc & 0x1FFFFFFF) >> 1) & FPCB_MASK];
		*place = (void *)(uintptr_t)link_code;
		to = ngen_link(code_at + site + 5, pc);
		*place = NULL;
		mix_target((uintptr_t)to);
		mix(p_sh4rcb->cntx.pc);
		mix(b->link_at[0]);
		mix(b->link_at[1]);
		mix(b->pBranchBlock == &link_to);
		mix(b->pNextBlock == &link_to);
		mix_code(code_at + site, LINK_SITE);

		/* and unlinked again */
		if (b->link_at[which] != 0)
		{
			b->pBranchBlock = b->pNextBlock = NULL;
			ngen_Relink(b);
			mix_code(code_at + site, LINK_SITE);
		}
		memcpy(code_at + site, keep, LINK_SITE);
		b->link_at[which] = site;
	}
	link_from = NULL;
}
#endif

#if REC_ARM
/* Each fast memory access in the block rewritten as a call, as the
 * handler of the fault it raises has it (ngen_readm_fail_v2()): the access
 * is the instruction after the one that cuts the address to 29 bits (bic
 * r1, rN, #0xE0000000), or after that and an add of the base */
static void rewrites(RuntimeBlockInfo *b)
{
	u32 *const words = (u32 *)(uintptr_t)b->code;
	u32 at;
	for (at = 2; at < b->host_opcodes; at++)
	{
		u32 keep[8], regs[16];
		const u32 *to;
		u32 k;
		const bool after_bic = (words[at - 1] & 0xFFF0FFFFu) == 0xE3C0120Eu && words[at] != 0xE0811008u;
		const bool after_add = (words[at - 2] & 0xFFF0FFFFu) == 0xE3C0120Eu && words[at - 1] == 0xE0811008u;
		if (!after_bic && !after_add)
			continue;
		memcpy(keep, &words[at - 2], sizeof(keep));
		for (k = 0; k < 16; k++)
			regs[k] = 0x1000 * k;
		to = ngen_readm_fail_v2(&words[at], regs, regs[8]);
		mix(0x52570000u | at);
		mix((u32)(&words[at] - to));
		mix_code((const u8 *)to, 6 * 4);
		memcpy(&words[at - 2], keep, sizeof(keep));
	}
}
#else
/* Each fast memory access in the block rewritten as a call */
static void rewrites(RuntimeBlockInfo *b)
{
	u8 *const code_at = (u8 *)(uintptr_t)b->code;
	u32 at = 0;
#if REC_ARM64
	/* (the code, not the literals after it) */
	const u32 end = b->host_opcodes * 4;
#else
	const u32 end = b->host_code_size;
#endif
	link_from = b;
	while (at < end)
	{
		u8 keep[16];
		unat pc = (unat)(code_at + at);
		const u32 n = mix_insn(code_at + at);
		memcpy(keep, code_at + at, n);
		if (ngen_Rewrite(&pc))
		{
			mix(0x52570000u | at);
			mix_code(code_at + at, n);
			memcpy(code_at + at, keep, n);
		}
		at += n;
	}
	link_from = NULL;
}
#endif

static void compile(RuntimeBlockInfo *b)
{
	const u32 r = rnd();
	const bool force_checks = (r & 7) == 0;
	const bool optimise = (r & 0x18) != 0;
#if REC_X86
	static const int cpus[4] = { 0, 2, 0, 2 };
#elif REC_A
	static const int cpus[4] = { 0, 0, 0, 0 };
#else
	static const int cpus[4] = { 0, 2, 3, 7 };
#endif

	rec_cpu = cpus[(r >> 5) & 3];
	b->temp_block = ((r >> 7) & 7) == 0;
	b->mmu_go_on = ((r >> 10) & 1) != 0;
	b->has_fpu_op = ((r >> 11) & 3) != 0;
	b->read_only = ((r >> 13) & 1) != 0;
	b->link_at[0] = b->link_at[1] = 0;
	b->linked_entry = 0;
#if REC_A
	/* (where the block is, as against the address it runs at) */
	b->addr = b->vaddr & 0x1FFFFFFF;
#endif
	b->pBranchBlock = b->pNextBlock = NULL;
	cache_at = cache_base;
	ngen_Compile(b, force_checks, false, false, optimise);
	mix(0x424C4B00u | force_checks | optimise << 1 | (u32)rec_cpu << 2);
	mix((u32)((u8 *)(uintptr_t)b->code - cache_base));
	mix(b->host_code_size);
	mix(b->link_at[0]);
	mix(b->link_at[1]);
	mix(b->linked_entry);
	mix_code((const u8 *)(uintptr_t)b->code, b->host_code_size);
	if ((r >> 14) & 1)
		links(b);
	if (optimise && ((r >> 15) & 1))
		rewrites(b);
}

/* the guest's memory mapped, or not; with the MMU on, or not */
static void environment(void)
{
	const u32 r = rnd();
	settings.dreamcast.FullMMU = (r & 3) == 0;
	CCN_MMUCR.AT = (r & 4) != 0;
	mmu_strict = (r & 0x38) == 0;
#if REC_X86
	virt_ram_base = (r & 0x40) ? FAKE_RAM_BASE : NULL;
#else
	virt_ram_base = (r & 0x40) ? ram - 0x0C000000 : NULL;
#endif
	vmem_4gb_space = virt_ram_base != NULL && (r & 0x80) != 0;
	settings.dynarec.AccurateTiming = (r & 0x100) != 0;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 20000, t;
	static RuntimeBlockInfo a;
#if REC_X86
	{
		u8 *fixed = (u8 *)mmap((void *)(uintptr_t)FIXED_AT, FIXED_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (fixed != (u8 *)(uintptr_t)FIXED_AT)
		{
			printf("FAIL: no memory at %08x for the SH4's context\n", FIXED_AT);
			return 1;
		}
		rcb = (Sh4RCB *)fixed;
		sites = (WaitSite *)(fixed + sizeof(Sh4RCB));
	}
#elif REC_A
	{
		u8 *fixed = (u8 *)mmap((void *)(uintptr_t)FIXED_AT, FIXED_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		cache = (u8 *)mmap((void *)(uintptr_t)CACHE_AT, CACHE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (fixed != (u8 *)(uintptr_t)FIXED_AT || cache != (u8 *)(uintptr_t)CACHE_AT
				|| sizeof(Sh4RCB) > SITES_AT - FIXED_AT)
		{
			printf("FAIL: no memory at %08x and %08x\n", FIXED_AT, CACHE_AT);
			return 1;
		}
		cache_at = cache_base = cache;
		rcb = (Sh4RCB *)fixed;
		sites = (WaitSite *)(uintptr_t)SITES_AT;
#if REC_ARM
		sh4_dyna_rcb = (u8 *)&rcb->cntx + sizeof(Sh4Context);
#endif
	}
#else
	rcb = &rcb_room;
	sites = sites_room;
#endif
	p_sh4rcb = rcb;
	for (t = 0; t < 0x10000; t++)
	{
		sin_table[t].u[0] = rnd();
		sin_table[t].u[1] = rnd();
	}
	BuildOpcodeTables();
	for (t = 0; t < 0x10000; t++)
		if (OpDesc[t]->rec_oph != NULL || OpDesc[t]->decode != 0)
			handled[handled_count++] = (u16)t;
	know_all();

	ngen_init();
#if REC_A
	trace = getenv("SH4_TEST_TRACE") != NULL;
#endif
#if REC_ARM64
	/* the main loop, made as it is run, with the MMU off and on: it comes
	 * back at once */
	settings.dreamcast.FullMMU = false;
	ngen_ResetBlocks();
	ngen_mainloop(&p_sh4rcb->cntx);
	settings.dreamcast.FullMMU = true;
	ngen_ResetBlocks();
	ngen_mainloop(&p_sh4rcb->cntx);
	settings.dreamcast.FullMMU = false;
	emit_SetBaseAddr();
#endif
	mix((u32)(cache_base - cache));
	mix_code(cache, (u32)(cache_base - cache));

	for (t = 0; t < n; t++)
	{
		u32 pc;
		fpscr_t cfg;
		environment();
		/* (in P1, or in P0 where the MMU translates it) */
		code_base = rnd() & 3 ? 0x8C010000 : 0x0C010000;
		if (t & 1)
			random_kinds();
		else
			random_code();
		pc = code_base + (rnd() % 64) * 2;
		cfg.full = 0;
		cfg.PR = rnd() & 1;
		cfg.SZ = rnd() & 1;
		cfg.RM = rnd() & 3;
		settings.dynarec.DisableDivMatching = (rnd() & 7) == 0;
		p_sh4rcb->cntx.sr.FD = 0;
		setup(&a, pc, cfg);
		if (!dec_DecodeBlock(&a, SH4_TIMESLICE / 2))
			continue;
		copy_block(&compiled, &a);
		AnalyseBlock(&compiled);
		compile(&compiled);
	}
	free(a.oplist);
	free(compiled.oplist);
	printf("%d blocks, %u things reached, %08x\n", n, other_count, h);
	return 0;
}
