/*
	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
*/
/* What every cartridge has, a NAOMI ROM board's registers, and the boards
 * that are no more than that: an M2 board, and a flat image. */
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "naomi_cart.h"
#include "naomi_regs.h"
#include "naomi.h"
#include "decrypt.h"
#include "hw/holly/holly_intc.h"
#include "hw/mem/_vmem.h"
#include "archive/archive.h"

/* A board for @size bytes at @rom_ptr, its registers as they are when it
 * is switched on */
static void cart_setup(cartridge *c, const cartridge_ops *ops, u8 *rom_ptr, u32 size)
{
	c->ops = ops;
	c->rom_ptr = rom_ptr;
	c->rom_size = size;
	c->rom_pio_offset = 0;
	c->rom_pio_autoinc = false;
	c->dma_offset = 0;
	c->dma_count = 0xffff;
	c->key = 0;
}

void cart_construct(cartridge *c, const cartridge_ops *ops, u32 size)
{
	u8 *rom = (u8 *)malloc(size);
	/* Without its memory the cartridge is empty, and every read of it is
	 * one past its end. */
	cart_setup(c, ops, rom, rom != NULL ? size : 0);
	if (c->rom_size != 0)
		memset(c->rom_ptr, 0xff, c->rom_size);
}

void cart_destruct(cartridge *c)
{
	if (c->rom_ptr != NULL)
		free(c->rom_ptr);
	c->rom_ptr = NULL;
	c->rom_size = 0;
}

bool cart_init(cartridge *c)
{
	(void)c;
	return true;
}

void cart_nothing(cartridge *c)
{
	(void)c;
}

bool cart_read(cartridge *c, u32 offset, u32 size, void *dst)
{
	offset &= 0x1fffffff;
	if (offset >= c->rom_size || (offset + size) > c->rom_size)
	{
		/* Makes Outtrigger boot */
		INFO_LOG(NAOMI, "offset %d > %d\n", offset, c->rom_size);
		/* (as many bytes of ones as were asked for) */
		memset(dst, 0xff, size);
	}
	else
		memcpy(dst, &c->rom_ptr[offset], size);
	return true;
}

bool cart_write(cartridge *c, u32 offset, u32 size, u32 data)
{
	(void)c;
	(void)size;
	INFO_LOG(NAOMI, "Invalid write @ %08x data %x\n", offset, data);
	return false;
}

void *cart_get_ptr(cartridge *c, u32 offset, u32 *size)
{
	offset &= 0x1fffffff;
	if (offset >= c->rom_size || *size > c->rom_size - offset)
	{
		WARN_LOG(NAOMI, "Invalid naomi cart: offset %x size %x rom size %x\n", offset, *size, c->rom_size);
		*size = 0;
		return NULL;
	}
	return &c->rom_ptr[offset];
}

u32 cart_trim_id(char *id, const char *s, u32 len)
{
	while (len != 0 && s[len - 1] == ' ')
		len--;
	memcpy(id, s, len);
	id[len] = '\0';
	return len;
}

static const char ROM_TOO_SMALL[] = "(ROM too small)";

/* The name in the ROM's header, and how long it is */
static u32 cart_base_id(cartridge *c, char *id)
{
	static const char AWNAOMI[] = "AWNAOMI                         ";
	const char *name;
	if (c->rom_size < 0x30 + 0x20)
	{
		strcpy(id, ROM_TOO_SMALL);
		return sizeof(ROM_TOO_SMALL) - 1;
	}
	name = (const char *)c->rom_ptr + 0x30;
	if (memcmp(name, AWNAOMI, 0x20) == 0 && c->rom_size >= 0xff50)
		name = (const char *)c->rom_ptr + 0xff30;
	return cart_trim_id(id, name, 0x20);
}

void cart_game_id(cartridge *c, char *id)
{
	cart_base_id(c, id);
}

void cart_no_state(cartridge *c, void **data, unsigned int *total_size)
{
	(void)c;
	(void)data;
	(void)total_size;
}

