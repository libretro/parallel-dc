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
 * The IC card reader of dinokich and loveber3cn (see dinokich.h)
 */
#include <time.h>
#include <string.h>
#include <streams/file_stream.h>
#include <compat/strl.h>
#include <retro_miscellaneous.h>
#include "dinokich.h"
#include "dinokich_card.h"
#include "savewriter.h"

#define REQ_SOH 0xaa
#define RESP_SOH 0x55
/* what the game finds on a card */
#define CARD_ID 0x20		/* "BKACHN" (Dinosaur King) or "BLXCHN" (Love and Berry), and the card's number */
#define CARD_CHECK 0x30		/* 0x12 */
#define CARD_BLOCK 0x40		/* the encrypted block, 64 bytes */
#define CARD_SUM 0x80		/* the low byte of the sum of the block's bytes */
#define CARD_CHECK_VALUE 0x3fe	/* the block's check value, 2 bytes */
/* the status of an answer */
#define ST_OK 0x00
#define ST_NO_CARD 0x10
#define ST_BAD_ADDRESS 0x70
#define ST_BAD_COMMAND 0x80

#define EEPROM_SIZE DINOKICH_EEPROM_SIZE
#define MAX_FRAME DINOKICH_MAX_FRAME
#define TX_SIZE DINOKICH_TX_SIZE

enum
{
	PRESENT = 1,
	POWERED = 2
};

struct dinokich_reader
{
	sp_pipe pipe;
	char paths[2][PATH_MAX_LENGTH];
	retro_atomic_int_t failed[2];
	struct
	{
		u8 flags;
		u8 eeprom[EEPROM_SIZE];
	} slots[2];
	/* the request so far */
	u8 rx[MAX_FRAME];
	u16 rxLen;
	/* what the reader has to send */
	u8 tx[TX_SIZE];
	u16 txHead;
	u16 txCount;
};

/* the one there is (a System SP has one ROM board): not allocated */
static dinokich_reader the_reader;

static u8 xor_sum(const u8 *p, u32 len)
{
	u8 s = 0;
	u32 i;
	for (i = 0; i < len; i++)
		s ^= p[i];
	return s;
}

static u8 next_random(u32 *random)
{
	*random = *random * 1103515245u + 12345u;
	return (u8)(*random >> 16);
}

/* A new card, with all the credits a card can have: the game takes one a
 * play. The contents, key, pad and check value are random, as is the
 * card's number. */
static void make_card(dinokich_reader *r, int slot, bool loveBerry, u32 *random)
{
	dinokich_plain_t plain;
	u8 key[8];
	u8 *eeprom = r->slots[slot].eeprom;
	size_t i;

	INFO_LOG(NAOMI, "Creating new IC card %d\n", slot + 1);
	memset(&plain, 0, sizeof(plain));
	memcpy(plain.serial_prefix, "DINO", sizeof(plain.serial_prefix));
	memcpy(plain.tag, "CHINA-CARD!", sizeof(plain.tag));
	plain.credits = 999;
	for (i = 0; i < sizeof(plain.pad); i++)
		plain.pad[i] = next_random(random);
	for (i = 0; i < sizeof(plain.bufout); i++)
		plain.bufout[i] = next_random(random);
	for (i = 0; i < sizeof(key); i++)
		key[i] = next_random(random);

	memset(eeprom, 0, EEPROM_SIZE);
	dinokich_encode_card(&plain, key, &eeprom[CARD_BLOCK]);
	eeprom[CARD_CHECK_VALUE] = plain.bufout[0];
	eeprom[CARD_CHECK_VALUE + 1] = plain.bufout[1];
	/* read on its own, and the card is refused without it */
	memcpy(&eeprom[CARD_ID], loveBerry ? "BLXCHN" : "BKACHN", 6);
	for (i = 0; i < 4; i++)
		eeprom[CARD_ID + 9 + i] = next_random(random);
	eeprom[CARD_CHECK] = 0x12;
	eeprom[CARD_SUM] = (u8)dinokich_sum_bytes(&eeprom[CARD_BLOCK], 64);
}

