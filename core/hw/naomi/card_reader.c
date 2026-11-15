/*
	Copyright 2022 flyinghead

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
#include "card_reader.h"
#include "naomi_cart.h"
#include "naomi_periph.h"
#include "hw/sh4/modules/modules.h"
#include <string.h>

#define STX 2
#define ETX 3
#define ENQ 5
#define ACK 6

/* The bytes from @begin to @end xored together */
static u8 calc_crc(const u8 *begin, const u8 *end)
{
	u32 crc = 0;
	const u8 *it;
	for (it = begin; it != end; it++)
		crc ^= *it;
	return (u8)crc;
}

static retro_atomic_int_t card_save_failed;

#define TRACK_SIZE 0x45

/* The state of any of the readers, as a save state has it: the parts a
 * reader does not use stay zero. */
typedef struct reader_state
{
	u8 flags;			/* card in, shutter open, read waiting for a card */
	u16 inLen;
	u8 in[256];
	u8 out_buf[512];
	periph_queue out;
	u16 rxCommandLen;
	u8 rxCommand[256];
	u8 cardData[TRACK_SIZE * 3];
} reader_state;

static void reader_state_clear(reader_state *st)
{
	st->flags = 0;
	st->inLen = 0;
	memset(st->in, 0, sizeof(st->in));
	memset(st->out_buf, 0, sizeof(st->out_buf));
	periph_queue_init(&st->out, st->out_buf, sizeof(st->out_buf));
	st->rxCommandLen = 0;
	memset(st->rxCommand, 0, sizeof(st->rxCommand));
	memset(st->cardData, 0, sizeof(st->cardData));
}

/* (and its queue its own) */
static void reader_state_copy(reader_state *dst, const reader_state *src)
{
	*dst = *src;
	dst->out.buf = dst->out_buf;
}

enum { FLAG_INSERTED = 1, FLAG_DOOR_OPEN = 2, FLAG_READ_PENDING = 4 };

enum
{
	KIND_NONE,
	KIND_INITD,		/* Sanwa CRP-1231BR on the SCIF */
	KIND_DERBY_BR,	/* the same on the I/O board bridge */
	KIND_DERBY_LR,	/* Sanwa CRP-1231LR on the I/O board bridge */
	KIND_CLUBKART	/* Sanwa CR-1231R on the SCIF */
};

/* The card out of the reader - what its file has, or what the game last
 * wrote on it - is the machine's too, in a state: put in after a state
 * load, it is the card as it was when the state was saved. */
#define FILE_STATE_SIZE (1 + TRACK_SIZE * 3)

typedef struct card_reader
{
	serial_pipe pipe;
	u8 kind;
	u32 cardSize;
	reader_state st;
	/* the card in its file, read once: putting it in later reads it from here */
	u8 fileCard[TRACK_SIZE * 3];
	u32 fileSize;
} card_reader;

/* the one there is: made at a game's load, not allocated */
static card_reader reader;
static card_reader *cardReader;

static bool inserted(const card_reader *r) { return (r->st.flags & FLAG_INSERTED) != 0; }
static void set_inserted(card_reader *r, bool v)
{
	r->st.flags = v ? r->st.flags | FLAG_INSERTED : r->st.flags & ~FLAG_INSERTED;
}
static bool door_open(const card_reader *r) { return (r->st.flags & FLAG_DOOR_OPEN) != 0; }
static void set_door_open(card_reader *r, bool v)
{
	r->st.flags = v ? r->st.flags | FLAG_DOOR_OPEN : r->st.flags & ~FLAG_DOOR_OPEN;
}

/* What the game wrote on the card, to its file */
static void save_card(card_reader *r)
{
	memcpy(r->fileCard, r->st.cardData, r->cardSize);
	r->fileSize = r->cardSize;
	periph_save_file(".card", r->fileCard, r->cardSize, &card_save_failed);
}

static void push_bytes(card_reader *r, const u8 *p, u32 n)
{
	u32 i;
	for (i = 0; i < n; i++)
		periph_queue_push(&r->st.out, p[i]);
}

static int reader_available(serial_pipe *pipe)
{
	return (int)((card_reader *)pipe)->st.out.count;
}

