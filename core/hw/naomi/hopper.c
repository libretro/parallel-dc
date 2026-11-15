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
#include "hopper.h"
#include "naomi_cart.h"
#include "naomi_periph.h"
#include "hw/sh4/modules/modules.h"
#include "hw/maple/maple_devs.h"
#include <string.h>

/* What the board keeps through a power cut, and in its file in the save
 * directory: the credits, the counters and the operator's settings. All
 * of it 32-bit words, flags 0 or 1. */
typedef struct hopper_config
{
	u32 credit0;
	u32 credit1;
	u32 totalCredit;
	u32 premium;
	u32 gameNumber;
	u32 freePlay;
	u32 autoPayOut;
	u32 autoExchange;
	u32 twoWayMode;
	u32 coinDiscrimination;
	u32 betButton;
	u32 currency;		/* 0:medal, 1:pound, 2:dollar, 3:euro, 4:token, 5: any cash (837-14438 only) */
	u32 medalExchRate;
	u32 maxHopperFloat;
	u32 maxPay;
	u32 maxCredit;
	u32 hopperSize;
	u32 maxBet;
	u32 minBet;
	u32 addBet;
	u32 paidAmount;
	u32 wonAmount;
	u32 curBase;
} hopper_config;
/* (words only) */
typedef char hopper_config_size_check[sizeof(hopper_config) == 23 * 4 ? 1 : -1];

static void config_initial(hopper_config *c)
{
	c->credit0 = 0;
	c->credit1 = 0;
	c->totalCredit = 100; /* min bet */
	c->premium = 0;
	c->gameNumber = 0;
	c->freePlay = 0;
	c->autoPayOut = 0;
	c->autoExchange = 0;
	c->twoWayMode = 1;
	c->coinDiscrimination = 1;
	c->betButton = 1;
	c->currency = 0xff;
	c->medalExchRate = 5;
	c->maxHopperFloat = 200;
	c->maxPay = 1999900;
	c->maxCredit = 1999900;
	c->hopperSize = 39900;
	c->maxBet = 10000;
	c->minBet = 100; /* normally 1000 */
	c->addBet = 100;
	c->paidAmount = 0;
	c->wonAmount = 0;
	c->curBase = 100;
}

static void config_normalize(hopper_config *c)
{
	c->freePlay &= 1;
	c->autoPayOut &= 1;
	c->autoExchange &= 1;
	c->twoWayMode &= 1;
	c->coinDiscrimination &= 1;
	c->betButton &= 1;
	c->currency &= 0xff;
	c->medalExchRate &= 0xff;
	c->curBase &= 0xff;
}

static retro_atomic_int_t config_save_failed;

/* The two boards: the SEGA 837-14438 (Kick'4'Cash; Club Kart Prize ver.B
 * and Shootout Pool Prize ver.B with P1 button 0 held at boot), and the
 * NAOMI SWP hopper board. The same messages, numbered and laid out each
 * its own way. Their status word (837-14438: coin in and yen in enabled,
 * front, back and cash doors open, refill needed; SWP: an error, the ATP
 * type, the door open) says nothing is wrong. */
#define HOPPER_STATUS 0

typedef struct hopper_board
{
	serial_pipe pipe;
	bool sega;		/* the 837-14438; else the SWP board */
	hopper_config cfg;
	hopper_config saved;	/* as its file has it */
	u8 recv[256];
	u32 recvLen;
	u32 expectedBytes;
	u8 to_send_buf[2048];
	periph_queue to_send;
	u8 started;
	u8 coinKey;
} hopper_board;

/* the one there is: made at a game's load, not allocated */
static hopper_board board;
static hopper_board *hopper;

static u32 rd32(const hopper_board *h, u32 offset)
{
	u32 v;
	memcpy(&v, &h->recv[offset], sizeof(v));
	return v;
}

/* 'H' <u8 command> <u16 length> payload ... <u8 checksum> */
static void send_message(hopper_board *h, u8 command, const u8 *payload, size_t len)
{
	u8 chksum = 'H' + command;
	size_t i;

	DEBUG_LOG(NAOMI, "hopper sending command %x size %x\n", command, (int)len + 5);
	periph_queue_push(&h->to_send, 'H');
	periph_queue_push(&h->to_send, command);
	len += 5;
	periph_queue_push(&h->to_send, (u8)(len & 0xff));
	chksum += len & 0xff;
	periph_queue_push(&h->to_send, (u8)(len >> 8));
	chksum += len >> 8;
	len -= 5;
	for (i = 0; i < len; i++, payload++)
	{
		periph_queue_push(&h->to_send, *payload);
		chksum += *payload;
	}
	periph_queue_push(&h->to_send, chksum);
	scif_update_status();
}

/* (what is less than none is none) */
static u32 minus(u32 a, u32 b)
{
	const int v = (int)a - (int)b;
	return v > 0 ? (u32)v : 0;
}

