/* The host's faults that are the emulator's own (pages of memory it closed
 * to find out when they are written), and the rest of what the core sets
 * up of the process. */
#if !defined(__MACH__) && !defined(_GNU_SOURCE)
/* (sigaction(), and the registers' names in a ucontext_t) */
#define _GNU_SOURCE
#endif
#include "types.h"
#include "emulator.h"
#include "rend/rtt_watch.h"

#include <errno.h>

#ifdef __MACH__
#define _XOPEN_SOURCE 1
#define __USE_GNU 1
#endif

#if !defined(TARGET_NO_EXCEPTIONS)
#ifndef _WIN32
#ifndef __HAIKU__
#include <ucontext.h>
#endif
#endif
#endif

#if defined(_ANDROID)
#include <asm/sigcontext.h>
#endif

#include <fcntl.h>
#include <semaphore.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/time.h>
#include <unistd.h>
#include "oslib/host_context.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/mem/_vmem.h"
#include "hw/pvr/pvr_mem.h"

#ifdef _WIN32
#include "../imgread/common.h"

#include <windows.h>

#ifndef UNW_FLAG_NHANDLER
#define UNW_FLAG_NHANDLER 0x00
#define UNW_FLAG_UHANDLER 0x02
#endif

static LONG WINAPI ExceptionHandler(EXCEPTION_POINTERS *ExceptionInfo)
{
   EXCEPTION_POINTERS* ep = ExceptionInfo;
   u32 dwCode = ep->ExceptionRecord->ExceptionCode;
   EXCEPTION_RECORD* pExceptionRecord=ep->ExceptionRecord;
   u8* address;

   if (dwCode != EXCEPTION_ACCESS_VIOLATION)
      return EXCEPTION_CONTINUE_SEARCH;

   address=(u8*)pExceptionRecord->ExceptionInformation[1];

#if FEAT_SHREC != DYNAREC_NONE
	if (bm_RamWriteFault(address))
	{
		return EXCEPTION_CONTINUE_EXECUTION;
	}
#endif
	if (rtt_watch_fault(address) || VramLockedWrite(address))
      return EXCEPTION_CONTINUE_EXECUTION;
#if !defined(TARGET_NO_NVMEM) && FEAT_SHREC != DYNAREC_NONE
   if (BM_LockedWrite(address))
      return EXCEPTION_CONTINUE_EXECUTION;
#endif
#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X64
   /* A load or store that recompiled code made straight on the SH4's
    * memory as it is mapped here, at an address that is not memory: it is
    * written over with a call, and run again. */
   {
      const u8 *const pc = (const u8 *)CC_RX2RW(ep->ContextRecord->Rip);

      if (pc > CodeCache && pc < CodeCache + CODE_SIZE + TEMP_CODE_SIZE
            && ngen_Rewrite((unat *)&ep->ContextRecord->Rip))
         return EXCEPTION_CONTINUE_EXECUTION;
   }
#endif
#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86
   /* (the same, for the 32-bit recompiler) */
   {
      const u8 *const pc = (const u8 *)ep->ContextRecord->Eip;

      if (pc > CodeCache && pc < CodeCache + CODE_SIZE + TEMP_CODE_SIZE
            && ngen_Rewrite((unat *)&ep->ContextRecord->Eip))
         return EXCEPTION_CONTINUE_EXECUTION;
   }
#endif
   ERROR_LOG(COMMON, "[GPF]Unhandled access to : %p\n", address);

   return EXCEPTION_CONTINUE_SEARCH;
}

#ifdef _WIN64
/* An x64 unwind record (UNWIND_INFO) as the system reads it: the version
 * in the low 3 bits of the first byte and the flags in the high 5; the
 * frame register in the low 4 bits of the fourth and its offset in the
 * high 4; and each unwind code the offset in the prologue, then the
 * operation in the low 4 bits and its information in the high 4. (Bytes,
 * not bit-fields, whose layout C leaves to the compiler.) */
typedef struct _UNWIND_INFO {
   u8 VersionFlags;
   u8 SizeOfProlog;
   u8 CountOfCodes;
   u8 FrameRegisterOffset;
   u8 UnwindCode[1][2];
} UNWIND_INFO, *PUNWIND_INFO;