static void save_card(dinokich_reader *r, int slot)
{
	if (retro_atomic_load_acquire_int(&r->failed[slot]) && retro_atomic_exchange_int(&r->failed[slot], 0))
		WARN_LOG(NAOMI, "Can't save IC card to %s\n", r->paths[slot]);
	if (!save_writer_put_file(r->paths[slot], r->slots[slot].eeprom, EEPROM_SIZE, &r->failed[slot]))
		WARN_LOG(NAOMI, "Can't save IC card to %s\n", r->paths[slot]);
}

/* As a device on a port: what it keeps in a state is in V25's part, not
 * in the port's */
static void op_write(sp_pipe *pipe, u8 v) { dinokich_reader_write((dinokich_reader *)pipe, v); }
static int op_available(sp_pipe *pipe) { return dinokich_reader_available((dinokich_reader *)pipe); }
static u8 op_read(sp_pipe *pipe) { return dinokich_reader_read((dinokich_reader *)pipe); }
static void op_reset(sp_pipe *pipe) { dinokich_reader_reset((dinokich_reader *)pipe); }
static void op_state(sp_pipe *pipe, void **data, unsigned int *total_size)
{
	(void)pipe;
	(void)data;
	(void)total_size;
}
static void op_close(sp_pipe *pipe) { dinokich_reader_close((dinokich_reader *)pipe); }

static const sp_pipe_ops dinokich_ops = { op_write, op_available, op_read, op_reset, op_state, op_state, op_close };

sp_pipe *dinokich_reader_pipe(dinokich_reader *r)
{
	return &r->pipe;
}

