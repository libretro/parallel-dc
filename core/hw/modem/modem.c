/*
	Originally based on nullDC: nullExtDev/modem.cpp

	Created on: Sep 9, 2018

	Copyright 2018 skmp, flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <time.h>
#include <string.h>
#include "modem.h"
#include "modem_regs.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_sched.h"
#include <libretro.h>
#include <features/features_cpu.h>
#include "network/picoppp.h"
#include "reios/reios.h"
#include "v42.h"

#define MODEM_COUNTRY_RES 0
#define MODEM_COUNTRY_JAP 1
#define MODEM_COUNTRY_USA 2

#define MODEM_MAKER_SEGA 0
#define MODEM_MAKER_ROCKWELL 1

#define MODEM_DEVICE_TYPE_336K 0

static const u32 MODEM_ID[2] =
{
	MODEM_COUNTRY_USA,
	(MODEM_MAKER_ROCKWELL<<4) | (MODEM_DEVICE_TYPE_336K),
};

/* The registers (modem_regs.h) */
static u8 modem_regs[MODEM_REGS];

static u8 dspram[0x1000];

/* (-1 on a machine that has no modem, and has never registered the event) */
int modem_sched = -1;

enum ModemStates
{
	MS_INVALID,				/* needs reset */
	MS_RESET,				/* reset is low */
	MS_RESETING,			/* reset is hi */
	MS_ST_CONTROLER,		/* Controller self test */
	MS_ST_DSP,				/* DSP self test */
	MS_END_DSP,				/* DSP self test end */
	MS_NORMAL	 			/* Normal operation */
};
static int state = MS_INVALID;

enum ConnectState
{
	DISCONNECTED,
	DIALING,
	RINGING,
	HANDSHAKING,
	PRE_CONNECTED,
	CONNECTED,
	NEGO_COMPLETE			/* the V.8 bis exchange is over: the game dials the line itself next */
};
static int connect_state = DISCONNECTED;
/* The transmitter's FIFO: how many of the bytes the game wrote have not gone yet */
static float txFifoSize = 0.f;
static u8 rxFifoBuf[16];
static byte_fifo rxFifo = { rxFifoBuf, sizeof(rxFifoBuf), 0, 0 };
/* A byte at the speed of the connection, and the speed sent at over the one received at */
#define DEFAULT_CYCLES_PER_BYTE (SH4_MAIN_CLOCK / 1000000 * 238)
static int cyclesPerByte = DEFAULT_CYCLES_PER_BYTE;
static float txRxRatio = 1.f;

/* A field of a register: its register and its bits (modem_regs.h) */
static int reg_bit(u32 reg, u8 mask)
{
	return (modem_regs[reg] & mask) != 0;
}

static void reg_set(u32 reg, u8 mask, int v)
{
	if (v & 1)
		modem_regs[reg] |= mask;
	else
		modem_regs[reg] &= (u8)~mask;
}

static u16 reg18_19(void)
{
	u16 v;
	memcpy(&v, &modem_regs[0x18], sizeof(v));
	return v;
}

static void set_reg18_19(u16 v)
{
	memcpy(&modem_regs[0x18], &v, sizeof(v));
}

static void set_speed(u32 v)
{
	modem_regs[0x0e] = (u8)((modem_regs[0x0e] & ~MR0E_SPEED_MASK) | (v & MR0E_SPEED_MASK));
}

static void schedule_callback(int ms);

static void update_interrupt(void)
{
	reg_set(MR1E_RDBIA, reg_bit(MR1E_RDBIE) && reg_bit(MR1E_RDBF));
	reg_set(MR1E_TDBIA, reg_bit(MR1E_TDBIE) && reg_bit(MR1E_TDBE));

	if (reg_bit(MR1F_NCIA) || reg_bit(MR1F_NSIA) || reg_bit(MR1E_RDBIA) || reg_bit(MR1E_TDBIA))
		asic_RaiseInterrupt(holly_EXP_8BIT);
	else
		asic_CancelInterrupt(holly_EXP_8BIT);
}

static u32 get_masked_status(u32 reg)
{
	u8 int_mask = dspram[regs_int_mask_addr[reg]];
	return int_mask & modem_regs[reg];
}

/* A status bit of @reg set to @value, and the status interrupt asked for
 * if that changes what the game wants to be told of */
static void set_status(u32 reg, u8 mask, int value)
{
	if (reg_bit(reg, mask) != (value != 0))
	{
		if (!reg_bit(MR1F_NSIE))
			reg_set(reg, mask, value);
		else
		{
			const u8 before = (u8)get_masked_status(reg);
			reg_set(reg, mask, value);
			if (before != get_masked_status(reg))
				reg_set(MR1F_NSIA, 1);
		}
	}
}

static void ControllerTestEnd(void);
static void DSPTestStart(void);
static void DSPTestEnd(void);

static u64 last_dial_time;
static u64 connected_time;

/* The telephone line.
 *
 * A save state holds the modem - its registers, the DSP's memory, how far a
 * call has got - but not the line: the PPP stack at the far end and the
 * host's sockets behind it are this process's, and cannot be put back. So a
 * modem that is connected says in the state which line it is connected over,
 * by a number no other line has, and the modem of a state that is loaded is
 * only on the line again if that line is the one this process has up:
 *
 * - it is (run-ahead and rewind load states of a moment ago): the line is
 *   left alone. What the game took from it since the state was made is
 *   given to it again from rx_log, in the same order, and what it sent since
 *   then, and now sends again, is not sent twice;
 * - it is not (a state of another session or of an earlier call, or one from
 *   too long ago for rx_log): a line that is up now is hung up, since the
 *   game of the state knows nothing of it, and the state's own line is dead.
 *   Nothing comes over a dead line, and after as long as a modem waits for a
 *   carrier that has gone, the modem reports the far end's hanging up: the
 *   game can dial again.
 *
 * What comes over the line is taken at each of the modem's steps into
 * line_fifo, which is in the state, and all the modem does with it is done
 * from there. Each byte's place in rx_log has the time it was taken at: a
 * state loaded on the live line takes them again at the same times, and
 * nothing from the line itself at a time the line was already looked at,
 * so that what the game is given is the same, byte for byte and step for
 * step. */
