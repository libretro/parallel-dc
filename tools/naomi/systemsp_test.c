/*
 * The System SP's ROM board (core/hw/naomi/systemsp.c), by itself, with
 * the M4 board it is (m4cartridge.c, cartridge.c): its area 0 registers
 * (SRAM, the CompactFlash controller and its decrypting window, the
 * interrupts, each kind of cabinet's I/O ports, the serial EEPROM, the
 * UARTs), the game's header from a CompactFlash card's FAT16 partition
 * or the ROM, the flash's commands, the network board's commands, the
 * region and EEPROM files, and the save state from V24 and V25. What it
 * prints must be what it always was (systemsp_expected.txt).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <streams/file_stream.h>
#include "types.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/naomi_periph.h"
#include "hw/naomi/systemsp_serial.h"
#include "hw/naomi/dinokich.h"
#include "hw/flashrom/flashrom.h"
#include "hw/maple/maple_devs.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_mem.h"
#include "imgread/chd_image.h"
#include "savewriter.h"
#include "paths.h"

#ifdef SP_TEST_OLD
/* (the board as a class was, through a shim) */
#define systemsp_serialize_v24 sp_old_serialize_v24
#define systemsp_unserialize_v24 sp_old_unserialize_v24
#define systemsp_state_before_v24 sp_old_state_before_v24
#define systemsp_serialize_v25 sp_old_serialize_v25
#define systemsp_unserialize_v25 sp_old_unserialize_v25
#define systemsp_state_before_v25 sp_old_state_before_v25
void sp_old_serialize_v24(void **data, unsigned int *total_size);
bool sp_old_unserialize_v24(void **data, unsigned int *total_size);
void sp_old_state_before_v24(void);
void sp_old_serialize_v25(void **data, unsigned int *total_size);
bool sp_old_unserialize_v25(void **data, unsigned int *total_size);
void sp_old_state_before_v25(void);
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

/*
 * The machine around the board
 */
settings_t settings;
u32 kcode[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
s8 joyx[4], joyy[4];
InputDescriptors *naomi_game_inputs;
u8 *naomi_default_eeprom;
char g_base_name[128];
char g_roms_dir[PATH_MAX_LENGTH];
char g_parent_name[128];
char eeprom_file[PATH_MAX_LENGTH];
char nvmem_file[PATH_MAX_LENGTH];
unsigned BBSRAM_SIZE;
unsigned RAM_MASK = 0xffff;
static u8 ram[0x10000];
VArray2 mem_b = { ram, sizeof(ram) };
RegisterStruct sb_regs[SB_REGS];
MemChip sys_nvmem_sram;

void memchip_allocate(MemChip *chip, u32 size)
{
	free(chip->data);
	chip->data = (u8 *)calloc(1, size);
	chip->size = size;
	chip->mask = size - 1;
	printf("  sram %u\n", size);
}
bool memchip_load(MemChip *chip, const char *file)
{
	u32 i;
	for (i = 0; i < chip->size; i++)
		chip->data[i] = (u8)(i * 7 + 3);
	printf("  sram load %s\n", file);
	return true;
}
u32 memchip_read(MemChip *chip, u32 addr, u32 sz)
{
	(void)sz;
	return chip->data[addr & chip->mask];
}
void memchip_write(MemChip *chip, u32 addr, u32 val, u32 sz)
{
	(void)sz;
	chip->data[addr & chip->mask] = (u8)val;
}

void asic_CancelInterrupt(int inter) { printf("  cancel %x\n", inter); }
void asic_RaiseInterrupt(int inter) { printf("  raise %x\n", inter); }
void naomi_periph_poll(void) { printf("  poll\n"); }

/* what the board's NAOMI part does (cartridge.c) */
u32 reg_dimm_command, reg_dimm_offsetl, reg_dimm_parameterl, reg_dimm_parameterh, reg_dimm_status;
u16 NaomiGameIDRead(void) { return 0; }
void NaomiGameIDWrite(u16 data) { printf("  game id write %x\n", data); }
void naomi_process(u32 command, u32 offsetl, u32 parameterl, u32 parameterh)
{
	printf("  naomi_process %x %x %x %x\n", command, offsetl, parameterl, parameterh);
}
bool mem_region_release(void *start, size_t len) { (void)start; (void)len; return true; }
void archive_close(void *a) { (void)a; }

static u64 now64 = 1000000;
/* (the board registers its callback each time it is opened) */
static sh4_sched_callback *callback;
static int registered;
u64 sh4_sched_now64(void) { return now64; }
u32 sh4_sched_now(void) { return (u32)now64; }
int sh4_sched_register(int tag, sh4_sched_callback *ssc)
{
	(void)tag;
	callback = ssc;
	return registered++;
}
void sh4_sched_request(int id, int cycles) { printf("  sched %d %d\n", id, cycles); }

bool ra_serialize(const void *src, unsigned int size, void **dest, unsigned int *total)
{
	if (*dest != NULL)
	{
		memcpy(*dest, src, size);
		*dest = (u8 *)*dest + size;
	}
	*total += size;
	return true;
}
bool ra_unserialize(void *dst, unsigned int size, void **src, unsigned int *total)
{
	memcpy(dst, *src, size);
	*src = (u8 *)*src + size;
	*total += size;
	return true;
}

/* what the board saves */
static u32 saves;
int save_writer_put_file(const char *path, const void *data, size_t len, retro_atomic_int_t *failed)
{
	(void)failed;
	printf("  save %s %u %08x\n", path, (unsigned)len, hash(data, len));
	return ++saves % 5 != 0;
}
void save_writer_drain(void) { printf("  drain\n"); }

/* The files there are: one at most, its path and contents */
struct RFILE
{
	int write;
	u32 at;
};
static RFILE the_file;
static char file_path[PATH_MAX_LENGTH];
static u8 *file_data;
static u32 file_size;
static bool file_bad;

static void file_set(const char *path, const u8 *data, u32 size)
{
	strcpy(file_path, path);
	free(file_data);
	file_data = (u8 *)malloc(size + 1);
	memcpy(file_data, data, size);
	file_size = size;
}

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	(void)hints;
	printf("  open %s %u\n", path, mode);
	if (mode == RETRO_VFS_FILE_ACCESS_WRITE)
	{
		if (file_bad)
			return NULL;
		the_file.write = 1;
		return &the_file;
	}
	if (strcmp(path, file_path) != 0)
		return NULL;
	the_file.write = 0;
	the_file.at = 0;
	return &the_file;
}
int64_t filestream_get_size(RFILE *stream) { (void)stream; return file_size; }
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
	int64_t n = len;
	if (stream->at + n > file_size)
		n = file_size - stream->at;
	memcpy(data, file_data + stream->at, (size_t)n);
	stream->at += (u32)n;
	return n;
}
int64_t filestream_write(RFILE *stream, const void *data, int64_t len)
{
	(void)stream;
	printf("  write %u %08x\n", (unsigned)len, hash(data, (size_t)len));
	return len;
}
int filestream_close(RFILE *stream) { (void)stream; printf("  close\n"); return 0; }

