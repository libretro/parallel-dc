/* The games' fixes and widescreen cheats (core/cheats.c), by themselves:
 * which game gets which, by its product number or arcade name and set,
 * what they write and over what, and what goes back when the widescreen
 * option is turned off (sh tools/cheats/cheats_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "cheats.h"
#include "paths.h"
#include "reios/reios.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/mem/_vmem.h"
#include <file/file_path.h>

ip_meta_t ip_meta;
char naomi_game_id[33];
char *game_data;
settings_t settings;
unsigned RAM_SIZE = 16 * 1024 * 1024;

/* (no log) */
retro_log_printf_t log_cb;

const char *path_basename(const char *path)
{
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

char *path_remove_extension(char *path)
{
	char *s = strrchr(path, '.');
	if (s)
		*s = '\0';
	return path;
}

/* Main memory, at 0x8C000000 */
#define MEM_SIZE (32 * 1024 * 1024)
static u8 *mem;
static u8 *at(u32 a) { return &mem[(a - 0x8C000000) % MEM_SIZE]; }
u8 DYNACALL _vmem_ReadMem8(u32 a) { return *at(a); }
u16 DYNACALL _vmem_ReadMem16(u32 a) { u16 v; memcpy(&v, at(a), 2); return v; }
u32 DYNACALL _vmem_ReadMem32(u32 a) { u32 v; memcpy(&v, at(a), 4); return v; }
void DYNACALL _vmem_WriteMem8(u32 a, u8 v) { *at(a) = v; }
void DYNACALL _vmem_WriteMem16(u32 a, u16 v) { memcpy(at(a), &v, 2); }
void DYNACALL _vmem_WriteMem32(u32 a, u32 v) { memcpy(at(a), &v, 4); }

static u32 rd32(u32 off) { return _vmem_ReadMem32(0x8C000000 + off); }
static u16 rd16(u32 off) { return _vmem_ReadMem16(0x8C000000 + off); }
static void wr32(u32 off, u32 v) { _vmem_WriteMem32(0x8C000000 + off, v); }
static void wr16(u32 off, u16 v) { _vmem_WriteMem16(0x8C000000 + off, v); }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void dreamcast(const char product[10], const char *disk_num, bool wide)
{
	memset(mem, 0, MEM_SIZE);
	memset(&ip_meta, 0, sizeof(ip_meta));
	memcpy(ip_meta.product_number, product, 10);
	memcpy(ip_meta.disk_num, disk_num, 5);
	settings.System = DC_PLATFORM_DREAMCAST;
	settings.rend.WidescreenGameHacks = wide;
	RAM_SIZE = 16 * 1024 * 1024;
}

static void naomi(const char *name, const char *file, bool wide)
{
	memset(mem, 0, MEM_SIZE);
	memset(&ip_meta, 0, sizeof(ip_meta));
	strcpy(naomi_game_id, name);
	game_data = (char *)file;
	settings.System = DC_PLATFORM_NAOMI;
	settings.rend.WidescreenGameHacks = wide;
	RAM_SIZE = 32 * 1024 * 1024;
}

int main(void)
{
	mem = (u8 *)calloc(1, MEM_SIZE);
	if (!mem)
		return 1;

	/* A fix by the product number, the spaces after it left out */
	dreamcast("T-8120N   ", "1/1  ", false);
	CHECK(!cheats_reset());
	cheats_apply();
	CHECK(rd32(0x30B8CC) == 0x42040000);
	/* a product number with zeros in it is not the table's */
	dreamcast("T-8120N\0\0\0", "1/1  ", false);
	cheats_reset();
	cheats_apply();
	CHECK(rd32(0x30B8CC) == 0);
	/* one written only over what it is meant for */
	dreamcast("MK-51140  ", "1/1  ", false);
	cheats_reset();
	cheats_apply();
	CHECK(rd16(0x085D2C) == 0);
	wr16(0x085D2C, 0x3630);
	cheats_apply();
	CHECK(rd16(0x085D2C) == 0x3330);
	/* every one of a game's, and none of the next game's */
	dreamcast("HDR-0100  ", "1/1  ", false);
	wr32(0x0BA980, 0x2FD62FE6);
	wr32(0x0A2D3C, 0xED048900);
	wr32(0x03AD30, 0x2FD62FE6);
	cheats_reset();
	cheats_apply();
	CHECK(rd32(0x0BA980) == 0xE000000B && rd32(0x0A2D3C) == 0xED048B00);
	CHECK(rd32(0x03AD30) == 0x2FD62FE6);

	/* Shenmue II's widescreen, over the value it is meant for, and that
	 * value back when the option is turned off */
	dreamcast("HDR-0164  ", "1/4  ", true);
	CHECK(cheats_reset());
	wr32(0x30D67C, 0x12345678);
	cheats_apply();
	CHECK(rd32(0x30D67C) == 0x12345678);
	wr32(0x30D67C, 0x43A00000);
	cheats_apply();
	CHECK(rd32(0x30D67C) == 0x43700000);
	settings.rend.WidescreenGameHacks = false;
	CHECK(!cheats_change());
	CHECK(rd32(0x30D67C) == 0x43A00000);
	/* not on its fourth disc */
	dreamcast("HDR-0164  ", "4/4  ", true);
	CHECK(!cheats_reset());

	/* An arcade game's, by its name and set; and a fix by its name */
	naomi("INITIAL D Ver.2", "/roms/initdv2j.zip", true);
	CHECK(cheats_reset());
	cheats_apply();
	CHECK(rd32(0x1B4F34) == 0x3FE38E39 && rd32(0x1B4F74) == 0);
	naomi("INITIAL D Ver.2", "/roms/unknown.zip", true);
	CHECK(!cheats_reset());
	naomi("THE KING OF ROUTE66", "/roms/kingrt66.zip", false);
	cheats_reset();
	cheats_apply();
	CHECK(rd32(0x023EE0) == 0x0009000B);
	/* none past the end of the machine's memory */
	naomi("AIRLINE PILOTS IN JAPAN", "/roms/alpilotj.zip", true);
	CHECK(cheats_reset());
	RAM_SIZE = 16 * 1024 * 1024;
	CHECK(!cheats_reset());

	free(mem);
	if (fails)
	{
		printf("cheats_test: %d failed\n", fails);
		return 1;
	}
	printf("cheats_test: each game's fixes and cheats as they should be\n");
	return 0;
}
