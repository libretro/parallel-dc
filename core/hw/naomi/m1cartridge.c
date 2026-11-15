/*
 * m1cartridge.c
 *
 *  Created on: Nov 4, 2018
 *      Author: flyinghead
 *  Plagiarized from mame (mame/src/mame/machine/naomim1.cpp)
 *  // license:BSD-3-Clause
 *  // copyright-holders:Olivier Galibert
 */
#include <string.h>
#include "m1cartridge.h"

/* How much is decoded ahead of what is read: a transfer moves this much at
 * a time, so nothing is left over to be moved down the buffer. (The buffer
 * is 32 KB, as the save states have it, and an older state's is full: that
 * is read to its end before any more is decoded.) */
#define BUFFER_FILL 1024

typedef struct
{
	cartridge base;
	u16 actel_id;
	u8 buffer[32768];
	u8 dict[111], hist[2];
	u64 avail_val;
	u32 rom_cur_address, buffer_actual_size, avail_bits;
	bool stream_ended, has_history;
	bool encryption;
} m1_cart;

static m1_cart m1;

static u32 get_decrypted_32b(m1_cart *m)
{
	const cartridge *c = &m->base;
	u32 swapped_key;
	/* The stream is read until its own end mark, from an address that is
	 * the game's (or a save state's): past the end of the ROMs it is all
	 * ones, as cart_read() gives it. */
	u8 a = 0xff, b = 0xff, cc = 0xff, d = 0xff;
	if (m->rom_cur_address < c->rom_size && c->rom_size - m->rom_cur_address >= 4)
	{
		const u8 *base = c->rom_ptr + m->rom_cur_address;
		a = base[0];
		b = base[1];
		cc = base[2];
		d = base[3];
	}
	m->rom_cur_address += 4;

	swapped_key = (c->key >> 24) | ((c->key >> 8) & 0xff00)
			| ((c->key << 8) & 0xff0000) | (c->key << 24);
	return swapped_key ^ (((u32)(b ^ d) << 24) | ((u32)(a ^ cc) << 16) | ((u32)b << 8) | a);
}

static u32 lookb(m1_cart *m, int bits)
{
	if ((u32)bits > m->avail_bits)
	{
		m->avail_val = (m->avail_val << 32) | get_decrypted_32b(m);
		m->avail_bits += 32;
	}
	return (u32)(m->avail_val >> (m->avail_bits - bits)) & ((1 << bits) - 1);
}

static void skipb(m1_cart *m, int bits)
{
	m->avail_bits -= bits;
}

static u32 getb(m1_cart *m, int bits)
{
	const u32 res = lookb(m, bits);
	skipb(m, bits);
	return res;
}

static void enc_reset(m1_cart *m)
{
	unsigned i;
	m->avail_val = 0;
	m->avail_bits = 0;
	m->stream_ended = false;
	m->has_history = false;
	m->buffer_actual_size = 0;
	for (i = 0; i < sizeof(m->dict); i++)
		m->dict[i] = (u8)getb(m, 8);
}

static void wb(m1_cart *m, u8 byte)
{
	const u32 n = m->buffer_actual_size;
	if (m->dict[0] & 64)
	{
		if (n < 2)
		{
			if (m->has_history)
				m->buffer[n] = m->hist[n] - byte;
			else
				m->buffer[n] = byte;
		}
		else
			m->buffer[n] = m->buffer[n - 2] - byte;
	}
	else
		m->buffer[n] = byte;
	m->buffer_actual_size++;
}

static void enc_fill(m1_cart *m)
{
	while (m->buffer_actual_size < BUFFER_FILL && !m->stream_ended)
	{
		switch (lookb(m, 3))
		{
		/* 00+2 - 0000+esc */
		case 0:
		case 1:
			{
				int addr;
				skipb(m, 2);
				addr = (int)getb(m, 2);
				if (addr)
					wb(m, m->dict[addr]);
				else
					wb(m, (u8)getb(m, 8));
			}
			break;
		/* 010+2 */
		case 2:
			skipb(m, 3);
			wb(m, m->dict[getb(m, 2) + 4]);
			break;
		/* 011+3 */
		case 3:
			skipb(m, 3);
			wb(m, m->dict[getb(m, 3) + 8]);
			break;
		/* 10+5 */
		case 4:
		case 5:
			skipb(m, 2);
			wb(m, m->dict[getb(m, 5) + 16]);
			break;
		/* 11+6 */
		case 6:
		case 7:
			{
				int addr;
				skipb(m, 2);
				addr = (int)getb(m, 6) + 48;
				if (addr == 111)
					m->stream_ended = true;
				else
					wb(m, m->dict[addr]);
			}
			break;
		}
	}
	while (m->buffer_actual_size < BUFFER_FILL)
		m->buffer[m->buffer_actual_size++] = 0;
}

