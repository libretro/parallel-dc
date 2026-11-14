/*
	bios & nvmem related code
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <streams/file_stream.h>

#include "flashrom.h"

void memchip_free(MemChip *chip)
{
	free(chip->data);
	chip->data = NULL;
	chip->size = 0;
	chip->mask = 0;
}

void memchip_allocate(MemChip *chip, u32 size)
{
	/* Zeroed: a chip with no file to load (a NAOMI's SRAM on the first
	 * boot) would otherwise start with whatever the heap held. */
	free(chip->data);
	chip->data = (u8 *)calloc(size, 1);
	chip->size = size;
	chip->mask = size - 1;	/* must be power of 2 */
	/* (as a new chip: none of the last machine's protection or state) */
	chip->write_protect_size = 0;
	chip->state = FS_Normal;
}

/* What the flash answers in its select mode, which a read ends */
static u8 flash_select_read(MemChip *flash, u32 addr)
{
	flash->state = FS_Normal;
	switch (addr & 0x43)
	{
	case 0:	/* manufacturer's code */
		return 4;		/* or 0x20 or 1 */
	case 1:	/* device code */
		return 0xb0;	/* or 0x40 or 0x3e */
	case 2:	/* sector protection verification */
		DEBUG_LOG(FLASHROM, "Sector protection address %x\n", addr);
		return (addr & 0x1e000) == 0x1a000;
	default:
		WARN_LOG(FLASHROM, "SelectMode unknown address %x\n", addr);
		return 0;
	}
}

u8 memchip_read8(MemChip *chip, u32 addr)
{
	if (chip->kind == MEMCHIP_FLASH && chip->state == FS_SelectMode)
		return flash_select_read(chip, addr);
	return chip->data[addr & chip->mask];
}

u32 memchip_read(MemChip *chip, u32 addr, u32 sz)
{
	const u8 *p;
	u32 rv = 0, i;

	addr &= chip->mask;
	/* (read where it is, but in the flash's select mode and across the
	 * chip's end, a byte at a time as it always was) */
	if ((chip->kind == MEMCHIP_FLASH && chip->state == FS_SelectMode) || addr + sz > chip->size)
	{
		for (i = 0; i < sz; i++)
			rv |= (u32)memchip_read8(chip, addr + i) << (i * 8);
		return rv;
	}
	p = chip->data + addr;
	switch (sz)
	{
	case 1:
		return p[0];
	case 2:
		return p[0] | ((u32)p[1] << 8);
	case 4:
		return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
	default:
		for (i = 0; i < sz; i++)
			rv |= (u32)p[i] << (i * 8);
		return rv;
	}
}

static void sram_write(MemChip *chip, u32 addr, u32 val, u32 sz)
{
	addr &= chip->mask;
	if (addr < chip->write_protect_size)
		return;
	switch (sz)
	{
	case 1:
		chip->data[addr] = (u8)val;
		return;
	case 2:
		*(u16 *)&chip->data[addr] = (u16)val;
		return;
	case 4:
		*(u32 *)&chip->data[addr] = val;
		return;
	default:
		die("invalid access size");
	}
}

