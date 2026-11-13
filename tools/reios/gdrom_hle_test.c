/* The HLE BIOS's GD-ROM calls (core/reios/gdrom_hle.c), by themselves: a
 * multi-read PIO transfer into the game's memory that stops at a page not
 * mapped yet puts back what it had taken off the read, so the call made
 * again, once the game has mapped the page, writes the same bytes to the
 * same places from its start; and a save state's part of it read back as
 * written (sh tools/reios/gdrom_hle_run.sh). */
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/mem/_vmem.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/gdrom/gdromv3.h"
#include "hw/holly/holly_intc.h"
#include "reios/gdrom_hle.h"
#include "reios/reios.h"
#undef r

/* What the calls take from the rest of the machine */
static Sh4RCB rcb;
Sh4RCB *p_sh4rcb = &rcb;
RegisterStruct CCN[SH4_CCN_REGS];
bool mmu_strict;
settings_t settings;
unsigned RAM_SIZE = 16 * 1024 * 1024, RAM_MASK = 16 * 1024 * 1024 - 1;
int GDROM_TICK = 1500000;
GD_SecNumbT SecNumber;
cdda_t cdda;
GD_HardwareInfo_t GD_HardwareInfo;
signed int sns_asc, sns_ascq, sns_key;
sh4_catch *sh4_catch_top;
bool sh4_in_delay_slot;

void GenericLog(enum LOG_TYPE_NAME(LOG_LEVELS) level, enum LOG_TYPE_NAME(LOG_TYPE) type,
		const char *file, int line, const char *fmt, ...)
{
	(void)level; (void)type; (void)file; (void)line; (void)fmt;
}

void sh4_throw(u32 epc, u32 expEvn, u32 callVect)
{
	sh4_catch *c = sh4_catch_top;
	if (c == NULL)
	{
		printf("FAIL: an exception with nothing to catch it\n");
		exit(1);
	}
	sh4_catch_top = c->prev;
	c->ex.epc = epc;
	c->ex.expEvn = expEvn;
	c->ex.callVect = callVect;
	SH4_LONGJMP(c->env);
}

u8 *GetMemPtr(u32 addr, u32 size) { (void)addr; (void)size; return NULL; }
u64 sh4_sched_now64(void) { return 0; }
void asic_RaiseInterrupt(HollyInterruptID inter) { (void)inter; }
bool reios_disc_is_gdrom(void) { return true; }
u32 gd_get_subcode(u32 format, u32 fad, u8 *subc_info) { (void)format; (void)fad; memset(subc_info, 0, 100); return 0; }
void libGDR_Prefetch(u32 s, u32 n) { (void)s; (void)n; }
void libGDR_GetToc(u32 *toc, u32 area) { (void)area; memset(toc, 0, 102 * 4); }
u32 libGDR_GetDiscType(void) { return GdRom; }
u32 libGDR_GetTrackNumber(u32 sector, u32 *elapsed) { (void)sector; *elapsed = 0; return 1; }
bool libGDR_GetTrack(u32 t, u32 *s, u32 *e) { (void)t; *s = 0; *e = 0; return true; }
void libGDR_GetTrackAdrAndControl(u32 t, u8 *adr, u8 *ctrl) { (void)t; *adr = 1; *ctrl = 4; }
bool ra_serialize(const void *src, unsigned int size, void **dest, unsigned int *total)
{
	if (*dest) { memcpy(*dest, src, size); *(u8 **)dest += size; }
	*total += size;
	return true;
}
bool ra_unserialize(void *dst, unsigned int size, void **src, unsigned int *total)
{
	memcpy(dst, *src, size); *(u8 **)src += size; *total += size;
	return true;
}

/* Sector n holds bytes (n * 7 + i) */
void libGDR_ReadSector(u8 *buff, u32 sector, u32 count, u32 secsz)
{
	u32 i;
	for (i = 0; i < count * secsz; i++)
		buff[i] = (u8)((sector + i / secsz) * 7 + i % secsz);
}

/* The game's memory: params at 0x100, the buffer from 0x1000, a page not
 * mapped from fault_at (once) */
#define MEM_SIZE 0x4000
static u8 mem[MEM_SIZE];
static u32 fault_at = 0xFFFFFFFF;