void cart_set_key(cartridge *c, u32 key)
{
	(void)c;
	(void)key;
}

void cart_set_key_data(cartridge *c, u8 *key_data)
{
	(void)c;
	(void)key_data;
}

void cart_offset_changed(cartridge *c, u32 offset)
{
	(void)c;
	(void)offset;
}

/* A NAOMI ROM board */

void naomi_board_reset(cartridge *c)
{
	c->rom_pio_offset = 0;
	c->rom_pio_autoinc = false;
	c->dma_offset = 0;
	c->dma_count = 0xffff;
}

void naomi_board_set_key(cartridge *c, u32 key)
{
	c->key = key;
}

void *naomi_board_get_dma_ptr(cartridge *c, u32 *size)
{
	const u32 offset = c->dma_offset & 0x1fffffff;
	if (offset >= c->rom_size)
	{
		INFO_LOG(NAOMI, "Error: DmaOffset >= RomSize\n");
		*size = 0;
		return NULL;
	}
	if (*size > c->rom_size - offset)
		*size = c->rom_size - offset;
	return c->ops->get_ptr(c, c->dma_offset, size);
}

/* What a transfer has read is behind it: the next one goes on from there
 * unless the game says otherwise, and a transfer that get_dma_ptr() gives
 * in two pieces - at the end of the cartridge, or of a 4 MB bank - gets
 * the second piece and not the first again. */
void naomi_board_advance_ptr(cartridge *c, u32 size)
{
	c->dma_offset += size;
}

u32 naomi_board_read_mem(cartridge *c, u32 address, u32 size)
{
	switch (address & 255)
	{
	case 0x3c:	/* 5f703c: DIMM COMMAND */
		DEBUG_LOG(NAOMI, "DIMM COMMAND read<%d>\n", size);
		return 0xffff;
	case 0x40:	/* 5f7040: DIMM OFFSETL */
		DEBUG_LOG(NAOMI, "DIMM OFFSETL read<%d>\n", size);
		return reg_dimm_offsetl;
	case 0x44:	/* 5f7044: DIMM PARAMETERL */
		DEBUG_LOG(NAOMI, "DIMM PARAMETERL read<%d>\n", size);
		return reg_dimm_parameterl;
	case 0x48:	/* 5f7048: DIMM PARAMETERH */
		DEBUG_LOG(NAOMI, "DIMM PARAMETERH read<%d>\n", size);
		return reg_dimm_parameterh;
	case 0x4c:	/* 5f704c: DIMM STATUS */
		DEBUG_LOG(NAOMI, "DIMM STATUS read<%d>\n", size);
		return reg_dimm_status;

	case NAOMI_ROM_OFFSETH_addr & 255:
		return c->rom_pio_offset >> 16 | ((u32)c->rom_pio_autoinc << 15);

	case NAOMI_ROM_OFFSETL_addr & 255:
		return c->rom_pio_offset & 0xffff;

	case NAOMI_ROM_DATA_addr & 255:
		{
			u32 rv = 0;
			c->ops->read(c, c->rom_pio_offset, 2, &rv);
			if (c->rom_pio_autoinc)
				c->rom_pio_offset += 2;
			return rv;
		}

	case NAOMI_DMA_COUNT_addr & 255:
		return (u16)c->dma_count;

	case NAOMI_BOARDID_READ_addr & 255:
		return NaomiGameIDRead() ? 0x8000 : 0x0000;

	/* (no communication board: nothing there) */
	case NAOMI_COMM_OFFSET_addr & 255:
	case NAOMI_COMM_DATA_addr & 255:
		return 1;

	case NAOMI_DMA_OFFSETH_addr & 255:
		return c->dma_offset >> 16;
	case NAOMI_DMA_OFFSETL_addr & 255:
		return c->dma_offset & 0xffff;

	case NAOMI_BOARDID_WRITE_addr & 255:
		DEBUG_LOG(NAOMI, "naomi ReadBoardId: %X, %d\n", address, size);
		return 1;

	/* (the communication board's registers, 5f7018 to 5f7028, never get
	 * here on a NAOMI: see ReadMem_naomi()) */

	default:
		break;
	}
	DEBUG_LOG(NAOMI, "naomi?WTF? ReadMem: %X, %d\n", address, size);
	return 0xffff;
}

