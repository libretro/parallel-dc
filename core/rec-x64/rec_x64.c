#include "build.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X64
#include <setjmp.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#include "types.h"
#include "jit/x64_emit.h"
#include "x64_vector.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/dyna/ssa_regalloc.h"
#include "hw/sh4/modules/ccn.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_interpreter.h"

#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/sh4/dyna/wait_site.h"
#include "hw/holly/sb_mem.h"
#include "hw/mem/_vmem.h"

/* One block goes on to the next with a jump straight to its code.
 *
 * Where a block ends in a jump whose target is known as it is compiled -
 * an unconditional one, or either way out of a conditional one - there is
 * a link site: seven bytes that are one of three things.
 *
 *   call the link stub   as compiled. The first time the block leaves
 *                        that way, ngen_link() has the target found or
 *                        compiled and makes the site one of the other two.
 *   jmp target           linked: the next block's code, directly.
 *   jmp [table]          through the table of blocks, as every block went
 *                        on before: for a way out that is not to be
 *                        linked - into or out of a block in the temporary
 *                        cache, which is thrown away without its
 *                        neighbours being told.
 *
 * A jump through the table is a load and an indirect jump, at the end of
 * every block; in a loop that is one block - half a dozen instructions
 * that jump back to their own start - they were a good part of what a turn
 * cost.
 *
 * When a block is thrown away the block manager goes through the blocks
 * that are linked to it (pre_refs), takes it out of them and has them
 * linked again (ngen_Relink()): their sites go back to calling the stub,
 * which finds the new block that is compiled in its place. A block that is
 * thrown away is unlinked the same way, since it may still be running -
 * it may be what wrote over the code - and has yet to leave. */

/* What is left of the time slice. Recompiled code has it in r14, where
 * taking a block's cycles off it is one instruction on a register; it was
 * this word, read, changed and written back by every block.
 *
 * The word is still what the main loop starts from - at its beginning,
 * and when an exception has come back to it by longjmp(), which puts r14
 * back to what it was at the setjmp(). So a block that can raise one - a
 * block compiled with the MMU on - also writes the word, once, when it has
 * taken its cycles off; and the routines that raise one add theirs to the
 * word. With the MMU off nothing comes back that way and nothing writes
 * it. */
int cycle_counter;

#ifdef __MACH__
#define _U "_"
#else
#define _U
#endif

#ifdef _WIN32
#define WIN32_ONLY(x) x
#else
#define WIN32_ONLY(x)
#endif

#define STRINGIFY(x) #x
#define _S(x) STRINGIFY(x)
#define CPU_RUNNING 135266148
#define PC 135266120
#define SCHED_NEXT 135266152
#define INTERRUPT_PEND 135266156
#define CYCLE_COUNTER 135266168
/* Recompiled code keeps r15 pointing into the SH4's context, so that a
 * register in it is reached with one short instruction where it took a
 * 64-bit address in rax and then the access. It points 160 bytes in: a
 * signed byte then reaches the general registers, the floating-point ones
 * but the first eight of the second bank, and pc, pr, T and FPSCR.
 * CTX_BASE is that place from the start of the block holding the context. */
#define CTX_BIAS 160
#define CTX_BASE 135266016

jmp_buf jmp_env;

#ifdef _WIN32
        /* Fully naked function in win32 for proper SEH prologue */
        __asm__ (
                        ".text                                                  \n\t"
                        ".p2align 4,,15                                 \n\t"
                        ".globl ngen_mainloop                   \n\t"
                        ".def   ngen_mainloop;  .scl    2;      .type   32;     .endef  \n\t"
                        ".seh_proc      ngen_mainloop           \n\t"
                "ngen_mainloop:                                         \n\t"
#else
void ngen_mainloop(void* v_cntx)
{
        (void)v_cntx;
        __asm__ (
#endif
                        "pushq %rbx                                             \n\t"
WIN32_ONLY(     ".seh_pushreg %rbx                              \n\t")
                        "pushq %rbp                                             \n\t"
#ifdef _WIN32
                        ".seh_pushreg %rbp                              \n\t"
                        "pushq %rdi                                             \n\t"
                        ".seh_pushreg %rdi                              \n\t"
                        "pushq %rsi                                             \n\t"
                        ".seh_pushreg %rsi                              \n\t"
#endif
                        "pushq %r12                                             \n\t"
WIN32_ONLY(     ".seh_pushreg %r12                              \n\t")
                        "pushq %r13                                             \n\t"
WIN32_ONLY(     ".seh_pushreg %r13                              \n\t")
                        "pushq %r14                                             \n\t"
WIN32_ONLY(     ".seh_pushreg %r14                              \n\t")
                        "pushq %r15                                             \n\t"
#ifdef _WIN32
                        ".seh_pushreg %r15                              \n\t"
                        /* 32-byte shadow space + 8 for stack 16-byte alignment */
                        "subq $40, %rsp                                 \n\t"
                        ".seh_stackalloc 40                             \n\t"
                        ".seh_endprologue                               \n\t"
#else
                        /* 8 for stack 16-byte alignment */
                        "subq $8, %rsp                                  \n\t"
#endif
                        "movl $" _S(SH4_TIMESLICE) "," _U "cycle_counter(%rip)  \n\t"
                        /* the context, for all recompiled code */
                        "movq " _U "p_sh4rcb(%rip), %r15                 \n\t"
                        "addq $" _S(CTX_BASE) ", %r15                    \n"

#ifdef _WIN32
                        /* SETJMP, with no frame pointer */
                        "leaq " _U "jmp_env(%rip), %rcx                 \n\t"
                        "xor %rdx, %rdx                                 \n\t"
#else
                        "leaq " _U "jmp_env(%rip), %rdi                 \n\t"
#endif
#if defined(__MACH__) || defined(_WIN32)
                        "call " _U "setjmp                              \n\t"
#else
                        "call " _U "setjmp@PLT                          \n\t"
#endif
                        /* (here at the start, and again after every longjmp) */
                        "movl " _U "cycle_counter(%rip), %r14d  \n\t"

                /* run_loop */
                "1:                                                     \n\t"
                        "movq " _U "p_sh4rcb(%rip), %rax       \n\t"
                        "movl " _S(CPU_RUNNING) "(%rax), %edx  \n\t"
                        "testl %edx, %edx                      \n\t"
                        /* end_run_loop */
                        "je 3f                                                          \n"

                /* slice_loop */
                "2:                                                                             \n\t"
                        "movq " _U "p_sh4rcb(%rip), %rax        \n\t"
#ifdef _WIN32
                        "movl " _S(PC)"(%rax), %ecx     \n\t"
#else
                        "movl " _S(PC)"(%rax), %edi     \n\t"
#endif
                        "call " _U "bm_GetCodeByVAddr				\n\t"
                        /* A block is jumped to, not called, and comes back by
                         * jumping to the label below: recompiled code runs on
                         * this function's stack as it stands - a multiple of
                         * 16 for the calls it makes, with the 32 bytes Windows
                         * wants above them already there - so no block has a
                         * stack to set up or put back, and one block goes on
                         * to the next without either. The address it is for
                         * goes with it in edx: see ngen_compile_stub. */
                        "movq " _U "p_sh4rcb(%rip), %rcx        \n\t"
                        "movl " _S(PC)"(%rcx), %edx     \n\t"
                        "jmp *%rax                                              \n\t"
                        ".globl " _U "ngen_block_return         \n"
                _U "ngen_block_return:                                  \n\t"
                        "testl %r14d, %r14d                                     \n\t"
                        /* slice_loop */
                        "jg 2b                                                          \n\t"

                        "addl $" _S(SH4_TIMESLICE) ", %r14d             \n\t"
                        /* UpdateSystem_INTC(), here, for the slices - most
                         * of them - at whose end no event is due and no
                         * interrupt waits: the slice is taken off the time
                         * to the next event, and that is all it would have
                         * done. Otherwise it is called, and does all of it. */
                        "movq " _U "p_sh4rcb(%rip), %rax        \n\t"
                        "movl " _S(SCHED_NEXT) "(%rax), %edx   \n\t"
                        "subl $" _S(SH4_TIMESLICE) ", %edx              \n\t"
                        "js 4f                                                          \n\t"
                        "cmpl $0, " _S(INTERRUPT_PEND) "(%rax) \n\t"
                        "jne 4f                                                         \n\t"
                        "movl %edx, " _S(SCHED_NEXT) "(%rax)   \n\t"
                        "jmp 1b                                                         \n"
                "4:                                                                             \n\t"
                        /* (the timers' time is the end of the slice here: see cycle_counter in sh4_if.h) */
                        "movl $" _S(SH4_TIMESLICE) ", " _S(CYCLE_COUNTER) "(%rax)   \n\t"
                        "call " _U "UpdateSystem_INTC           \n\t"
                        /* run_loop */
                        "jmp 1b                                                         \n"

                /* end_run_loop */
                "3:                                                                             \n\t"

#ifdef _WIN32
                        "addq $40, %rsp                                         \n\t"
#else
                        "addq $8, %rsp                                          \n\t"
#endif
                        "popq %r15                                                      \n\t"
                        "popq %r14                                                      \n\t"
                        "popq %r13                                                      \n\t"
                        "popq %r12                                                      \n\t"
#ifdef _WIN32
                        "popq %rsi                                                      \n\t"
                        "popq %rdi                                                      \n\t"
#endif
                        "popq %rbp                                                      \n\t"
                        "popq %rbx                                                      \n\t"
#ifdef _WIN32
                        "ret                                                            \n\t"
                        ".seh_endproc                   \n"
        );
#else
        );
}
#endif

/* What the table of blocks holds for an address with no block yet. It is
 * reached as a block is - jumped to, from the main loop or from the end of
 * another block, with the address in edx - has the block compiled, and
 * goes back to the main loop, which finds it there. */
void ngen_block_return(void);
void ngen_compile_stub(void);
#ifdef __GNUC__
__attribute__((used))
#endif
void ngen_compile_missing(u32 pc)
{
	rdv_FailedToFindBlock(pc);
}
__asm__ (
		".text                                  \n\t"
		".p2align 4                             \n\t"
		".globl " _U "ngen_compile_stub         \n"
	_U "ngen_compile_stub:                      \n\t"
#ifdef _WIN32
		"movl %edx, %ecx                        \n\t"
#else
		"movl %edx, %edi                        \n\t"
#endif
		"call " _U "ngen_compile_missing        \n\t"
		"jmp " _U "ngen_block_return            \n"
);

#ifndef _WIN32
/* Calls the function whose address is in rax, with the arguments as they
 * are, and keeps xmm8 to xmm15 across it: the floating-point registers
 * recompiled code is handed, which no function has to keep on these hosts.
 *
 * For a fast memory access that faulted and was written over with a call
 * (ngen_Rewrite): the code around it was compiled for a move, which leaves
 * every register alone, so whatever of those four is in use there has to
 * survive, and there is no room at the place itself to save them. The low
 * 32 bits are what recompiled code keeps in them. */
void ngen_call_keep_xmm(void);
__asm__ (
		".text                                  \n\t"
		".p2align 4                             \n\t"
		".globl " _U "ngen_call_keep_xmm        \n"
	_U "ngen_call_keep_xmm:                     \n\t"
		/* 32 for the eight, and the stack back to a multiple of 16 */
		"subq $40, %rsp                         \n\t"
		"movd %xmm8, 0(%rsp)                    \n\t"
		"movd %xmm9, 4(%rsp)                    \n\t"
		"movd %xmm10, 8(%rsp)                   \n\t"
		"movd %xmm11, 12(%rsp)                  \n\t"
		"movd %xmm12, 16(%rsp)                  \n\t"
		"movd %xmm13, 20(%rsp)                  \n\t"
		"movd %xmm14, 24(%rsp)                  \n\t"
		"movd %xmm15, 28(%rsp)                  \n\t"
		"call *%rax                             \n\t"
		"movd 0(%rsp), %xmm8                    \n\t"
		"movd 4(%rsp), %xmm9                    \n\t"
		"movd 8(%rsp), %xmm10                   \n\t"
		"movd 12(%rsp), %xmm11                  \n\t"
		"movd 16(%rsp), %xmm12                  \n\t"
		"movd 20(%rsp), %xmm13                  \n\t"
		"movd 24(%rsp), %xmm14                  \n\t"
		"movd 28(%rsp), %xmm15                  \n\t"
		"addq $40, %rsp                         \n\t"
		"ret                                    \n"
);
#endif

#undef _U
#undef _S

/* The table of blocks is first filled before ngen_init() is called, and a
 * place in it filled with the C function this replaces would be jumped to
 * as a block is - with nowhere to return to. So: from the start. */
#ifdef __GNUC__
__attribute__((constructor))
#endif
static void compile_stub_init(void)
{
	ngen_FailedToFindBlock = &ngen_compile_stub;
}

/* The registers a block is handed: r15 is not, as recompiled code keeps
 * the address of the SH4's context in it (see CTX_BIAS). Nor is r14, which
 * has what is left of the time slice (cycle_counter) - it was the fifth
 * register a block could have on Linux, and blocks came out no smaller and
 * no quicker for having it. */