static u8 reader_read(serial_pipe *pipe)
{
	return periph_queue_pop(&((card_reader *)pipe)->st.out);
}

/*
	Sanwa CRP-1231BR-10 card reader/writer protocol (from my good friend Metallic)
	used in InitialD and Derby Owners Club

>>>	SEND PKT: [START 02][LEN][CMD][0][0][0]{optional data}[STOP 03][CRC]
<<<	RECV ACK: [OK 06]
>>>	SEND REQ: [RQ 05]
<<<	RECV PKT: [START 02][LEN][CMD][RESULT1][RESULT2][RESULT3]{optional data}[STOP 03][CRC]
<<< RECV ERR: [15]
	RESULT1 (SENSORS): binary value SSTCCCCC
		S - Shutter state:
			0 - "ERROR"
			1 - "CLOSED"
			2 - "OPEN"
			3 - "NOW" (both open and close sensors)
		T - Stocker (card dispenser) state:
			0 - "NO" (empty)
			1 - "OK" (full)
		C - Card sensors:
			0     - "NO CARD"
			1     - "CARD EXIST (ENTER)" (card inserted in front of shutter)
			18    - "CARD EXIST" (card loaded inside of reader)
			other - "CARD EXIST (OTHER)"
	RESULT2: char 01235QRSTUV`
		Error ENUM "OK", "READ ERR", "WRITE ERR", "STOP ERR", "PRINT ERR", "READERR T1", "READERR T2", "READERR T3",
		 "READERR T12", "READERR T13", "READERR T23", "SHUT ERR"
	RESULT3 (CMD_STATE): char 02345
		State ENUM "OK", "DISABLE", "BUSY", "CARD WAIT", "NO CARD IN BOX"

	Protocol dumps and more:
	https://www.arcade-projects.com/threads/naomi-2-chihiro-triforce-card-reader-emulator-initial-d3-wmmt-mario-kart-f-zero-ax.814/

	Bits from YACardEmu: https://github.com/GXTX/YACardEmu/
	Copyright (C) 2020-2023 wutno (https://github.com/GXTX)
    Copyright (C) 2022-2023 tugpoat (https://github.com/tugpoat)
*/

enum
{
	CARD_INIT			= 0x10,
	CARD_GET_CARD_STATE	= 0x20,
	CARD_CANCEL			= 0x40,
	CARD_LOAD_CARD		= 0xB0,
	CARD_CLEAN_CARD		= 0xA0,
	CARD_READ			= 0x33,
	CARD_WRITE			= 0x53,
	CARD_PRINT			= 0x7C,
	CARD_PRINT_SETTINGS	= 0x78,
	CARD_REGISTER_FONT	= 0x7A,
	CARD_ERASE_PRINT	= 0x7D,
	CARD_DOOR			= 0xD0,
	CARD_EJECT			= 0x80,
	CARD_NEW            = 0xB0
};

static u8 sanwa_status1(const card_reader *r)
{
	if (r->kind == KIND_DERBY_LR)
	{
		/* The CRP-1231LR's:
		 * '0'	no card
		 * '1'	pos magnetic read/write
		 * '2'	pos thermal printer
		 * '3'	pos thermal dispenser
		 * '4'	ejected not removed */
		return inserted(r) ? '1' : '0';
	}
	return (u8)(((door_open(r) ? 2 : 1) << 6) | 0x20 | (inserted(r) ? 0x18 : 0));
}

