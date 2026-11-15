/* The cartridges' ROM boards (core/hw/naomi/cartridge.c, m1cartridge.c,
 * m4cartridge.c, awcartridge.c), by themselves: an M2 board in both of
 * its modes, with its decryption chip; an M1 board's decompressor; an M4
 * board's cipher and its flash's CFI table; an AtomisWave's cipher and its
 * file system's offsets; a flat image, mapped or an archive's. For each:
 * every register read and written, what a transfer reads a piece at a
 * time, the game's name, and its part of a save state, read back and not
 * right. What they give is printed, for cart_expected.txt
 * (sh tools/naomi/cart_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"

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
u32 reg_dimm_command, reg_dimm_offsetl, reg_dimm_parameterl, reg_dimm_parameterh, reg_dimm_status;
static u16 game_id_bit = 1;
u16 NaomiGameIDRead(void)
{
	game_id_bit ^= 1;
	return game_id_bit;
}
void NaomiGameIDWrite(u16 data)
{
	printf("  game id write %x\n", data);
}
void naomi_process(u32 command, u32 offsetl, u32 parameterl, u32 parameterh)
{
	printf("  dimm process %x %x %x %x\n", command, offsetl, parameterl, parameterh);
}
void asic_CancelInterrupt(int inter)
{
	printf("  cancel interrupt %x\n", inter);
}
bool mem_region_release(void *start, size_t len)
{
	printf("  mapping released (%x bytes)\n", (unsigned)len);
	free(start);
	return true;
}
void archive_close(void *a)
{
	printf("  archive closed\n");
	free(a);
}
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

/* The boards, as the test has them: the core's, or (CART_TEST_OLD) the
 * classes they were, through a shim */
enum { T_FLAT, T_M1, T_M2, T_M4, T_AW, T_ARCHIVED };

void *tc_open(int type, u32 size, u8 *rom);
u8 *tc_rom(void *c, u32 offset, u32 *size);
void tc_set_key(void *c, u32 key);
void tc_set_key_data(void *c, u8 *data);
bool tc_init(void *c);
void tc_reset(void *c);
u32 tc_read_mem(void *c, u32 address, u32 size);
void tc_write_mem(void *c, u32 address, u32 data, u32 size);
bool tc_read(void *c, u32 offset, u32 size, void *dst);
bool tc_write(void *c, u32 offset, u32 size, u32 data);
void *tc_dma(void *c, u32 *size);
void tc_advance(void *c, u32 size);
void tc_game_id(void *c, char *id);
void tc_serialize(void *c, void **data, unsigned int *total_size);
void tc_unserialize(void *c, void **data, unsigned int *total_size);
void tc_serialize_v21(void *c, void **data, unsigned int *total_size);
void tc_unserialize_v21(void *c, void **data, unsigned int *total_size);
void tc_close(void *c);

#ifndef CART_TEST_OLD
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/m1cartridge.h"
#include "hw/naomi/m4cartridge.h"
#include "hw/naomi/awcartridge.h"

cartridge *CurrentCartridge;