/*
 * The serial devices: what they are sent, and what they have to send
 */
typedef struct fake_pipe
{
	sp_pipe base;
	const char *what;
	u8 q[8];
	int n;
	u32 stateSize;
	u8 state;
} fake_pipe;

static fake_pipe pipes[3];

static void fp_write(sp_pipe *p, u8 v) { printf("  %s <- %02x\n", ((fake_pipe *)p)->what, v); }
static int fp_available(sp_pipe *p) { return ((fake_pipe *)p)->n; }
static u8 fp_read(sp_pipe *p)
{
	fake_pipe *f = (fake_pipe *)p;
	u8 v;
	if (f->n == 0)
		return 0xee;
	v = f->q[0];
	memmove(f->q, f->q + 1, sizeof(f->q) - 1);
	f->n--;
	return v;
}
static void fp_reset(sp_pipe *p) { printf("  %s reset\n", ((fake_pipe *)p)->what); }
static void fp_serialize(sp_pipe *p, void **data, unsigned int *total_size)
{
	fake_pipe *f = (fake_pipe *)p;
	u32 i;
	for (i = 0; i < f->stateSize; i++)
	{
		u8 b = (u8)(f->state + i);
		LIBRETRO_S(b);
	}
}
static void fp_unserialize(sp_pipe *p, void **data, unsigned int *total_size)
{
	fake_pipe *f = (fake_pipe *)p;
	u32 i;
	u8 b = 0;
	for (i = 0; i < f->stateSize; i++)
		LIBRETRO_US(b);
	printf("  %s state %02x\n", f->what, b);
}
static void fp_close(sp_pipe *p) { printf("  %s close\n", ((fake_pipe *)p)->what); }

static const sp_pipe_ops fp_ops = {
	fp_write, fp_available, fp_read, fp_reset, fp_serialize, fp_unserialize, fp_close
};

static sp_pipe *fp_open(int i, const char *what, u32 stateSize, void (*update_status)(void *port), void *port)
{
	fake_pipe *f = &pipes[i];
	memset(f, 0, sizeof(*f));
	f->base.ops = &fp_ops;
	f->base.update_status = update_status;
	f->base.port = port;
	f->what = what;
	f->stateSize = stateSize;
	f->state = (u8)(0x40 * i + 1);
	f->q[0] = (u8)(0x10 * i + 1);
	f->q[1] = (u8)(0x10 * i + 2);
	f->n = 2;
	return &f->base;
}

sp_pipe *sp_rfid_open(int index, const char *gameName, int region, const char *path,
		void (*update_status)(void *port), void *port)
{
	printf("  rfid %d %s %d %s\n", index, gameName, region, path);
	return fp_open(index, index == 1 ? "rfid1" : "rfid2", 100 * index, update_status, port);
}

sp_pipe *sp_touchscreen_open(void (*update_status)(void *port), void *port)
{
	printf("  touchscreen\n");
	return fp_open(1, "touch", 0, update_status, port);
}

static int dinokich_reader_dummy;
dinokich_reader *dinokich_reader_open(const char *gameName, const char *path1, const char *path2,
		void (*update_status)(void *port), void *port)
{
	printf("  dinokich %s %s %s\n", gameName, path1, path2);
	fp_open(0, "dinokich", 767, update_status, port);
	return (dinokich_reader *)&dinokich_reader_dummy;
}
sp_pipe *dinokich_reader_pipe(dinokich_reader *r) { (void)r; return &pipes[0].base; }
void dinokich_reader_close(dinokich_reader *r) { (void)r; printf("  dinokich close\n"); }
void dinokich_reader_serialize_v25(dinokich_reader *r, void **data, unsigned int *total_size)
{
	u32 v = 0xd1d1;
	(void)r;
	LIBRETRO_S(v);
}
void dinokich_reader_unserialize_v25(dinokich_reader *r, void **data, unsigned int *total_size)
{
	u32 v = 0;
	(void)r;
	LIBRETRO_US(v);
	printf("  dinokich v25 %x\n", v);
}
void dinokich_reader_state_before_v25(dinokich_reader *r) { (void)r; printf("  dinokich before v25\n"); }

void sp_save_event(int id, void **data, unsigned int *total_size)
{
	u32 v = 0x5000 + (u32)id;
	LIBRETRO_S(v);
}
void sp_load_event(int id, u32 most, void **data, unsigned int *total_size)
{
	u32 v = 0;
	LIBRETRO_US(v);
	printf("  event %d %u %x\n", id, most, v);
}

/*
 * The CompactFlash card: hunks of 4 KB, one of which cannot be read
 */
#define HUNK 4096
#define CF_HUNKS 4
static u8 cf[HUNK * CF_HUNKS];
static char cf_path[PATH_MAX_LENGTH];
static u32 cf_hunkbytes = HUNK;
static int cf_bad_hunk = -1;
static int cf_handle;

chd_image_t *chd_image_open(const char *path, char *err, size_t err_len)
{
	(void)err;
	(void)err_len;
	printf("  chd open %s\n", path);
	if (strcmp(path, cf_path) != 0)
		return NULL;
	return (chd_image_t *)&cf_handle;
}
void chd_image_close(chd_image_t *img) { (void)img; printf("  chd close\n"); }
uint32_t chd_image_hunk_bytes(const chd_image_t *img) { (void)img; return cf_hunkbytes; }
uint32_t chd_image_hunk_count(const chd_image_t *img) { (void)img; return CF_HUNKS; }
bool chd_image_read_hunk(chd_image_t *img, uint32_t hunk, uint8_t *dst)
{
	(void)img;
	printf("  hunk %u\n", hunk);
	if ((int)hunk == cf_bad_hunk)
		return false;
	memcpy(dst, &cf[hunk * HUNK], HUNK);
	return true;
}

/* The cipher (as m4cartridge.c has it), and the other way */
static const u8 k_sboxes[4][16] = {
	{9,8,2,11,1,14,5,15,12,6,0,3,7,13,10,4},
	{2,10,0,15,14,1,11,3,7,12,13,8,4,9,5,6},
	{4,11,3,8,7,2,15,13,1,5,14,9,6,12,0,10},
	{1,13,8,2,0,5,6,14,4,11,15,10,12,3,7,9}
};
static u16 rnd1[0x10000], inv1[0x10000];
static u16 k1, k2;

static void cipher_init(void)
{
	int x, n, i;
	for (x = 0; x < 0x10000; x++)
	{
		u8 in[4], out[4], aux;
		u16 r = 0;
		for (n = 0; n < 4; n++)
		{
			in[n] = (x >> (n * 4)) & 0xf;
			out[n] = 0;
		}
		aux = in[3];
		for (n = 0; n < 4; n++)
		{
			aux ^= k_sboxes[n][in[n]];
			for (i = 0; i < 4; i++)
				out[(n - i) & 3] |= aux & (1 << i);
		}
		for (n = 0; n < 4; n++)
			r |= (u16)(out[n] << (4 * n));
		rnd1[x] = r;
		inv1[r] = (u16)x;
	}
}