static void sanwa_handle_command(card_reader *r)
{
	const u8 *cmd = r->st.rxCommand;
	u8 status1, status2 = '0', status3 = '0';
	/* STX, length, command, three results, up to three tracks, ETX, crc */
	u8 reply[8 + TRACK_SIZE * 3];
	u32 n = 0;

	if (r->st.rxCommandLen == 0)
		return;
	status1 = sanwa_status1(r);
	switch (cmd[0])
	{
	case CARD_DOOR:
		set_door_open(r, cmd[4] == '1');
		INFO_LOG(NAOMI, "Door %s\n", door_open(r) ? "open" : "closed");
		status1 = sanwa_status1(r);
		break;

	case CARD_NEW:
		INFO_LOG(NAOMI, "New card\n");
		set_inserted(r, true);
		set_door_open(r, false);
		status1 = sanwa_status1(r);
		break;

	case CARD_WRITE:
		/* 4: mode ('0': read 0x45 bytes, '1': variable length write 1-47 bytes)
		 * 5: parity ('0': 7-bit parity, '1': 8-bit no parity)
		 * 6: track (see below) */
		INFO_LOG(NAOMI, "Card write mode %c parity %c track %c\n", cmd[4], cmd[5], cmd[6]);
		switch (cmd[6])
		{
		case '0': /* track 1 */
			memcpy(r->st.cardData, &cmd[7], TRACK_SIZE);
			break;
		case '1': /* track 2 */
			memcpy(r->st.cardData + TRACK_SIZE, &cmd[7], TRACK_SIZE);
			break;
		case '2': /* track 3 */
			memcpy(r->st.cardData + TRACK_SIZE * 2, &cmd[7], TRACK_SIZE);
			break;
		case '3': /* track 1 & 2 */
			memcpy(r->st.cardData, &cmd[7], TRACK_SIZE * 2);
			break;
		case '4': /* track 1 & 3 */
			memcpy(r->st.cardData, &cmd[7], TRACK_SIZE);
			memcpy(r->st.cardData + TRACK_SIZE * 2, &cmd[7 + TRACK_SIZE], TRACK_SIZE);
			break;
		case '5': /* track 2 & 3 */
			memcpy(r->st.cardData + TRACK_SIZE, &cmd[7], TRACK_SIZE * 2);
			break;
		case '6': /* track 1 2 & 3 */
			memcpy(r->st.cardData, &cmd[7], TRACK_SIZE * 3);
			break;
		default:
			WARN_LOG(NAOMI, "Unknown track# %02x\n", cmd[6]);
			break;
		}
		save_card(r);
		break;

	case CARD_READ:
		/* 4: mode ('0': read 0x45 bytes, '1': variable length read 1-47 bytes, '2': card capture, pull in card?)
		 * 5: parity ('0': 7-bit parity, '1': 8-bit no parity)
		 * 6: track (see below) */
		DEBUG_LOG(NAOMI, "Card read mode %c parity %c track %c\n", cmd[4], cmd[5], cmd[6]);
		if (!inserted(r) || door_open(r))
			status3 = inserted(r) ? '0' : '4';
		break;

	case CARD_EJECT:
		NOTICE_LOG(NAOMI, "Card ejected\n");
		set_inserted(r, false);
		status1 = sanwa_status1(r);
		break;

	case CARD_CANCEL:
	case CARD_GET_CARD_STATE:
	case CARD_INIT:
	case CARD_REGISTER_FONT:
	case CARD_PRINT_SETTINGS:
	case CARD_PRINT:
	case CARD_CLEAN_CARD:
		break;

	default:
		WARN_LOG(NAOMI, "Unknown command %x\n", cmd[0]);
		break;
	}
	reply[n++] = STX;
	reply[n++] = 6;		/* the length, the data's added below */
	reply[n++] = cmd[0];
	reply[n++] = status1;
	reply[n++] = status2;
	reply[n++] = status3;
	if (cmd[0] == CARD_READ && inserted(r) && !door_open(r) && cmd[4] != '2')
	{
		u32 idx = 0;
		u32 size = TRACK_SIZE;
		switch (cmd[6])
		{
		case '0': /* track 1 */
			break;
		case '1': /* track 2 */
			idx = TRACK_SIZE;
			break;
		case '2': /* track 3 */
			idx = TRACK_SIZE * 2;
			break;
		case '3': /* track 1 & 2 */
			size = TRACK_SIZE * 2;
			break;
		case '4': /* track 1 & 3 */
			memcpy(&reply[n], r->st.cardData, TRACK_SIZE);
			n += TRACK_SIZE;
			reply[1] += TRACK_SIZE;
			idx = TRACK_SIZE * 2;
			break;
		case '5': /* track 2 & 3 */
			idx = TRACK_SIZE;
			size = TRACK_SIZE * 2;
			break;
		case '6': /* track 1 2 & 3 */
			size = TRACK_SIZE * 3;
			break;
		default:
			WARN_LOG(NAOMI, "Unknown track# %02x\n", cmd[6]);
			size = 0;
			break;
		}
		memcpy(&reply[n], r->st.cardData + idx, size);
		n += size;
		reply[1] += size;
	}
	reply[n++] = ETX;
	reply[n] = calc_crc(reply + 1, reply + n);
	n++;
	push_bytes(r, reply, n);
}

