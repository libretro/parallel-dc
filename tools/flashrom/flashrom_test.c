/* The BIOS ROM, the SRAM and the Dreamcast's flash memory
 * (core/hw/flashrom/flashrom.c), by themselves: reads of every size, at the
 * end of a chip too; the SRAM's writes past its protected start; the
 * flash's commands - its ID, program, sector and chip erase, its select
 * mode - with what it does to unexpected writes; its partitions validated
 * and its blocks written and read until a partition is full; and the chips
 * loaded and saved, by name among several. All of it hashed against what
 * it always did (sh tools/flashrom/flashrom_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <streams/file_stream.h>
#include "hw/flashrom/flashrom.h"

/* (no log) */
retro_log_printf_t log_cb;
void os_DebugBreak(void) { abort(); }

/* libretro-common's files, as stdio has them */
struct RFILE { FILE *f; };

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	FILE *f = fopen(path, mode == RETRO_VFS_FILE_ACCESS_WRITE ? "wb" : "rb");
	RFILE *r;
	(void)hints;
	if (f == NULL)
		return NULL;
	r = (RFILE *)malloc(sizeof(*r));
	r->f = f;
	return r;
}

int64_t filestream_read(RFILE *r, void *data, int64_t len)
{
	return (int64_t)fread(data, 1, (size_t)len, r->f);
}

int64_t filestream_write(RFILE *r, const void *data, int64_t len)
{
	return (int64_t)fwrite(data, 1, (size_t)len, r->f);
}

int filestream_close(RFILE *r)
{
	fclose(r->f);
	free(r);
	return 0;
}

static unsigned h = 2166136261u;

/* FNV-1a */
static void mix(const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	while (n--)
	{
		h ^= *b++;
		h *= 16777619u;
	}
}

static void mix32(u32 v) { mix(&v, 4); }

static u32 seed = 1;
static u32 rnd(void)
{
	seed = seed * 1103515245u + 12345u;
	return seed >> 8;
}

static MemChip rom, sram, flash;

/* A command: its writes at the addresses the BIOS uses */
static void cmd(u32 base, const u32 *seq, int n)
{
	int i;
	for (i = 0; i < n; i++)
	{
		memchip_write(&flash, seq[2 * i], seq[2 * i + 1], 1);
		mix32(flash.state);
	}
	(void)base;
}