void naomi_board_write_mem(cartridge *c, u32 address, u32 data, u32 size)
{
	switch (address & 255)
	{
	case 0x3c:	/* 5f703c: DIMM COMMAND */
		reg_dimm_command = data;
		DEBUG_LOG(NAOMI, "DIMM COMMAND Write: %X <= %X, %d\n", address, data, size);
		return;
	case 0x40:	/* 5f7040: DIMM OFFSETL */
		reg_dimm_offsetl = data;
		DEBUG_LOG(NAOMI, "DIMM OFFSETL Write: %X <= %X, %d\n", address, data, size);
		return;
	case 0x44:	/* 5f7044: DIMM PARAMETERL */
		reg_dimm_parameterl = data;
		DEBUG_LOG(NAOMI, "DIMM PARAMETERL Write: %X <= %X, %d\n", address, data, size);
		return;
	case 0x48:	/* 5f7048: DIMM PARAMETERH */
		reg_dimm_parameterh = data;
		DEBUG_LOG(NAOMI, "DIMM PARAMETERH Write: %X <= %X, %d\n", address, data, size);
		return;
	case 0x4c:	/* 5f704c: DIMM STATUS */
		if (data & 0x100)
			asic_CancelInterrupt(holly_EXP_PCI);
		else if ((data & 1) == 0)
			naomi_process(reg_dimm_command, reg_dimm_offsetl, reg_dimm_parameterl, reg_dimm_parameterh);
		reg_dimm_status = data & ~0x100;
		DEBUG_LOG(NAOMI, "DIMM STATUS Write: %X <= %X, %d\n", address, data, size);
		return;

	/* These are known to be valid on normal ROMs and DIMM board */
	case NAOMI_ROM_OFFSETH_addr & 255:
		c->rom_pio_autoinc = (data & 0x8000) != 0;
		c->rom_pio_offset &= 0x0000ffff;
		c->rom_pio_offset |= (data << 16) & 0x7fff0000;
		c->ops->pio_offset_changed(c, c->rom_pio_offset);
		return;

	case NAOMI_ROM_OFFSETL_addr & 255:
		c->rom_pio_offset &= 0xffff0000;
		c->rom_pio_offset |= data;
		c->ops->pio_offset_changed(c, c->rom_pio_offset);
		return;

	case NAOMI_ROM_DATA_addr & 255:
		c->ops->write(c, c->rom_pio_offset, size, data);
		if (c->rom_pio_autoinc)
			c->rom_pio_offset += 2;
		return;

	case NAOMI_DMA_OFFSETH_addr & 255:
		c->dma_offset &= 0x0000ffff;
		c->dma_offset |= (data & 0x7fff) << 16;
		c->ops->dma_offset_changed(c, c->dma_offset);
		return;

	case NAOMI_DMA_OFFSETL_addr & 255:
		c->dma_offset &= 0xffff0000;
		c->dma_offset |= data;
		c->ops->dma_offset_changed(c, c->dma_offset);
		return;

	case NAOMI_DMA_COUNT_addr & 255:
		c->dma_count = data;
		return;

	case NAOMI_STATUS_LEDS_addr & 255:
		return;

	case NAOMI_BOARDID_WRITE_addr & 255:
		NaomiGameIDWrite((u16)data);
		return;

	/* (no communication board: nothing there) */
	case NAOMI_COMM_OFFSET_addr & 255:
	case NAOMI_COMM_DATA_addr & 255:
		return;

	case NAOMI_BOARDID_READ_addr & 255:
		DEBUG_LOG(NAOMI, "naomi WriteMem: %X <= %X, %d\n", address, data, size);
		return;

	default:
		break;
	}
	DEBUG_LOG(NAOMI, "naomi?WTF? WriteMem: %X <= %X, %d\n", address, data, size);
}

