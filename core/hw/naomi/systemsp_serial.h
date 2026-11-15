/*
	Copyright 2023 flyinghead

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
/* based on mame code:
 * license:BSD-3-Clause
 * copyright-holders:David Haywood, MetalliC */
/*
 * What is plugged into the System SP ROM board's two serial ports (UART1,
 * UART2): an RFID chip reader/writer on each (Dinosaur King, Love and
 * Berry), the touchscreen of Isshoni Wanwan Waiwai Puppy, or the IC card
 * reader of dinokich and loveber3cn (dinokich.h).
 *
 * A device is a pipe: the port hands it each byte the game sends, asks
 * how many it has for the game and for the next of them, and tells it of
 * a power-on; the device tells the port, @update_status(@port), when it has
 * something new to send. The port has it, and closes it when it goes.
 */
#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sp_pipe sp_pipe;

typedef struct sp_pipe_ops
{
	void (*write)(sp_pipe *pipe, u8 v);
	int (*available)(sp_pipe *pipe);
	u8 (*read)(sp_pipe *pipe);
	/* what a power-on leaves it with (what it keeps in a file stays) */
	void (*reset)(sp_pipe *pipe);
	/* at most SP_PIPE_STATE_SIZE bytes, in the save state from V24 */
	void (*serialize)(sp_pipe *pipe, void **data, unsigned int *total_size);
	void (*unserialize)(sp_pipe *pipe, void **data, unsigned int *total_size);
	/* gone: what it writes to its files is written */
	void (*close)(sp_pipe *pipe);
} sp_pipe_ops;

/* A device: the first member of its own struct */
struct sp_pipe
{
	const sp_pipe_ops *ops;
	void (*update_status)(void *port);
	void *port;
};

#define SP_PIPE_STATE_SIZE 768

/* The RFID chip reader/writer on port @index (1 or 2), of @gameName in
 * @region (for a card made new), with its card in the file at @path */
sp_pipe *sp_rfid_open(int index, const char *gameName, int region, const char *path,
		void (*update_status)(void *port), void *port);
/* The touchscreen */
sp_pipe *sp_touchscreen_open(void (*update_status)(void *port), void *port);

/* A scheduler event as a save state has it, and back: one that the state
 * says is waiting more than @most cycles from now (a bad state) is due at
 * once. */
void sp_save_event(int id, void **data, unsigned int *total_size);
void sp_load_event(int id, u32 most, void **data, unsigned int *total_size);

#ifdef __cplusplus
}
#endif