static void m1_clear(m1_cart *m)
{
	m->rom_cur_address = 0;
	m->buffer_actual_size = 0;
	m->has_history = false;
	m->stream_ended = false;
	memset(m->dict, 0, sizeof(m->dict));
	memset(m->hist, 0, sizeof(m->hist));
	m->avail_val = 0;
	m->avail_bits = 0;
	m->encryption = false;
}

static void m1_reset(cartridge *c)
{
	naomi_board_reset(c);
	m1_clear(&m1);
}

static u32 m1_read_mem(cartridge *c, u32 address, u32 size)
{
	if ((address & 0xff) == 0x14)
		return m1.actel_id;
	return naomi_board_read_mem(c, address, size);
}

static void *m1_get_dma_ptr(cartridge *c, u32 *size)
{
	if (m1.encryption)
	{
		if (*size > m1.buffer_actual_size)
			*size = m1.buffer_actual_size;
		return m1.buffer;
	}
	return naomi_board_get_dma_ptr(c, size);
}

static void m1_advance_ptr(cartridge *c, u32 size)
{
	m1_cart *m = &m1;
	if (!m->encryption)
	{
		naomi_board_advance_ptr(c, size);
		return;
	}
	if (size < m->buffer_actual_size)
	{
		memmove(m->buffer, m->buffer + size, m->buffer_actual_size - size);
		m->buffer_actual_size -= size;
	}
	else
	{
		m->hist[0] = m->buffer[m->buffer_actual_size - 2];
		m->hist[1] = m->buffer[m->buffer_actual_size - 1];
		m->has_history = true;
		m->buffer_actual_size = 0;
	}
	enc_fill(m);
}

static void m1_dma_offset_changed(cartridge *c, u32 dma_offset)
{
	m1_cart *m = &m1;
	m->rom_cur_address = dma_offset & 0x1fffffff;
	if ((dma_offset & 0x20000000) == 0 && m->rom_cur_address < c->rom_size)
	{
		m->encryption = true;
		enc_reset(m);
		enc_fill(m);
	}
	else
		m->encryption = false;
}

static void m1_serialize(cartridge *c, void **data, unsigned int *total_size)
{
	LIBRETRO_S(m1.buffer);
	LIBRETRO_S(m1.dict);
	LIBRETRO_S(m1.hist);
	LIBRETRO_S(m1.avail_val);
	LIBRETRO_S(m1.rom_cur_address);
	LIBRETRO_S(m1.buffer_actual_size);
	LIBRETRO_S(m1.avail_bits);
	LIBRETRO_S(m1.stream_ended);
	LIBRETRO_S(m1.has_history);
	LIBRETRO_S(m1.encryption);
	naomi_board_serialize(c, data, total_size);
}

static void m1_unserialize(cartridge *c, void **data, unsigned int *total_size)
{
	LIBRETRO_US(m1.buffer);
	LIBRETRO_US(m1.dict);
	LIBRETRO_US(m1.hist);
	LIBRETRO_US(m1.avail_val);
	LIBRETRO_US(m1.rom_cur_address);
	LIBRETRO_US(m1.buffer_actual_size);
	LIBRETRO_US(m1.avail_bits);
	LIBRETRO_US(m1.stream_ended);
	LIBRETRO_US(m1.has_history);
	LIBRETRO_US(m1.encryption);
	/* (the buffer is full whenever the decoder is on: m1_advance_ptr()
	 * counts on it) */
	if (m1.buffer_actual_size > sizeof(m1.buffer) || (m1.encryption && m1.buffer_actual_size < 2))
		m1.buffer_actual_size = sizeof(m1.buffer);
	/* (bits waiting in avail_val: never more than a refill on top of seven) */
	if (m1.avail_bits > 39)
		m1.avail_bits = 0;
	naomi_board_unserialize(c, data, total_size);
}

static const cartridge_ops m1_ops = {
	cart_init,
	m1_reset,
	m1_read_mem,
	naomi_board_write_mem,
	cart_read,
	cart_write,
	cart_get_ptr,
	m1_get_dma_ptr,
	m1_advance_ptr,
	cart_game_id,
	m1_serialize,
	m1_unserialize,
	cart_no_state,
	cart_no_state,
	naomi_board_set_key,
	cart_set_key_data,
	m1_dma_offset_changed,
	cart_offset_changed,
	cart_destruct
};

cartridge *m1_cart_open(u32 size)
{
	cart_construct(&m1.base, &m1_ops, size);
	m1.actel_id = 0;
	m1_clear(&m1);
	/* (what a save state holds of the buffer past what was decoded:
	 * nothing, and not what was in the memory before) */
	memset(m1.buffer, 0, sizeof(m1.buffer));
	return &m1.base;
}