static void flash_write(MemChip *flash, u32 addr, u32 val, u32 sz)
{
	u8 *data = flash->data;

	if (sz != 1)
	{
		INFO_LOG(FLASHROM, "invalid access size %d\n", sz);
		return;
	}

	addr &= flash->mask;

	switch (flash->state)
	{
	case FS_Normal:
		switch (val & 0xff)
		{
		case 0xf0:
		case 0xff:  /* reset chip mode */
			flash->state = FS_Normal;
			break;
		case 0xaa:  /* AMD ID select part 1 */
			if ((addr & 0xfff) == 0x555 || (addr & 0xfff) == 0xaaa)
				flash->state = FS_ReadAMDID1;
			break;
		default:
			INFO_LOG(FLASHROM, "Unknown FlashWrite mode: %x\n", val);
			break;
		}
		break;

	case FS_ReadAMDID1:
		if ((addr & 0xffff) == 0x02aa && (val & 0xff) == 0x55)
			flash->state = FS_ReadAMDID2;
		else if ((addr & 0xffff) == 0x2aaa && (val & 0xff) == 0x55)
			flash->state = FS_ReadAMDID2;
		else if ((addr & 0xfff) == 0x555 && (val & 0xff) == 0x55)
			flash->state = FS_ReadAMDID2;
		else
		{
			if (val != 0xf0)
				WARN_LOG(FLASHROM, "FlashRom: ReadAMDID1 unexpected write @ %x: %x\n", addr, val);
			flash->state = FS_Normal;
		}
		break;

	case FS_ReadAMDID2:
		if ((addr & 0xffff) == 0x0555 && (val & 0xff) == 0x80)
			flash->state = FS_EraseAMD1;
		else if ((addr & 0xffff) == 0x5555 && (val & 0xff) == 0x80)
			flash->state = FS_EraseAMD1;
		else if ((addr & 0xfff) == 0xaaa && (val & 0xff) == 0x80)
			flash->state = FS_EraseAMD1;
		else if ((addr & 0xffff) == 0x0555 && (val & 0xff) == 0xa0)
			flash->state = FS_ByteProgram;
		else if ((addr & 0xffff) == 0x5555 && (val & 0xff) == 0xa0)
			flash->state = FS_ByteProgram;
		else if ((addr & 0xfff) == 0xaaa && (val & 0xff) == 0xa0)
			flash->state = FS_ByteProgram;
		else if ((addr & 0xffff) == 0x5555 && (val & 0xff) == 0x90)
			flash->state = FS_SelectMode;
		else
		{
			if (val != 0xf0)
				WARN_LOG(FLASHROM, "FlashRom: ReadAMDID2 unexpected write @ %x: %x\n", addr, val);
			flash->state = FS_Normal;
		}
		break;

	case FS_ByteProgram:
		if ((addr & 0x1e000) != 0x1a000 && addr >= flash->write_protect_size)
			data[addr] &= val;
		flash->state = FS_Normal;
		break;

	case FS_EraseAMD1:
		if ((addr & 0xfff) == 0x555 && (val & 0xff) == 0xaa)
			flash->state = FS_EraseAMD2;
		else if ((addr & 0xfff) == 0xaaa && (val & 0xff) == 0xaa)
			flash->state = FS_EraseAMD2;
		else
		{
			if (val != 0xf0)
				WARN_LOG(FLASHROM, "FlashRom: EraseAMD1 unexpected write @ %x: %x\n", addr, val);
			flash->state = FS_Normal;
		}
		break;

	case FS_EraseAMD2:
		if ((addr & 0xffff) == 0x02aa && (val & 0xff) == 0x55)
			flash->state = FS_EraseAMD3;
		else if ((addr & 0xffff) == 0x2aaa && (val & 0xff) == 0x55)
			flash->state = FS_EraseAMD3;
		else if ((addr & 0xfff) == 0x555 && (val & 0xff) == 0x55)
			flash->state = FS_EraseAMD3;
		else
		{
			if (val != 0xf0)
				WARN_LOG(FLASHROM, "FlashRom: EraseAMD2 unexpected write @ %x: %x\n", addr, val);
			flash->state = FS_Normal;
		}
		break;

	case FS_EraseAMD3:
		if (((addr & 0xfff) == 0x555 && (val & 0xff) == 0x10)
			|| ((addr & 0xfff) == 0xaaa && (val & 0xff) == 0x10))
		{
			/* chip erase: all but the write-protected area (0x1a000, 8 KB),
			 * erased around it */
			INFO_LOG(FLASHROM, "Erasing Chip!\n");
			if (flash->write_protect_size < 0x1a000)
				memset(data + flash->write_protect_size, 0xff, 0x1a000 - flash->write_protect_size);
			if (flash->size > 0x1c000)
			{
				u32 from = flash->write_protect_size > 0x1c000 ? flash->write_protect_size : 0x1c000;
				if (from < flash->size)
					memset(data + from, 0xff, flash->size - from);
			}
			flash->state = FS_Normal;
		}
		else if ((val & 0xff) == 0x30)
		{
			/* sector erase */
			if (addr >= flash->write_protect_size)
			{
				u8 *start;
				u32 len;
				switch (addr & ~0x1FFF)
				{
				case 0x00000:	/* SA0 */
					start = &data[0];
					len = 0x10000;
					break;
				case 0x10000:	/* SA1 */
					start = &data[0x10000];
					len = 0x8000;
					break;
				case 0x18000:	/* SA2 */
					start = &data[0x18000];
					len = 0x2000;
					break;
				case 0x1c000:	/* SA4 */
					start = &data[0x1c000];
					len = 0x4000;
					break;
				default:		/* SA3 (0x1a000) and what there is not */
					start = NULL;
					len = 0;
					break;
				}
				INFO_LOG(FLASHROM, "Erase Sector %08X!\n", addr);
				if (start != NULL)
					memset(start, 0xFF, len);
			}
		}
		else if (val != 0xf0)
			WARN_LOG(FLASHROM, "FlashRom: EraseAMD3 unexpected write @ %x: %x\n", addr, val);
		flash->state = FS_Normal;
		break;
	default:
		WARN_LOG(FLASHROM, "FlashRom: invalid state. write @ %x: %x\n", addr, val);
		flash->state = FS_Normal;
		break;
	}
}

