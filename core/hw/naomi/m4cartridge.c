/*
 * m4cartridge.c
 *
 *  Created on: Nov 5, 2018
 *      Author: flyinghead
 *  Plagiarized from mame (mame/src/mame/machine/naomim4.cpp)
 *  // license:BSD-3-Clause
 *  // copyright-holders:Olivier Galibert,Andreas Naive
 */
#include <stdlib.h>
#include <string.h>
#include "m4cartridge.h"
#include "naomi_regs.h"

/* Decoder for M4-type NAOMI cart encryption
 *
 * In hardware, the decryption is managed by the XC3S50 Xilinx Spartan FPGA (IC2)
 * and the annexed PIC16C621A PIC MCU (IC3).
 * - The FPGA control the clock line of the security PIC.
 * - The protocol between the FPGA and the MCU is nibble-based, though it hasn't been RE for now.
 * - The decryption algorithm is clearly nibble-based too.
 *
 * The decryption algorithm itself implements a stream cipher built on top of a 16-bits block cipher.
 * The underlying block-cipher is a SP-network of 2 rounds (both identical in structure). In every
 * round, the substitution phase is done using 4 fixed 4-to-4 sboxes acting on every nibble. The permutation
 * phase is indeed a nibble-based linear combination.
 * With that block cipher, a stream cipher is constructed by feeding the output result of the 1st round
 * of a certain 16-bits block as a whitening value for the next block. The cart dependent data used by
 * the algorithm is a 32-bits key stored in the PIC16C621A. The hardware auto-reset the feed value
 * to the cart-based IV every 16 blocks (32 bytes); that reset is not address-based, but index-based. */

static const u8 k_sboxes[4][16] = {
	{9,8,2,11,1,14,5,15,12,6,0,3,7,13,10,4},
	{2,10,0,15,14,1,11,3,7,12,13,8,4,9,5,6},
	{4,11,3,8,7,2,15,13,1,5,14,9,6,12,0,10},
	{1,13,8,2,0,5,6,14,4,11,15,10,12,3,7,9}
};

/* from the S29GL512N datasheet */
static const u8 cfidata[] = {
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x51,0x00,0x52,0x00,0x59,0x00,0x02,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x27,0x00,0x36,0x00,0x00,0x00,0x00,0x00,0x07,0x00,
0x07,0x00,0x0a,0x00,0x00,0x00,0x01,0x00,0x05,0x00,0x04,0x00,0x00,0x00,0x1a,0x00,0x02,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0x01,0x00,0xff,0x00,0x01,0x00,0x00,0x00,
0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x50,0x00,0x52,0x00,0x49,0x00,0x31,0x00,0x33,0x00,0x10,0x00,0x02,0x00,0x01,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0xb5,0x00,0xc5,0x00,0x04,0x00,
0x01,0x00
};

/* How much is decoded ahead of what is read: a transfer moves this much at
 * a time, so nothing is left over to be moved down the buffer, and a word
 * read through the data register moves 1 KB down and not 32. (The buffer
 * is 32 KB, as the save states have it, and an older state's is full: that
 * is read to its end before any more is decoded.) */
#define BUFFER_FILL 1024

/* What a read past the ROMs, or past the CFI table, gives */
static u8 retzero[2];

static m4_cart m4;

#define M4(c) ((m4_cart *)(c))

static void enc_init(m4_cart *m)
{
	int round_input;
	for (round_input = 0; round_input < 0x10000; round_input++)
	{
		u8 input_nibble[4];
		u8 output_nibble[4];
		u8 aux_nibble;
		u16 result = 0;
		int nibble_idx, i;

		for (nibble_idx = 0; nibble_idx < 4; ++nibble_idx)
		{
			input_nibble[nibble_idx] = (round_input >> (nibble_idx * 4)) & 0xf;
			output_nibble[nibble_idx] = 0;
		}

		aux_nibble = input_nibble[3];
		for (nibble_idx = 0; nibble_idx < 4; ++nibble_idx)
		{
			/* 4 s-boxes per round */
			aux_nibble ^= k_sboxes[nibble_idx][input_nibble[nibble_idx]];
			for (i = 0; i < 4; ++i)		/* diffusion of the bits */
				output_nibble[(nibble_idx - i) & 3] |= aux_nibble & (1 << i);
		}

		for (nibble_idx = 0; nibble_idx < 4; ++nibble_idx)
			result |= (output_nibble[nibble_idx] << (4 * nibble_idx));

		m->one_round[round_input] = result;
	}
}

static void device_reset(m4_cart *m)
{
	m->rom_cur_address = 0;
	m->buffer_actual_size = 0;
	m->encryption = false;
	m->cfi_mode = false;
	m->counter = 0;
	m->iv = 0;
	m->xfer_ready = false;
}

static void enc_reset(m4_cart *m)
{
	m->buffer_actual_size = 0;
	m->iv = 0;
	m->counter = 0;
}

static u16 decrypt_one_round(const m4_cart *m, u16 word, u16 subkey)
{
	return m->one_round[(u16)(word ^ subkey)] ^ subkey;
}

