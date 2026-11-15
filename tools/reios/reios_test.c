/* The HLE BIOS (core/reios/reios.c), by itself: its ROM as set up with and
 * without a font.bin; a Dreamcast's boot from a GD-ROM and a CD-R with the
 * boot file on it, without it and with no disc; the system calls and the
 * NAOMI BIOS's traps each reached through the trap at their address, and
 * one at an address with none; and its part of a save state. What each
 * leaves (registers, memory, what it asked of the rest of the machine) is
 * printed, for reios_expected.txt (sh tools/reios/reios_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <retro_miscellaneous.h>
#include "types.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/mem/_vmem.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_if.h"
#include "hw/flashrom/flashrom.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/gdrom/gdromv3.h"
#include "hw/holly/sb.h"
#include "hw/pvr/pvr_regs.h"
#include "reios/reios.h"
#include "reios/reios_elf.h"
#include <streams/file_stream.h>
#undef r

#define RAM_BYTES (16 * 1024 * 1024)

/* What the BIOS takes from the rest of the machine */
static Sh4RCB rcb;
Sh4RCB *p_sh4rcb = &rcb;
settings_t settings;
unsigned RAM_SIZE = RAM_BYTES, RAM_MASK = RAM_BYTES - 1;
unsigned BIOS_SIZE = 2 * 1024 * 1024;
RegisterStruct CCN[SH4_CCN_REGS];
RegisterStruct BSC[SH4_BSC_REGS];
RegisterStruct DMAC[SH4_DMAC_REGS];
BSC_PDTRA_type BSC_PDTRA;
u32 RealTimeClock = 0x5bfc8900;
RegisterStruct sb_regs[SB_REGS];
u8 pvr_regs[pvr_RegSize];
bool bios_loaded;
int reset_requested;
char game_dir_no_slash[1024];
MemChip sys_nvmem_flash;

static u8 *ram;
static u8 rom_area[2 * 1024 * 1024];

static void logf_(enum retro_log_level level, const char *fmt, ...)
{
	va_list ap;
	printf("log %d: ", (int)level);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
retro_log_printf_t log_cb = logf_;

/* Main memory at area 3, the BIOS at 0; the rest is printed */
static u8 *at(u32 addr)
{
	u32 a = addr & 0x1fffffff;
	if (a >= 0x0c000000 && a < 0x10000000)
		return ram + (a & RAM_MASK);
	if (a < sizeof(rom_area))
		return rom_area + a;
	return NULL;
}

u8 *GetMemPtr(u32 addr, u32 size)
{
	(void)size;
	return at(addr);
}

static u32 rd(u32 addr, int sz)
{
	u8 *p = at(addr);
	u32 v = 0;
	if (p == NULL)
	{
		printf("read%d %08x\n", sz * 8, addr);
		return 0;
	}
	memcpy(&v, p, sz);
	return v;
}
static void wr(u32 addr, u32 v, int sz)
{
	u8 *p = at(addr);
	if (p == NULL)
		printf("write%d %08x %08x\n", sz * 8, addr, v);
	else
		memcpy(p, &v, sz);
}
static u8 DYNACALL rd8(u32 a) { return (u8)rd(a, 1); }
static u16 DYNACALL rd16(u32 a) { return (u16)rd(a, 2); }
static u32 DYNACALL rd32(u32 a) { return rd(a, 4); }
static u64 DYNACALL rd64(u32 a) { return rd(a, 4) | (u64)rd(a + 4, 4) << 32; }
static void DYNACALL wr8(u32 a, u8 v) { wr(a, v, 1); }
static void DYNACALL wr16(u32 a, u16 v) { wr(a, v, 2); }
static void DYNACALL wr32(u32 a, u32 v) { wr(a, v, 4); }
ReadMem8Func ReadMem8 = rd8;
ReadMem16Func ReadMem16 = rd16;
ReadMem32Func ReadMem32 = rd32;
ReadMem64Func ReadMem64 = rd64;
WriteMem8Func WriteMem8 = wr8;
WriteMem16Func WriteMem16 = wr16;
WriteMem32Func WriteMem32 = wr32;
u8 DYNACALL _vmem_ReadMem8(u32 a) { return (u8)rd(a, 1); }
void DYNACALL _vmem_WriteMem32(u32 a, u32 v) { wr(a, v, 4); }

void os_DebugBreak(void) { printf("FAIL: verify\n"); exit(1); }
void UpdateFPSCR(void) { printf("UpdateFPSCR\n"); }
bool UpdateSR(void) { printf("UpdateSR\n"); return false; }
int UpdateINTC(void) { printf("UpdateINTC\n"); return 0; }
void libAICA_Reset(bool hard) { printf("libAICA_Reset %d\n", (int)hard); }
void libAICA_WriteReg(u32 addr, u32 data, u32 sz) { printf("libAICA_WriteReg %08x %08x %u\n", addr, data, sz); }
void pvr_WriteReg(u32 paddr, u32 data) { printf("pvr_WriteReg %08x %08x\n", paddr, data); }
void dc_request_reset(void) { printf("dc_request_reset\n"); }
void gdrom_hle_op(void) { printf("gdrom_hle_op r6 %08x r7 %08x\n", Sh4cntx.r[6], Sh4cntx.r[7]); }
int msgboxf(const char *text, unsigned type, ...)
{
	va_list ap;
	printf("msgboxf %u: ", type);
	va_start(ap, type);
	vprintf(text, ap);
	va_end(ap);
	printf("|\n");
	return 0;
}
bool reios_loadElf(const char *elf, u32 *entry) { printf("reios_loadElf %s\n", elf); *entry = 0x8c010000; return false; }
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

/* Files, by stdio */
RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	(void)mode; (void)hints;
	printf("filestream_open %s\n", strncmp(path, "/nonexistent", 12) == 0 ? path : "(font.bin)");
	return (RFILE *)fopen(path, "rb");
}
int64_t filestream_get_size(RFILE *stream)
{
	FILE *fp = (FILE *)stream;
	long n;
	fseek(fp, 0, SEEK_END);
	n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	return n;
}
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
	return (int64_t)fread(data, 1, (size_t)len, (FILE *)stream);
}
int64_t filestream_write(RFILE *stream, const void *data, int64_t len)
{
	(void)stream; (void)data;
	return len;
}
int filestream_close(RFILE *stream)
{
	return fclose((FILE *)stream);
}