void memchip_write(MemChip *chip, u32 addr, u32 val, u32 sz)
{
	switch (chip->kind)
	{
	case MEMCHIP_SRAM:
		sram_write(chip, addr, val, sz);
		break;
	case MEMCHIP_FLASH:
		flash_write(chip, addr, val, sz);
		break;
	default:
		die("Method not supported");
		break;
	}
}

void memchip_reset(MemChip *chip)
{
	/* (the flash's command state: a ROM and an SRAM have none) */
	chip->state = FS_Normal;
}

bool memchip_load(MemChip *chip, const char *file)
{
	RFILE *f;
	bool rv;
	const u32 len = chip->size - chip->write_protect_size;

	if (chip->data == NULL || file == NULL || file[0] == '\0')
		return false;
	f = filestream_open(file, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f == NULL)
		return false;
	/* (straight into the chip) */
	rv = filestream_read(f, chip->data + chip->write_protect_size, len) == (int64_t)len;
	filestream_close(f);
	if (rv && file != chip->load_filename)
	{
		chip->load_filename[0] = '\0';
		strncat(chip->load_filename, file, sizeof(chip->load_filename) - 1);
	}
	return rv;
}

bool memchip_reload(MemChip *chip)
{
	return memchip_load(chip, chip->load_filename);
}

void memchip_save(MemChip *chip, const char *file)
{
	RFILE *f;

	if (chip->data == NULL)
		return;
	f = filestream_open(file, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f)
	{
		filestream_write(f, chip->data + chip->write_protect_size, chip->size - chip->write_protect_size);
		filestream_close(f);
	}
}

/* @a, @b and @c one after the other in @out, cut short at its size */
static void path3(char *out, size_t size, const char *a, const char *b, const char *c)
{
	out[0] = '\0';
	strncat(out, a, size - 1);
	strncat(out, b, size - 1 - strlen(out));
	strncat(out, c, size - 1 - strlen(out));
}

bool memchip_load_named(MemChip *chip, const char *root, const char *prefix,
		const char *names, const char *title)
{
	char path[PATH_MAX_LENGTH];
	char name[512];
	const char *curr = names;

	for (;;)
	{
		const char *next = strchr(curr, ';');
		size_t len = next ? (size_t)(next - curr) : strlen(curr);

		if (len > sizeof(name) - 1)
			len = sizeof(name) - 1;
		memcpy(name, curr, len);
		name[len] = '\0';
		if (name[0] == '%')
			path3(path, sizeof(path), root, prefix, name + 1);
		else
			path3(path, sizeof(path), root, "", name);
		if (memchip_load(chip, path))
		{
			INFO_LOG(FLASHROM, "Loaded %s as %s\n", path, title);
			return true;
		}
		if (next == NULL)
			return false;
		curr = next + 1;
	}
}

void memchip_save_named(MemChip *chip, const char *root, const char *prefix,
		const char *name, const char *title)
{
	char path[PATH_MAX_LENGTH];

	path3(path, sizeof(path), root, prefix, name);
	memchip_save(chip, path);
	INFO_LOG(FLASHROM, "Saved %s as %s\n", path, title);
}

/*
 * Flash block handling code borrowed from redream (https://github.com/inolen/redream)
 */

void flash_partition_info(int part_id, int *offset, int *size)
{
	switch (part_id)
	{
	case FLASH_PT_FACTORY:
		*offset = 0x1a000;
		*size = 8 * 1024;
		break;
	case FLASH_PT_RESERVED:
		*offset = 0x18000;
		*size = 8 * 1024;
		break;
	case FLASH_PT_USER:
		*offset = 0x1c000;
		*size = 16 * 1024;
		break;
	case FLASH_PT_GAME:
		*offset = 0x10000;
		*size = 32 * 1024;
		break;
	case FLASH_PT_UNKNOWN:
		*offset = 0x00000;
		*size = 64 * 1024;
		break;
	default:
		die("unknown partition");
		break;
	}
}

static int crc_block(const flash_user_block *block)
{
	const u8 *buf = (const u8 *)block;
	int size = 62;
	/* (unsigned: the CRC's shifts go past bit 31 of an int) */
	u32 n = 0xffff;
	int i, c;

	for (i = 0; i < size; i++)
	{
		n ^= ((u32)buf[i] << 8);
		for (c = 0; c < 8; c++)
		{
			if (n & 0x8000)
				n = (n << 1) ^ 4129;
			else
				n = (n << 1);
		}
	}

	return (int)((~n) & 0xffff);
}