static void sanwa_write(serial_pipe *pipe, u8 b)
{
	card_reader *r = (card_reader *)pipe;
	reader_state *st = &r->st;
	u32 len;
	u8 crc;

	if (st->inLen == 0 && b == ENQ)
	{
		DEBUG_LOG(NAOMI, "Received RQ(5)\n");
		sanwa_handle_command(r);
		return;
	}
	st->in[st->inLen++] = b;
	if (st->inLen < 3)
		return;
	if (st->in[0] != STX)
	{
		INFO_LOG(NAOMI, "Unexpected cmd start byte %x\n", st->in[0]);
		st->inLen = 0;
		return;
	}
	len = st->in[1];
	if (st->inLen < len + 2)
	{
		if (st->inLen == sizeof(st->in))
		{
			WARN_LOG(NAOMI, "Card reader buffer overflow\n");
			st->inLen = 0;
		}
		return;
	}
	crc = calc_crc(st->in + 1, st->in + st->inLen - 1);
	if (crc != st->in[st->inLen - 1])
	{
		INFO_LOG(NAOMI, "Wrong crc: expected %x got %x\n", crc, st->in[st->inLen - 1]);
		st->inLen = 0;
		return;
	}
	DEBUG_LOG(NAOMI, "Received cmd %x len %d\n", st->in[2], st->in[1]);
	periph_queue_push(&st->out, ACK);
	st->rxCommandLen = (u16)(st->inLen - 3u < sizeof(st->rxCommand) ? st->inLen - 3u : sizeof(st->rxCommand));
	memcpy(st->rxCommand, &st->in[2], st->rxCommandLen);
	st->inLen = 0;
}

/*
	Club Kart - Sanwa CR-1231R

>>>	SEND CMD: [START 02][CMD char1][CMD char2]{parameter char}{data}[STOP 03][CRC]
<<<	RECV ACK: [OK 06] or [ERR 15]
<<<	RECV STX: [START 02]{REPLY}{data}[STOP 03][CRC]
	note: it seems reply packet sent only after command fully completed or error happened

	REPLY: 2chars
		OK - RESULT_OK
		O1 - RESULT_CANCEL_INSERT
		N0 - ERROR_CONNECT / Unknown Error
		N1 - ERROR_COMMAND / Connection Error
		N2 - ERROR_MOTOR / Mechanic Error 1
		N3 - ERROR_HEAD_UPDOWN / Mechanic Error 2
		N4 - ERROR_CARD_STUCK / Card Stuffed
		N5 - ERROR_VERIFY / OK ????
		N6 - ERROR_HEAD_TEMP / Mechanic Error 3
		N7 - ERROR_CARD_EMPTY / Card Empty
		N8 - ERROR_CARD_LOAD / Draw Card Error
		N9 - ERROR_NO_HOPPER / Card Empty
		NA - ERROR_CARD_PRESENT
		NB - ERROR_CARD_EJECT
		NC - ERROR_CANT_CANCEL
		ND - ERROR_NOT_INSERT
		NE - ERROR_NOT_WAIT
		NF - ERROR_BAD_CARD

	CMD SS REPLY: 6 ASCII characters
		5chars '0'/'1' - Card Sensors Status, MSB first
			0
			10 18
			1C C E 8 7 3
			other
		1char '0'/'1'  - Dispenser Status
			'0' - Empty
			'1' - Full

Commands:
IN   - init
CA   - cancel command
OT0  - eject card
HI   - get new card from dispenser
CL   - cleaning
RT5  - unknown
RL   - read data/load card into reader
WL   - write data (followed by 69 bytes of data)
SS   - get status

reply for commands is simple
06
02 'O' 'K' 03 crc
except
- RL command, which have 69 bytes of card data after OK (or no any reply if no card insterted)
- SS command, which is
06
02 '0/1' '0/1' '0/1' '0/1' '0/1' '0/1' 03 crc
there 0/1 - encoded in char binary value described earlier
 */

