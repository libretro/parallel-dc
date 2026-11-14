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
#include <string.h>
#include <retro_miscellaneous.h>
#include "touchscreen.h"
#include "naomi_periph.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/modules/modules.h"
#include "hw/maple/maple_devs.h"

/*
 * 837-14672 touchscreen sensor board
 * used by Manic Panic Ghosts and Touch De Zunou
 *
 * Once the game has sent it something, the board sends where both players
 * touch the screen sixty times a second. Each player's position is where
 * the player's gun points, the touch is button 1, and button 2 is "charge".
 */
typedef struct touchscreen_pipe
{
	serial_pipe pipe;
	u8 to_send[48];
	u32 count;
	u8 scheduler_started;
	u8 touch[2];
	int sched_id;
} touchscreen_pipe;

#define FRAME_CYCLES (SH4_MAIN_CLOCK / 60)

/* (the one there is, when the game has one) */
static touchscreen_pipe panel;
static touchscreen_pipe *touchscreen;

static void ts_clear(touchscreen_pipe *ts)
{
	memset(ts->to_send, 0, sizeof(ts->to_send));
	ts->count = 0;
	ts->scheduler_started = 0;
	ts->touch[0] = ts->touch[1] = 0;
}

static u8 ts_checksum(const u8 *data, int size)
{
	u8 c = 0;
	int i;
	for (i = 0; i < size; i++)
		c += data[i];
	return (u8)(256 - c);
}

static void ts_send(touchscreen_pipe *ts, const u8 *msg, int size)
{
	if (ts->count >= 32)
		return;
	memcpy(ts->to_send + ts->count, msg, size);
	ts->count += size;
	ts->to_send[ts->count++] = ts_checksum(msg, size);
	scif_update_status();
}

static void ts_write(serial_pipe *pipe, u8 data)
{
	touchscreen_pipe *ts = (touchscreen_pipe *)pipe;
	if (data == 0x39)
	{
		/* status request */
		static const u8 status[] = { 0xaa, 0x39, 0 };
		ts_send(ts, status, sizeof(status));
	}
	if (!ts->scheduler_started)
	{
		sh4_sched_request(ts->sched_id, FRAME_CYCLES);
		ts->scheduler_started = 1;
	}
}

static int ts_available(serial_pipe *pipe)
{
	return (int)((touchscreen_pipe *)pipe)->count;
}

static u8 ts_read(serial_pipe *pipe)
{
	touchscreen_pipe *ts = (touchscreen_pipe *)pipe;
	u8 data;
	if (ts->count == 0)
		return 0;
	data = ts->to_send[0];
	ts->count--;
	memmove(ts->to_send, ts->to_send + 1, ts->count);
	return data;
}

static const serial_pipe_ops ts_ops = { ts_write, ts_available, ts_read };

static int ts_sched(int tag, int cycles, int jitter)
{
	touchscreen_pipe *ts = touchscreen;
	u32 pack[2];
	u8 msg[8];
	int i;
	if (ts == NULL)
		return 0;
	for (i = 0; i < 2; i++)
	{
		f32 fx, fy;
		u32 kcode, x, y, hit, charge;
		int ix, iy;
		mcfg_ArcadeAbsPos(i, &fx, &fy);
		kcode = mcfg_ArcadeKcode(i);
		ix = (int)fx;
		iy = (int)fy;
		x = (u32)MIN(MAX(ix, 0), 1023);
		y = (u32)MIN(MAX(iy, 0), 1023);
		hit = (kcode & NAOMI_BTN0_KEY) == 0;
		charge = (kcode & NAOMI_BTN1_KEY) == 0;
		/* touches require bits 20, 21 and 22
		 * drag needs bit 22 off
		 * bit 23 is charge */
		pack[i] = (charge << 23) | (hit << 21) | (hit << 20) | (y << 10) | x;
		if (!ts->touch[i])
			pack[i] |= hit << 22;
		ts->touch[i] = (u8)hit;
	}
	msg[0] = 0xaa;
	msg[1] = 0x10;
	msg[2] = (u8)(pack[0] >> 16);
	msg[3] = (u8)(pack[0] >> 8);
	msg[4] = (u8)pack[0];
	msg[5] = (u8)(pack[1] >> 16);
	msg[6] = (u8)(pack[1] >> 8);
	msg[7] = (u8)pack[1];
	ts_send(ts, msg, sizeof(msg));

	return FRAME_CYCLES;
}

void touchscreen_init(void)
{
	touchscreen_term();
	panel.pipe.ops = &ts_ops;
	ts_clear(&panel);
	panel.sched_id = sh4_sched_register(0, &ts_sched);
	scif_set_pipe(&panel.pipe);
	touchscreen = &panel;
}

void touchscreen_term(void)
{
	if (touchscreen == NULL)
		return;
	scif_set_pipe(NULL);
	sh4_sched_request(touchscreen->sched_id, -1);
	touchscreen = NULL;
}

bool touchscreen_active(void)
{
	return touchscreen != NULL;
}

/* The panel's part of a save state (V24): what it has still to send,
 * whether it has started sending, whether each player was touching the
 * last time, and when it sends next. 60 bytes. */
void touchscreen_serialize_v24(void **data, unsigned int *total_size)
{
	u8 counts[4];
	s32 start, end;
	if (touchscreen == NULL)
		return;
	counts[0] = (u8)touchscreen->count;
	counts[1] = touchscreen->scheduler_started;
	counts[2] = touchscreen->touch[0];
	counts[3] = touchscreen->touch[1];
	start = sch_list[touchscreen->sched_id].start;
	end = sch_list[touchscreen->sched_id].end;
	LIBRETRO_SA(touchscreen->to_send, sizeof(touchscreen->to_send));
	LIBRETRO_SA(counts, 4);
	LIBRETRO_S(start);
	LIBRETRO_S(end);
}

bool touchscreen_unserialize_v24(void **data, unsigned int *total_size)
{
	u8 counts[4] = { 0, 0, 0, 0 };
	s32 start = -1, end = -1;
	if (touchscreen == NULL)
		return true;
	LIBRETRO_USA(touchscreen->to_send, sizeof(touchscreen->to_send));
	LIBRETRO_USA(counts, 4);
	LIBRETRO_US(start);
	if (!LIBRETRO_US(end))
		return false;
	touchscreen->count = MIN((u32)counts[0], (u32)sizeof(touchscreen->to_send));
	touchscreen->scheduler_started = counts[1] != 0;
	touchscreen->touch[0] = counts[2] != 0;
	touchscreen->touch[1] = counts[3] != 0;
	periph_sched_restore(touchscreen->sched_id, start, end);
	return true;
}

/* An older state: the panel had not been spoken to, which is what the game
 * found when it had none. */
void touchscreen_state_before_v24(void)
{
	if (touchscreen == NULL)
		return;
	ts_clear(touchscreen);
	sh4_sched_request(touchscreen->sched_id, -1);
}