static void bet(hopper_board *h, u32 offset)
{
	u32 values[3];
	int i;

	for (i = 0; i < 3; i++)
		values[i] = rd32(h, offset + i * 4);
	for (i = 0; i < 2; i++)
	{
		u32 *primary = i == 0 ? &h->cfg.credit0 : &h->cfg.credit1;
		u32 *second = i == 0 ? &h->cfg.credit1 : &h->cfg.credit0;
		if (*primary >= values[i])
			*primary -= values[i];
		else
		{
			const u32 residual = values[i] - *primary;
			*primary = 0;
			*second = minus(*second, residual);
		}
	}
	h->cfg.premium = minus(h->cfg.premium, values[2]);
}

static void sega_pay_win(hopper_board *h);
static void naomi_pay_win(hopper_board *h);

static void pay_out(hopper_board *h, u32 value)
{
	if (value == 0)
		return;
	h->cfg.wonAmount += value;
	if (!h->cfg.autoPayOut)
		h->cfg.credit0 += value;
	else
	{
		h->cfg.paidAmount += value;
		if (h->sega)
			sega_pay_win(h);
		else
			naomi_pay_win(h);
	}
}

static void insert_coin(hopper_board *h, u32 value)
{
	h->cfg.credit0 += value;
}

static void sega_coin_in(hopper_board *h)
{
	u32 payload[8];

	insert_coin(h, 100);
	memset(payload, 0, 8 * sizeof(u32));
	payload[0] = 0;         /* atp num */
	payload[1] = 0;         /* atp kind and error code */
	payload[2] = HOPPER_STATUS;
	payload[3] = h->cfg.credit0;
	payload[4] = h->cfg.credit1;
	payload[5] = h->cfg.totalCredit;
	payload[6] = h->cfg.premium;
	send_message(h, 1, (const u8 *)payload, 8 * sizeof(u32) - 1);
}

static void sega_pay_win(hopper_board *h)
{
	u32 payload[0xa];

	memset(payload, 0, 0xa * sizeof(u32));
	payload[0] = 0;         /* atp num */
	payload[1] = 0;         /* atp kind and error code */
	payload[2] = HOPPER_STATUS;
	payload[3] = h->cfg.credit0;
	payload[4] = h->cfg.credit1;
	payload[5] = h->cfg.totalCredit;
	payload[6] = h->cfg.premium;
	payload[7] = h->cfg.wonAmount;
	payload[8] = h->cfg.paidAmount;
	send_message(h, 3, (const u8 *)payload, 0xa * sizeof(u32) - 1);
}

static const u32 *sega_default_values(const hopper_board *h)
{
	static const u32 defaults[][8] = {
		/* free play...           max pay         hopper size   min bet
		 *            auto exch...        max credit      max bet     add bet */
		{ 0x05010000, 0x01640100, 1999900, 1999900, 39900, 10000, 1000, 100 },
		{      0x100,    0x50000,   10000, 1999900, 39900,   100,  100,  50 },
		{      0x100,    0x50000,   10000, 1999900, 39900,   100,  100, 100 },
		{      0x100,    0x50000,   10000, 1999900, 39900,   100,  100, 100 },
		{          0, 0x01640000, 1999900, 1999900, 39900, 10000,  100, 100 },
		{      0x100,    0x50000,   10000, 1999900, 39900,   100,  100, 100 },
	};
	if (h->cfg.currency >= sizeof(defaults) / sizeof(defaults[0]))
		return defaults[0];
	else
		return defaults[h->cfg.currency];
}

static void sega_set_defaults(hopper_board *h)
{
	const u32 *def = sega_default_values(h);
	h->cfg.freePlay = def[0] & 1;
	h->cfg.autoPayOut = (def[0] & 0x100) != 0;
	h->cfg.twoWayMode = (def[0] & 0x10000) != 0;
	h->cfg.medalExchRate = def[0] >> 24;
	h->cfg.autoExchange = def[1] & 1;
	h->cfg.coinDiscrimination = (def[1] & 0x100) != 0;
	h->cfg.curBase = (def[1] >> 16) & 0xff;
	h->cfg.betButton = (def[1] & 0x1000000) != 0;
	h->cfg.maxPay = def[2];
	h->cfg.maxCredit = def[3];
	h->cfg.hopperSize = def[4];
	h->cfg.maxBet = def[5];
	h->cfg.minBet = def[6];
	h->cfg.addBet = def[7];
	h->cfg.totalCredit = h->cfg.minBet;
}

