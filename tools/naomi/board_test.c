/* What core/hw/naomi/naomi.c keeps of a NAOMI and an AtomisWave, by
 * itself: the two serial EEPROMs, the main board's and the ROM board's,
 * clocked by the game a bit at a time; the DIMM board's old stand-in; the
 * cartridge's registers and its transfer, a piece at a time on the
 * scheduler; an AtomisWave's coin and device registers; the game's devices,
 * chosen by its name or its set's, polled, and their part of a save state;
 * their files' names; and each of naomi.c's parts of a save state. What it
 * does is printed, for board_expected.txt (sh tools/naomi/board_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "paths.h"
#include "hw/naomi/naomi.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/naomi_periph.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/modules/modules.h"
#include "libretro/pad_state.h"
#include "hw/maple/maple_devs.h"
#include "streams/file_stream.h"
#include <retro_atomic.h>

#ifdef BOARD_TEST_OLD
/* (the parts of the old naomi.cpp that were C++'s, through a shim) */
#define naomi_serialize_v21 board_old_serialize_v21
#define naomi_unserialize_v21 board_old_unserialize_v21
#define naomi_state_before_v21 board_old_state_before_v21
#define naomi_serialize_v23 board_old_serialize_v23
#define naomi_unserialize_v23 board_old_unserialize_v23
#define naomi_state_before_v23 board_old_state_before_v23
void board_old_serialize_v21(void **data, unsigned int *total_size);
bool board_old_unserialize_v21(void **data, unsigned int *total_size);
void board_old_state_before_v21(void);
void board_old_serialize_v23(void **data, unsigned int *total_size);
bool board_old_unserialize_v23(void **data, unsigned int *total_size);
void board_old_state_before_v23(void);
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
settings_t settings;
char eeprom_file[4096];
char naomi_game_id[33];
cartridge *CurrentCartridge;
RegisterStruct sb_regs[SB_REGS];
u32 kcode[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
static u32 arcade_kcode[2] = { 0xffffffff, 0xffffffff };
u32 mcfg_ArcadeKcode(int port) { return arcade_kcode[port]; }
void asic_RaiseInterrupt(int inter) { printf("  raise %x\n", inter); }
void cryptoReset(void) { printf("  crypto reset\n"); }
bool naomi_gd_serial_in_v25(void) { return false; }

static int m3comm_range;
u32 m3comm_read(u32 addr, u32 size) { printf("  m3comm read %x %u\n", addr, size); return 0x1234; }
void m3comm_write(u32 addr, u32 data, u32 size) { printf("  m3comm write %x %x %u\n", addr, data, size); }
bool m3comm_dma_start(u32 addr, u32 data) { printf("  m3comm dma %x %x\n", addr, data); return m3comm_range; }
void m3comm_reset(void) { printf("  m3comm reset\n"); }
void m3comm_term(void) { printf("  m3comm term\n"); }
void m3comm_serialize(void **data, unsigned int *total_size)
{
	static const u8 x[3] = { 1, 2, 3 };
	ra_serialize(x, 3, data, total_size);
}
bool m3comm_unserialize(void **data, unsigned int *total_size)
{
	u8 x[3];
	ra_unserialize(x, 3, data, total_size);
	return x[0] == 1;
}
void naomi_jvs_serialize_v23(void **data, unsigned int *total_size)
{
	static const u8 x[2] = { 4, 5 };
	ra_serialize(x, 2, data, total_size);
}
bool naomi_jvs_unserialize_v23(void **data, unsigned int *total_size)
{
	u8 x[2];
	ra_unserialize(x, 2, data, total_size);
	return true;
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
static size_t state_limit = (size_t)-1;
static const u8 *state_base;
bool ra_unserialize(void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	if (*dest != NULL && src_size != 0)
	{
		if ((size_t)((u8 *)*dest - state_base) + src_size > state_limit)
		{
			memset(src, 0, src_size);
			*total_size += src_size;
			return false;
		}
		memcpy(src, *dest, src_size);
		*dest = (u8 *)*dest + src_size;
	}
	*total_size += src_size;
	return true;
}

/* The scheduler */
sched_list sch_list[SH4_SCHED_SLOTS];
static int registered;
static u32 now = 5000;
u32 sh4_sched_now(void) { return now; }
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
	sch_list[id].start = cycles == -1 ? -1 : (int)now;
	sch_list[id].end = cycles == -1 ? -1 : (int)(now + cycles);
}
bool sh4_sched_is_scheduled(int id) { return sch_list[id].end != -1; }
void sh4_sched_ffts(void) { printf("  ffts\n"); }

/* The registers' handlers */
static RegWriteAddrFP *gdst_write, *gden_write;
void sb_rio_register(u32 reg_addr, RegIO flags, RegReadAddrFP *rf, RegWriteAddrFP *wf)
{
	(void)rf;
	printf("  rio %x %d\n", reg_addr, (int)flags);
	if (reg_addr == SB_GDST_addr)
		gdst_write = wf;
	else if (reg_addr == SB_GDEN_addr)
		gden_write = wf;
}

/* The NAOMI's memory: what the transfer writes */
static u32 written = 2166136261u, written_bytes;
void WriteMemBlock_nommu_ptr(u32 dst, u32 *src, u32 size)
{
	written = (written ^ dst) * 16777619u;
	written = (written ^ hash(src, size)) * 16777619u;
	written_bytes += size;
	/* (a transfer onto the register that stops it) */
	if (dst <= 0x10000 && dst + size > 0x10000)
		SB_GDST = 0;
}

/* The game's devices */
static int pipe_there;
serial_pipe *scif_get_pipe(void) { return pipe_there ? (serial_pipe *)&pipe_there : NULL; }
#define DEVICE_STATE(name, n) \
	void name##_serialize_v24(void **data, unsigned int *total_size) \
	{ static u8 x[n]; memset(x, n & 0xff, n); ra_serialize(x, n, data, total_size); } \
	bool name##_unserialize_v24(void **data, unsigned int *total_size) \
	{ static u8 x[n]; printf("  " #name " state\n"); return ra_unserialize(x, n, data, total_size); } \
	void name##_state_before_v24(void) { printf("  " #name " before\n"); }
DEVICE_STATE(scif, 45)
DEVICE_STATE(touchscreen, 60)
DEVICE_STATE(hopper, 2404)
DEVICE_STATE(rfid, 262)
static int card_readers, barcode, touch, hopper;
void card_reader_serialize_v24(void **data, unsigned int *total_size)
{
	static u8 x[2] = { 7, 7 };
	ra_serialize(x, 2, data, total_size);
}
bool card_reader_unserialize_v24(void **data, unsigned int *total_size)
{
	u8 x[2];
	printf("  card reader state\n");
	return ra_unserialize(x, 2, data, total_size);
}
void card_reader_state_before_v24(void) { printf("  card reader before\n"); }
void card_reader_initd_init(void) { printf("  card reader initd\n"); card_readers = 1; }
void card_reader_clubk_init(void) { printf("  card reader clubk\n"); card_readers = 1; }
void card_reader_derby_init(void) { printf("  card reader derby\n"); card_readers = 1; }
void card_reader_barcode_init(void) { printf("  barcode reader\n"); barcode = 1; }
void card_reader_term(void) { card_readers = 0; }
void card_reader_barcode_term(void) { barcode = 0; }
bool card_reader_available(void) { return card_readers != 0; }
bool card_reader_barcode_available(void) { return barcode != 0; }
void card_reader_insert_card(int player) { printf("  insert card %d\n", player); }
void touchscreen_init(void) { printf("  touchscreen\n"); touch = 1; }
void touchscreen_term(void) { touch = 0; }
bool touchscreen_active(void) { return touch != 0; }
void hopper_init(void) { printf("  hopper\n"); hopper = 1; }
void hopper_term(void) { hopper = 0; }
bool hopper_active(void) { return hopper != 0; }
void hopper_poll(void) { if (hopper) printf("  hopper poll\n"); }
void printer_init(void) { printf("  printer\n"); }
void printer_term(void) { }
void rfid_init(bool game, bool insert) { if (game) printf("  rfid %d\n", insert); }
void save_writer_drain(void) { }
int save_writer_put_file(const char *path, const void *data, size_t len, retro_atomic_int_t *failed)
{
	(void)failed;
	printf("  save %s %u %08x\n", path, (unsigned)len, hash(data, len));
	return path[strlen(path) - 1] != 'x';
}

/* The devices' files */
RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	(void)mode;
	(void)hints;
	printf("  open %s\n", path);
	return strstr(path, "there") != NULL ? (RFILE *)path : NULL;
}
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
	(void)stream;
	memset(data, 0x5a, (size_t)(len > 10 ? 10 : len));
	return len > 10 ? 10 : len;
}
int filestream_close(RFILE *stream) { (void)stream; return 0; }

/* A cartridge: its registers, and what a transfer reads of it */
static u8 cart_rom[0x3000];
static u32 cart_dma;
static u32 c_read_mem(cartridge *c, u32 a, u32 s) { (void)c; printf("  cart read %x %u\n", a, s); return 0x55; }
static void c_write_mem(cartridge *c, u32 a, u32 d, u32 s) { (void)c; printf("  cart write %x %x %u\n", a, d, s); }
static void *c_get_dma_ptr(cartridge *c, u32 *size)
{
	(void)c;
	if (cart_dma >= sizeof(cart_rom))
	{
		*size = 0;
		return NULL;
	}
	/* (in pieces of 0x300 at most: the end of a bank) */
	if (*size > 0x300)
		*size = 0x300;
	if (*size > sizeof(cart_rom) - cart_dma)
		*size = sizeof(cart_rom) - cart_dma;
	return cart_rom + cart_dma;
}
static void c_advance(cartridge *c, u32 size) { (void)c; cart_dma += size; }
static void c_reset(cartridge *c) { (void)c; printf("  cart reset\n"); }
static void c_v21(cartridge *c, void **data, unsigned int *total_size)
{
	static const u8 x = 9;
	(void)c;
	ra_serialize(&x, 1, data, total_size);
}
static void c_un_v21(cartridge *c, void **data, unsigned int *total_size)
{
	u8 x;
	(void)c;
	ra_unserialize(&x, 1, data, total_size);
	printf("  cart v21 %u\n", x);
}
static const cartridge_ops c_ops = {
	NULL, c_reset, c_read_mem, c_write_mem, NULL, NULL, NULL, c_get_dma_ptr, c_advance, NULL,
	NULL, NULL, c_v21, c_un_v21, NULL, NULL, NULL, NULL, NULL
};
static cartridge cart = { &c_ops };

/* The serial EEPROMs, clocked as a game does: bit 2 the clock, bit 3
 * the data, bit 4 reset, bit 5 chip select (the ROM board's: 1, 0, 3 and
 * 2) */
static void board_bits(int rom, u8 cs, u8 rst, u8 scl, u8 sda)
{
	if (rom)
		NaomiGameIDWrite((cs ? 4 : 0) | (rst ? 8 : 0) | (scl ? 2 : 0) | (sda ? 1 : 0));
	else
		NaomiBoardIDWrite((cs ? 0x20 : 0) | (rst ? 0x10 : 0) | (scl ? 4 : 0) | (sda ? 8 : 0));
}
static int read_bit(int rom)
{
	return rom ? NaomiGameIDRead() : (NaomiBoardIDRead() >> 3) & 1;
}

/* Reset, and the first 4 bits of the answer to it; then a read command
 * with the read password, and @n bytes */
static void serial_read(int rom, int n)
{
	static const u8 password[8] = { 0x69, 0x79, 0x68, 0x6b, 0x74, 0x6d, 0x68, 0x6d };
	int i, b;
	u32 got = 0;
	printf("  serial %s:", rom ? "rom" : "main");
	/* reset: RST high with CS low, a clock, RST low */
	board_bits(rom, 0, 0, 0, 0);
	board_bits(rom, 0, 1, 0, 0);
	board_bits(rom, 0, 1, 1, 0);
	for (b = 0; b < 32; b++)
	{
		board_bits(rom, 0, 1, 0, 0);
		got = (got << 1) | (u32)read_bit(rom);
		board_bits(rom, 0, 1, 1, 0);
	}
	board_bits(rom, 0, 0, 0, 0);
	printf(" reset %08x", got);
	/* start: SDA falls while SCL is high, CS low */
	board_bits(rom, 1, 0, 1, 1);
	board_bits(rom, 0, 0, 1, 1);
	board_bits(rom, 0, 0, 1, 0);
	{
		u8 bytes[9];
		bytes[0] = 0x81;	/* read, from 0 */
		memcpy(bytes + 1, password, 8);
		for (i = 0; i < 9; i++)
		{
			for (b = 7; b >= 0; b--)
			{
				board_bits(rom, 0, 0, 0, (bytes[i] >> b) & 1);
				board_bits(rom, 0, 0, 1, (bytes[i] >> b) & 1);
			}
			/* ack */
			board_bits(rom, 0, 0, 0, 1);
			got = (u32)read_bit(rom);
			board_bits(rom, 0, 0, 1, 1);
			printf(" %d", (int)got);
		}
	}
	/* the bytes, an ack after each */
	got = 2166136261u;
	for (i = 0; i < n; i++)
	{
		u8 v = 0;
		for (b = 0; b < 8; b++)
		{
			board_bits(rom, 0, 0, 0, 1);
			v = (u8)((v << 1) | read_bit(rom));
			board_bits(rom, 0, 0, 1, 1);
		}
		board_bits(rom, 0, 0, 0, 0);
		board_bits(rom, 0, 0, 1, 0);
		got = (got ^ v) * 16777619u;
	}
	printf(" data %08x\n", got);
	/* stop, and chip select off */
	board_bits(rom, 0, 0, 0, 0);
	board_bits(rom, 0, 0, 1, 0);
	board_bits(rom, 0, 0, 1, 1);
	board_bits(rom, 1, 0, 1, 1);
}

static u8 state[8192];

static unsigned save(void (*f)(void **, unsigned int *), const char *what)
{
	void *p = state;
	unsigned int total = 0;
	f(&p, &total);
	printf("  %s %u %08x\n", what, total, hash(state, total));
	return total;
}

static void load(bool (*f)(void **, unsigned int *), const char *what, size_t limit)
{
	void *p = state;
	unsigned int total = 0;
	bool ok;
	state_base = state;
	state_limit = limit;
	ok = f(&p, &total);
	state_limit = (size_t)-1;
	printf("  %s read %d %u\n", what, ok, total);
}

static void regs(void)
{
	printf("  gdst %u gden %u gdstard %x gdlend %x sched %d %d\n", SB_GDST, SB_GDEN, SB_GDSTARD, SB_GDLEND,
			(int)sch_list[0].start, (int)sch_list[0].end);
}

static void run_dma(void)
{
	int i;
	for (i = 0; i < 40 && sch_list[0].end != -1; i++)
	{
		int next;
		now = (u32)sch_list[0].end;
		sch_list[0].start = sch_list[0].end = -1;
		next = sch_list[0].cb(sch_list[0].tag, 0, 0);
		printf("  piece %d\n", next);
		if (next > 0)
		{
			sch_list[0].start = (int)now;
			sch_list[0].end = (int)(now + next);
		}
	}
	printf("  written %u %08x\n", written_bytes, written);
	regs();
}

static void dma(u32 to, u32 len)
{
	SB_GDSTAR = to;
	SB_GDLEN = len;
	SB_GDDIR = 1;
	gdst_write(SB_GDST_addr, 1);
	regs();
	run_dma();
}

static void periph_game(const char *id, const char *set)
{
	strcpy(naomi_game_id, id);
	strcpy(naomi_set_name, set);
	printf("periph [%s] [%s]\n", id, set);
	naomi_periph_init();
	printf("  card %d rfid %d\n", naomi_periph_has_card_reader(), naomi_periph_rfid_game());
}

int main(void)
{
	static const char *const games[][2] = {
		{ "INITIAL D Ver.3", "initdv3j" },
		{ "CLUB KART IN JAPAN", "clubkrt" },
		{ "CLUB KART IN JAPAN", "clubkprz" },
		{ " DERBY OWNERS CLUB II-----------", "derbyoc2" },
		{ "MUSHIKING II", "mushi2k5" },
		{ "MKG TKOB 2K3 2ND VER1.003-", "mushike" },
		{ "VF4 EVOLUTION JAPAN", "vf4evo" },
		{ "TOUCH DE ZUNO (JAPAN)", "zunou" },
		{ " TOUCH DE UNOH 2 -----------", "tduno2" },
		{ "SHOOTOUT POOL", "shootplm" },
		{ "KICK '4' CASH", "kick4csh" },
		{ "OTHER GAME", "other" },
	};
	static u8 rom_serial[0x84];
	unsigned i, n;

	settings.System = DC_PLATFORM_NAOMI;
	for (i = 0; i < sizeof(cart_rom); i++)
		cart_rom[i] = (u8)(i * 7 + (i >> 8));
	for (i = 0; i < sizeof(rom_serial); i++)
		rom_serial[i] = (u8)(i ^ 0x3c);

	printf("init\n");
	naomi_reg_Init();
	strcpy(naomi_game_id, "SOME GAME");
	naomi_reg_Reset(true);

	printf("serial\n");
	naomi_serial_id_load(NULL);
	serial_read(0, 8);
	serial_read(1, 0x20);
	printf("  rom data %08x\n", hash(naomi_rom_serial_data(), 0x10));
	naomi_serial_id_load(rom_serial);
	serial_read(1, 0x20);
	printf("  rom data %08x\n", hash(naomi_rom_serial_data(), 0x10));
	n = save(naomi_serial_serialize_v26, "v26");
	{
		void *p = state;
		unsigned int total = 0;
		naomi_rom_serial_serialize(&p, &total);
		printf("  rom serial %u %08x\n", total, hash(state, total));
		p = state;
		total = 0;
		naomi_rom_serial_unserialize(&p, &total);
		naomi_rom_serial_reset();
	}
	n = save(naomi_serial_serialize_v26, "v26");
	load(naomi_serial_unserialize_v26, "v26", n);
	naomi_serial_state_before_v26();
	serial_read(0, 4);
	printf("  legacy %u %d %u\n", GSerialBuffer, SerStep, (unsigned)GSerial[3]);

	printf("dimm\n");
	for (i = 0; i < 14; i++)
		naomi_process(0x1e03 + (i == 3 ? 0xfc : 0), 0, 0, 0);
	printf("  command %x\n", reg_dimm_command);

	printf("registers\n");
	printf("  read %x\n", ReadMem_naomi(0x5f7000, 2));
	CurrentCartridge = &cart;
	printf("  read %x\n", ReadMem_naomi(0x5f7000, 2));
	printf("  read %x\n", ReadMem_naomi(0x5f7018, 4));
	printf("  read %x\n", ReadMem_naomi(0x5f702c, 4));
	WriteMem_naomi(0x5f7028, 3, 4);
	WriteMem_naomi(0x5f7030, 3, 2);
	settings.System = DC_PLATFORM_ATOMISWAVE;
	printf("  read %x\n", ReadMem_naomi(0x5f7018, 4));
	WriteMem_naomi(0x5f7020, 3, 4);
	settings.System = DC_PLATFORM_NAOMI;
	CurrentCartridge = NULL;
	WriteMem_naomi(0x5f7000, 3, 2);

	printf("dma\n");
	CurrentCartridge = &cart;
	gden_write(SB_GDEN_addr, 1);
	dma(0x0c100000, 0x1000);
	/* a length that is not whole lines: to the next 32 bytes */
	cart_dma = 0;
	dma(0x0c200000, 0x41);
	/* a last piece shorter than the others */
	cart_dma = 0;
	dma(0x0c280000, 0x1100);
	/* past the end of the cartridge: zeroes */
	cart_dma = 0x2800;
	dma(0x0c300000, 0x1000);
	/* stopped by what it writes */
	cart_dma = 0;
	dma(0x0000fc00, 0x1000);
	/* disabled while under way; not enabled */
	cart_dma = 0;
	SB_GDSTAR = 0x0c400000;
	SB_GDLEN = 0x2000;
	gdst_write(SB_GDST_addr, 1);
	regs();
	gden_write(SB_GDEN_addr, 0);
	regs();
	gdst_write(SB_GDST_addr, 1);
	regs();
	/* the communication board's */
	gden_write(SB_GDEN_addr, 1);
	m3comm_range = 1;
	SB_GDSTAR = 0x0c500000;
	SB_GDLEN = 0x100;
	gdst_write(SB_GDST_addr, 1);
	regs();
	m3comm_range = 0;
	/* with no cartridge */
	CurrentCartridge = NULL;
	gdst_write(SB_GDST_addr, 1);
	regs();
	CurrentCartridge = &cart;
	/* a state taken during one */
	cart_dma = 0;
	SB_GDSTAR = 0x0c600000;
	SB_GDLEN = 0x2000;
	gdst_write(SB_GDST_addr, 1);
	now += 20000;
	n = save(naomi_serialize_v21, "v21");
	load(naomi_unserialize_v21, "v21", n);
	regs();
	sch_list[0].end = -1;
	load(naomi_unserialize_v21, "v21", n);
	regs();
	naomi_state_before_v21();
	regs();
	SB_GDST = 0;
	load(naomi_unserialize_v21, "v21", n);
	regs();

	printf("atomiswave\n");
	settings.System = DC_PLATFORM_ATOMISWAVE;
	strcpy(naomi_game_id, "SOME GAME");
	naomi_reg_Reset(false);
	regs();
	kcode[1] &= ~AWAVE_COIN_KEY;
	kcode[3] &= ~AWAVE_COIN_KEY;
	for (i = 0; i < 8; i++)
		printf("  coins %x\n", libExtDevice_ReadMem_A0_006(0x00600280, 4));
	kcode[1] |= AWAVE_COIN_KEY;
	printf("  coins %x\n", libExtDevice_ReadMem_A0_006(0x00600280, 4));
	libExtDevice_WriteMem_A0_006(0x00600284, 0x5a, 4);
	libExtDevice_WriteMem_A0_006(0x00600288, 0x5a, 4);
	printf("  devices %x %x\n", libExtDevice_ReadMem_A0_006(0x00600284, 4), libExtDevice_ReadMem_A0_006(0x00600290, 4));
	n = save(naomi_serialize_v21, "v21");
	/* (the devices, and a coin switch read as closed too long) */
	state[8] = 0x33;
	state[14] = 9;
	naomi_reg_Reset(false);
	load(naomi_unserialize_v21, "v21", n);
	printf("  devices %x coins %x %x\n", libExtDevice_ReadMem_A0_006(0x00600284, 4),
			libExtDevice_ReadMem_A0_006(0x00600280, 4), libExtDevice_ReadMem_A0_006(0x00600280, 4));
	n = save(naomi_serialize_v21, "v21");
	n = save(naomi_serialize_v23, "v23");
	settings.System = DC_PLATFORM_DREAMCAST;
	n = save(naomi_serialize_v21, "v21");
	load(naomi_unserialize_v21, "v21", 0);
	naomi_state_before_v21();
	n = save(naomi_serial_serialize_v26, "v26");
	settings.System = DC_PLATFORM_NAOMI;
	strcpy(naomi_game_id, "KENJU");
	naomi_reg_Reset(true);
	gden_write(SB_GDEN_addr, 1);
	cart_dma = 0;
	dma(0x0c700000, 0x800);
	n = save(naomi_serialize_v23, "v23");
	load(naomi_unserialize_v23, "v23", n);
	state[0] = 0;
	load(naomi_unserialize_v23, "v23", n);
	naomi_state_before_v23();

	naomi_card_readers_enabled = true;
	for (i = 0; i < sizeof(games) / sizeof(games[0]); i++)
	{
		periph_game(games[i][0], games[i][1]);
		arcade_kcode[0] &= ~NAOMI_INSERT_CARD_KEY;
		naomi_periph_poll();
		naomi_periph_poll();
		arcade_kcode[0] |= NAOMI_INSERT_CARD_KEY;
		arcade_kcode[1] &= ~NAOMI_INSERT_CARD_KEY;
		naomi_periph_poll();
		arcade_kcode[1] |= NAOMI_INSERT_CARD_KEY;
		pipe_there = i & 1;
		n = save(naomi_periph_serialize_v24, "v24");
		load(naomi_periph_unserialize_v24, "v24", n);
		load(naomi_periph_unserialize_v24, "v24", n - 1);
	}
	/* a state with devices this game has not got, and one with none */
	periph_game("VF4 EVOLUTION JAPAN", "vf4evo");
	pipe_there = 1;
	n = save(naomi_periph_serialize_v24, "v24");
	periph_game("OTHER GAME", "other");
	pipe_there = 0;
	load(naomi_periph_unserialize_v24, "v24", n);
	state[0] = 0x40;
	load(naomi_periph_unserialize_v24, "v24", n);
	state[0] = 0;
	load(naomi_periph_unserialize_v24, "v24", n);
	naomi_periph_state_before_v24();
	naomi_card_readers_enabled = false;
	periph_game("INITIAL D Ver.3", "initdv3j");
	settings.System = DC_PLATFORM_ATOMISWAVE;
	periph_game("INITIAL D Ver.3", "initdv3j");
	n = save(naomi_periph_serialize_v24, "v24");
	settings.System = DC_PLATFORM_NAOMI;

	printf("files\n");
	{
		static u8 buf[16];
		retro_atomic_int_t failed;
		retro_atomic_int_init(&failed, 1);
		printf("  loaded %u\n", (unsigned)periph_load_file(".card", buf, sizeof(buf)));
		strcpy(eeprom_file, "/saves/game.zip.eeprom");
		printf("  loaded %u\n", (unsigned)periph_load_file(".card", buf, sizeof(buf)));
		strcpy(eeprom_file, "/saves/there.eeprom");
		printf("  loaded %u %08x\n", (unsigned)periph_load_file(".card", buf, sizeof(buf)), hash(buf, sizeof(buf)));
		strcpy(eeprom_file, "/saves/there.nvmem");
		printf("  loaded %u\n", (unsigned)periph_load_file(".card", buf, 4));
		periph_save_file(".card", buf, 8, &failed);
		periph_save_file(".x", buf, 8, &failed);
		printf("  failed %d\n", retro_atomic_load_acquire_int(&failed));
		eeprom_file[0] = '\0';
		periph_save_file(".card", buf, 8, &failed);
	}
	printf("sched\n");
	periph_sched_restore(-1, 0, 0);
	periph_sched_restore(3, 100, 200);
	printf("  %d %d\n", (int)sch_list[3].start, (int)sch_list[3].end);
	periph_sched_restore(3, 100, 100 + SH4_MAIN_CLOCK + 1);
	printf("  %d %d\n", (int)sch_list[3].start, (int)sch_list[3].end);
	periph_sched_restore(3, (int)now - 50, (int)now - 10);
	printf("  %d %d\n", (int)sch_list[3].start, (int)sch_list[3].end);
	periph_sched_restore(3, 0, -1);
	printf("  %d %d\n", (int)sch_list[3].start, (int)sch_list[3].end);

	printf("term\n");
	naomi_reg_Term();
	regs();
	printf("done\n");
	return 0;
}