static void touch(u32 addr)
{
	if (addr >= fault_at)
	{
		fault_at = 0xFFFFFFFF;
		sh4_throw(0x8c001000, 0x0A0, 0x400);
	}
}
static u32 DYNACALL rd32(u32 addr) { u32 v; memcpy(&v, &mem[addr % MEM_SIZE], 4); return v; }
static void DYNACALL wr8(u32 addr, u8 v) { touch(addr); mem[addr % MEM_SIZE] = v; }
static void DYNACALL wr16(u32 addr, u16 v) { touch(addr); memcpy(&mem[addr % MEM_SIZE], &v, 2); }
static void DYNACALL wr32(u32 addr, u32 v) { touch(addr); memcpy(&mem[addr % MEM_SIZE], &v, 4); }
/* (the transfers to physical addresses: no page to fault) */
void DYNACALL _vmem_WriteMem8(u32 addr, u8 v) { mem[addr % MEM_SIZE] = v; }
void DYNACALL _vmem_WriteMem16(u32 addr, u16 v) { memcpy(&mem[addr % MEM_SIZE], &v, 2); }
void DYNACALL _vmem_WriteMem32(u32 addr, u32 v) { memcpy(&mem[addr % MEM_SIZE], &v, 4); }
ReadMem32Func ReadMem32 = rd32;
WriteMem8Func WriteMem8 = wr8;
WriteMem16Func WriteMem16 = wr16;
WriteMem32Func WriteMem32 = wr32;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* A multi-read of 3 sectors from sector 100, 1000 bytes in, and the PIO
 * transfer of @size bytes to 0x1000 asked for and run (GDROM_MAIN) */
static void start(u32 size)
{
	memset(mem, 0, sizeof(mem));
	memset(&gd_hle_state, 0, sizeof(gd_hle_state));
	gd_hle_state.status = BIOS_DATA_AVAIL;
	gd_hle_state.multi_read_sector = 100;
	gd_hle_state.multi_read_offset = 1000;
	gd_hle_state.multi_read_count = 3 * 2048 - 1000;
	gd_hle_state.multi_read_total = 3 * 2048 - 1000;
	gd_hle_state.command = GDCC_REQ_PIO_TRANS;
	gd_hle_state.params[0] = 0x1000;
	gd_hle_state.params[1] = size;
}

static bool run_main(void)
{
	sh4_catch c;
	Sh4cntx.r[6] = SYSCALL_GDROM;
	Sh4cntx.r[7] = GDROM_MAIN;
	SH4_TRY(c)
	{
		gdrom_hle_op();
		sh4_catch_pop(&c);
		return true;
	}
	return false;
}

int main(void)
{
	u8 want[MEM_SIZE];
	u32 i;

	/* the transfer as it goes when nothing faults */
	start(3000);
	CHECK(run_main());
	memcpy(want, mem, sizeof(want));
	for (i = 0; i < 3000; i++)
	{
		u32 at = 1000 + i;
		CHECK(want[0x1000 + i] == (u8)((100 + at / 2048) * 7 + at % 2048));
		if (fails > 5)
			return 1;
	}
	CHECK(gd_hle_state.multi_read_count == 3 * 2048 - 1000 - 3000);

	/* a page not mapped at 0x1800, past the first sector's end: the call
	 * stops there with what it had taken off the read put back */
	start(3000);
	fault_at = 0x1800;
	CHECK(!run_main());
	CHECK(gd_hle_state.multi_read_sector == 100);
	CHECK(gd_hle_state.multi_read_offset == 1000);
	CHECK(gd_hle_state.multi_read_count == 3 * 2048 - 1000);
	/* and made again, the same bytes in the same places */
	CHECK(run_main());
	CHECK(memcmp(mem, want, sizeof(want)) == 0);
	CHECK(gd_hle_state.multi_read_count == 3 * 2048 - 1000 - 3000);

	/* the save state's part, read back as it was written */
	{
		u8 buf[256];
		void *p = buf;
		unsigned total = 0;
		gdrom_hle_state_t was;
		u32 *w;

		/* every field something (and the padding nothing) */
		memset(&gd_hle_state, 0, sizeof(gd_hle_state));
		gd_hle_state.last_request_id = 11;
		gd_hle_state.next_request_id = 12;
		gd_hle_state.status = BIOS_DATA_AVAIL;
		gd_hle_state.command = 13;
		for (i = 0, w = gd_hle_state.params; i < 4; i++)
			w[i] = 20 + i;
		for (i = 0, w = gd_hle_state.result; i < 4; i++)
			w[i] = 30 + i;
		gd_hle_state.cur_sector = 14;
		gd_hle_state.multi_read_sector = 15;
		gd_hle_state.multi_read_offset = 16;
		gd_hle_state.multi_read_count = 17;
		gd_hle_state.multi_read_total = 18;
		gd_hle_state.multi_callback = 19;
		gd_hle_state.multi_callback_arg = 40;
		gd_hle_state.dma_trans_ended = true;
		gd_hle_state.xfer_end_time = 41;
		gd_hle_state.dma_read_sector = 42;
		gd_hle_state.dma_read_count = 43;
		gd_hle_state.dma_read_addr = 44;
		was = gd_hle_state;
		gd_hle_serialize(&p, &total);
		memset(&gd_hle_state, 0, sizeof(gd_hle_state));
		p = buf;
		total = 0;
		gd_hle_unserialize(&p, &total);
		CHECK(memcmp(&gd_hle_state, &was, sizeof(was)) == 0);
	}

	if (fails)
	{
		printf("gdrom_hle_test: %d failed\n", fails);
		return 1;
	}
	printf("gdrom_hle_test: a transfer stopped at a fault is made again as it was\n");
	return 0;
}