#define LINE_WINDOW 4096
/* A modem gives the line up after this long without a carrier */
#define DEAD_LINE_CYCLES ((u64)SH4_MAIN_CLOCK * 14 / 10)
static u32 line_id;			/* the line the modem is connected over, 0 when it is not connected (in the state) */
static u32 rx_pos;			/* bytes the modem has taken from that line (in the state) */
static u32 tx_pos;			/* bytes the modem has given to it (in the state) */
static u16 dead_steps;		/* steps a connected modem has gone without its line (in the state) */
static u32 live_line;		/* the line this process has up, 0 for none */
static u32 rx_taken;		/* bytes taken from the live line */
static u32 tx_sent;			/* bytes given to the live line */
static u8 rx_log[LINE_WINDOW];	/* the last bytes taken from the live line */
static u64 rx_log_time[LINE_WINDOW];	/* and when */
static u64 line_looked;		/* the last time the live line itself was read */
static u64 live_line_made;	/* when the live line was made (emulated time) */
static u8 line_fifo_buf[256];
static byte_fifo line_fifo = { line_fifo_buf, sizeof(line_fifo_buf), 0, 0 };	/* taken from the line, not yet used (in the state) */

static bool line_is_live(void)
{
	return live_line != 0 && line_id == live_line;
}

/* The line is hung up, if one is up */
static void line_close(void)
{
	stop_pico();
	live_line = 0;
}

/* What has come over the line, into line_fifo */
static void line_fetch(void)
{
	const u64 now = sh4_sched_now64();

	while (byte_fifo_room(&line_fifo) > 0)
	{
		int c;

		if (rx_pos != rx_taken)
		{
			/* Again: at the time it was taken the first time */
			if (rx_log_time[rx_pos % LINE_WINDOW] > now)
				break;
			c = rx_log[rx_pos % LINE_WINDOW];
		}
		else
		{
			/* The line had nothing more at this time when it was read */
			if (now <= line_looked)
				break;
			c = read_pico();
			if (c < 0)
				break;
			rx_log[rx_taken % LINE_WINDOW] = (u8)c;
			rx_log_time[rx_taken % LINE_WINDOW] = now;
			rx_taken++;
		}
		rx_pos++;
		byte_fifo_push(&line_fifo, (u8)c);
	}
	if (rx_pos == rx_taken && now > line_looked)
		line_looked = now;
}

static void line_write(u8 b)
{
	if (tx_pos == tx_sent)
	{
		write_pico(b);
		tx_sent++;
	}
	tx_pos++;
}

static void line_lost(void);

/* The modem's end of the line: what has come over it, and what is sent. In
 * synchronous mode the V.42 protocol is between these and the game. */
static int net_read(void *ctx)
{
	(void)ctx;
	return byte_fifo_pop(&line_fifo);
}

static int net_available(void *ctx)
{
	(void)ctx;
	return byte_fifo_size(&line_fifo);
}

static void net_write(void *ctx, u8 v)
{
	(void)ctx;
	/* (what is sent over a dead line is lost) */
	if (line_is_live())
		line_write(v);
}

static const modem_stream netIn = { net_read, net_available, NULL, NULL };
static const modem_stream netOut = { NULL, NULL, net_write, NULL };

static v42_protocol v42Proto;
static bool v8bis;
static v8bis_protocol v8bisProto;

static int v42_stream_read(void *ctx) { (void)ctx; return v42_read(&v42Proto); }
static int v42_stream_available(void *ctx) { (void)ctx; return 1; }
static void v42_stream_write(void *ctx, u8 v) { (void)ctx; v42_write(&v42Proto, v); }
static const modem_stream v42Stream = { v42_stream_read, v42_stream_available, v42_stream_write, NULL };

static int v8bis_stream_read(void *ctx) { (void)ctx; return v8bis_read(&v8bisProto); }
static int v8bis_stream_available(void *ctx) { (void)ctx; return v8bis_available(&v8bisProto); }
static void v8bis_stream_write(void *ctx, u8 v) { (void)ctx; v8bis_write(&v8bisProto, v); }
static const modem_stream v8bisStream = { v8bis_stream_read, v8bis_stream_available, v8bis_stream_write, NULL };

static const modem_stream *curInput = &netIn;
static const modem_stream *curOutput = &netOut;

/* The protocols, set up once, as they are before there is a machine */
static void protocols_setup(void)
{
	static bool done;
	if (done)
		return;
	done = true;
	v42_init(&v42Proto, &netIn, &netOut);
	v8bis_init(&v8bisProto);
}

/* The disc's product number is @id, without the spaces after it */
static bool game_is(const char *id)
{
	size_t len = sizeof(ip_meta.product_number);
	while (len > 0 && ip_meta.product_number[len - 1] == ' ')
		len--;
	return len == strlen(id) && memcmp(ip_meta.product_number, id, len) == 0;
}

static void updateRxFifoStatus(void)
{
	/* The rx fifo timeout set in dspram[0x32c]:2-4 doesn't seem to be used */
	if (byte_fifo_size(&rxFifo) != 0)
	{
		modem_regs[MR00] = (u8)byte_fifo_front(&rxFifo);
		reg_set(MR1E_RDBF, (unsigned)byte_fifo_size(&rxFifo) >= rxFifoTrigger[(dspram[0x32c] >> 6) & 3]);
		if (reg_bit(MR04_FIFOEN))
			set_status(MR0C_RXFNE, 1);
		set_status(MR01_RXHF, byte_fifo_size(&rxFifo) >= 8);
	}
	else
	{
		reg_set(MR1E_RDBF, 0);
		if (reg_bit(MR04_FIFOEN))
			set_status(MR0C_RXFNE, 0);
		set_status(MR01_RXHF, 0);
	}
}

static void configureStreams(void)
{
	if (v8bis)
	{
		curInput = &v8bisStream;
		curOutput = &v8bisStream;
	}
	else if (reg_bit(MR08_ASYN) == 0)
	{
		/* Synchronous: V.42 or V.14 */
		curInput = &v42Stream;
		curOutput = &v42Stream;
	}
	else
	{
		curInput = &netIn;
		curOutput = &netOut;
	}
}

/* The speed of the connection, from the configuration the game has asked
 * for (CONF). False if it is not one a connection can be made with. */