#define UNWIND_VERSION_FLAGS(version, flags) ((u8)((version) | (flags) << 3))
#define UNWIND_OP_INFO(op, info) ((u8)((op) | (info) << 4))

static RUNTIME_FUNCTION Table[1];
static UNWIND_INFO unwind_info[1];
static bool seh_installed = false;
static PVOID veh_handle = NULL;

EXCEPTION_DISPOSITION
__gnat_SEH_error_handler(struct _EXCEPTION_RECORD* ExceptionRecord,
      void *EstablisherFrame,
      struct _CONTEXT* ContextRecord,
      void *DispatcherContext)
{
   EXCEPTION_POINTERS ep;
   ep.ContextRecord = ContextRecord;
   ep.ExceptionRecord = ExceptionRecord;

   return (EXCEPTION_DISPOSITION)ExceptionHandler(&ep);
}

PRUNTIME_FUNCTION
seh_callback(
      _In_ DWORD64 ControlPc,
      _In_opt_ PVOID Context
      ) {
   unwind_info[0].VersionFlags = UNWIND_VERSION_FLAGS(1, UNW_FLAG_UHANDLER);
   /* We don't use the unwinding info so fill the structure with 0 values.  */
   unwind_info[0].SizeOfProlog = 0;
   unwind_info[0].CountOfCodes = 0;
   unwind_info[0].FrameRegisterOffset = 0;
   /* Set its scope to the entire program.  */
   Table[0].BeginAddress = 0;
   Table[0].EndAddress = CODE_SIZE + TEMP_CODE_SIZE;
   Table[0].UnwindData = (ULONG)((u8 *)unwind_info - CodeCache);
   DEBUG_LOG(COMMON, "TABLE CALLBACK\n");
   return Table;
}

void setup_seh(void)
{
   /* dc_init() calls this on every content load, and CodeCache is re-prepared
    * each time, so drop any registration from a previous load before adding
    * the current one. Leaving the old one registered would point the OS
    * unwinder at a freed code cache. */
   if (seh_installed)
   {
      RtlDeleteFunctionTable(Table);
      seh_installed = false;
   }
   /* Get the base of the module.  */
   /* Current version is always 1 and we are registering an
      exception handler.  */
   unwind_info[0].VersionFlags = UNWIND_VERSION_FLAGS(1, UNW_FLAG_NHANDLER);
   /* We don't use the unwinding info so fill the structure with 0 values.  */
   unwind_info[0].SizeOfProlog = 0;
   unwind_info[0].CountOfCodes = 1;
   unwind_info[0].FrameRegisterOffset = 0;
   /* UWOP_ALLOC_SMALL of OpInfo * 8 + 8 bytes: 0x28 */
   unwind_info[0].UnwindCode[0][0] = 0;
   unwind_info[0].UnwindCode[0][1] = UNWIND_OP_INFO(2, 0x20 / 8);

   /* Set its scope to the entire program.  */
   Table[0].BeginAddress = 0;
   Table[0].EndAddress = CODE_SIZE + TEMP_CODE_SIZE;
   Table[0].UnwindData = (ULONG)((u8 *)unwind_info - CodeCache);
   /* Register the unwind information.  */
   RtlAddFunctionTable(Table, 1, (DWORD64)CodeCache);
   seh_installed = true;
}
#endif
#endif

#define MCTX(p) (((ucontext_t *)(segfault_ctx))->uc_mcontext p)

/* @rei from @seg, or the other way */
#define bicopy(rei, seg, to_segfault) \
   do { if (to_segfault) (seg) = (rei); else (rei) = (seg); } while (0)

