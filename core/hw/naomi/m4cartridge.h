/*
 * m4cartridge.h
 *
 *  Created on: Nov 5, 2018
 *      Author: flyinghead
 *  Plagiarized from mame (mame/src/mame/machine/naomim4.h)
 *  // license:BSD-3-Clause
 *  // copyright-holders:Olivier Galibert,Andreas Naive
 *
 */

#ifndef CORE_HW_NAOMI_M4CARTRIDGE_H_
#define CORE_HW_NAOMI_M4CARTRIDGE_H_

#include "naomi_cart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An M4 board: ROMs read through its stream cipher, and a flash's CFI
 * table. (The System SP's ROM board is one, and more: systemsp.cpp.) */
typedef struct m4_cart
{
	cartridge base;
	u16 m4id;
	u8 *key_data;			/* 2048 bytes */
	u16 subkey1;
	u16 subkey2;
	u16 one_round[0x10000];
	u8 buffer[32768];
	u32 rom_cur_address, buffer_actual_size;
	u16 iv;
	u8 counter;
	bool encryption;
	bool cfi_mode;
	bool xfer_ready;
} m4_cart;

/* NULL: no memory for its ROMs */
cartridge *m4_cart_open(u32 size);

/* For a board that is an M4 board and more: one with @ops, @size bytes of
 * ROM; and released */
void m4_cart_construct(m4_cart *m, const cartridge_ops *ops, u32 size);
void m4_cart_destruct(cartridge *c);
/* What an M4 board does, for a board that does it too */
bool m4_cart_init(cartridge *c);
void m4_cart_reset(cartridge *c);
u32 m4_cart_read_mem(cartridge *c, u32 address, u32 size);
bool m4_cart_read(cartridge *c, u32 offset, u32 size, void *dst);
bool m4_cart_write(cartridge *c, u32 offset, u32 size, u32 data);
void *m4_cart_get_dma_ptr(cartridge *c, u32 *size);
void m4_cart_advance_ptr(cartridge *c, u32 size);
void m4_cart_game_id(cartridge *c, char *id);
void m4_cart_serialize(cartridge *c, void **data, unsigned int *total_size);
void m4_cart_unserialize(cartridge *c, void **data, unsigned int *total_size);
void m4_cart_set_key(cartridge *c, u32 key);
void m4_cart_set_key_data(cartridge *c, u8 *key_data);
void m4_cart_dma_offset_changed(cartridge *c, u32 dma_offset);
void m4_cart_pio_offset_changed(cartridge *c, u32 pio_offset);

#ifdef __cplusplus
}
#endif

#endif /* CORE_HW_NAOMI_M4CARTRIDGE_H_ */