static void sega_get_ranges(const hopper_board *h, u32 *p)
{
	switch (h->cfg.currency)
	{
	case 0: /* medal */
	case 4: /* token */
		p[0x0] = 100;
		p[0x1] = 10000;
		p[0x2] = 100 | (499 << 16);
		p[0x3] = 500 | (499 << 16);
		if (h->cfg.currency == 0)
		{
			p[0x4] = 500 | (49 << 16);
			p[0x5] = 50;
			p[0x6] = 200;
			p[0xa] = 2000 | (200 << 16);
			p[0xb] = 100;
		}
		else
		{
			p[0x4] = 500 | (19 << 16);
			p[0x5] = 20;
			p[0x6] = 200;
			p[0xa] = 200 | (50 << 16);
			p[0xb] = 10;
		}
		p[0x7] = 0;
		p[0x8] = 0;
		p[0x9] = 0;
		p[0xc] = 0;

		p[0xd] = 100;
		p[0xe] = 0;
		p[0xf] = 0;
		p[0x10] = 0;

		p[0x11] = 100 | (200 << 16);
		p[0x12] = 300 | (400 << 16);
		p[0x13] = 500 | (1000 << 16);
		p[0x14] = 2000 | (2500 << 16);
		p[0x15] = 5000 | (10000 << 16);
		p[0x16] = 0;
		break;
	case 1: /* pound */
		p[0x0] = 50;
		p[0x1] = 100;
		p[0x2] = 50 | (5 << 16);
		p[0x3] = 5 | (499 << 16);
		p[0x4] = 500 | (19 << 16);
		p[0x5] = 20;
		p[0x6] = 70;
		p[0x7] = 0;
		p[0x8] = 0;
		p[0x9] = 0;
		p[0xa] = 70 | (70 << 16);
		p[0xb] = 0;
		p[0xc] = 0;

		p[0xd] = 5 | (10 << 16);
		p[0xe] = 20 | (50 << 16);
		p[0xf] = 50 | (100 << 16);
		p[0x10] = 200;

		p[0x11] = 5 | (10 << 16);
		p[0x12] = 20 | (50 << 16);
		p[0x13] = 100 | (200 << 16);
		p[0x14] = 500 | (1000 << 16);
		p[0x15] = 0;
		p[0x16] = 0;
		break;
	case 3: /* euro */
		p[0x0] = 50;
		p[0x1] = 100;
		p[0x2] = 50 | (5 << 16);
		p[0x3] = 5 | (499 << 16);
		p[0x4] = 500 | (19 << 16);
		p[0x5] = 20;
		p[0x6] = 200;
		p[0x7] = 0;
		p[0x8] = 0;
		p[0x9] = 0;
		p[0xa] = 200 | (50 << 16);
		p[0xb] = 10;
		p[0xc] = 0;

		p[0xd] = 5 | (10 << 16);
		p[0xe] = 20 | (50 << 16);
		p[0xf] = 50 | (100 << 16);
		p[0x10] = 200 | (100 << 16);

		p[0x11] = 5 | (10 << 16);
		p[0x12] = 20 | (50 << 16);
		p[0x13] = 100 | (200 << 16);
		p[0x14] = 500 | (1000 << 16);
		p[0x15] = 0;
		p[0x16] = 0;
		break;
	case 2: /* dollar */
	case 5: /* any cash */
	default:
		p[0x0] = 50;
		p[0x1] = 100;
		p[0x2] = 50 | (5 << 16);
		p[0x3] = 5 | (499 << 16);
		p[0x4] = 500 | (19 << 16);
		p[0x5] = 20;
		p[0x6] = 200;
		p[0x7] = 0;
		p[0x8] = 0;
		p[0x9] = 0;
		p[0xa] = 200 | (50 << 16);
		p[0xb] = 10;
		p[0xc] = 0;

		p[0xd] = 5 | (10 << 16);
		p[0xe] = 20 | (25 << 16);
		p[0xf] = 50 | (100 << 16);
		p[0x10] = 200 | (100 << 16);

		p[0x11] = 5 | (10 << 16);
		p[0x12] = 20 | (25 << 16);
		p[0x13] = 50 | (100 << 16);
		p[0x14] = 200 | (500 << 16);
		p[0x15] = 1000;
		p[0x16] = 0;
		break;
	}
}