static bool setModemSpeedFromCONF(void)
{
	static const int v90Speeds[] = {
		0, 28800, 29333, 30667, 32000, 33333, 34667, 36000, 37333, 38667, 40000, 41333,
		42667, 44000, 45333, 46667, 48000, 49333, 50667, 52000, 53333, 54667, 56000
	};
	static const int v34Speeds[] = {
		0, 2400, 4800, 7200, 9600, 12000, 14400, 16800, 19200, 21600, 24000, 26400, 28800, 31200, 33600
	};
	const u8 conf = modem_regs[MR12];
	int speed;
	int rxSpeed = 0;
	set_speed(0);
	switch (conf)
	{
	default:
		if (conf >= 0xe0 && conf <= 0xf6)
		{
			/* V.90 */
			rxSpeed = v90Speeds[conf - 0xe0];
			if (rxSpeed == 0)
				speed = 0;
			else
			{
				dspram[0x2e4] = conf - 0xe0;
				speed = 33600;
				set_speed(0x10);
				dspram[0x2e5] = 0xd;
			}
		}
		else if (conf >= 0x90 && conf <= 0x9d)
		{
			/* K56flex */
			if (conf == 0x90)
				speed = 0;
			else
			{
				rxSpeed = 30000 + (conf - 0x90) * 2000;
				speed = 33600;
				set_speed(0x10);
			}
		}
		else if (conf >= 0xc0 && conf <= 0xce)
		{
			/* V.34 */
			speed = v34Speeds[conf - 0xc0];
			if (speed != 0)
				set_speed(conf - 0xc0 + 2);
			dspram[0x2e4] = conf - 0xc0;
			dspram[0x2e5] = dspram[0x2e4];
		}
		else
			speed = 0;
		break;

	/* V.33 */
	case 0x31: speed = 14400; break;
	case 0x32: speed = 12000; break;
	case 0x34: speed = 9600; break;
	case 0x38: speed = 7200; break;
	/* V.32 [bis] */
	case 0x76: speed = 14400; break;
	case 0x72: speed = 12000; break;
	case 0x74: speed = 9600; break;
	case 0x75: speed = 9600; break;
	case 0x78: speed = 7200; break;
	case 0x71: speed = 4800; break;
	case 0x70: speed = 0; break;
	/* V.17 */
	case 0xb1: speed = 14400; break;
	case 0xb2: speed = 12000; break;
	case 0xb4: speed = 9600; break;
	case 0xb8: speed = 7200; break;
	/* V.29 */
	case 0x14: speed = 9600; break;
	case 0x12: speed = 7200; break;
	case 0x11: speed = 4800; break;
	/* V.27 ter, V.22 bis, V.22, V.21 */
	case 0x02: speed = 4800; break;
	case 0x01: speed = 2400; break;
	case 0x84: speed = 2400; break;
	case 0x82: speed = 1200; break;
	case 0x52: speed = 1200; break;
	case 0x51: speed = 600; break;
	case 0xa0: speed = 300; break;
	case 0xa8: speed = 300; break;
	/* Bell, V.23 */
	case 0x23: speed = 4800; break;
	case 0x62: speed = 1200; break;
	case 0x60: speed = 300; break;
	case 0xa4: speed = 1200; rxSpeed = 75; break;
	case 0xa1: speed = 75; rxSpeed = 1200; break;
	}

	if (rxSpeed == 0)
		rxSpeed = speed;
	if (speed == 0)
	{
		set_speed(0);
		cyclesPerByte = SH4_MAIN_CLOCK * 8 / 300;
		txRxRatio = 1.f;
	}
	else
	{
		if ((modem_regs[0x0e] & MR0E_SPEED_MASK) == 0)
		{
			switch (speed)
			{
			case 14400: set_speed(7); break;
			case 12000: set_speed(6); break;
			case 9600: set_speed(5); break;
			case 7200: set_speed(8); break;
			case 4800: set_speed(4); break;
			case 2400: set_speed(3); break;
			case 1200: set_speed(2); break;
			case 600: set_speed(1); break;
			default: set_speed(0); break;
			}
		}
		/* The modem steps at the speed it receives at, a byte to the game
		 * at each step, and sends at its own speed: speed / rxSpeed bytes
		 * a step. (Stepping at the sending speed, with that ratio, had a
		 * V.90 line receive at 33600 and send at 20160 bits a second.) */
		cyclesPerByte = SH4_MAIN_CLOCK * 8 / rxSpeed;
		txRxRatio = (float)speed / rxSpeed;
	}
	NOTICE_LOG(MODEM, "MODEM Connected %s %d/%d bps\n", reg_bit(MR08_ASYN) ? "ASYNC" : "SYNC", rxSpeed, speed);
	return speed != 0;
}

#ifndef NDEBUG
static unsigned long last_comm_stats;
static int sent_bytes;
static int recvd_bytes;
static FILE *recv_fp;
static FILE *sent_fp;
static bool mem_dumped;
#endif

