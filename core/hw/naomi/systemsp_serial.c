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
/* What is plugged into the System SP ROM board's serial ports: see
 * systemsp_serial.h. */
#include <time.h>
#include <string.h>
#include <streams/file_stream.h>
#include <compat/strl.h>
#include <retro_miscellaneous.h>
#include <retro_atomic.h>
#include "systemsp_serial.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/maple/maple_devs.h"
#include "libretro/pad_state.h"
#include "savewriter.h"

void sp_save_event(int id, void **data, unsigned int *total_size)
{
	s32 start = -1, end = -1;
	if (id >= 0)
	{
		start = sch_list[id].start;
		end = sch_list[id].end;
	}
	LIBRETRO_S(start);
	LIBRETRO_S(end);
}

void sp_load_event(int id, u32 most, void **data, unsigned int *total_size)
{
	s32 start = -1, end = -1;
	LIBRETRO_US(start);
	LIBRETRO_US(end);
	if (id < 0)
		return;
	if (end != -1)
	{
		const u32 now = sh4_sched_now();
		if ((u32)end - now > most)
		{
			start = (s32)now;
			end = start == -1 ? 0 : start;
		}
	}
	sch_list[id].start = start;
	sch_list[id].end = end;
}

/* What the devices send: a queue of fixed size, which what does not fit
 * is dropped from */
#define BYTE_QUEUE_SIZE 512
typedef struct byte_queue
{
	u8 buf[BYTE_QUEUE_SIZE];
	u16 head;
	u16 count;
} byte_queue;

static void queue_clear(byte_queue *q)
{
	q->head = 0;
	q->count = 0;
	memset(q->buf, 0, sizeof(q->buf));
}

static void queue_push(byte_queue *q, u8 v)
{
	if (q->count == BYTE_QUEUE_SIZE)
	{
		WARN_LOG(NAOMI, "System SP serial port: nothing reads what the device sends; dropped\n");
		return;
	}
	q->buf[(q->head + q->count) % BYTE_QUEUE_SIZE] = v;
	q->count++;
}

static void queue_push_bytes(byte_queue *q, const u8 *p, u32 len)
{
	u32 i;
	for (i = 0; i < len; i++)
		queue_push(q, p[i]);
}

static u8 queue_pop(byte_queue *q)
{
	u8 v;
	if (q->count == 0)
		return 0;
	v = q->buf[q->head];
	q->head = (q->head + 1) % BYTE_QUEUE_SIZE;
	q->count--;
	return v;
}

static void queue_serialize(byte_queue *q, void **data, unsigned int *total_size)
{
	LIBRETRO_S(q->head);
	LIBRETRO_S(q->count);
	LIBRETRO_SA(q->buf, BYTE_QUEUE_SIZE);
}

static void queue_unserialize(byte_queue *q, void **data, unsigned int *total_size)
{
	LIBRETRO_US(q->head);
	LIBRETRO_US(q->count);
	LIBRETRO_USA(q->buf, BYTE_QUEUE_SIZE);
	if (q->head >= BYTE_QUEUE_SIZE)
		q->head = 0;
	if (q->count > BYTE_QUEUE_SIZE)
		q->count = 0;
}

