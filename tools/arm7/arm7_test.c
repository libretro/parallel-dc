/* Runs the sound chip's ARM7 on made-up programs and prints what each
 * leaves behind. tools/arm7/run.sh builds this twice, once with the
 * recompiler (core/hw/arm7/arm7_rec.c and a backend) and once with
 * the interpreter (arm7.c, arm-new.h), runs both and compares what they
 * print: registers, flags and memory have to be the same from both after
 * every program.
 *
 * A program is 8 to 40 random instructions and then a branch to itself:
 * arithmetic and logic with every kind of second operand, shift and
 * condition, with and without setting flags, often in rows of the same
 * condition; multiplies; loads and stores of words and bytes with every
 * addressing mode; block loads and stores; reads and writes of the flags
 * and the SPSR; and branches, with and without link, and r15 written by
 * an add or a mov, with low bits for it to drop, forward only, so that
 * every program gets to its end. It starts from random registers that
 * point into the middle of memory. What it leaves is printed with the
 * cycles left over, which the recompilers count alike and the interpreter
 * its own way (run.sh compares them between recompilers only).
 *
 * Left out: anything else that writes r15, changes of mode
 * (but with $ARM7_TEST_MODES: see below), software interrupts and undefined instructions, and the block transfers
 * and writebacks the ARM manual calls unpredictable. A program that stores
 * over its own instructions is not compared: the recompiler does not look
 * for that, and the sound drivers do not do it.
 *
 * arm7_test [programs [first]] runs that many, from the first given.
 * $ARM7_TEST_INTERPRET has the build with the recompiler run them through
 * the interpreter it also has, and $ARM7_TEST_SWITCH has it change between
 * the two over its first samples; see run.sh.
 * $ARM7_TEST_MODES has one instruction in ten change the processor's mode,
 * to one of those with banked registers or system mode, with interrupts
 * off: other programs than without it.
 * $ARM7_TEST_CASE runs one; $ARM7_TEST_PRINT also prints its instructions,
 * and $ARM7_TEST_PROGRAM names a file of hex words to run in their place. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "types.h"
#include "hw/arm7/arm7.h"
#include "hw/aica/aica_if.h"
#include "log/Log.h"
#include "hw/mem/_vmem.h"

settings_t settings;
unsigned ARAM_SIZE = 2 * 1024 * 1024;
unsigned ARAM_MASK = 2 * 1024 * 1024 - 1;
VArray2 aica_ram;
DECL_ALIGN(4096) static u8 ram[2 * 1024 * 1024];

/* A check in the ARM7 code failed. That ends the program being run, not
 * the test: a program that has stored over its own instructions goes on to
 * run whatever it stored, and that can be anything. */
static jmp_buf stopped;
void os_DebugBreak(void) { longjmp(stopped, 1); }
/* (no log) */
retro_log_printf_t log_cb;
u32 libAICA_ReadReg(u32 addr, u32 size) { (void)addr; (void)size; return 0; }
void libAICA_WriteReg(u32 addr, u32 data, u32 size) { (void)addr; (void)data; (void)size; }
void libAICA_TimeStep(void) {}

bool vmem_platform_prepare_jit_block(void *code_area, unsigned size, void **code_area_rwx)
{
   const uintptr_t page = 4096;
   const uintptr_t from = (uintptr_t)code_area & ~(page - 1);
   const uintptr_t to   = ((uintptr_t)code_area + size + page - 1) & ~(page - 1);
#ifdef _WIN32
   DWORD old;
   if (!VirtualProtect((void*)from, to - from, PAGE_EXECUTE_READWRITE, &old))
      return false;
#else
   if (mprotect((void*)from, to - from, PROT_READ | PROT_WRITE | PROT_EXEC))
      return false;
#endif
   *code_area_rwx = code_area;
   return true;
}

void vmem_platform_flush_cache(void *icache_start, void *icache_end, void *dcache_start, void *dcache_end)
{
   (void)dcache_start;
   (void)dcache_end;
   __builtin___clear_cache((char *)icache_start, (char *)icache_end);
}

static bool modes;

static u64 seed;
static u32 rnd(void)
{
   seed = seed * 6364136223846793005ull + 1442695040888963407ull;
   return (u32)(seed >> 33);
}
static u32 pick(u32 n) { return rnd() % n; }

#define PROGRAM 0x1000u         /* where the program is */

/* The condition of the instruction before: the recompilers run a row of
 * instructions with the same one as one, until one changes the flags */
static u32 last_cond;

static u32 cond(void)
{
   /* the one before, one time in three; else always, three times in four,
    * and any condition but "never" the rest */
   if (pick(3))
      last_cond = (pick(4) ? 14u : pick(14)) << 28;
   return last_cond;
}

/* The second of two instructions that go together, made with the first */
static u32 pending;
/* Where each forward jump goes, and the mov pc, r12 of each pair: one of
 * those that a jump lands on is not run after its add, and is taken out */
