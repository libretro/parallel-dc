/* The NAOMI and AtomisWave romset loader (core/hw/naomi/naomi_cart.c), by
 * itself: the machine a file is for, a set found by its name or by what
 * is in it, a merged set's games and the one asked for, a clone's ROMs in
 * its parent's set, the BIOS from its own archive, each kind of ROM put
 * where the table says (decoded in place, streamed, interleaved, copied, a
 * key, a ROM board's serial EEPROM, a default EEPROM), a flat image in an
 * archive, a .bin and a .lst with its pieces, and the header the BIOS goes
 * by. The archives are the test's own, and the cartridges the loader opens
 * are stand-ins that keep what is put in them; what the loader does with
 * them is printed, for loader_expected.txt (sh tools/naomi/loader_run.sh). */
#ifndef _WIN32
#define _DEFAULT_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "types.h"
#include "paths.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/naomi_periph.h"
#include "hw/flashrom/flashrom.h"
#include "archive/archive.h"
#include "file/file_path.h"
#include "streams/file_stream.h"

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

/* Where the test's files are: printed as $T */
static char dir[512];
static void path_print(const char *what, const char *p)
{
	const size_t n = strlen(dir);
	if (p != NULL && n != 0 && !strncmp(p, dir, n))
		printf("  %s $T%s\n", what, p + n);
	else
		printf("  %s %s\n", what, p ? p : "(null)");
}

/* The rest of the core */
settings_t settings;
char game_dir_no_slash[1024];
char *game_data;
char eeprom_file[4096];
char g_base_name[128];
char g_roms_dir[4096];
char naomi_set_name[32];
bool naomi_cart_systemsp;
static u8 rom_data[0x200000], flash_data[0x20000];
MemChip sys_rom, sys_nvmem_flash;
bool memchip_reload(MemChip *chip)
{
	printf("  memchip reload %d\n", chip == &sys_nvmem_flash);
	return true;
}
void naomi_serial_id_load(const u8 *rom_board)
{
	if (rom_board)
		printf("  serial id %08x\n", hash(rom_board, 0x84));
}

/* The memory the image is mapped into */
static u8 *arena;
static size_t arena_size;
void *mem_region_reserve(void *start, size_t len)
{
	if (start == NULL)
	{
		free(arena);
		arena = (u8 *)calloc(1, len ? len : 1);
		arena_size = len;
		return arena;
	}
	return (u8 *)start + len <= arena + arena_size ? start : NULL;
}
bool mem_region_release(void *start, size_t len)
{
	(void)start;
	(void)len;
	return true;
}
void *mem_region_map_file(void *file_handle, void *dest, size_t len, size_t offset, bool readwrite)
{
	const int fd = (int)(uintptr_t)file_handle;
	ssize_t n;
	(void)readwrite;
	if ((u8 *)dest + len > arena + arena_size)
		return NULL;
	lseek(fd, (off_t)offset, SEEK_SET);
	n = read(fd, dest, len);
	printf("  mapped %x bytes (%d read) at %x\n", (unsigned)len, (int)n, (unsigned)((u8 *)dest - arena));
	return dest;
}

/* The cartridges: what the loader puts in them is kept */
typedef struct
{
	cartridge base;
	const char *kind;
	u8 *key_data;
	bool own;
} fake_cart;
static fake_cart fake;