/* The connection made, once the handshake is over */
static void pre_connected(void)
{
	bool validConfig;
	if (reg_bit(MR03_RLSDE))
		set_status(MR0F_RLSD, 1);
	if (modem_regs[MR12] == 0xAA)
	{
		/* V8 AUTO mode */
		dspram[0x302] |= 1 << 4;				/* protocol octet received */
		dspram[0x302] = (dspram[0x302] & 0x1f) | (dspram[0x304] & 0xe0);	/* Received Call Function */
		dspram[0x301] |= 1 << 4;				/* JM detected */
		dspram[0x303] |= 0xE1;					/* Received protocol bits (?), Received GSTN Octet */
		dspram[0x2e3] = 5;						/* Symbol rate 3429 */
		/* set the negotiated speed to the max configured by the game */
		if ((dspram[0x6a3] & 1) == 1 && (dspram[0x6a2] & 0x20) == 0x20)
		{
			if (game_is("HDR-0113") || game_is("HDR-0091"))
				/* Set symmetric speed (V.34 33.6) for Power Smash and Pro Yakyuu Team to avoid lag */
				modem_regs[MR12] = 0xCE;
			else
			{
				/* V.90 */
				modem_regs[MR12] = 0xE0 | (dspram[0x6a2] & 0x1F);
				dspram[0x6c1] = 0xff;		/* V90 Server DPCM Transmit Data Rate Mask */
				dspram[0x6c2] = 0xff;
				dspram[0x6c3] = 0x3f;
			}
		}
		else
			modem_regs[MR12] = dspram[0x309];
		validConfig = setModemSpeedFromCONF();
		dspram[0x239] = 12;						/* RTD 0 @ 3429 sym rate */
		if (reg_bit(MR1F_NSIE))
		{
			/* CONF */
			if (dspram[regs_int_mask_addr[0x12]] & (1 << 7))
				reg_set(MR1F_NSIA, 1);
		}
	}
	else
		validConfig = setModemSpeedFromCONF();
	if (reg_bit(MR1F_NSIE))
	{
		/* SPEED */
		if (dspram[regs_int_mask_addr[0x0e]] & 0x1f)
			reg_set(MR1F_NSIA, 1);
	}
	if (reg_bit(MR15_AUTO))
	{
		reg_set(MR09_DATA, 1);
		reg_set(MR15_AUTO, 0);
	}
	if (!validConfig)
	{
		modem_regs[MR14] = 1;	/* ABCODE: (fake) FED turned off while waiting to get into round trip delay estimate (RTDE) */
		connect_state = DISCONNECTED;
	}
	else
	{
		INFO_LOG(MODEM, "MODEM Connected\n");
		modem_regs[MR14] = 0x00;			/* ABCODE: no error */
		set_status(MR0F_DSR, 1);
		if (reg_bit(MR02_RTSDE))
			set_status(MR0F_RTSDT, 1);

		/* Energy detected. Required for games to detect the connection */
		set_status(MR0F_FED, 1);

		if (live_line != 0 && line_id == 0 && live_line_made == sh4_sched_now64()
				&& rx_taken <= LINE_WINDOW)
		{
			/* The line was made at this very moment, by the machine of a
			 * state loaded since (run-ahead): it is this call's, and what
			 * came over it is given again as it came */
			line_id = live_line;
			rx_pos = 0;
			tx_pos = 0;
		}
		else
		{
			line_close();
			start_pico();
			/* The time is the line's number: no other line, of this
			 * process or of another, was made at this microsecond */
			live_line = (u32)cpu_features_get_time_usec();
			if (live_line == 0)
				live_line = 1;
			live_line_made = sh4_sched_now64();
			line_id = live_line;
			rx_pos = rx_taken = 0;
			tx_pos = tx_sent = 0;
			line_looked = 0;
		}
		byte_fifo_clear(&line_fifo);
		dead_steps = 0;
		connect_state = CONNECTED;
		connected_time = 0;
	}
	if (reg_bit(MR1F_NSIE))
	{
		/* ABCODE */
		if (dspram[regs_int_mask_addr[0x014]])
			reg_set(MR1F_NSIA, 1);
	}
	reg_set(MR1F_NEWS, 1);

	/* V.34 Remote Modem Data Rate Capability */
	dspram[0x208] = 0xff;	/* 2.4 - 19.2 kpbs supported */
	dspram[0x209] = 0xbf;	/* 21.6 - 33.6 kpbs supported, asymmetric supported */

	v42_reset(&v42Proto);
}

/* A step of a connection: a byte to the game, the line's at its speed */
static int connected_step(void)
{
	const int callback_cycles = cyclesPerByte;
	float tx;
#ifndef NDEBUG
	if (!mem_dumped)
	{
		int i;
		mem_dumped = true;
		for (i = 0 ; i < (int)sizeof(modem_regs); i++)
			DEBUG_LOG(MODEM, "modem_regs %02x == %02x\n", i, modem_regs[i]);
	}
#endif
	if (!line_is_live())
	{
		/* The line of a state that was loaded, which is not up */
		if (++dead_steps >= DEAD_LINE_CYCLES / (u64)cyclesPerByte)
		{
			line_lost();
			return callback_cycles;
		}
	}
	else
		line_fetch();
	tx = txFifoSize - txRxRatio;
	txFifoSize = tx < 0.f ? 0.f : tx;
	reg_set(MR1E_TDBE, txFifoSize == 0.f);

	if (v8bis && v8bis_completed(&v8bisProto))
	{
		connect_state = NEGO_COMPLETE;
		v8bis = false;
		configureStreams();
	}
	if (byte_fifo_size(&rxFifo) < 16)
	{
		const int c = curInput->read(curInput->ctx);
		if (c >= 0)
		{
#ifndef NDEBUG
			recvd_bytes++;
#endif
			byte_fifo_push(&rxFifo, (u8)c);
			updateRxFifoStatus();
		}
	}
	return callback_cycles;
}

static int modem_sched_func(int tag, int cycles, int jitter)
{
	int callback_cycles = 0;
	(void)tag;
	(void)cycles;
#ifndef NDEBUG
	/* (the stats, every two seconds) */
	if ((unsigned long)time(NULL) - last_comm_stats >= 2)
	{
		if (last_comm_stats != 0)
		{
			DEBUG_LOG(MODEM, "Stats sent %d (%.2f kB/s) received %d (%.2f kB/s) TDBE %d RDBF %d\n", sent_bytes, sent_bytes / 2000.0,
					recvd_bytes, recvd_bytes / 2000.0,
					reg_bit(MR1E_TDBE), reg_bit(MR1E_RDBF));
			sent_bytes = 0;
			recvd_bytes = 0;
		}
		last_comm_stats = (unsigned long)time(NULL);
	}
#endif

	switch (state)
	{
	case MS_ST_CONTROLER:
		ControllerTestEnd();
		break;
	case MS_ST_DSP:
		DSPTestStart();
		break;
	case MS_END_DSP:
		DSPTestEnd();
		break;
	case MS_NORMAL:
		reg_set(MR1F_NEWC, 0);		/* Not needed when state is CONNECTED but who cares */

		switch (connect_state)
		{
		case DIALING:
			if (last_dial_time != 0 && sh4_sched_now64() - last_dial_time >= (u64)(SH4_MAIN_CLOCK + jitter))
			{
				DEBUG_LOG(MODEM, "Switching to RINGING state\n");
				connect_state = RINGING;
				schedule_callback(100);
			}
			else
			{
				last_dial_time = sh4_sched_now64();

				reg_set(MR1E_TDBE, 1);
				schedule_callback(1000);	/* To switch to Ringing state */
			}
			break;
		case RINGING:
		case NEGO_COMPLETE:
			last_dial_time = 0;
			DEBUG_LOG(MODEM, "\t\t *** %s STATE ***\n", connect_state == RINGING ? "RINGING" : "NEGO COMPLETE");
			reg_set(MR1F_NEWS, 1);
			if (!reg_bit(MR09_DATA) && connect_state == RINGING)
			{
				set_status(MR0F_RI, 1);
				set_status(MR0B_ATV25, 1);
			}
			break;
		case HANDSHAKING:
			DEBUG_LOG(MODEM, "\t\t *** HANDSHAKING STATE ***\n");
			if (modem_regs[MR12] == 0xAA)
				/* V8 AUTO mode */
				dspram[0x302] |= 1 << 3;				/* ANSam detected */
			reg_set(MR1F_NEWS, 1);
			set_status(MR0F_RI, 0);
			set_status(MR0B_ATV25, 0);

			callback_cycles = SH4_MAIN_CLOCK / 1000 * 500;		/* 500 ms */
			connect_state = PRE_CONNECTED;
			break;

		case PRE_CONNECTED:
			pre_connected();
			callback_cycles = SH4_MAIN_CLOCK / 1000000 * 238;	/* 238 us */
			break;

		case CONNECTED:
			callback_cycles = connected_step();
			break;

		default:
			break;
		}
		break;

	default:
		break;
	}
	update_interrupt();

	return callback_cycles;
}