/* @len bytes at @p, which the board decrypts to what they are now */
static void encrypt(u8 *p, u32 len)
{
	u16 iv = 0;
	int counter = 0;
	u32 i;
	for (i = 0; i + 1 < len; i += 2)
	{
		const u16 plain = (u16)(p[i] | (p[i + 1] << 8));
		const u16 newIv = (u16)(inv1[(u16)(plain ^ iv ^ k2)] ^ k2);
		const u16 w = (u16)(inv1[(u16)(newIv ^ k1)] ^ k1 ^ iv);
		p[i] = (u8)w;
		p[i + 1] = (u8)(w >> 8);
		iv = newIv;
		if (++counter == 16)
		{
			counter = 0;
			iv = 0;
		}
	}
}

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }

static void dirent(u8 *e, const char *name11, u8 attr, u32 cluster, u32 size)
{
	memcpy(e, name11, 11);
	e[11] = attr;
	put16(&e[26], cluster);
	put32(&e[28], size);
}

/* The game's header, as the BIOS reads it */
static void header(u8 *h, u8 country, const char *title1, const char *title2)
{
	u32 i;
	for (i = 0; i < 0x500; i++)
		h[i] = (u8)(i * 13 + 5);
	memcpy(h, "SystemSP", 8);
	memset(&h[0x30], ' ', 0x40);
	memcpy(&h[0x30], title1, strlen(title1));
	memcpy(&h[0x50], title2, strlen(title2));
	h[0x428] = country;
}

/* What a card may have wrong, or odd */
#define V_ZERO_ENTRY 1	/* the directory's end before the image */
#define V_CLUSTER1 2	/* the image at cluster 1 */
#define V_PTYPE 4		/* the partition not a FAT16 one */
#define V_SMALL1ST 8	/* 1STREAD.BIN 0x80 bytes */
#define V_SIG 16		/* 1STREAD.BIN's signature wrong */
#define V_SMALLIMG 32	/* the image a byte short */
#define V_NOMBR 64		/* no boot record */

/* A card whose 1STREAD.BIN names @image (its first 128 bytes), whose
 * header is encrypted @twice or not; its partition with 2 sectors a
 * cluster, 1 reserved, 3 FATs of 1 sector, 16 entries in the root: the
 * root at 5, the data at 6 */
static void make_card(const char *image, const char *entry11, int twice, u8 country, const char *title2,
		u32 variant)
{
	u8 *s = cf;
	u8 *dir;
	u8 h[0x500];
	u32 i;
	for (i = 0; i < sizeof(cf); i++)
		cf[i] = (u8)(i * 31 + 7);
	/* master boot record: partition 1 at sector 1 */
	memset(s, 0, 512);
	s[446 + 4] = variant & V_PTYPE ? 0x0b : 6;
	put32(&s[446 + 8], 1);
	s[510] = variant & V_NOMBR ? 0 : 0x55;
	s[511] = 0xaa;
	s = &cf[512];
	memset(s, 0, 512);
	s[13] = 2;
	put16(&s[14], 1);
	s[16] = 3;
	put16(&s[17], 16);
	put16(&s[22], 1);
	dir = &cf[5 * 512];
	memset(dir, 0, 512);
	dirent(&dir[0], "DELETED TXT", 0x20, 9, 9);
	dir[0] = 0xe5;
	/* a long name's entry, and the volume's, as the image is named */
	dirent(&dir[32], entry11, 0x0f, 1, 9);
	dirent(&dir[64], entry11, 0x08, 1, 9);
	dirent(&dir[96], "1STREAD BIN", 0x20, 2, variant & V_SMALL1ST ? 0x80 : 0x200);
	dirent(&dir[128], "OTHER   BIN", 0x20, 7, 0x600);
	if (variant & V_ZERO_ENTRY)
		dir[160] = 0;
	else
		dirent(&dir[160], "PLAIN      ", 0x20, 8, 0x600);
	dirent(&dir[192], entry11, 0x20, variant & V_CLUSTER1 ? 1 : 3, variant & V_SMALLIMG ? 0x4ff : 0x600);
	/* 1STREAD.BIN, at cluster 2 */
	s = &cf[6 * 512];
	for (i = 0; i < 0x100; i++)
		s[i] = (u8)(i ^ 0x5a);
	memcpy(s, variant & V_SIG ? "SPCX" : "SPCF", 4);
	memset(&s[0x80], 0, 0x80);
	memcpy(&s[0x80], image, strlen(image) < 0x80 ? strlen(image) : 0x80);
	encrypt(s, 0x100);
	/* the header, at cluster 3 */
	header(h, country, "FIRST TITLE", title2);
	encrypt(h, sizeof(h));
	if (twice)
		encrypt(h, sizeof(h));
	memcpy(&cf[8 * 512], h, sizeof(h));
}

static cartridge *open_board(const char *name, const char *media, const char *parent, u32 size);
static bool init_board(void);
static void game_id(void);
static void close_board(void);

static void card_variant(const char *what, const char *image, const char *entry11, u32 variant)
{
	printf("card %s\n", what);
	make_card(image, entry11, 0, 0x02, "T", variant);
	open_board("loveber3", "cf", NULL, 0x800000);
	init_board();
	game_id();
	close_board();
}

/*
 * The board
 */
static cartridge *cart;
static u32 rom_size;

static void rd(u32 addr, u32 sz)
{
	const u32 v = systemsp_read_area0(addr, sz);
	printf(" r%u %x = %x\n", sz, addr, v);
}

static void wr(u32 addr, u32 v, u32 sz)
{
	printf(" w%u %x %x\n", sz, addr, v);
	systemsp_write_area0(addr, v, sz);
}

static void fire(void)
{
	printf(" ata ready\n");
	callback(0, 0, 0);
}

static void game_id(void)
{
	char id[CART_GAME_ID_SIZE];
	const char *c;
	memset(id, 0, sizeof(id));
	cart->ops->game_id(cart, id);
	printf(" id '");
	for (c = id; *c != '\0'; c++)
		if (*c >= ' ' && *c <= '~')
			putchar(*c);
		else
			printf("\\x%02x", (u8)*c);
	printf("' region %d\n", systemsp_cart_region());
}

/* The state, hashed */
static u8 state[64 * 1024];
static unsigned state_size;

static void save(const char *what)
{
	void *p = state;
	unsigned int total = 0;
	settings.System = DC_PLATFORM_NAOMI;
	naomi_cart_systemsp = true;
	systemsp_serialize_v24(&p, &total);
	systemsp_serialize_v25(&p, &total);
	state_size = total;
	printf(" state %s %u %08x\n", what, total, hash(state, total));
}

static void load(const char *what)
{
	void *p = state;
	unsigned int total = 0;
	bool ok;
	ok = systemsp_unserialize_v24(&p, &total);
	ok = systemsp_unserialize_v25(&p, &total) && ok;
	printf(" load %s %d %u\n", what, ok, total);
}