static void context_segfault(host_context_t* reictx, void* segfault_ctx, bool to_segfault)
{
#if !defined(TARGET_NO_EXCEPTIONS)
#if HOST_CPU == CPU_ARM
#ifdef __linux__
   u32* r =(u32*) &MCTX(.arm_r0);
   int i;

   bicopy(reictx->pc, MCTX(.arm_pc), to_segfault);
   for (i = 0; i < 15; i++)
      bicopy(reictx->r[i], r[i], to_segfault);
#elif defined(__MACH__)
   int i;

   bicopy(reictx->pc, MCTX(->__ss.__pc), to_segfault);
   for (i = 0; i < 15; i++)
      bicopy(reictx->r[i], MCTX(->__ss.__r[i]), to_segfault);
#endif
#elif HOST_CPU == CPU_ARM64
#if defined(__MACH__)
	bicopy(reictx->pc, MCTX(->__ss.__pc), to_segfault);
	bicopy(reictx->x2, MCTX(->__ss.__x[2]), to_segfault);
#else
	bicopy(reictx->pc, MCTX(.pc), to_segfault);
	bicopy(reictx->x2, MCTX(.regs[2]), to_segfault);
#endif
#elif HOST_CPU == CPU_X86
#ifdef __linux__
   bicopy(reictx->pc, MCTX(.gregs[REG_EIP]), to_segfault);
   bicopy(reictx->esp, MCTX(.gregs[REG_ESP]), to_segfault);
   bicopy(reictx->eax, MCTX(.gregs[REG_EAX]), to_segfault);
   bicopy(reictx->ecx, MCTX(.gregs[REG_ECX]), to_segfault);
#elif defined(__MACH__)
   bicopy(reictx->pc, MCTX(->__ss.__eip), to_segfault);
   bicopy(reictx->esp, MCTX(->__ss.__esp), to_segfault);
   bicopy(reictx->eax, MCTX(->__ss.__eax), to_segfault);
   bicopy(reictx->ecx, MCTX(->__ss.__ecx), to_segfault);
#endif
#elif HOST_CPU == CPU_X64
#ifdef __linux__
   bicopy(reictx->pc, MCTX(.gregs[REG_RIP]), to_segfault);
#elif defined(__MACH__)
   bicopy(reictx->pc, MCTX(->__ss.__rip), to_segfault);
#endif
#elif HOST_CPU == CPU_MIPS
   bicopy(reictx->pc, MCTX(.pc), to_segfault);
#elif HOST_CPU == CPU_GENERIC
   /* (no registers of the host's are known: no recompiler rewrites code) */
   if (!to_segfault)
      reictx->pc = 0;
#else
#error Unsupported HOST_CPU
#endif
#endif
}

static void context_from_segfault(host_context_t* reictx, void* segfault_ctx)
{
   context_segfault(reictx, segfault_ctx, false);
}

static void context_to_segfault(host_context_t* reictx, void* segfault_ctx)
{
	context_segfault(reictx, segfault_ctx, true);
}

#if !defined(TARGET_NO_EXCEPTIONS)
#ifdef __MACH__
static void sigill_handler(int sn, siginfo_t * si, void *segfault_ctx)
{
   host_context_t ctx;
   size_t pc;
   bool dyna_cde;

   context_from_segfault(&ctx, segfault_ctx);

   pc     = (size_t)ctx.pc;
#if FEAT_SHREC == DYNAREC_JIT
   dyna_cde = (pc > (size_t)CodeCache) && (pc < (size_t)(CodeCache + CODE_SIZE + TEMP_CODE_SIZE));
#else
	dyna_cde = false;
#endif

	ERROR_LOG(COMMON, "SIGILL @ %p ... %p -> was not in vram, %d\n",
         pc, si->si_addr, dyna_cde);

	ERROR_LOG(COMMON, "Entering infiniloop\n");
   for (;;);
}
#endif

#if defined(__MACH__) || defined(__linux__) || defined(__HAIKU__) || \
   defined(__FreeBSD__) || defined(__DragonFly__) || defined(HAVE_LIBNX)
#ifdef HAVE_LIBNX
extern char __start__;
#endif

#if !defined(TARGET_NO_EXCEPTIONS)
static struct sigaction old_sigsegv;
static struct sigaction old_sigbus;
static struct sigaction old_sigill;
#endif

