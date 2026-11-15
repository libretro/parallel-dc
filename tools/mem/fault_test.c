/* The host's faults the core takes for its own (core/libretro/common.c), by
 * themselves: a write to a page closed for each of the reasons the core
 * closes them is handed to the one that closed it, and goes on; one that
 * is none of them is handed to whoever had the fault before the core (a
 * second instance of it, under run-ahead), after a second content as after
 * the first; one in recompiled code is rewritten by the recompiler and goes
 * on. On 64-bit Windows, the unwind
 * record registered for the code cache, and taken away again; and a
 * VArray2 zeroed and opened (sh tools/mem/fault_run.sh). */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "types.h"
#include "emulator.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/mem/_vmem.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/rtt_watch.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* (no log) */
retro_log_printf_t log_cb;
void os_DebugBreak(void) { printf("FAIL: os_DebugBreak\n"); exit(1); }

/* Seven pages: one for each of the core's reasons, one for nobody's, one
 * written by recompiled code, and one for the VArray2 */
#define PAGES 7
enum { P_RAM, P_VRAM, P_RTT, P_NVMEM, P_OTHER, P_CODE, P_ARRAY };
static u8 *pages;
static size_t page_size;
static int calls[PAGES];
static int prev_calls;

static void page_open(int i, int open)
{
#ifdef _WIN32
	DWORD old;
	VirtualProtect(pages + i * page_size, page_size, open ? PAGE_READWRITE : PAGE_READONLY, &old);
#else
	mprotect(pages + i * page_size, page_size, open ? PROT_READ | PROT_WRITE : PROT_READ);
#endif
}

static int page_of(const void *p)
{
	size_t off = (size_t)((const u8 *)p - pages);
	if ((const u8 *)p < pages || off >= PAGES * page_size)
		return -1;
	return (int)(off / page_size);
}

static bool take(const void *p, int which)
{
	if (page_of(p) != which)
		return false;
	calls[which]++;
	page_open(which, 1);
	return true;
}

bool bm_RamWriteFault(void *p) { return take(p, P_RAM); }
bool VramLockedWrite(u8 *address) { return take(address, P_VRAM); }
bool rtt_watch_fault(void *address) { return take(address, P_RTT); }
bool BM_LockedWrite(u8 *address) { return take(address, P_NVMEM); }
bool mem_region_unlock(void *start, size_t len)
{
	if (page_of(start) == P_ARRAY && len == page_size)
	{
		calls[P_ARRAY]++;
		page_open(P_ARRAY, 1);
	}
	return true;
}

/* The code cache: one routine, which writes 1 at its first argument (not
 * at its very start, which is not taken for its code) */
#define ROUTINE 16
u8 *CodeCache;
static int rewrites;
bool ngen_Rewrite(unat *host_pc)
{
	/* (where the fault was: the routine's store) */
	CHECK(*host_pc == (unat)(CodeCache + ROUTINE));
	rewrites++;
	page_open(P_CODE, 1);
	return true;
}