void naomi_board_serialize(cartridge *c, void **data, unsigned int *total_size)
{
	LIBRETRO_S(c->rom_pio_offset);
	LIBRETRO_S(c->rom_pio_autoinc);
	LIBRETRO_S(c->dma_offset);
	LIBRETRO_S(c->dma_count);
}

void naomi_board_unserialize(cartridge *c, void **data, unsigned int *total_size)
{
	LIBRETRO_US(c->rom_pio_offset);
	LIBRETRO_US(c->rom_pio_autoinc);
	LIBRETRO_US(c->dma_offset);
	LIBRETRO_US(c->dma_count);
}

/* An M2 board: the ROMs, which a game reads 4 MB at a time or as one, and
 * the decryption chip, with its RAM */

typedef struct
{
	cartridge base;
	u8 ram[64 * 1024];
} m2_cart;

static m2_cart m2;

static bool m2_read(cartridge *c, u32 offset, u32 size, void *dst)
{
	if (offset & 0x40000000)
	{
		if (offset == 0x4001fffe)
		{
			u16 data;
			cyptoSetKey(c->key);
			data = cryptoDecrypt();
			*(u16 *)dst = data;
			return true;
		}
		INFO_LOG(NAOMI, "Invalid read @ %08x\n", offset);
		return false;
	}
	else if (!(c->rom_pio_offset & 0x20000000))
		/* 4MB mode */
		offset = (offset & 0x103fffff) | ((offset & 0x07c00000) << 1);
	return cart_read(c, offset, size, dst);
}

static void *m2_get_dma_ptr(cartridge *c, u32 *size)
{
	u32 offset4mb;
	if (c->rom_pio_offset & 0x20000000)
		return naomi_board_get_dma_ptr(c, size);

	/* 4MB mode */
	offset4mb = (c->dma_offset & 0x103fffff) | ((c->dma_offset & 0x07c00000) << 1);
	if (*size > 0x400000 - (offset4mb & 0x3fffff))
		*size = 0x400000 - (offset4mb & 0x3fffff);
	if (*size > c->rom_size - offset4mb)
		*size = c->rom_size - offset4mb;
	return c->ops->get_ptr(c, offset4mb, size);
}

static bool m2_write(cartridge *c, u32 offset, u32 size, u32 data)
{
	if (offset & 0x40000000)
	{
		if (offset & 0x00020000)
		{
			/* (a word of it: an odd offset is the word's, not one past the RAM) */
			offset &= sizeof(m2.ram) - 2;
			m2.ram[offset] = (u8)data;
			m2.ram[offset + 1] = (u8)(data >> 8);
			return true;
		}
		switch (offset & 0x1ffff)
		{
		case 0x1fff8:
			cyptoSetLowAddr(data);
			return true;
		case 0x1fffa:
			cyptoSetHighAddr(data);
			return true;
		case 0x1fffc:
			cyptoSetSubkey(data);
			return true;
		}
	}
	return cart_write(c, offset, size, data);
}

u16 naomi_m2_read_ciphered(u32 offset)
{
	const cartridge *c = &m2.base;
	if ((offset & 0xffff0000) == 0x01000000)
	{
		const int base = 2 * (offset & 0x7fff);
		return m2.ram[base + 1] | (m2.ram[base] << 8);
	}
	/* Past the end of the ROMs it reads as all ones, as m2_read() does. */
	if (offset >= c->rom_size / 2)
		return 0xffff;
	return c->rom_ptr[2 * offset + 1] | (c->rom_ptr[2 * offset] << 8);
}

static void m2_game_id(cartridge *c, char *id)
{
	const u32 len = cart_base_id(c, id);
	if ((len < 2 || ((u8)id[0] == 0xff && (u8)id[1] == 0xff)) && c->rom_size >= 0x800050)
		cart_trim_id(id, (const char *)c->rom_ptr + 0x800030, 0x20);
}

