/* A NAOMI GD-ROM game's DIMM board (core/hw/naomi/gdcartridge.c), by
 * itself: the PIC's data, a real PIC binary and one extracted, the disc
 * found where the set is or its parent's, the file the PIC names found on
 * it - through a file that names another, or in the network board's ROM
 * folder - and decrypted with the PIC's key; then the board's memory as a
 * transfer and a read have it, the game's name, the DIMM board's registers
 * and its commands, its poll, and its part of a save state. What it gives
 * is printed, for gd_expected.txt (sh tools/naomi/gd_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/gdcartridge.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_sched.h"
#include "imgread/common.h"

#ifdef GD_TEST_OLD
/* (the board as a class was, through a shim) */
#define naomi_gd_serialize_v25 gd_old_serialize_v25
#define naomi_gd_unserialize_v25 gd_old_unserialize_v25
#define naomi_gd_state_before_v25 gd_old_state_before_v25
#define naomi_gd_serial_in_v25 gd_old_serial_in_v25
#define naomi_gd_serialize_v31 gd_old_serialize_v31
#define naomi_gd_unserialize_v31 gd_old_unserialize_v31
#define naomi_gd_state_before_v31 gd_old_state_before_v31
void gd_old_serialize_v25(void **data, unsigned int *total_size);
bool gd_old_unserialize_v25(void **data, unsigned int *total_size);
void gd_old_state_before_v25(void);
bool gd_old_serial_in_v25(void);
void gd_old_serialize_v31(void **data, unsigned int *total_size);
bool gd_old_unserialize_v31(void **data, unsigned int *total_size);
void gd_old_state_before_v31(void);
#endif

/* (no log) */
retro_log_printf_t log_cb;
void os_DebugBreak(void) { printf("FAIL: os_DebugBreak\n"); exit(1); }

static u32 hash(const void *p, size_t n)
{
	const u8 *b = (const u8 *)p;
	u32 h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

/* The rest of the machine */
cartridge *CurrentCartridge;
char g_base_name[128];
char g_roms_dir[4096];
char g_parent_name[128];
u32 reg_dimm_command, reg_dimm_offsetl, reg_dimm_parameterl, reg_dimm_parameterh, reg_dimm_status;
RegisterStruct sb_regs[SB_REGS];
u16 NaomiGameIDRead(void) { return 0; }
void NaomiGameIDWrite(u16 data) { printf("  game id write %x\n", data); }
void naomi_process(u32 command, u32 offsetl, u32 parameterl, u32 parameterh)
{
	printf("  naomi process %x %x %x %x\n", command, offsetl, parameterl, parameterh);
}
void asic_CancelInterrupt(int inter) { printf("  cancel interrupt %x\n", inter); }
void asic_RaiseInterrupt(int inter) { printf("  raise interrupt %x\n", inter); }
bool mem_region_release(void *start, size_t len) { (void)start; (void)len; return true; }
void archive_close(void *a) { (void)a; }
bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	if (*dest != NULL && src_size != 0)
	{
		memcpy(*dest, src, src_size);
		*dest = (u8 *)*dest + src_size;
	}
	*total_size += src_size;
	return true;
}
bool ra_unserialize(void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	if (*dest != NULL && src_size != 0)
	{
		memcpy(src, *dest, src_size);
		*dest = (u8 *)*dest + src_size;
	}
	*total_size += src_size;
	return true;
}

/* The scheduler */
sched_list sch_list[SH4_SCHED_SLOTS];
static int registered;
int sh4_sched_register(int tag, sh4_sched_callback *ssc)
{
	sch_list[registered].cb = ssc;
	sch_list[registered].tag = tag;
	sch_list[registered].start = -1;
	sch_list[registered].end = -1;
	printf("  sched register %d\n", registered);
	return registered++;
}
void sh4_sched_request(int id, int cycles)
{
	printf("  sched request %d %d\n", id, cycles);
	sch_list[id].start = cycles == -1 ? -1 : 0;
	sch_list[id].end = cycles;
}
void periph_sched_restore(int id, s32 start, s32 end)
{
	printf("  sched restore %d %d %d\n", id, (int)start, (int)end);
}

/* The NAOMI's memory, as the board writes it (hashed) and reads it */
static u32 written = 2166136261u, writes;
void DYNACALL _vmem_WriteMem32(u32 address, u32 data)
{
	written = (written ^ address ^ data) * 16777619u;
	writes++;
}
void DYNACALL _vmem_WriteMem8(u32 address, u8 data)
{
	written = (written ^ address ^ data) * 16777619u;
	writes++;
}
static u32 request;
u32 DYNACALL _vmem_ReadMem32(u32 address)
{
	printf("  read %x\n", address);
	return request;
}

