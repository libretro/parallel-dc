/*
 * awcartridge.h
 *
 *  Created on: Nov 5, 2018
 *      Author: flyinghead
 *  Plagiarized from mame (mame/src/mame/machine/awboard.h)
 *  // license:BSD-3-Clause
 *  // copyright-holders:Olivier Galibert,Andreas Naive
 *
 */

#ifndef CORE_HW_NAOMI_AWCARTRIDGE_H_
#define CORE_HW_NAOMI_AWCARTRIDGE_H_

#include "naomi_cart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An AtomisWave's cartridge: its ROMs, read through its cipher
 * (NULL: no memory for them) */
cartridge *aw_cart_open(u32 size);

#ifdef __cplusplus
}
#endif

#endif /* CORE_HW_NAOMI_AWCARTRIDGE_H_ */