#define C ((cartridge *)c)
void *tc_open(int type, u32 size, u8 *rom)
{
	switch (type)
	{
	case T_FLAT: return CurrentCartridge = decrypted_cart_open(rom, size);
	case T_ARCHIVED: return CurrentCartridge = archived_cart_open(rom, size, (struct archive *)malloc(1));
	case T_M1: return CurrentCartridge = m1_cart_open(size);
	case T_M2: return CurrentCartridge = m2_cart_open(size);
	case T_M4: return CurrentCartridge = m4_cart_open(size);
	default: return CurrentCartridge = aw_cart_open(size);
	}
}
u8 *tc_rom(void *c, u32 offset, u32 *size) { return (u8 *)C->ops->get_ptr(C, offset, size); }
void tc_set_key(void *c, u32 key) { C->ops->set_key(C, key); }
void tc_set_key_data(void *c, u8 *data) { C->ops->set_key_data(C, data); }
bool tc_init(void *c) { return C->ops->init(C); }
void tc_reset(void *c) { C->ops->reset(C); }
u32 tc_read_mem(void *c, u32 address, u32 size) { return C->ops->read_mem(C, address, size); }
void tc_write_mem(void *c, u32 address, u32 data, u32 size) { C->ops->write_mem(C, address, data, size); }
bool tc_read(void *c, u32 offset, u32 size, void *dst) { return C->ops->read(C, offset, size, dst); }
bool tc_write(void *c, u32 offset, u32 size, u32 data) { return C->ops->write(C, offset, size, data); }
void *tc_dma(void *c, u32 *size) { return C->ops->get_dma_ptr(C, size); }
void tc_advance(void *c, u32 size) { C->ops->advance_ptr(C, size); }
void tc_game_id(void *c, char *id) { C->ops->game_id(C, id); }
void tc_serialize(void *c, void **data, unsigned int *total_size) { C->ops->serialize(C, data, total_size); }
void tc_unserialize(void *c, void **data, unsigned int *total_size) { C->ops->unserialize(C, data, total_size); }
void tc_serialize_v21(void *c, void **data, unsigned int *total_size) { C->ops->serialize_v21(C, data, total_size); }
void tc_unserialize_v21(void *c, void **data, unsigned int *total_size) { C->ops->unserialize_v21(C, data, total_size); }
void tc_close(void *c) { C->ops->close(C); CurrentCartridge = NULL; }
#endif

static u32 seed;
static u32 rnd(void)
{
	seed = seed * 1103515245u + 12345u;
	return seed >> 8;
}

static void fill(void *c, u32 size)
{
	u32 len = size, i;
	u8 *rom = tc_rom(c, 0, &len);
	if (rom == NULL || len != size)
	{
		printf("FAIL: no ROM\n");
		exit(1);
	}
	for (i = 0; i < size; i++)
		rom[i] = (u8)rnd();
}

static void name(void *c, u32 offset, const char *s)
{
	u32 len = 0x20;
	u8 *p = tc_rom(c, offset, &len);
	if (p == NULL)
		return;
	memset(p, ' ', 0x20);
	memcpy(p, s, strlen(s));
}

static void print_id(void *c)
{
	char id[64];
	const char *p;
	memset(id, 0x55, sizeof(id));
	tc_game_id(c, id);
	printf("  id [");
	for (p = id; *p != '\0'; p++)
		printf((u8)*p >= 0x20 && (u8)*p < 0x7f ? "%c" : "\\x%02x", (u8)*p);
	printf("]\n");
}

static void regs(void *c)
{
	u32 a;
	printf("  regs");
	for (a = 0; a < 0x100; a += 4)
		if (a != 0x08 && a != 0x80)	/* (the data registers: read on their own) */
			printf(" %x", tc_read_mem(c, 0x5f7000 + a, 2));
	printf("\n");
}

static void dma(void *c, int pieces, u32 want)
{
	int i;
	for (i = 0; i < pieces; i++)
	{
		u32 size = want;
		const void *p = tc_dma(c, &size);
		printf("  dma %x %08x\n", size, p != NULL ? hash(p, size) : 0);
		if (size == 0)
			break;
		tc_advance(c, size);
	}
}

static void pio(void *c, u32 offset, int words, u32 high_flags)
{
	int i;
	tc_write_mem(c, 0x5f7000, ((offset >> 16) & 0x7fff) | high_flags, 2);
	tc_write_mem(c, 0x5f7004, offset & 0xffff, 2);
	printf("  pio");
	for (i = 0; i < words; i++)
		printf(" %x", tc_read_mem(c, 0x5f7008, 2));
	printf("\n");
}

static void dma_at(void *c, u32 offset)
{
	tc_write_mem(c, 0x5f700c, (offset >> 16) & 0x7fff, 2);
	tc_write_mem(c, 0x5f7010, offset & 0xffff, 2);
}