static cartridge *open_board(const char *name, const char *media, const char *parent, u32 size)
{
	u8 *key = (u8 *)calloc(1, 2048);
	key[0x5e0] = (u8)k1;
	key[0x5e2] = (u8)(k1 >> 8);
	key[0x5e4] = (u8)k2;
	key[0x5e6] = (u8)(k2 >> 8);
	rom_size = size;
	printf("open %s %s %s\n", name, media != NULL ? media : "-", parent != NULL ? parent : "-");
	cart = systemsp_cart_open(size, name, media, parent);
	naomi_cart_systemsp = true;
	cart->ops->set_key_data(cart, key);
	return cart;
}

static bool init_board(void)
{
	const bool ok = cart->ops->init(cart);
	printf(" init %d %u\n", ok, BBSRAM_SIZE);
	BBSRAM_SIZE = 0;
	return ok;
}

static void close_board(void)
{
	printf("close\n");
	cart->ops->close(cart);
	cart = NULL;
}

/* the EEPROM's pins: one clock with DI as @bit */
static void ee_bit(int bit)
{
	systemsp_write_area0(0x10128, 2 | (bit ? 1 : 0), 1);
	systemsp_write_area0(0x10128, 2 | 4 | (bit ? 1 : 0), 1);
}

static void ee_cmd(u32 bits, int n)
{
	int i;
	for (i = n - 1; i >= 0; i--)
		ee_bit((bits >> i) & 1);
}

static void ee_read(u32 addr)
{
	u32 v = 0;
	int i;
	ee_cmd(0x180 | addr, 9);
	for (i = 0; i < 17; i++)
	{
		v = (v << 1) | ((systemsp_read_area0(0x10128, 1) >> 4) & 1);
		ee_bit(0);
	}
	printf(" eeprom %u = %x\n", addr, v);
}

static void ee_release(void)
{
	printf(" eeprom release\n");
	systemsp_write_area0(0x10128, 0, 1);
}

static void eeprom_tests(void)
{
	printf("eeprom\n");
	ee_read(0);
	ee_read(5);
	ee_cmd(0x1c5, 9);	/* erase 5, not enabled */
	ee_cmd(0x130, 9);	/* enable */
	ee_cmd((0x140 | 5) << 16 | 0xbeef, 25);	/* write */
	ee_read(5);
	ee_cmd(0x1c6, 9);	/* erase 6 */
	ee_read(6);
	ee_bit(0);			/* not a start bit */
	ee_release();
	ee_cmd(0x120, 9);	/* erase all */
	ee_read(1);
	ee_cmd((0x110 << 16) | 0x1234, 25);	/* write all */
	ee_read(7);
	ee_cmd((0x140 | 45) << 16 | 0x4545, 25);
	ee_read(45);
	ee_read(13);
	/* clocked while not selected */
	systemsp_write_area0(0x10128, 1, 1);
	systemsp_write_area0(0x10128, 5, 1);
	systemsp_write_area0(0x10128, 1, 1);
	systemsp_write_area0(0x10128, 5, 1);
	ee_read(7);
	ee_cmd(0x100, 9);	/* disable */
	ee_cmd((0x140 | 9) << 16 | 0x5555, 25);
	ee_read(9);
	/* a command half way: in the state */
	ee_cmd(0x141, 9);
	ee_cmd(0x3, 4);
	ee_release();
}

static void sram_tests(void)
{
	printf("sram\n");
	wr(0x10000, 0x3901, 2);
	rd(0x10, 1);
	rd(0x11, 1);
	rd(0x10, 2);
	rd(0x10, 4);
	wr(0x20, 0xa1b2c3d4, 4);
	wr(0x31, 0x77, 1);
	wr(0x30, 0x66, 1);
	wr(0x40, 0x1234, 2);
	rd(0x20, 4);
	rd(0x30, 2);
	rd(0x40, 1);
	rd(0x10000, 2);
	rd(0x10000, 4);
}

static void ata_tests(int decrypting)
{
	u32 i, h = 2166136261u;
	printf("ata %d\n", decrypting);
	wr(0x10000, 0x3a00, 2);
	rd(0x1c, 1);
	rd(0x1c, 1);
	wr(0x04, 0x1ff, 2);
	wr(0x08, 0x102, 2);
	wr(0x0c, 0xff, 1);
	wr(0x10, 0x3412, 4);
	wr(0x14, 0x00, 1);
	wr(0x18, 0x0, 1);
	rd(0x08, 1);
	rd(0x0c, 2);
	rd(0x10, 1);
	rd(0x14, 4);
	rd(0x18, 1);
	rd(0x04, 1);
	rd(0x20, 2);
	wr(0x20, 0x55, 1);
	/* sectors 7 and 8, in two hunks: 512 bytes each, then nothing */
	wr(0x10, 0x00, 1);
	wr(0x0c, 0x07, 1);
	wr(0x1c, 0x20, 1);
	rd(0x1c, 1);
	fire();
	rd(0x1c, 1);
	rd(0x10080, 1);
	if (decrypting)
		wr(0x10000, 0x7a00, 2);
	for (i = 0; i < 520; i++)
	{
		const u32 v = systemsp_read_area0(i == 3 ? 0x01 : 0x00, 2);
		h = (h ^ v) * 16777619u;
		if (i == 255 || i == 256 || i >= 510)
			printf(" data %u %x %08x\n", i, v, h);
	}
	wr(0x10000, 0x3a00, 2);
	rd(0x10080, 4);
	rd(0x0c, 1);
	/* the first sector, by bytes and longs */
	wr(0x08, 1, 1);
	wr(0x0c, 5, 1);
	wr(0x18, 0xe0, 1);
	wr(0x1c, 0x20, 1);
	rd(0x00, 1);
	rd(0x00, 4);
	rd(0x00, 1);
	/* past the card's end, a hunk that cannot be read, and the carry
	 * into the head */
	wr(0x18, 0x01, 1);
	wr(0x1c, 0x20, 1);
	rd(0x00, 1);
	wr(0x18, 0x00, 1);
	wr(0x10, 0x00, 1);
	wr(0x14, 0x00, 1);
	wr(0x0c, 16, 1);
	wr(0x1c, 0x20, 1);
	rd(0x00, 1);
	wr(0x0c, 24, 1);
	wr(0x1c, 0x20, 1);
	rd(0x00, 1);
	wr(0x08, 2, 1);
	wr(0x0c, 0xff, 1);
	wr(0x10, 0xff, 1);
	wr(0x14, 0xff, 1);
	wr(0x18, 0x02, 1);
	wr(0x1c, 0x20, 1);
	for (i = 0; i < 512; i++)
		systemsp_read_area0(0, 1);
	rd(0x18, 1);
	rd(0x10, 1);
	rd(0x14, 1);
	rd(0x0c, 1);
	wr(0x08, 1, 1);
	wr(0x0c, 0xff, 1);
	wr(0x10, 0x00, 1);
	wr(0x14, 0x00, 1);
	wr(0x18, 0x01, 1);
	wr(0x1c, 0x20, 1);
	for (i = 0; i < 512; i++)
		systemsp_read_area0(0, 1);
	rd(0x18, 1);
	rd(0x10, 1);
	wr(0x08, 2, 1);
	wr(0x0c, 0xff, 1);
	wr(0x1c, 0x20, 1);
	for (i = 0; i < 512; i++)
		systemsp_read_area0(0, 1);
	rd(0x18, 1);
	rd(0x10, 1);
	rd(0x0c, 1);
	wr(0x0c, 0x05, 1);
	wr(0x10, 0x00, 1);
	wr(0x18, 0x08, 1);
	wr(0x1c, 0x20, 1);
	rd(0x00, 1);
	rd(0x00, 1);
	/* idle, and what is not a command */
	wr(0x1c, 0xe1, 1);
	rd(0x1c, 1);
	wr(0x1c, 0x99, 1);
	wr(0x24, 0x1, 1);
	rd(0x24, 1);
	/* the alternate status, and a software reset; interrupts masked */
	wr(0x10000, 0x3b00, 2);
	rd(0x18, 1);
	rd(0x10, 1);
	wr(0x18, 0xff, 1);
	wr(0x18, 0x02, 1);
	wr(0x10, 0x00, 1);
	fire();
	wr(0x10000, 0x3a00, 2);
	rd(0x00, 1);
	rd(0x10080, 1);
	wr(0x10000, 0x3b00, 2);
	wr(0x18, 0x00, 1);
	fire();
	rd(0x10080, 1);
}