enum
{
	CLUBK_INIT,
	CLUBK_CANCEL_CMD,
	CLUBK_EJECT,
	CLUBK_NEW,
	CLUBK_CLEAN,
	CLUBK_RT5,
	CLUBK_READ,
	CLUBK_WRITE,
	CLUBK_STATUS,

	CLUBK_MAX
};

static const u8 clubk_command_bytes[CLUBK_MAX][2] =
{
	{ 'I', 'N' },	/* init */
	{ 'C', 'A' },	/* cancel command */
	{ 'O', 'T' },	/* ...0  - eject card */
	{ 'H', 'I' },	/* get new card from dispenser */
	{ 'C', 'L' },	/* cleaning */
	{ 'R', 'T' },	/* ...5  - unknown */
	{ 'R', 'L' },	/* read data/load card into reader */
	{ 'W', 'L' },	/* write data (followed by 69 bytes of data) */
	{ 'S', 'S' }	/* get status */
};

static void clubk_send_reply(card_reader *r, int cmd)
{
	u8 reply[4 + TRACK_SIZE + 2];
	u32 n = 0;
	reply[n++] = STX;
	if (cmd == CLUBK_STATUS)
	{
		reply[n++] = '0';
		reply[n++] = '0';
		reply[n++] = '0';
		reply[n++] = inserted(r) ? '1' : '0';
		reply[n++] = inserted(r) ? '1' : '0';
		reply[n++] = '1'; /* dispenser full */
	}
	else
	{
		reply[n++] = 'O';
		reply[n++] = 'K';
		if (cmd == CLUBK_READ)
		{
			memcpy(&reply[n], r->st.cardData, TRACK_SIZE);
			n += TRACK_SIZE;
		}
	}
	reply[n++] = ETX;
	reply[n] = calc_crc(reply + 1, reply + n);
	n++;
	push_bytes(r, reply, n);
}

static void clubk_handle_command(card_reader *r)
{
	reader_state *st = &r->st;
	int cmd;
	u8 crc;

	st->flags &= ~FLAG_READ_PENDING;
	for (cmd = 0; cmd < CLUBK_MAX; cmd++)
		if (st->in[1] == clubk_command_bytes[cmd][0] && st->in[2] == clubk_command_bytes[cmd][1])
			break;
	if (cmd == CLUBK_MAX)
	{
		WARN_LOG(NAOMI, "Unhandled command '%c%c'\n", st->in[1], st->in[2]);
		return;
	}
	crc = calc_crc(st->in + 1, st->in + st->inLen - 1);
	if (crc != st->in[st->inLen - 1])
	{
		WARN_LOG(NAOMI, "Wrong crc: expected %x got %x\n", crc, st->in[st->inLen - 1]);
		return;
	}
	periph_queue_push(&st->out, ACK);
	switch (cmd)
	{
	case CLUBK_WRITE:
		INFO_LOG(NAOMI, "Card write\n");
		memcpy(st->cardData, &st->in[3], TRACK_SIZE);
		save_card(r);
		break;
	case CLUBK_READ:
		INFO_LOG(NAOMI, "Card read\n");
		if (!inserted(r))
		{
			st->flags |= FLAG_READ_PENDING;
			return;
		}
		break;
	case CLUBK_EJECT:
		NOTICE_LOG(NAOMI, "Card ejected\n");
		set_inserted(r, false);
		break;
	case CLUBK_NEW:
		INFO_LOG(NAOMI, "New card\n");
		set_inserted(r, true);
		break;
	case CLUBK_INIT:
		DEBUG_LOG(NAOMI, "Card init\n");
		break;
	case CLUBK_CANCEL_CMD:
		DEBUG_LOG(NAOMI, "Cancel cmd\n");
		break;
	case CLUBK_CLEAN:
		DEBUG_LOG(NAOMI, "Card clean\n");
		break;
	case CLUBK_RT5:
		DEBUG_LOG(NAOMI, "Card RT5\n");
		break;
	case CLUBK_STATUS:
		DEBUG_LOG(NAOMI, "Card status (cardInserted %d)\n", inserted(r));
		break;
	}
	clubk_send_reply(r, cmd);
}

