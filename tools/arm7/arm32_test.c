/* The sound chip's ARM7 recompiler for 32-bit ARM hosts
 * (core/hw/arm7/arm7_rec_arm32.c), on random programs: the code it makes
 * of each block, hashed, against what the C++ recompiler it replaced made
 * (tools/arm7/arm32_run.sh). Only made, not run.
 *
 * A branch out of the code is hashed as which function it goes to, by the
 * order they are first gone to, rather than as a number, so that the hash
 * is the same in every build of this. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "types.h"
#include "hw/arm7/arm7.h"
#include "hw/aica/aica_if.h"
#include "hw/mem/_vmem.h"

settings_t settings;
unsigned ARAM_SIZE = 2 * 1024 * 1024;
unsigned ARAM_MASK = 2 * 1024 * 1024 - 1;
VArray2 aica_ram;
static u8 ram[2 * 1024 * 1024];
retro_log_printf_t log_cb;
void os_DebugBreak(void) { fprintf(stderr, "verify failed\n"); abort(); }
u32 libAICA_ReadReg(u32 addr, u32 size) { (void)addr; (void)size; return 0; }
void libAICA_WriteReg(u32 addr, u32 data, u32 size) { (void)addr; (void)data; (void)size; }
void libAICA_TimeStep(void) {}
bool mem_region_set_exec(void *start, size_t len)
{
	const uintptr_t from = (uintptr_t)start & ~(uintptr_t)4095;
	return mprotect((void *)from, ((uintptr_t)start + len - from + 4095) & ~(uintptr_t)4095,
			PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

/* (the main loop's, ngen_arm.S: gone to by the code, never run here) */
void arm_dispatch(void) { abort(); }
void arm_exit(void) { abort(); }
void arm_compilecode(void) { abort(); }
void arm_mainloop(u32 cycl, void *regs, void *entrypoints) { (void)cycl; (void)regs; (void)entrypoints; abort(); }

extern void *EntryPoints[];
void CompileCode(void);

static u32 rng = 1;
static unsigned h = 2166136261u;
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

/* the functions gone to, numbered as they come */
static uintptr_t targets[64];
static u32 target_count;

static void mix_target(uintptr_t to)
{
	u32 i;
	for (i = 0; i < target_count; i++)
		if (targets[i] == to)
			break;
	if (i == target_count)
	{
		if (target_count == 64)
		{
			printf("FAIL: too many functions gone to\n");
			exit(1);
		}
		targets[target_count++] = to;
	}
	mix(0x7A000000u | i);
}

/* An instruction: one that branches (b, bl, blx) as where it goes */
static void mix_insn(const u32 *p)
{
	const u32 insn = *p;
	if ((insn & 0x0E000000u) == 0x0A000000u)
	{
		const s32 words = (s32)(insn << 8) >> 8;
		mix(insn & 0xFF000000u);
		mix_target((uintptr_t)((const u8 *)p + 8 + (intptr_t)words * 4));
	}
	else
		mix(insn);
}

/* An instruction of the kinds the recompiler has something of its own for,
 * or of any other */
static u32 random_insn(bool straight)
{
	const u32 cond = rnd() % 4 ? 0xE0000000u : (rnd() % 15) << 28;
	const u32 r = rnd();
	switch (straight ? rnd() % 4 : rnd() % 14)
	{
	case 0: case 1: case 2:	/* data processing, every form */
		return cond | (r & 0x03FFFFFFu);
	case 3:	/* data processing, register shifted by a register */
		return cond | (r & 0x01FFFF0Fu & ~0x80u) | 0x10u;
	case 4:	/* ldr/str, an immediate offset */
		return cond | 0x04000000u | (r & 0x01FFFFFFu);
	case 5:	/* ldr/str, a register shifted left */
		return cond | 0x06000000u | (r & 0x01FFFF8Fu);
	case 6:	/* ldm/stm of one register, or more */
		return cond | 0x08000000u | (r & 0x01BF0000u) | (rnd() % 2 ? 1u << (rnd() % 16) : (rnd() & 0xFFFF));
	case 7:	/* b, bl */
		return cond | 0x0A000000u | (r & 0x01FFFFFFu);
	case 8:	/* mov pc, rN */
		return cond | 0x01A0F000u | (r & 15);
	case 9:	/* mrs, msr */
		return r & 1 ? (0xE10F0000u | (r & 0x00400000u) | (rnd() % 15) << 12) : (0xE129F000u | (r & 0x00400000u) | (rnd() % 15));
	case 10: /* r15 read or written */
		return cond | (r & 0x03FFFFFFu) | 0x0000F00Fu;
	case 11: /* ldr/str with no offset, or of r15 */
		return cond | 0x04000000u | (r & 0x01FFF000u) | (rnd() % 2 ? 0x0000F000u : 0);
	case 12: /* ldr/str, a register shifted any way */
		return cond | 0x06000000u | (r & 0x01FFFFFFu);
	default:
		return r;
	}
}

static u32 *code_at(void)
{
	return (u32 *)EntryPoints[(arm_Reg[R15_ARM_NEXT].I & (ARAM_SIZE_MAX - 1)) / 4];
}

int main(int argc, char **argv)
{
	const int n = argc > 1 ? atoi(argv[1]) : 20000;
	u32 *cache = NULL;
	int t;
	u32 i;

	aica_ram.data = ram;
	aica_ram.size = sizeof(ram);
	aicaarm_init();
	aicaarm_reset();

	for (t = 0; t < n; t++)
	{
		const u32 pc = 0x1000 + (rnd() % 256) * 4;
		/* (now and then a block as long as they come: nothing that branches) */
		const bool straight = rnd() % 4 == 0;
		u32 *p;

		for (i = 0; i < 64; i++)
		{
			const u32 w = random_insn(straight);
			memcpy(&ram[pc + i * 4], &w, 4);
		}
		arm7_rec_flush();
		arm_Reg[R15_ARM_NEXT].I = pc | (rnd() & 3);
		if (cache != NULL)
			memset(cache, 0xFF, 0x10000);
		CompileCode();
		p = code_at();
		if (cache == NULL)
		{
			/* (the first time: where the code goes is known now, and is
			 * filled so that the end of the next block can be found) */
			cache = p;
			memset(cache, 0xFF, 0x10000);
			arm7_rec_flush();
			CompileCode();
		}
		if (p != cache)
		{
			printf("FAIL: a block not at the start of the code\n");
			return 1;
		}
		mix(0xB10C0000u | (u32)t);
		for (i = 0; i < 0x4000 && cache[i] != 0xFFFFFFFFu; i++)
			mix_insn(&cache[i]);
		mix(i);
	}
	printf("%d blocks, %u functions gone to, %08x\n", n, target_count, h);
	return 0;
}