static void signal_handler(int sn, siginfo_t * si, void *segfault_ctx)
{
   host_context_t ctx;
   bool dyna_cde;
#if !defined(HAVE_LIBNX)
   struct sigaction *prev;
#endif

   context_from_segfault(&ctx, segfault_ctx);

#if FEAT_SHREC == DYNAREC_JIT
	dyna_cde = ((uintptr_t)CC_RX2RW(ctx.pc) > (uintptr_t)CodeCache) && ((uintptr_t)CC_RX2RW(ctx.pc) < (uintptr_t)(CodeCache + CODE_SIZE + TEMP_CODE_SIZE));
#else
	dyna_cde = false;
#endif

	DEBUG_LOG(COMMON, "mprot hit @ ptr %p @@ pc: %zx, %d\n", si->si_addr, ctx.pc, dyna_cde);

#if FEAT_SHREC != DYNAREC_NONE
	if (bm_RamWriteFault(si->si_addr))
		return;
#endif
	if (rtt_watch_fault(si->si_addr) || VramLockedWrite((u8*)si->si_addr))
      return;
#if !defined(TARGET_NO_NVMEM) && FEAT_SHREC != DYNAREC_NONE
   if (BM_LockedWrite((u8*)si->si_addr))
      return;
#endif
#if FEAT_SHREC == DYNAREC_JIT
#if HOST_CPU==CPU_ARM
   if (dyna_cde)
   {
      ctx.pc = (u32)ngen_readm_fail_v2((u32*)ctx.pc, ctx.r, (size_t)si->si_addr);

      context_to_segfault(&ctx, segfault_ctx);
   }
#elif HOST_CPU==CPU_X86
	if (dyna_cde && ngen_Rewrite(&ctx.pc))
	{
		context_to_segfault(&ctx, segfault_ctx);
	}
#elif HOST_CPU == CPU_X64
	else if (dyna_cde && ngen_Rewrite(&ctx.pc))
	{
		context_to_segfault(&ctx, segfault_ctx);
	}
#elif HOST_CPU == CPU_ARM64
	else if (dyna_cde && ngen_Rewrite(&ctx.pc))
	{
		context_to_segfault(&ctx, segfault_ctx);
	}
#else
	/* (the generic recompiler's code makes no accesses that fault) */
	if (false)
	{
	}
#endif
#endif
   else
   {
      /* Not one of ours. If another handler was installed before this one --
       * e.g. a second in-process instance of this core, as created by
       * RetroArch's run-ahead second instance, whose handler this one
       * displaced -- forward the signal to it: the fault may be that
       * instance's own intentional lazy-commit or protection fault, and dying
       * here is what crashed run-ahead users (libretro/flycast#451). The
       * Windows path already chains via EXCEPTION_CONTINUE_SEARCH; this is
       * the POSIX equivalent. Only when nobody else can handle the fault is
       * it a genuine crash. */
#if !defined(HAVE_LIBNX)
      prev = (sn == SIGBUS) ? &old_sigbus : &old_sigsegv;
      if (prev->sa_flags & SA_SIGINFO)
      {
         if (prev->sa_sigaction != NULL && prev->sa_sigaction != signal_handler)
         {
            prev->sa_sigaction(sn, si, segfault_ctx);
            return;
         }
      }
      else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN)
      {
         prev->sa_handler(sn);
         return;
      }
#endif
   	ERROR_LOG(COMMON, "SIGSEGV @ %zx ... %p -> was not in vram (dyna code %d)\n", ctx.pc, si->si_addr, dyna_cde);
#ifdef HAVE_LIBNX
    {
       MemoryInfo meminfo;
       u32 pageinfo;
       svcQueryMemory(&meminfo, &pageinfo, (u64)&__start__);
       ERROR_LOG(COMMON, ".text base: %p\n", meminfo.addr);
    }
#endif
   	die("segfault");
   	signal(SIGSEGV, SIG_DFL);
   }
}
#endif

#endif

#ifndef _WIN32