static void clubk_write(serial_pipe *pipe, u8 data)
{
	card_reader *r = (card_reader *)pipe;
	reader_state *st = &r->st;

	if (st->inLen == sizeof(st->in))
		st->inLen = 0;
	st->in[st->inLen++] = data;
	if (st->inLen == 5)
	{
		if ((st->in[1] != 'W' || st->in[2] != 'L') && st->in[2] != 'T')
		{
			clubk_handle_command(r);
			st->inLen = 0;
		}
	}
	else if (st->inLen == 6 && st->in[2] == 'T') /* OT0, RT5 */
	{
		clubk_handle_command(r);
		st->inLen = 0;
	}
	else if (st->inLen == TRACK_SIZE + 5) /* WL */
	{
		clubk_handle_command(r);
		st->inLen = 0;
	}
}

static const serial_pipe_ops sanwa_ops = { sanwa_write, reader_available, reader_read };
static const serial_pipe_ops clubk_ops = { clubk_write, reader_available, reader_read };

/* The reader of @kind: on the SCIF (Initial D's, Club Kart's), or on the
 * MIE through a 838-13661 RS232/RS422 converter board (Derby Owners
 * Club's) */
static void reader_make(u8 kind)
{
	card_reader_term();
	memset(&reader, 0, sizeof(reader));
	reader.kind = kind;
	reader.pipe.ops = kind == KIND_CLUBKART ? &clubk_ops : &sanwa_ops;
	reader.cardSize = kind == KIND_CLUBKART ? TRACK_SIZE : TRACK_SIZE * 3;
	reader_state_clear(&reader.st);
	reader.fileSize = (u32)periph_load_file(".card", reader.fileCard, reader.cardSize);
	cardReader = &reader;
	if (kind == KIND_DERBY_BR || kind == KIND_DERBY_LR)
		naomi_mie_set_pipe(&cardReader->pipe);
	else
		scif_set_pipe(&cardReader->pipe);
}

void card_reader_initd_init(void)
{
	reader_make(KIND_INITD);
}

void card_reader_derby_init(void)
{
	reader_make(!strcmp(naomi_game_id, " DERBY OWNERS CLUB ------------") ? KIND_DERBY_LR : KIND_DERBY_BR);
}

void card_reader_clubk_init(void)
{
	reader_make(KIND_CLUBKART);
}

void card_reader_term(void)
{
	if (cardReader == NULL)
		return;
	if (cardReader->kind == KIND_DERBY_BR || cardReader->kind == KIND_DERBY_LR)
		naomi_mie_set_pipe(NULL);
	else
		scif_set_pipe(NULL);
	cardReader = NULL;
}

static void reader_insert_card(card_reader *r)
{
	if (r->fileSize == 0)
		return;
	memcpy(r->st.cardData, r->fileCard, r->cardSize);
	r->st.flags |= FLAG_INSERTED;
	INFO_LOG(NAOMI, "Card inserted\n");
	/* (Club Kart's: a read waiting for a card is answered now) */
	if (r->kind == KIND_CLUBKART && (r->st.flags & FLAG_READ_PENDING))
	{
		clubk_send_reply(r, CLUBK_READ);
		r->st.flags &= ~FLAG_READ_PENDING;
		scif_update_status();
	}
}

/* A barcode reader: what is printed on a card is sent as it is read,
 * followed by the reader's end character. What it reads is the first line
 * of the game's .barcode file in the save directory (Code 39 text, such as
 * "*0123456789*"). */
typedef struct barcode_reader
{
	serial_pipe pipe;
	u8 to_send_buf[64];
	periph_queue to_send;
	char eotChar;
	char card[64];
} barcode_reader;

static barcode_reader barcode;
static barcode_reader *barcodeReader;

static int barcode_available(serial_pipe *pipe)
{
	return (int)((barcode_reader *)pipe)->to_send.count;
}

