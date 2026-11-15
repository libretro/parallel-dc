/*
 * m1cartridge.h
 *
 *  Created on: Nov 4, 2018
 *      Author: flyinghead
 *  Plagiarized from mame (mame/src/mame/machine/naomim1.h)
 *  // license:BSD-3-Clause
 *  // copyright-holders:Olivier Galibert
 */
#ifndef CORE_HW_NAOMI_M1CARTRIDGE_H_
#define CORE_HW_NAOMI_M1CARTRIDGE_H_

#include "naomi_cart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An M1 board: ROMs that a transfer reads through its decompressor
 * (NULL: no memory for them) */
cartridge *m1_cart_open(u32 size);

#ifdef __cplusplus
}
#endif

#endif /* CORE_HW_NAOMI_M1CARTRIDGE_H_ */