static void other_banks(void)
{
	printf("banks\n");
	wr(0x10000, 0x3d00, 2);
	rd(0x100, 4);
	wr(0x100, 1, 4);
	wr(0x10000, 0x3f00, 2);
	rd(0x0, 1);
	wr(0x0, 1, 1);
	wr(0x10000, 0x0002, 2);
	rd(0x0, 2);
	rd(0x2, 1);
	rd(0x4, 4);
	wr(0x6, 0x99, 2);
	rd(0x210006, 2);
	wr(0x10000, 0x3e00, 2);
	rd(0x0, 2);
	rd(0x10030, 1);
	rd(0x10084, 1);
	rd(0x100f0, 2);
	wr(0x10084, 3, 1);
	wr(0x10010, 3, 1);
	rd(0x10124, 1);
	rd(0x10118, 1);
	rd(0x10114, 1);
	wr(0x10124, 3, 1);
	wr(0x10108, 3, 1);
	rd(0x10150, 2);
	rd(0x10160, 1);
	wr(0x10160, 1, 1);
	rd(0x10180, 1);
	rd(0x10184, 1);
	rd(0x10188, 1);
	rd(0x10198, 1);
	rd(0x101b0, 1);
	rd(0x101c0, 1);
	rd(0x101c8, 1);
	rd(0x101d8, 1);
	rd(0x101f0, 4);
	wr(0x10180, 0x41, 1);
	wr(0x101a8, 0x41, 1);
	wr(0x101b0, 0x41, 1);
	wr(0x101c0, 0x42, 1);
	wr(0x101e8, 0x42, 1);
	wr(0x101f0, 0x43, 4);
	rd(0x10180, 1);
	rd(0x101c0, 1);
	rd(0x101c0, 1);
	rd(0x10188, 1);
	/* a device with something to send */
	rd(0x10080, 1);
	pipes[0].base.update_status(pipes[0].base.port);
	rd(0x10080, 1);
	if (pipes[2].base.update_status != NULL)
	{
		pipes[2].base.update_status(pipes[2].base.port);
		rd(0x10080, 1);
	}
	/* the bank register, all of it, and what a byte or a word of it is */
	wr(0x10000, 0xb901, 2);
	rd(0x10000, 2);
	wr(0x10000, 0x3a9901, 1);
	rd(0x10000, 2);
	wr(0x10000, 0x12345678, 2);
	rd(0x10000, 4);
}

static const char *const kinds[] = {
	"default", "card reader", "isshoni", "battle racer", "hopper", "manpuku", "kingyo", "medal"
};

static void inputs(void)
{
	static const u32 keys[] = {
		NAOMI_START_KEY, NAOMI_RIGHT_KEY, NAOMI_LEFT_KEY, NAOMI_UP_KEY, NAOMI_DOWN_KEY,
		NAOMI_BTN0_KEY, NAOMI_BTN1_KEY, NAOMI_BTN2_KEY, NAOMI_BTN3_KEY, NAOMI_BTN4_KEY,
		NAOMI_BTN5_KEY, NAOMI_BTN6_KEY, NAOMI_BTN7_KEY, NAOMI_SERVICE_KEY, NAOMI_TEST_KEY,
		NAOMI_COIN_KEY
	};
	static const u32 ports[] = { 0x10100, 0x10104, 0x10108, 0x1010c, 0x10110, 0x10118, 0x10120 };
	u32 k, p, pl;
	for (pl = 0; pl < 2; pl++)
		for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
		{
			printf(" p%u key %x:", pl + 1, keys[k]);
			kcode[pl] = ~keys[k];
			for (p = 0; p < sizeof(ports) / sizeof(ports[0]); p++)
				printf(" %02x", systemsp_read_area0(ports[p], 1));
			printf("\n");
			kcode[pl] = 0xffffffff;
		}
}

static void wheel(void)
{
	static const int xs[] = { -128, -64, -7, -6, 0, 6, 7, 63, 64, 127 };
	u32 i;
	for (i = 0; i < sizeof(xs) / sizeof(xs[0]); i++)
	{
		joyx[0] = (s8)xs[i];
		joyx[1] = (s8)xs[sizeof(xs) / sizeof(xs[0]) - 1 - i];
		printf(" wheel %d: %02x\n", xs[i], systemsp_read_area0(0x10100, 1));
	}
	joyx[0] = joyx[1] = 0;
}

/* the motors running, and time going on */
static void motors(void)
{
	u32 h = 2166136261u;
	static const u32 outs[] = { 0xff, 0x00, 0xf7, 0xef, 0xdf, 0xbf, 0x7f };
	u32 o, t;
	for (o = 0; o < sizeof(outs) / sizeof(outs[0]); o++)
	{
		wr(0x10114, o & 2 ? 0x02 : 0x00, 1);
		wr(0x1010c, outs[o], 1);
		wr(0x10110, o & 1 ? 0x10 : 0x00, 1);
		for (t = 0; t < 12; t++)
		{
			now64 += 3000000ull + o * 1000000ull;
			if (t == 5)
			{
				kcode[0] &= ~NAOMI_COIN_KEY;
				kcode[1] &= ~NAOMI_COIN_KEY;
			}
			if (t == 9)
				kcode[0] = kcode[1] = 0xffffffff;
			printf(" t%u: %02x %02x %02x\n", t, systemsp_read_area0(0x10100, 1),
					systemsp_read_area0(0x10104, 1), systemsp_read_area0(0x10120, 1));
		}
	}
	/* the pusher, and the others, going round for long */
	wr(0x1010c, 0x00, 1);
	for (t = 0; t < 400; t++)
	{
		now64 += 9000000ull + (t % 7) * 1000000ull;
		h = (h ^ systemsp_read_area0(0x10100, 1)) * 16777619u;
		h = (h ^ systemsp_read_area0(0x10104, 1)) * 16777619u;
		if (t % 50 == 49)
			printf(" long %u: %08x\n", t, h);
	}
}