#ifdef _WIN32
static const u32 alloc_regs[] = { X86_EBX, X86_EBP, X86_EDI, X86_ESI, X86_R12, X86_R13 };
/* XMM6 to XMM15 are callee-saved in Windows */
static const u32 alloc_fregs[] = { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
#else
static const u32 alloc_regs[] = { X86_EBX, X86_EBP, X86_R12, X86_R13 };
/* XMM8 to XMM15. No function has to keep any of them on these hosts: the
 * ones in use are saved round every call recompiled code makes (gen_call). */
static const u32 alloc_fregs[] = { 8, 9, 10, 11, 12, 13, 14, 15 };
#endif
#define ALLOC_REGS ((int)(sizeof(alloc_regs) / sizeof(alloc_regs[0])))
#define ALLOC_FREGS ((int)(sizeof(alloc_fregs) / sizeof(alloc_fregs[0])))
/* A pair of the SH4's floating-point registers (DRn, XDn) is kept in two
 * xmm registers, as single ones are. On hosts where no function keeps an
 * xmm register, the ones in use are saved round every call (gen_call) and
 * round a rewritten memory access (ngen_call_keep_xmm), pairs as well. */
#define ALLOC_F64 true

/* The registers a function takes its arguments in */
#ifdef _WIN32
static const int call_regs[4] = { X86_ECX, X86_EDX, X86_R8, X86_R9 };
#define MEM_REG_ADDR  X86_ECX
#define MEM_REG_STORE X86_EDX
#else
static const int call_regs[4] = { X86_EDI, X86_ESI, X86_EDX, X86_ECX };
#define MEM_REG_ADDR  X86_EDI
#define MEM_REG_STORE X86_ESI
#endif

/* The kinds of fast memory access there are (fast_memory()): a load or a
 * store, of 1, 2, 4 or 8 bytes, the 4 from or to a general register or a
 * floating-point one. */
enum
{
	MEM_LOAD8, MEM_LOAD16, MEM_LOAD32, MEM_LOAD32F, MEM_LOAD64,
	MEM_STORE8, MEM_STORE16, MEM_STORE32, MEM_STORE32F, MEM_STORE64,
	MEM_KINDS
};
/* (whether a kind's data is in a floating-point register) */
#define MEM_KIND_F(kind) ((kind) == MEM_LOAD32F || (kind) == MEM_STORE32F)

/* What a fast memory access that faulted is written over with a call to
 * (ngen_Rewrite()): a routine that does the access by asking.
 *
 * A fast access has its address and its data in whatever registers the
 * block has them in, so there is a routine for each kind of access, each
 * register the address can be in and each the data can be in: 240 of
 * them on Linux and macOS and 448 on Windows, a few instructions each,
 * written once. They are in the code cache, like the link stub, so that a
 * near call reaches them. The access they stand in for changes nothing
 * but its data, so they keep the block's floating-point registers: by
 * going through ngen_call_keep_xmm, or, on Windows, by those being ones
 * every function keeps.
 *
 * The registers, by number: for the address and for data in a general
 * register, 0 is the one a call has it in (the first argument; rax for
 * what is loaded, the second argument for what is stored) and 1 on are
 * the block's own, alloc_regs; for data in a floating-point register, 0
 * on are the block's, alloc_fregs. 64 bits are always in rax or the
 * second argument. */
#define MEM_REGS 8
#define MEM_FREGS 10
static u8 *mem_stubs[MEM_KINDS][MEM_REGS][MEM_FREGS];

/* What recompiled code calls (global: tools/sh4/x64_run.sh builds the C++
 * recompiler this replaced against them) */
s32 rec_x64_mem_load8(u32 addr)  { return (s8)ReadMem8(addr); }
s32 rec_x64_mem_load16(u32 addr) { return (s16)ReadMem16(addr); }
u32 rec_x64_mem_load32(u32 addr) { return ReadMem32(addr); }
u64 rec_x64_mem_load64(u32 addr) { return ReadMem64(addr); }
void rec_x64_mem_store8(u32 addr, u32 data)  { WriteMem8(addr, (u8)data); }
void rec_x64_mem_store16(u32 addr, u32 data) { WriteMem16(addr, (u16)data); }
void rec_x64_mem_store32(u32 addr, u32 data) { WriteMem32(addr, data); }
void rec_x64_mem_store64(u32 addr, u64 data) { WriteMem64(addr, data); }

/* The number a general register goes by here: 0 if it is @first, 1 on for
 * the block's own, -1 for any other. */
static int mem_reg_number(int reg, int first)
{
	int i;
	if (reg == first)
		return 0;
	for (i = 0; i < ALLOC_REGS; i++)
		if ((int)alloc_regs[i] == reg)
			return i + 1;
	return -1;
}

/* The same for a floating-point register, which is one of the block's or nothing */
static int mem_freg_number(int reg)
{
	int i;
	for (i = 0; i < ALLOC_FREGS; i++)
		if ((int)alloc_fregs[i] == reg)
			return i;
	return -1;
}

static void mem_stub_make(int kind, int addr, int data)
{
	static const uintptr_t routines[MEM_KINDS] = {
		(uintptr_t)&rec_x64_mem_load8, (uintptr_t)&rec_x64_mem_load16, (uintptr_t)&rec_x64_mem_load32,
		(uintptr_t)&rec_x64_mem_load32, (uintptr_t)&rec_x64_mem_load64,
		(uintptr_t)&rec_x64_mem_store8, (uintptr_t)&rec_x64_mem_store16, (uintptr_t)&rec_x64_mem_store32,
		(uintptr_t)&rec_x64_mem_store32, (uintptr_t)&rec_x64_mem_store64,
	};
	const bool load = kind < MEM_STORE8;
	/* (what is loaded has to be put somewhere after the call, unless rax is where it is wanted) */
	const bool after = load && (MEM_KIND_F(kind) || data != 0);
	x86_emit s;

	x86_init(&s, (u8 *)emit_GetCCPtr(), 64);
	mem_stubs[kind][addr][data] = s.p;
	/* what is left of the time slice, for the timers (see cycle_counter in sh4_if.h) */
	x86_store(&s, x86_m(X86_R15, CYCLE_COUNTER - CTX_BASE), X86_R14);
	/* the address and what is stored to where a function takes them (neither is ever in the other's place) */
	if (addr != 0)
		x86_mov_rr(&s, MEM_REG_ADDR, (int)alloc_regs[addr - 1]);
	if (!load && MEM_KIND_F(kind))
		x64_movd_rx(&s, MEM_REG_STORE, (int)alloc_fregs[data]);
	else if (!load && data != 0)
		x86_mov_rr(&s, MEM_REG_STORE, (int)alloc_regs[data - 1]);
	x86_mov_ri64(&s, X86_EAX, routines[kind]);
#ifdef _WIN32
	/* room for the function to keep its arguments in, and the stack a multiple of 16 */
	x86_alu_ri64(&s, X86_SUB, X86_ESP, 40);
	x64_call_r(&s, X86_EAX);
	x86_alu_ri64(&s, X86_ADD, X86_ESP, 40);
#else
	x86_mov_ri64(&s, X86_R11, (uintptr_t)&ngen_call_keep_xmm);
	if (after)
	{
		x86_alu_ri64(&s, X86_SUB, X86_ESP, 8);
		x64_call_r(&s, X86_R11);
		x86_alu_ri64(&s, X86_ADD, X86_ESP, 8);
	}
	else
		x64_jmp_r(&s, X86_R11);
#endif
	if (after)
	{
		if (MEM_KIND_F(kind))
			x64_movd_xr(&s, (int)alloc_fregs[data], X86_EAX);
		else
			x86_mov_rr(&s, (int)alloc_regs[data - 1], X86_EAX);
	}
#ifdef _WIN32
	x86_ret(&s);
#else
	if (after)
		x86_ret(&s);
#endif
	verify(!s.full);
	emit_Skip(((u32)(s.p - s.start) + 15) & ~15u);
}

/* The link stub, which is in the code cache so that every block reaches
 * it with a near call: see RuntimeBlockInfo::link_at. In the cache's
 * writable mapping; a block is the same distance from it in the executable
 * one. */
static u8 *link_stub;

/* Where entry @index of the table of blocks is from r15. The table is the
 * first thing in the block of memory the context is at the end of, so r15,
 * which every block has, reaches it: the table's address does not have to
 * be loaded - ten bytes and an instruction less at every way out of a
 * block. */
static int fpcb_at(u32 index)
{
	return (int)((ptrdiff_t)(offsetof(Sh4RCB, fpcb) + (size_t)index * sizeof(void *)) - (ptrdiff_t)CTX_BASE);
}

static void link_site_call_stub(u8 *site)
{
	const s32 rel = (s32)(link_stub - (site + 5));

	site[0] = 0xE8;		/* call rel32 */
	memcpy(site + 1, &rel, 4);
	site[5] = 0xCC;
	site[6] = 0xCC;
}

static void link_site_jump(u8 *site, const u8 *code)
{
	const s32 rel = (s32)(code - (site + 5));

	site[0] = 0xE9;		/* jmp rel32 */
	memcpy(site + 1, &rel, 4);
	site[5] = 0xCC;
	site[6] = 0xCC;
}

u32 ngen_Relink(RuntimeBlockInfo *block)
{
	int i;
	for (i = 0; i < 2; i++)
	{
		const RuntimeBlockInfo *to = i == 0 ? block->pBranchBlock : block->pNextBlock;
		u8 *const code = (u8 *)(uintptr_t)block->code;

		if (block->link_at[i] == 0)
			continue;
		if (to != NULL)
			link_site_jump(code + block->link_at[i], (const u8 *)(uintptr_t)to->code + to->linked_entry);
		else
			link_site_call_stub(code + block->link_at[i]);
	}
	return 0;
}

/* From the link stub: the block whose link site returns to @after is
 * leaving for @pc for the first time. The block there is found or
 * compiled, the site is made a jump to it if the two may be linked, and
 * its code is where the stub goes. */
#ifdef __GNUC__
__attribute__((used))
#endif
void *ngen_link(u8 *after, u32 pc)
{
	u8 *const site_rx = after - 5;
	RuntimeBlockInfoPtr from = bm_GetBlock2(site_rx);
	const bool mmu = mmu_enabled();
	/* where @pc is, for the block manager */
	u32 addr = pc;
	DynarecCodeEntryPtr code;
	RuntimeBlockInfo *block;
	RuntimeBlockInfoPtr to;
	u8 *site;
	u32 at;
	int which;

	if (mmu)
	{
		/* With the MMU on a block has a link site only for an address
		 * that is where it is whatever the TLB says (rdv_MmuSamePlace()),
		 * and the context has the address as well: gen_go_on(). So nothing
		 * is translated here - and no exception raised in passing.
		 *
		 * Unless the block is the one that turned the MMU on. It was
		 * compiled for a machine without one, has a site for anywhere it
		 * goes and put nothing in the context: the main loop, sent there
		 * with the context as it was, ran the block a second time. */
		if (from == NULL || !rdv_MmuSamePlace(from, pc, &addr))
		{
			next_pc = pc;
			return (void *)(uintptr_t)&ngen_block_return;
		}
		code = (DynarecCodeEntryPtr)(uintptr_t)p_sh4rcb->fpcb[(addr >> 1) & FPCB_MASK];
	}
	else
		code = bm_GetCodeByVAddr(pc);
	if (code == ngen_FailedToFindBlock)
	{
		code = rdv_FailedToFindBlock(pc);
		/* (compiling it was an exception instead: the context says where to) */
		if (mmu && next_pc != pc)
			return (void *)(uintptr_t)&ngen_block_return;
	}
	/* No block to be had there: to the main loop, which goes by the pc in
	 * the context. (The compile stub, which is what there is in a block's
	 * place then, wants the address in edx, and this has used edx.) */
	if (code == ngen_FailedToFindBlock)
	{
		next_pc = pc;
		return (void *)(uintptr_t)&ngen_block_return;
	}

	/* Compiling may have emptied the cache, and the block that asked with
	 * it; or the block was thrown away before it got here (it wrote over
	 * its own code), and is not found at all. Nothing is linked then. */
	if (from == NULL || bm_GetBlock2(site_rx) != from)
		return (void *)(uintptr_t)code;

	block = from;
	site = (u8 *)CC_RX2RW(site_rx);
	at = (u32)(site - (u8 *)(uintptr_t)block->code);
	which = at == block->link_at[0] ? 0 : at == block->link_at[1] ? 1 : -1;
	to = bm_GetBlock(addr);

	if (which < 0 || pc != (which == 0 ? block->BranchBlock : block->NextBlock))
		return (void *)(uintptr_t)code;		/* not a site of this block's: left as it is */
	if (to == NULL || to->addr != addr
			|| (DynarecCodeEntryPtr)(uintptr_t)CC_RW2RX((void *)(uintptr_t)to->code) != code)
		return (void *)(uintptr_t)code;		/* nothing to link to (yet) */
	/* (with the MMU on a block is compiled for the address it was first run at) */
	if (mmu && to->vaddr != pc)
		return (void *)(uintptr_t)code;

	if (to->temp_block)
	{
		/* Not linked, ever: through the table from now on.
		 * jmp qword [r15 + disp32] */
		const s32 disp = fpcb_at((addr >> 1) & FPCB_MASK);

		site[0] = 0x41;
		site[1] = 0xFF;
		site[2] = 0xA7;
		memcpy(site + 3, &disp, 4);
		block->link_at[which] = 0;
		return (void *)(uintptr_t)code;
	}

	if (which == 0)
		block->pBranchBlock = to;
	else
		block->pNextBlock = to;
	bm_BlockAddRef(to, from);
	ngen_Relink(block);
	return (void *)(uintptr_t)code;
}

void ngen_init(void)
{
	x86_emit s;
	int kind, addr, data;
	const int regs = 1 + ALLOC_REGS;
	const int fregs = ALLOC_FREGS;

	ngen_FailedToFindBlock = &ngen_compile_stub;

	/* The link stub, ahead of the blocks, where emptying the cache leaves
	 * it. It is called from a link site with the address the block is
	 * leaving for in edx, as every block is left: the site's return
	 * address and that address are ngen_link()'s arguments, and what it
	 * returns is jumped to. (The stack is the main loop's, as it is for
	 * any call a block makes, once the return address is off it.) */
	x86_init(&s, (u8 *)emit_GetCCPtr(), 64);
	link_stub = s.p;
#ifdef _WIN32
	x86_pop(&s, X86_ECX);
	/* (edx is the second argument already) */
#else
	x86_pop(&s, X86_EDI);
	x86_mov_rr(&s, X86_ESI, X86_EDX);
#endif
	x86_mov_ri64(&s, X86_EAX, (uintptr_t)&ngen_link);
	x64_call_r(&s, X86_EAX);
	x64_jmp_r(&s, X86_EAX);
	verify(!s.full);
	emit_Skip(((u32)(s.p - s.start) + 15) & ~15u);

	verify(regs <= MEM_REGS && fregs <= MEM_FREGS);
	for (kind = 0; kind < MEM_KINDS; kind++)
		for (addr = 0; addr < regs; addr++)
		{
			const int datas = MEM_KIND_F(kind) ? fregs : kind == MEM_LOAD64 || kind == MEM_STORE64 ? 1 : regs;

			for (data = 0; data < datas; data++)
				mem_stub_make(kind, addr, data);
		}
	emit_SetBaseAddr();
}

void ngen_ResetBlocks(void)
{
}

void ngen_GetFeatures(ngen_features* dst)
{
	dst->InterpreterFallback = false;
	dst->OnlyDynamicEnds = false;
}

void ngen_blockcheckfail(u32 pc)
{
	rdv_BlockCheckFail(pc);
}

static void handle_mem_exception(u32 exception_raised, u32 pc)
{
	if (exception_raised)
	{
		/* (an odd pc: in a delay slot) */
		if (pc & 1)
			spc = pc - 1;
		else
			spc = pc;
		cycle_counter += CPU_RATIO * 2;	/* probably more is needed but no easy way to find out */
		longjmp(jmp_env, 1);
	}
}

/* With the MMU on, an access by the address translated, and the exception
 * raised if it is not to be had */
static u32 read_noex_paddr(u32 addr, u32 pc, u32 size)
{
	u32 paddr = 0;
#ifndef NO_MMU
	const u32 rv = mmu_data_translation_c(addr, &paddr, MMU_TT_DREAD, size);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(addr, rv, MMU_TT_DREAD);
		handle_mem_exception(1, pc);
	}
	/* where the host's mapping does not do the translating, the next read
	 * of the page need not come here: see mmu.h */
	mmu_lut_fill(addr, paddr, false);
#else
	(void)addr;
	(void)pc;
	(void)size;
#endif
	return paddr;
}

static u32 write_noex_paddr(u32 addr, u32 pc, u32 size)
{
	u32 paddr = 0;
#ifndef NO_MMU
	const u32 rv = mmu_data_translation_c(addr, &paddr, MMU_TT_DWRITE, size);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(addr, rv, MMU_TT_DWRITE);
		handle_mem_exception(1, pc);
	}
	mmu_lut_fill(addr, paddr, true);
#else
	(void)addr;
	(void)pc;
	(void)size;
#endif
	return paddr;
}

u8 rec_x64_read_noex8(u32 addr, u32 pc)
{
#ifndef NO_MMU
	return _vmem_ReadMem8(read_noex_paddr(addr, pc, 1));
#else
	return 0;
#endif
}

u16 rec_x64_read_noex16(u32 addr, u32 pc)
{
#ifndef NO_MMU
	return _vmem_ReadMem16(read_noex_paddr(addr, pc, 2));
#else
	return 0;
#endif
}

u32 rec_x64_read_noex32(u32 addr, u32 pc)
{
#ifndef NO_MMU
	return _vmem_ReadMem32(read_noex_paddr(addr, pc, 4));
#else
	return 0;
#endif
}