static void sega_handle(hopper_board *h, u8 command)
{
	u32 payload[0x1ea];

	switch (command)
	{
		case 0x40:
		{
			/* VERSION */
			INFO_LOG(NAOMI, "hopper received VERSION\n");
			memset(payload, 0, 4 * sizeof(u32));
			payload[0] = 0x00010001;
			payload[1] = 3;
			send_message(h, 0x20, (const u8 *)payload, 4 * sizeof(u32) - 1);
			break;
		}

		case 0x41:
		{
			/* POWER ON */
			INFO_LOG(NAOMI, "hopper received POWER ON\n");
			/* 8: currency */
			if (h->cfg.currency != h->recv[8]) {
				h->cfg.currency = h->recv[8];
				sega_set_defaults(h);
			}
			memset(payload, 0, 0x1ea * sizeof(u32));
			payload[0] = h->cfg.gameNumber;	/* game number */
			payload[1] = 0;         	/* atp num */
			payload[2] = 0;         	/* atp kind and error code */
			payload[3] = HOPPER_STATUS;		/* status bitfield. rectangle in background if != 0 */
			payload[4] = h->cfg.freePlay | (h->cfg.autoPayOut << 8) | (h->cfg.twoWayMode << 16) | (h->cfg.medalExchRate << 24);
			payload[5] = h->cfg.autoExchange | (h->cfg.coinDiscrimination << 8) | (h->cfg.curBase << 16) | (h->cfg.betButton << 24);
			payload[6] = 0;				/* ? 8c027124 */
			payload[7] = h->cfg.maxPay;
			payload[8] = h->cfg.maxCredit;
			payload[9] = h->cfg.hopperSize;
			payload[10] = h->cfg.maxBet;
			payload[11] = h->cfg.minBet;
			payload[12] = h->cfg.addBet;
			payload[13] = h->cfg.currency;	/* currency (lsb, 0:medal, 1:pound, 2:dollar, 3:euro, 4:token, 5: any cash) */
			payload[14] = h->cfg.credit0;
			payload[15] = h->cfg.credit1;
			payload[16] = h->cfg.totalCredit;
			payload[17] = h->cfg.premium;
			payload[0x2d] = h->cfg.wonAmount;
			payload[0x32] = h->cfg.paidAmount;	/* paid? 8c027190 */
			payload[0x33] = h->cfg.maxHopperFloat;
			payload[0x37] = 0x80;	/* dataportCondition (b6: protocol on, bit7: forever) */
			/* Default values */
			{
				const u32 *def = sega_default_values(h);
				payload[0x1c0] = def[0];
				payload[0x1c1] = def[1];
				payload[0x1c3] = def[2];
				payload[0x1c4] = def[3];
				payload[0x1c5] = def[4];
				payload[0x1c6] = def[5];
				payload[0x1c7] = def[6];
				payload[0x1c8] = def[7];
			}
			sega_get_ranges(h, &payload[0x1c9]);
			payload[0x1e9] = 1;		/* hopper ready flag */
			send_message(h, 0x21, (const u8 *)payload, 0x1ea * sizeof(u32) - 1);
			h->started = 1;
			break;
		}

		case 0x42:
		{
			/* GET_STATUS */
			INFO_LOG(NAOMI, "hopper received GET STATUS\n");
			memset(payload, 0, 4 * sizeof(u32));
			payload[0] = 0;         /* atp num */
			payload[1] = 0;         /* atp kind and error code */
			payload[2] = HOPPER_STATUS;
			send_message(h, 0x22, (const u8 *)payload, 4 * sizeof(u32) - 1);
			break;
		}

		case 0x43:
		{
			/* GAME START */
			INFO_LOG(NAOMI, "hopper received GAME START\n");
			bet(h, 4);
			memset(payload, 0, 0x1f * sizeof(u32));
			payload[0] = ++h->cfg.gameNumber;	/* game#, ? */
			payload[1] = 0;				/* atp num */
			payload[2] = 0;				/* atp kind and error code */
			payload[3] = HOPPER_STATUS;
			payload[4] = h->cfg.credit0;
			payload[5] = h->cfg.credit1;
			payload[6] = h->cfg.totalCredit;
			payload[7] = h->cfg.premium;
			send_message(h, 0x23, (const u8 *)payload, 0x1f * sizeof(u32) - 1);
			break;
		}

		case 0x44:
		{
			/* GAME END */
			INFO_LOG(NAOMI, "hopper received GAME END\n");
			memset(payload, 0, 0x1e * sizeof(u32));
			payload[0] = h->cfg.gameNumber;	/* gameNum, upper 16 bits: ? 0, -1 or -2 */
			payload[1] = h->cfg.credit0;
			payload[2] = h->cfg.credit1;
			payload[3] = h->cfg.totalCredit;
			payload[4] = h->cfg.premium;
			payload[27] = 2;			/* wins */
			payload[28] = 0;			/* last paid */
			send_message(h, 0x24, (const u8 *)payload, 0x1e * sizeof(u32) - 1);
			break;
		}

		case 0x45:
		{
			/* TEST */
			INFO_LOG(NAOMI, "hopper received TEST\n");
			memset(payload, 0, 0x3c * sizeof(u32));
			payload[0] = 0;         /* atp num */
			payload[1] = 0;         /* atp kind and error code */
			payload[2] = HOPPER_STATUS;
			payload[0x38] = 0;
			send_message(h, 0x25, (const u8 *)payload, 0x3c * sizeof(u32) - 1);
			break;
		}

		case 0x4a:
			/* SWITCH */
			INFO_LOG(NAOMI, "hopper received SWITCH\n");
			break;

		case 0x4b:
		{
			/* CONFIG_HOP */
			INFO_LOG(NAOMI, "hopper received CONFIG HOP\n");
			if (h->recv[2] == 0x2c && h->recv[3] == 0)
			{
				h->cfg.freePlay = h->recv[4] & 1;
				h->cfg.autoPayOut = h->recv[5] & 1;
				h->cfg.twoWayMode = h->recv[6] & 1;
				h->cfg.medalExchRate = h->recv[7];
				h->cfg.autoExchange = h->recv[8] & 1;
				h->cfg.coinDiscrimination = h->recv[9] & 1;
				h->cfg.curBase = h->recv[0xa];
				h->cfg.betButton = h->recv[0xb] & 1;
				/* 0xc ?? */
				h->cfg.maxPay = rd32(h, 0x10);
				h->cfg.maxCredit = rd32(h, 0x14);
				h->cfg.hopperSize = rd32(h, 0x18);
				h->cfg.maxBet = rd32(h, 0x1c);
				h->cfg.minBet = rd32(h, 0x20);
				h->cfg.addBet = rd32(h, 0x24);
			}
			memset(payload, 0, 0xa * sizeof(u32));
			payload[0] = h->cfg.freePlay | (h->cfg.autoPayOut << 8) | (h->cfg.twoWayMode << 16) | (h->cfg.medalExchRate << 24);
			payload[1] = h->cfg.autoExchange | (h->cfg.coinDiscrimination << 8) | (h->cfg.curBase << 16) | (h->cfg.betButton << 24);
			payload[2] = 0;				/* ? 8c027124 */
			payload[3] = h->cfg.maxPay;
			payload[4] = h->cfg.maxCredit;
			payload[5] = h->cfg.hopperSize;
			payload[6] = h->cfg.maxBet;
			payload[7] = h->cfg.minBet;
			payload[8] = h->cfg.addBet;
			payload[9] = h->cfg.currency;
			send_message(h, 0x2b, (const u8 *)payload, 0xa * sizeof(u32) - 1);
			break;
		}

		case 0x61: /* COIN IN ACK */
		case 0x62: /* COIN OUT ACK */
			break;

		default:
			WARN_LOG(NAOMI, "Unexpected hopper message: %x\n", command);
			break;
	}
}