static u8 barcode_read(serial_pipe *pipe)
{
	return periph_queue_pop(&((barcode_reader *)pipe)->to_send);
}

static void barcode_write(serial_pipe *pipe, u8 data)
{
	(void)pipe;
	(void)data;
}

static const serial_pipe_ops barcode_ops = { barcode_write, barcode_available, barcode_read };

void card_reader_barcode_init(void)
{
	size_t len;

	card_reader_barcode_term();
	memset(&barcode, 0, sizeof(barcode));
	barcode.pipe.ops = &barcode_ops;
	periph_queue_init(&barcode.to_send, barcode.to_send_buf, sizeof(barcode.to_send_buf));
	/* (System SP Battle Racer's ends with a carriage return) */
	barcode.eotChar = !strcmp(naomi_game_id, "BTR 2K9 VER 1.004") ? '\r' : '*';
	len = periph_load_file(".barcode", barcode.card, sizeof(barcode.card) - 1);
	barcode.card[len] = '\0';
	barcode.card[strcspn(barcode.card, "\r\n")] = '\0';
	barcodeReader = &barcode;
	scif_set_pipe(&barcodeReader->pipe);
}

void card_reader_barcode_term(void)
{
	if (barcodeReader == NULL)
		return;
	scif_set_pipe(NULL);
	barcodeReader = NULL;
}

bool card_reader_barcode_available(void)
{
	return barcodeReader != NULL;
}

static void barcode_insert_card(barcode_reader *b)
{
	const char *c;
	if (b->to_send.count >= 32 || b->card[0] == '\0')
		return;
	INFO_LOG(NAOMI, "Card read: %s\n", b->card);
	for (c = b->card; *c != '\0'; c++)
		periph_queue_push(&b->to_send, (u8)*c);
	periph_queue_push(&b->to_send, (u8)b->eotChar);
	scif_update_status();
}

void card_reader_insert_card(int playerNum)
{
	if (cardReader != NULL)
		reader_insert_card(cardReader);
	else if (barcodeReader != NULL)
		barcode_insert_card(barcodeReader);
	else
		insertRfidCard(playerNum);
}

bool card_reader_available(void)
{
	return cardReader != NULL
			|| card_reader_barcode_available()
			|| getRfidCardData(0) != NULL;
}

/* The readers' part of a save state (V24): which reader the game has and
 * all of what it holds - the bytes on their way each way, the command
 * being answered, the card in it and the state of its sensors - and the
 * bytes the barcode reader has still to send. A reader the state has and
 * the game has not is read past. 1238 bytes for a card reader, 66 for a
 * barcode reader, 2 more. */
void card_reader_serialize_v24(void **data, unsigned int *total_size)
{
	u8 kinds[2];
	kinds[0] = (u8)(cardReader != NULL ? cardReader->kind : KIND_NONE);
	kinds[1] = (u8)(barcodeReader != NULL);
	LIBRETRO_SA(kinds, 2);
	if (cardReader != NULL)
	{
		const reader_state *st = &cardReader->st;
		LIBRETRO_S(st->flags);
		LIBRETRO_S(st->inLen);
		LIBRETRO_SA(st->in, sizeof(st->in));
		periph_queue_serialize(&st->out, data, total_size);
		LIBRETRO_S(st->rxCommandLen);
		LIBRETRO_SA(st->rxCommand, sizeof(st->rxCommand));
		LIBRETRO_SA(st->cardData, sizeof(st->cardData));
	}
	if (barcodeReader != NULL)
		periph_queue_serialize(&barcodeReader->to_send, data, total_size);
}