u64 rec_x64_read_noex64(u32 addr, u32 pc)
{
#ifndef NO_MMU
	/* (two accesses, each translated: mmu.h) */
	if (mmu_in_two_pages(addr))
	{
		const u32 low = rec_x64_read_noex32(addr, pc);
		return ((u64)rec_x64_read_noex32(addr + 4, pc) << 32) | low;
	}
	return _vmem_ReadMem64(read_noex_paddr(addr, pc, 8));
#else
	return 0;
#endif
}

void rec_x64_write_noex8(u32 addr, u8 data, u32 pc)
{
#ifndef NO_MMU
	_vmem_WriteMem8(write_noex_paddr(addr, pc, 1), data);
#endif
}

void rec_x64_write_noex16(u32 addr, u16 data, u32 pc)
{
#ifndef NO_MMU
	_vmem_WriteMem16(write_noex_paddr(addr, pc, 2), data);
#endif
}

void rec_x64_write_noex32(u32 addr, u32 data, u32 pc)
{
#ifndef NO_MMU
	_vmem_WriteMem32(write_noex_paddr(addr, pc, 4), data);
#endif
}

void rec_x64_write_noex64(u32 addr, u64 data, u32 pc)
{
#ifndef NO_MMU
	if (mmu_in_two_pages(addr))
	{
		rec_x64_write_noex32(addr, (u32)data, pc);
		rec_x64_write_noex32(addr + 4, (u32)(data >> 32), pc);
		return;
	}
	_vmem_WriteMem64(write_noex_paddr(addr, pc, 8), data);
#endif
}

static void handle_sh4_exception(SH4ThrownException *ex, u32 pc)
{
	/* (an odd pc: in a delay slot) */
	if (pc & 1)
	{
		AdjustDelaySlotException(ex);
		pc--;
	}
	Do_Exception(pc, ex->expEvn, ex->callVect);
	cycle_counter += CPU_RATIO * 4;	/* probably more is needed */
	longjmp(jmp_env, 1);
}

void rec_x64_interpreter_fallback(u16 op, OpCallFP *oph, u32 pc)
{
	sh4_catch c;
	SH4_TRY(c)
	{
		oph(op);
		sh4_catch_pop(&c);
	}
	else
		handle_sh4_exception(&c.ex, pc);
}

void rec_x64_do_sqw_mmu_no_ex(u32 addr, u32 pc)
{
	sh4_catch c;
	SH4_TRY(c)
	{
		do_sqw_mmu(addr);
		sh4_catch_pop(&c);
	}
	else
		handle_sh4_exception(&c.ex, pc);
}

void rec_x64_do_sqw_nommu_local(u32 addr, u8* sqb)
{
	do_sqw_nommu(addr, sqb);
}

/* The processor's features, found once (-1 until then; global, so that
 * tools/sh4/x64_test.c can have each set compiled for) */
enum { X64_CPU_AVX = 1, X64_CPU_FMA = 2, X64_CPU_AVX512F = 4 };
int rec_x64_cpu = -1;

static void x64_cpuid(u32 leaf, u32 sub, u32 *out)
{
#if defined(_MSC_VER)
	int d[4];
	__cpuidex(d, (int)leaf, (int)sub);
	out[0] = (u32)d[0]; out[1] = (u32)d[1]; out[2] = (u32)d[2]; out[3] = (u32)d[3];
#else
	__cpuid_count(leaf, sub, out[0], out[1], out[2], out[3]);
#endif
}

static int x64_cpu_features(void)
{
	if (rec_x64_cpu < 0)
	{
		u32 id[4];
		int f = 0;
		x64_cpuid(1, 0, id);
		if (id[2] & (1u << 27))		/* OSXSAVE */
		{
			u64 bv;
#if defined(_MSC_VER)
			bv = _xgetbv(0);
#else
			u32 lo, hi;
			__asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0));
			bv = ((u64)hi << 32) | lo;
#endif
			if ((bv & 6) == 6)
			{
				if (id[2] & (1u << 28))
					f |= X64_CPU_AVX;
				if (id[2] & (1u << 12))
					f |= X64_CPU_FMA;
				if (((bv >> 5) & 7) == 7)
				{
					x64_cpuid(7, 0, id);
					if (id[1] & (1u << 16))
						f |= X64_CPU_AVX512F;
				}
			}
		}
		rec_x64_cpu = f;
	}
	return rec_x64_cpu;
}

/* A label that many jumps go to (a block's check of its code: one a few
 * bytes of it) */
#define FAR_LABEL_JUMPS 1024
typedef struct
{
	u8 *at;
	u8 *patch[FAR_LABEL_JUMPS];
	int patches;
} x64_far_label;

/* The block being compiled */
typedef struct
{
	x86_emit e;
	RuntimeBlockInfo *block;
	sh4_regalloc ra;
	size_t current_opid;
	x86_label exit_block;
	x86_label slice_out;
	x64_far_label check_failed;
	x64_far_label vaddr_failed;
	bool vaddr_checked;
	bool slice_out_used;
	u32 block_cycles;
	bool charge_at_tail;
	/* the canonical call's parameters (ngen_CC_Param()) */
	struct
	{
		CanonicalParamType type;
		const shil_param *prm;
	} cc_pars[8];
	int cc_count;
} x64_compiler;

static x64_compiler *comp;

static void far_jcc(x86_emit *e, int cc, x64_far_label *l)
{
	/* (near, ahead: as Xbyak had it) */
	if (cc < 0)
		x86_db(e, 0xE9);
	else
	{
		x86_db(e, 0x0F);
		x86_db(e, 0x80 | (u32)cc);
	}
	if (l->patches == FAR_LABEL_JUMPS)
		e->full = true;
	else
		l->patch[l->patches++] = e->p;
	x86_dd(e, 0);
}

static void far_bind(x86_emit *e, x64_far_label *l)
{
	int i;
	l->at = e->p;
	if (e->full)
		return;
	for (i = 0; i < l->patches; i++)
	{
		const u32 v = (u32)(s32)(l->at - (l->patch[i] + 4));
		memcpy(l->patch[i], &v, 4);
	}
	l->patches = 0;
}

/* Something in the SH4's context, from r15 */
static x86_mem ctx(const void *p)
{
	const ptrdiff_t at = (const u8*)p - (const u8*)&p_sh4rcb->cntx;
	verify(at >= 0 && at < (ptrdiff_t)sizeof(Sh4Context));
	return x86_m(X86_R15, (s32)at - CTX_BIAS);
}

static int map_reg(x64_compiler *c, const shil_param *p)
{
	const int host = (int)(s8)sh4_regalloc_mapg(&c->ra, p);
	if (host == -1)
		die("Register not allocated");
	return host;
}

static int map_xreg(x64_compiler *c, const shil_param *p, int index)
{
	const int host = (int)(s8)sh4_regalloc_mapf(&c->ra, p, index);
	if (host == -1)
		die("VRegister not allocated");
	return host;
}

#define IS_ALLOCG(c, p) sh4_regalloc_is_allocg(&(c)->ra, (p))
#define IS_ALLOCF(c, p) sh4_regalloc_is_allocf(&(c)->ra, (p))
#define MAPG(c, p) map_reg(c, p)
#define MAPF(c, p) map_xreg(c, p, 0)
#define MAPF_I(c, p, i) map_xreg(c, p, i)
#define SAME_G(c, a, b) (sh4_regalloc_mapg(&(c)->ra, (a)) == sh4_regalloc_mapg(&(c)->ra, (b)))
#define SAME_F(c, a, b) (sh4_regalloc_mapf(&(c)->ra, (a), 0) == sh4_regalloc_mapf(&(c)->ra, (b), 0))

/* A call to @function. The floating-point registers handed out are xmm8 to
 * xmm15 on Linux and macOS, and none of them is kept by a function there:
 * the ones in use at this point are saved round the call. That goes for
 * the memory accesses made with the MMU on too: the registers are written
 * back before them but stay in use after, and what such an access reaches
 * (a register's handler, the tile accelerator, the sound chip's channels)
 * can use any of the eight. */
static void gen_call_skip(x64_compiler *c, uintptr_t function, bool skip_floats)
{
	x86_emit *e = &c->e;
#ifndef _WIN32
	int saved[8];
	int saved_count = 0, i;
	u32 stack_size = 0;
	if (!skip_floats && c->current_opid != (size_t)-1)
		for (i = 8; i < 16; i++)
			if (sh4_regalloc_reg_used(&c->ra, (u32)i, true))
				saved[saved_count++] = i;
	if (saved_count != 0)
	{
		/* (the stack a multiple of 16 for the call) */
		stack_size = (((4 * saved_count + 15) >> 4) << 4);
		x86_alu_ri64(e, X86_SUB, X86_ESP, stack_size);
		for (i = 0; i < saved_count; i++)
			x64_movd_mx(e, x86_m(X86_ESP, i * 4), saved[i]);
	}
#else
	(void)skip_floats;
#endif

	x86_call(e, (uintptr_t)CC_RX2RW((void *)function));

#ifndef _WIN32
	if (saved_count != 0)
	{
		for (i = 0; i < saved_count; i++)
			x64_movd_xm(e, saved[i], x86_m(X86_ESP, i * 4));
		x86_alu_ri64(e, X86_ADD, X86_ESP, stack_size);
	}
#endif
}

#define gen_call(c, f) gen_call_skip(c, (uintptr_t)(f), false)

/* A parameter into a general register (@xmm false) or an xmm one; uses
 * eax for a constant into a floating-point register */
static void param_to_reg(x64_compiler *c, const shil_param *param, int reg, bool xmm)
{
	x86_emit *e = &c->e;
	if (shil_is_imm(param))
	{
		if (!xmm)
			x86_mov_ri(e, reg, param->_imm);
		else
		{
			x86_mov_ri(e, X86_EAX, param->_imm);
			x64_movd_xr(e, reg, X86_EAX);
		}
	}
	else if (shil_is_reg(param))
	{
		if (shil_is_r32f(param))
		{
			if (IS_ALLOCF(c, param))
			{
				const int sreg = MAPF(c, param);
				if (!xmm)
					x64_movd_rx(e, reg, sreg);
				else if (reg != sreg)
					x64_movss_rr(e, reg, sreg);
			}
			else
			{
				verify(!xmm);
				x86_load(e, reg, ctx(shil_reg_ptr(param)));
			}
		}
		else
		{
			if (IS_ALLOCG(c, param))
			{
				const int sreg = MAPG(c, param);
				if (xmm)
					x64_movd_xr(e, reg, sreg);
				else if (reg != sreg)
					x86_mov_rr(e, reg, sreg);
			}
			else
			{
				if (!xmm)
					x86_load(e, reg, ctx(shil_reg_ptr(param)));
				else
					x64_movss_load(e, reg, ctx(shil_reg_ptr(param)));
			}
		}
	}
	else
	{
		verify(shil_is_null(param));
	}
}

/* A host register into a parameter */
static void reg_to_param(x64_compiler *c, const shil_param *param, int reg, bool xmm)
{
	x86_emit *e = &c->e;
	if (IS_ALLOCG(c, param))
	{
		const int sreg = MAPG(c, param);
		if (!xmm)
			x86_mov_rr(e, sreg, reg);
		else if (reg != sreg)
			x64_movd_rx(e, sreg, reg);
	}
	else if (IS_ALLOCF(c, param))
	{
		const int sreg = MAPF(c, param);
		if (!xmm)
			x64_movd_xr(e, sreg, reg);
		else if (reg != sreg)
			x64_movss_rr(e, sreg, reg);
	}
	else
	{
		/* (straight there: by way of rax, it was lost if it was in rax) */
		if (!xmm)
			x86_store(e, ctx(shil_reg_ptr(param)), reg);
		else
			x64_movss_store(e, ctx(shil_reg_ptr(param)), reg);
	}
}

/* A pair of the SH4's floating-point registers (DRn, XDn) the block has
 * in two xmm registers, to and from 64 bits in a general register: the
 * first of the pair is the low half, as in memory. xmm0 is scratch. */
static void pair_to_reg64(x64_compiler *c, const shil_param *param, int reg)
{
	x64_movaps_rr(&c->e, 0, MAPF_I(c, param, 0));
	x64_unpcklps(&c->e, 0, MAPF_I(c, param, 1));
	x64_movq_rx(&c->e, reg, 0);
}

/* (what is above the low 32 bits of a register the block has is not looked at) */
static void reg64_to_pair(x64_compiler *c, const shil_param *param, int reg)
{
	const int lo = MAPF_I(c, param, 0);
	x64_movq_xr(&c->e, lo, reg);
	x64_pshufd(&c->e, MAPF_I(c, param, 1), lo, 0x55);
}

/* Takes what the block costs off the cycle counter, which is r14. A block
 * that pays at its start - one compiled with the MMU on, which can leave
 * by an exception - leaves a copy where the main loop finds it afterwards:
 * see cycle_counter. (Neither changes the flags of the subtraction.) */
static void gen_charge(x64_compiler *c)
{
	x86_alu_ri(&c->e, false, X86_SUB, X86_R14, c->block_cycles);
	if (!c->charge_at_tail)
	{
#ifdef FEAT_NO_RWX_PAGES
		/* (absolute addressing for this one) */
		x86_mov_ri64(&c->e, X86_ECX, (uintptr_t)&cycle_counter);
		x86_store(&c->e, x86_m(X86_ECX, 0), X86_R14);
#else
		x86_store(&c->e, x86_rip(&cycle_counter), X86_R14);
#endif
	}
}

/* To the main loop if the time slice is used up, by way of slice_out,
 * which stores the address in edx; otherwise on */
static void gen_slice_check(x64_compiler *c)
{
	if (c->charge_at_tail)
		gen_charge(c);		/* leaves the flags of the counter against 0 */
	else
		x86_test_rr(&c->e, X86_R14, X86_R14);
	x86_jmp_label(&c->e, X86_CC_LE, &c->slice_out, true);
	c->slice_out_used = true;
}

/* What is left of the time slice, where the timers find it, before a call
 * that can reach an on-chip register: see cycle_counter in sh4_if.h. */
static void gen_slice_left(x64_compiler *c)
{
	x86_store(&c->e, ctx(&p_sh4rcb->cntx.cycle_counter), X86_R14);
}

/* A block going back: every so often, see whether it is only waiting, and
 * give up the rest of the time slice if it is. See wait_site.h. */
static void gen_wait_check(x64_compiler *c, const RuntimeBlockInfo *block)
{
	x86_emit *e = &c->e;
	WaitSite *site = sh4_wait_site(block);
	x86_label over;
	if (site == NULL)
		return;
	x86_label_init(&over);

	x86_mov_ri64(e, X86_EAX, (uintptr_t)&site->skip);
	x64_dec_m(e, x86_m(X86_EAX, 0));
	x86_jmp_label(e, X86_CC_NS, &over, true);
	x86_mov_ri64(e, call_regs[0], (uintptr_t)site);
	x86_mov_rr(e, call_regs[1], X86_R14);
	gen_call(c, &sh4_wait_check);
	x86_mov_rr(e, X86_R14, X86_EAX);
	x86_label_bind(e, &over);
}

/* On to the block at @target: by a link site (see RuntimeBlockInfo::link_at),
 * which is the block's way out to its BranchBlock (@which 0) or its
 * NextBlock (1). A block in the temporary cache is not linked: it goes
 * through the target's place in the table. So does a block that goes round
 * to its own start. Linked to itself, such a loop came out faster or
 * slower by the loop - one of shifts and adds a twelfth faster, one of
 * stores to memory a twelfth slower, on the one processor it was tried on
 * - and two games slower rather than faster: left as it was. */