dinokich_reader *dinokich_reader_open(const char *gameName, const char *path1, const char *path2,
		void (*update_status)(void *port), void *port)
{
	dinokich_reader *r = &the_reader;
	u32 random = (u32)time(NULL) * 2654435761u;
	int i;

	memset(r, 0, sizeof(*r));
	r->pipe.ops = &dinokich_ops;
	r->pipe.update_status = update_status;
	r->pipe.port = port;
	strlcpy(r->paths[0], path1, sizeof(r->paths[0]));
	strlcpy(r->paths[1], path2, sizeof(r->paths[1]));
	for (i = 0; i < 2; i++)
	{
		RFILE *f;
		retro_atomic_int_init(&r->failed[i], 0);
		memset(r->slots[i].eeprom, 0, EEPROM_SIZE);
		f = filestream_open(r->paths[i], RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (f != NULL)
		{
			if (filestream_read(f, r->slots[i].eeprom, EEPROM_SIZE) != (int64_t)EEPROM_SIZE)
				WARN_LOG(NAOMI, "IC card %d: truncated read\n", i + 1);
			filestream_close(f);
		}
		else
		{
			make_card(r, i, strncmp(gameName, "love", 4) == 0, &random);
			save_card(r, i);
		}
		/* both cards are in, all the time */
		r->slots[i].flags = PRESENT;
	}
	return r;
}

void dinokich_reader_close(dinokich_reader *r)
{
	(void)r;
	/* (the flags are this one's: the writer is done with them first) */
	save_writer_drain();
}

/* A command's answer: its status, and @payloadLen bytes in @payload. The
 * request is @len bytes: AA cmd 00 N, N bytes from f[4], its xor. A byte a
 * command reads past those is not there, and the command is refused. */
static u8 command(dinokich_reader *r, const u8 *f, u32 len, u8 *payload, u32 *payloadLen)
{
	/* the request's bytes, the xor not counted, are f[0] to f[last] */
	const u32 last = len - 2;
	const u8 cmd = f[1];
	const int slot = last >= 4 ? f[4] - 1 : -1;
	switch (cmd)
	{
	case 0xb0:
		/* The firmware's version. The game reads its digits at 5 and 7, and
		 * only needs an answer. */
		if (len != 12 || f[10] != 0x08)
			return ST_BAD_COMMAND;
		memcpy(payload, "DK_C 1.0", 8);
		*payloadLen = 8;
		return ST_OK;

	case 0xc0:
		/* how a slot did, for its lamp */
		return ST_OK;

	case 0xb9:
		/* which slots have a card: bit 1 the first, bit 0 the second */
		return ((r->slots[0].flags & PRESENT) ? 2 : 0) | ((r->slots[1].flags & PRESENT) ? 1 : 0);

	case 0x67:
		{
			static const u8 ok = 1;
			/* The reader's answer to the salt the game sends, which the game
			 * unscrambles and wants above 0 */
			if (last < 10 || (slot != 0 && slot != 1))
				return ST_BAD_COMMAND;
			if (!(r->slots[slot].flags & PRESENT))
				return ST_NO_CARD;
			dinokich_md5_scramble(&f[9], &ok, 1, payload);
			*payloadLen = 1;
			return ST_OK;
		}

	case 0xb2:
	case 0xb3:
		/* the card's power on and off */
		if (slot != 0 && slot != 1)
			return ST_BAD_COMMAND;
		if (cmd == 0xb2)
		{
			if (!(r->slots[slot].flags & PRESENT))
				return ST_NO_CARD;
			r->slots[slot].flags |= POWERED;
		}
		else
			r->slots[slot].flags &= ~POWERED;
		return ST_OK;

	case 0x61:
		{
			u32 addr, n;
			/* Read: slot at 4, address at 7 (high) and 8, salt at 9 and 10,
			 * length at 12. Whether the card is powered does not matter: the
			 * game reads the card's number when it has just turned it off. */
			if (last < 12 || (slot != 0 && slot != 1))
				return ST_BAD_COMMAND;
			if (!(r->slots[slot].flags & PRESENT))
				return ST_NO_CARD;
			addr = (f[7] << 8) | f[8];
			n = f[12];
			if (n == 0 || addr + n > EEPROM_SIZE)
				return ST_BAD_ADDRESS;
			dinokich_md5_scramble(&f[9], &r->slots[slot].eeprom[addr], n, payload);
			*payloadLen = n;
			return ST_OK;
		}

	case 0x60:
	case 0x6d:
		{
			u32 addr, n;
			/* Write (0x6d is the same, for 16 bytes at 0x20 of the first
			 * card): slot at 4, address at 7 and 8, salt at 9 and 10, the
			 * data from 11 to the end, so N - 7 bytes of it. */
			if (slot != 0 && slot != 1)
				return ST_BAD_COMMAND;
			if (!(r->slots[slot].flags & PRESENT))
				return ST_NO_CARD;
			if (f[3] < 8)
				return ST_BAD_ADDRESS;
			addr = (f[7] << 8) | f[8];
			n = f[3] - 7u;
			if (addr + n > EEPROM_SIZE)
				return ST_BAD_ADDRESS;
			dinokich_md5_scramble(&f[9], &f[11], n, &r->slots[slot].eeprom[addr]);
			save_card(r, slot);
			return ST_OK;
		}

	case 0x64:
	case 0x63:
		{
			/* The card's check value, scrambled, at 11 and 12: 0x64 shows the
			 * reader the game knows it, which is taken as it is; 0x63 writes
			 * it at the end of the card. */
			if (last < 12 || (slot != 0 && slot != 1))
				return ST_BAD_COMMAND;
			if (!(r->slots[slot].flags & PRESENT))
				return ST_NO_CARD;
			if (cmd == 0x63)
			{
				dinokich_md5_scramble(&f[9], &f[11], 2, &r->slots[slot].eeprom[CARD_CHECK_VALUE]);
				save_card(r, slot);
			}
			return ST_OK;
		}

	default:
		WARN_LOG(NAOMI, "IC card reader: command %02x unknown\n", cmd);
		return ST_BAD_COMMAND;
	}
}

static void send_response(dinokich_reader *r, u8 status, const u8 *payload, u32 len)
{
	u8 x = 0;
	u8 head[4];
	u32 i;

	if (r->txCount + len + 5 > TX_SIZE)
	{
		WARN_LOG(NAOMI, "IC card reader: nothing reads what the reader sends; dropped\n");
		return;
	}
	head[0] = RESP_SOH;
	head[1] = status;
	head[2] = 0;
	head[3] = (u8)len;
	for (i = 0; i < len + 5; i++)
	{
		const u8 b = i < 4 ? head[i] : i < len + 4 ? payload[i - 4] : x;
		x ^= b;
		r->tx[(r->txHead + r->txCount) % TX_SIZE] = b;
		r->txCount++;
	}
}

static void handle_frame(dinokich_reader *r)
{
	u8 payload[255];
	u32 payloadLen = 0;
	u8 status;

	if (xor_sum(r->rx, r->rxLen) != 0)
	{
		WARN_LOG(NAOMI, "IC card reader: bad checksum, frame dropped\n");
		return;
	}
	status = command(r, r->rx, r->rxLen, payload, &payloadLen);
	send_response(r, status, payload, payloadLen);
}

void dinokich_reader_write(dinokich_reader *r, u8 v)
{
	/* (a frame starts with its first byte: anything before it is dropped) */
	if (r->rxLen != 0 || v == REQ_SOH)
	{
		r->rx[r->rxLen++] = v;
		if (r->rxLen >= 4 && r->rxLen == r->rx[3] + 5u)
		{
			handle_frame(r);
			r->rxLen = 0;
		}
	}
	r->pipe.update_status(r->pipe.port);
}

int dinokich_reader_available(const dinokich_reader *r)
{
	return r->txCount;
}

u8 dinokich_reader_read(dinokich_reader *r)
{
	u8 v;
	if (r->txCount == 0)
		return 0;
	v = r->tx[r->txHead];
	r->txHead = (r->txHead + 1) % TX_SIZE;
	r->txCount--;
	if (r->txCount == 0)
		r->pipe.update_status(r->pipe.port);
	return v;
}

void dinokich_reader_reset(dinokich_reader *r)
{
	r->rxLen = 0;
	r->txHead = 0;
	r->txCount = 0;
	memset(r->rx, 0, sizeof(r->rx));
	memset(r->tx, 0, sizeof(r->tx));
	r->slots[0].flags = PRESENT;
	r->slots[1].flags = PRESENT;
}

/* In the state from V25, for dinokich and loveber3cn only: for each card,
 * whether it is in and powered, and its 1 KB; the request so far; what is
 * still to be sent. DINOKICH_STATE_SIZE (2,828) bytes. */
void dinokich_reader_serialize_v25(dinokich_reader *r, void **data, unsigned int *total_size)
{
	int i;
	for (i = 0; i < 2; i++)
	{
		LIBRETRO_S(r->slots[i].flags);
		LIBRETRO_SA(r->slots[i].eeprom, EEPROM_SIZE);
	}
	LIBRETRO_S(r->rxLen);
	LIBRETRO_SA(r->rx, MAX_FRAME);
	LIBRETRO_S(r->txHead);
	LIBRETRO_S(r->txCount);
	LIBRETRO_SA(r->tx, TX_SIZE);
}

void dinokich_reader_unserialize_v25(dinokich_reader *r, void **data, unsigned int *total_size)
{
	int i;
	for (i = 0; i < 2; i++)
	{
		LIBRETRO_US(r->slots[i].flags);
		LIBRETRO_USA(r->slots[i].eeprom, EEPROM_SIZE);
		r->slots[i].flags &= PRESENT | POWERED;
	}
	LIBRETRO_US(r->rxLen);
	LIBRETRO_USA(r->rx, MAX_FRAME);
	LIBRETRO_US(r->txHead);
	LIBRETRO_US(r->txCount);
	LIBRETRO_USA(r->tx, TX_SIZE);
	/* (a request so far is one that has not ended yet: anything else is not one) */
	if (r->rxLen > MAX_FRAME || (r->rxLen != 0 && r->rx[0] != REQ_SOH) || (r->rxLen >= 4 && r->rxLen >= r->rx[3] + 5u))
		r->rxLen = 0;
	if (r->txHead >= TX_SIZE || r->txCount > TX_SIZE)
	{
		r->txHead = 0;
		r->txCount = 0;
	}
}

/* A state from before V25 was saved when the reader was not emulated: the
 * cards are what their files have, and nothing is under way. */
void dinokich_reader_state_before_v25(dinokich_reader *r)
{
	dinokich_reader_reset(r);
}