static u8 state[256 * 1024];

static u32 save(void *c)
{
	void *p = state;
	unsigned int total = 0;
	tc_serialize(c, &p, &total);
	printf("  state %u %08x\n", total, hash(state, total));
	return total;
}

static void load(void *c)
{
	void *p = state;
	unsigned int total = 0;
	tc_unserialize(c, &p, &total);
	printf("  state read %u\n", total);
}

/* What every NAOMI board has */
static void naomi_board(void *c)
{
	u32 v = 0;
	regs(c);
	tc_write_mem(c, 0x5f703c, 0x1e03, 2);
	tc_write_mem(c, 0x5f7040, 0x1234, 2);
	tc_write_mem(c, 0x5f7044, 0x5678, 2);
	tc_write_mem(c, 0x5f7048, 0x9abc, 2);
	tc_write_mem(c, 0x5f704c, 0x0, 2);
	tc_write_mem(c, 0x5f704c, 0x1, 2);
	tc_write_mem(c, 0x5f704c, 0x101, 2);
	tc_write_mem(c, 0x5f7014, 0x12345, 2);
	tc_write_mem(c, 0x5f7068, 0x3, 2);
	tc_write_mem(c, 0x5f7078, 0x1ffff, 2);
	tc_write_mem(c, 0x5f707c, 0x7, 2);
	tc_write_mem(c, 0x5f7050, 0x7, 2);
	tc_write_mem(c, 0x5f7054, 0x7, 2);
	tc_write_mem(c, 0x5f70f0, 0x7, 2);
	regs(c);
	printf("  write %d\n", tc_write(c, 0x100, 2, 0x55));
	printf("  read %d", tc_read(c, 0x1ffffffe, 4, &v));
	printf(" %x\n", v);
}

static void flat(int type)
{
	const u32 size = 0x20000;
	u8 *rom = (u8 *)malloc(size);
	void *c;
	u32 i;
	for (i = 0; i < size; i++)
		rom[i] = (u8)rnd();
	printf("%s\n", type == T_FLAT ? "flat" : "archived");
	c = tc_open(type, size, rom);
	tc_set_key(c, 0x12345678);
	printf("  init %d\n", tc_init(c));
	name(c, 0x30, "AWNAOMI");
	name(c, 0xff30, "  THE GAME  ");
	print_id(c);
	naomi_board(c);
	pio(c, 0x1fff0, 12, 0x8000);
	pio(c, 0x40, 4, 0);
	dma_at(c, 0x1f000);
	dma(c, 4, 0x700);
	dma_at(c, 0x3000);
	dma(c, 1, 0x100);
	save(c);
	dma(c, 1, 0x100);
	load(c);
	dma(c, 1, 0x100);
	regs(c);
	tc_reset(c);
	regs(c);
	{
		u32 len = 4;
		printf("  ptr %d %x\n", tc_rom(c, size - 2, &len) != NULL, len);
		len = 4;
		printf("  ptr %d %x\n", tc_rom(c, 0xe0000010, &len) != NULL, len);
	}
	tc_close(c);
	/* (an archive's image is the archive's) */
	if (type == T_ARCHIVED)
		free(rom);
}