static void gen_go_on(x64_compiler *c, RuntimeBlockInfo *block, int which)
{
	x86_emit *e = &c->e;
	const u32 target = which == 0 ? block->BranchBlock : block->NextBlock;
	/* where the target is, for the table of blocks */
	u32 place = target;

	x86_mov_ri(e, X86_EDX, target);
	if (mmu_enabled())
	{
		/* With the MMU on the context has the address as well: a block
		 * that checks its code goes by it, and so does whatever finds that
		 * there is an exception to raise instead. */
		x86_store(e, ctx(&next_pc), X86_EDX);
		rdv_MmuSamePlace(block, target, &place);
	}
	gen_slice_check(c);
	if (block->temp_block || target == block->vaddr || link_stub == NULL)
	{
		x86_jmp_m(e, x86_m(X86_R15, fpcb_at((place >> 1) & FPCB_MASK)));
		return;
	}
	block->link_at[which] = (u32)(e->p - e->start);
	x86_call(e, (uintptr_t)link_stub);
	x86_db(e, 0xCC);
	x86_db(e, 0xCC);
}

/* With the MMU on an access is a call. First, though, the table of
 * translations kept by (mmu.h): on a hit, rax is what to add to the
 * address to be at the page on the host; the call is for a miss, and
 * fills the table in. The address is in call_regs[0]; rax, r10 and r11
 * belong to nobody here. */
static bool gen_mmu_lookup(x64_compiler *c, const uintptr_t *table, u32 size, x86_label *miss)
{
#ifdef MMU_HOST_PAGE_LUT
	x86_emit *e = &c->e;
	if (!mmu_enabled())
		return false;
	/* Not for an address that is not a multiple of the access's size:
	 * that is an address error, which the call raises - and which a page
	 * already in the table used to get past, reading on into the next page
	 * of the host's memory at the end of one. 64 bits are two accesses of
	 * 32 to the SH4, so a multiple of 4 will do for them - but then the
	 * second half must not be in the next page, which is another page
	 * altogether: the call's again. */
	if (size == 2 || size == 4 || size == 8)
	{
		x86_test_ri(e, call_regs[0], size == 2 ? 1 : 3);
		x86_jmp_label(e, X86_CC_NE, miss, false);
	}
	if (size == 8)
	{
		x64_lea(e, false, X86_EAX, x86_m(call_regs[0], 4));
		x86_test_ri(e, X86_EAX, 0xFFF);
		x86_jmp_label(e, X86_CC_E, miss, false);
	}
	x86_mov_rr(e, X86_EAX, call_regs[0]);
	x86_shift_ri(e, X86_SHR, X86_EAX, 12);
	x86_mov_ri64(e, X86_R10, (uintptr_t)table);
	x64_load64(e, X86_EAX, x86_mi(X86_R10, X86_EAX, 8, 0));
	x64_test_rr64(e, X86_EAX, X86_EAX);
	x86_jmp_label(e, X86_CC_E, miss, false);
	return true;
#else
	(void)c;
	(void)table;
	(void)size;
	(void)miss;
	return false;
#endif
}

static u32 op_pc(const RuntimeBlockInfo *block, const shil_opcode *op)
{
	return block->vaddr + op->guest_offs - (op->delay_slot ? 1 : 0);
}

static void gen_read_memory_slow(x64_compiler *c, const shil_opcode *op, RuntimeBlockInfo *block)
{
	x86_emit *e = &c->e;
	const u32 size = op->flags & 0x7f;
	x86_label lut_miss, lut_done;
	bool lut;
	x86_label_init(&lut_miss);
	x86_label_init(&lut_done);
#ifdef MMU_HOST_PAGE_LUT
	lut = gen_mmu_lookup(c, mmu_read_lut, size, &lut_miss);
#else
	lut = false;
#endif
	if (lut)
	{
		const x86_mem at = x86_mi(X86_EAX, call_regs[0], 1, 0);
		switch (size)
		{
		case 1:
			x64_movsx8_m(e, X86_EAX, at);
			break;
		case 2:
			x86_movsx16(e, X86_EAX, at);
			break;
		case 8:
			x64_load64(e, X86_EAX, at);
			break;
		default:
			x86_load(e, X86_EAX, at);
			break;
		}
		/* (past the call, and the registers saved round it) */
		x86_jmp_label(e, -1, &lut_done, true);
		x86_label_bind(e, &lut_miss);
	}
	gen_slice_left(c);
	if (mmu_enabled())
		x86_mov_ri(e, call_regs[1], op_pc(block, op));	/* pc */

	switch (size)
	{
	case 1:
		if (!mmu_enabled())
			gen_call(c, ReadMem8);
		else
			gen_call(c, &rec_x64_read_noex8);
		x64_movsx8_rr(e, X86_EAX, X86_EAX);
		break;
	case 2:
		if (!mmu_enabled())
			gen_call(c, ReadMem16);
		else
			gen_call(c, &rec_x64_read_noex16);
		x64_movsx16_rr(e, X86_EAX, X86_EAX);
		break;
	case 4:
		if (!mmu_enabled())
			gen_call(c, ReadMem32);
		else
			gen_call(c, &rec_x64_read_noex32);
		break;
	case 8:
		if (!mmu_enabled())
			gen_call(c, ReadMem64);
		else
			gen_call(c, &rec_x64_read_noex64);
		break;
	default:
		die("1..8 bytes");
	}
	if (lut)
		x86_label_bind(e, &lut_done);
}

static void gen_write_memory_slow(x64_compiler *c, const shil_opcode *op, RuntimeBlockInfo *block)
{
	x86_emit *e = &c->e;
	const u32 size = op->flags & 0x7f;
	x86_label lut_miss, lut_done;
	bool lut;
	x86_label_init(&lut_miss);
	x86_label_init(&lut_done);
#ifdef MMU_HOST_PAGE_LUT
	lut = gen_mmu_lookup(c, mmu_write_lut, size, &lut_miss);
#else
	lut = false;
#endif
	if (lut)
	{
		/* the data is in call_regs[1] */
		const x86_mem at = x86_mi(X86_EAX, call_regs[0], 1, 0);
		switch (size)
		{
		case 1:
			x86_store8(e, at, call_regs[1]);
			break;
		case 2:
			x64_store16(e, at, call_regs[1]);
			break;
		case 8:
			x64_store64(e, at, call_regs[1]);
			break;
		default:
			x86_store(e, at, call_regs[1]);
			break;
		}
		/* (past the call, and the registers saved round it) */
		x86_jmp_label(e, -1, &lut_done, true);
		x86_label_bind(e, &lut_miss);
	}
	gen_slice_left(c);
	if (mmu_enabled())
		x86_mov_ri(e, call_regs[2], op_pc(block, op));	/* pc */

	switch (size)
	{
	case 1:
		if (!mmu_enabled())
			gen_call(c, WriteMem8);
		else
			gen_call(c, &rec_x64_write_noex8);
		break;
	case 2:
		if (!mmu_enabled())
			gen_call(c, WriteMem16);
		else
			gen_call(c, &rec_x64_write_noex16);
		break;
	case 4:
		if (!mmu_enabled())
			gen_call(c, WriteMem32);
		else
			gen_call(c, &rec_x64_write_noex32);
		break;
	case 8:
		if (!mmu_enabled())
			gen_call(c, WriteMem64);
		else
			gen_call(c, &rec_x64_write_noex64);
		break;
	default:
		die("1..8 bytes");
	}
	if (lut)
		x86_label_bind(e, &lut_done);
}

/* Whether a load or store can be done as one move from or to the guest's
 * memory as the host has it mapped, in place of a call: with the MMU off -
 * every game but the Windows CE ones - and where the host has room to map
 * the whole 4 GB the SH4 can address. (With the MMU on it is the table of
 * pages: gen_mmu_lookup().)
 *
 * What is not memory - registers, the BIOS, the store queues - is not
 * mapped. A move there faults, and ngen_Rewrite() turns it into the call
 * it would have been, once. A store to a page that is watched, because
 * code was compiled from it or a texture read from it, faults too, and is
 * let through by the handlers that watch. */
static bool fast_memory(void)
{
	return !mmu_enabled() && _nvmem_4gb_space();
}

/* Where the SH4's memory as the host has it mapped is from r15. The
 * mapping comes straight after the block that holds the context, which is
 * what r15 points into, so a fast access is one instruction:
 * [r15 + address + this]. */
static int mem_from_ctx(void)
{
	return (int)((ptrdiff_t)sizeof(Sh4RCB) - (ptrdiff_t)CTX_BASE);
}

/* A place in memory that is known when the block is compiled, as an
 * instruction is to have it: from r15 where it is in the SH4's memory as
 * the host has it laid out and near enough - everything is there again in
 * the first 512 MB of it - and by way of rax, with its 64-bit address,
 * where not.
 *
 * That first 512 MB is P0, which with the MMU on and all 4 GB laid out is
 * not where pages are protected (bm_ProtectPage()): then it is the address
 * as it comes. */
static x86_mem const_mem(x64_compiler *c, const void *ptr)
{
	if (_nvmem_enabled() && (!mmu_enabled() || !_nvmem_4gb_space()))
	{
		u64 offset = (u64)((const u8 *)ptr - virt_ram_base);

		if (_nvmem_4gb_space() && offset < 0xE0000000u)
			offset &= 0x1FFFFFFF;
		if (offset < 0x40000000u)
			return x86_m(X86_R15, mem_from_ctx() + (int)offset);
	}
	x86_mov_ri64(&c->e, X86_EAX, (uintptr_t)ptr);
	return x86_m(X86_EAX, 0);
}

static x86_mem mem_plus(x86_mem m, s32 d)
{
	m.disp += d;
	return m;
}

/* Whether what is at @code is a fast access as gen_fast_access() writes
 * them, and if so which kind, with which registers (as mem_stubs numbers
 * them) and how long it is. A fast access is told by what it is, so
 * nothing has to be kept about where in a block they are: one of ten
 * moves, to or from [r15 + a register + mem_from_ctx()]. Nothing else a
 * block has looks like that. */
static bool fast_access_at(const u8 *code, int *kind, int *addr, int *data, int *length)
{
	const u8 *p = code;
	u8 prefix = 0, rex, modrm, sib;
	bool wide, load;
	int index, reg;

	if (*p == 0x66 || *p == 0xF3)
		prefix = *p++;
	/* REX, with B for r15 */
	rex = *p++;
	if ((rex & 0xF1) != 0x41)
		return false;
	wide = (rex & 8) != 0;
	if (p[0] == 0x0F && !wide)
	{
		if (p[1] == 0xBE && prefix == 0)
			*kind = MEM_LOAD8;
		else if (p[1] == 0xBF && prefix == 0)
			*kind = MEM_LOAD16;
		else if (p[1] == 0x10 && prefix == 0xF3)
			*kind = MEM_LOAD32F;
		else if (p[1] == 0x11 && prefix == 0xF3)
			*kind = MEM_STORE32F;
		else
			return false;
		p += 2;
	}
	else if (*p == 0x8B && prefix == 0)
	{
		*kind = wide ? MEM_LOAD64 : MEM_LOAD32;
		p++;
	}
	else if (*p == 0x88 && prefix == 0 && !wide)
	{
		*kind = MEM_STORE8;
		p++;
	}
	else if (*p == 0x89 && prefix != 0xF3 && !(prefix != 0 && wide))
	{
		*kind = prefix != 0 ? MEM_STORE16 : wide ? MEM_STORE64 : MEM_STORE32;
		p++;
	}
	else
		return false;

	/* [base + index + a distance]: r15, a register times one, and that distance */
	modrm = *p++;
	sib = *p++;
	if ((modrm & 7) != 4 || (sib & 0xC7) != 0x07)
		return false;
	if ((modrm >> 6) == 2)
	{
		s32 distance;
		memcpy(&distance, p, 4);
		if (distance != mem_from_ctx())
			return false;
		p += 4;
	}
	else if ((modrm >> 6) == 1)
	{
		if ((s8)*p++ != mem_from_ctx())
			return false;
	}
	else
		return false;

	index = ((sib >> 3) & 7) | ((rex & 2) != 0 ? 8 : 0);
	reg = ((modrm >> 3) & 7) | ((rex & 4) != 0 ? 8 : 0);
	load = *kind < MEM_STORE8;

	*addr = mem_reg_number(index, MEM_REG_ADDR);
	if (MEM_KIND_F(*kind))
		*data = mem_freg_number(reg);
	else
	{
		*data = mem_reg_number(reg, load ? X86_EAX : MEM_REG_STORE);
		if ((*kind == MEM_LOAD64 || *kind == MEM_STORE64) && *data != 0)
			return false;
	}
	if (*addr < 0 || *data < 0)
		return false;
	*length = (int)(p - code);
	return true;
}

/* A fast access of @kind: the SH4's address is in the general register
 * @addr, and the data in, or wanted in, register @data - a general one, or
 * a floating-point one for the two kinds that are. */
static void gen_fast_access(x64_compiler *c, int kind, int addr, int data)
{
	x86_emit *e = &c->e;
	const u8 *start_addr = e->p;
	const x86_mem at = x86_mi(X86_R15, addr, 1, mem_from_ctx());
	int k, a, d, length;

	switch (kind)
	{
	case MEM_LOAD8:    x64_movsx8_m(e, data, at); break;
	case MEM_LOAD16:   x86_movsx16(e, data, at); break;
	case MEM_LOAD32:   x86_load(e, data, at); break;
	case MEM_LOAD32F:  x64_movss_load(e, data, at); break;
	case MEM_LOAD64:   x64_load64(e, data, at); break;
	case MEM_STORE8:   x86_store8(e, at, data); break;
	case MEM_STORE16:  x64_store16(e, at, data); break;
	case MEM_STORE32:  x86_store(e, at, data); break;
	case MEM_STORE32F: x64_movss_store(e, at, data); break;
	case MEM_STORE64:  x64_store64(e, at, data); break;
	default:           die("no such access");
	}
	/* ngen_Rewrite() has to know it again, and needs five bytes for its call */
	verify(fast_access_at(start_addr, &k, &a, &d, &length) && k == kind
			&& length == (int)(e->p - start_addr) && length >= 5);
	(void)k;
	(void)a;
	(void)d;
	(void)length;
}

/* The address of a load or store, into call_regs[0] */
static void gen_mem_addr(x64_compiler *c, const shil_opcode *op)
{
	x86_emit *e = &c->e;
	if (!shil_is_null(&op->rs3) && shil_is_reg(&op->rs1) && IS_ALLOCG(c, &op->rs1))
	{
		/* one instruction for the two of them, where both are to hand */
		const int base = MAPG(c, &op->rs1);

		if (shil_is_imm(&op->rs3))
		{
			x64_lea(e, false, call_regs[0], x86_m(base, (s32)op->rs3._imm));
			return;
		}
		if (IS_ALLOCG(c, &op->rs3))
		{
			x64_lea(e, false, call_regs[0], x86_mi(base, MAPG(c, &op->rs3), 1, 0));
			return;
		}
	}
	param_to_reg(c, &op->rs1, call_regs[0], false);
	if (!shil_is_null(&op->rs3))
	{
		if (shil_is_imm(&op->rs3))
			x86_alu_ri(e, false, X86_ADD, call_regs[0], op->rs3._imm);
		else if (IS_ALLOCG(c, &op->rs3))
			x86_add_rr(e, call_regs[0], MAPG(c, &op->rs3));
		else
			x86_add_rm(e, call_regs[0], ctx(shil_reg_ptr(&op->rs3)));
	}
}