static int num_user_blocks(u32 size)
{
	/* the physical blocks, less the bitmaps' and the header */
	return (int)(size / FLASH_BLOCK_SIZE)
		- (int)((size + FLASH_BITMAP_BYTES - 1) / FLASH_BITMAP_BYTES) - 1;
}

static int is_allocated(const u8 *bitmap, u32 phys_id)
{
	int index = (phys_id - 1) % FLASH_BITMAP_BLOCKS;
	return (bitmap[index / 8] & (0x80 >> (index % 8))) == 0x0;
}

static void set_allocated(u8 *bitmap, u32 phys_id)
{
	int index = (phys_id - 1) % FLASH_BITMAP_BLOCKS;
	bitmap[index / 8] &= ~(0x80 >> (index % 8));
}

static void write_physical_block(MemChip *flash, u32 offset, u32 phys_id, const void *data)
{
	memcpy(&flash->data[offset + phys_id * FLASH_BLOCK_SIZE], data, FLASH_BLOCK_SIZE);
}

static void read_physical_block(MemChip *flash, u32 offset, u32 phys_id, void *data)
{
	memcpy(data, &flash->data[offset + phys_id * FLASH_BLOCK_SIZE], FLASH_BLOCK_SIZE);
}

static int validate_header_at(MemChip *flash, u32 offset, u32 part_id)
{
	flash_header_block header;
	read_physical_block(flash, offset, 0, &header);

	if (memcmp(header.magic, FLASH_MAGIC_COOKIE, sizeof(header.magic)) != 0)
		return 0;
	if (header.part_id != part_id)
		return 0;
	return 1;
}

static int validate_header(MemChip *flash, u32 part_id)
{
	int offset, size;
	flash_partition_info(part_id, &offset, &size);
	return validate_header_at(flash, offset, part_id);
}

static int lookup_block(MemChip *flash, u32 offset, u32 size, u32 block_id)
{
	u8 bitmap[FLASH_BLOCK_SIZE];
	int blocks = num_user_blocks(size);
	int bitmap_id = 1 + blocks;
	int phys_id = 1;
	int phys_end = bitmap_id;
	/* in order to lookup a logical block, all physical blocks must be
	 * iterated. since physical blocks are allocated linearly, the physical
	 * block with the highest address takes precedence */
	int result = 0;

	while (phys_id < phys_end)
	{
		flash_user_block user;

		/* read the next bitmap every FLASH_BITMAP_BLOCKS */
		if (phys_id % FLASH_BITMAP_BLOCKS == 1)
			read_physical_block(flash, offset, bitmap_id++, bitmap);

		/* being that physical blocks are allocated linearly, stop
		 * processing once the first unallocated block is hit */
		if (!is_allocated(bitmap, phys_id))
			break;

		read_physical_block(flash, offset, phys_id, &user);
		if (user.block_id == block_id)
		{
			if (user.crc != crc_block(&user))
				WARN_LOG(FLASHROM, "flash_lookup_block physical block %d has an invalid crc\n", phys_id);
			else
				result = phys_id;
		}

		phys_id++;
	}

	return result;
}

static int alloc_block(MemChip *flash, u32 offset, u32 size)
{
	u8 bitmap[FLASH_BLOCK_SIZE];
	int blocks = num_user_blocks(size);
	int bitmap_id = blocks;
	int phys_id = 1;
	int phys_end = 1 + blocks;

	while (phys_id < phys_end)
	{
		u16 id;

		/* read the next bitmap every FLASH_BITMAP_BLOCKS */
		if (phys_id % FLASH_BITMAP_BLOCKS == 1)
			read_physical_block(flash, offset, ++bitmap_id, bitmap);

		/* use the first unallocated block */
		if (!is_allocated(bitmap, phys_id))
			break;
		/* if the current block has been rewritten, use it */
		memcpy(&id, &flash->data[offset + phys_id * FLASH_BLOCK_SIZE], sizeof(id));
		if (lookup_block(flash, offset, size, id) != phys_id)
			break;

		phys_id++;
	}

	if (phys_id >= phys_end)
	{
		WARN_LOG(FLASHROM, "Cannot allocate block in flash. Full?\n");
		return 0;
	}

	/* mark the block as allocated */
	set_allocated(bitmap, phys_id);
	write_physical_block(flash, offset, bitmap_id, bitmap);

	return phys_id;
}