static void enc_fill(m4_cart *m)
{
	const cartridge *c = &m->base;
	/* The buffer is filled ahead of what is asked for, from an address
	 * that is the game's (or a save state's): past the end of the ROMs the
	 * words to decode are all ones, as cart_read() gives them. */
	const u32 left = m->rom_cur_address < c->rom_size ? c->rom_size - m->rom_cur_address : 0;
	const bool inside = m->buffer_actual_size < BUFFER_FILL && BUFFER_FILL - m->buffer_actual_size <= left;
	u32 pos = m->rom_cur_address;
	while (m->buffer_actual_size < BUFFER_FILL)
	{
		u16 enc, dec;
		if (inside)
			enc = c->rom_ptr[pos] | (c->rom_ptr[pos + 1] << 8);
		else if (m->rom_cur_address < c->rom_size && c->rom_size - m->rom_cur_address >= 2)
			enc = c->rom_ptr[m->rom_cur_address] | (c->rom_ptr[m->rom_cur_address + 1] << 8);
		else
			enc = 0xffff;
		dec = m->iv;
		m->iv = decrypt_one_round(m, enc ^ m->iv, m->subkey1);
		dec ^= decrypt_one_round(m, m->iv, m->subkey2);

		m->buffer[m->buffer_actual_size++] = (u8)dec;
		m->buffer[m->buffer_actual_size++] = (u8)(dec >> 8);

		pos += 2;
		m->rom_cur_address += 2;

		m->counter++;
		if (m->counter == 16)
		{
			m->counter = 0;
			m->iv = 0;
		}
	}
}

bool m4_cart_init(cartridge *c)
{
	m4_cart *m = M4(c);
	if (m->m4id == 0)
	{
		EMUERROR("Warning: M4 ID not provided\n");
		m->m4id = 0x5504;
	}
	if (m->key_data != NULL)
	{
		m->subkey1 = (m->key_data[0x5e2] << 8) | m->key_data[0x5e0];
		m->subkey2 = (m->key_data[0x5e6] << 8) | m->key_data[0x5e4];
	}
	else
		/* a set without its key file */
		WARN_LOG(NAOMI, "Missing M4 key\n");
	enc_init(m);
	device_reset(m);
	return true;
}

void m4_cart_reset(cartridge *c)
{
	naomi_board_reset(c);
	device_reset(M4(c));
}

u32 m4_cart_read_mem(cartridge *c, u32 address, u32 size)
{
	u32 data;
	if ((address & 0xff) == 0x34)
		return M4(c)->m4id & 0xff80;
	data = naomi_board_read_mem(c, address, size);
	if ((address & 0xff) == (NAOMI_ROM_OFFSETH_addr & 0xff))
		/* indicates that security PIC is present */
		data |= 0x2000;
	return data;
}

void m4_cart_dma_offset_changed(cartridge *c, u32 dma_offset)
{
	(void)dma_offset;
	M4(c)->xfer_ready = false;
}

void m4_cart_pio_offset_changed(cartridge *c, u32 pio_offset)
{
	M4(c)->encryption = (pio_offset & 0x40000000) != 0;
	M4(c)->xfer_ready = false;
}

bool m4_cart_read(cartridge *c, u32 offset, u32 size, void *dst)
{
	m4_cart *m = M4(c);
	if (m->cfi_mode)
	{
		const u32 fpr_num = m->m4id & 0x7f;
		if (((offset >> 26) & 0x07) < fpr_num)
		{
			/* (the table is all there is: zero past it) */
			if ((offset & 0xffff) + 2 <= sizeof(cfidata))
				memcpy(dst, &cfidata[offset & 0xffff], 2);
			else
				memset(dst, 0, 2);
			return true;
		}
	}
	if (!m->xfer_ready)
	{
		m->rom_cur_address = c->rom_pio_offset & 0x1ffffffe;
		if (m->encryption)
		{
			enc_reset(m);
			enc_fill(m);
		}
		m->xfer_ready = true;
	}
	if (!m->encryption)
		return cart_read(c, offset & 0x1ffffffe, size, dst);
	switch (size)
	{
	case 2:
		memcpy(dst, m->buffer, 2);
		break;
	case 4:
		memcpy(dst, m->buffer, 4);
		break;
	}
	if (c->rom_pio_autoinc)
		c->ops->advance_ptr(c, size);
	return true;
}