static const u8 DINOKING_CHIP_DATA[128] = {
    0x12, 0x34, 0x56, 0x78, /* Serial No.0 */
    0x31, 0x00, 0x86, 0x07, /* Serial No.1 */
    0x00, 0x00, 0x00, 0x00, /* Key */
    0x05, 0x40, 0x00, 0xAA, /* Extend  Extend  Access  Mode */
    0x23, 0xFF, 0xFF, 0xFF, /* Counter4  Counter3  Counter2  Counter1 */
    0x00, 0x00, 0x00, 0x00, /* User Data (first set date: day bits 0-4, month bits 5-8, year bits 9-... + 2000) */
    0x00, 0x00, 0x00, 0x00, /* User Data */
    0x00, 0x00, 0x00, 0x00, /* User Data */
    0x00, 0x00, 0x00, 0x00, /* User Data */
    0x00, 0x00, 0x00, 0x00, /* User Data */
    0x00, 0x00, 0x00, 0x00, /* User Data */
    0x23, 0xFF, 0xFF, 0xFF, /* User Data (max counters) */
};
static const u8 LOVEBERRY_CHIP_DATA[128] = {
    0x10, 0xc5, 0x10, 0x96, /* Serial No.0 */
    0xab, 0x2f, 0xf1, 0x74, /* Serial No.1 */
    0x99, 0x33, 0x78, 0xa7, /* Key */
    0x05, 0x77, 0x00, 0xAA, /* Extend  Extend  Access  Mode */
    0x23, 0xFF, 0xFF, 0xFF, /* Counter4  Counter3  Counter2  Counter1 */
    0x00, 0x00, 0x00, 0x00, /* User Data */
    0x00, 0x00, 0x00, 0x00, /* User Data */
    0x00, 0x00, 0x00, 0x00, /* User Data */
    0x00, 0x00, 0x00, 0x00, /* User Data */
    0x00, 0x00, 0x00, 0x00, /* User Data */
    0x00, 0x00, 0x00, 0x00, /* User Data */
    0x23, 0xFF, 0xFF, 0xFF, /* User Data */
};

/*
 * RS232C I/F board (838-14244) connected to RFID Chip R/W board (838-14243 aka Maxell Picochet ME-MR23)
 */
enum
{
	RF_OK = 0xa,
	RF_RETRY = 0xca,
	RF_NG = 0x3a,
	RF_NOT = 0x6a
};

enum
{
	RF_REQ = 0x4e,
	RF_SEL = 0x2e,
	RF_READ = 0xe,
	RF_WRITE = 0xce,
	RF_WRITE_NOP = 0x8e,
	RF_WRITE_ST = 0xee,
	RF_HALT = 0xae,
	RF_COUNT = 0x6e,
	RF_RESET = 0x7e,
	RF_TEST = 0x5e,
	RF_CHANGE = 0x3e
};
#define RF_OFF 0x80

enum
{
	ST_OFF,
	ST_CONNECTING,
	ST_CONNECTED,
	ST_FULL_READ
};

typedef struct rfid_rw
{
	sp_pipe pipe;
	int index;
	char gameName[32];
	int region;
	char path[PATH_MAX_LENGTH];
	retro_atomic_int_t failed;
	byte_queue toSend;
	u8 cardData[128];
	u8 expectedBytes;
	u8 recvBuffer[10];
	u8 recvLen;
	u8 state;
	u8 rowCounter;
	u32 random;
} rfid_rw;

/* one on each port: made at a game's load, not allocated */
static rfid_rw rfids[2];

static void rfid_expect(rfid_rw *r, u8 cmd, int bytes)
{
	r->expectedBytes = (u8)(bytes + 1);
	r->recvLen = 0;
	r->recvBuffer[r->recvLen++] = cmd;
}

static u8 rfid_next_random(rfid_rw *r)
{
	r->random = r->random * 1103515245u + 12345u;
	return (u8)(r->random >> 16);
}

static u32 dino_shuffle(u32 v)
{
	return ((v & 1) << 22) | ((v & 2) << 28) | ((v & 4) << 1) | ((v & 8) << 3)
			| ((v & 0x10) << 21) | ((v & 0x20) << 10) | ((v & 0x40) << 1) | ((v & 0x80) >> 7)
			| ((v & 0x100) << 22) | ((v & 0x200) >> 1) | ((v & 0x400) >> 1) | ((v & 0x800) << 17)
			| ((v & 0x1000) << 4) | ((v & 0x2000) << 18) | ((v & 0x4000) >> 4) | ((v & 0x8000) >> 13)
			| ((v & 0x10000) >> 15) | ((v & 0x20000) >> 4) | ((v & 0x40000) << 1) | ((v & 0x80000) >> 8)
			| ((v & 0x100000) >> 6) | ((v & 0x200000) << 2) | ((v & 0x400000) >> 18) | ((v & 0x800000) >> 2)
			| ((v & 0x1000000) >> 12) | ((v & 0x2000000) >> 7) | ((v & 0x4000000) << 1) | ((v & 0x8000000) >> 3)
			| ((v & 0x10000000) >> 11) | ((v & 0x20000000) >> 9) | ((v & 0x40000000) >> 25) | ((v & 0x80000000) >> 5);
}