/* The cartridge: 64 KB that count up, with a header that says where its
 * programs load */
static u8 cart[0x10000];
static bool cart_in;
bool naomi_cart_loaded(void) { return cart_in; }
bool naomi_cart_read(u32 offset, u32 size, void *dst)
{
	printf("naomi_cart_read %08x %08x\n", offset, size);
	if (!cart_in || offset > sizeof(cart) || size > sizeof(cart) - offset)
		return false;
	memcpy(dst, cart + offset, size);
	return true;
}

/* The disc: an IP.BIN at its start, a PVD 16 sectors on, a root directory
 * with the boot file in it, and the boot file's sectors counting up */
static u32 disc_type;
static u32 disc_base;      /* where the data track starts */
static bool disc_has_file;
static u8 sector[2048];

u32 libGDR_GetDiscType(void) { return disc_type; }
void libGDR_GetSessionInfo(u8 *pout, u8 session)
{
	memset(pout, 0, 6);
	pout[2] = 2;
	if (session != 0)
	{
		pout[3] = (u8)(disc_base >> 16);
		pout[4] = (u8)(disc_base >> 8);
		pout[5] = (u8)disc_base;
	}
}
static void put733(u8 *p, u32 v)
{
	p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
	p[4] = (u8)(v >> 24); p[5] = (u8)(v >> 16); p[6] = (u8)(v >> 8); p[7] = (u8)v;
}
static void make_sector(u32 fad)
{
	u32 rel = fad - disc_base, i;
	memset(sector, 0, sizeof(sector));
	if (rel == 0)
	{
		memcpy(sector, "SEGA SEGAKATANA SEGA ENTERPRISES", 32);
		memcpy(sector + 0x20, "8B40 GD-ROM1/1  JUE        0799A0   T-12345   V1.000", 0x34);
		memcpy(sector + 0x60, "1ST_READ.BIN    ", 16);
	}
	else if (rel == 16)
	{
		sector[0] = 1;
		memcpy(sector + 1, "CD001", 5);
		sector[6] = 1;
		/* the root directory record: at LBA 200, 4096 bytes */
		put733(sector + 156 + 2, 200);
		put733(sector + 156 + 10, 4096);
	}
	else if (fad == 150 + 200 || fad == 150 + 201)
	{
		/* in the second sector: ".", then the file (or another) */
		if (fad == 150 + 201)
		{
			u8 *d = sector;
			d[0] = 34; d[32] = 1;
			d += 34;
			d[0] = 33 + 14; put733(d + 2, 300); put733(d + 10, 5000);
			d[32] = 14;
			memcpy(d + 33, disc_has_file ? "1ST_READ.BIN;1" : "2ND_READ.BIN;1", 14);
		}
	}
	else
		for (i = 0; i < sizeof(sector); i++)
			sector[i] = (u8)(fad * 13 + i);
}
void libGDR_ReadSector(u8 *buff, u32 startSector, u32 sectorCount, u32 sectorSize)
{
	u32 s;
	printf("libGDR_ReadSector %u %u %u\n", startSector, sectorCount, sectorSize);
	for (s = 0; s < sectorCount; s++)
	{
		make_sector(startSector + s);
		memcpy(buff + s * sectorSize, sector, sectorSize < 2048 ? sectorSize : 2048);
	}
}