static unsigned jump_to[64];
static bool pair_second[64];

/* a register for a value: r0-r12, now and then r15, which reads as the
 * address of the instruction plus 8 */
static u32 src_reg(void) { return pick(24) ? pick(13) : 15; }
static u32 dst_reg(void) { return pick(13); }

static u32 operand2(void)
{
   switch (pick(3))
   {
      case 0:  return (1u << 25) | (pick(16) << 8) | pick(256);            /* rotated immediate */
      case 1:  return (pick(32) << 7) | (pick(4) << 5) | src_reg();        /* shift by a count */
      default: return (pick(13) << 8) | (pick(4) << 5) | (1u << 4) | pick(13); /* shift by a register */
   }
}

static u32 instruction(unsigned at, unsigned count)
{
   if (pending)
   {
      const u32 next = pending;
      pending = 0;
      return next;
   }
   if (modes && pick(10) == 0)
   {
      /* MSR CPSR_c, #imm: interrupts off, and FIQ, IRQ, supervisor, abort,
       * undefined or system mode */
      static const u32 mode[] = { 0x11, 0x12, 0x13, 0x17, 0x1b, 0x1f };
      return cond() | 0x0321F0C0 | mode[pick(6)];
   }
   switch (pick(16))
   {
      default:                                           /* arithmetic and logic */
      {
         const u32 op = pick(16);
         const bool test = op >= 8 && op <= 11;         /* TST TEQ CMP CMN: flags only */
         return cond() | (op << 21) | ((test || pick(2)) ? 1u << 20 : 0)
            | (src_reg() << 16) | ((test ? 0 : dst_reg()) << 12) | operand2();
      }
      case 9:                                            /* multiply, multiply and add */
      {
         u32 rd = dst_reg(), rm = pick(13);
         if (rm == rd)
            rm = (rm + 1) % 13;
         return cond() | (pick(2) << 21) | (pick(2) << 20) | (rd << 16) | (pick(13) << 12)
            | (pick(13) << 8) | 0x90 | rm;
      }
      case 10: case 11: case 12:                         /* load or store, word or byte */
      {
         const u32 rn = pick(13);
         u32 rd = pick(13);
         const u32 pre = pick(2), wb = pick(2);
         u32 offset;
         if (rd == rn)
            rd = (rd + 1) % 13;
         if (pick(2))
            offset = pick(4096);
         else
         {
            u32 rm = pick(13);
            if (rm == rn)
               rm = (rm + 1) % 13;
            offset = (1u << 25) | (pick(32) << 7) | (pick(4) << 5) | rm;
         }
         return cond() | (1u << 26) | (pre << 24) | (pick(2) << 23) | (pick(2) << 22)
            | ((pre ? wb : 0) << 21) | (pick(2) << 20) | (rn << 16) | (rd << 12) | offset;
      }
      case 13:                                           /* block load or store */
      {
         const u32 rn = pick(13), load = pick(2);
         u32 list = rnd() & 0x1FFF;
         list &= ~(1u << rn);                           /* the base is not in the list */
         if (!list)
            list = 1u << ((rn + 1) % 13);
         return cond() | (4u << 25) | (pick(2) << 24) | (pick(2) << 23) | (pick(2) << 21)
            | (load << 20) | (rn << 16) | list;
      }
      case 14:                                           /* the flags, read or written */
      {
         /* CPSR, or the SPSR of the mode it runs in (supervisor, from
          * the reset, or one of $ARM7_TEST_MODES's, all with one) */
         const u32 spsr = (!modes && pick(4) == 0) ? 1u << 22 : 0;
         if (pick(2))
            return cond() | 0x010F0000 | spsr | (dst_reg() << 12);            /* MRS rd, PSR */
         if (pick(2))
            return cond() | 0x0128F000 | spsr | pick(13);                     /* MSR PSR_f, rm */
         return cond() | 0x0328F000 | spsr | (pick(16) << 8) | pick(256);    /* MSR PSR_f, #imm */
      }
      case 15:                                           /* a branch forward, or branch and link */
      {
         const u32 skip = pick(3);                      /* over 0 to 2 instructions */
         const u32 low = pick(4);                       /* what the pc drops */
         if (at + 3 + skip > count)
            return 0xE1A00000;                          /* too near the end: mov r0, r0 */
         switch (pick(4))
         {
            case 0:                                     /* add pc, pc, #: the pc written as a result */
               jump_to[at] = at + 2 + skip;
               return cond() | 0x028FF000 | (skip * 4 + low);
            case 1:                                     /* add r12, pc, #, then mov pc, r12 */
               pending = cond() | 0x01A0F00C;
               pair_second[at + 1] = true;
               return 0xE28FC000 | ((skip + 1) * 4 + low);
            default:
               jump_to[at] = at + 2 + skip;
               return cond() | (5u << 25) | (pick(2) << 24) | skip;
         }
      }
   }
}