static u32 lovebery_deobfuscate(u32 v)
{
	v ^= 0xa35ec5e3;
	/* 87654321 -> 34781256 */
	return ((v & 0xf00000) >> 20) | ((v & 0xf0000) >> 12)
			| ((v & 0xf0) << 4) | ((v & 0xf) << 12)
			| ((v & 0xf0000000) >> 12) | ((v & 0xf000000) >> 4)
			| ((v & 0xf000) << 12) | ((v & 0xf00) << 20);
}

static u32 lovebery_obfuscate(u32 v)
{
	v = ((v & 0xf) << 20) | ((v & 0xf0) << 12)
			| ((v & 0xf00) >> 4) | ((v & 0xf000) >> 12)
			| ((v & 0xf0000) << 12) | ((v & 0xf00000) << 4)
			| ((v & 0xf000000) >> 12) | ((v & 0xf0000000) >> 20);
	return v ^ 0xa35ec5e3;
}

static void rfid_make_new_card(rfid_rw *r)
{
	int i;
	INFO_LOG(NAOMI, "Creating new RFID card\n");
	if (strncmp(r->gameName, "dinoki", 6) == 0)
	{
		u32 ser1;
		memcpy(&r->cardData[0], &DINOKING_CHIP_DATA[0], sizeof(DINOKING_CHIP_DATA));
		for (i = 0; i < 4; i++)
			r->cardData[i] = rfid_next_random(r);

		/* (in this order: the first is the top byte) */
		ser1 = (u32)rfid_next_random(r) << 24;
		ser1 |= (u32)rfid_next_random(r) << 16;
		ser1 |= (u32)rfid_next_random(r) << 8;
		ser1 |= rfid_next_random(r);
		ser1 &= ~0xc0000000;
		if (r->region == 1) /* USA (dinoking, dinokior) */
			ser1 |= 0x40000000;
		ser1 = dino_shuffle(ser1);
		r->cardData[4] = (u8)(ser1 >> 24);
		r->cardData[5] = (u8)(ser1 >> 16);
		r->cardData[6] = (u8)(ser1 >> 8);
		r->cardData[7] = (u8)ser1;
	}
	else
	{
		u32 ser1;
		memcpy(&r->cardData[0], &LOVEBERRY_CHIP_DATA[0], sizeof(LOVEBERRY_CHIP_DATA));
		for (i = 0; i < 4; i++)
			r->cardData[i] = rfid_next_random(r);

		r->cardData[5] = rfid_next_random(r);
		r->cardData[7] = rfid_next_random(r);
		ser1 = ((u32)r->cardData[4] << 24) | (r->cardData[5] << 16) | (r->cardData[6] << 8) | r->cardData[7];
		if (r->region == 2) /* Export */
		{
			ser1 = lovebery_deobfuscate(ser1);
			ser1 &= ~0xc0000000;
			ser1 = lovebery_obfuscate(ser1);
			r->cardData[4] = (u8)(ser1 >> 24);
			r->cardData[5] = (u8)(ser1 >> 16);
			r->cardData[6] = (u8)(ser1 >> 8);
			r->cardData[7] = (u8)ser1;
		}

		ser1 ^= 0x321c89d3;
		r->cardData[8] = (u8)(ser1 >> 24);
		r->cardData[9] = (u8)(ser1 >> 16);
		r->cardData[10] = (u8)(ser1 >> 8);
		r->cardData[11] = (u8)ser1;
	}
}

static void rfid_save_data(rfid_rw *r)
{
	if (retro_atomic_load_acquire_int(&r->failed) && retro_atomic_exchange_int(&r->failed, 0))
		WARN_LOG(NAOMI, "Can't save RFID card to %s\n", r->path);
	if (!save_writer_put_file(r->path, r->cardData, sizeof(r->cardData), &r->failed))
		WARN_LOG(NAOMI, "Can't save RFID card to %s\n", r->path);
}