/* (a remapped button, and a switch of the second player's) */
static InputDescriptors remap;

static void io_tests(int kind)
{
	printf("io %s\n", kinds[kind]);
	inputs();
	if (kind == 3)
		wheel();
	if (kind >= 4)
		motors();
	naomi_game_inputs = &remap;
	{
		static const u32 presses[][2] = {
			{ ~NAOMI_BTN0_KEY, ~NAOMI_BTN1_KEY },
			{ ~NAOMI_BTN1_KEY, 0xffffffff },
			{ 0xffffffff, ~NAOMI_BTN0_KEY },
			{ ~NAOMI_BTN3_KEY, ~NAOMI_BTN3_KEY },
			{ ~NAOMI_BTN2_KEY, 0xffffffff }
		};
		u32 i;
		for (i = 0; i < sizeof(presses) / sizeof(presses[0]); i++)
		{
			kcode[0] = presses[i][0];
			kcode[1] = presses[i][1];
			printf(" remap: %02x %02x %02x %02x %02x\n", systemsp_read_area0(0x10100, 1),
					systemsp_read_area0(0x10104, 1), systemsp_read_area0(0x10108, 1),
					systemsp_read_area0(0x1010c, 1), systemsp_read_area0(0x10120, 1));
		}
	}
	kcode[0] = kcode[1] = 0xffffffff;
	naomi_game_inputs = NULL;
}

static void flash_tests(void)
{
	u8 *rom = cart->rom_ptr;
	u32 v;
	printf("flash\n");
	/* program a word */
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x555, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0xa0);
	cart->ops->write(cart, 0x1001, 2, 0x12345);
	printf(" %02x %02x %02x\n", rom[0x1000], rom[0x1001], rom[0x1002]);
	/* a write buffer of 3 words, and its end */
	cart->ops->write(cart, 0x1aaa, 2, 0xaa);
	cart->ops->write(cart, 0x1554, 2, 0x55);
	cart->ops->write(cart, 0x2000, 2, 0x25);
	save("write buffer");
	cart->ops->write(cart, 0x2000, 2, 2);
	cart->ops->write(cart, 0x2000, 2, 0x1111);
	cart->ops->write(cart, 0x2002, 2, 0x2222);
	cart->ops->write(cart, 0x2004 + rom_size, 2, 0x3333);
	cart->ops->write(cart, 0x2000, 2, 0x29);
	printf(" %08x\n", hash(rom, rom_size));
	/* erase a sector, and the chip */
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x555, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0x80);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x555, 2, 0x55);
	cart->ops->write(cart, 0x20123, 2, 0x30);
	printf(" %08x\n", hash(rom, rom_size));
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x554, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0x80);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x554, 2, 0x55);
	cart->ops->write(cart, 0x4000aaa, 2, 0x10);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x554, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0x80);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x554, 2, 0x55);
	cart->ops->write(cart, 0x20030000, 2, 0x30);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x554, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0x80);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x554, 2, 0x55);
	cart->ops->write(cart, 0x1000aaa, 2, 0x10);
	printf(" %08x\n", hash(rom, rom_size));
	/* what is not a command: to the M4 board */
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x123, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x555, 2, 0x55);
	cart->ops->write(cart, 0x123, 2, 0x99);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x555, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0x80);
	cart->ops->write(cart, 0xaa0, 2, 0xaa);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x555, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0x80);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x556, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x555, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0x80);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x555, 2, 0x55);
	cart->ops->write(cart, 0xaa0, 2, 0x10);
	cart->ops->write(cart, 0x2000, 2, 0x29);
	printf(" %08x\n", hash(rom, rom_size));
	/* the PIO window, read as the M4 board does */
	cart->ops->write(cart, 0x1fffffff, 4, 0x12);
	v = 0;
	cart->ops->read(cart, 0x1000, 4, &v);
	printf(" read %x\n", v);
	cart->ops->write(cart, 0x1fffffff, 4, 0x29);
	v = 0;
	cart->ops->read(cart, 0x1000, 4, &v);
	printf(" read %x\n", v);
	save("pio");
	/* the chip from 32 MB on: all of it */
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x554, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0x80);
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x554, 2, 0x55);
	cart->ops->write(cart, 0x2000aaa, 2, 0x10);
	printf(" %08x\n", hash(rom, rom_size));
}

/* the network board's memory, through the cartridge window */
static void nw(u32 offset, u32 v, u32 size)
{
	cart->ops->write(cart, 0x3d000000 | offset, size, v);
}

static u32 nr(u32 offset, u32 size)
{
	u32 v = 0;
	cart->ops->read(cart, 0x3d000000 | offset, size, &v);
	return v;
}

static void dimm_cmd(u32 cmd, u32 a208)
{
	nw(0x200, 0x1234 + cmd, 1);
	nw(0x202, cmd, 2);
	nw(0x208, a208, 4);
	cart->ops->write_mem(cart, 0x4c, 0, 4);
	printf(" cmd %x: %x %x %x %x %x %x %x\n", cmd, nr(0, 4), nr(4, 4), nr(0x1c, 4), nr(0x24, 4), nr(0x28, 4),
			nr(0x200, 4), cart->dma_offset);
	nw(0x1c, 0x1c1c1c1c, 4);
}

