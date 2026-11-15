/*
 * gdcartridge.h
 *
 *  Created on: Nov 16, 2018
 *      Author: flyinghead
 *
 * From mame naomigd.h
 * license:BSD-3-Clause
 * copyright-holders:Olivier Galibert
 *
 */

#ifndef CORE_HW_NAOMI_GDCARTRIDGE_H_
#define CORE_HW_NAOMI_GDCARTRIDGE_H_

#include "naomi_cart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A NAOMI's DIMM board, and the GD-ROM it loads the game from: @size bytes
 * of the PIC's data, the disc @gdrom_name (in the content's folder, or the
 * parent set's). @dimm: the board answers as a DIMM board at
 * 5f703c-5f704c, the plain one (firmware 1.02) or, @net, the network one
 * (firmware 3.17, with no network behind it); without it, those answer
 * as they do for a ROM cartridge. NULL: no memory for the PIC's data. */
cartridge *gd_cart_open(u32 size, const char *gdrom_name, bool dimm, bool net);

/* The DIMM board's part of the save state, from V25 for the plain board,
 * with the ROM board's serial EEPROM, and from V31 for the network board,
 * whose serial EEPROM is in the V26 part */
void naomi_gd_serialize_v25(void **data, unsigned int *total_size);
bool naomi_gd_unserialize_v25(void **data, unsigned int *total_size);
void naomi_gd_state_before_v25(void);
bool naomi_gd_serial_in_v25(void);
void naomi_gd_serialize_v31(void **data, unsigned int *total_size);
bool naomi_gd_unserialize_v31(void **data, unsigned int *total_size);
void naomi_gd_state_before_v31(void);

#ifdef __cplusplus
}
#endif

#endif /* CORE_HW_NAOMI_GDCARTRIDGE_H_ */