int main(int argc, char **argv)
{
	static const u32 id[] = { 0x5555, 0xaa, 0x2aaa, 0x55, 0x5555, 0x90 };
	static const u32 prog_hdr[] = { 0x5555, 0xaa, 0x2aaa, 0x55, 0x5555, 0xa0 };
	static const u32 erase_hdr[] = { 0x5555, 0xaa, 0x2aaa, 0x55, 0x5555, 0x80, 0x5555, 0xaa, 0x2aaa, 0x55 };
	const char *dir = argc > 1 ? argv[1] : "/tmp";
	u32 i, a, sizes[3] = { 1, 2, 4 };
	char path[1024];
	int r;

	/* ROM: reads of every size, across its end too */
	memchip_allocate(&rom, 0x10000);
	for (i = 0; i < rom.size; i++)
		rom.data[i] = (u8)(i * 7 + (i >> 8));
	for (i = 0; i < 3000; i++)
	{
		a = rnd() % (rom.size + 8);
		if (i < 16)
			a = rom.size - 4 + (i % 4);
		mix32(memchip_read(&rom, a, sizes[i % 3]));
	}

	/* SRAM: writes of every size, none in its protected start */
	sram.kind = MEMCHIP_SRAM;
	memchip_allocate(&sram, 0x8000);
	sram.write_protect_size = 0x100;
	for (i = 0; i < 3000; i++)
	{
		a = (rnd() % 0x8000) & ~(sizes[i % 3] - 1);
		memchip_write(&sram, a, rnd(), sizes[i % 3]);
	}
	mix(sram.data, sram.size);

	/* Flash: the commands, and writes of all kinds at the command addresses */
	flash.kind = MEMCHIP_FLASH;
	memchip_allocate(&flash, 0x20000);
	memset(flash.data, 0xff, flash.size);
	cmd(0, id, 3);
	for (i = 0; i < 6; i++)
		mix32(memchip_read8(&flash, 0x40 * (i & 1) + i));	/* the codes, then data */
	memchip_write(&flash, 0, 0xf0, 1);
	cmd(0, id, 3);
	mix32(memchip_read(&flash, 2, 4));	/* the select mode ends at the first byte */
	for (i = 0; i < 200; i++)
	{
		cmd(0, prog_hdr, 3);
		a = rnd() % flash.size;
		memchip_write(&flash, a, rnd(), 1);
		mix32(flash.state);
	}
	mix(flash.data, flash.size);
	/* a sector of each */
	for (i = 0; i < 5; i++)
	{
		static const u32 sect[5] = { 0x00000, 0x10000, 0x18000, 0x1a000, 0x1c000 };
		cmd(0, erase_hdr, 5);
		memchip_write(&flash, sect[i] + 0x123, 0x30, 1);
		mix(flash.data, flash.size);
		for (a = 0; a < 64; a++)
		{
			cmd(0, prog_hdr, 3);
			memchip_write(&flash, sect[i] + a, (u8)rnd(), 1);
		}
	}
	/* unexpected writes, every state's */
	for (i = 0; i < 4000; i++)
	{
		static const u32 addrs[] = { 0x555, 0xaaa, 0x2aa, 0x2aaa, 0x5555, 0x1a555, 0x0555 };
		static const u32 vals[] = { 0xaa, 0x55, 0x80, 0xa0, 0x90, 0x10, 0x30, 0xf0, 0xff, 0x12 };
		memchip_write(&flash, addrs[rnd() % 7], vals[rnd() % 10], (i % 97) ? 1 : 2);
		mix32(flash.state);
		if (flash.state == FS_SelectMode)
			mix32(memchip_read8(&flash, rnd() & 0x43));
	}
	memchip_reset(&flash);
	/* the whole chip, but its factory area */
	memset(flash.data, 0x5a, flash.size);
	cmd(0, erase_hdr, 5);
	memchip_write(&flash, 0x555, 0x10, 1);
	mix(flash.data, flash.size);
	/* and with a protected start: half, as an AtomisWave's, and ending
	 * in the factory area and past it */
	for (i = 0; i < 3; i++)
	{
		static const u32 wps[3] = { 0x10000, 0x1b000, 0x1d000 };
		memset(flash.data, 0x5a, flash.size);
		flash.write_protect_size = wps[i];
		cmd(0, erase_hdr, 5);
		memchip_write(&flash, 0x555, 0x10, 1);
		cmd(0, prog_hdr, 3);
		memchip_write(&flash, 0x8000, 0, 1);
		cmd(0, erase_hdr, 5);
		memchip_write(&flash, 0x1e123, 0x30, 1);
		mix(flash.data, flash.size);
	}
	flash.write_protect_size = 0;

	/* Partitions validated, from nothing and from something else */
	memset(flash.data, 0xff, flash.size);
	flash_validate(&flash);
	mix(flash.data, flash.size);
	for (i = 0; i < flash.size; i++)
		flash.data[i] = (u8)rnd();
	flash_validate(&flash);
	mix(flash.data, flash.size);
	/* blocks written, again and again, until a partition is full */
	for (i = 0; i < 600; i++)
	{
		u8 block[64], back[64];
		u32 part = (i % 3 == 0) ? FLASH_PT_USER : FLASH_PT_GAME;
		u32 id = rnd() % 40;
		for (a = 0; a < 64; a++)
			block[a] = (u8)rnd();
		r = flash_write_block(&flash, part, id, block);
		mix32((u32)r);
		memset(back, 0, sizeof(back));
		r = flash_read_block(&flash, part, id, back);
		mix32((u32)r);
		mix(back, sizeof(back));
	}
	mix32((u32)flash_read_block(&flash, FLASH_PT_FACTORY, 1, path));
	mix(flash.data, flash.size);
	{
		int off, size, p;
		for (p = 0; p < FLASH_PT_NUM; p++)
		{
			flash_partition_info(p, &off, &size);
			mix32((u32)off);
			mix32((u32)size);
		}
	}

	/* Loaded and saved: the first of the names there is, a prefix's or
	 * not; and reloaded */
	sprintf(path, "%s/", dir);
	memchip_save_named(&flash, path, "dc_", "nvmem.bin", "nvmem");
	memset(flash.data, 0, flash.size);
	mix32(memchip_load_named(&flash, path, "dc_", "%missing.bin;missing2.bin;%nvmem.bin;%other.bin", "nvram"));
	mix(flash.data, flash.size);
	mix32(memchip_load_named(&flash, path, "dc_", "%missing.bin;dc_nvmem.bin", "nvram"));
	mix32(memchip_load_named(&flash, path, "dc_", "%missing.bin", "nvram"));
	memset(flash.data, 0, flash.size);
	mix32(memchip_reload(&flash));
	mix(flash.data, flash.size);
	sprintf(path, "%s/dc_nvmem.bin", dir);
	remove(path);
	/* a file too short is not loaded */
	sprintf(path, "%s/short.bin", dir);
	memchip_save(&sram, path);
	mix32(memchip_load(&flash, path));
	remove(path);

	memchip_free(&rom);
	memchip_free(&sram);
	memchip_free(&flash);
	printf("flashrom: %08x\n", h);
	return 0;
}