static void naomi_coin_in(hopper_board *h)
{
	u32 payload[7];

	insert_coin(h, 100);
	memset(payload, 0, 7 * sizeof(u32));
	payload[0] = 0;			/* atp num */
	payload[1] = HOPPER_STATUS;
	payload[2] = h->cfg.credit0;
	payload[3] = h->cfg.credit1;
	payload[4] = h->cfg.totalCredit;
	payload[5] = h->cfg.premium;
	send_message(h, 1, (const u8 *)payload, 7 * sizeof(u32) - 1);
}

static void naomi_pay_win(hopper_board *h)
{
	u32 payload[9];

	memset(payload, 0, 9 * sizeof(u32));
	payload[0] = 0;			/* atp num */
	payload[1] = HOPPER_STATUS;
	payload[2] = h->cfg.credit0;
	payload[3] = h->cfg.credit1;
	payload[4] = h->cfg.totalCredit;
	payload[5] = h->cfg.premium;
	payload[6] = h->cfg.wonAmount;
	payload[7] = h->cfg.paidAmount;
	send_message(h, 3, (const u8 *)payload, 9 * sizeof(u32) - 1);
}

static void naomi_set_defaults(hopper_board *h)
{
	switch (h->cfg.currency)
	{
	case 0:	/* Medal */
		h->cfg.medalExchRate = 5;
		h->cfg.twoWayMode = 1;
		h->cfg.autoPayOut = 0;
		h->cfg.coinDiscrimination = 1;
		h->cfg.curBase = 100;
		h->cfg.betButton = 1;
		h->cfg.maxPay = 1999900;
		h->cfg.maxCredit = 1999900;
		h->cfg.hopperSize = 39900;
		h->cfg.maxBet = 10000;
		h->cfg.minBet = 1000;
		h->cfg.addBet = 100;
		break;
	case 1:	/* Pound */
		h->cfg.medalExchRate = 0;
		h->cfg.twoWayMode = 0;
		h->cfg.autoPayOut = 1;
		h->cfg.coinDiscrimination = 0;
		h->cfg.curBase = 5;
		h->cfg.betButton = 0;
		h->cfg.maxPay = 4000;
		h->cfg.maxCredit = 1999900;
		h->cfg.hopperSize = 1999900;
		h->cfg.maxBet = 100;
		h->cfg.minBet = 100;
		h->cfg.addBet = 50;
		break;
	case 2:	/* Dollar */
	case 3:	/* Euro */
		h->cfg.medalExchRate = 0;
		h->cfg.twoWayMode = 0;
		h->cfg.autoPayOut = 0;
		h->cfg.coinDiscrimination = 0;
		h->cfg.curBase = 5;
		h->cfg.betButton = 1;
		h->cfg.maxPay = 1999900;
		h->cfg.maxCredit = 1999900;
		h->cfg.hopperSize = 39900;
		h->cfg.maxBet = 10000;
		h->cfg.minBet = 100;
		h->cfg.addBet = 100;
		break;
	case 4:	/* Token */
		h->cfg.medalExchRate = 0;
		h->cfg.twoWayMode = 0;
		h->cfg.autoPayOut = 0;
		h->cfg.coinDiscrimination = 0;
		h->cfg.curBase = 100;
		h->cfg.betButton = 1;
		h->cfg.maxPay = 1999900;
		h->cfg.maxCredit = 1999900;
		h->cfg.hopperSize = 39900;
		h->cfg.maxBet = 10000;
		h->cfg.minBet = 100;
		h->cfg.addBet = 100;
		break;
	default:
		WARN_LOG(NAOMI, "Unsupported currency %d\n", h->cfg.currency);
		break;
	}
	h->cfg.totalCredit = h->cfg.minBet;
}