static void dimm_tests(void)
{
	static const u32 cmds[] = {
		1, 0x100, 0x101, 0x104, 0x401, 0x402, 0x404, 0x408, 0x409, 0x40e, 0x410, 0x411, 0x414,
		0x415, 0x999
	};
	u32 i, v;
	printf("dimm\n");
	SB_ISTEXT = 0;
	printf(" status %x\n", cart->ops->read_mem(cart, 0x4c, 2));
	SB_ISTEXT = 8;
	printf(" status %x\n", cart->ops->read_mem(cart, 0x14c, 2));
	cart->ops->write_mem(cart, 0x4c, 0x102, 4);
	for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		dimm_cmd(cmds[i], 0);
	nw(0x24, 0x0a00a8c0, 4);
	nw(0x28, 0x0000ffff, 4);
	nw(4, 3, 1);
	dimm_cmd(0x204, 0);
	dimm_cmd(0x104, 0);
	/* the network test, to the system's RAM, and not */
	dimm_cmd(0x301, 0x0c000100);
	printf(" ram %08x\n", hash(ram, sizeof(ram)));
	dimm_cmd(0x301, 0x10000200);
	printf(" ram %08x\n", hash(ram, sizeof(ram)));
	dimm_cmd(0x301, 0x1c000300);
	printf(" ram %08x\n", hash(ram, sizeof(ram)));
	/* sockets */
	for (i = 0; i < 65; i++)
	{
		cart->ops->write_mem(cart, 0x4c, 0, 4);
		nw(0x202, 0x40b, 2);
		cart->ops->write_mem(cart, 0x4c, 0, 4);
		if (i % 16 == 0 || i > 61)
			printf(" socket %u: %x %x\n", i, nr(2, 2), nr(4, 4));
	}
	dimm_cmd(0x403, 5);
	dimm_cmd(0x403, 5);
	dimm_cmd(0x403, 0);
	dimm_cmd(0x403, 64);
	dimm_cmd(0x40b, 0);
	/* inet_addr */
	{
		static const char *const addrs[] = {
			"192.168.1.20", "10.0.0.1", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1..2.3", "1.2.3.a",
			"", "0.0.0.0", "255.255.255.255"
		};
		for (i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++)
		{
			u32 k;
			for (k = 0; k <= strlen(addrs[i]); k++)
				nw(0x1000 + k, (u8)addrs[i][k], 1);
			dimm_cmd(0x406, 0x1000);
			printf("  '%s'\n", addrs[i]);
		}
		/* at the memory's very end, without its zero */
		for (i = 0; i < 3; i++)
			nw(0x7ffffd + i, '1', 1);
		dimm_cmd(0x406, 0x7ffffd);
		dimm_cmd(0x406, 0xfffffffd);
	}
	nw(0x210, 0x777, 4);
	dimm_cmd(0x40a, 0);
	/* the FIXME: a word at 2 is at 0x202 too */
	nw(2, 0xabcd, 2);
	nw(0x8000002, 0x12345678, 4);
	nw(0x3, 0xef, 1);
	printf(" %x %x\n", nr(0x200, 4), nr(0x202, 2));
	/* reads of each size, wrapping */
	nw(0x7ffffc, 0x89abcdef, 4);
	printf(" %x %x %x %x\n", nr(0x7ffffc, 4), nr(0x7fffff, 2), nr(0x7ffffd, 1), nr(0xfffffe, 4));
	nw(0x7ffffe, 0x1122, 1);
	nw(0x400001, 0x3344, 2);
	nw(0x400005, 0x55667788, 4);
	printf(" %x %x\n", nr(0x7ffffc, 4), nr(0x400000, 4));
	printf(" %x\n", nr(0x400004, 3));
	/* present, whatever the size */
	v = 0xffffffff;
	cart->ops->read(cart, 0x3f000000, 2, &v);
	printf(" present %x\n", v);
	v = 0xffffffff;
	cart->ops->read(cart, 0x7f000010, 8, &v);
	printf(" present %x\n", v);
	cart->ops->write_mem(cart, 0x4c, 0x100, 4);
	cart->ops->write_mem(cart, 0x50, 0x12, 4);
}

static void corrupt_state(void)
{
	/* the offsets, as the state is laid out */
	const unsigned cmd = 2 + 1 + 2 + 8 + 512 + 4 + 4;
	const unsigned enc = cmd + 1 + 4 + 2 + 4 + 4;
	const unsigned ee = enc + 2 + 1;
	const unsigned net = ee + 128 + 1 + 4 + 1 + 1 + 2 + 1 + 2 * SP_PIPE_STATE_SIZE + 1 + 4 * 16;
	const unsigned count = net + 4 + 4 + 8;
	u32 c;
	printf("bad state\n");
	save("good");
	state[cmd] = 9;			/* the flash's command */
	state[enc + 2] = 16;		/* encCounter */
	state[ee + 128] = 0x09;		/* pins */
	state[12] = 0x5a;			/* reg84 */
	state[ee + 128 + 1] = 0xff;	/* command */
	state[ee + 128 + 5] = 30;	/* commandBits */
	state[ee + 128 + 6] = 7;	/* expected */
	state[ee + 128 + 9] = 18;	/* dataOutBits */
	state[net + 8] = 0xff;		/* sockets */
	c = 40;
	memcpy(&state[count], &c, 4);
	state[count + 4] = 0x00;
	state[count + 5] = 0x80;	/* a page past the end */
	state[count + 6] = 3;
	state[count + 7] = 0;
	state[count + 4 + 2 * 31] = 7;
	state[count + 4 + 2 * 31 + 1] = 0;
	load("bad");
	save("after bad");
	/* a command half way, and the EEPROM's */
	memcpy(&c, &state[cmd + 1], 4);
	printf(" prog %x\n", c);
	state[ee + 128 + 5] = 10;
	state[ee + 128 + 6] = 25;
	load("half");
	save("after half");
	rd(0x10084, 1);
	state[ee + 128 + 5] = 12;
	state[ee + 128 + 6] = 0;
	load("bits");
	save("after bits");
	state[ee + 128 + 5] = 25;
	state[ee + 128 + 6] = 25;
	load("all bits");
	save("after all bits");
}

/*
 * The games
 */
static void rom_game(const char *name, int encrypted, u8 country, int region)
{
	u8 h[0x500];
	settings.dreamcast.region = region;
	open_board(name, NULL, NULL, 0x200000);
	header(h, country, "ROM GAME", encrypted == 2 ? "" : "  ");
	if (encrypted == 1)
		encrypt(h, sizeof(h));
	if (encrypted == 3)
		memcpy(h, "NAOMI", 5);
	if (encrypted == 4)
		memset(&h[0x50], 0, 0x20);
	memcpy(cart->rom_ptr, h, sizeof(h));
	init_board();
	game_id();
	cart->ops->write(cart, 0xaaa, 2, 0xaa);
	cart->ops->write(cart, 0x555, 2, 0x55);
	cart->ops->write(cart, 0xaaa, 2, 0xa0);
	cart->ops->write(cart, 0x10, 2, 0x55aa);
	systemsp_cart_load_eeprom();
	save("init");
	close_board();
}