static void rfid_write(sp_pipe *pipe, u8 v)
{
	rfid_rw *r = (rfid_rw *)pipe;

	if (r->expectedBytes > 0)
	{
		if (r->recvLen < sizeof(r->recvBuffer))
			r->recvBuffer[r->recvLen++] = v;
		if (r->recvLen < r->expectedBytes)
			return;

		switch (r->recvBuffer[0])
		{
		case RF_TEST:
			if (r->recvBuffer[1] == RF_OFF)
			{
				r->state = ST_OFF;
				queue_push(&r->toSend, RF_OK);
			}
			else if (r->state == ST_OFF)
			{
				r->state = ST_CONNECTING;
				queue_push(&r->toSend, RF_NOT);
			}
			else
				queue_push(&r->toSend, RF_OK);
			break;
		case RF_SEL:
			if ((r->state == ST_CONNECTED || r->state == ST_FULL_READ)
					&& (r->recvBuffer[5] != 0 || r->recvBuffer[6] != 0 || r->recvBuffer[7] != 0 || r->recvBuffer[8] != 0))
				r->state = ST_FULL_READ;
			else
				r->state = ST_CONNECTED;
			queue_push(&r->toSend, RF_OK);
			break;
		case RF_COUNT:
			{
				u8 newVal = 0;
				switch (r->recvBuffer[1])
				{
				case 0xc0:
					newVal = --r->cardData[16];
					break;
				case 0x30:
					newVal = --r->cardData[17];
					break;
				case 0x0c:
					newVal = --r->cardData[18];
					break;
				case 0x03:
					newVal = --r->cardData[19];
					if (newVal == 0)
						rfid_make_new_card(r);
					break;
				}
				queue_push(&r->toSend, RF_OK);
				queue_push(&r->toSend, newVal);
				rfid_save_data(r);
				break;
			}
		case RF_WRITE:
			/* (a row past the chip's 32 is not written: the count of rows
			 * is the game's, and went on past the end of the card's data) */
			if (r->rowCounter < sizeof(r->cardData) / 4)
			{
				memcpy(&r->cardData[4 * r->rowCounter], &r->recvBuffer[1], 4);
				rfid_save_data(r);
			}
			queue_push(&r->toSend, RF_OK);
			break;
		case RF_WRITE_NOP:
		case RF_WRITE_ST:
		case RF_CHANGE:
		case RF_HALT:
			queue_push(&r->toSend, RF_OK);
			break;
		default:
			break;
		}
		r->expectedBytes = 0;
	}
	else
	{
		switch (v)
		{
		case RF_TEST:
			rfid_expect(r, v, 2);
			break;
		case RF_CHANGE: /* change baud rate */
			rfid_expect(r, v, 1);
			break;
		case RF_RESET:
			r->state = ST_OFF;
			queue_push(&r->toSend, RF_OK);
			break;
		case RF_HALT:
			rfid_expect(r, v, 4); /* ser0 */
			break;
		case RF_READ:
			queue_push(&r->toSend, RF_OK);
			if (r->state == ST_FULL_READ)
				queue_push_bytes(&r->toSend, r->cardData, sizeof(r->cardData));
			else
			{
				static const u8 maskedData[120] = { 0, 0, 0, 0, 0, 0, 0, 0xaa };
				queue_push_bytes(&r->toSend, r->cardData, 8);
				queue_push_bytes(&r->toSend, maskedData, sizeof(maskedData));
			}
			break;
		case RF_SEL:
			rfid_expect(r, v, 8);	/* ser0 key (key is calc'ed from ser1) */
			break;
		case RF_REQ:
			if (r->state == ST_CONNECTING)
			{
				r->state = ST_CONNECTED;
				queue_push(&r->toSend, RF_RETRY);
			}
			else if (r->state == ST_CONNECTED || r->state == ST_FULL_READ)
			{
				queue_push(&r->toSend, RF_OK);
				queue_push_bytes(&r->toSend, r->cardData, 4);	/* ser0 */
			}
			else
				queue_push(&r->toSend, RF_RETRY);
			break;
		case RF_COUNT:
			rfid_expect(r, v, 1);
			break;
		case RF_WRITE:
			r->rowCounter++;
			rfid_expect(r, v, 4);
			break;
		case RF_WRITE_ST:
		case RF_WRITE_NOP:
			if (v == RF_WRITE_ST)
				r->rowCounter = 0;
			else
				r->rowCounter++;
			rfid_expect(r, v, 4);
			break;
		default:
			INFO_LOG(NAOMI, "UART%d write data out: unknown cmd %x\n", r->index, v);
			queue_push(&r->toSend, RF_NG);
			break;
		}
	}
	r->pipe.update_status(r->pipe.port);
}