void ModemInit(void)
{
	protocols_setup();
	modem_sched = sh4_sched_register(0, modem_sched_func);
}

void ModemTerm(void)
{
	protocols_setup();
	line_close();
	line_id = 0;
	dead_steps = 0;
	/* A reset of the console calls this as well. The step of the chip's
	 * self test or of a call that was due is not to come: it used to,
	 * after the reset and with the registers the game before had left,
	 * and when that game was connected it came 4200 times a second from
	 * then on. And a chip that has been used is as it is when the console
	 * is switched on: waiting for the game to reset it, nothing dialled,
	 * no interrupt asked for. */
	if (sh4_sched_is_scheduled(modem_sched))
		sh4_sched_request(modem_sched, -1);
	byte_fifo_clear(&line_fifo);
	if (state != MS_INVALID)
	{
		memset(modem_regs, 0, sizeof(modem_regs));
		state = MS_INVALID;
		connect_state = DISCONNECTED;
		last_dial_time = 0;
		connected_time = 0;
		txFifoSize = 0.f;
		byte_fifo_clear(&rxFifo);
		cyclesPerByte = DEFAULT_CYCLES_PER_BYTE;
		txRxRatio = 1.f;
		v8bis = false;
		v8bis_reset(&v8bisProto);
		v42_reset(&v42Proto);
		configureStreams();
		update_interrupt();
	}
}

static void schedule_callback(int ms)
{
	sh4_sched_request(modem_sched, SH4_MAIN_CLOCK / 1000 * ms);
}

/* The far end has hung up: the carrier and what comes with it are gone,
 * the abort code says that a cleardown was asked for, and the modem is
 * ready to dial again. */
static void line_lost(void)
{
	connect_state = DISCONNECTED;
	line_id = 0;
	dead_steps = 0;

	modem_regs[MR14] = 0x96;			/* ABCODE: cleardown */
	if (reg_bit(MR1F_NSIE))
	{
		/* ABCODE */
		if (dspram[regs_int_mask_addr[0x014]])
			reg_set(MR1F_NSIA, 1);
	}
	reg_set(MR1F_NEWS, 1);
	set_status(MR0F_RLSD, 0);
	set_status(MR0F_FED, 0);
	set_status(MR0F_DSR, 0);
	set_status(MR0F_RTSDT, 0);
	reg_set(MR1E_TDBE, 1);
}

static void SetReg16(u32 rh, u32 rl, u32 v)
{
	modem_regs[rh] = (u8)(v >> 8);
	modem_regs[rl] = (u8)(v & 0xFF);
}

static void NormalDefaultRegs(void)
{
	verify(state == MS_NORMAL);
	verify(sizeof(regs_write_mask) == sizeof(modem_regs));
	verify(sizeof(por_dspram) == sizeof(dspram));

	/* Default values for normal state */
	memset(modem_regs, 0, sizeof(modem_regs));
	memcpy(dspram, por_dspram, sizeof(dspram));
	reg_set(MR05_CEQ, 1);
	modem_regs[MR12] = 0x76;	/* CONF: V.32 bis TCM 14400 */
	reg_set(MR09_DATA, 1);
	reg_set(MR09_DTMF, 1);
	reg_set(MR15_HWRWK, 1);
	reg_set(MR07_RDLE, 1);
	reg_set(MR15_RDWK, 1);
	reg_set(MR03_RLSDE, 1);
	reg_set(MR02_TDE, 1);
	modem_regs[0x13] = (u8)((modem_regs[0x13] & ~MR13_TLVL_MASK) | 0x9);

	reg_set(MR1E_TDBE, 1);
	connect_state = DISCONNECTED;
	last_dial_time = 0;
	txFifoSize = 0.f;
	byte_fifo_clear(&rxFifo);
	v8bis = false;
	configureStreams();
}

static void DSPTestEnd(void)
{
	verify(state == MS_END_DSP);
	state = MS_NORMAL;

	DEBUG_LOG(MODEM, "DSPTestEnd\n");
	NormalDefaultRegs();
}

static void DSPTestStart(void)
{
	verify(state == MS_ST_DSP);
	state = MS_END_DSP;
	DEBUG_LOG(MODEM, "DSPTestStart\n");

	reg_set(MR1E_TDBE, 1);
	SetReg16(0x1B, 0x1A, 0xF083);	/* EC checksum */
	SetReg16(0x19, 0x18, 0x46EE);	/* Multiplier checksum */
	SetReg16(0x17, 0x16, 0x00FA);	/* RAM checksum */
	SetReg16(0x15, 0x14, 0x0A09);	/* ROM checksum */
	SetReg16(0x13, 0x12, 0x3730);	/* Part number */
	SetReg16(0x11, 0x00, 0x2041);	/* Revision level */

	schedule_callback(50);
}

static void ControllerTestEnd(void)
{
	verify(state == MS_ST_CONTROLER);
	state = MS_ST_DSP;

	schedule_callback(50);
}

/* The end of the reset, and the internal tests started */
static void ControllerTestStart(void)
{
	verify(state == MS_RESETING);
	/* the self test's values */
	state = MS_ST_CONTROLER;

	/* 1E:3 -> set */
	reg_set(MR1E_TDBE, 1);

	/*
	RAM1 Checksum  = 0xEA3C or 0x451
	RAM2 Checksum  = 0x5536 or 0x49A5
	ROM1 Checksum  = 0x5F4C
	ROM2 Checksum  = 0x3835 or 0x3836
	Timer/ROM/RAM  = 0x801 or 0xDE00
	Part Number    = 0x3730 or 0x3731
	Revision Level = 0x4241
	*/
	SetReg16(0x1D, 0x1C, 0xEA3C);
	SetReg16(0x1B, 0x1A, 0x5536);
	SetReg16(0x19, 0x18, 0x5F4C);
	SetReg16(0x17, 0x16, 0x3835);
	SetReg16(0x15, 0x14, 0x801);
	SetReg16(0x13, 0x12, 0x3730);
	SetReg16(0x11, 0x0, 0x4241);

	ControllerTestEnd();
}