/* The same for a fast access, which takes it from any register: the one
 * the block has it in, where there is nothing to add to it. (A register of
 * the block's has nothing above its low 32 bits: every instruction that
 * writes one writes 32.) */
static int gen_fast_addr(x64_compiler *c, const shil_opcode *op)
{
	if (shil_is_null(&op->rs3) && shil_is_reg(&op->rs1) && IS_ALLOCG(c, &op->rs1))
		return MAPG(c, &op->rs1);
	gen_mem_addr(c, op);
	return call_regs[0];
}

/* A load as one instruction, from the SH4's memory as the host has it
 * mapped straight into the register the block wants it in. False if that
 * is not to be had (fast_memory()), and nothing written. */
static bool gen_read_memory_fast(x64_compiler *c, const shil_opcode *op)
{
	const u32 size = op->flags & 0x7f;
	int addr, kind;

	if (!fast_memory())
		return false;
	addr = gen_fast_addr(c, op);

	if (size == 8)
	{
		gen_fast_access(c, MEM_LOAD64, addr, X86_EAX);
		if (IS_ALLOCF(c, &op->rd))
			reg64_to_pair(c, &op->rd, X86_EAX);
		else
			x64_store64(&c->e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);
		return true;
	}
	kind = size == 1 ? MEM_LOAD8 : size == 2 ? MEM_LOAD16 : MEM_LOAD32;
	if (IS_ALLOCG(c, &op->rd))
		gen_fast_access(c, kind, addr, MAPG(c, &op->rd));
	else if (size == 4 && IS_ALLOCF(c, &op->rd))
		gen_fast_access(c, MEM_LOAD32F, addr, MAPF(c, &op->rd));
	else
	{
		gen_fast_access(c, kind, addr, X86_EAX);
		reg_to_param(c, &op->rd, X86_EAX, false);
	}
	return true;
}

/* A store, likewise */
static bool gen_write_memory_fast(x64_compiler *c, const shil_opcode *op)
{
	const u32 size = op->flags & 0x7f;
	int addr, kind;

	if (!fast_memory())
		return false;
	addr = gen_fast_addr(c, op);

	if (size == 8)
	{
		if (IS_ALLOCF(c, &op->rs2))
			pair_to_reg64(c, &op->rs2, call_regs[1]);
		else
			x64_load64(&c->e, call_regs[1], ctx(shil_reg_ptr(&op->rs2)));
		gen_fast_access(c, MEM_STORE64, addr, call_regs[1]);
		return true;
	}
	kind = size == 1 ? MEM_STORE8 : size == 2 ? MEM_STORE16 : MEM_STORE32;
	if (shil_is_reg(&op->rs2) && IS_ALLOCG(c, &op->rs2))
		gen_fast_access(c, kind, addr, MAPG(c, &op->rs2));
	else if (size == 4 && shil_is_reg(&op->rs2) && IS_ALLOCF(c, &op->rs2))
		gen_fast_access(c, MEM_STORE32F, addr, MAPF(c, &op->rs2));
	else
	{
		param_to_reg(c, &op->rs2, call_regs[1], false);
		gen_fast_access(c, kind, addr, call_regs[1]);
	}
	return true;
}

/* The translation of an address known when the block is compiled, with
 * the MMU on: false where it is not to be done here */
static bool translate_const(const RuntimeBlockInfo *block, u32 *addr, u32 size, u32 tt)
{
	u32 paddr, rv;
	if (!(mmu_enabled() && mmu_is_translated_c(*addr, size, tt)))
		return true;
	/* When full mmu is on, only consider addresses in the same 4k page */
	if ((*addr >> 12) != (block->vaddr >> 12))
		return false;
	switch (size)
	{
	case 1:
	case 2:
		rv = mmu_data_translation_c(*addr, &paddr, tt, size);
		break;
	case 4:
	case 8:
		rv = mmu_data_translation_c(*addr, &paddr, tt, 4);
		break;
	default:
		die("Invalid immediate size");
		return false;
	}
	if (rv != MMU_ERROR_NONE)
		return false;
	*addr = paddr;
	return true;
}

static bool gen_read_mem_immediate(x64_compiler *c, const shil_opcode *op, RuntimeBlockInfo *block)
{
	x86_emit *e = &c->e;
	u32 size, addr;
	bool isram = false;
	void *ptr;

	if (!shil_is_imm(&op->rs1))
		return false;
	size = op->flags & 0x7f;
	addr = op->rs1._imm;
	if (!translate_const(block, &addr, size, MMU_TT_DREAD))
		return false;
	ptr = _vmem_read_ptr(addr, &isram, size > 4 ? 4 : size);
	if (!isram && size <= 4)
	{
		/* a system bus register that is only storage is read as memory */
		const void *reg = area0_read_const(ptr, addr, size);
		if (reg != NULL)
		{
			ptr = (void *)reg;
			isram = true;
		}
	}

	if (isram)
	{
		/* Immediate pointer to RAM: super-duper fast access */
		const x86_mem at = const_mem(c, ptr);
		switch (size)
		{
		case 1:
			if (IS_ALLOCG(c, &op->rd))
				x64_movsx8_m(e, MAPG(c, &op->rd), at);
			else
			{
				x64_movsx8_m(e, X86_EAX, at);
				x86_store(e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);
			}
			break;

		case 2:
			if (IS_ALLOCG(c, &op->rd))
				x86_movsx16(e, MAPG(c, &op->rd), at);
			else
			{
				x86_movsx16(e, X86_EAX, at);
				x86_store(e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);
			}
			break;

		case 4:
			if (IS_ALLOCG(c, &op->rd))
				x86_load(e, MAPG(c, &op->rd), at);
			else if (IS_ALLOCF(c, &op->rd))
				x64_movd_xm(e, MAPF(c, &op->rd), at);
			else
			{
				x86_load(e, X86_EAX, at);
				x86_store(e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);
			}
			break;

		case 8:
			if (IS_ALLOCF(c, &op->rd))
			{
				x64_movss_load(e, MAPF_I(c, &op->rd, 0), at);
				x64_movss_load(e, MAPF_I(c, &op->rd, 1), mem_plus(at, 4));
			}
			else
			{
				x64_load64(e, X86_ECX, at);
				x64_store64(e, ctx(shil_reg_ptr(&op->rd)), X86_ECX);
			}
			break;

		default:
			die("Invalid immediate size");
			break;
		}
	}
	else
	{
		/* Not RAM: the returned pointer is a memory handler */
		if (sh4_area7(addr))
			gen_slice_left(c);
		if (size == 8)
		{
			/* Need to call the handler twice */
			x86_mov_ri(e, call_regs[0], addr);
			gen_call(c, ptr);
			if (IS_ALLOCF(c, &op->rd))
				x64_movd_xr(e, MAPF_I(c, &op->rd, 0), X86_EAX);
			else
				x86_store(e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);

			x86_mov_ri(e, call_regs[0], addr + 4);
			gen_call(c, ptr);
			if (IS_ALLOCF(c, &op->rd))
				x64_movd_xr(e, MAPF_I(c, &op->rd, 1), X86_EAX);
			else
				x86_store(e, ctx((const u8 *)shil_reg_ptr(&op->rd) + 4), X86_EAX);
		}
		else
		{
			x86_mov_ri(e, call_regs[0], addr);

			switch (size)
			{
			case 1:
				gen_call(c, ptr);
				x64_movsx8_rr(e, X86_EAX, X86_EAX);
				break;

			case 2:
				gen_call(c, ptr);
				x64_movsx16_rr(e, X86_EAX, X86_EAX);
				break;

			case 4:
				gen_call(c, ptr);
				break;

			default:
				die("Invalid immediate size");
				break;
			}
			reg_to_param(c, &op->rd, X86_EAX, false);
		}
	}

	return true;
}

static bool gen_write_mem_immediate(x64_compiler *c, const shil_opcode *op, RuntimeBlockInfo *block)
{
	x86_emit *e = &c->e;
	u32 size, addr;
	bool isram = false;
	void *ptr;

	if (!shil_is_imm(&op->rs1))
		return false;
	size = op->flags & 0x7f;
	addr = op->rs1._imm;
	if (!translate_const(block, &addr, size, MMU_TT_DWRITE))
		return false;
	ptr = _vmem_write_ptr(addr, &isram, size > 4 ? 4 : size);

	if (isram)
	{
		/* Immediate pointer to RAM: super-duper fast access */
		const x86_mem at = const_mem(c, ptr);
		switch (size)
		{
		case 1:
			if (IS_ALLOCG(c, &op->rs2))
				x86_store8(e, at, MAPG(c, &op->rs2));
			else if (shil_is_imm(&op->rs2))
				x64_store8_imm(e, at, (u8)op->rs2._imm);
			else
			{
				x86_load(e, X86_ECX, ctx(shil_reg_ptr(&op->rs2)));
				x86_store8(e, at, X86_ECX);
			}
			break;

		case 2:
			if (IS_ALLOCG(c, &op->rs2))
				x64_store16(e, at, MAPG(c, &op->rs2));
			else if (shil_is_imm(&op->rs2))
				x64_store16_imm(e, at, (u16)op->rs2._imm);
			else
			{
				x86_load(e, X86_ECX, ctx(shil_reg_ptr(&op->rs2)));
				x64_store16(e, at, X86_ECX);
			}
			break;

		case 4:
			if (IS_ALLOCG(c, &op->rs2))
				x86_store(e, at, MAPG(c, &op->rs2));
			else if (IS_ALLOCF(c, &op->rs2))
				x64_movd_mx(e, at, MAPF(c, &op->rs2));
			else if (shil_is_imm(&op->rs2))
				x86_store_imm(e, at, op->rs2._imm);
			else
			{
				x86_load(e, X86_ECX, ctx(shil_reg_ptr(&op->rs2)));
				x86_store(e, at, X86_ECX);
			}
			break;

		case 8:
			if (IS_ALLOCF(c, &op->rs2))
			{
				x64_movss_store(e, at, MAPF_I(c, &op->rs2, 0));
				x64_movss_store(e, mem_plus(at, 4), MAPF_I(c, &op->rs2, 1));
			}
			else
			{
				x64_load64(e, X86_ECX, ctx(shil_reg_ptr(&op->rs2)));
				x64_store64(e, at, X86_ECX);
			}
			break;

		default:
			die("Invalid immediate size");
			break;
		}
	}
	else
	{
		/* Not RAM: the returned pointer is a memory handler */
		if (sh4_area7(addr))
			gen_slice_left(c);
		if (size == 8)
		{
			/* a 32-bit handler: called for each half, as for a read */
			x86_mov_ri(e, call_regs[0], addr);
			if (IS_ALLOCF(c, &op->rs2))
				x64_movd_rx(e, call_regs[1], MAPF_I(c, &op->rs2, 0));
			else
				x86_load(e, call_regs[1], ctx(shil_reg_ptr(&op->rs2)));
			gen_call(c, ptr);

			x86_mov_ri(e, call_regs[0], addr + 4);
			if (IS_ALLOCF(c, &op->rs2))
				x64_movd_rx(e, call_regs[1], MAPF_I(c, &op->rs2, 1));
			else
				x86_load(e, call_regs[1], ctx((const u8 *)shil_reg_ptr(&op->rs2) + 4));
			gen_call(c, ptr);
		}
		else
		{
			x86_mov_ri(e, call_regs[0], addr);
			param_to_reg(c, &op->rs2, call_regs[1], false);

			gen_call(c, ptr);
		}
	}

	return true;
}

static void check_block(x64_compiler *c, RuntimeBlockInfo* block)
{
	x86_emit *e = &c->e;
	s32 sz = (s32)block->sh4_code_size;
	u32 sa = block->addr;

	x86_mov_ri(e, call_regs[0], block->addr);

	/* FIXME This test shouldn't be necessary. However the decoder makes
	 * various assumptions about the current PC value, which are simply not
	 * true in a virtualized memory model. So this can only work if virtual
	 * and phy addresses are the same at compile and run times. */
	if (mmu_enabled())
	{
		x86_mov_ri64(e, X86_EAX, (uintptr_t)&next_pc);
		x86_alu_mi(e, X86_CMP, x86_m(X86_EAX, 0), block->vaddr);
		far_jcc(e, X86_CC_NE, &c->check_failed);
	}

	while (sz > 0)
	{
		void* ptr = (void*)GetMemPtr(sa, sz > 8 ? 8 : sz);
		if (ptr)
		{
			const uintptr_t p = (uintptr_t)ptr;
			x86_mov_ri64(e, X86_EAX, p);

			if (sz >= 8 && !(p & 7))
			{
				u64 v;
				memcpy(&v, ptr, 8);
				x86_mov_ri64(e, X86_EDX, v);
				{
					const x86_mem m = x86_m(X86_EAX, 0);
					x64_op1(e, 0, true, 0x39, -1, X86_EDX, 0, &m, 0, false, false);
				}
				sz -= 8;
				sa += 8;
			}
			else if (sz >= 4 && !(p & 3))
			{
				u32 v;
				memcpy(&v, ptr, 4);
				x86_mov_ri(e, X86_EDX, v);
				x86_op_m(e, false, 0x39, -1, X86_EDX, x86_m(X86_EAX, 0), 0, false);
				sz -= 4;
				sa += 4;
			}
			else
			{
				u16 v;
				memcpy(&v, ptr, 2);
				x86_mov_ri(e, X86_EDX, v);
				x86_db(e, 0x66);
				x86_op_m(e, false, 0x39, -1, X86_EDX, x86_m(X86_EAX, 0), 0, false);
				sz -= 2;
				sa += 2;
			}
			far_jcc(e, X86_CC_NE, &c->check_failed);
		}
	}
}

static void gen_binary_op(x64_compiler *c, const shil_opcode *op, int alu)
{
	x86_emit *e = &c->e;
	const int rd = MAPG(c, &op->rd);
	const shil_param *rs2 = &op->rs2;
	if (!SAME_G(c, &op->rd, &op->rs1))
	{
		if (shil_is_reg(&op->rs2) && SAME_G(c, &op->rd, &op->rs2))
		{
			if (op->op == shop_sub)
			{
				/* This op isn't commutative */
				x86_neg(e, rd);
				x86_alu_rr(e, X86_ADD, rd, MAPG(c, &op->rs1));
				return;
			}
			/* otherwise just swap the operands */
			rs2 = &op->rs1;
		}
		else
			x86_mov_rr(e, rd, MAPG(c, &op->rs1));
	}
	if (shil_is_imm(&op->rs2))
		/* The constant goes in the instruction, not through a register */
		x86_alu_ri(e, false, alu, rd, op->rs2._imm);
	else
		x86_alu_rr(e, alu, rd, MAPG(c, rs2));
}

/* A NaN comes out as the one NaN an ARM host gives, its default NaN,
 * 0x7fc00000 (which the SH4's thread asks for there: sh4_core_regs.c, and
 * the interpreter here: fixNaN() in sh4_core.h). An x86 processor gives a
 * NaN made from an operand's, or 0xffc00000: the same game had other bits
 * in its memory here than there, which netplay between the two, or a state
 * taken from one to the other, soon found. Games make NaNs in the course of
 * things. The branch is rarely taken. */
static void gen_nan_fix(x64_compiler *c, int rd)
{
	x86_emit *e = &c->e;
	x86_label ok;
	x86_label_init(&ok);
	x64_ucomiss(e, rd, rd);
	x86_jmp_label(e, 0xB /* NP */, &ok, false);
	x86_mov_ri(e, X86_EAX, 0x7fc00000);
	x64_movd_xr(e, rd, X86_EAX);
	x86_label_bind(e, &ok);
}