static void m2(void)
{
	const u32 size = 0x1000000;
	void *c = tc_open(T_M2, size, NULL);
	int i;
	printf("m2\n");
	fill(c, size);
	{
		u32 len = 0x20;
		memset(tc_rom(c, 0x30, &len), 0xff, 0x20);
	}
	name(c, 0x800030, "M2 GAME");
	tc_set_key(c, 0x2a8c5f0e);
	printf("  init %d\n", tc_init(c));
	print_id(c);
	naomi_board(c);
	/* 4 MB mode, and the whole */
	pio(c, 0x00400010, 4, 0x8000);
	pio(c, 0x20400010, 4, 0x8000);
	pio(c, 0x00c00010, 4, 0x8000);
	dma_at(c, 0x003fff00);
	dma(c, 3, 0x80);
	pio(c, 0x20000000, 1, 0);
	dma_at(c, 0x003fff00);
	dma(c, 3, 0x80);
	dma_at(c, 0x00fffff0);
	dma(c, 2, 0x80);
	/* The decryption chip: its RAM, where it reads from, its key */
	for (i = 0; i < 16; i++)
		tc_write(c, 0x40020000 + 2 * i + (i & 1), 0x1111 * i, 2);
	tc_write(c, 0x4001fff8, 0x0000, 2);
	tc_write(c, 0x4001fffa, 0x0100, 2);
	tc_write(c, 0x4001fffc, 0x8e1f, 2);
	printf("  decrypt");
	for (i = 0; i < 8; i++)
	{
		u16 w = 0;
		tc_read(c, 0x4001fffe, 2, &w);
		printf(" %x", w);
	}
	printf("\n");
	tc_write(c, 0x4001fff8, 0x2000, 2);
	tc_write(c, 0x4001fffa, 0x0000, 2);
	{
		u16 w = 0;
		printf("  bad read %d", tc_read(c, 0x40000010, 2, &w));
		printf(" %x\n", w);
	}
	printf("  decrypt");
	for (i = 0; i < 8; i++)
	{
		u16 w = 0;
		tc_read(c, 0x4001fffe, 2, &w);
		printf(" %x", w);
	}
	printf("\n");
	save(c);
	{
		static u8 v21[65536];
		void *p = v21;
		unsigned int total = 0, total2 = 0;
		u16 w = 0, w2 = 0;
		tc_serialize_v21(c, &p, &total);
		printf("  v21 %u %08x\n", total, hash(v21, total));
		tc_read(c, 0x4001fffe, 2, &w);
		p = v21;
		tc_unserialize_v21(c, &p, &total2);
		tc_read(c, 0x4001fffe, 2, &w2);
		printf("  again %x %x\n", w, w2);
	}
	tc_write(c, 0x40020000, 0xbeef, 2);
	load(c);
	tc_write(c, 0x4001fff8, 0x0000, 2);
	tc_write(c, 0x4001fffa, 0x0100, 2);
	printf("  decrypt");
	for (i = 0; i < 4; i++)
	{
		u16 w = 0;
		tc_read(c, 0x4001fffe, 2, &w);
		printf(" %x", w);
	}
	printf("\n");
	tc_reset(c);
	regs(c);
	/* a name of one letter: the one at 8 MB */
	name(c, 0x30, "X");
	print_id(c);
	tc_close(c);
}

/* An M1 board's compressed stream at @offset, as its decompressor reads it
 * with a key of 0: a dictionary whose first byte has bit 6 (each byte the
 * one two before less what is decoded), then @count bytes, each sent as
 * itself, none of them the end */
static u32 bits_word, bits_count, bits_at;

static void put_bits(void *c, u32 value, int n)
{
	while (n-- > 0)
	{
		bits_word = (bits_word << 1) | ((value >> n) & 1);
		if (++bits_count == 32)
		{
			/* (the word the board reads is put together from four bytes) */
			const u32 w = bits_word;
			const u8 a = (u8)w, b = (u8)(w >> 8);
			u32 len = 4;
			u8 *rom = tc_rom(c, bits_at, &len);
			rom[0] = a;
			rom[1] = b;
			rom[2] = a ^ (u8)(w >> 16);
			rom[3] = b ^ (u8)(w >> 24);
			bits_at += 4;
			bits_count = 0;
			bits_word = 0;
		}
	}
}