/* The ROM board's serial EEPROM */
static u8 serial[142];
const u8 *naomi_rom_serial_data(void) { return serial; }
void naomi_rom_serial_reset(void) { printf("  serial reset\n"); }
void naomi_rom_serial_serialize(void **data, unsigned int *total_size)
{
	ra_serialize(serial, 142, data, total_size);
}
void naomi_rom_serial_unserialize(void **data, unsigned int *total_size)
{
	static u8 sink[142];
	ra_unserialize(sink, 142, data, total_size);
}

/* The disc: some sectors, the rest zeroes; found at one path */
#define SECTORS 16
static u32 sector_at[SECTORS];
static u8 sectors[SECTORS][2048];
static int used;
static const char *disc_path;
static Disc the_disc;

static u8 *sector(u32 n)
{
	sector_at[used] = n;
	memset(sectors[used], 0, 2048);
	return sectors[used++];
}

Disc *OpenDisc(const char *fn)
{
	printf("  open %s: %s\n", fn, disc_path != NULL && strcmp(fn, disc_path) == 0 ? "yes" : "no");
	return disc_path != NULL && strcmp(fn, disc_path) == 0 ? &the_disc : NULL;
}
void disc_read_sectors(Disc *d, u32 FAD, u32 count, u8 *dst, u32 fmt)
{
	u32 i;
	int j;
	(void)d;
	printf("  disc read %u %u %u\n", FAD, count, fmt);
	for (i = 0; i < count; i++)
	{
		memset(dst + 2048 * i, 0, 2048);
		for (j = 0; j < used; j++)
			if (sector_at[j] == FAD - 150 + i)
				memcpy(dst + 2048 * i, sectors[j], 2048);
	}
}
void disc_free(Disc *d)
{
	printf("  disc freed %d\n", d == &the_disc);
}

static u32 seed = 1;
static u32 rnd(void)
{
	seed = seed * 1103515245u + 12345u;
	return seed >> 8;
}

static void put32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

/* A directory record: @flags, @name, its file at @start, @size bytes */
static u32 record(u8 *dir, u32 pos, u8 flags, const char *name, u32 start, u32 size)
{
	const u32 len = (u32)strlen(name);
	const u32 reclen = (33 + len + 1) & ~1u;
	dir[pos] = (u8)reclen;
	put32(dir + pos + 2, start);
	put32(dir + pos + 10, size);
	dir[pos + 25] = flags;
	dir[pos + 32] = (u8)len;
	memcpy(dir + pos + 33, name, len);
	return pos + reclen;
}

static void fill(u8 *p, u32 len)
{
	u32 i;
	for (i = 0; i < len; i++)
		p[i] = (u8)rnd();
}

static void print_id(cartridge *c)
{
	char id[64];
	const char *p;
	memset(id, 0x55, sizeof(id));
	c->ops->game_id(c, id);
	printf("  id [");
	for (p = id; *p != '\0'; p++)
		printf((u8)*p >= 0x20 && (u8)*p < 0x7f ? "%c" : "\\x%02x", (u8)*p);
	printf("]\n");
}

static void regs(cartridge *c)
{
	u32 a;
	printf("  regs");
	for (a = 0; a < 0x100; a += 4)
		if (a != 0x08)
			printf(" %x", c->ops->read_mem(c, 0x5f7000 + a, 2));
	printf("\n");
}

static u8 state[4096];