static int rfid_available(sp_pipe *pipe)
{
	return ((rfid_rw *)pipe)->toSend.count;
}

static u8 rfid_read(sp_pipe *pipe)
{
	rfid_rw *r = (rfid_rw *)pipe;
	const u8 b = queue_pop(&r->toSend);
	if (r->toSend.count == 0)
		r->pipe.update_status(r->pipe.port);
	return b;
}

static void rfid_reset(sp_pipe *pipe)
{
	rfid_rw *r = (rfid_rw *)pipe;
	queue_clear(&r->toSend);
	r->expectedBytes = 0;
	r->recvLen = 0;
	memset(r->recvBuffer, 0, sizeof(r->recvBuffer));
	r->state = ST_OFF;
	r->rowCounter = 0;
}

static void rfid_serialize(sp_pipe *pipe, void **data, unsigned int *total_size)
{
	rfid_rw *r = (rfid_rw *)pipe;
	queue_serialize(&r->toSend, data, total_size);
	LIBRETRO_S(r->expectedBytes);
	LIBRETRO_S(r->recvLen);
	LIBRETRO_SA(r->recvBuffer, sizeof(r->recvBuffer));
	LIBRETRO_S(r->state);
	LIBRETRO_S(r->rowCounter);
	LIBRETRO_SA(r->cardData, sizeof(r->cardData));
	LIBRETRO_S(r->random);
}

static void rfid_unserialize(sp_pipe *pipe, void **data, unsigned int *total_size)
{
	rfid_rw *r = (rfid_rw *)pipe;
	u8 st = ST_OFF;
	queue_unserialize(&r->toSend, data, total_size);
	LIBRETRO_US(r->expectedBytes);
	LIBRETRO_US(r->recvLen);
	LIBRETRO_USA(r->recvBuffer, sizeof(r->recvBuffer));
	LIBRETRO_US(st);
	LIBRETRO_US(r->rowCounter);
	LIBRETRO_USA(r->cardData, sizeof(r->cardData));
	LIBRETRO_US(r->random);
	if (r->expectedBytes > sizeof(r->recvBuffer))
		r->expectedBytes = 0;
	if (r->recvLen > sizeof(r->recvBuffer))
		r->recvLen = 0;
	r->state = st <= ST_FULL_READ ? st : ST_OFF;
}

static void rfid_close(sp_pipe *pipe)
{
	(void)pipe;
	/* (the flag is this one's: the writer is done with it first) */
	save_writer_drain();
}

static const sp_pipe_ops rfid_ops = {
	rfid_write, rfid_available, rfid_read, rfid_reset, rfid_serialize, rfid_unserialize, rfid_close
};

sp_pipe *sp_rfid_open(int index, const char *gameName, int region, const char *path,
		void (*update_status)(void *port), void *port)
{
	rfid_rw *r = &rfids[index == 2 ? 1 : 0];
	RFILE *f;

	memset(r, 0, sizeof(*r));
	r->pipe.ops = &rfid_ops;
	r->pipe.update_status = update_status;
	r->pipe.port = port;
	r->index = index;
	strlcpy(r->gameName, gameName, sizeof(r->gameName));
	r->region = region;
	strlcpy(r->path, path, sizeof(r->path));
	retro_atomic_int_init(&r->failed, 0);
	r->state = ST_OFF;
	/* (the numbers a new card is made with: they are kept in the state,
	 * so a card made while the game runs is the same card again when a
	 * frame is run again from a state) */
	r->random = (u32)time(NULL) * 2654435761u + index;

	f = filestream_open(r->path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f != NULL)
	{
		if (filestream_read(f, r->cardData, sizeof(r->cardData)) != (int64_t)sizeof(r->cardData))
			WARN_LOG(NAOMI, "Rfid card %d: truncated read\n", index);
		filestream_close(f);
	}
	else
		rfid_make_new_card(r);
	return &r->pipe;
}

