/*
	Copyright 2026 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
/*
 * The IC card reader of Dinosaur King (China) and Love and Berry 3 (China),
 * made by Hirocon: two card slots, on the ROM board's first serial port.
 *
 * A request is AA cmd 00 N, N bytes, and a byte that makes the xor of the
 * whole frame 0; the answer is 55 status 00 N, N bytes, and the same. The
 * cards are 1 KB EEPROMs, kept in the save directory as the RFID cards of
 * the other sets are (<game>-<region>.rfid1 and .rfid2), and made when
 * there are none.
 */
#pragma once
#include <retro_atomic.h>
#include "types.h"
#include "systemsp_serial.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DINOKICH_EEPROM_SIZE 1024
#define DINOKICH_MAX_FRAME (255 + 5)
#define DINOKICH_TX_SIZE 512
/* what the reader has in a save state from V25 */
#define DINOKICH_STATE_SIZE (2 * (1 + DINOKICH_EEPROM_SIZE) + 2 + DINOKICH_MAX_FRAME + 4 + DINOKICH_TX_SIZE)

typedef struct dinokich_reader dinokich_reader;

/* The reader of @gameName ("dinokich", "loveber3cn"), with its two cards in
 * the files at @path1 and @path2 (made when there are none), on a port
 * that @update_status(@port) tells it has something new to send. There is
 * one: opened again, it is made again. */
dinokich_reader *dinokich_reader_open(const char *gameName, const char *path1, const char *path2,
		void (*update_status)(void *port), void *port);
/* The cards' files written */
void dinokich_reader_close(dinokich_reader *r);
/* The reader as a device on its port, which closes it when it goes */
sp_pipe *dinokich_reader_pipe(dinokich_reader *r);

/* a byte from the game; how many it has for the game; the next of them */
void dinokich_reader_write(dinokich_reader *r, u8 v);
int dinokich_reader_available(const dinokich_reader *r);
u8 dinokich_reader_read(dinokich_reader *r);
/* what a power-on leaves it with (the cards stay) */
void dinokich_reader_reset(dinokich_reader *r);

/* What the reader has in a save state from V25: DINOKICH_STATE_SIZE bytes */
void dinokich_reader_serialize_v25(dinokich_reader *r, void **data, unsigned int *total_size);
void dinokich_reader_unserialize_v25(dinokich_reader *r, void **data, unsigned int *total_size);
void dinokich_reader_state_before_v25(dinokich_reader *r);

#ifdef __cplusplus
}
#endif