static u32 sum(const u8 *p, u32 n)
{
	u32 h = 2166136261u, i;
	for (i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

static void show(const char *what)
{
	int i;
	printf("== %s: pc %08x pr %08x sr %08x fpscr %08x", what, next_pc, pr,
			sr.status, fpscr.full);
	for (i = 0; i < 16; i++)
		printf(" r%d %08x", i, Sh4cntx.r[i]);
	printf("\n   vbr %08x gbr %08x ssr %08x spc %08x ram %08x rom %08x ccn %08x bsc %08x dmac %08x sb %08x pvr %08x flash %08x\n",
			vbr, gbr, ssr, spc,
			sum(ram, RAM_BYTES), sum(rom_area, sizeof(rom_area)),
			sum((const u8 *)CCN, sizeof(CCN)), sum((const u8 *)BSC, sizeof(BSC)),
			sum((const u8 *)DMAC, sizeof(DMAC)), sum((const u8 *)sb_regs, sizeof(sb_regs)),
			sum(pvr_regs, sizeof(pvr_regs)), sum(sys_nvmem_flash.data, sys_nvmem_flash.size));
}

/* The trap at @pc, as the CPU runs it */
static void trap(u32 pc, const char *what)
{
	next_pc = pc + 2;
	reios_trap(REIOS_OPCODE);
	show(what);
}

static void regs(u32 r4, u32 r5, u32 r6, u32 r7)
{
	Sh4cntx.r[4] = r4; Sh4cntx.r[5] = r5; Sh4cntx.r[6] = r6; Sh4cntx.r[7] = r7;
	Sh4cntx.r[15] = 0x8c00f400;
	pr = 0x8c0abcd0;
}

static void dc_boot(const char *what)
{
	reios_reset(rom_area, &sys_nvmem_flash);
	memset(&Sh4cntx, 0, sizeof(Sh4cntx));
	trap(0xa0000000, what);
}

int main(int argc, char **argv)
{
	static const u32 dc_calls[] = { 0x8c001000, 0x8c001002, 0x8c001004, 0x8c001006, 0x8c001008, 0x8c0010f0 };
	char what[64];
	u32 i, j;
	FILE *f;

	ram = (u8 *)calloc(1, RAM_BYTES);
	memchip_allocate(&sys_nvmem_flash, 0x20000);
	for (i = 0; i < 0x20000; i++)
		sys_nvmem_flash.data[i] = (u8)(i * 7);
	for (i = 0; i < sizeof(cart); i++)
		cart[i] = (u8)(i * 3 + (i >> 8));
	settings.System = DC_PLATFORM_DREAMCAST;
	settings.dreamcast.broadcast = 1;
	settings.dreamcast.region = 1;

	reios_init();

	/* the ROM, with the built-in font and then with a font.bin (argv[1] is
	 * a directory to put it in) */
	strcpy(game_dir_no_slash, "/nonexistent");
	reios_reset(rom_area, &sys_nvmem_flash);
	show("rom, built-in font");
	if (argc > 1)
	{
		strcpy(game_dir_no_slash, argv[1]);
		sprintf(what, "%s/font.bin", argv[1]);
		f = fopen(what, "wb");
		for (i = 0; i < 70000; i++)
			fputc((int)(i * 5 + 1) & 0xff, f);
		fclose(f);
		reios_reset(rom_area, &sys_nvmem_flash);
		show("rom, font.bin");
		remove(what);
		strcpy(game_dir_no_slash, "/nonexistent");
	}

	/* a Dreamcast's boot */
	disc_type = GdRom; disc_base = 45150; disc_has_file = true;
	dc_boot("boot GD-ROM");
	disc_has_file = false;
	dc_boot("boot GD-ROM, no boot file");
	disc_type = CdRom_XA; disc_base = 11702; disc_has_file = true;
	dc_boot("boot CD-R");
	disc_type = NoDisk;
	dc_boot("boot, no disc");
	strcpy(settings.reios.ElfFile, "prog.elf");
	dc_boot("boot an ELF");
	settings.reios.ElfFile[0] = 0;

	/* the system calls, from a boot of the GD-ROM */
	disc_type = GdRom; disc_base = 45150; disc_has_file = true;
	dc_boot("boot again");
	for (i = 0; i < sizeof(dc_calls) / sizeof(dc_calls[0]); i++)
		for (j = 0; j < 10; j++)
		{
			u32 r4 = j < 4 ? j : 0x8c100000 + j * 0x100;
			regs(r4, j == 9 ? 0x8c200000 : (j & 3), j % 3, j < 6 ? j : (u32)-1);
			if (dc_calls[i] == 0x8c001004 && j >= 2)
			{
				/* the flash's: partition, offset, size */
				Sh4cntx.r[4] = j % 5;
				Sh4cntx.r[5] = 0x8c300000;
				Sh4cntx.r[6] = j * 0x40;
				Sh4cntx.r[7] = j - 2;
			}
			sprintf(what, "call %08x %u", dc_calls[i], j);
			trap(dc_calls[i], what);
		}
	regs(0, 0, 0, 0);
	trap(0x8c001010, "no call there");
	trap(0x0c001000, "a call at another of its addresses");

	/* the save state's part */
	{
		u8 buf[64];
		void *p = buf;
		unsigned int total = 0;
		reios_serialize_v21(&p, &total);
		printf("reios state %u bytes:", total);
		for (i = 0; i < total; i++)
			printf(" %02x", buf[i]);
		printf("\n");
		p = buf; total = 0;
		printf("reios unserialize %d\n", (int)reios_unserialize_v21(&p, &total));
		reios_state_before_v21();
	}

	/* a NAOMI's boot and its BIOS's traps */
	settings.System = DC_PLATFORM_NAOMI;
	cart_in = true;
	memcpy(cart, "NAOMI           ", 16);
	cart[0x420] = 0x00; cart[0x421] = 0x10; cart[0x422] = 0x01; cart[0x423] = 0x8c;
	for (i = 0; i < 8; i++)
	{
		u32 *e = (u32 *)(cart + 0x360 + i * 12);
		e[0] = i == 0 ? 0x100 : 0xffffffff;
		e[1] = i == 0 ? 0x0c010000 : 0;
		e[2] = i == 0 ? 0x4000 : 0;
	}
	cart[0x428] = 0xff;
	reios_reset(rom_area, NULL);
	memset(&Sh4cntx, 0, sizeof(Sh4cntx));
	trap(0xa0000000, "NAOMI boot");
	for (i = 0; i < 26; i++)
		for (j = 0; j < 3; j++)
		{
			u32 k;
			/* the game's settings, switches and counts: small numbers */
			for (k = 0x1d000; k < 0x1f000; k += 4)
			{
				ram[k] = (u8)((k >> 2) * 7 + i + j) & 3;
			}
			/* (and what a frame's call writes, not there yet) */
			for (k = 0; k < 4; k++)
				wr(0x0c01e040 + k * 4, 0xffffffff, 4);
			regs(0x0c01f000 + j * 0x10, 0x0c01e000, 0x0c01d000, (j * 0x5a) | 0x100);
			Sh4cntx.r[15] = 0x0c01c000;
			wr(0x0c01c000, 0x0c01b000, 4);
			wr(0x0c01c004, 0x0c01a000, 4);
			sprintf(what, "NAOMI trap %u %u", i, j);
			trap(0x0c018400 + i * 8, what);
		}
	regs(0, 0, 0, 0);
	trap(0x0c000100, "NAOMI exception");
	trap(0x0c000600, "NAOMI interrupt");
	trap(0x0c000680, "NAOMI handler return");
	cart_in = false;
	trap(0xa0000000, "NAOMI boot, no cartridge");
	{
		u8 buf[64];
		void *p = buf;
		unsigned int total = 0;
		nbios_serialize_v21(&p, &total);
		printf("nbios state %u bytes\n", total);
		p = buf; total = 0;
		printf("nbios unserialize %d\n", (int)nbios_unserialize_v21(&p, &total));
		nbios_state_before_v21();
	}

	memchip_free(&sys_nvmem_flash);
	free(ram);
	return 0;
}