static void naomi_handle(hopper_board *h, u8 command)
{
	u32 payload[0x13a];

	switch (command)
	{
		case 0x20:
		{
			/* POWER ON */
			static const char hopperErrors[][32] = {
				"UNKNOWN ERROR",
				"ROM IS BAD",
				"LOW BATTERY",
				"ROM HAS CHANGED",
				"RAM DATA IS BAD",
				"I/O ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"COIN IN JAM(HOPPER)",
				"COIN IN JAM(GAME)",
				"HOPPER OVER PAID",
				"HOPPER RUNAWAY",
				"HOPPER EMPTY/JAM",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"COM. TIME OUT",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"I/O BOARD IS BAD",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"UNKNOWN ERROR",
				"ATP NONE\0\0\0\0\0\0\0\0MAX. PAY",
				"HOPPER SIZE\0\0\0\0\0MAX. CREDIT",
				"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0DOOR IS OPEN"
			};

			/* 8: currency */
			if (h->cfg.currency != h->recv[8]) {
				h->cfg.currency = h->recv[8];
				naomi_set_defaults(h);
			}
			INFO_LOG(NAOMI, "hopper received POWER ON\n");
			memset(payload, 0, 0x13a * sizeof(u32));
			payload[0] = h->cfg.gameNumber;
			payload[1] = 0;			/* atp num */
			payload[2] = HOPPER_STATUS;	/* status bitfield. rectangle in background if != 0 */
			payload[3] = h->cfg.freePlay | (h->cfg.autoPayOut << 8) | (h->cfg.twoWayMode << 16) | (h->cfg.medalExchRate << 24);
			payload[4] = h->cfg.autoExchange | (h->cfg.coinDiscrimination << 8) | (h->cfg.curBase << 16) | (h->cfg.betButton << 24);
			payload[5] = h->cfg.maxPay;
			payload[6] = h->cfg.maxCredit;
			payload[7] = h->cfg.hopperSize;
			payload[8] = h->cfg.maxBet;
			payload[9] = h->cfg.minBet;
			payload[10] = h->cfg.addBet;
			payload[11] = h->cfg.currency;		/* ? BYTE_8c0c9fa8-b */
			payload[12] = h->cfg.credit0;
			payload[13] = h->cfg.credit1;
			payload[14] = h->cfg.totalCredit;
			payload[15] = h->cfg.premium;
			payload[16] = h->cfg.wonAmount;
			payload[17] = h->cfg.paidAmount;	/* paid amount (if autoPO) else credit won? */
			{
				size_t idx = 0x84, i;
				for (i = 0; i < sizeof(hopperErrors) / sizeof(hopperErrors[0]); i++) {
					memcpy((u8 *)payload + idx, hopperErrors[i], sizeof(hopperErrors[i]));
					idx += 32;
				}
			}
			/* init done/hopper ready? */
			payload[0x139] = 1;
			send_message(h, 0x10, (const u8 *)payload, 0x13a * sizeof(u32) - 1);

			h->started = 1;
			break;
		}

		case 0x21:	/* GET_STATUS */
		{
			INFO_LOG(NAOMI, "hopper received GET STATUS\n");
			memset(payload, 0, 3 * sizeof(u32));
			payload[0] = 0;			/* atp num? */
			payload[1] = HOPPER_STATUS;
			send_message(h, 0x11, (const u8 *)payload, 3 * sizeof(u32) - 1);
			break;
		}

		case 0x22:	/* GAME START */
		{
			INFO_LOG(NAOMI, "hopper received GAME START\n");
			/* 4: bet value: credit0 */
			/* 8:            credit1 */
			/* c:			 premium */
			bet(h, 4);
			h->cfg.wonAmount = 0;
			h->cfg.paidAmount = 0;
			memset(payload, 0, 0x17 * sizeof(u32));
			payload[0] = ++h->cfg.gameNumber;
			payload[1] = 0;				/* atp num */
			payload[2] = HOPPER_STATUS;
			payload[3] = h->cfg.credit0;
			payload[4] = h->cfg.credit1;
			payload[5] = h->cfg.totalCredit;
			payload[6] = h->cfg.premium;
			send_message(h, 0x12, (const u8 *)payload, 0x17 * sizeof(u32) - 1);
			break;
		}

		case 0x23:	/* GAME END */
		{
			INFO_LOG(NAOMI, "hopper received GAME END\n");
			/* 8: amount to pay? */
			pay_out(h, rd32(h, 8));
			memset(payload, 0, 0x15 * sizeof(u32));
			payload[0] = h->cfg.gameNumber;	/* gameNum, upper 16 bits: ? 0, -1 or -2 */
			payload[1] = h->cfg.credit0;
			payload[2] = h->cfg.credit1;
			payload[3] = h->cfg.totalCredit;
			payload[4] = h->cfg.premium;
			send_message(h, 0x13, (const u8 *)payload, 0x15 * sizeof(u32) - 1);
			break;
		}

		case 0x24:	/* TEST START */
		{
			INFO_LOG(NAOMI, "hopper received TEST START\n");
			memset(payload, 0, 0x33 * sizeof(u32));
			payload[0] = 0;				/* atp num */
			payload[1] = HOPPER_STATUS;
			payload[0x12] = 2023 | (7 << 16) | (19 << 24);	/* year month day */
			payload[0x13] = (55 << 24) | (48 << 16) | (11 << 8) | 3; /* day of week, hour, minute, second */
			payload[0x14] = h->cfg.gameNumber;
			send_message(h, 0x14, (const u8 *)payload, 0x33 * sizeof(u32) - 1);
			break;
		}

		case 0x25:	/* BACKUP CLEAR */
		{
			INFO_LOG(NAOMI, "hopper received BACKUP CLEAR\n");
			h->cfg.credit0 = 0;
			h->cfg.credit1 = 0;
			h->cfg.premium = 0;
			h->cfg.paidAmount = 0;
			h->cfg.wonAmount = 0;
			memset(payload, 0, 0x44 * sizeof(u32));
			payload[0] = h->cfg.credit0;
			payload[1] = h->cfg.credit1;
			payload[2] = h->cfg.totalCredit;
			payload[3] = h->cfg.premium;
			payload[0x23] = 2023 | (7 << 16) | (19 << 24);	/* year month day */
			payload[0x24] = (55 << 24) | (48 << 16) | (11 << 8) | 3; /* day of week, hour, minute, second */
			payload[0x25] = h->cfg.gameNumber;
			send_message(h, 0x15, (const u8 *)payload, 0x44 * sizeof(u32) - 1);
			break;
		}

		case 0x26:	/* BET */
		{
			INFO_LOG(NAOMI, "hopper received BET\n");
			memset(payload, 0, 2 * sizeof(u32));
			payload[0] = rd32(h, 4);
			send_message(h, 0x16, (const u8 *)payload, 2 * sizeof(u32) - 1);
			break;
		}

		case 0x29:	/* SWITCH */
			INFO_LOG(NAOMI, "hopper received SWITCH\n");
			/* 4: test button */
			/* 5: service button */
			/* 6-7: c01 in test menu */
			break;

		case 0x2a:	/* CONFIG HOP */
		{
			INFO_LOG(NAOMI, "hopper received CONFIG HOP\n");
			if (h->recv[2] == 0x28 && h->recv[3] == 0)
			{
				h->cfg.freePlay = h->recv[4] & 1;
				h->cfg.autoPayOut = h->recv[5] & 1;
				h->cfg.twoWayMode = h->recv[6] & 1;
				h->cfg.medalExchRate = h->recv[7];
				h->cfg.autoExchange = h->recv[8] & 1;
				h->cfg.coinDiscrimination = h->recv[9] & 1;
				h->cfg.curBase = h->recv[0xa];
				h->cfg.betButton = h->recv[0xb] & 1;
				h->cfg.maxPay = rd32(h, 0xc);
				h->cfg.maxCredit = rd32(h, 0x10);
				h->cfg.hopperSize = rd32(h, 0x14);
				h->cfg.maxBet = rd32(h, 0x18);
				h->cfg.minBet = rd32(h, 0x1c);
				h->cfg.addBet = rd32(h, 0x20);
			}
			memset(payload, 0, 9 * sizeof(u32));
			payload[0] = h->cfg.freePlay | (h->cfg.autoPayOut << 8) | (h->cfg.twoWayMode << 16) | (h->cfg.medalExchRate << 24);
			payload[1] = h->cfg.autoExchange | (h->cfg.coinDiscrimination << 8) | (h->cfg.curBase << 16) | (h->cfg.betButton << 24);
			payload[2] = h->cfg.maxPay;
			payload[3] = h->cfg.maxCredit;
			payload[4] = h->cfg.hopperSize;
			payload[5] = h->cfg.maxBet;
			payload[6] = h->cfg.minBet;
			payload[7] = h->cfg.addBet;
			send_message(h, 0x1a, (const u8 *)payload, 9 * sizeof(u32) - 1);
			break;
		}

		case 0x2f: /* TEST DATA MON */
			INFO_LOG(NAOMI, "hopper received cmd TEST DATA MON\n");
			/* no reply */
			break;

		case 0x31: /* COIN IN ACK? */
		case 0x32: /* COIN OUT ACK? */
			/* no reply */
			break;

		default:
			WARN_LOG(NAOMI, "Unexpected hopper message: %x\n", command);
			break;
	}
}