static void modem_reset(u32 v)
{
	if (v == 0)
	{
		memset(modem_regs, 0, sizeof(modem_regs));
		state = MS_RESET;
		DEBUG_LOG(MODEM, "Modem reset start ...\n");
		/* The line drops as soon as reset is asserted: some games never
		 * release it again */
		line_close();
		line_id = 0;
		dead_steps = 0;
		byte_fifo_clear(&line_fifo);
	}
	else
	{
		if (state == MS_RESET)
		{
			memset(modem_regs, 0, sizeof(modem_regs));
			state = MS_RESETING;
			ControllerTestStart();
			INFO_LOG(MODEM, "MODEM Reset\n");
		}
		modem_regs[0x20] = (u8)v;
	}
}

static void check_start_handshake(void)
{
	if (reg_bit(MR09_DTR)
			&& (reg_bit(MR09_DATA) || reg_bit(MR15_AUTO))
			&& (connect_state == RINGING || connect_state == NEGO_COMPLETE))
	{
		DEBUG_LOG(MODEM, "DTR asserted. starting handshaking\n");
		connect_state = HANDSHAKING;
		schedule_callback(1);
	}
}

static bool word_dspram_write;
static bool module_download;
static u32 reg1b_save;
static u8 download_crc;

/* TBUFFER: a tone, a byte of a download, a dial, or a byte to send */
static void write_tbuffer(u32 data)
{
	if (v8bis && !reg_bit(MR08_RTS))
		/* emit V8 bis tone */
		v8bis_emit_tone(&v8bisProto, (u8)data);
	else if (module_download)
		download_crc = (u8)((download_crc << 1) + ((download_crc & 0x80) >> 7) + (data & 0xFF));
	else if (connect_state == DISCONNECTED || connect_state == DIALING)
	{
		if (connect_state == DISCONNECTED)
		{
			INFO_LOG(MODEM, "MODEM Dialing\n");
			connect_state = DIALING;
			modem_regs[MR16] = 0;		/* SECRXB */
		}
		schedule_callback(100);
	}
	else if (connect_state == CONNECTED && reg_bit(MR08_RTS))
	{
#ifndef NDEBUG
		sent_bytes++;
		if (sent_fp)
			fputc(data, sent_fp);
#endif
		curOutput->write(curOutput->ctx, (u8)data);

		reg_set(MR1E_TDBE, 0);
		txFifoSize += 1.f;
		if (reg_bit(MR04_FIFOEN))
		{
			set_status(MR0D_TXFNF, txFifoSize <= 15.f);
			set_status(MR01_TXHF, txFifoSize >= 8.f);
		}
		else
		{
			set_status(MR0D_TXFNF, 0);
			set_status(MR01_TXHF, 1);
		}
	}
}

/* The DSP's memory, read or written through MEDAL, MEDAM */
static void dspram_access(u32 old)
{
	u32 dspram_addr;
	if (reg_bit(MR1D_MEMW) && !(old & (1 << 5)))
		word_dspram_write = false;
	if (!reg_bit(MR1D_MEACC))
		return;
	reg_set(MR1D_MEACC, 0);
	reg_set(MR1F_NEWS, 1);
	dspram_addr = modem_regs[0x1c] | ((modem_regs[0x1d] & MR1D_MEMADD_H_MASK) << 8);
	if (reg_bit(MR1D_MEMW))
	{
		const u16 v = reg18_19();
		DEBUG_LOG(MODEM, "DSP mem Write%s address %08x = %x\n", word_dspram_write ? " (w)" : "", dspram_addr, v);
		if (word_dspram_write)
		{
			if (dspram_addr & 1)
			{
				/* The address has 12 bits: the second byte of a word at
				 * the last one is the first byte of the memory */
				dspram[dspram_addr] = v & 0xFF;
				dspram[(dspram_addr + 1) & (sizeof(dspram) - 1)] = (v >> 8) & 0xFF;
			}
			else
				memcpy(&dspram[dspram_addr], &v, sizeof(v));
		}
		else
			dspram[dspram_addr] = v & 0xFF;
		if ((dspram_addr == 0x26B || dspram_addr == 0x26c) && (v & 0xFF) != 0)
			dspram[0x26F] = 1; /* Saved Filtered EQM */
	}
	else
	{
		u16 v;
		if (dspram_addr & 1)
			v = (u16)(dspram[dspram_addr] | (dspram[(dspram_addr + 1) & (sizeof(dspram) - 1)] << 8));
		else
			memcpy(&v, &dspram[dspram_addr], sizeof(v));
		set_reg18_19(v);
		DEBUG_LOG(MODEM, "DSP mem Read address %08x == %x\n", dspram_addr, v);
	}
}