static void *f_get_ptr(cartridge *c, u32 offset, u32 *size)
{
	offset &= 0x1fffffff;
	if (offset >= c->rom_size || *size > c->rom_size - offset)
	{
		*size = 0;
		return NULL;
	}
	return c->rom_ptr + offset;
}
static bool f_read(cartridge *c, u32 offset, u32 size, void *dst)
{
	if (offset >= c->rom_size || size > c->rom_size - offset)
		memset(dst, 0xff, size);
	else
		memcpy(dst, c->rom_ptr + offset, size);
	return true;
}
static bool f_init(cartridge *c) { printf("  init\n"); return c->key != 0xdead; }
static void f_set_key(cartridge *c, u32 key) { printf("  key %x\n", key); c->key = key; }
static void f_set_key_data(cartridge *c, u8 *data)
{
	printf("  key data %08x\n", hash(data, 2048));
	free(fake.key_data);
	fake.key_data = data;
}
static void f_game_id(cartridge *c, char *id)
{
	if (c->rom_size >= 0x50)
		cart_trim_id(id, (const char *)c->rom_ptr + 0x30, 0x20);
	else
		strcpy(id, "");
}
static void f_close(cartridge *c)
{
	size_t used = 0, i;
	if (c->rom_ptr != NULL)
		for (i = 0; i < c->rom_size; i++)
			if (c->rom_ptr[i] != 0)
				used = i + 1;
	printf("  closed %s: %x bytes, %08x\n", fake.kind, (unsigned)used, c->rom_ptr ? hash(c->rom_ptr, used) : 0);
	if (fake.own)
		free(c->rom_ptr);
	free(fake.key_data);
	fake.key_data = NULL;
	c->rom_ptr = NULL;
}
static const cartridge_ops f_ops = {
	f_init, NULL, NULL, NULL, f_read, NULL, f_get_ptr, NULL, NULL, f_game_id,
	NULL, NULL, NULL, NULL, f_set_key, f_set_key_data, NULL, NULL, f_close
};

static cartridge *open_fake(const char *kind, u8 *rom, u32 size, bool own)
{
	memset(&fake, 0, sizeof(fake));
	fake.base.ops = &f_ops;
	fake.base.rom_ptr = rom;
	fake.base.rom_size = rom ? size : 0;
	fake.kind = kind;
	fake.own = own;
	printf("  open %s %x\n", kind, size);
	return &fake.base;
}
static cartridge *open_board(const char *kind, u32 size)
{
	return open_fake(kind, (u8 *)calloc(1, size), size, true);
}
cartridge *m1_cart_open(u32 size) { return open_board("m1", size); }
cartridge *m2_cart_open(u32 size) { return open_board("m2", size); }
cartridge *m4_cart_open(u32 size) { return open_board("m4", size); }
cartridge *aw_cart_open(u32 size) { return open_board("aw", size); }
cartridge *gd_cart_open(u32 size, const char *gdrom_name, bool dimm, bool net)
{
	printf("  gd %s %d %d\n", gdrom_name, dimm, net);
	return open_board("gd", size);
}
cartridge *systemsp_cart_open(u32 size, const char *game_name, const char *media_name, const char *parent_name)
{
	printf("  system sp %s %s %s\n", game_name, media_name ? media_name : "-", parent_name ? parent_name : "-");
	return open_board("sp", size);
}
int systemsp_cart_region(void) { return 1; }
void systemsp_cart_load_eeprom(void) { printf("  sp eeprom\n"); }
cartridge *decrypted_cart_open(u8 *rom_ptr, u32 size) { return open_fake("flat", rom_ptr, size, false); }
cartridge *archived_cart_open(const u8 *rom_ptr, u32 size, struct archive *archive)
{
	(void)archive;
	return open_fake("archived", (u8 *)rom_ptr, size, false);
}
u32 cart_trim_id(char *id, const char *s, u32 len)
{
	while (len != 0 && s[len - 1] == ' ')
		len--;
	memcpy(id, s, len);
	id[len] = '\0';
	return len;
}

/* The archives: each a list of members, their bytes made from their
 * names */
typedef struct
{
	const char *name;
	u32 crc;
	u32 size;
	u8 usable;
} member;

struct archive
{
	const char *path;
	const member *members;
	unsigned count;
	archive_entry_t entries[16];
	u8 *data[16];
	bool open;
};

#define MAX_ARCHIVES 16
static struct archive archives[MAX_ARCHIVES];
static int narchives;