/*
 * Isshoni Wanwan Waiwai Puppy touchscreen
 */
#define TOUCH_SCHED_CYCLES (SH4_MAIN_CLOCK / 60)

typedef struct touchscreen
{
	sp_pipe pipe;
	byte_queue toSend;
	u8 recvBuffer[10];
	u8 recvLen;
	u32 lastPosX;
	u32 lastPosY;
	u8 lastButton;
} touchscreen;

static touchscreen touch;
/* the one there is, open */
static touchscreen *touch_open;
static int touchSchedId = -1;

static void touch_send_position(touchscreen *t, int type)
{
	/* where the light gun of the first port points, 0-1023 */
	int ax = (int)mo_x_abs[0];
	int ay = (int)mo_y_abs[0];
	u32 x, y;

	ax = ax < 0 ? 0 : ax > 639 ? 639 : ax;
	ay = ay < 0 ? 0 : ay > 479 ? 479 : ay;
	x = (640 - ax) * 1023 / 639;
	y = ay * 1023 / 479;

	if (type == 1)
	{
		u8 msg[9];
		u8 crc = 0xaa;
		int i;
		msg[0] = 'U';
		msg[1] = 'T';
		msg[2] = 0x20;	/* bit 0 and 1 are checked */
		msg[3] = x & 0xff;
		msg[4] = (x >> 8) & 0x1f;
		msg[5] = y & 0xff;
		msg[6] = (y >> 8) & 0x1f;
		msg[7] = 0;	/* z pos */
		for (i = 0; i < 8; i++)
			crc += msg[i];
		msg[8] = crc;
		queue_push_bytes(&t->toSend, msg, sizeof(msg));
		t->pipe.update_status(t->pipe.port);
	}
	else
	{
		const u8 button = (kcode[0] & NAOMI_BTN0_KEY) == 0;
		if (button != t->lastButton || x != t->lastPosX || y != t->lastPosY)
		{
			/* bit 6 is touch down */
			queue_push(&t->toSend, button ? 0xc0 : 0x80);
			queue_push(&t->toSend, (u8)((x & 7) << 4));
			queue_push(&t->toSend, (u8)((x >> 3) & 0x7f));
			queue_push(&t->toSend, (u8)((y & 7) << 4));
			queue_push(&t->toSend, (u8)((y >> 3) & 0x7f));
			t->lastButton = button;
			t->lastPosX = x;
			t->lastPosY = y;
			t->pipe.update_status(t->pipe.port);
		}
	}
}

static int touch_sched_callback(int tag, int cycles, int jitter)
{
	(void)tag;
	(void)cycles;
	(void)jitter;
	if (touch_open == NULL)
		return 0;
	touch_send_position(touch_open, 0);
	return TOUCH_SCHED_CYCLES;
}