static void ModemNormalWrite(u32 reg, u32 data)
{
	u32 old;
#ifndef NDEBUG
	if (recv_fp == NULL)
	{
		recv_fp = fopen("ppp_recv.dump", "w");
		sent_fp = fopen("ppp_sent.dump", "w");
	}
#endif
	old = modem_regs[reg];
	modem_regs[reg] = (u8)((old & ~regs_write_mask[reg]) | (data & regs_write_mask[reg]));

	switch (reg)
	{
	case 0x02:
		reg_set(MR0F_RTSDT, reg_bit(MR02_RTSDE) && connect_state == CONNECTED);
		break;

	case 0x06:
		DEBUG_LOG(MODEM, "PEN = %d\n", reg_bit(MR06_PEN));
		if (reg_bit(MR06_PEN))
			WARN_LOG(MODEM, "Parity not supported\n");
		if (reg_bit(MR06_HDLC))
			WARN_LOG(MODEM, "HDLC mode not supported\n");
		break;

	case 0x08:
		DEBUG_LOG(MODEM, "TPDM = %d ASYN = %d\n", reg_bit(MR08_TPDM), reg_bit(MR08_ASYN));
		if ((old ^ modem_regs[reg]) & 0x80)
			/* ASYN changed */
			configureStreams();
		break;

	case 0x09:
		check_start_handshake();	/* DTR and DATA */
		if (connect_state == CONNECTED)
		{
			if (reg_bit(MR09_DTR))
			{
				DEBUG_LOG(MODEM, "DTR asserted\n");
				set_status(MR0F_DSR, 1);
			}
			else
			{
				DEBUG_LOG(MODEM, "DTR reset\n");
				set_status(MR0F_DSR, 0);
				set_status(MR0F_RLSD, 0);
			}
		}
		break;

	case 0x10:	/* TBUFFER */
		write_tbuffer(data);
		break;

	case 0x11:
		DEBUG_LOG(MODEM, "PARSL = %d\n", MR11_PARSL(modem_regs[0x11]));
		break;

	case 0x14:	/* ABCODE */
		if (data == 0x4f || data == 0x5f)
		{
			reg1b_save = modem_regs[0x1b];
			modem_regs[0x1b] = (u8)data;
			module_download = true;
			download_crc = 0;
		}
		else if (data == 0 && module_download)
		{
			/* If module download is in progress, this signals the end of it */
			modem_regs[MR17] = 0xFF;
			modem_regs[MR16] = download_crc;
			/* Restore reg 1b */
			modem_regs[0x1b] = (u8)reg1b_save;
			module_download = false;
		}
		break;

	case 0x15:
		check_start_handshake();	/* AUTO */
		break;

	/* Data Write Regs, for transfers to DSP */
	case 0x18:	/* MEDAL */
		break;

	case 0x19:	/* MEDAM */
		word_dspram_write = true;
		break;

	case 0x1a:
		if (connect_state == CONNECTED && reg_bit(MR1A_SCIBE))
			WARN_LOG(MODEM, "Unexpected state: connected and SCIBE==1\n");
		break;

	case 0x1C:	/* MEADDL */
		break;

	case 0x1D:	/* MEADDH */
		dspram_access(old);
		break;

	case 0x1F:
		if (!reg_bit(MR1F_NCIE))
			reg_set(MR1F_NCIA, 0);
		if (reg_bit(MR1F_NEWC))
		{
			if (reg_bit(MR1A_SFRES))
			{
				reg_set(MR1A_SFRES, 0);
				DEBUG_LOG(MODEM, "Soft Reset SET && NEWC, executing reset and init\n");
				/* As the reset line pulled and let go, but with NEWC left
				 * set until the self tests are over */
				modem_reset(0);
				modem_reset(1);
				reg_set(MR1F_NEWC, 1);
				modem_regs[0x20] = 0;
			}
			else
			{
				reg_set(MR1F_NEWC, 0);	/* accept the settings */
				if (reg_bit(MR1F_NCIE))
					reg_set(MR1F_NCIA, 1);
				DEBUG_LOG(MODEM, "NEWC CONF=%x\n", modem_regs[MR12]);
			}
			if (modem_regs[MR12] == 0x81 && !v8bis)
			{
				/* V.8 bis detector */
				v8bis = (dspram[0x439] & 1) == 1;
				if (v8bis)
				{
					v8bis_reset(&v8bisProto);
					configureStreams();
				}
			}
		}
		/* Don't allow NEWS to be set if 0 */
		if ((old & (1 << 3)) == 0)
			reg_set(MR1F_NEWS, 0);
		if (!reg_bit(MR1F_NEWS))
			reg_set(MR1F_NSIA, 0);
		break;

	default:
		break;
	}
	update_interrupt();
}

/* A register read in normal operation */
static u32 normal_read(u32 reg)
{
	/* Dial tone is detected if TONEA, TONEB and TONEC are set */
	const int tone = connect_state == DISCONNECTED || connect_state == NEGO_COMPLETE;
	u8 data;
	set_status(MR0F_CTS, reg_bit(MR08_RTS) && connect_state == CONNECTED);
	set_status(MR0B_TONEA, tone);
	set_status(MR0B_TONEB, tone);
	set_status(MR0B_TONEC, tone);
	if (reg_bit(MR04_FIFOEN) || module_download)
	{
		set_status(MR0D_TXFNF, txFifoSize <= 15.f);
		set_status(MR01_TXHF, txFifoSize >= 8.f);
	}
	else
	{
		set_status(MR0D_TXFNF, txFifoSize == 0.f);
		set_status(MR01_TXHF, txFifoSize >= 1.f);
	}
	data = modem_regs[reg];
	if (reg == 0x00)	/* RBUFFER */
	{
#ifndef NDEBUG
		if (connect_state == CONNECTED && recv_fp)
			fputc(data, recv_fp);
#endif
		if (byte_fifo_size(&rxFifo) != 0)
		{
			byte_fifo_pop(&rxFifo);
			updateRxFifoStatus();
			update_interrupt();
		}
	}
	else if (reg == 0x16)	/* SECRXB */
	{
		if (v8bis)
		{
			if (connect_state == CONNECTED)
				data = 0;
			else
				data = v8bis_detect_tone(&v8bisProto);
			modem_regs[MR16] = data;
		}
	}
	return data;
}

u32 ModemReadMem_A0_006(u32 addr, u32 size)
{
	u32 reg = (addr & 0x7FF) >> 2;
	(void)size;
	protocols_setup();

	if (reg < 0x100)
		return MODEM_ID[reg & 1];

	reg -= 0x100;
	if (reg < 0x21)
	{
		switch (state)
		{
		case MS_NORMAL:
			return normal_read(reg);

		case MS_ST_CONTROLER:
		case MS_ST_DSP:
			if (reg == 0x10)
			{
				reg_set(MR1E_TDBE, 0);
				return 0;
			}
			return modem_regs[reg];

		case MS_END_DSP:
			/* Nothing to read, but NEWC: a soft reset is not over, and no
			 * write is taken, until the last self test has ended */
			return reg == 0x1f ? modem_regs[reg] : 0;

		case MS_RESETING:
			return 0; /* still reset */

		default:
			return 0;
		}
	}

	DEBUG_LOG(MODEM, "modem reg %03X read -- wtf is it ?\n", reg);
	return 0;
}

void ModemWriteMem_A0_006(u32 addr, u32 data, u32 size)
{
	u32 reg = (addr & 0x7FF) >> 2;
	(void)size;
	protocols_setup();
	if (reg < 0x100)
	{
		DEBUG_LOG(MODEM, "modem reg %03X write -- MODEM ID?!\n", reg);
		return;
	}

	reg -= 0x100;
	if (reg < 0x20)
	{
		if (state == MS_NORMAL)
			ModemNormalWrite(reg, data);
		else
			DEBUG_LOG(MODEM, "modem reg %03X write %X -- undef state?\n", reg, data);
		return;
	}
	if (reg == 0x20)
	{
		/* Hard reset */
		modem_reset(data);
		return;
	}

	DEBUG_LOG(MODEM, "modem reg %03X write %X -- wtf is it?\n", reg, data);
}