static void add_archive(const char *path, const member *m, unsigned count)
{
	struct archive *a = &archives[narchives++];
	unsigned i;
	a->path = path;
	a->members = m;
	a->count = count;
	for (i = 0; i < count; i++)
	{
		u32 j, h = hash(m[i].name, strlen(m[i].name));
		a->entries[i].name = m[i].name;
		a->entries[i].size = m[i].size;
		a->entries[i].crc = m[i].crc;
		a->entries[i].usable = m[i].usable;
		a->entries[i].is_dir = m[i].name[strlen(m[i].name) - 1] == '/';
		a->data[i] = (u8 *)malloc(m[i].size ? m[i].size : 1);
		for (j = 0; j < m[i].size; j++)
		{
			h = h * 1103515245u + 12345u;
			a->data[i][j] = (u8)((h >> 16) | 1);
		}
	}
}

static void drop_archives(void)
{
	int i;
	unsigned j;
	for (i = 0; i < narchives; i++)
	{
		if (archives[i].open)
			printf("FAIL: %s left open\n", archives[i].path);
		for (j = 0; j < archives[i].count; j++)
			free(archives[i].data[j]);
	}
	narchives = 0;
}

/* (@path, or @path with .zip after it, as archive.c tries; a folder's
 * name with a slash too many is the folder's) */
archive_t *archive_open(const char *path)
{
	char p[1024];
	size_t n = 0;
	int i;
	for (; *path != '\0' && n + 1 < sizeof(p); path++)
		if (!(*path == '/' && n != 0 && p[n - 1] == '/'))
			p[n++] = *path;
	p[n] = '\0';
	for (i = 0; i < narchives; i++)
	{
		if (!strcmp(p, archives[i].path)
				|| (!strncmp(p, archives[i].path, n) && !strcmp(archives[i].path + n, ".zip")))
		{
			path_print("archive", archives[i].path);
			archives[i].open = true;
			return &archives[i];
		}
	}
	path_print("no archive", p);
	return NULL;
}
void archive_close(archive_t *a)
{
	if (a != NULL)
		a->open = false;
}
unsigned archive_num_entries(const archive_t *a) { return a->count; }
const archive_entry_t *archive_entry(const archive_t *a, unsigned index)
{
	return index < a->count ? &a->entries[index] : NULL;
}
int archive_find_crc(const archive_t *a, uint32_t crc)
{
	unsigned i;
	for (i = 0; i < a->count; i++)
		if (a->entries[i].crc == crc && crc != 0)
			return (int)i;
	return -1;
}
const uint8_t *archive_entry_data(archive_t *a, unsigned index, size_t *len)
{
	*len = a->entries[index].size;
	printf("  data %s\n", a->entries[index].name);
	return a->data[index];
}
int archive_entry_read(archive_t *a, unsigned index, uint8_t *dst, size_t dst_size, size_t *len)
{
	printf("  read %s\n", a->entries[index].name);
	if (a->entries[index].size > dst_size)
		return 0;
	memcpy(dst, a->data[index], a->entries[index].size);
	*len = a->entries[index].size;
	return 1;
}
int archive_entry_stream(archive_t *a, unsigned index, archive_sink_t sink, void *ud)
{
	printf("  stream %s\n", a->entries[index].name);
	return sink(ud, 0, a->data[index], a->entries[index].size);
}

/* The frontend's files: the host's */
RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	(void)mode;
	(void)hints;
	return (RFILE *)fopen(path, "rb");
}
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
	return (int64_t)fread(data, 1, (size_t)len, (FILE *)stream);
}
char *filestream_gets(RFILE *stream, char *s, size_t len)
{
	return fgets(s, (int)len, (FILE *)stream);
}
int64_t filestream_get_size(RFILE *stream)
{
	FILE *f = (FILE *)stream;
	long n;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	return n;
}
int filestream_close(RFILE *stream)
{
	return fclose((FILE *)stream);
}
struct tm *rtime_localtime(const time_t *timep, struct tm *result)
{
	(void)timep;
	return result;
}