static int exception_handler_install_platform(void)
{
#if defined(HAVE_LIBNX)
   return 0;
#elif !defined(TARGET_NO_EXCEPTIONS)
   struct sigaction new_sa, cur_sa;

   /* Already ours (a second content, the same instance): what was there
    * before stays what a fault that is not ours is handed to. Taken again,
    * it would be this handler itself, and another instance's faults (a
    * second instance's under run-ahead) would be taken for crashes. */
   if (sigaction(SIGSEGV, NULL, &cur_sa) == 0 && (cur_sa.sa_flags & SA_SIGINFO)
         && cur_sa.sa_sigaction == signal_handler)
      return 1;

   new_sa.sa_flags = SA_SIGINFO;
   sigemptyset(&new_sa.sa_mask);
   new_sa.sa_sigaction = signal_handler;

   if (sigaction(SIGSEGV, &new_sa, &old_sigsegv) != 0)
      return 0;

#ifdef __MACH__
   /* this is broken on OSX/iOS/Mach in general */
   sigaction(SIGBUS, &new_sa, &old_sigbus);
   new_sa.sa_sigaction = sigill_handler;
#endif

   if (sigaction(SIGILL, &new_sa, &old_sigill))
         return 0;
#endif

   return 1;
}
#endif

static void print_mem_addr(void)
{
   char line [ 512 ];
   FILE *ifp, *ofp;

   ifp = fopen("/proc/self/maps", "r");

   if (ifp == NULL)
   {
      fprintf(stderr, "Can't open input file /proc/self/maps!\n");
      exit(1);
   }

   ofp = stderr;

   while (fgets(line, sizeof line, ifp) != NULL)
      fprintf(ofp, "%s", line);

   fclose(ifp);
   if (ofp != stderr)
      fclose(ofp);
}


static void enable_runfast(void)
{
#if HOST_CPU==CPU_ARM && !defined(ARMCC)
   static const unsigned int x = 0x04086060;
   static const unsigned int y = 0x03000000;
   int r;
   __asm__ __volatile__ (
         "fmrx	%0, fpscr			\n\t"	/* r0 = FPSCR */
         "and	%0, %0, %1			\n\t"	/* r0 = r0 & 0x04086060 */
         "orr	%0, %0, %2			\n\t"	/* r0 = r0 | 0x03000000 */
         "fmxr	fpscr, %0			\n\t"	/* FPSCR = r0 */
         : "=r"(r)
         : "r"(x), "r"(y)
         );

   DEBUG_LOG(BOOT, "ARM VFP-Run Fast (NFP) enabled !\n");
#endif
}

void varray2_zero(VArray2 *a)
{
#ifndef TARGET_NO_EXCEPTIONS
   mem_region_unlock(a->data, a->size);
#endif
   memset(a->data, 0, a->size);
}

#if defined(_WIN64) && defined(_DEBUG)
/* All of the address space below 4 GB taken, so that a pointer kept in 32
 * bits somewhere shows up at once in a debug build */
static void ReserveBottomMemory(void)
{
    static bool s_initialized = false;
    const size_t LOW_MEM_LINE = 0x100000000LL;
    size_t totalReservation = 0;
    size_t numVAllocs = 0;
    size_t numHeapAllocs = 0;
    size_t oneMB = 1024 * 1024;
    size_t size, blockSize;
    HANDLE heap;
    char buffer[1000];

    if ( s_initialized )
        return;
    s_initialized = true;

    /* Start by reserving large blocks of address space, and then
     * gradually reduce the size in order to capture all of the
     * fragments. Technically we should continue down to 64 KB but
     * stopping at 1 MB is sufficient to keep most allocators out. */
    for (size = 256 * oneMB; size >= oneMB; size /= 2)
    {
        for (;;)
        {
            void* p = VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
            if (!p)
                break;

            if ((size_t)p >= LOW_MEM_LINE)
            {
                /* We don't need this memory, so release it completely. */
                VirtualFree(p, 0, MEM_RELEASE);
                break;
            }

            totalReservation += size;
            ++numVAllocs;
        }
    }

    /* Now repeat the same process but making heap allocations, to use up
     * the already reserved heap blocks that are below the 4 GB line. */
    heap = GetProcessHeap();
    for (blockSize = 64 * 1024; blockSize >= 16; blockSize /= 2)
    {
        for (;;)
        {
            void* p = HeapAlloc(heap, 0, blockSize);
            if (!p)
                break;

            if ((size_t)p >= LOW_MEM_LINE)
            {
                /* We don't need this memory, so release it completely. */
                HeapFree(heap, 0, p);
                break;
            }

            totalReservation += blockSize;
            ++numHeapAllocs;
        }
    }

    /* Perversely enough the CRT doesn't use the process heap. Suck up
     * the memory the CRT heap has already reserved. */
    for (blockSize = 64 * 1024; blockSize >= 16; blockSize /= 2)
    {
        for (;;)
        {
            void* p = malloc(blockSize);
            if (!p)
                break;

            if ((size_t)p >= LOW_MEM_LINE)
            {
                /* We don't need this memory, so release it completely. */
                free(p);
                break;
            }

            totalReservation += blockSize;
            ++numHeapAllocs;
        }
    }

    /* Print diagnostics showing how many allocations we had to make in
     * order to reserve all of low memory, typically less than 200. */
    sprintf_s(buffer, sizeof(buffer), "Reserved %1.3f MB (%d vallocs,"
                      "%d heap allocs) of low-memory.\n",
            totalReservation / (1024 * 1024.0),
            (int)numVAllocs, (int)numHeapAllocs);
    OutputDebugStringA(buffer);
}
#endif