static void board(cartridge *c, int net)
{
	unsigned int total = 0, total2 = 0;
	void *p = state;
	u32 v, i;

	print_id(c);
	/* the memory, read and a transfer's */
	for (i = 0; i < 4; i++)
	{
		v = 0x55555555;
		c->ops->read(c, 0x1000 * i + 0x7fe, 4, &v);
		printf("  read %x\n", v);
	}
	/* (at the end of the memory: what is there of it) */
	v = 0x55555555;
	c->ops->read(c, 0xffffffe, 4, &v);
	printf("  read %x\n", v);
	c->ops->write_mem(c, 0x5f700c, 0x7fff, 2);
	c->ops->write_mem(c, 0x5f7010, 0xfff0, 2);
	for (i = 0; i < 3; i++)
	{
		u32 size = 0x2000;
		const void *d = c->ops->get_dma_ptr(c, &size);
		printf("  dma %x %08x\n", size, d != NULL ? hash(d, size) : 0);
		c->ops->advance_ptr(c, size);
	}
	regs(c);
	/* a command: start up, and one of the network's */
	c->ops->write_mem(c, 0x5f703c, 0x1e00, 2);
	c->ops->write_mem(c, 0x5f7040, 0x12345, 2);
	c->ops->write_mem(c, 0x5f7044, 0x6789a, 2);
	c->ops->write_mem(c, 0x5f7048, 0xbcdef, 2);
	c->ops->write_mem(c, 0x5f704c, 0x0, 2);
	printf("  written %u %08x\n", writes, written);
	regs(c);
	c->ops->write_mem(c, 0x5f704c, 0x101, 2);
	c->ops->write_mem(c, 0x5f703c, 0x2200, 2);
	c->ops->write_mem(c, 0x5f704c, 0x0, 2);
	regs(c);
	c->ops->write_mem(c, 0x5f703c, 0x0a00, 2);
	c->ops->write_mem(c, 0x5f704c, 0x100, 2);
	c->ops->write_mem(c, 0x5f704c, 0x0, 2);
	c->ops->write_mem(c, 0x5f703c, 0x1a00, 2);
	c->ops->write_mem(c, 0x5f704c, 0x0, 2);
	/* the poll: with the last interrupt not taken, and taken */
	for (i = 0; i < SH4_SCHED_SLOTS && sch_list[i].cb != NULL; i++)
		;
	if (i > 0)
	{
		sched_list *s = &sch_list[i - 1];
		SB_ISTEXT = 8;
		printf("  poll %d\n", s->cb(s->tag, 0, 0));
		regs(c);
		SB_ISTEXT = 0;
		request = 0x0101 | (u32)net << 16;
		printf("  poll %d\n", s->cb(s->tag, 0, 0));
		printf("  written %u %08x\n", writes, written);
		regs(c);
	}
	/* its part of a state */
	printf("  in v25 %d\n", naomi_gd_serial_in_v25());
	if (net)
		naomi_gd_serialize_v31(&p, &total);
	else
		naomi_gd_serialize_v25(&p, &total);
	printf("  state %u %08x\n", total, hash(state, total));
	state[0] = 0;
	p = state;
	if (net)
		naomi_gd_unserialize_v31(&p, &total2);
	else
		naomi_gd_unserialize_v25(&p, &total2);
	printf("  state read %u\n", total2);
	regs(c);
	c->ops->reset(c);
	regs(c);
	if (net)
		naomi_gd_state_before_v31();
	else
		naomi_gd_state_before_v25();
	regs(c);
	/* (the other board's are nothing) */
	total = 0;
	p = NULL;
	if (net)
		naomi_gd_serialize_v25(&p, &total);
	else
		naomi_gd_serialize_v31(&p, &total);
	printf("  other state %u\n", total);
	/* (and none while the cartridge is not the current one) */
	CurrentCartridge = NULL;
	total = 0;
	p = NULL;
	if (net)
		naomi_gd_serialize_v31(&p, &total);
	else
		naomi_gd_serialize_v25(&p, &total);
	printf("  not current %u %d\n", total, naomi_gd_serial_in_v25());
	CurrentCartridge = c;
	c->ops->reset(c);
	regs(c);
}

/* A real PIC binary: its file's name, its key and whether it is the
 * network board's */
static void real_pic(cartridge *c, const char *name, int netpic)
{
	u32 len = 0x4000, i;
	u8 *pic = (u8 *)c->ops->get_ptr(c, 0, &len);
	fill(pic, 0x4000);
	for (i = 0; i < 7; i++)
	{
		pic[0x7c0 + i * 2] = (u8)name[i];
		pic[0x7e0 + i * 2] = (u8)name[7 + i];
	}
	pic[0x6ee] = (u8)netpic;
}

/* The plain board's disc: a file of the PIC's name that names the file
 * (and others), at 45000 */