bool card_reader_unserialize_v24(void **data, unsigned int *total_size)
{
	u8 kinds[2];
	kinds[0] = KIND_NONE;
	kinds[1] = 0;
	if (!LIBRETRO_USA(kinds, 2))
		return false;
	if (kinds[0] > KIND_CLUBKART)
		return false;
	if (kinds[0] != KIND_NONE)
	{
		/* (read whole before any of it is taken: not on the stack) */
		static reader_state st;
		reader_state_clear(&st);
		LIBRETRO_US(st.flags);
		LIBRETRO_US(st.inLen);
		LIBRETRO_USA(st.in, sizeof(st.in));
		periph_queue_unserialize(&st.out, data, total_size);
		LIBRETRO_US(st.rxCommandLen);
		LIBRETRO_USA(st.rxCommand, sizeof(st.rxCommand));
		if (!LIBRETRO_USA(st.cardData, sizeof(st.cardData)))
			return false;
		st.flags &= FLAG_INSERTED | FLAG_DOOR_OPEN | FLAG_READ_PENDING;
		if (st.inLen >= sizeof(st.in))
			st.inLen = 0;
		if (st.rxCommandLen > sizeof(st.rxCommand))
			st.rxCommandLen = 0;
		if (cardReader != NULL && cardReader->kind == kinds[0])
			reader_state_copy(&cardReader->st, &st);
	}
	else if (cardReader != NULL)
		reader_state_clear(&cardReader->st);
	if (kinds[1] != 0)
	{
		static u8 buf[64];
		static periph_queue to_send;
		periph_queue_init(&to_send, buf, sizeof(buf));
		periph_queue_unserialize(&to_send, data, total_size);
		if (barcodeReader != NULL)
		{
			memcpy(barcodeReader->to_send_buf, buf, sizeof(buf));
			barcodeReader->to_send.head = to_send.head;
			barcodeReader->to_send.count = to_send.count;
		}
	}
	else if (barcodeReader != NULL)
		periph_queue_clear(&barcodeReader->to_send);
	return true;
}

/* An older state: no reader had anything on its way, and no card was in
 * one, which is how a game that had none saw it. */
void card_reader_state_before_v24(void)
{
	if (cardReader != NULL)
		reader_state_clear(&cardReader->st);
	if (barcodeReader != NULL)
		periph_queue_clear(&barcodeReader->to_send);
}

/* The cards out of the readers, in a save state from V30 on, on a NAOMI: a
 * byte with the card reader's kind (bits 0-2) and whether there are IC card
 * readers (bit 3); then the card reader's card as its file has it (208
 * bytes), and the two IC cards' (256 bytes). 1 byte for a game with none,
 * 209 with a card reader, 257 with IC card readers. What the game wrote on
 * a card goes to its file and stays there whatever state is loaded; without
 * this the card put in after a load was the one written after the state was
 * saved (and run-ahead writes ahead of what is shown). A card the state has
 * and the game has no reader for is read past. */
enum { V30_RFID = 8 };

void naomi_cards_serialize_v30(void **data, unsigned int *total_size)
{
	u8 have;
	if (!SYSTEM_IS_NAOMI())
		return;
	have = (u8)(cardReader != NULL ? cardReader->kind : KIND_NONE);
	if (getRfidCardData(0) != NULL)
		have |= V30_RFID;
	LIBRETRO_S(have);
	if (cardReader != NULL)
	{
		u8 file_have = cardReader->fileSize != 0;
		LIBRETRO_S(file_have);
		LIBRETRO_SA(cardReader->fileCard, sizeof(cardReader->fileCard));
	}
	if (have & V30_RFID)
		rfid_files_serialize_v30(data, total_size);
}

bool naomi_cards_unserialize_v30(void **data, unsigned int *total_size)
{
	u8 have = 0, kind;
	if (!SYSTEM_IS_NAOMI())
		return true;
	if (!LIBRETRO_US(have))
		return false;
	kind = have & 7;
	/* (anything else, and how much follows is not known) */
	if (kind > KIND_CLUBKART || (have & ~(7 | V30_RFID)) != 0)
		return false;
	if (kind != KIND_NONE)
	{
		u8 file[FILE_STATE_SIZE];
		if (!LIBRETRO_USA(file, sizeof(file)))
			return false;
		if (cardReader != NULL && cardReader->kind == kind)
		{
			memcpy(cardReader->fileCard, file + 1, sizeof(cardReader->fileCard));
			cardReader->fileSize = file[0] != 0 ? cardReader->cardSize : 0;
		}
	}
	if (have & V30_RFID)
		return rfid_files_unserialize_v30(data, total_size);
	return true;
}

/* An older state: the cards out of the readers are what they are now, as
 * they were with such a state before. */
void naomi_cards_state_before_v30(void)
{
}