static void m1_stream(void *c, u32 offset, int count)
{
	int i;
	bits_word = 0;
	bits_count = 0;
	bits_at = offset;
	put_bits(c, 0x40, 8);
	for (i = 1; i < 111; i++)
		put_bits(c, (u32)(i * 7), 8);
	for (i = 0; i < count; i++)
	{
		put_bits(c, 0, 4);
		put_bits(c, rnd() & 0xff, 8);
	}
	while (bits_count != 0)
		put_bits(c, 0, 1);
}

static void m1(void)
{
	const u32 size = 0x80000;
	void *c = tc_open(T_M1, size, NULL);
	u32 total;
	printf("m1\n");
	fill(c, size);
	name(c, 0x30, "M1 GAME");
	tc_set_key(c, 0x0d6f8e4c);
	printf("  init %d\n", tc_init(c));
	print_id(c);
	naomi_board(c);
	/* compressed, from two places; and not */
	dma_at(c, 0x1000);
	dma(c, 3, 0x400);
	dma(c, 3, 0x180);
	dma_at(c, 0x7ff00);
	dma(c, 3, 0x400);
	dma_at(c, 0x10000);
	dma(c, 2, 0x300);
	total = save(c);
	dma(c, 2, 0x300);
	load(c);
	dma(c, 2, 0x300);
	dma_at(c, 0x20001000);
	dma(c, 2, 0x200);
	/* a state not right: its buffer longer than it is, its bits too many */
	dma_at(c, 0x2000);
	dma(c, 1, 0x80);
	save(c);
	memset(state + 32768 + 111 + 2 + 8 + 4, 0xff, 8);
	load(c);
	save(c);
	dma(c, 2, 0x100);
	dma(c, 2, 0x8000);
	(void)total;
	tc_reset(c);
	regs(c);
	dma(c, 1, 0x100);
	{
		static const u32 keys[] = { 0x12345678, 0x9abcdef0, 0x0f1e2d3c, 0x4b5a6978, 0xc3d2e1f0, 0x00000000 };
		unsigned k;
		for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
		{
			tc_set_key(c, keys[k]);
			dma_at(c, 0x400 * k);
			dma(c, 3, 0x400);
			dma(c, 2, 0x3ff);
		}
	}
	/* a stream that goes on: what is decoded past a whole buffer goes by
	 * the last two bytes of it */
	m1_stream(c, 0x40000, 4000);
	tc_set_key(c, 0);
	dma_at(c, 0x40000);
	dma(c, 3, 0x400);
	dma(c, 3, 0x3fe);
	dma(c, 2, 0x401);
	tc_close(c);
	/* too small for a name */
	c = tc_open(T_M1, 0x40, NULL);
	print_id(c);
	tc_close(c);
}

static void m4(void)
{
	const u32 size = 0x200000;
	void *c = tc_open(T_M4, size, NULL);
	u8 *key = (u8 *)malloc(2048);
	int i;
	printf("m4\n");
	fill(c, size);
	for (i = 0; i < 2048; i++)
		key[i] = (u8)rnd();
	tc_set_key(c, 0x5582);
	tc_set_key_data(c, key);
	printf("  init %d\n", tc_init(c));
	print_id(c);
	naomi_board(c);
	/* plain, then through the cipher */
	pio(c, 0x100, 4, 0x8000);
	pio(c, 0x40000100, 8, 0x8000);
	pio(c, 0x40000200, 4, 0);
	dma_at(c, 0x4000);
	dma(c, 3, 0x400);
	dma(c, 2, 0x37e);
	pio(c, 0x00000100, 1, 0);
	dma_at(c, 0x1ffff0);
	dma(c, 2, 0x40);
	dma_at(c, 0x300000);
	dma(c, 1, 0x40);
	/* the flash's CFI table */
	tc_write_mem(c, 0x5f7000, 0, 2);
	tc_write_mem(c, 0x5f7004, 0xaa, 2);
	tc_write_mem(c, 0x5f7008, 0x98, 2);
	pio(c, 0x20, 6, 0x8000);
	pio(c, 0xfffe, 1, 0);
	pio(c, 0x0c000020, 1, 0);
	dma_at(c, 0x20);
	dma(c, 2, 0x10);
	dma_at(c, 0x1000);
	dma(c, 1, 0x10);
	tc_write_mem(c, 0x5f7004, 0x0, 2);
	tc_write_mem(c, 0x5f7008, 0xf0, 2);
	pio(c, 0x20, 2, 0);
	/* a state, and one not right */
	pio(c, 0x40000400, 3, 0x8000);
	save(c);
	pio(c, 0x40000800, 3, 0x8000);
	load(c);
	printf("  pio %x\n", tc_read_mem(c, 0x5f7008, 2));
	{
		u32 big = 0x12345;
		memcpy(state + 32768 + 4, &big, 4);
	}
	load(c);
	save(c);
	dma(c, 1, 0x40000);
	{
		u32 odd = 0x101;
		memcpy(state + 32768 + 4, &odd, 4);
	}
	load(c);
	save(c);
	dma(c, 1, 0x40000);
	tc_reset(c);
	regs(c);
	print_id(c);
	{
		u32 len = 2;
		memcpy(tc_rom(c, 0, &len), "NA", 2);
	}
	name(c, 0x30, "M4 PLAIN");
	print_id(c);
	tc_close(c);
}

