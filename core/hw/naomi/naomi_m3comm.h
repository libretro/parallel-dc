/*
	Created on: Mar 15, 2020
	Copyright 2020 flyinghead
	This file is part of flycast.
    flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
    flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The M3 link board some NAOMI games link machines with (naomi_m3comm.c).
 * Its network thread runs once the game has reset the board's CPU, with a
 * server or the address of one set. */
u32 m3comm_read(u32 address, u32 size);
void m3comm_write(u32 address, u32 data, u32 size);
/* A G1 DMA to or from the board: false if the board does not take it */
bool m3comm_dma_start(u32 addr, u32 data);

/* The network thread stopped and its links closed */
void m3comm_close_network(void);
/* ...and the network's server sockets too, for the machine's end */
void m3comm_term(void);
/* The board as it is when the machine is switched on */
void m3comm_reset(void);
/* The board's registers and its two memories, in a save state from V23 */
void m3comm_serialize(void **data, unsigned int *total_size);
bool m3comm_unserialize(void **data, unsigned int *total_size);

#ifdef __cplusplus
}
#endif
