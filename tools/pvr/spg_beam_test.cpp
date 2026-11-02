/* The beam a read of SPG_STATUS sees, as spg_beam() keeps it from one read
 * to the next on the same line, against the beam found afresh. Linked into
 * one program with the core's objects and tools/threads/headless.c, with
 * spg_sync() wrapped (spg_beam_test.sh): each read is made as the core
 * makes it, and made again once spg_beam_forget() has thrown away what it
 * kept. The second finds the beam from nothing; were the first's line
 * short of it, the second would move it on, and SPG_STATUS would read
 * otherwise. At the end: "spg_beam_test: N reads, M lines, K different". */
#include "types.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/spg.h"
#include <stdio.h>
#include <string.h>

extern "C" void __real__Z8spg_syncv(void);
extern "C" void *__real_dlopen(const char *file, int mode);
extern u32 pvr_cur_scanline;

/* headless.c opens the core; "self" is the program, the core linked in */
extern "C" void *__wrap_dlopen(const char *file, int mode)
{
	return __real_dlopen(file != NULL && strcmp(file, "self") == 0 ? NULL : file, mode);
}

static unsigned long long reads, lines, different;
static u32 last_line = ~0u;

extern "C" void __wrap__Z8spg_syncv(void)
{
	u32 status, line;

	__real__Z8spg_syncv();
	status = SPG_STATUS.full;
	line = pvr_cur_scanline;
	reads++;
	if (line != last_line)
		lines++;
	last_line = line;

	spg_beam_forget();
	__real__Z8spg_syncv();
	if (SPG_STATUS.full != status || pvr_cur_scanline != line)
	{
		if (different++ < 8)
			fprintf(stderr, "spg_beam_test: read %llu: line %u status %08x kept, %u %08x afresh\n",
					reads, line, status, pvr_cur_scanline, SPG_STATUS.full);
	}
}

/* (headless.c leaves through _exit(), past the destructors) */
extern "C" void __real__exit(int status);
extern "C" void __wrap__exit(int status)
{
	printf("spg_beam_test: %llu reads, %llu lines, %llu different\n", reads, lines, different);
	fflush(stdout);
	__real__exit(status);
}