static void aw(void)
{
	const u32 size = 0x800000;
	void *c = tc_open(T_AW, size, NULL);
	int i;
	printf("aw\n");
	fill(c, size);
	tc_set_key(c, 0x9d);
	printf("  init %d\n", tc_init(c));
	print_id(c);
	regs(c);
	/* the program, its file system's records, a file */
	tc_write_mem(c, 0x5f7000, 0x0100, 2);
	tc_write_mem(c, 0x5f7004, 0x0000, 2);
	dma(c, 3, 0x40);
	printf("  pio %x\n", tc_read_mem(c, 0x5f7080, 2));
	tc_write_mem(c, 0x5f700c, 0x3, 2);
	dma(c, 2, 0x40);
	tc_write_mem(c, 0x5f7010, 0x2, 2);
	tc_write_mem(c, 0x5f7014, 0x10, 2);
	tc_write_mem(c, 0x5f7018, 0x0, 2);
	dma(c, 2, 0x20);
	/* another bank */
	tc_write_mem(c, 0x5f7000, 0xffff, 2);
	tc_write_mem(c, 0x5f7004, 0x007f, 2);
	tc_write_mem(c, 0x5f7080, 0x2, 2);
	tc_write_mem(c, 0x5f7004, 0x7fff, 2);
	tc_write_mem(c, 0x5f7000, 0x0000, 2);
	printf("  pio %x\n", tc_read_mem(c, 0x5f7080, 2));
	tc_write_mem(c, 0x5f7080, 0x2, 2);
	tc_write_mem(c, 0x5f7004, 0xffff, 2);
	tc_write_mem(c, 0x5f7000, 0x007f, 2);
	tc_write_mem(c, 0x5f7080, 0x7, 2);
	tc_write_mem(c, 0x5f700c, 0x9, 2);
	dma(c, 2, 0x40);
	for (i = 0; i < 4; i++)
		tc_write_mem(c, 0x5f7020 + 4 * i, i, 2);
	save(c);
	tc_write_mem(c, 0x5f700c, 0x1, 2);
	load(c);
	dma(c, 1, 0x40);
	{
		u32 bank = 0x77;
		memcpy(state + 4, &bank, 4);
	}
	load(c);
	save(c);
	tc_write_mem(c, 0x5f700c, 0x1, 2);
	dma(c, 1, 0x40);
	tc_reset(c);
	dma(c, 1, 0x40);
	print_id(c);
	tc_close(c);
}

int main(void)
{
	seed = 1;
	flat(T_FLAT);
	flat(T_ARCHIVED);
	m2();
	m1();
	m4();
	aw();
	printf("done\n");
	return 0;
}