/* A file of the test's */
static void write_file(const char *name, const void *p, size_t len)
{
	char path[1024];
	FILE *f;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	fwrite(p, 1, len, f);
	fclose(f);
}

static void loaded(bool ok)
{
	static u8 header[0x500];
	const char *p;
	printf("  loaded %d [", ok);
	for (p = naomi_game_id; *p != '\0'; p++)
		printf((u8)*p >= 0x20 && (u8)*p < 0x7f ? "%c" : "\\x%02x", (u8)*p);
	printf("] set [%s] bios %d rotation %d inputs %d\n", naomi_set_name,
			bios_loaded, naomi_cart_GetRotation(), naomi_game_inputs != NULL);
	printf("  rom %08x flash %08x eeprom %08x\n", hash(rom_data, sizeof(rom_data)), hash(flash_data, sizeof(flash_data)),
			naomi_default_eeprom ? hash(naomi_default_eeprom, 0x80) : 0);
	printf("  header %d\n", naomi_cart_GetBootHeader(header, sizeof(header)));
}

static void load(const char *path)
{
	bool ok;
	memset(rom_data, 0, sizeof(rom_data));
	memset(flash_data, 0, sizeof(flash_data));
	path_print("load", path);
	game_data = (char *)path;
	ok = naomi_cart_SelectFile();
	loaded(ok);
	naomi_cart_Close();
}

static void system_type(const char *path)
{
	int system;
	path_print("system", path);
	system = naomi_cart_GetSystemType(path);
	printf("  is %d sp %d\n", system, naomi_cart_systemsp);
}