int main(void)
{
	static const char *const io_games[] = {
		"tetgiano", "dinoki4", "isshoni", "btlracer", "kingyo2", "manpuku", "shateki", "westdrmg",
		"lovebox", "kingyx"
	};
	static const int io_kinds[] = { 0, 1, 2, 3, 6, 5, 6, 7, 0, 0 };
	u32 i;
	cipher_init();
	k1 = 0x1234;
	k2 = 0xbeef;
	strcpy(g_roms_dir, "/roms/");
	strcpy(g_base_name, "dinokich.set.zip");
	strcpy(eeprom_file, "/saves/dinokich.zip.eeprom");
	strcpy(nvmem_file, "/saves/dinokich.zip.nvmem");
	settings.System = DC_PLATFORM_NAOMI;
	remap.buttons[0].mask = NAOMI_BTN0_KEY;
	remap.buttons[0].target = NAOMI_START_KEY;
	remap.buttons[1].mask = NAOMI_BTN1_KEY;
	remap.buttons[1].p2_mask = NAOMI_COIN_KEY;
	remap.buttons[2].mask = NAOMI_BTN2_KEY;
	remap.buttons[2].target = 0xffffffff;
	remap.buttons[3].mask = NAOMI_BTN3_KEY;
	remap.buttons[3].target = NAOMI_UP_KEY;

	naomi_default_eeprom = (u8 *)malloc(128);
	memset(naomi_default_eeprom, 0x5a, 128);

	/* no board */
	rd(0x10000, 2);
	rd(0x10000, 4);
	rd(0x10000, 1);
	wr(0x10000, 1, 2);
	save("none");

	/* a game on a card, its header twice encrypted, its name a long one */
	make_card("longname_game.bin", "LONGNA~1BIN", 1, 0x0e, "THE TITLE", 0);
	strcpy(cf_path, "/roms/dinokich.set/cf.chd");
	file_set("/saves/dinokich.zip.flash", cf, 0);
	open_board("dinokich", "cf", "dinoki", 0x800000);
	init_board();
	game_id();
	file_set("/saves/dinokich.zip-jp.eeprom", (const u8 *)"0123456789abcdef0123456789abcdef0123456789abcdef"
			"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 128);
	systemsp_cart_load_eeprom();
	eeprom_tests();
	sram_tests();
	cf_bad_hunk = 1;
	ata_tests(1);
	cf_bad_hunk = -1;
	ata_tests(0);
	other_banks();
	flash_tests();
	dimm_tests();
	io_tests(1);
	save("dinokich");
	{
		u8 copy[sizeof(state)];
		const unsigned n = state_size;
		memcpy(copy, state, n);
		cart->ops->reset(cart);
		save("reset");
		memcpy(state, copy, n);
		load("dinokich");
		save("loaded");
	}
	corrupt_state();
	naomi_cart_systemsp = false;
	{
		void *p = state;
		unsigned int total = 0;
		systemsp_serialize_v24(&p, &total);
		printf(" not sp %u %d\n", total, systemsp_unserialize_v24(&p, &total));
		naomi_cart_systemsp = true;
		settings.System = DC_PLATFORM_DREAMCAST;
		systemsp_serialize_v24(&p, &total);
		systemsp_serialize_v25(&p, &total);
		printf(" not a naomi %u %d\n", total, systemsp_unserialize_v24(&p, &total));
		settings.System = DC_PLATFORM_NAOMI;
		naomi_cart_systemsp = false;
		systemsp_state_before_v24();
		systemsp_state_before_v25();
		naomi_cart_systemsp = true;
		systemsp_state_before_v25();
	}
	close_board();

	/* from the parent's folder, encrypted once, its name short; the flash
	 * from its file; the EEPROM the BIOS's */
	make_card("game.bin", "GAME    BIN", 0, 0x04, "", 0);
	strcpy(cf_path, "/roms/loveber/cf.chd");
	strcpy(g_base_name, "loveber3.7z");
	strcpy(eeprom_file, "/saves/loveber3.7z.eeprom");
	{
		u8 *flash = (u8 *)malloc(0x800000);
		for (i = 0; i < 0x800000; i++)
			flash[i] = (u8)(i >> 8);
		file_set("/saves/loveber3.7z.flash", flash, 0x800000);
		free(flash);
	}
	settings.dreamcast.region = 0;
	open_board("loveber3", "cf", "loveber", 0x800000);
	init_board();
	game_id();
	printf(" flash %08x\n", hash(cart->rom_ptr, 0x800000));
	systemsp_cart_load_eeprom();
	ee_read(3);
	io_tests(1);
	rd(0x10180, 1);
	rd(0x101c0, 1);
	pipes[2].base.update_status(pipes[2].base.port);
	rd(0x10080, 1);
	pipes[1].base.update_status(pipes[1].base.port);
	rd(0x10080, 1);
	save("loveber3");
	load("loveber3");
	flash_tests();
	ee_cmd(0x130, 9);
	ee_cmd((0x140 | 2) << 16 | 0x2222, 25);
	file_bad = true;
	close_board();
	file_bad = false;

	/* its flash's file not the flash's size */
	file_set("/saves/loveber3.7z.flash", cf, 100);
	open_board("loveber3", "cf", "loveber", 0x800000);
	init_board();
	printf(" flash %08x\n", hash(cart->rom_ptr, 0x800000));
	close_board();

	/* no card, or not one */
	strcpy(cf_path, "/nowhere");
	open_board("loveber2", "cf", NULL, 0x800000);
	init_board();
	close_board();
	strcpy(cf_path, "/roms/loveber3/cf.chd");
	cf_hunkbytes = 1000;
	open_board("loveber3", "cf", NULL, 0x800000);
	init_board();
	close_board();
	cf_hunkbytes = HUNK;
	/* cards that are not as they should be, or odd */
	strcpy(cf_path, "/roms/loveber3/cf.chd");
	{
		char name[0x81];
		memset(name, 'a', 0x7c);
		strcpy(&name[0x7c], ".bin");
		card_variant("a name of 128", name, "AAAAAA~1BIN", 0);
	}
	card_variant("plain", "game.bin", "GAME    BIN", 0);
	card_variant("a name without its last letter", "game.bi", "GAME    BIN", 0);
	card_variant("no name", ".bin", "        BIN", 0);
	card_variant("the directory's end", "game.bin", "GAME    BIN", V_ZERO_ENTRY);
	card_variant("cluster 1", "game.bin", "GAME    BIN", V_CLUSTER1);
	card_variant("FAT32", "game.bin", "GAME    BIN", V_PTYPE);
	card_variant("1STREAD.BIN short", "game.bin", "GAME    BIN", V_SMALL1ST);
	card_variant("signature", "game.bin", "GAME    BIN", V_SIG);
	card_variant("image short", "game.bin", "GAME    BIN", V_SMALLIMG);
	card_variant("no boot record", "game.bin", "GAME    BIN", V_NOMBR);

	/* games in ROM: the header plain, encrypted, a NAOMI one; the regions */
	rom_game("puyomedal", 0, 0x0e, 1);
	rom_game("unomedal", 1, 0x02, 1);
	rom_game("ochaken", 2, 0x08, 0);
	rom_game("magicpop", 3, 0x00, 2);
	rom_game("dinoki2", 0, 0x04, 3);
	rom_game("dinoki3", 4, 0x04, 1);
	open_board("tetgiano", NULL, NULL, 0x400);
	memset(cart->rom_ptr, 'A', 0x400);
	init_board();
	game_id();
	close_board();

	/* each kind of cabinet */
	for (i = 0; i < sizeof(io_games) / sizeof(io_games[0]); i++)
	{
		open_board(io_games[i], NULL, NULL, 0x200000);
		init_board();
		io_tests(io_kinds[i]);
		rd(0x10180, 1);
		save(io_games[i]);
		load(io_games[i]);
		cart->ops->reset(cart);
		save("reset");
		close_board();
	}
	return 0;
}