/* The modem in a save state, since version 21 of the format: everything a
 * game can see through the registers, and which line the modem is on (see
 * line_id). The step that is due next is the scheduler's, and has been in
 * the state all along. Only a Dreamcast has a modem. */
void modem_serialize_v21(void **data, unsigned int *total_size)
{
	u8 state8, connect_state8, word_dspram_write8, module_download8, reg1b_save8;
	if (settings.System != DC_PLATFORM_DREAMCAST)
		return;

	state8 = (u8)state;
	connect_state8 = (u8)connect_state;
	word_dspram_write8 = word_dspram_write;
	module_download8 = module_download;
	reg1b_save8 = (u8)reg1b_save;

	LIBRETRO_SA(modem_regs, sizeof(modem_regs));
	LIBRETRO_SA(dspram, sizeof(dspram));
	LIBRETRO_S(state8);
	LIBRETRO_S(connect_state8);
	LIBRETRO_S(word_dspram_write8);
	LIBRETRO_S(module_download8);
	LIBRETRO_S(reg1b_save8);
	LIBRETRO_S(download_crc);
	LIBRETRO_S(dead_steps);
	LIBRETRO_S(line_id);
	LIBRETRO_S(rx_pos);
	LIBRETRO_S(tx_pos);
	LIBRETRO_S(last_dial_time);
	LIBRETRO_S(connected_time);
}

bool modem_unserialize_v21(void **data, unsigned int *total_size)
{
	u8 state8 = 0;
	u8 connect_state8 = 0;
	u8 word_dspram_write8 = 0;
	u8 module_download8 = 0;
	u8 reg1b_save8 = 0;
	if (settings.System != DC_PLATFORM_DREAMCAST)
		return true;

	LIBRETRO_USA(modem_regs, sizeof(modem_regs));
	LIBRETRO_USA(dspram, sizeof(dspram));
	LIBRETRO_US(state8);
	LIBRETRO_US(connect_state8);
	LIBRETRO_US(word_dspram_write8);
	LIBRETRO_US(module_download8);
	LIBRETRO_US(reg1b_save8);
	LIBRETRO_US(download_crc);
	LIBRETRO_US(dead_steps);
	LIBRETRO_US(line_id);
	LIBRETRO_US(rx_pos);
	LIBRETRO_US(tx_pos);
	LIBRETRO_US(last_dial_time);
	LIBRETRO_US(connected_time);

	/* Any byte is a register's or the DSP memory's; the rest is checked */
	state = state8 <= MS_NORMAL ? state8 : MS_INVALID;
	connect_state = connect_state8 <= NEGO_COMPLETE ? connect_state8 : DISCONNECTED;
	word_dspram_write = word_dspram_write8 != 0;
	module_download = module_download8 != 0;
	reg1b_save = reg1b_save8;
	/* Only a modem that is connected is on a line */
	if (state != MS_NORMAL || connect_state != CONNECTED)
		line_id = 0;

	/* The line that is up is not the state's, or the state is from too long
	 * ago on it for the game to be given again what it has taken since */
	if (live_line != 0
			&& (line_id != live_line
				|| rx_taken - rx_pos > LINE_WINDOW
				|| tx_sent - tx_pos > LINE_WINDOW)
			/* (but a line made after the state's time, which its machine
			 * may make again at the same time: see pre_connected()) */
			&& !(line_id == 0 && live_line_made > sh4_sched_now64() && rx_taken <= LINE_WINDOW))
		line_close();

	return true;
}

/* A state from before version 21 has nothing of the modem but its next
 * step: the modem, and the line, stay as they are. */
void modem_state_before_v21(void)
{
}

/* What a state holds of the modem since version 24, in the network devices'
 * part (net_serialize_v24()): what has come over the line and not been
 * used yet, the receive FIFO, how full the transmit FIFO is, the speed of
 * the connection, and the V.42 / V.42 bis and V.8 bis ends of it. */
void modem_serialize_v24(void **data, unsigned int *total_size)
{
	s32 cpb = cyclesPerByte;
	u8 v8 = v8bis;
	protocols_setup();

	byte_fifo_serialize(&line_fifo, data, total_size);
	byte_fifo_serialize(&rxFifo, data, total_size);
	LIBRETRO_S(txFifoSize);
	LIBRETRO_S(cpb);
	LIBRETRO_S(txRxRatio);
	LIBRETRO_S(v8);
	v42_serialize(&v42Proto, data, total_size);
	v8bis_serialize(&v8bisProto, data, total_size);
}

bool modem_unserialize_v24(void **data, unsigned int *total_size)
{
	s32 cpb = DEFAULT_CYCLES_PER_BYTE;
	u8 v8 = 0;
	protocols_setup();

	byte_fifo_unserialize(&line_fifo, data, total_size);
	byte_fifo_unserialize(&rxFifo, data, total_size);
	LIBRETRO_US(txFifoSize);
	LIBRETRO_US(cpb);
	LIBRETRO_US(txRxRatio);
	LIBRETRO_US(v8);
	v42_unserialize(&v42Proto, data, total_size);
	v8bis_unserialize(&v8bisProto, data, total_size);

	/* (the comparisons are false for a NaN) */
	if (!(txFifoSize >= 0.f && txFifoSize <= 64.f))
		txFifoSize = 0.f;
	/* From 56000 to 75 bits a second */
	cyclesPerByte = cpb >= SH4_MAIN_CLOCK * 8 / 56000 && cpb <= SH4_MAIN_CLOCK * 8 / 75 ? cpb : DEFAULT_CYCLES_PER_BYTE;
	if (!(txRxRatio > 0.f && txRxRatio <= 16.f))
		txRxRatio = 1.f;
	v8bis = v8 != 0;
	configureStreams();
	return true;
}

/* An older state: the modem passed bytes straight between the game and the
 * line, one each 238 us, and a byte in the receive buffer is all its FIFO
 * holds. */
void modem_state_before_v24(void)
{
	protocols_setup();
	byte_fifo_clear(&line_fifo);
	byte_fifo_clear(&rxFifo);
	if (reg_bit(MR1E_RDBF))
		byte_fifo_push(&rxFifo, modem_regs[MR00]);
	txFifoSize = 0.f;
	cyclesPerByte = DEFAULT_CYCLES_PER_BYTE;
	txRxRatio = 1.f;
	v8bis = false;
	v8bis_reset(&v8bisProto);
	v42_reset(&v42Proto);
	configureStreams();
}