static void code_cache(void)
{
#if defined(__x86_64__) || defined(_M_X64)
	static const u8 store[] = {
#ifdef _WIN32
		0xc6, 0x01, 0x01,	/* mov byte [rcx], 1 */
#else
		0xc6, 0x07, 0x01,	/* mov byte [rdi], 1 */
#endif
		0xc3			/* ret */
	};
#ifdef _WIN32
	/* (below the program, as the core has it: the unwind record, in the
	 * program, is found from the code cache 32 bits up) */
	CodeCache = (u8 *)VirtualAlloc((u8 *)GetModuleHandleA(NULL) - 2 * (CODE_SIZE + TEMP_CODE_SIZE),
			CODE_SIZE + TEMP_CODE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
	CodeCache = (u8 *)mmap(NULL, CODE_SIZE + TEMP_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
	memcpy(CodeCache + ROUTINE, store, sizeof(store));
#endif
}

#ifdef _WIN32
/* Who had the faults before (after the core's, which goes first) */
static LONG WINAPI prev_handler(EXCEPTION_POINTERS *ep)
{
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION
			|| page_of((void *)ep->ExceptionRecord->ExceptionInformation[1]) != P_OTHER)
		return EXCEPTION_CONTINUE_SEARCH;
	prev_calls++;
	page_open(P_OTHER, 1);
	return EXCEPTION_CONTINUE_EXECUTION;
}
#else
static void prev_handler(int sn, siginfo_t *si, void *ctx)
{
	(void)ctx;
	if (sn != SIGSEGV || page_of(si->si_addr) != P_OTHER)
	{
		printf("FAIL: a fault nobody takes\n");
		_exit(1);
	}
	prev_calls++;
	page_open(P_OTHER, 1);
}
#endif

int main(void)
{
	static const char *const names[PAGES] = { "main memory", "video memory", "render to texture",
		"the SH4's memory", "nobody's", "recompiled code", "VArray2" };
	VArray2 arr;
	int i;

	setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		page_size = si.dwPageSize;
		pages = (u8 *)VirtualAlloc(NULL, PAGES * page_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	}
	AddVectoredExceptionHandler(0, prev_handler);
#else
	{
		struct sigaction sa;
		page_size = (size_t)sysconf(_SC_PAGESIZE);
		pages = (u8 *)mmap(NULL, PAGES * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		memset(&sa, 0, sizeof(sa));
		sa.sa_flags = SA_SIGINFO;
		sigemptyset(&sa.sa_mask);
		sa.sa_sigaction = prev_handler;
		sigaction(SIGSEGV, &sa, NULL);
	}
#endif
	code_cache();

	common_libretro_setup();
	/* (and again, as for a second content: no second handler, and the
	 * one before still handed what is not the core's) */
	common_libretro_cleanup();
	common_libretro_setup();

	/* each page closed, and written */
	for (i = P_RAM; i <= P_OTHER; i++)
	{
		page_open(i, 0);
		pages[i * page_size + 5] = (u8)(i + 1);
		printf("%s: written %d, taken %d\n", names[i], pages[i * page_size + 5], i == P_OTHER ? prev_calls : calls[i]);
		CHECK(pages[i * page_size + 5] == i + 1);
		CHECK((i == P_OTHER ? prev_calls : calls[i]) == 1);
	}
	CHECK(prev_calls == 1);

	/* a store in the code cache */
	if (CodeCache != NULL)
	{
		union { u8 *p; void (*fn)(u8 *); } code;
		page_open(P_CODE, 0);
		code.p = CodeCache + ROUTINE;
		code.fn(pages + P_CODE * page_size + 9);
		printf("%s: written %d, rewritten %d\n", names[P_CODE], pages[P_CODE * page_size + 9], rewrites);
		CHECK(pages[P_CODE * page_size + 9] == 1);
		CHECK(rewrites == 1);
	}

	/* a VArray2, closed, zeroed */
	arr.data = pages + P_ARRAY * page_size;
	arr.size = (u32)page_size;
	memset(arr.data, 0x5a, page_size);
	page_open(P_ARRAY, 0);
	varray2_zero(&arr);
	printf("%s: opened %d, %d %d\n", names[P_ARRAY], calls[P_ARRAY], arr.data[0], arr.data[page_size - 1]);
	CHECK(calls[P_ARRAY] == 1 && arr.data[0] == 0 && arr.data[page_size - 1] == 0);

#ifdef _WIN64
	/* the code cache's unwind record: there, as the system reads it */
	{
		DWORD64 base = 0;
		PRUNTIME_FUNCTION fn;
		const u8 *info;

		setup_seh();
		setup_seh();
		fn = RtlLookupFunctionEntry((DWORD64)(CodeCache + 0x100), &base, NULL);
		CHECK(fn != NULL);
		if (fn != NULL)
		{
			info = (const u8 *)(size_t)(base + fn->UnwindData);
			printf("unwind: %08lx %08lx base %d: %02x %02x %02x %02x %02x %02x\n",
					(unsigned long)fn->BeginAddress, (unsigned long)fn->EndAddress,
					(int)(base == (DWORD64)CodeCache), info[0], info[1], info[2], info[3], info[4], info[5]);
			CHECK(base == (DWORD64)CodeCache);
			CHECK(fn->BeginAddress == 0 && fn->EndAddress == CODE_SIZE + TEMP_CODE_SIZE);
			/* version 1, no handler; no prologue, one code, no frame
			 * register; at 0, UWOP_ALLOC_SMALL of 0x28 bytes */
			CHECK(info[0] == 0x01 && info[1] == 0 && info[2] == 1 && info[3] == 0);
			CHECK(info[4] == 0 && info[5] == 0x42);
		}
		common_libretro_cleanup();
		CHECK(RtlLookupFunctionEntry((DWORD64)(CodeCache + 0x100), &base, NULL) == NULL);
	}
#else
	common_libretro_cleanup();
#endif

	if (fails)
	{
		printf("fault: %d failed\n", fails);
		return 1;
	}
	printf("fault: all passed\n");
	return 0;
}