static void plain_disc(u32 file_size)
{
	u8 *s;
	u32 pos = 0;
	used = 0;
	s = sector(45016);
	put32(s + 0x8c, 45020);
	s = sector(45020);
	put32(s + 2, 45021);
	s = sector(45021);
	pos = record(s, pos, 2, "\001", 45021, 2048);
	pos = record(s, pos, 0, "OTHER.BIN;1", 45022, 0x100);
	pos = record(s, pos, 0, "ABCDEFGHIJ.BI;1", 45023, 0x100);
	pos = record(s, pos, 0, "ABCDEFGHIJ.BIN;1", 45030, 0x100);
	pos = record(s, pos, 0, "GAME.DAT;1", 45040, file_size);
	pos = record(s, pos, 0, "GAME.DATX", 45050, 0x800);
	s = sector(45030);
	fill(s, 0xc0);
	memcpy(s + 0xc0, "GAME.DAT", 8);
	s = sector(45040);
	fill(s, 2048);
	s = sector(45041);
	fill(s, 2048);
	s = sector(45042);
	fill(s, 2048);
}

int main(void)
{
	cartridge *c;
	u8 *s;
	u32 len;

	strcpy(g_roms_dir, "/roms");
	strcpy(g_base_name, "game.v2.zip");

	printf("plain\n");
	disc_path = "/roms/game.v2/gdl-0001.chd";
	plain_disc(5000);
	c = CurrentCartridge = gd_cart_open(0x4000, "gdl-0001", true, false);
	real_pic(c, "ABCDEFGHIJ.BIN", 0);
	c->ops->set_key(c, 0x1234);
	printf("  init %d\n", c->ops->init(c));
	board(c, 0);
	c->ops->close(c);

	printf("net\n");
	used = 0;
	disc_path = "/roms/game.v2/gds-0002.gdi";
	s = sector(16);
	put32(s + 0x8c, 18);
	s = sector(18);
	s[0] = 1; put32(s + 2, 19); s[8] = 1;
	s[10] = 5; put32(s + 12, 20); memcpy(s + 18, "OTHER", 5);
	s[24] = 3; put32(s + 26, 21); memcpy(s + 32, "ROM", 3);
	s = sector(21);
	record(s, record(s, 0, 2, "\001", 21, 2048), 0, "ROM.BIN;1", 30, 6000);
	s = sector(30); fill(s, 2048);
	s = sector(31); fill(s, 2048);
	s = sector(32); fill(s, 2048);
	c = CurrentCartridge = gd_cart_open(0x4000, "gds-0002", true, true);
	/* (the network board's PIC names no file: ROM.BIN in the ROM folder) */
	real_pic(c, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 1);
	printf("  init %d\n", c->ops->init(c));
	board(c, 1);
	c->ops->close(c);

	printf("extracted, the parent's\n");
	strcpy(g_parent_name, "parent");
	strcpy(g_base_name, "game");
	disc_path = "/roms/parent/gdl-0003.chd";
	plain_disc(1000);
	c = CurrentCartridge = gd_cart_open(0x80, "gdl-0003", true, false);
	len = 0x80;
	s = (u8 *)c->ops->get_ptr(c, 0, &len);
	fill(s, 0x80);
	memcpy(s + 33, "ABCDEFG", 7);
	memcpy(s + 25, "HIJ.BIN", 7);
	printf("  init %d\n", c->ops->init(c));
	print_id(c);
	{
		u32 size = 0x10000;
		const void *d;
		c->ops->write_mem(c, 0x5f700c, 0, 2);
		c->ops->write_mem(c, 0x5f7010, 0, 2);
		d = c->ops->get_dma_ptr(c, &size);
		printf("  dma %x %08x\n", size, d != NULL ? hash(d, size) : 0);
	}
	c->ops->close(c);

	printf("no disc\n");
	disc_path = NULL;
	c = CurrentCartridge = gd_cart_open(0x4000, "gdl-0004", true, false);
	real_pic(c, "ABCDEFGHIJ.BIN", 0);
	printf("  init %d\n", c->ops->init(c));
	c->ops->close(c);

	printf("no file, no DIMM board\n");
	strcpy(g_parent_name, "");
	disc_path = "/roms/game/gdl-0005.chd";
	plain_disc(5000);
	c = CurrentCartridge = gd_cart_open(0x4000, "gdl-0005", false, false);
	real_pic(c, "NOTHERE.BIN   ", 0);
	printf("  init %d\n", c->ops->init(c));
	{
		u32 v = 0x55555555;
		c->ops->read(c, 0, 4, &v);
		printf("  read %x\n", v);
	}
	print_id(c);
	regs(c);
	c->ops->write_mem(c, 0x5f704c, 0x0, 2);
	printf("  in v25 %d\n", naomi_gd_serial_in_v25());
	c->ops->reset(c);
	regs(c);
	c->ops->close(c);

	printf("done\n");
	return 0;
}