static void touch_write(sp_pipe *pipe, u8 v)
{
	touchscreen *t = (touchscreen *)pipe;

	if (v == '\r')
	{
		if (t->recvLen >= 2 && t->recvBuffer[0] == 1)
		{
			queue_push(&t->toSend, 1);
			if (t->recvLen == 3 && t->recvBuffer[1] == 'O' && t->recvBuffer[2] == 'I')
			{
				/* get name */
				static const u8 name[] = { 'A','3','0','9','9','9' };
				queue_push_bytes(&t->toSend, name, sizeof(name));
			}
			else if (t->recvLen == 3 && t->recvBuffer[1] == 'N' && t->recvBuffer[2] == 'M')
			{
				/* unit verify */
				static const u8 id[] = { 'E','X','I','I','-','7','7','2','0','S','C',' ','R','e','v',' ','3','.','0' };
				queue_push_bytes(&t->toSend, id, sizeof(id));
			}
			else if (t->recvLen == 3 && t->recvBuffer[1] == 'U' && t->recvBuffer[2] == 'V')
			{
				/* reset */
				static const u8 resp[] = { 'Q','M','V','*','*','*','0','0' };
				queue_push_bytes(&t->toSend, resp, sizeof(resp));
			}
			else if (t->recvLen == 2 && t->recvBuffer[1] == 'R')
			{
				queue_push(&t->toSend, '0');
				sh4_sched_request(touchSchedId, TOUCH_SCHED_CYCLES);
			}
			else
				queue_push(&t->toSend, '0');
			queue_push(&t->toSend, '\r');
			t->pipe.update_status(t->pipe.port);

			/* FIXME */
			if (t->recvLen == 2 && t->recvBuffer[1] == 'Z')
				touch_send_position(t, 0);
		}
		else
			WARN_LOG(NAOMI, "\\r ignored. buf size %d\n", (int)t->recvLen);
		t->recvLen = 0;
	}
	else
	{
		if (t->recvLen == 9)
		{
			if (!memcmp(&t->recvBuffer[0], "Ua0000000", 9))
				touch_send_position(t, 1);
			else
				WARN_LOG(NAOMI, "Unknown command %.9s\n", (const char *)&t->recvBuffer[0]);
			t->recvLen = 0;
		}
		else
			t->recvBuffer[t->recvLen++] = v;
	}
}

static int touch_available(sp_pipe *pipe)
{
	return ((touchscreen *)pipe)->toSend.count;
}

static u8 touch_read(sp_pipe *pipe)
{
	touchscreen *t = (touchscreen *)pipe;
	const u8 data = queue_pop(&t->toSend);
	if (t->toSend.count == 0)
		t->pipe.update_status(t->pipe.port);
	return data;
}

static void touch_reset(sp_pipe *pipe)
{
	touchscreen *t = (touchscreen *)pipe;
	queue_clear(&t->toSend);
	t->recvLen = 0;
	memset(t->recvBuffer, 0, sizeof(t->recvBuffer));
	t->lastPosX = ~0u;
	t->lastPosY = ~0u;
	t->lastButton = 0;
	if (touchSchedId >= 0)
		sh4_sched_request(touchSchedId, -1);
}

static void touch_serialize(sp_pipe *pipe, void **data, unsigned int *total_size)
{
	touchscreen *t = (touchscreen *)pipe;
	queue_serialize(&t->toSend, data, total_size);
	LIBRETRO_S(t->recvLen);
	LIBRETRO_SA(t->recvBuffer, sizeof(t->recvBuffer));
	LIBRETRO_S(t->lastPosX);
	LIBRETRO_S(t->lastPosY);
	LIBRETRO_S(t->lastButton);
	sp_save_event(touchSchedId, data, total_size);
}

static void touch_unserialize(sp_pipe *pipe, void **data, unsigned int *total_size)
{
	touchscreen *t = (touchscreen *)pipe;
	queue_unserialize(&t->toSend, data, total_size);
	LIBRETRO_US(t->recvLen);
	LIBRETRO_USA(t->recvBuffer, sizeof(t->recvBuffer));
	LIBRETRO_US(t->lastPosX);
	LIBRETRO_US(t->lastPosY);
	LIBRETRO_US(t->lastButton);
	sp_load_event(touchSchedId, TOUCH_SCHED_CYCLES, data, total_size);
	if (t->recvLen > 9)
		t->recvLen = 0;
}

static void touch_close(sp_pipe *pipe)
{
	(void)pipe;
	if (touchSchedId >= 0)
		sh4_sched_request(touchSchedId, -1);
	touch_open = NULL;
}

static const sp_pipe_ops touch_ops = {
	touch_write, touch_available, touch_read, touch_reset, touch_serialize, touch_unserialize, touch_close
};

sp_pipe *sp_touchscreen_open(void (*update_status)(void *port), void *port)
{
	touchscreen *t = &touch;
	memset(t, 0, sizeof(*t));
	t->pipe.ops = &touch_ops;
	t->pipe.update_status = update_status;
	t->pipe.port = port;
	t->lastPosX = ~0u;
	t->lastPosY = ~0u;
	touchSchedId = sh4_sched_register(0, touch_sched_callback);
	touch_open = t;
	return &t->pipe;
}
