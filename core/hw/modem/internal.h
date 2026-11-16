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
#ifndef MODEM_INTERNAL_H
#define MODEM_INTERNAL_H
#include "types.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A stream of bytes between two parts of the modem: what is read from it,
 * how much is there, and what is written to it, with @ctx */
typedef struct modem_stream
{
	int (*read)(void *ctx);
	int (*available)(void *ctx);
	void (*write)(void *ctx, u8 v);
	void *ctx;
} modem_stream;

/* A queue of bytes of a fixed size, in @buf, so that what it holds can go
 * into a save state as it is. The size is a power of two. A byte that does
 * not fit is refused: the users of these are sized so that it does not
 * happen. */
typedef struct byte_fifo
{
	u8 *buf;
	unsigned size;
	unsigned head;
	unsigned count;
} byte_fifo;

/* Empty, over the @size bytes at @buf (zeroed) */
void byte_fifo_init(byte_fifo *f, u8 *buf, unsigned size);
void byte_fifo_clear(byte_fifo *f);
int byte_fifo_size(const byte_fifo *f);
int byte_fifo_room(const byte_fifo *f);
bool byte_fifo_push(byte_fifo *f, u8 v);
/* The first byte, taken; -1 if there is none */
int byte_fifo_pop(byte_fifo *f);
int byte_fifo_front(const byte_fifo *f);
/* Always size + 2 bytes: the count, then the bytes in order and zeroes
 * after them, so that two queues holding the same bytes are saved the
 * same. */
void byte_fifo_serialize(const byte_fifo *f, void **data, unsigned int *total_size);
void byte_fifo_unserialize(byte_fifo *f, void **data, unsigned int *total_size);

#ifdef __cplusplus
}
#endif

#endif