int flash_write_block(MemChip *flash, u32 part_id, u32 block_id, const void *data)
{
	int offset, size, phys_id;
	flash_user_block user;

	flash_partition_info(part_id, &offset, &size);
	if (!validate_header_at(flash, offset, part_id))
		return 0;

	/* the real system libraries allocate and write to a new physical block
	 * each time a logical block is updated. the reason being that, flash
	 * memory can only be programmed once, and after that the entire sector
	 * must be reset in order to reprogram it. flash storage has a finite
	 * number of these erase operations before its integrity deteriorates,
	 * so the libraries try to minimize how often they occur by writing to a
	 * new physical block until the partition is completely full
	 *
	 * this limitation of the original hardware isn't a problem for us, so
	 * try and just update an existing logical block if it exists */
	phys_id = lookup_block(flash, offset, size, block_id);
	if (!phys_id)
	{
		phys_id = alloc_block(flash, offset, size);
		if (!phys_id)
			return 0;
	}

	/* update the block's crc before writing it back out */
	memcpy(&user, data, sizeof(user));
	user.block_id = block_id;
	user.crc = crc_block(&user);

	write_physical_block(flash, offset, phys_id, &user);

	return 1;
}

int flash_read_block(MemChip *flash, u32 part_id, u32 block_id, void *data)
{
	int offset, size, phys_id;

	flash_partition_info(part_id, &offset, &size);
	if (!validate_header_at(flash, offset, part_id))
		return 0;

	phys_id = lookup_block(flash, offset, size, block_id);
	if (!phys_id)
		return 0;

	read_physical_block(flash, offset, phys_id, data);

	return 1;
}

static void erase_partition(MemChip *flash, u32 part_id)
{
	int offset, size;
	flash_partition_info(part_id, &offset, &size);
	memset(flash->data + offset, 0xFF, size);
}

static void write_header(MemChip *flash, int part_id)
{
	int offset, size;
	flash_header_block header;

	flash_partition_info(part_id, &offset, &size);
	memset(&header, 0xff, sizeof(header));
	memcpy(header.magic, FLASH_MAGIC_COOKIE, sizeof(header.magic));
	header.part_id = part_id;
	header.version = 0;

	write_physical_block(flash, offset, 0, &header);
}

void flash_validate(MemChip *flash)
{
	/* validate partition 0 (factory settings) */
	bool valid = true;
	char sysinfo[16];
	int i;

	for (i = 0; i < (int)sizeof(sysinfo); i++)
		sysinfo[i] = memchip_read8(flash, 0x1a000 + i);
	valid = valid && memcmp(&sysinfo[5], "Dreamcast  ", 11) == 0;

	for (i = 0; i < (int)sizeof(sysinfo); i++)
		sysinfo[i] = memchip_read8(flash, 0x1a0a0 + i);
	valid = valid && memcmp(&sysinfo[5], "Dreamcast  ", 11) == 0;

	if (!valid)
	{
		INFO_LOG(FLASHROM, "DCFlashChip::Validate resetting FLASH_PT_FACTORY\n");

		memcpy(sysinfo, "00000Dreamcast  ", sizeof(sysinfo));
		erase_partition(flash, FLASH_PT_FACTORY);
		memcpy(flash->data + 0x1a000, sysinfo, sizeof(sysinfo));
		memcpy(flash->data + 0x1a0a0, sysinfo, sizeof(sysinfo));
	}

	/* validate partition 1 (reserved) */
	erase_partition(flash, FLASH_PT_RESERVED);

	/* validate partition 2 (user settings, block allocated) */
	if (!validate_header(flash, FLASH_PT_USER))
	{
		INFO_LOG(FLASHROM, "DCFlashChip::Validate resetting FLASH_PT_USER\n");
		erase_partition(flash, FLASH_PT_USER);
		write_header(flash, FLASH_PT_USER);
	}

	/* validate partition 3 (game settings, block allocated) */
	if (!validate_header(flash, FLASH_PT_GAME))
	{
		INFO_LOG(FLASHROM, "DCFlashChip::Validate resetting FLASH_PT_GAME\n");
		erase_partition(flash, FLASH_PT_GAME);
		write_header(flash, FLASH_PT_GAME);
	}

	/* validate partition 4 (unknown, block allocated) */
	if (!validate_header(flash, FLASH_PT_UNKNOWN))
	{
		INFO_LOG(FLASHROM, "DCFlashChip::Validate resetting FLASH_PT_UNKNOWN\n");
		erase_partition(flash, FLASH_PT_UNKNOWN);
		write_header(flash, FLASH_PT_UNKNOWN);
	}
}