static void clear_recv(hopper_board *h)
{
	memset(h->recv, 0, sizeof(h->recv));
	h->recvLen = 0;
	h->expectedBytes = 0;
}

/* What has changed of what the board keeps, to its file */
static void save_config(hopper_board *h)
{
	if (!memcmp(&h->cfg, &h->saved, sizeof(h->cfg)))
		return;
	h->saved = h->cfg;
	periph_save_file("-hopper.bin", &h->saved, sizeof(h->saved), &config_save_failed);
}

static u8 op_read(serial_pipe *pipe)
{
	return periph_queue_pop(&((hopper_board *)pipe)->to_send);
}

static int op_available(serial_pipe *pipe)
{
	return (int)((hopper_board *)pipe)->to_send.count;
}

static void op_write(serial_pipe *pipe, u8 data)
{
	hopper_board *h = (hopper_board *)pipe;

	if (h->recvLen == 0 && data != 'H')
	{
		WARN_LOG(NAOMI, "Ignored data %02x %c\n", data, data);
		return;
	}
	h->recv[h->recvLen++] = data;
	if (h->recvLen == 3)
		h->expectedBytes = data;
	else if (h->recvLen == 4)
	{
		h->expectedBytes += data << 8;
		/* (no message is shorter than its header and checksum, and none the game sends is this long) */
		if (h->expectedBytes < 5 || h->expectedBytes > sizeof(h->recv))
		{
			WARN_LOG(NAOMI, "Hopper message of %d bytes dropped\n", h->expectedBytes);
			clear_recv(h);
		}
	}
	else if (h->expectedBytes > 0 && h->recvLen == h->expectedBytes)
	{
		if (h->sega)
			sega_handle(h, h->recv[1]);
		else
			naomi_handle(h, h->recv[1]);
		clear_recv(h);
		save_config(h);
	}
}