static u64 fold(u64 h, const void *data, size_t size)
{
   const u8 *p = (const u8*)data;
   size_t i;

   for (i = 0; i + 8 <= size; i += 8)
   {
      u64 v;
      memcpy(&v, p + i, 8);
      h = (h ^ v) * 0x100000001b3ull;
      h ^= h >> 29;
   }
   return h;
}

static void run_case(unsigned number)
{
   u32 program[64];
   unsigned count, i;

   seed = 0x9E3779B97F4A7C15ull * (number + 1);
   last_cond = 14u << 28;
   pending = 0;
   memset(ram, 0, sizeof(ram));
   count = 8 + pick(33);
   memset(jump_to, 0, sizeof(jump_to));
   memset(pair_second, 0, sizeof(pair_second));
   for (i = 0; i < count; i++)
      program[i] = instruction(i, count);
   for (i = 0; i < count; i++)
      if (jump_to[i] && pair_second[jump_to[i]])
         program[jump_to[i]] = 0xE1A00000;             /* mov r0, r0 */
   program[count] = 0xEAFFFFFE;                         /* b . */
   if (getenv("ARM7_TEST_PROGRAM"))
   {
      FILE *f = fopen(getenv("ARM7_TEST_PROGRAM"), "r");
      for (i = 0; i <= count; i++)
         if (fscanf(f, "%x", &program[i]) != 1)
            program[i] = 0xEAFFFFFE;
      fclose(f);
   }
   if (getenv("ARM7_TEST_PRINT"))
   {
      for (i = 0; i <= count; i++)
         fprintf(stderr, "%08x ", program[i]);
      fprintf(stderr, "\n");
   }
   memcpy(&ram[PROGRAM], program, (count + 1) * 4);
   *(u32*)&ram[0] = 0xEA000000 | ((PROGRAM - 8) / 4);   /* the reset vector: b PROGRAM */
   /* something to load */
   for (i = 0x100000; i < 0x100000 + 0x10000; i += 4)
      *(u32*)&ram[i] = rnd();

   aicaarm_enable(false);
   aicaarm_enable(true);                               /* which resets it */
   for (i = 0; i < 13; i++)
      arm_Reg[i].I = pick(4) ? 0x100000 + (pick(0x4000) << 2) + pick(4) : rnd();
   arm_Reg[13].I = 0x180000;

   if (setjmp(stopped))
   {
      printf("%u %u stopped on a check of its own\n", number, count);
      return;
   }
   /* how the build with the recompiler in it is asked for the interpreter
    * instead, as the core option does: for the whole program, or changing
    * over and back while it runs */
   if (getenv("ARM7_TEST_INTERPRET"))
   {
      settings.dynarec.Arm7Interpreter = true;
      aicaarm_run(64);
      settings.dynarec.Arm7Interpreter = false;
   }
   else if (getenv("ARM7_TEST_SWITCH"))
   {
      /* over the first samples, which is where a program this short
       * does its work; every change back to the recompiler makes it start
       * over, which takes time, so not for all 64 */
      for (i = 0; i < 4; i++)
      {
         settings.dynarec.Arm7Interpreter = (number + i) & 1;
         aicaarm_run(1);
      }
      settings.dynarec.Arm7Interpreter = false;
      aicaarm_run(60);
   }
   else
      aicaarm_run(64);

   if (memcmp(&ram[PROGRAM], program, (count + 1) * 4) || *(u32*)&ram[0] != (0xEA000000 | ((PROGRAM - 8) / 4)))
   {
      printf("%u %u wrote over its own instructions: not compared\n", number, count);
      return;
   }
   CPUUpdateCPSR();
   printf("%u %u", number, count);
   for (i = 0; i < 15; i++)
      printf(" %08x", arm_Reg[i].I);
   printf(" pc %08x cpsr %08x mem %016llx cyc %08x\n", arm_Reg[R15_ARM_NEXT].I, arm_Reg[RN_CPSR].I,
         (unsigned long long)fold(0xcbf29ce484222325ull, ram, sizeof(ram)), arm_Reg[CYCL_CNT].I);
}

int main(int argc, char **argv)
{
   const unsigned cases = argc > 1 ? (unsigned)atoi(argv[1]) : 1000;
   unsigned i;

   modes = getenv("ARM7_TEST_MODES") != NULL;
   aica_ram.data = ram;
   aica_ram.size = sizeof(ram);
   aicaarm_init();
   if (getenv("ARM7_TEST_CASE"))
      run_case((unsigned)atoi(getenv("ARM7_TEST_CASE")));
   else
      for (i = argc > 2 ? (unsigned)atoi(argv[2]) : 0; i < cases; i++)
         run_case(i);
   return 0;
}