/* The same for FTRV's four results, in xmm0. Uses xmm1 and xmm2. */
static void gen_nan_fix4(x64_compiler *c)
{
	x86_emit *e = &c->e;
	x86_label ok;
	x86_label_init(&ok);
	x64_movaps_rr(e, 1, 0);
	x64_cmpunordps(e, 1, 1);		/* all ones where a NaN is */
	x64_movmskps(e, X86_EAX, 1);
	x86_test_rr(e, X86_EAX, X86_EAX);
	x86_jmp_label(e, X86_CC_E, &ok, false);
	x86_mov_ri(e, X86_EAX, 0x7fc00000);
	x64_movd_xr(e, 2, X86_EAX);
	x64_shufps(e, 2, 2, 0);
	x64_andps(e, 2, 1);
	x64_andnps(e, 1, 0);
	x64_orps(e, 1, 2);
	x64_movaps_rr(e, 0, 1);
	x86_label_bind(e, &ok);
}

static void gen_binary_fop(x64_compiler *c, const shil_opcode *op, u32 natop)
{
	x86_emit *e = &c->e;
	const int rd = MAPF(c, &op->rd);
	const shil_param *rs2 = &op->rs2;
	if (!SAME_F(c, &op->rd, &op->rs1))
	{
		if (shil_is_reg(&op->rs2) && SAME_F(c, &op->rd, &op->rs2))
		{
			if (op->op == shop_fsub || op->op == shop_fdiv)
			{
				/* these ops aren't commutative so we need a scratch reg */
				x64_movss_rr(e, 0, MAPF(c, &op->rs2));
				x64_movss_rr(e, rd, MAPF(c, &op->rs1));
				x64_ss_rr(e, natop, rd, 0);
				return;
			}
			/* otherwise just swap the operands */
			rs2 = &op->rs1;
		}
		else
			x64_movss_rr(e, rd, MAPF(c, &op->rs1));
	}
	if (shil_is_imm(&op->rs2))
	{
		x86_mov_ri(e, X86_EAX, op->rs2._imm);
		x64_movd_xr(e, 0, X86_EAX);
		x64_ss_rr(e, natop, rd, 0);
	}
	else
		x64_ss_rr(e, natop, rd, MAPF(c, rs2));
}

/* the destination given the first source, if it is not in it already */
static void mov_rd_rs1(x64_compiler *c, const shil_opcode *op)
{
	if (!SAME_G(c, &op->rd, &op->rs1))
		x86_mov_rr(&c->e, MAPG(c, &op->rd), MAPG(c, &op->rs1));
}

/* the destination given the first source, a constant or a register */
static void rd_from_rs1(x64_compiler *c, const shil_opcode *op, int rd)
{
	if (shil_is_imm(&op->rs1))
		x86_mov_ri(&c->e, rd, shil_imm_value(&op->rs1));
	else if (!SAME_G(c, &op->rd, &op->rs1))
		x86_mov_rr(&c->e, rd, MAPG(c, &op->rs1));
}

/* A call to the store queue's write */
static void gen_sqw(x64_compiler *c, const shil_opcode *op, RuntimeBlockInfo *block)
{
	if (mmu_enabled())
	{
		x86_mov_ri(&c->e, call_regs[1], op_pc(block, op));	/* pc */
		gen_call(c, &rec_x64_do_sqw_mmu_no_ex);
	}
	else if (CCN_MMUCR.AT == 1)
		gen_call(c, &do_sqw_mmu);
	else
	{
		x86_mov_ri64(&c->e, call_regs[1], (uintptr_t)sq_both);
		gen_call(c, &rec_x64_do_sqw_nommu_local);
	}
}