void common_libretro_setup(void)
{
#if defined(_WIN64) && defined(_DEBUG)
   ReserveBottomMemory();
#endif

#if defined(__MACH__) || defined(__linux__)
   enable_runfast();
#endif
#ifdef _WIN32
#ifdef _WIN64
   /* Installed per dc_init(); keep the handle so dc_term() can remove it.
    * Guard against re-adding (which would stack handlers across content
    * reloads, all but one pointing at freed dynarec/vmem state). */
   if (veh_handle == NULL)
      veh_handle = AddVectoredExceptionHandler(1, ExceptionHandler);
#else
   SetUnhandledExceptionFilter(&ExceptionHandler);
#endif
#else
   exception_handler_install_platform();
   signal(SIGINT, exit);
#endif

#if defined(__MACH__) || defined(__linux__)
   DEBUG_LOG(BOOT, "Linux paging: %08lX %08X %08X\n",sysconf(_SC_PAGESIZE),PAGE_SIZE,PAGE_MASK);
   verify((PAGE_SIZE % sysconf(_SC_PAGESIZE)) == 0);
#endif
}

/*
 * Symmetric teardown for common_libretro_setup()/setup_seh(), called from
 * dc_term(). Without this the Win64 vectored exception handler and the dynarec
 * unwind function table stay registered after content close, still pointing at
 * the code cache and vmem that dc_term() then frees. The next stack unwind or
 * access violation walks that freed state inside the OS exception machinery,
 * crashing on close/reload in a way debuggers can't step through.
 */
void common_libretro_cleanup(void)
{
#ifdef _WIN32
#ifdef _WIN64
   if (veh_handle != NULL)
   {
      RemoveVectoredExceptionHandler(veh_handle);
      veh_handle = NULL;
   }
   if (seh_installed)
   {
      RtlDeleteFunctionTable(Table);
      seh_installed = false;
   }
#endif
#endif
}

#if !defined(TARGET_NO_EXCEPTIONS) && defined(HAVE_LIBNX)
__attribute__((aligned(16))) u8 __nx_exception_stack[0x1000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
extern void context_switch_aarch64(void* context);

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
   mcontext_t m_ctx;
   ucontext_t u_ctx;
   siginfo_t sig_info;
   uint64_t handle[64] = { 0 };
   uint64_t *ptr = (uint64_t*)handle;
   int i;

   m_ctx.pc = ctx->pc.x;

   for (i = 0; i < 29; i++)
      m_ctx.regs[i] = ctx->cpu_gprs[i].x;

   u_ctx.uc_mcontext = m_ctx;

   sig_info.si_addr = (void*)ctx->far.x;

   signal_handler(0, &sig_info, (void*) &u_ctx);

   /* x0 to x28 (the non-volatiles from x8), at 0 to 224 */
   for (i = 0; i < 29; i++)
      ptr[i] = m_ctx.regs[i];
   /* Special regs */
   ptr[29] = ctx->fp.x; /* frame pointer 232 */
   ptr[30] = ctx->lr.x; /* link register 240 */
   ptr[31] = ctx->sp.x; /* stack pointer 248 */
   ptr[32] = (uintptr_t)ctx->pc.x; /* PC 256 */

   context_switch_aarch64(ptr);
}
#endif