int main(void)
{
	static char path[1024], path2[1024];
	static u8 image[0x2000];
	NaomiSet sets[8];
	int i, n;

	snprintf(dir, sizeof(dir), "%s/loader_test.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	if (mkdtemp(dir) == NULL)
		return 1;
	sys_rom.data = rom_data;
	sys_rom.size = sizeof(rom_data);
	sys_nvmem_flash.data = flash_data;
	sys_nvmem_flash.size = sizeof(flash_data);
	strcpy(game_dir_no_slash, "/sys");
	strcpy(g_roms_dir, "/roms/");
	settings.System = DC_PLATFORM_NAOMI;
	settings.dreamcast.region = 1;
	settings.bios.UseRealBios = true;

	/* A set by its name, with the BIOS: a ROM twice, decoded in place */
	{
		static const member tetris[] = {
			{ "epr-22909.ic22", 0x486b2fdf, 0x80, 1 },
			{ "mpr-22910.ic1", 0x7968b67e, 0x40, 1 },
			{ "mpr-22911.ic2", 0, 0x40, 1 },			/* by its name */
			{ "mpr-22912.ic3", 0x67667a56, 0x40, 0 },	/* not readable in place */
			{ "mpr-22913.ic4", 0x1fbdc41a, 0x40, 1 },
			{ "mpr-22914.ic5", 0x77844b60, 0x40, 1 },
			{ "mpr-22915.ic6", 0x12345678, 0x40, 1 },	/* (another checksum) */
		};
		static const member naomi[] = {
			{ "epr-21576h.ic27", 0xd4895685, 0x100, 1 },
			{ "epr-21576g.ic27", 0xd2a1c6bf, 0x100, 1 },
			{ "epr-21577h.ic27", 0xfdf17452, 0x100, 1 },
			{ "epr-21578h.ic27", 0x7b452946, 0x100, 0 },
		};
		add_archive("/roms/sgtetris.zip", tetris, 7);
		add_archive("/sys/naomi.zip", naomi, 4);
		system_type("/roms/sgtetris.zip");
		printf("  merged %d\n", naomi_cart_MergedSets("/roms/sgtetris.zip", sets, 8));
		load("/roms/sgtetris.zip");
		/* the region asked for not there: the export BIOS read */
		settings.dreamcast.region = 2;
		load("/roms/sgtetris.zip");
		/* none for the region (Korea, none): any */
		settings.dreamcast.region = 3;
		load("/roms/sgtetris.zip");
		settings.dreamcast.region = 1;
		/* without the BIOS */
		settings.bios.UseRealBios = false;
		load("/roms/sgtetris.zip");
		settings.bios.UseRealBios = true;
		/* Japan's: its newest revision, and not the one after */
		settings.dreamcast.region = 0;
		load("/roms/sgtetris.zip");
		settings.dreamcast.region = 1;
		/* by what is in it, under no set's name */
		add_archive("/roms/My Tetris.zip", tetris, 7);
		system_type("/roms/My Tetris.zip");
		load("/roms/My Tetris.zip");
		drop_archives();
	}

	/* A clone with its parent's ROMs in the parent's set; and a merged
	 * set, its clone asked for */
	{
		static const member clone[] = {
			{ "epr-23428.ic22", 0xf8597496, 0x60, 1 },
		};
		static const member parent[] = {
			{ "epr-23428a.ic22", 0xd628dbce, 0x60, 1 },
			{ "mpr-23420.ic1", 0x9d5991f2, 0x20, 1 },
			{ "mpr-23421.ic2", 0x6c351db3, 0x20, 1 },
			{ "mpr-23422.ic3", 0xa1d4bd29, 0x20, 1 },
			{ "mpr-23423.ic4", 0x08cbf373, 0x20, 1 },
			{ "mpr-23424.ic5", 0xf4404000, 0x20, 1 },
			{ "mpr-23425.ic6", 0x47f51da2, 0x20, 1 },
			{ "mpr-23426.ic7", 0x7f91b13f, 0x20, 1 },
			{ "mpr-23427.ic8", 0x5851d525, 0x20, 1 },
		};
		static const member merged[] = {
			{ "epr-23428a.ic22", 0xd628dbce, 0x60, 1 },
			{ "csmasho/epr-23428.ic22", 0, 0x60, 1 },
			{ "mpr-23420.ic1", 0x9d5991f2, 0x20, 1 },
			{ "mpr-23421.ic2", 0x6c351db3, 0x20, 1 },
			{ "mpr-23422.ic3", 0xa1d4bd29, 0x20, 1 },
			{ "mpr-23423.ic4", 0x08cbf373, 0x20, 1 },
			{ "mpr-23424.ic5", 0xf4404000, 0x20, 1 },
			{ "mpr-23425.ic6", 0x47f51da2, 0x20, 1 },
			{ "mpr-23426.ic7", 0x7f91b13f, 0x20, 1 },
			{ "mpr-23427.ic8", 0x5851d525, 0x20, 1 },
		};
		settings.bios.UseRealBios = false;
		add_archive("/roms/csmasho.zip", clone, 1);
		add_archive("/roms/csmash.zip", parent, 9);
		load("/roms/csmasho.zip");
		drop_archives();
		add_archive("/roms/csmash.zip", merged, 10);
		n = naomi_cart_MergedSets("/roms/csmash.zip", sets, 8);
		printf("  merged %d:", n);
		for (i = 0; i < n; i++)
			printf(" %s (%s)", sets[i].name, sets[i].description);
		printf("\n");
		naomi_cart_WantSet("csmasho");
		load("/roms/csmash.zip");
		naomi_cart_WantSet("doa2");
		load("/roms/csmash.zip");
		naomi_cart_WantSet(NULL);
		load("/roms/csmash.zip");
		drop_archives();
		settings.bios.UseRealBios = true;
	}

	/* An M1 cartridge: interleaved ROMs, streamed and copied, and a copy */
	{
		static const member mushike[] = {
			{ "epr-24217.ic11", 0x5ecf807b, 0x80, 1 },
			{ "mpr-24218.ic17s", 0xe8c51e1d, 0x40, 1 },
			{ "mpr-24219.ic18", 0xb0a5709f, 0x40, 0 },
			{ "mpr-24220.ic19s", 0x9f6cefe2, 0x40, 1 },
			{ "mpr-24221.ic20", 0x73bf9cc6, 0x40, 1 },
			{ "mpr-24222.ic21s", 0x7098e728, 0x40, 1 },
			{ "MPR-24223.IC22", 0, 0x40, 1 },
		};
		settings.bios.UseRealBios = false;
		add_archive("/roms/mushike.zip", mushike, 7);
		load("/roms/mushike.zip");
		/* one missing: not loaded */
		drop_archives();
		add_archive("/roms/mushike.zip", mushike, 5);
		load("/roms/mushike.zip");
		drop_archives();
		settings.bios.UseRealBios = true;
	}

	/* An AtomisWave: its BIOS, required, in the flash */
	{
		static const member anml[] = {
			{ "vm2001f01.u3", 0x4fb33380, 0x40, 1 },
			{ "vm2001f01.u4", 0x7cb2e7c3, 0x40, 1 },
			{ "vm2001f01.u2", 0x386070a1, 0x40, 1 },
			{ "vm2001f01.u15", 0x2bb1be28, 0x40, 1 },
		};
		static const member awbios[] = {
			{ "bios0.ic23", 0x719b2b0b, 0x80, 1 },
		};
		add_archive("/roms/anmlbskt.zip", anml, 4);
		system_type("/roms/anmlbskt.zip");
		settings.System = DC_PLATFORM_ATOMISWAVE;
		load("/roms/anmlbskt.zip");
		add_archive("/sys/awbios.zip", awbios, 1);
		load("/roms/anmlbskt.zip");
		settings.System = DC_PLATFORM_NAOMI;
		drop_archives();
	}

	/* A BIOS kept in another one's archive, and its revision not there;
	 * a game for export alone; a set too little of which is there */
	{
		static const member marstv[] = { { "epr-22993.ic22", 0x6f4acc31, 0x40, 1 } };
		static const member naomi_d[] = {
			{ "epr-21576d.ic27", 0x3b2afa7b, 0x100, 1 },
			{ "epr-21577h.ic27", 0xfdf17452, 0x100, 1 },
		};
		static const member naomi[] = { { "epr-21577h.ic27", 0xfdf17452, 0x100, 1 } };
		static const member vs[] = { { "epr-23663b.ic22", 0x15733e44, 0x40, 1 } };
		static const member naomi2[] = {
			{ "epr-23605c.ic27", 0x297ea6ed, 0x100, 1 },
			{ "epr-23607c.ic27", 0x750e254b, 0x100, 1 },
			{ "epr-23608c.ic27", 0x6ef1dd8e, 0x100, 1 },
		};
		static const member partial[] = {
			{ "mpr-23420.ic1", 0x9d5991f2, 0x20, 1 },
			{ "mpr-23421.ic2", 0x6c351db3, 0x20, 1 },
		};
		static const member most[] = {
			{ "mpr-23420.ic1", 0x9d5991f2, 0x20, 1 },
			{ "mpr-23421.ic2", 0x6c351db3, 0x20, 1 },
			{ "mpr-23422.ic3", 0xa1d4bd29, 0x20, 1 },
			{ "mpr-23423.ic4", 0x08cbf373, 0x20, 1 },
			{ "mpr-23424.ic5", 0xf4404000, 0x20, 1 },
			{ "mpr-23425.ic6", 0x47f51da2, 0x20, 1 },
		};
		add_archive("/roms/marstv.zip", marstv, 1);
		add_archive("/sys/naomi.zip", naomi_d, 2);
		load("/roms/marstv.zip");
		drop_archives();
		add_archive("/roms/marstv.zip", marstv, 1);
		add_archive("/sys/naomi.zip", naomi, 1);
		load("/roms/marstv.zip");
		drop_archives();
		add_archive("/roms/vstrik3c.zip", vs, 1);
		add_archive("/sys/naomi2.zip", naomi2, 3);
		settings.System = DC_PLATFORM_NAOMI2;
		load("/roms/vstrik3c.zip");
		settings.System = DC_PLATFORM_NAOMI;
		drop_archives();
		add_archive("/roms/partial.zip", partial, 2);
		add_archive("/roms/most.zip", most, 6);
		system_type("/roms/partial.zip");
		system_type("/roms/most.zip");
		drop_archives();
	}

	/* A key file; a ROM board's serial EEPROM, and one too short; a
	 * game's default EEPROM, and its table's */
	{
		static const member zunou[] = {
			{ "fpr-24338.ic8", 0x1423c374, 0x40, 1 },
			{ "fpr-24339.ic9", 0x11883792, 0x40, 1 },
			{ "317-0435-jpn.ic3", 0xb553d900, 0x800, 1 },
		};
		static const member tduno[] = {
			{ "epr-22073.ic22", 0xdbeee93c, 0x40, 1 },
			{ "mpr-22074.ic1", 0xfd6070a4, 0x40, 1 },
			{ "mpr-22075.ic2", 0x4c11d298, 0x40, 1 },
			{ "mpr-22076.ic3", 0xe4c98898, 0x40, 1 },
			{ "mpr-22077.ic4", 0xf33d7620, 0x40, 1 },
			{ "x76f100.ic37", 0xc79251d5, 0x84, 1 },
		};
		static const member tduno_short[] = {
			{ "epr-22073.ic22", 0xdbeee93c, 0x40, 1 },
			{ "mpr-22074.ic1", 0xfd6070a4, 0x40, 1 },
			{ "mpr-22075.ic2", 0x4c11d298, 0x40, 1 },
			{ "mpr-22076.ic3", 0xe4c98898, 0x40, 1 },
			{ "mpr-22077.ic4", 0xf33d7620, 0x40, 1 },
			{ "x76f100.ic37", 0xc79251d5, 0x40, 1 },
		};
		static const member bdrdown[] = {
			{ "317-5097-jpn.pic", 0x16d2a748, 0x4000, 1 },
			{ "bdrdown-default-eeprom.bin", 0x5b19727c, 0x80, 1 },
		};
		static const member bdrdown_none[] = {
			{ "317-5097-jpn.pic", 0x16d2a748, 0x4000, 1 },
		};
		static const member alpilot[] = { { "x", 0, 1, 1 } };
		settings.bios.UseRealBios = false;
		add_archive("/roms/zunou.zip", zunou, 3);
		add_archive("/roms/tduno.zip", tduno, 6);
		add_archive("/roms/bdrdown.zip", bdrdown, 2);
		load("/roms/zunou.zip");
		load("/roms/tduno.zip");
		load("/roms/bdrdown.zip");
		drop_archives();
		add_archive("/roms/tduno.zip", tduno_short, 6);
		add_archive("/roms/bdrdown.zip", bdrdown_none, 1);
		load("/roms/tduno.zip");
		load("/roms/bdrdown.zip");
		/* (one with the table's own: none in the set, a ROM missing) */
		add_archive("/roms/alpilot.zip", alpilot, 1);
		load("/roms/alpilot.zip");
		drop_archives();
		settings.bios.UseRealBios = true;
	}

	/* The NAOMI 2's machine; a System SP's; a GD-ROM game's PIC */
	{
		static const member vs[] = { { "epr-23663b.ic22", 0x15733e44, 0x40, 1 } };
		static const member sp[] = { { "x", 0, 1, 1 } };
		static const member azu[] = { { "317-5091-jpn.pic", 0xb71ede16, 0x4000, 1 } };
		add_archive("/roms/vstrik3c.zip", vs, 1);
		add_archive("/roms/bingogal.zip", sp, 1);
		add_archive("/roms/azumanga.zip", azu, 1);
		system_type("/roms/vstrik3c.zip");
		system_type("/roms/bingogal.zip");
		system_type("/roms/azumanga.zip");
		settings.bios.UseRealBios = false;
		load("/roms/azumanga.zip");
		settings.bios.UseRealBios = true;
		drop_archives();
	}

	/* Not a set: an archive of a flat image, an archive of two, nothing */
	{
		static const member flat[] = { { "readme.txt", 0, 0x10, 1 }, { "game.BIN", 0, 0x600, 1 } };
		static const member two[] = { { "a.bin", 0, 0x600, 1 }, { "b.dat", 0, 0x600, 1 } };
		static const member dat[] = { { "game.dat", 0, 0x600, 1 } };
		add_archive("/roms/image.zip", flat, 2);
		add_archive("/roms/two.zip", two, 2);
		system_type("/roms/image.zip");
		system_type("/roms/two.zip");
		system_type("/roms/none.7z");
		drop_archives();
		/* (an image's own first bytes say it is one) */
		add_archive("/roms/image.zip", flat, 2);
		memcpy(archives[0].data[1], "NAOMI", 5);
		memset(archives[0].data[1] + 0x30, ' ', 0x20);
		memcpy(archives[0].data[1] + 0x30, "FLAT GAME", 9);
		archives[0].data[1][0x428] = 4;
		system_type("/roms/image.zip");
		load("/roms/image.zip");
		/* a NAOMI 2's, a .dat */
		add_archive("/roms/image2.zip", dat, 1);
		memcpy(archives[1].data[0], "Naomi2", 6);
		system_type("/roms/image2.zip");
		/* looked at, and another loaded: dropped */
		system_type("/roms/image.zip");
		load("/roms/two.zip");
		drop_archives();
	}

	/* A .bin, and a .lst of pieces: one "null", one short */
	{
		memset(image, 0x11, sizeof(image));
		memcpy(image, "Naomi2", 6);
		memset(image + 0x30, ' ', 0x20);
		memcpy(image + 0x30, "BIN GAME", 8);
		write_file("game.bin", image, sizeof(image));
		snprintf(path, sizeof(path), "%s/game.bin", dir);
		system_type(path);
		settings.bios.UseRealBios = false;
		load(path);
		write_file("p1.ic22", image, 0x1000);
		memset(image, 0x22, sizeof(image));
		write_file("p2.ic1", image, 0x800);
		{
			static const char list[] =
				"Some Game\r\n"
				"\"p1.ic22\",0,1000\n"
				"\"null\",1000,1000\n"
				"bad line\n"
				"\"p2.ic1\",2000,800\n"
				"\"far.ic9\",1ffff000,2000\n";
			write_file("game.lst", list, sizeof(list) - 1);
		}
		snprintf(path, sizeof(path), "%s/game.lst", dir);
		load(path);
		{
			static const char list[] = "Game\n\"p2.ic1\",0,2000\n";
			write_file("short.lst", list, sizeof(list) - 1);
		}
		snprintf(path2, sizeof(path2), "%s/short.lst", dir);
		load(path2);
		{
			static const char list[] = "Game\n\"missing.ic1\",0,1000\n";
			write_file("missing.lst", list, sizeof(list) - 1);
		}
		snprintf(path2, sizeof(path2), "%s/missing.lst", dir);
		load(path2);
		{
			static const char list[] = "no end of line";
			write_file("noline.lst", list, sizeof(list) - 1);
		}
		snprintf(path2, sizeof(path2), "%s/noline.lst", dir);
		load(path2);
		settings.bios.UseRealBios = true;
		snprintf(path2, sizeof(path2), "rm -rf '%s'", dir);
		if (system(path2) != 0)
			printf("FAIL: the files stay\n");
	}

	load("/roms/unknown.zip");
	free(arena);
	printf("done\n");
	return 0;
}