static void gen_op(x64_compiler *c, shil_opcode *op, RuntimeBlockInfo *block, bool optimise)
{
	x86_emit *e = &c->e;
	switch (op->op)
	{
	case shop_ifb:
		if (mmu_enabled())
		{
			x86_mov_ri64(e, call_regs[1], (uintptr_t)OpDesc[op->rs3._imm]->oph);	/* op handler */
			x86_mov_ri(e, call_regs[2], op_pc(block, op));	/* pc */
		}
		if (op->rs1._imm)
		{
			x86_mov_ri64(e, X86_EAX, (uintptr_t)&next_pc);
			x86_store_imm(e, x86_m(X86_EAX, 0), op->rs2._imm);
		}

		x86_mov_ri(e, call_regs[0], op->rs3._imm);
		gen_slice_left(c);

		if (!mmu_enabled())
			gen_call(c, OpDesc[op->rs3._imm]->oph);
		else
			gen_call(c, &rec_x64_interpreter_fallback);
		break;

	case shop_jcond:
	case shop_jdyn:
		{
			const int rd = MAPG(c, &op->rd);
			/* This shouldn't happen since the block type would have been
			 * changed to static. But it doesn't hurt and is handy when
			 * partially disabling ssa for testing */
			if (shil_is_imm(&op->rs1))
			{
				if (shil_is_imm(&op->rs2))
					x86_mov_ri(e, rd, op->rs1._imm + op->rs2._imm);
				else
				{
					x86_mov_ri(e, rd, op->rs1._imm);
					verify(shil_is_null(&op->rs2));
				}
			}
			else
			{
				const int rs1 = MAPG(c, &op->rs1);
				if (rd != rs1)
					x86_mov_rr(e, rd, rs1);
				if (shil_is_imm(&op->rs2))
					x86_alu_ri(e, false, X86_ADD, rd, op->rs2._imm);
			}
		}
		break;

	case shop_mov32:
		verify(shil_is_reg(&op->rd));
		verify(shil_is_reg(&op->rs1) || shil_is_imm(&op->rs1));
		if (IS_ALLOCF(c, &op->rd))
			param_to_reg(c, &op->rs1, MAPF(c, &op->rd), true);
		else
			param_to_reg(c, &op->rs1, MAPG(c, &op->rd), false);
		break;

	case shop_mov64:
		verify(shil_is_r64f(&op->rd));
		verify(shil_is_r64f(&op->rs1));
		if (IS_ALLOCF(c, &op->rd))
		{
			const int rd0 = MAPF_I(c, &op->rd, 0);
			const int rd1 = MAPF_I(c, &op->rd, 1);
			const int rs0 = MAPF_I(c, &op->rs1, 0);
			const int rs1 = MAPF_I(c, &op->rs1, 1);
			/* The allocator can give the first of the destination the
			 * register the second of the source is in (a source used for
			 * the last time gives its register up): that one is moved out
			 * of the way first. */
			if (rd0 == rs1)
			{
				x64_movss_rr(e, 0, rs1);
				x64_movss_rr(e, rd0, rs0);
				x64_movss_rr(e, rd1, 0);
			}
			else
			{
				if (rd0 != rs0)
					x64_movss_rr(e, rd0, rs0);
				if (rd1 != rs1)
					x64_movss_rr(e, rd1, rs1);
			}
		}
		else
		{
			x64_load64(e, X86_EAX, ctx(shil_reg_ptr(&op->rs1)));
			x64_store64(e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);
		}
		break;

	case shop_readm:
		/* Not an immediate address: one instruction, or the call */
		if (!gen_read_mem_immediate(c, op, block) && (!optimise || !gen_read_memory_fast(c, op)))
		{
			u32 size;
			gen_mem_addr(c, op);
			gen_read_memory_slow(c, op, block);

			size = op->flags & 0x7f;
			if (size != 8)
				reg_to_param(c, &op->rd, X86_EAX, false);
			else if (IS_ALLOCF(c, &op->rd))
				reg64_to_pair(c, &op->rd, X86_EAX);
			else
				x64_store64(e, ctx(shil_reg_ptr(&op->rd)), X86_EAX);
		}
		break;

	case shop_writem:
		if (!gen_write_mem_immediate(c, op, block) && (!optimise || !gen_write_memory_fast(c, op)))
		{
			u32 size;
			gen_mem_addr(c, op);

			size = op->flags & 0x7f;
			if (size != 8)
				param_to_reg(c, &op->rs2, call_regs[1], false);
			else if (IS_ALLOCF(c, &op->rs2))
				pair_to_reg64(c, &op->rs2, call_regs[1]);
			else
				x64_load64(e, call_regs[1], ctx(shil_reg_ptr(&op->rs2)));
			gen_write_memory_slow(c, op, block);
		}
		break;

	case shop_sync_sr:
		gen_call(c, &UpdateSR);
		break;
	case shop_sync_fpscr:
		gen_call(c, &UpdateFPSCR);
		break;

	case shop_swaplb:
		mov_rd_rs1(c, op);
		x64_shift_ri16(e, X86_ROR, MAPG(c, &op->rd), 8);
		break;

	case shop_neg:
		mov_rd_rs1(c, op);
		x86_neg(e, MAPG(c, &op->rd));
		break;
	case shop_not:
		mov_rd_rs1(c, op);
		x86_not(e, MAPG(c, &op->rd));
		break;

	case shop_and:
		gen_binary_op(c, op, X86_AND);
		break;
	case shop_or:
		gen_binary_op(c, op, X86_OR);
		break;
	case shop_xor:
		gen_binary_op(c, op, X86_XOR);
		break;
	case shop_add:
		gen_binary_op(c, op, X86_ADD);
		break;
	case shop_sub:
		gen_binary_op(c, op, X86_SUB);
		break;

	case shop_shl:
	case shop_shr:
	case shop_sar:
	case shop_ror:
		mov_rd_rs1(c, op);
		if (shil_is_imm(&op->rs2))
			x86_shift_ri(e, op->op == shop_shl ? X86_SHL : op->op == shop_shr ? X86_SHR
					: op->op == shop_sar ? X86_SAR : X86_ROR, MAPG(c, &op->rd), op->rs2._imm);
		else
			die("Unsupported operand");
		break;

	case shop_adc:
		{
			int rs2 = 0;
			int rd;
			x86_alu_ri(e, false, X86_CMP, MAPG(c, &op->rs3), 1);	/* C = ~rs3 */
			rd = MAPG(c, &op->rd);
			if (shil_is_reg(&op->rs2))
			{
				rs2 = MAPG(c, &op->rs2);
				if (SAME_G(c, &op->rd, &op->rs2))
				{
					x86_mov_rr(e, X86_ECX, rs2);
					rs2 = X86_ECX;
				}
			}
			rd_from_rs1(c, op, rd);
			x86_cmc(e);		/* C = rs3 */
			if (shil_is_reg(&op->rs2))
				x86_alu_rr(e, X86_ADC, rd, rs2);	/* (C,rd)=rs1+rs2+rs3(C) */
			else
				x86_alu_ri(e, false, X86_ADC, rd, shil_imm_value(&op->rs2));
			x86_setcc(e, X86_CC_B, MAPG(c, &op->rd2));	/* rd2 = C */
		}
		break;

	case shop_negc:
		{
			int rs2 = 0;
			int rd, rd2;
			if (shil_is_reg(&op->rs2))
			{
				rs2 = MAPG(c, &op->rs2);
				if (SAME_G(c, &op->rd, &op->rs2))
				{
					x86_mov_rr(e, X86_ECX, rs2);
					rs2 = X86_ECX;
				}
			}
			rd = MAPG(c, &op->rd);
			rd_from_rs1(c, op, rd);
			x64_f7_w(e, true, 3, rd);	/* neg rd64 */
			if (shil_is_imm(&op->rs2))
				x86_alu_ri(e, true, X86_SUB, rd, shil_imm_value(&op->rs2));
			else
				x64_alu_rr_w(e, true, X86_SUB, rd, rs2);
			rd2 = MAPG(c, &op->rd2);
			x64_mov_rr64(e, rd2, rd);
			x64_shift_ri_w(e, true, X86_SHR, rd2, 63);
			/* Nothing is left above the low 32 bits of the result: this
			 * very instruction, done again on it, would take the borrow
			 * from what was there, and a register of the block's is an
			 * address as it is (gen_fast_addr()). */
			x86_mov_rr(e, rd, rd);
		}
		break;

	case shop_rocr:
	case shop_rocl:
		{
			const int rd = MAPG(c, &op->rd);
			x86_alu_ri(e, false, X86_CMP, MAPG(c, &op->rs2), 1);	/* C = ~rs2 */
			rd_from_rs1(c, op, rd);
			x86_cmc(e);		/* C = rs2 */
			x86_shift_ri(e, op->op == shop_rocr ? X86_RCR : X86_RCL, rd, 1);
			x86_setcc(e, X86_CC_B, X86_EAX);
			x64_movzx8_rr(e, MAPG(c, &op->rd2), X86_EAX);	/* rd2 = C */
		}
		break;

	case shop_shld:
	case shop_shad:
		{
			int rd;
			x86_label negative_shift, non_zero, exit;
			x86_label_init(&negative_shift);
			x86_label_init(&non_zero);
			x86_label_init(&exit);
			if (shil_is_reg(&op->rs2))
				x86_mov_rr(e, X86_ECX, MAPG(c, &op->rs2));
			else
				/* This shouldn't happen. If arg is imm -> shop_shl/shr/sar */
				x86_mov_ri(e, X86_ECX, shil_imm_value(&op->rs2));
			rd = MAPG(c, &op->rd);
			rd_from_rs1(c, op, rd);

			x86_alu_ri(e, false, X86_CMP, X86_ECX, 0);
			x86_jmp_label(e, X86_CC_S, &negative_shift, false);
			x86_shift_cl(e, X86_SHL, rd);
			x86_jmp_label(e, -1, &exit, false);

			x86_label_bind(e, &negative_shift);
			x86_test_ri(e, X86_ECX, 0x1f);
			x86_jmp_label(e, X86_CC_NE, &non_zero, false);
			if (op->op == shop_shld)
				x86_xor_rr(e, rd, rd);
			else
				x86_shift_ri(e, X86_SAR, rd, 31);
			x86_jmp_label(e, -1, &exit, false);

			x86_label_bind(e, &non_zero);
			x86_neg(e, X86_ECX);
			x86_shift_cl(e, op->op == shop_shld ? X86_SHR : X86_SAR, rd);
			x86_label_bind(e, &exit);
		}
		break;

	case shop_test:
	case shop_seteq:
	case shop_setge:
	case shop_setgt:
	case shop_setae:
	case shop_setab:
		if (op->op == shop_test)
		{
			if (shil_is_imm(&op->rs2))
				x86_test_ri(e, MAPG(c, &op->rs1), op->rs2._imm);
			else
				x86_test_rr(e, MAPG(c, &op->rs1), MAPG(c, &op->rs2));
		}
		else
		{
			if (shil_is_imm(&op->rs2))
				x86_alu_ri(e, false, X86_CMP, MAPG(c, &op->rs1), op->rs2._imm);
			else
				x86_alu_rr(e, X86_CMP, MAPG(c, &op->rs1), MAPG(c, &op->rs2));
		}
		x86_setcc(e, op->op == shop_setge ? X86_CC_GE : op->op == shop_setgt ? X86_CC_G
				: op->op == shop_setae ? X86_CC_NB : op->op == shop_setab ? X86_CC_A : X86_CC_E, X86_EAX);
		x64_movzx8_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;

	case shop_setpeq:
		{
			int rd;
			x86_label end;
			x86_label_init(&end);
			x86_mov_rr(e, X86_ECX, MAPG(c, &op->rs1));
			if (shil_is_r32i(&op->rs2))
				x86_xor_rr(e, X86_ECX, MAPG(c, &op->rs2));
			else
				x86_alu_ri(e, false, X86_XOR, X86_ECX, op->rs2._imm);

			rd = MAPG(c, &op->rd);
			x86_mov_ri(e, rd, 1);
			x86_test_ri(e, X86_ECX, 0xFF000000);
			x86_jmp_label(e, X86_CC_E, &end, false);
			x86_test_ri(e, X86_ECX, 0x00FF0000);
			x86_jmp_label(e, X86_CC_E, &end, false);
			x86_test_ri(e, X86_ECX, 0x0000FF00);
			x86_jmp_label(e, X86_CC_E, &end, false);
			x86_xor_rr(e, rd, rd);
			x64_test_rr8(e, X86_ECX, X86_ECX);
			x86_setcc(e, X86_CC_E, rd);
			x86_label_bind(e, &end);
		}
		break;

	case shop_mul_u16:
		x64_movzx16_rr(e, X86_EAX, MAPG(c, &op->rs1));
		if (shil_is_reg(&op->rs2))
			x64_movzx16_rr(e, X86_ECX, MAPG(c, &op->rs2));
		else
			x86_mov_ri(e, X86_ECX, op->rs2._imm & 0xFFFF);
		x64_f7_w(e, false, 4, X86_ECX);
		x86_mov_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;
	case shop_mul_s16:
		x64_movsx16_rr(e, X86_EAX, MAPG(c, &op->rs1));
		if (shil_is_reg(&op->rs2))
			x64_movsx16_rr(e, X86_ECX, MAPG(c, &op->rs2));
		else
			x86_mov_ri(e, X86_ECX, (u32)(s32)(s16)op->rs2._imm);
		x64_f7_w(e, false, 4, X86_ECX);
		x86_mov_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;
	case shop_mul_i32:
		x86_mov_rr(e, X86_EAX, MAPG(c, &op->rs1));
		if (shil_is_reg(&op->rs2))
			x64_f7_w(e, false, 4, MAPG(c, &op->rs2));
		else
		{
			x86_mov_ri(e, X86_ECX, op->rs2._imm);
			x64_f7_w(e, false, 4, X86_ECX);
		}
		x86_mov_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;
	case shop_mul_u64:
		x86_mov_rr(e, X86_EAX, MAPG(c, &op->rs1));
		if (shil_is_reg(&op->rs2))
			x86_mov_rr(e, X86_ECX, MAPG(c, &op->rs2));
		else
			x86_mov_ri(e, X86_ECX, op->rs2._imm);
		x64_f7_w(e, true, 4, X86_ECX);
		x86_mov_rr(e, MAPG(c, &op->rd), X86_EAX);
		x64_shift_ri_w(e, true, X86_SHR, X86_EAX, 32);
		x86_mov_rr(e, MAPG(c, &op->rd2), X86_EAX);
		break;
	case shop_mul_s64:
		x86_movsxd(e, X86_EAX, MAPG(c, &op->rs1));
		if (shil_is_reg(&op->rs2))
			x86_movsxd(e, X86_ECX, MAPG(c, &op->rs2));
		else
			x86_mov_ri64(e, X86_ECX, (u64)(s64)(s32)op->rs2._imm);
		x64_f7_w(e, true, 4, X86_ECX);
		x86_mov_rr(e, MAPG(c, &op->rd), X86_EAX);
		x64_shift_ri_w(e, true, X86_SHR, X86_EAX, 32);
		x86_mov_rr(e, MAPG(c, &op->rd2), X86_EAX);
		break;

	case shop_pref:
		if (shil_is_imm(&op->rs1))
		{
			/* this test shouldn't be necessary */
			if ((op->rs1._imm & 0xFC000000) == 0xE0000000)
			{
				x86_mov_ri(e, call_regs[0], op->rs1._imm);
				gen_sqw(c, op, block);
			}
		}
		else
		{
			int rn;
			x86_label no_sqw;
			x86_label_init(&no_sqw);
			if (IS_ALLOCG(c, &op->rs1))
				rn = MAPG(c, &op->rs1);
			else
			{
				x86_load(e, X86_EAX, ctx(shil_reg_ptr(&op->rs1)));
				rn = X86_EAX;
			}
			x86_mov_rr(e, X86_ECX, rn);
			x86_shift_ri(e, X86_SHR, X86_ECX, 26);
			x86_alu_ri(e, false, X86_CMP, X86_ECX, 0x38);
			/* (the call can be long: it keeps the floating-point registers in use) */
			x86_jmp_label(e, X86_CC_NE, &no_sqw, true);

			x86_mov_rr(e, call_regs[0], rn);
			gen_sqw(c, op, block);
			x86_label_bind(e, &no_sqw);
		}
		break;

	case shop_ext_s8:
		x86_mov_rr(e, X86_EAX, MAPG(c, &op->rs1));
		x64_movsx8_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;
	case shop_ext_s16:
		x64_movsx16_rr(e, MAPG(c, &op->rd), MAPG(c, &op->rs1));
		break;

	case shop_xtrct:
		{
			const int rd = MAPG(c, &op->rd);
			int rs1 = X86_ECX, rs2 = X86_EAX;
			if (shil_is_reg(&op->rs1))
				rs1 = MAPG(c, &op->rs1);
			else
				x86_mov_ri(e, rs1, shil_imm_value(&op->rs1));
			if (shil_is_reg(&op->rs2))
				rs2 = MAPG(c, &op->rs2);
			else
				x86_mov_ri(e, rs2, shil_imm_value(&op->rs2));
			if (rd == rs2)
			{
				x86_shift_ri(e, X86_SHL, rd, 16);
				x86_mov_rr(e, X86_EAX, rs1);
				x86_shift_ri(e, X86_SHR, X86_EAX, 16);
				x86_alu_rr(e, X86_OR, rd, X86_EAX);
				break;
			}
			else if (rd != rs1)
				x86_mov_rr(e, rd, rs1);
			x86_shift_ri(e, X86_SHR, rd, 16);
			x86_mov_rr(e, X86_EAX, rs2);
			x86_shift_ri(e, X86_SHL, X86_EAX, 16);
			x86_alu_rr(e, X86_OR, rd, X86_EAX);
		}
		break;

	/* FPU */

	case shop_fadd:
		gen_binary_fop(c, op, X64_ADDSS);
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;
	case shop_fsub:
		gen_binary_fop(c, op, X64_SUBSS);
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;
	case shop_fmul:
		gen_binary_fop(c, op, X64_MULSS);
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;
	case shop_fdiv:
		gen_binary_fop(c, op, X64_DIVSS);
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;

	case shop_fabs:
		x64_movd_rx(e, X86_EAX, MAPF(c, &op->rs1));
		x86_alu_ri(e, false, X86_AND, X86_EAX, 0x7FFFFFFF);
		x64_movd_xr(e, MAPF(c, &op->rd), X86_EAX);
		break;
	case shop_fneg:
		x64_movd_rx(e, X86_EAX, MAPF(c, &op->rs1));
		x86_alu_ri(e, false, X86_XOR, X86_EAX, 0x80000000);
		x64_movd_xr(e, MAPF(c, &op->rd), X86_EAX);
		break;

	case shop_fsqrt:
		x64_ss_rr(e, X64_SQRTSS, MAPF(c, &op->rd), MAPF(c, &op->rs1));
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;

	case shop_fmac:
		/* One rounding, of the sum, as the interpreter and the other
		 * recompilers have it (the reference is sh4_fmac()). A processor
		 * with no FMA instructions has the reference called. */
		if (!(x64_cpu_features() & X64_CPU_FMA))
		{
			shil_chf[op->op](op);
			break;
		}
		{
			const int rs1 = MAPF(c, &op->rs1);
			int rs2 = MAPF(c, &op->rs2);
			int rs3 = MAPF(c, &op->rs3);
			const int rd = MAPF(c, &op->rd);
			if (rd == rs2)
			{
				x64_movss_rr(e, 1, rs2);
				rs2 = 1;
			}
			if (rd == rs3)
			{
				x64_movss_rr(e, 2, rs3);
				rs3 = 2;
			}
			if (shil_is_imm(&op->rs1))
			{
				x86_mov_ri(e, X86_EAX, op->rs1._imm);
				x64_movd_xr(e, rd, X86_EAX);
			}
			else if (rd != rs1)
				x64_movss_rr(e, rd, rs1);
			x64_vfmadd231ss(e, rd, rs2, rs3);
			gen_nan_fix(c, rd);
		}
		break;

	case shop_fsrra:
		/* RSQRTSS has an |error| <= 1.5*2^-12 where the SH4 FSRRA needs |error| <= 2^-21 */
		x64_ss_rr(e, X64_SQRTSS, 0, MAPF(c, &op->rs1));
		x86_mov_ri(e, X86_EAX, 0x3f800000);	/* 1.0 */
		x64_movd_xr(e, MAPF(c, &op->rd), X86_EAX);
		x64_ss_rr(e, X64_DIVSS, MAPF(c, &op->rd), 0);
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;

	case shop_fsetgt:
	case shop_fseteq:
		x64_ucomiss(e, MAPF(c, &op->rs1), MAPF(c, &op->rs2));
		if (op->op == shop_fsetgt)
			x86_setcc(e, X86_CC_A, X86_EAX);
		else
		{
			/* the unordered case taken into account */
			x64_lahf(e);
			x64_test_ah_imm(e, 0x44);
			x86_setcc(e, 0xB /* NP */, X86_EAX);
		}
		x64_movzx8_rr(e, MAPG(c, &op->rd), X86_EAX);
		break;

	case shop_fsca:
		if (shil_is_imm(&op->rs1))
			x86_mov_ri64(e, X86_EAX, op->rs1._imm & 0xFFFF);
		else
			x64_movx_rr(e, true, 0xB7, X86_EAX, MAPG(c, &op->rs1), false);
		x86_mov_ri64(e, X86_ECX, (uintptr_t)&sin_table);
		if (IS_ALLOCF(c, &op->rd))
		{
			x64_movss_load(e, MAPF_I(c, &op->rd, 0), x86_mi(X86_ECX, X86_EAX, 8, 0));
			x64_movss_load(e, MAPF_I(c, &op->rd, 1), x86_mi(X86_ECX, X86_EAX, 8, 4));
		}
		else
		{
			x64_load64(e, X86_ECX, x86_mi(X86_ECX, X86_EAX, 8, 0));
			x64_store64(e, ctx(shil_reg_ptr(&op->rd)), X86_ECX);
		}
		break;

	case shop_fipr:
		/* In line, in doubles: see x64_vector.h */
		x64_lea(e, true, X86_EAX, ctx(shil_reg_ptr(&op->rs1)));
		x64_lea(e, true, X86_ECX, ctx(shil_reg_ptr(&op->rs2)));
		x64_emit_fipr(e, MAPF(c, &op->rd));
		gen_nan_fix(c, MAPF(c, &op->rd));
		break;

	case shop_ftrv:
		/* As above */
		x64_lea(e, true, X86_EAX, ctx(shil_reg_ptr(&op->rs1)));		/* the vector */
		x64_lea(e, true, X86_ECX, ctx(shil_reg_ptr(&op->rs2)));		/* the matrix */
		x64_emit_ftrv(e);
		gen_nan_fix4(c);
		x64_lea(e, true, X86_EAX, ctx(shil_reg_ptr(&op->rd)));
		x64_movups_store(e, x86_m(X86_EAX, 0), 0);
		break;

	case shop_frswap:
		x64_lea(e, true, X86_EAX, ctx(shil_reg_ptr(&op->rs1)));
		x64_lea(e, true, X86_ECX, ctx(shil_reg_ptr(&op->rd)));
		/* The wide registers' upper parts are cleared after: left dirty,
		 * every SSE instruction after this - all of the floating-point
		 * code, and the C library's - has to merge with them. Nothing keeps
		 * more than the low lanes. */
		if (x64_cpu_features() & X64_CPU_AVX512F)
		{
			x64_vmovaps_zmm(e, false, 0, X86_EAX);
			x64_vmovaps_zmm(e, false, 1, X86_ECX);
			x64_vmovaps_zmm(e, true, 1, X86_EAX);
			x64_vmovaps_zmm(e, true, 0, X86_ECX);
			x64_vzeroupper(e);
		}
		else if (x64_cpu_features() & X64_CPU_AVX)
		{
			x64_vmovaps_ymm(e, false, 0, x86_m(X86_EAX, 0));
			x64_vmovaps_ymm(e, false, 1, x86_m(X86_ECX, 0));
			x64_vmovaps_ymm(e, true, 1, x86_m(X86_EAX, 0));
			x64_vmovaps_ymm(e, true, 0, x86_m(X86_ECX, 0));

			x64_vmovaps_ymm(e, false, 0, x86_m(X86_EAX, 32));
			x64_vmovaps_ymm(e, false, 1, x86_m(X86_ECX, 32));
			x64_vmovaps_ymm(e, true, 1, x86_m(X86_EAX, 32));
			x64_vmovaps_ymm(e, true, 0, x86_m(X86_ECX, 32));
			x64_vzeroupper(e);
		}
		else
		{
			int i;
			for (i = 0; i < 4; i++)
			{
				x64_movaps_load(e, 0, x86_m(X86_EAX, i * 16));
				x64_movaps_load(e, 1, x86_m(X86_ECX, i * 16));
				x64_movaps_store(e, x86_m(X86_EAX, i * 16), 1);
				x64_movaps_store(e, x86_m(X86_ECX, i * 16), 0);
			}
		}
		break;

	case shop_cvt_f2i_t:
		{
			/* Too large for 32 bits, the host answers 0x80000000 whatever
			 * the sign, and so it does for a NaN. That is right for a
			 * negative value and for a NaN; a positive one is 0x7fffffff.
			 * So the value is compared with zero: below it or not
			 * comparable (a NaN) keeps 0x80000000. */
			const int rd = MAPG(c, &op->rd);
			x86_label done;
			x86_label_init(&done);

			x64_cvttss2si(e, X86_EDX, MAPF(c, &op->rs1));
			x86_mov_ri(e, rd, 0x7fffffff);
			x86_alu_ri(e, false, X86_CMP, X86_EDX, 0x7fffff80);	/* 2147483520.0f */
			x86_jmp_label(e, X86_CC_G, &done, false);
			x86_mov_rr(e, rd, X86_EDX);
			x86_alu_ri(e, false, X86_CMP, rd, 0x80000000);	/* indefinite integer */
			x86_jmp_label(e, X86_CC_NE, &done, false);
			x86_xor_rr(e, X86_EAX, X86_EAX);
			x64_pxor(e, 0, 0);
			x64_ucomiss(e, MAPF(c, &op->rs1), 0);
			x86_setcc(e, X86_CC_B, X86_EAX);
			x86_alu_ri(e, false, X86_ADD, X86_EAX, 0x7fffffff);
			x86_mov_rr(e, rd, X86_EAX);
			x86_label_bind(e, &done);
		}
		break;
	case shop_cvt_i2f_n:
	case shop_cvt_i2f_z:
		x64_cvtsi2ss(e, MAPF(c, &op->rd), MAPG(c, &op->rs1));
		break;

	default:
		shil_chf[op->op](op);
		break;
	}
}

static void x64_preload(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	x86_load(&comp->e, (int)(s8)nreg, ctx(GetRegPtr(reg)));
}

static void x64_writeback(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	x86_store(&comp->e, ctx(GetRegPtr(reg)), (int)(s8)nreg);
}

static void x64_preload_fpu(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	x64_movss_load(&comp->e, (int)(s8)nreg, ctx(GetRegPtr(reg)));
}