static const serial_pipe_ops hopper_ops = { op_write, op_available, op_read };

static void clear_state(hopper_board *h)
{
	clear_recv(h);
	periph_queue_clear(&h->to_send);
	h->started = 0;
	h->coinKey = 0;
}

void hopper_init(void)
{
	hopper_term();
	memset(&board, 0, sizeof(board));
	board.pipe.ops = &hopper_ops;
	board.sega = !strcmp(naomi_game_id, "KICK '4' CASH");
	periph_queue_init(&board.to_send, board.to_send_buf, sizeof(board.to_send_buf));
	if (periph_load_file("-hopper.bin", &board.cfg, sizeof(board.cfg)) == sizeof(board.cfg))
		config_normalize(&board.cfg);
	else
	{
		INFO_LOG(NAOMI, "Hopper config not found\n");
		config_initial(&board.cfg);
	}
	board.saved = board.cfg;
	hopper = &board;
	scif_set_pipe(&hopper->pipe);
}

void hopper_term(void)
{
	if (hopper == NULL)
		return;
	scif_set_pipe(NULL);
	hopper = NULL;
}

bool hopper_active(void)
{
	return hopper != NULL;
}

/* The coin switch: a coin goes into the hopper */
void hopper_poll(void)
{
	bool down;

	if (hopper == NULL || !hopper->started)
		return;
	down = (mcfg_ArcadeKcode(0) & NAOMI_COIN_KEY) == 0;
	if (down && !hopper->coinKey)
	{
		if (hopper->sega)
			sega_coin_in(hopper);
		else
			naomi_coin_in(hopper);
		save_config(hopper);
	}
	hopper->coinKey = down;
}

/* The hopper board's part of a save state (V24): what it keeps (the
 * credits and settings, which are in its file too), the message coming
 * in, the bytes it has still to send, whether the game has started it and
 * whether the coin switch was down. 2404 bytes. */
void hopper_serialize_v24(void **data, unsigned int *total_size)
{
	u32 words[23];
	u8 flags[2];
	u16 recvLen, expected;

	if (hopper == NULL)
		return;
	memcpy(words, &hopper->cfg, sizeof(words));
	flags[0] = hopper->started;
	flags[1] = hopper->coinKey;
	recvLen = (u16)hopper->recvLen;
	expected = (u16)hopper->expectedBytes;
	LIBRETRO_SA(words, 23);
	LIBRETRO_SA(hopper->recv, sizeof(hopper->recv));
	LIBRETRO_S(recvLen);
	LIBRETRO_S(expected);
	periph_queue_serialize(&hopper->to_send, data, total_size);
	LIBRETRO_SA(flags, 2);
}

bool hopper_unserialize_v24(void **data, unsigned int *total_size)
{
	u32 words[23];
	u8 flags[2];
	u16 recvLen = 0, expected = 0;

	if (hopper == NULL)
		return true;
	flags[0] = flags[1] = 0;
	memcpy(words, &hopper->cfg, sizeof(words));
	LIBRETRO_USA(words, 23);
	LIBRETRO_USA(hopper->recv, sizeof(hopper->recv));
	LIBRETRO_US(recvLen);
	LIBRETRO_US(expected);
	periph_queue_unserialize(&hopper->to_send, data, total_size);
	if (!LIBRETRO_USA(flags, 2))
		return false;
	memcpy(&hopper->cfg, words, sizeof(words));
	config_normalize(&hopper->cfg);
	hopper->recvLen = recvLen < sizeof(hopper->recv) ? recvLen : 0;
	hopper->expectedBytes = expected <= sizeof(hopper->recv) ? expected : 0;
	if (hopper->recvLen >= 4 && (hopper->expectedBytes < 5 || hopper->recvLen >= hopper->expectedBytes))
	{
		hopper->recvLen = 0;
		hopper->expectedBytes = 0;
	}
	hopper->started = flags[0] != 0;
	hopper->coinKey = flags[1] != 0;
	return true;
}

/* An older state: nothing on its way, and not started - the game will
 * start it again. What it keeps is what its file has. */
void hopper_state_before_v24(void)
{
	if (hopper != NULL)
		clear_state(hopper);
}