void *m4_cart_get_dma_ptr(cartridge *c, u32 *size)
{
	m4_cart *m = M4(c);
	if (m->cfi_mode)
	{
		const u32 fpr_num = m->m4id & 0x7f;
		if (((c->dma_offset >> 26) & 0x07) < fpr_num)
		{
			if (*size > 2)
				*size = 2;
			if ((c->dma_offset & 0xffff) + 2 > sizeof(cfidata))
				return retzero;
			return (void *)&cfidata[c->dma_offset & 0xffff];
		}
	}
	if (!m->xfer_ready)
	{
		m->rom_cur_address = c->dma_offset & 0x1ffffffe;
		if (m->encryption)
		{
			enc_reset(m);
			enc_fill(m);
		}
		m->xfer_ready = true;
	}
	if (m->encryption)
	{
		if (*size > m->buffer_actual_size)
			*size = m->buffer_actual_size;
		return m->buffer;
	}
	if ((c->dma_offset & 0x1ffffffe) < c->rom_size)
	{
		if (*size > c->rom_size - (c->dma_offset & 0x1ffffffe))
			*size = c->rom_size - (c->dma_offset & 0x1ffffffe);
		return c->rom_ptr + (c->dma_offset & 0x1ffffffe);
	}
	if (*size > 2)
		*size = 2;
	return retzero;
}

void m4_cart_advance_ptr(cartridge *c, u32 size)
{
	m4_cart *m = M4(c);
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
		m->buffer_actual_size = 0;
	enc_fill(m);
}

bool m4_cart_write(cartridge *c, u32 offset, u32 size, u32 data)
{
	(void)size;
	if ((offset & 0xffff) == 0x00aa && data == 0x0098)
		M4(c)->cfi_mode = true;
	if ((offset & 0xffff) == 0x0000 && data == 0x00f0)
		M4(c)->cfi_mode = false;
	return true;
}

void m4_cart_game_id(cartridge *c, char *id)
{
	m4_cart *m = M4(c);
	if (c->rom_size < 0x30 + 0x20)
	{
		strcpy(id, "(ROM too small)");
		return;
	}
	if (c->rom_ptr[0] == 'N' && c->rom_ptr[1] == 'A')
		cart_trim_id(id, (const char *)c->rom_ptr + 0x30, 0x20);
	else
	{
		m->rom_cur_address = 0;
		enc_reset(m);
		enc_fill(m);
		cart_trim_id(id, (const char *)m->buffer + 0x30, 0x20);
	}
}

void m4_cart_serialize(cartridge *c, void **data, unsigned int *total_size)
{
	m4_cart *m = M4(c);
	LIBRETRO_S(m->buffer);
	LIBRETRO_S(m->rom_cur_address);
	LIBRETRO_S(m->buffer_actual_size);
	LIBRETRO_S(m->iv);
	LIBRETRO_S(m->counter);
	LIBRETRO_S(m->encryption);
	LIBRETRO_S(m->cfi_mode);
	LIBRETRO_S(m->xfer_ready);
	naomi_board_serialize(c, data, total_size);
}

void m4_cart_unserialize(cartridge *c, void **data, unsigned int *total_size)
{
	m4_cart *m = M4(c);
	LIBRETRO_US(m->buffer);
	LIBRETRO_US(m->rom_cur_address);
	LIBRETRO_US(m->buffer_actual_size);
	/* (never more than the buffer holds, and whole words) */
	if (m->buffer_actual_size > sizeof(m->buffer))
		m->buffer_actual_size = sizeof(m->buffer);
	m->buffer_actual_size &= ~1u;
	LIBRETRO_US(m->iv);
	LIBRETRO_US(m->counter);
	LIBRETRO_US(m->encryption);
	LIBRETRO_US(m->cfi_mode);
	LIBRETRO_US(m->xfer_ready);
	naomi_board_unserialize(c, data, total_size);
}

void m4_cart_set_key(cartridge *c, u32 key)
{
	M4(c)->m4id = (u16)key;
}

void m4_cart_set_key_data(cartridge *c, u8 *key_data)
{
	M4(c)->key_data = key_data;
}

void m4_cart_construct(m4_cart *m, const cartridge_ops *ops, u32 size)
{
	cart_construct(&m->base, ops, size);
	m->m4id = 0;
	m->key_data = NULL;
	m->subkey1 = 0;
	m->subkey2 = 0;
	device_reset(m);
	/* (what a save state holds of the buffer past what was decoded:
	 * nothing, and not what was in the memory before) */
	memset(m->buffer, 0, sizeof(m->buffer));
}

void m4_cart_destruct(cartridge *c)
{
	m4_cart *m = M4(c);
	if (m->key_data != NULL)
		free(m->key_data);
	m->key_data = NULL;
	cart_destruct(c);
}

static const cartridge_ops m4_ops = {
	m4_cart_init,
	m4_cart_reset,
	m4_cart_read_mem,
	naomi_board_write_mem,
	m4_cart_read,
	m4_cart_write,
	cart_get_ptr,
	m4_cart_get_dma_ptr,
	m4_cart_advance_ptr,
	m4_cart_game_id,
	m4_cart_serialize,
	m4_cart_unserialize,
	cart_no_state,
	cart_no_state,
	m4_cart_set_key,
	m4_cart_set_key_data,
	m4_cart_dma_offset_changed,
	m4_cart_pio_offset_changed,
	m4_cart_destruct
};

cartridge *m4_cart_open(u32 size)
{
	m4_cart_construct(&m4, &m4_ops, size);
	return &m4.base;
}