static void x64_writeback_fpu(sh4_regalloc *ra, u32 reg, u32 nreg)
{
	(void)ra;
	x64_movss_store(&comp->e, ctx(GetRegPtr(reg)), (int)(s8)nreg);
}

static void compile_block(x64_compiler *c, RuntimeBlockInfo* block, bool force_checks, bool optimise)
{
	x86_emit *e = &c->e;
	bool go_on, mmu_go_on, go_on_branch, go_on_next, watch;
	u32 place;

	c->current_opid = (size_t)-1;
	if (force_checks)
		check_block(c, block);
	else if (mmu_enabled())
	{
		/* A block is found by where its code is in memory, and with the
		 * MMU on that can be at more than one address: is this the one it
		 * was compiled for? Everything in it that goes by the pc - where it
		 * branches to, what it leaves in PR - does. A block that checks its
		 * code asks this as well (check_block()); one in a write-protected
		 * page did not, and ran as if it were at its other address. The ARM
		 * recompilers ask every block, as upstream does. The context has the
		 * address whenever the MMU is on: see gen_go_on(). */
		x86_alu_mi(e, X86_CMP, ctx(&next_pc), block->vaddr);
		far_jcc(e, X86_CC_NE, &c->vaddr_failed);
		c->vaddr_checked = true;
		block->linked_entry = (u32)(e->p - e->start);
	}
	/* (No stack to set up: the block runs on the main loop's, which is as
	 * calls want it. See ngen_mainloop.) */

	if (mmu_enabled() && block->has_fpu_op)
	{
		x86_label fpu_enabled;
		x86_label_init(&fpu_enabled);
		x86_mov_ri64(e, X86_EAX, (uintptr_t)&sr);
		x64_test_mi(e, x86_m(X86_EAX, 0), 0x8000);	/* test SR.FD bit */
		x86_jmp_label(e, X86_CC_E, &fpu_enabled, false);
		x86_mov_ri(e, call_regs[0], block->vaddr);	/* pc */
		x86_mov_ri(e, call_regs[1], 0x800);			/* event */
		x86_mov_ri(e, call_regs[2], 0x100);			/* vector */
		gen_call(c, &Do_Exception);
		x86_jmp_label(e, -1, &c->exit_block, true);
		x86_label_bind(e, &fpu_enabled);
	}
	/* What the block costs is taken off the cycle counter where the block
	 * ends, when it can be: the subtraction then also says whether the
	 * time slice is used up, and the counter is touched once a block
	 * instead of twice. With the MMU on a block can be left from the
	 * middle, so it pays as it starts. */
	c->block_cycles = block->guest_cycles;
	c->charge_at_tail = !mmu_enabled();
	if (!c->charge_at_tail)
		gen_charge(c);
	if (settings.dynarec.AccurateTiming && !mmu_enabled())
	{
		/* a block that can change something other than a register says so: see wait_site.h */
		if (sh4_block_writes(block))
		{
#ifdef FEAT_NO_RWX_PAGES
			x86_mov_ri64(e, X86_EAX, (uintptr_t)&sh4_write_gen);
			x64_inc_m(e, x86_m(X86_EAX, 0));
#else
			x64_inc_m(e, x86_rip(&sh4_write_gen));
#endif
		}
	}
	sh4_regalloc_begin(&c->ra, block, alloc_regs, ALLOC_REGS, alloc_fregs, ALLOC_FREGS);

	for (c->current_opid = 0; c->current_opid < block->op_count; c->current_opid++)
	{
		shil_opcode *op = &block->oplist[c->current_opid];
		sh4_regalloc_op_begin(&c->ra, op, (int)c->current_opid);
		gen_op(c, op, block, optimise);
		sh4_regalloc_op_end(&c->ra, op);
	}
	sh4_regalloc_cleanup(&c->ra);
	c->current_opid = (size_t)-1;

	/* Where the block ends by going on to another, it goes there itself:
	 * through the next block's place in the table of blocks, which always
	 * holds that block's code, or the stub that compiles it if there is
	 * none (or no longer). What the main loop did in between still happens
	 * here: the next block is only gone on to while there is time left in
	 * the slice, and otherwise this returns as before.
	 *
	 * With the MMU on an address has to be translated before it means a
	 * place in the table, and the lookup function does that - but for the
	 * addresses that are where they are whatever the TLB says: see
	 * rdv_MmuMayGoOn(). */
	go_on = !mmu_enabled();
	mmu_go_on = !go_on && block->mmu_go_on;
	go_on_branch = go_on || (mmu_go_on && rdv_MmuSamePlace(block, block->BranchBlock, &place));
	go_on_next = go_on || (mmu_go_on && rdv_MmuSamePlace(block, block->NextBlock, &place));
	/* see wait_site.h */
	watch = go_on && settings.dynarec.AccurateTiming && block->BranchBlock <= block->vaddr;

	/* A block that goes on to another does not store the address in the
	 * context first: it takes it along in edx, and it is stored only if
	 * the time slice turns out to be used up (slice_out) or there is no
	 * block there yet (ngen_compile_stub). The context's pc is what the
	 * main loop goes by, and is right whenever it is the main loop's turn. */
	switch (block->BlockType)
	{
	case BET_StaticJump:
	case BET_StaticCall:
		if (go_on_branch)
		{
			if (watch)
				gen_wait_check(c, block);
			gen_go_on(c, block, 0);
		}
		else
			x86_store_imm(e, ctx(&next_pc), block->BranchBlock);
		break;

	case BET_Cond_0:
	case BET_Cond_1:
		{
			x86_label branch_not_taken;
			x86_label_init(&branch_not_taken);
			x86_alu_mi(e, X86_CMP, block->has_jcond ? ctx(&Sh4cntx.jdyn) : ctx(&sr.T), block->BlockType & 1);

			x86_jmp_label(e, X86_CC_NE, &branch_not_taken, true);
			if (go_on_branch)
			{
				if (watch)
					gen_wait_check(c, block);
				gen_go_on(c, block, 0);
			}
			else
			{
				x86_store_imm(e, ctx(&next_pc), block->BranchBlock);
				x86_jmp_label(e, -1, &c->exit_block, true);
			}
			x86_label_bind(e, &branch_not_taken);
			if (go_on_next)
				gen_go_on(c, block, 1);
			else
				x86_store_imm(e, ctx(&next_pc), block->NextBlock);
		}
		break;

	case BET_DynamicJump:
	case BET_DynamicCall:
	case BET_DynamicRet:
		x86_load(e, X86_EDX, ctx(&Sh4cntx.jdyn));
		if (go_on)
		{
			/* the address is only known now: its place in the table is worked out here */
			gen_slice_check(c);
			x86_mov_rr(e, X86_EAX, X86_EDX);
			x86_shift_ri(e, X86_SHR, X86_EAX, 1);
			x86_alu_ri(e, false, X86_AND, X86_EAX, FPCB_MASK);
			x86_jmp_m(e, x86_mi(X86_R15, X86_EAX, 8, fpcb_at(0)));
		}
		else
			x86_store(e, ctx(&next_pc), X86_EDX);
		break;

	case BET_DynamicIntr:
	case BET_StaticIntr:
		if (block->BlockType == BET_DynamicIntr)
		{
			x86_load(e, X86_EDX, ctx(&Sh4cntx.jdyn));
			x86_store(e, ctx(&next_pc), X86_EDX);
		}
		else
			x86_store_imm(e, ctx(&next_pc), block->NextBlock);

		if (c->charge_at_tail)
			gen_charge(c);
		gen_call(c, &UpdateINTC);
		break;

	default:
		die("Invalid block end type");
	}

	/* Back to the main loop. (Far from here, as a rule: by its address.) */
	x86_label_bind(e, &c->exit_block);
	x86_mov_ri64(e, X86_EAX, (uintptr_t)&ngen_block_return);
	x64_jmp_r(e, X86_EAX);

	/* The time slice is used up where the block would have gone on to
	 * another: the address it was going to is in edx, and is where the
	 * main loop picks up from. */
	if (c->slice_out_used)
	{
		x86_label_bind(e, &c->slice_out);
		x86_store(e, ctx(&next_pc), X86_EDX);
		x86_jmp_label(e, -1, &c->exit_block, true);
	}
	/* The block's code is no longer what it was compiled from. */
	if (force_checks)
	{
		far_bind(e, &c->check_failed);
		x86_mov_ri64(e, X86_EAX, (uintptr_t)&ngen_blockcheckfail);
		x64_call_r(e, X86_EAX);
		x86_jmp_label(e, -1, &c->exit_block, true);
	}
	/* Or it is not the block for this address. */
	if (c->vaddr_checked)
	{
		far_bind(e, &c->vaddr_failed);
		x86_mov_ri(e, call_regs[0], block->addr);
		x86_mov_ri64(e, X86_EAX, (uintptr_t)&ngen_blockcheckfail);
		x64_call_r(e, X86_EAX);
		x86_jmp_label(e, -1, &c->exit_block, true);
	}

	if (e->full)
		die("x64 recompiler: block too large");

	block->code = (DynarecCodeEntryPtr)(uintptr_t)e->start;
	block->host_code_size = (u32)(e->p - e->start);

	emit_Skip(block->host_code_size);
}

void ngen_Compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
{
	/* (in room of its own, made anew for each block: nothing allocated) */
	static x64_compiler room;
	(void)reset;
	(void)staging;

	verify(CPU_RUNNING == offsetof(Sh4RCB, cntx.CpuRunning));
	verify(PC == offsetof(Sh4RCB, cntx.pc));
	verify(SCHED_NEXT == offsetof(Sh4RCB, cntx.sh4_sched_next));
	verify(INTERRUPT_PEND == offsetof(Sh4RCB, cntx.interrupt_pend));
	verify(CYCLE_COUNTER == offsetof(Sh4RCB, cntx.cycle_counter));
	verify(CTX_BASE == offsetof(Sh4RCB, cntx) + CTX_BIAS);
	verify(emit_FreeSpace() >= CODE_MARGIN);

	comp = &room;
	x86_init(&room.e, (u8 *)emit_GetCCPtr(), emit_FreeSpace());
	room.block = block;
	sh4_regalloc_init(&room.ra, ALLOC_F64, x64_preload, x64_writeback, x64_preload_fpu, x64_writeback_fpu, &room);
	x86_label_init(&room.exit_block);
	x86_label_init(&room.slice_out);
	room.check_failed.patches = 0;
	room.vaddr_failed.patches = 0;
	room.vaddr_checked = false;
	room.slice_out_used = false;
	room.block_cycles = 0;
	room.charge_at_tail = false;
	room.cc_count = 0;

	compile_block(&room, block, force_checks, optimise);

	comp = NULL;
}

void ngen_CC_Start(shil_opcode* op)
{
	(void)op;
	comp->cc_count = 0;
}

void ngen_CC_Param(shil_opcode* op, shil_param* prm, CanonicalParamType tp)
{
	x64_compiler *c = comp;
	(void)op;
	switch (tp)
	{
	case CPT_u32:
	case CPT_ptr:
	case CPT_f32:
		verify(c->cc_count < 8);
		c->cc_pars[c->cc_count].type = tp;
		c->cc_pars[c->cc_count].prm = prm;
		c->cc_count++;
		break;

	/* store from eax */
	case CPT_u64rvL:
	case CPT_u32rv:
		x64_mov_rr64(&c->e, X86_ECX, X86_EAX);
		reg_to_param(c, prm, X86_ECX, false);
		break;

	case CPT_u64rvH:
		/* (CPT_u64rvL has just been called) */
		x64_shift_ri_w(&c->e, true, X86_SHR, X86_ECX, 32);
		reg_to_param(c, prm, X86_ECX, false);
		break;

	/* store from xmm0 */
	case CPT_f32rv:
		reg_to_param(c, prm, 0, true);
		break;
	}
}

void ngen_CC_Call(shil_opcode* op, void* function)
{
	x64_compiler *c = comp;
	int regused = 0, xmmused = 0, i;

	for (i = c->cc_count; i-- > 0;)
	{
		const shil_param *prm = c->cc_pars[i].prm;
		verify(xmmused < 4 && regused < 4);
		switch (c->cc_pars[i].type)
		{
		/* the contents */
		case CPT_u32:
			param_to_reg(c, prm, call_regs[regused++], false);
			break;

		case CPT_f32:
			param_to_reg(c, prm, xmmused++, true);
			break;

		/* the address */
		case CPT_ptr:
			verify(shil_is_reg(prm));
			x86_mov_ri64(&c->e, call_regs[regused++], (uintptr_t)shil_reg_ptr(prm));
			break;
		default:
			/* (the others are ngen_CC_Param()'s) */
			break;
		}
	}
	gen_call(c, function);
	/* A pair the function was given the address of to write (FSCA's, where
	 * it is a call) is read again into the registers the block has it in. */
	for (i = 0; i < c->cc_count; i++)
	{
		const shil_param *prm = c->cc_pars[i].prm;
		if (c->cc_pars[i].type == CPT_ptr && shil_count(prm) == 2 && IS_ALLOCF(c, prm)
				&& ((shil_is_reg(&op->rd) && op->rd._imm == prm->_imm) || (shil_is_reg(&op->rd2) && op->rd2._imm == prm->_imm)))
		{
			x64_movss_load(&c->e, MAPF_I(c, prm, 0), ctx(shil_reg_ptr(prm)));
			x64_movss_load(&c->e, MAPF_I(c, prm, 1), ctx((const u8 *)shil_reg_ptr(prm) + 4));
		}
	}
}

void ngen_CC_Finish(shil_opcode* op)
{
	(void)op;
}

/* A fast memory access (fast_memory()) faulted: it is written over with a
 * call to the routine that does such an access by asking, with the
 * registers this one has (mem_stubs), and run again from where it was. It
 * is five bytes or more, which is what a call takes, so there is no room
 * kept after it for this; what is left of it is filled in. */
bool ngen_Rewrite(unat *host_pc_at)
{
	static const u8 fill[6][5] = { { 0 }, { 0x90 }, { 0x66, 0x90 }, { 0x0F, 0x1F, 0x00 }, { 0x0F, 0x1F, 0x40, 0x00 },
		{ 0x0F, 0x1F, 0x44, 0x00, 0x00 } };
	unat host_pc = *host_pc_at;
	int kind, addr, data, length;
	u8 *site;
	s32 rel;

	if (!fast_memory())
		return false;
	if (bm_GetBlock2((void *)host_pc) == NULL)
	{
		WARN_LOG(DYNAREC, "ngen_Rewrite: Block at %p not found\n", (void *)host_pc);
		return false;
	}
	if (!fast_access_at((const u8 *)host_pc, &kind, &addr, &data, &length)
			|| mem_stubs[kind][addr][data] == NULL)
	{
		WARN_LOG(DYNAREC, "ngen_Rewrite: no memory access at %p\n", (void *)host_pc);
		return false;
	}

	/* (a block is as far from the stubs where it is run as where it is written) */
	site = (u8 *)CC_RX2RW((u8 *)host_pc);
	rel = (s32)(mem_stubs[kind][addr][data] - (site + 5));

	verify(length >= 5 && length <= 10);
	site[0] = 0xE8;
	memcpy(site + 1, &rel, 4);
	memcpy(site + 5, fill[length - 5], length - 5);
	return true;
}

void ngen_HandleException(void)
{
	longjmp(jmp_env, 1);
}
#else
/* (for the other processors: C has no empty file) */
typedef int rec_x64_not_here;
#endif