static void m2_serialize(cartridge *c, void **data, unsigned int *total_size)
{
	LIBRETRO_S(m2.ram);
	naomi_board_serialize(c, data, total_size);
}

static void m2_unserialize(cartridge *c, void **data, unsigned int *total_size)
{
	LIBRETRO_US(m2.ram);
	naomi_board_unserialize(c, data, total_size);
}

/* The decryption chip, which a game reads a word at a time: where it is in
 * the stream it was last given, so that a state taken between two words
 * goes on with the next one. */
static void m2_serialize_v21(cartridge *c, void **data, unsigned int *total_size)
{
	(void)c;
	cryptoSerialize(data, total_size);
}

static void m2_unserialize_v21(cartridge *c, void **data, unsigned int *total_size)
{
	(void)c;
	cryptoUnserialize(data, total_size);
}

static const cartridge_ops m2_ops = {
	cart_init,
	naomi_board_reset,
	naomi_board_read_mem,
	naomi_board_write_mem,
	m2_read,
	m2_write,
	cart_get_ptr,
	m2_get_dma_ptr,
	naomi_board_advance_ptr,
	m2_game_id,
	m2_serialize,
	m2_unserialize,
	m2_serialize_v21,
	m2_unserialize_v21,
	naomi_board_set_key,
	cart_set_key_data,
	cart_offset_changed,
	cart_offset_changed,
	cart_destruct
};

cartridge *m2_cart_open(u32 size)
{
	cart_construct(&m2.base, &m2_ops, size);
	/* (its RAM starts as zeroes, so that two machines that should be the
	 * same, in netplay, are) */
	memset(m2.ram, 0, sizeof(m2.ram));
	return &m2.base;
}

/* A flat image, mapped by the loader */

static cartridge decrypted;

static void decrypted_close(cartridge *c)
{
	mem_region_release(c->rom_ptr, c->rom_size);
	c->rom_ptr = NULL;
	c->rom_size = 0;
}

static const cartridge_ops decrypted_ops = {
	cart_init,
	naomi_board_reset,
	naomi_board_read_mem,
	naomi_board_write_mem,
	cart_read,
	cart_write,
	cart_get_ptr,
	naomi_board_get_dma_ptr,
	naomi_board_advance_ptr,
	cart_game_id,
	naomi_board_serialize,
	naomi_board_unserialize,
	cart_no_state,
	cart_no_state,
	naomi_board_set_key,
	cart_set_key_data,
	cart_offset_changed,
	cart_offset_changed,
	decrypted_close
};

cartridge *decrypted_cart_open(u8 *rom_ptr, u32 size)
{
	/* (the image is the cartridge: none is allocated for it) */
	cart_setup(&decrypted, &decrypted_ops, rom_ptr, size);
	return &decrypted;
}

/* A flat image that was a member of an archive, and is the archive's
 * memory */

static struct
{
	cartridge base;
	archive_t *archive;
} archived;

static void archived_close(cartridge *c)
{
	archive_close(archived.archive);
	archived.archive = NULL;
	c->rom_ptr = NULL;
	c->rom_size = 0;
}

static const cartridge_ops archived_ops = {
	cart_init,
	naomi_board_reset,
	naomi_board_read_mem,
	naomi_board_write_mem,
	cart_read,
	cart_write,
	cart_get_ptr,
	naomi_board_get_dma_ptr,
	naomi_board_advance_ptr,
	cart_game_id,
	naomi_board_serialize,
	naomi_board_unserialize,
	cart_no_state,
	cart_no_state,
	naomi_board_set_key,
	cart_set_key_data,
	cart_offset_changed,
	cart_offset_changed,
	archived_close
};

cartridge *archived_cart_open(const u8 *rom_ptr, u32 size, struct archive *archive)
{
	cart_setup(&archived.base, &archived_ops, (u8 *)rom_ptr, size);
	archived.archive = archive;
	return &archived.base;
}
