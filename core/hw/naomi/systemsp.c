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
 * Sega System SP: a NAOMI main board with a ROM board of its own. The ROM
 * board answers in the G2 extension area (area 0, 0x01000000) with banked
 * windows into its address space - battery-backed SRAM, a CompactFlash
 * slot, a network ("media") board - and its I/O ports, two serial ports
 * and a serial EEPROM. The cartridge is M4 flash, which the BIOS fills from
 * the CompactFlash card for the games that come on one.
 *
 * The machine runs as a NAOMI (settings.System): naomi_cart_systemsp says
 * that it is this one. There is one board at most: its state is static,
 * but for the network board's memory and the CompactFlash card's hunk,
 * which are taken when the game is loaded.
 */
#include <stdlib.h>
#include <string.h>
#include <streams/file_stream.h>
#include <compat/strl.h>
#include <retro_atomic.h>
#include <retro_miscellaneous.h>
#include "types.h"
#include "m4cartridge.h"
#include "dinokich.h"
#include "systemsp_serial.h"
#include "naomi_cart.h"
#include "naomi_periph.h"
#include "hw/flashrom/flashrom.h"
#include "hw/maple/maple_devs.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "imgread/chd_image.h"
#include "savewriter.h"
#include "paths.h"
#include "libretro/pad_state.h"

bool naomi_cart_systemsp;

#define INT_UART1 0x01
#define INT_UART2 0x02
#define INT_DIMM 0x08
#define INT_ATA 0x10
#define PIPE_STATE_SIZE SP_PIPE_STATE_SIZE
#define SECTOR_SIZE 512
#define DIMM_STATUS 0x8102
#define NETMEM_SIZE (8 * 1024 * 1024)
#define NETMEM_PAGE 1024
/* the pages of the network board's memory a save state holds at most */
#define NETMEM_STATE_PAGES 32
#define NETMEM_WORDS (NETMEM_SIZE / NETMEM_PAGE / 64)
#define NET_OK 0x8001
#define NET_ERROR 0x8000
#define U64_ONES (~(u64)0)
#define BIT64(b) ((u64)1 << (b))

/* where a command to the flash has got to */
enum
{
	FLASH_INIT,
	FLASH_AAA_AA_1,
	FLASH_555_55_1,
	FLASH_WRITE_BUF_1,
	FLASH_WRITE_BUF_2,
	FLASH_PROGRAM,
	FLASH_AAA_80,
	FLASH_AAA_AA_2,
	FLASH_555_55_2
};

/* The ROM board's own UARTs, and what is plugged into each (systemsp_serial.h) */
typedef struct sp_port
{
	sp_pipe *pipe;
	int index;
} sp_port;

/* Three-wire serial EEPROM, 1 Kb (64 words of 16 bits) */
typedef struct sp_eeprom
{
	u8 data[128];
	/* written to since it was last saved */
	bool dirty;
	bool cs;
	bool clk;
	bool di;
	bool writeEnable;
	/* the bits of the command so far, the first one highest */
	u32 command;
	u8 commandBits;
	u8 expected;
	u16 dataOut;
	u8 dataOutBits;
} sp_eeprom;

/* A sensor that a motor turns past, or that is on for a while when
 * something passes it. a and b: where it is in its turn and when it was
 * last looked at; or when it was set off (0: it was not). */
typedef struct sp_sensor
{
	u64 a;
	u64 b;
} sp_sensor;

/* The kinds of cabinet, as their I/O ports differ */
enum
{
	IO_DEFAULT,
	IO_CARD_READER,
	IO_ISSHONI,
	IO_BATTLE_RACER,
	IO_HOPPER,
	IO_MANPUKU,
	IO_KINGYO,
	IO_MEDAL
};

typedef struct system_sp
{
	m4_cart m4;
	char gameName[64];
	const char *mediaName;
	const char *parentName;
	chd_image_t *chd;
	u64 chdSize;
	u32 hunkbytes;
	u8 *hunkmem;
	u32 hunknum;
	/* the game's header, when it is known */
	u8 bootId[0x500];
	bool haveBootId;
	int region;
	/* the IC card reader on the first port, if there is one (the port has it) */
	dinokich_reader *dinokich;

	sp_eeprom eeprom;
	retro_atomic_int_t eepromFailed;
	sp_port uart1;
	sp_port uart2;
	/* the cabinet: its kind, the motors and lamps the game has turned on,
	 * as each kind has them, and its sensors */
	int ioKind;
	u8 outputs;
	sp_sensor sensors[4];

	u16 bank;
	u16 subkey1;
	u16 subkey2;
	u16 encIv;
	u8 encCounter;

	struct
	{
		u8 features;
		u16 cylinder;
		u8 sectorCount;
		u8 sectorNumber;
		u8 status;		/* BSY RDY DF DSC DRQ CORR IDX ERR */
		u8 error;
		u8 driveHead;	/* 1 LBA 1 DRV HEAD(4) */
		u8 devCtrl;		/* HOB - - - - SRST nIEN - */
		u8 interruptPending;
		u8 reg84;
		u8 buffer[SECTOR_SIZE];
		u32 bufferIndex;
	} ata;

	struct
	{
		u8 cmdState;
		u32 progAddress;
		u16 wordCount;
		/* how many times the flash has been programmed or erased, and what */
		u32 changes;
		u32 changesHash;
	} flash;
	/* the flash differs from its file */
	bool flashDirty;

	/* the network board's memory (NETMEM_SIZE), and which of its pages
	 * have been written to since it was cleared */
	u8 *netmem;
	u64 netPages[NETMEM_WORDS];
	bool netPagesWarned;
	u32 inetaddr;
	u32 netmask;
	/* the sockets the game has opened, 1 to 63 */
	u64 sockets;

	/* where the board's files are, while they are opened */
	char path[2][PATH_MAX_LENGTH];
} system_sp;

static system_sp sp_board;
/* the board, while there is one */
static system_sp *sp_live;
static int ataSchedId = -1;

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

/* The length of the string at @s, @max at most */
static size_t boundedLength(const char *s, size_t max)
{
	const char *end = (const char *)memchr(s, 0, max);
	return end != NULL ? (size_t)(end - s) : max;
}

static u32 min_u32(u32 a, u32 b) { return a < b ? a : b; }

/* The network board's memory, as the game sees it: in place, natively */
static u8 net_r8(system_sp *sp, u32 addr)
{
	return sp->netmem[addr & (NETMEM_SIZE - 1)];
}

static u16 net_r16(system_sp *sp, u32 addr)
{
	u16 v;
	memcpy(&v, &sp->netmem[addr & (NETMEM_SIZE - 2)], sizeof(v));
	return v;
}

static u32 net_r32(system_sp *sp, u32 addr)
{
	u32 v;
	memcpy(&v, &sp->netmem[addr & (NETMEM_SIZE - 4)], sizeof(v));
	return v;
}

static void net_touch(system_sp *sp, u32 addr)
{
	sp->netPages[addr / NETMEM_PAGE / 64] |= BIT64(addr / NETMEM_PAGE % 64);
}

static void net_w8(system_sp *sp, u32 addr, u8 v)
{
	addr &= NETMEM_SIZE - 1;
	sp->netmem[addr] = v;
	net_touch(sp, addr);
}

static void net_w16(system_sp *sp, u32 addr, u16 v)
{
	addr &= NETMEM_SIZE - 2;
	memcpy(&sp->netmem[addr], &v, sizeof(v));
	net_touch(sp, addr);
}

static void net_w32(system_sp *sp, u32 addr, u32 v)
{
	addr &= NETMEM_SIZE - 4;
	memcpy(&sp->netmem[addr], &v, sizeof(v));
	net_touch(sp, addr);
}

/* The pages of the network board's memory that were written to, cleared */
static void net_clear(system_sp *sp)
{
	u32 i, b;
	for (i = 0; i < NETMEM_WORDS; i++)
		for (b = 0; b < 64 && sp->netPages[i] != 0; b++)
			if (sp->netPages[i] & BIT64(b))
				memset(&sp->netmem[(i * 64 + b) * NETMEM_PAGE], 0, NETMEM_PAGE);
	memset(sp->netPages, 0, sizeof(sp->netPages));
}

static void sp_update_interrupt(system_sp *sp, u32 mask)
{
	sp->ata.interruptPending |= mask;
	if ((sp->ata.interruptPending & (INT_UART1 | INT_UART2 | INT_DIMM))
			|| ((sp->ata.interruptPending & INT_ATA) && !(sp->ata.devCtrl & 2)))
		asic_RaiseInterrupt(holly_EXP_PCI);
	else
		asic_CancelInterrupt(holly_EXP_PCI);
}

/*
 * The serial ports
 */
static void port_flush(sp_port *port)
{
	if (port->pipe != NULL)
		while (port->pipe->ops->available(port->pipe))
			port->pipe->ops->read(port->pipe);
}

static u8 port_read(sp_port *port, u32 addr)
{
	switch ((addr & 0x3f) / 4)
	{
	case 0: /* data in */
		if (port->pipe != NULL)
			return port->pipe->ops->read(port->pipe);
		return 0;
	case 1: /* out buffer len */
		return 0;
	case 2: /* in buffer len */
		if (port->pipe != NULL)
			return (u8)port->pipe->ops->available(port->pipe);
		return 0;
	case 3: /* errors? */
	case 4: /* unknown */
	case 5: /* flow control */
		return 0;
	case 6: /* status. bit 3: receive buffer not empty */
		if (port->pipe != NULL && port->pipe->ops->available(port->pipe) > 0)
			return 8;
		return 0;
	case 7: /* interrupt status/mask? */
	case 8: /* unknown */
	case 9: /* control? */
	case 10: /* baudrate (lsb) */
	case 11: /* baudrate (msb) */
		return 0;
	default:
		INFO_LOG(NAOMI, "Unknown UART%d port %x\n", port->index, addr);
		return 0;
	}
}

static void port_write(sp_port *port, u32 addr, u8 v)
{
	switch ((addr & 0x3f) / 4)
	{
	case 0: /* data out */
		if (port->pipe != NULL)
			port->pipe->ops->write(port->pipe, v);
		else
			INFO_LOG(NAOMI, "UART%d out: %02x %c\n", port->index, v, v);
		break;
	case 1: /* out buffer len */
	case 2: /* in buffer len */
	case 3: /* errors? */
	case 4: /* unknown */
	case 5: /* flow control */
	case 6: /* status */
	case 7: /* interrupt status/mask? */
	case 8: /* unknown */
	case 9: /* control? */
		break;
	case 10: /* baudrate (lsb) */
	case 11: /* baudrate (msb) */
		port_flush(port);
		break;
	default:
		INFO_LOG(NAOMI, "Unknown UART%d port %x\n", port->index, addr);
		break;
	}
}

/* (for a device: the port that has it has something new to send) */
static void port_update_status(void *p)
{
	const sp_port *port = (const sp_port *)p;
	if (sp_live != NULL)
		sp_update_interrupt(sp_live, port->index == 1 ? INT_UART1 : INT_UART2);
}

static void port_close(sp_port *port)
{
	if (port->pipe != NULL)
		port->pipe->ops->close(port->pipe);
	port->pipe = NULL;
}

/*
 * The I/O ports: the cabinet's switches, and what the game drives
 */
static u64 msAsCycles(u64 ms)
{
	return (u64)SH4_MAIN_CLOCK * ms / 1000;
}

/* On and Period in SH4 ms. (Half way round at first.) */
static bool periodic(sp_sensor *s, u64 on, u64 period, bool actuator)
{
	const u64 now = sh4_sched_now64();
	if (s->a == U64_ONES)
		s->a = msAsCycles(period) / 2;
	if (actuator)
		s->a = (s->a + now - s->b) % msAsCycles(period);
	s->b = now;
	return s->a < msAsCycles(on);
}

/* Width in SH4 ms */
static bool pulse(sp_sensor *s, u64 width, bool actuator)
{
	u64 now;
	if (!actuator)
	{
		s->b = 0;
		return false;
	}
	now = sh4_sched_now64();
	if (s->b == 0)
		s->b = now;
	return now - s->b < msAsCycles(width);
}

/* The switches of the first two players, active low, as NAOMI keys, with
 * the game's remapping: a switch of the player's that is another one of
 * his own (target), or the first player's switch that is one of the
 * second's (p2_mask). */
static void getInputState(u32 kc[2])
{
	int i;
	kc[0] = kcode[0];
	kc[1] = kcode[1];
	if (naomi_game_inputs == NULL)
		return;
	for (i = 0; naomi_game_inputs->buttons[i].mask != 0; i++)
	{
		const ButtonDescriptor *bd = &naomi_game_inputs->buttons[i];
		if (bd->target == 0xffffffff)
			continue;
		if (bd->p2_mask != 0)
		{
			if ((kc[0] & bd->mask) == 0)
				kc[1] &= ~bd->p2_mask;
		}
		else if (bd->target != 0)
		{
			if ((kc[0] & bd->mask) == 0)
				kc[0] &= ~bd->target;
			if ((kc[1] & bd->mask) == 0)
				kc[1] &= ~bd->target;
		}
	}
}

static void io_reset(system_sp *sp)
{
	int i;
	sp->outputs = 0;
	for (i = 0; i < 4; i++)
	{
		sp->sensors[i].a = U64_ONES;
		sp->sensors[i].b = 0;
	}
}

static void io_serialize(system_sp *sp, void **data, unsigned int *total_size)
{
	int i;
	LIBRETRO_S(sp->outputs);
	for (i = 0; i < 4; i++)
	{
		LIBRETRO_S(sp->sensors[i].a);
		LIBRETRO_S(sp->sensors[i].b);
	}
}

static void io_unserialize(system_sp *sp, void **data, unsigned int *total_size)
{
	int i;
	LIBRETRO_US(sp->outputs);
	for (i = 0; i < 4; i++)
	{
		LIBRETRO_US(sp->sensors[i].a);
		LIBRETRO_US(sp->sensors[i].b);
	}
}

/* A medal cabinet's outputs: the motors and the solenoid. (Its sensors: 0
 * the hopper, 1 the pusher, 2 the count hopper's rotation.) A hopper
 * cabinet's: bit 0 the hopper (sensors 0, 1 the coin chutes, 2 the
 * hopper's). */
enum
{
	JPMECHA_MTR = 1,
	COUNT_HOPPER_MTR = 2,
	HOPPER_MTR = 4,
	PUSHER_MTR = 8,
	SLOPE_MTR = 0x10,
	JPMECHA_SOLENOID = 0x20
};

/* The buttons as the board has them, upstream's names for them: button
 * 1-6 are BTN0-BTN5, the medal checkers 7 and 8 BTN6 and BTN7. */

/* IN_PORT0 (CN9 17-24) */
static u8 io_cn9_17_24(system_sp *sp)
{
	u8 v = 0xff;
	u32 kc[2];
	switch (sp->ioKind)
	{
	case IO_ISSHONI:
		return 0xff;
	case IO_BATTLE_RACER:
		{
			/* 0: P1 right, 1: P2 right, 2: P1 right, 3: P2 right,
			 * 4: P1 left, 5: P2 left, 6: P1 left, 7: P2 left
			 * (the wheel is the left stick, 0 to 255 with 128 in the
			 * middle: as a 16-bit axis it is turned at -1638 and at
			 * -16384, and the same to the right) */
			const int x0 = ((int)(u8)joyx[0] - 128) * 256;
			const int x1 = ((int)(u8)joyx[1] - 128) * 256;
			if (x0 <= -16384)
				v &= ~0x40;
			if (x0 <= -1638)
				v &= ~0x10;
			if (x0 >= 1638)
				v &= ~0x04;
			if (x0 >= 16384)
				v &= ~0x01;
			if (x1 <= -16384)
				v &= ~0x80;
			if (x1 <= -1638)
				v &= ~0x20;
			if (x1 >= 1638)
				v &= ~0x08;
			if (x1 >= 16384)
				v &= ~0x02;
			return v;
		}
	case IO_MANPUKU:
		/* 4: Left, 5: Middle, 6: Right */
		getInputState(kc);
		if (!(kc[0] & NAOMI_LEFT_KEY))
			v &= ~0x10;
		if (!(kc[0] & NAOMI_DOWN_KEY))
			v &= ~0x20;
		if (!(kc[0] & NAOMI_RIGHT_KEY))
			v &= ~0x40;
		return v;
	case IO_KINGYO:
		/* 0: Left, 1: Right, 2: Down, 3: Up, 4: Button 1 */
		getInputState(kc);
		if (!(kc[0] & NAOMI_LEFT_KEY))
			v &= ~0x01;
		if (!(kc[0] & NAOMI_RIGHT_KEY))
			v &= ~0x02;
		if (!(kc[0] & NAOMI_DOWN_KEY))
			v &= ~0x04;
		if (!(kc[0] & NAOMI_UP_KEY))
			v &= ~0x08;
		if (!(kc[0] & NAOMI_BTN0_KEY))
			v &= ~0x10;
		return v;
	case IO_MEDAL:
		/* 0: slope sensor up (active high)
		 * 1: slope sensor low (active high)
		 * 2: count hopper rot. (active high)
		 * 3: count hopper sensor (active high)
		 * 4: hopper sensor
		 * 5: pusher sensor (active high)
		 * 6: tilt bob sensor
		 * 7: checker 9 (active high) */
		v = 0x50;
		getInputState(kc);
		if (periodic(&sp->sensors[2], 100, 500, (sp->outputs & COUNT_HOPPER_MTR) != 0))
			v |= 0x04;	/* count hopper rot sensor */
		/* FIXME generates FIELD P/O TOO HIGH errors. Activate the sensor 50% of the time? */
		if (periodic(&sp->sensors[0], 50, 250, (sp->outputs & HOPPER_MTR) != 0))
			v &= ~0x10; /* hopper sensor */
		if (sp->outputs & SLOPE_MTR)
			v |= 2;	/* slope sensor low */
		else
			v |= 1;	/* slope sensor up */
		if (periodic(&sp->sensors[1], 100, 4300, (sp->outputs & PUSHER_MTR) != 0))
			v |= 0x20;	/* pusher sensor */
		if (!(kc[0] & NAOMI_START_KEY))
			v |= 0x80; /* checker 9 */
		return v;
	default:
		/* 0: P1 start, 1: P2 start, 2: P1 right, 3: P2 right,
		 * 4: P1 left, 5: P2 left, 6: P1 up, 7: P2 up */
		getInputState(kc);
		if (!(kc[0] & NAOMI_START_KEY))
			v &= ~0x01;
		if (!(kc[1] & NAOMI_START_KEY))
			v &= ~0x02;
		if (!(kc[0] & NAOMI_RIGHT_KEY))
			v &= ~0x04;
		if (!(kc[1] & NAOMI_RIGHT_KEY))
			v &= ~0x08;
		if (!(kc[0] & NAOMI_LEFT_KEY))
			v &= ~0x10;
		if (!(kc[1] & NAOMI_LEFT_KEY))
			v &= ~0x20;
		if (!(kc[0] & NAOMI_UP_KEY))
			v &= ~0x40;
		if (!(kc[1] & NAOMI_UP_KEY))
			v &= ~0x80;
		return v;
	}
}

/* IN_PORT3 / IO-0 (CN9 25-32) */
static u8 io_cn9_25_32(system_sp *sp)
{
	u8 v = 0xff;
	u32 kc[2];
	switch (sp->ioKind)
	{
	case IO_ISSHONI:
		return 0xff;
	case IO_MEDAL:
		/* 0-7: checker 1-8 (active high) */
		v = 0;
		getInputState(kc);
		if (!(kc[0] & NAOMI_BTN0_KEY))
			v |= 0x01;
		if (!(kc[0] & NAOMI_BTN1_KEY))
			v |= 0x02;
		if (!(kc[0] & NAOMI_BTN2_KEY))
			v |= 0x04;
		if (!(kc[0] & NAOMI_BTN3_KEY))
			v |= 0x08;
		if (!(kc[0] & NAOMI_BTN4_KEY))
			v |= 0x10;
		if (!(kc[0] & NAOMI_BTN5_KEY))
			v |= 0x20;
		if (!(kc[0] & NAOMI_BTN6_KEY))
			v |= 0x40;
		if (!(kc[0] & NAOMI_BTN7_KEY))
			v |= 0x80;
		return v;
	default:
		/* 0: P1 down, 1: P2 down, 2: P1 button 1, 3: P2 button 1,
		 * 4: P1 button 2, 5: P2 button 2, 6: P1 button 3, 7: P2 button 3 */
		getInputState(kc);
		if (!(kc[0] & NAOMI_DOWN_KEY))
			v &= ~0x01;
		if (!(kc[1] & NAOMI_DOWN_KEY))
			v &= ~0x02;
		if (!(kc[0] & NAOMI_BTN0_KEY))
			v &= ~0x04;
		if (!(kc[1] & NAOMI_BTN0_KEY))
			v &= ~0x08;
		if (!(kc[0] & NAOMI_BTN1_KEY))
			v &= ~0x10;
		if (!(kc[1] & NAOMI_BTN1_KEY))
			v &= ~0x20;
		if (!(kc[0] & NAOMI_BTN2_KEY))
			v &= ~0x40;
		if (!(kc[1] & NAOMI_BTN2_KEY))
			v &= ~0x80;
		return v;
	}
}

/* IO-1 (CN9 33-40) */
static u8 io_cn9_33_40(system_sp *sp)
{
	switch (sp->ioKind)
	{
	case IO_CARD_READER:
	case IO_ISSHONI:
		/* dinosaur king, love & berry:
		 * 0: P1 card set (not used)
		 * 2: CD1 input ok (active low)
		 * 4: CD1 card jam (active low)
		 * 6: CD1 empty (active low)
		 * (The card is never inserted: there is no input for it here,
		 * nor in upstream's libretro core.) */
		return 0xfb;
	case IO_BATTLE_RACER:
		{
			/* 0: P1 button
			 * 1: P2 button
			 * 2: CD1 input ok (active low)
			 * those aren't confirmed: assuming the same as dinoking and love & berry
			 * 4: CD1 card jam (active low)
			 * 6: CD1 empty (active low) */
			u8 v = 0xfb;
			u32 kc[2];
			getInputState(kc);
			if (!(kc[0] & NAOMI_BTN0_KEY))
				v &= ~0x01;
			if (!(kc[1] & NAOMI_BTN0_KEY))
				v &= ~0x02;
			return v;
		}
	default:
		return 0xff;
	}
}

/* IN_PORT1 (CN9 41-48) */
static u8 io_cn9_41_48(system_sp *sp)
{
	u8 v;
	u32 kc[2];
	getInputState(kc);
	switch (sp->ioKind)
	{
	case IO_HOPPER:
	case IO_MANPUKU:
	case IO_KINGYO:
		{
			/* 0: P1 service
			 * 2: P1 test
			 * 4: P1 coin
			 * 5: P2 coin
			 * 6: must be 0 (hopper: not connected / mount error)
			 * 7: must be 1 (hopper: coin2 chute jammed) */
			const bool hopperActive = (sp->outputs & 1) != 0;
			v = 0xbf;
			if (!(kc[0] & NAOMI_SERVICE_KEY))
				v &= ~0x01;
			if (!(kc[0] & NAOMI_TEST_KEY))
				v &= ~0x04;
			if (pulse(&sp->sensors[0], 100, !(kc[0] & NAOMI_COIN_KEY)))
				v &= ~0x10;
			if (pulse(&sp->sensors[1], 100, !(kc[1] & NAOMI_COIN_KEY)))
				v &= ~0x20;
			if (periodic(&sp->sensors[2], 10, 50, hopperActive) && hopperActive)
				v |= 0x40;
			return v;
		}
	case IO_MEDAL:
		/* 0: service/reset sw
		 * 1: left sw
		 * 2: test sw
		 * 3: center sw
		 * 4: door sensor up (active high)
		 * 5: right sw
		 * 6: door sensor left (active high)
		 * 7: tilt sensor br (active high) */
		v = 0x2f;
		if (!(kc[0] & NAOMI_SERVICE_KEY))	/* service/reset */
			v &= ~0x01;
		if (!(kc[0] & NAOMI_TEST_KEY))		/* test */
			v &= ~0x04;
		if (!(kc[0] & NAOMI_LEFT_KEY))		/* left sw */
			v &= ~0x02;
		if (!(kc[0] & NAOMI_DOWN_KEY))		/* center sw */
			v &= ~0x08;
		if (!(kc[0] & NAOMI_RIGHT_KEY))		/* right sw */
			v &= ~0x20;
		return v;
	default:
		/* 0: P1 service, 2: P1 test, 4: P1 coin, 5: P2 coin */
		v = 0xff;
		if (!(kc[0] & NAOMI_SERVICE_KEY))
			v &= ~0x01;
		if (!(kc[0] & NAOMI_TEST_KEY))
			v &= ~0x04;
		if (!(kc[0] & NAOMI_COIN_KEY))
			v &= ~0x10;
		if (!(kc[1] & NAOMI_COIN_KEY))
			v &= ~0x20;
		return v;
	}
}

/* IN_PORT4 / OUT-0 (CN9 49-56) */
static u8 io_cn9_49_56(system_sp *sp)
{
	/* FIXME these are outputs??
	 * 0: P1 coin meter
	 * 1: P2 coin meter */
	u8 v = 0;
	u32 kc[2];
	(void)sp;
	getInputState(kc);
	if (!(kc[0] & NAOMI_COIN_KEY))
		v |= 1;
	if (!(kc[1] & NAOMI_COIN_KEY))
		v |= 2;
	return v;
}

/* IN G-PORT (CN10 9-16) */
static u8 io_cn10_9_16(system_sp *sp)
{
	u8 v = 0;
	u32 kc[2];
	u32 left, right;
	if (sp->ioKind != IO_MEDAL)
		/* dinosaur king, love & berry:
		 * 0: 232c sel status 1 (not used)
		 * 1: 232c sel status 2 (not used)
		 * 2: card status1 (not used)
		 * 3: card status2 (not used)
		 * 4: card status3 (not used) */
		return 0;
	/* 0: jp mecha sensor u (active high)
	 * 2: jp mecha sensor d (active high)
	 * 3: co. full sensor (active high)
	 * 4: coin in L (active high)
	 * 5: coin in R (active high)
	 * 7: jp solenoid sensor (active high) */
	getInputState(kc);
	if (sp->outputs & JPMECHA_MTR)
		v |= 4;
	else
		v |= 1;
	/* FIXME not enough coin ins if multiple checkers pressed at once -> COIN IN RATIO TOO HIGH */
	left = NAOMI_BTN0_KEY | NAOMI_BTN1_KEY | NAOMI_BTN2_KEY | NAOMI_BTN3_KEY | NAOMI_BTN4_KEY | NAOMI_BTN5_KEY;
	right = NAOMI_BTN6_KEY | NAOMI_BTN7_KEY | NAOMI_START_KEY;
	if ((kc[0] & left) != left)
		v |= 0x10;	/* coin in L */
	if ((kc[0] & right) != right)
		v |= 0x20;	/* coin in R */
	if (sp->outputs & JPMECHA_SOLENOID)
		v |= 0x80;
	return v;
}

/* IO-1 (CN9 33-40) */
static void io_set_cn9_33_40(system_sp *sp, u8 v)
{
	if (sp->ioKind != IO_MEDAL)
		return;
	/* 3: jp mecha motor
	 * 4: count hopper motor
	 * 5: hopper motor
	 * 6: pusher motor
	 * 7: slope motor
	 * (all active low) */
	sp->outputs &= JPMECHA_SOLENOID;
	if (!(v & 0x08))
		sp->outputs |= JPMECHA_MTR;
	if (!(v & 0x10))
		sp->outputs |= COUNT_HOPPER_MTR;
	if (!(v & 0x20))
		sp->outputs |= HOPPER_MTR;
	if (!(v & 0x40))
		sp->outputs |= PUSHER_MTR;
	if (!(v & 0x80))
		sp->outputs |= SLOPE_MTR;
}

/* OUT-0 (CN9 49-56) */
static void io_set_cn9_49_56(system_sp *sp, u8 v)
{
	switch (sp->ioKind)
	{
	case IO_HOPPER:
	case IO_MANPUKU:
	case IO_KINGYO:
		/* 4: hopper
		 * 7: ? */
		sp->outputs = (v & 0x10) != 0;
		break;
	default:
		/* (a medal cabinet's: 4: sw lamp L, 5: sw lamp R, 6: patrol
		 * lamp, 7: jackpot lamp) */
		break;
	}
}

/* OUT-1 (CN10 17-24) */
static void io_set_cn10_17_24(system_sp *sp, u8 v)
{
	if (sp->ioKind != IO_MEDAL)
		return;
	/* 0: sw.lamp c
	 * 1: jp solenoid
	 * 6: side lamp L
	 * 7: side lamp R */
	if (v & 2)
		sp->outputs |= JPMECHA_SOLENOID;
	else
		sp->outputs &= ~JPMECHA_SOLENOID;
}

/* Notes on the medal cabinets:
 * COUNT HOPPER JAM - LOCK SENSOR ON 1 SEC
 * -> fixed by timing countHopperRot sensor to 0.5 s max
 * HOPPER EMPTY/JAM - MOTOR DRIVE BUT SENSOR OFF
 * -> fixed by resetting hopper sensor when hopper motor is on.
 * SLOPE SENSOR TIMEOUT - MOTOR DRIVE BUT SENSOR OFF
 * -> set slope sensor low when slope motor on
 * ILLEGAL CHACKER IN - MANY CHACKER IN BUT NO COIN IN
 * -> fixed by setting COIN L or R when checker on
 * COIN IN RATIO TOO HIGH - CTRL COIN IN RATIO OVER 105%
 * -> not enough coin L/R vs. checker in? */

/*
 * The serial EEPROM
 */
static u8 eeprom_address(const sp_eeprom *e)
{
	return (e->command >> (e->commandBits - 9)) & 0x3f;
}

/* DO, or READY when no data is going out */
static bool eeprom_read_do(const sp_eeprom *e)
{
	if (e->dataOutBits > 0)
		return (e->dataOut >> (e->dataOutBits - 1)) & 1;
	return true;
}

static void eeprom_write_clk(sp_eeprom *e, bool state)
{
	/* CS asserted and CLK rising */
	if (!e->clk && state && e->cs)
	{
		if (e->dataOutBits > 0)
			e->dataOutBits--;
		if (e->dataOutBits == 0)
		{
			if (e->commandBits == 0 && !e->di)
			{
				/* the start bit must be 1 */
			}
			else
			{
				e->command = (e->command << 1) | (e->di ? 1 : 0);
				e->commandBits++;
				if (e->commandBits == 9)
				{
					switch ((e->command >> 6) & 3)
					{
					case 0: /* write enable/disable, erase all, write all */
						switch ((e->command >> 4) & 3)
						{
						case 0: /* disable write */
							e->writeEnable = false;
							e->commandBits = 0;
							e->command = 0;
							break;
						case 1: /* write all */
							e->expected = 3 + 6 + 16; /* 6 bits of address, 16 bits of data */
							break;
						case 2: /* erase all */
							if (e->writeEnable)
							{
								memset(e->data, 0xff, sizeof(e->data));
								e->dirty = true;
							}
							e->commandBits = 0;
							e->command = 0;
							break;
						case 3: /* enable write */
							e->writeEnable = true;
							e->commandBits = 0;
							e->command = 0;
							break;
						}
						break;
					case 1: /* write */
						e->expected = 3 + 6 + 16; /* 6 bits of address, 16 bits of data */
						break;
					case 2: /* read */
						e->dataOut = (u16)(e->data[eeprom_address(e) * 2] | (e->data[eeprom_address(e) * 2 + 1] << 8));
						e->dataOutBits = 17; /* actually 16 but 0 means no data */
						e->commandBits = 0;
						e->command = 0;
						break;
					case 3: /* erase */
						if (e->writeEnable)
						{
							e->data[eeprom_address(e) * 2] = 0xff;
							e->data[eeprom_address(e) * 2 + 1] = 0xff;
							e->dirty = true;
						}
						e->commandBits = 0;
						e->command = 0;
						break;
					}
				}
				else if (e->expected > 0 && e->commandBits == e->expected)
				{
					const u16 v = (u16)(e->command & 0xffff);
					u32 i;
					switch ((e->command >> (e->commandBits - 3)) & 3)
					{
					case 0: /* write all */
						if (e->writeEnable)
						{
							for (i = 0; i < sizeof(e->data); i += 2)
							{
								e->data[i] = (u8)v;
								e->data[i + 1] = v >> 8;
							}
							e->dirty = true;
						}
						break;
					case 1: /* write */
						if (e->writeEnable)
						{
							e->data[eeprom_address(e) * 2] = (u8)v;
							e->data[eeprom_address(e) * 2 + 1] = v >> 8;
							e->dirty = true;
						}
						break;
					}
					e->commandBits = 0;
					e->command = 0;
					e->expected = 0;
				}
			}
		}
	}
	e->clk = state;
}

/* power-on: the contents stay */
static void eeprom_reset(sp_eeprom *e)
{
	e->cs = false;
	e->clk = false;
	e->di = false;
	e->command = 0;
	e->commandBits = 0;
	e->expected = 0;
	e->writeEnable = false;
	e->dataOut = 0;
	e->dataOutBits = 0;
}

static void eeprom_serialize(sp_eeprom *e, void **data, unsigned int *total_size)
{
	u8 pins = (e->cs ? 1 : 0) | (e->clk ? 2 : 0) | (e->di ? 4 : 0) | (e->writeEnable ? 8 : 0);
	LIBRETRO_SA(e->data, sizeof(e->data));
	LIBRETRO_S(pins);
	LIBRETRO_S(e->command);
	LIBRETRO_S(e->commandBits);
	LIBRETRO_S(e->expected);
	LIBRETRO_S(e->dataOut);
	LIBRETRO_S(e->dataOutBits);
}

static void eeprom_unserialize(sp_eeprom *e, void **data, unsigned int *total_size)
{
	u8 pins = 0;
	LIBRETRO_USA(e->data, sizeof(e->data));
	LIBRETRO_US(pins);
	LIBRETRO_US(e->command);
	LIBRETRO_US(e->commandBits);
	LIBRETRO_US(e->expected);
	LIBRETRO_US(e->dataOut);
	LIBRETRO_US(e->dataOutBits);
	e->cs = (pins & 1) != 0;
	e->clk = (pins & 2) != 0;
	e->di = (pins & 4) != 0;
	e->writeEnable = (pins & 8) != 0;
	if (e->expected != 0 && e->expected != 3 + 6 + 16)
		e->expected = 0;
	if (e->commandBits >= 3 + 6 + 16 || (e->expected == 0 && e->commandBits >= 9))
	{
		e->commandBits = 0;
		e->command = 0;
	}
	if (e->dataOutBits > 17)
		e->dataOutBits = 0;
}

/*
 * The cartridge's cipher, as the M4 board has it (m4cartridge.c): a round
 * of it for every 16-bit word, once for the program's whole life.
 */
static const u8 k_sboxes[4][16] = {
	{9,8,2,11,1,14,5,15,12,6,0,3,7,13,10,4},
	{2,10,0,15,14,1,11,3,7,12,13,8,4,9,5,6},
	{4,11,3,8,7,2,15,13,1,5,14,9,6,12,0,10},
	{1,13,8,2,0,5,6,14,4,11,15,10,12,3,7,9}
};
static u16 spOneRound[0x10000];
static bool spOneRoundDone;

static void encInit(void)
{
	int round_input;
	if (spOneRoundDone)
		return;
	for (round_input = 0; round_input < 0x10000; round_input++)
	{
		u8 input_nibble[4];
		u8 output_nibble[4];
		u8 aux_nibble;
		u16 result = 0;
		int nibble_idx, i;

		for (nibble_idx = 0; nibble_idx < 4; ++nibble_idx)
		{
			input_nibble[nibble_idx] = (round_input >> (nibble_idx * 4)) & 0xf;
			output_nibble[nibble_idx] = 0;
		}

		aux_nibble = input_nibble[3];
		for (nibble_idx = 0; nibble_idx < 4; ++nibble_idx)
		{
			/* 4 s-boxes per round */
			aux_nibble ^= k_sboxes[nibble_idx][input_nibble[nibble_idx]];
			for (i = 0; i < 4; ++i)  /* diffusion of the bits */
				output_nibble[(nibble_idx - i) & 3] |= aux_nibble & (1 << i);
		}

		for (nibble_idx = 0; nibble_idx < 4; ++nibble_idx)
			result |= (output_nibble[nibble_idx] << (4 * nibble_idx));

		spOneRound[round_input] = result;
	}
	spOneRoundDone = true;
}

static void encReset(system_sp *sp)
{
	sp->encIv = 0;
	sp->encCounter = 0;
}

static u16 sp_decrypt(system_sp *sp, u16 w)
{
	u16 dec = sp->encIv;
	sp->encIv = spOneRound[(u16)(w ^ sp->encIv) ^ sp->subkey1] ^ sp->subkey1;
	dec ^= spOneRound[sp->encIv ^ sp->subkey2] ^ sp->subkey2;
	if (++sp->encCounter == 16)
	{
		sp->encCounter = 0;
		sp->encIv = 0;
	}
	return dec;
}

/*
 * The CompactFlash card
 */

/* @len bytes of the CompactFlash card from @offset on: zeroes for what is
 * past its end, or cannot be read. */
static bool readCf(system_sp *sp, u64 offset, u32 len, u8 *dst)
{
	bool ok = true;
	while (len > 0)
	{
		u32 hunk, at, n;
		if (sp->chd == NULL || offset >= sp->chdSize)
		{
			memset(dst, 0, len);
			return false;
		}
		hunk = (u32)(offset / sp->hunkbytes);
		at = (u32)(offset % sp->hunkbytes);
		n = min_u32(len, sp->hunkbytes - at);
		if (hunk != sp->hunknum)
		{
			if (chd_image_read_hunk(sp->chd, hunk, sp->hunkmem))
				sp->hunknum = hunk;
			else
			{
				WARN_LOG(NAOMI, "System SP: CompactFlash hunk %u cannot be read\n", hunk);
				sp->hunknum = ~0u;
				memset(dst, 0, n);
				ok = false;
				dst += n;
				offset += n;
				len -= n;
				continue;
			}
		}
		memcpy(dst, &sp->hunkmem[at], n);
		dst += n;
		offset += n;
		len -= n;
	}
	return ok;
}

static void readSectors(system_sp *sp)
{
	/* (an address in cylinders, heads and sectors is not one the BIOS
	 * gives: it is taken as a block number all the same) */
	const u32 lba = ((sp->ata.driveHead & 0xf) << 24) | (sp->ata.cylinder << 8) | sp->ata.sectorNumber;
	readCf(sp, (u64)lba * SECTOR_SIZE, SECTOR_SIZE, sp->ata.buffer);
	sp->ata.bufferIndex = 0;
}

static void ataReady(system_sp *sp)
{
	sp->ata.status |= 0x40;		/* RDY */
	sp->ata.status &= ~0x80;	/* BSY */
	sp_update_interrupt(sp, INT_ATA);
}

static int ataSchedCallback(int tag, int cycles, int jitter)
{
	(void)tag;
	(void)cycles;
	(void)jitter;
	if (sp_live != NULL)
		ataReady(sp_live);
	return 0;
}

static void sp_save_eeprom(system_sp *sp);
static bool sp_read(cartridge *c, u32 offset, u32 size, void *dst);

/* A byte, or a word, as the bus gives it */
static u32 sz_mask(u32 sz)
{
	return sz == 1 ? 0xff : sz == 2 ? 0xffff : 0xffffffff;
}

/* (to be masked to @sz bytes) */
static u32 read_area0(system_sp *sp, u32 addr, u32 sz)
{
	addr &= 0x1fffff;
	if (addr < 0x10000)
	{
		/* banked access to ROM/NET board address space, mainly backup SRAM and ATA */
		const u32 offset = (addr & 0xffff) | ((sp->bank & 0x3fff) << 16);
		if ((sp->bank & 0x3f00) == 0x3900)
		{
			/* SRAM: an 8-bit device on a 16-bit bus */
			if (sz == 1)
			{
				if (offset & 1)
					return 0xff;
				return memchip_read(&sys_nvmem_sram, offset / 2, 1);
			}
			else if (sz == 2)
				return 0xff00 | memchip_read(&sys_nvmem_sram, offset / 2, 1);
			return 0xff00ff00 | memchip_read(&sys_nvmem_sram, offset / 2, 1) | (memchip_read(&sys_nvmem_sram, offset / 2 + 1, 1) << 16);
		}
		else if ((sp->bank & 0x3f00) == 0x3a00)
		{
			/* CF IDE registers */
			switch (addr & 0xffff)
			{
			case 0x00: /* RD data */
				if (sz == 2)
				{
					u16 ret;
					addr &= ~1;
					ret = (u16)read_area0(sp, addr, 1);
					ret |= (u16)(read_area0(sp, addr, 1) << 8);
					if (sp->bank & 0x4000)
						/* decrypt */
						ret = sp_decrypt(sp, ret);
					return ret;
				}
				else
				{
					u8 v = 0;
					if (sp->ata.bufferIndex < SECTOR_SIZE)
					{
						v = sp->ata.buffer[sp->ata.bufferIndex++];
						if (sp->ata.bufferIndex == SECTOR_SIZE)
						{
							if (sp->ata.sectorCount > 1)
							{
								/* read next sector */
								sp->ata.sectorCount--;
								sp->ata.sectorNumber++;
								/* (the carry goes on to the head only from a cylinder that went round) */
								if (sp->ata.sectorNumber == 0)
								{
									sp->ata.cylinder++;
									if (sp->ata.cylinder == 0)
										sp->ata.driveHead = (sp->ata.driveHead & 0xf0) | ((sp->ata.driveHead + 1) & 0xf);
								}
								readSectors(sp);
								sp_update_interrupt(sp, INT_ATA);
							}
							else
								/* no more data */
								sp->ata.status &= ~0x08;	/* DRQ */
						}
					}
					return v;
				}
			case 0x04: /* error */
				return 0;
			case 0x08: /* sector count */
				return sp->ata.sectorCount;
			case 0x0c: /* sector no */
				return sp->ata.sectorNumber;
			case 0x10: /* cylinder (lsb) */
				return sp->ata.cylinder & 0xff;
			case 0x14: /* cylinder (msb) */
				return sp->ata.cylinder >> 8;
			case 0x18: /* select card/head */
				return sp->ata.driveHead;
			case 0x1c: /* status */
				{
					/* BUSY RDY  DWF  DSC  DRQ  CORR   0  ERR */
					const u8 status = sp->ata.status;
					/* TODO correct? */
					sp->ata.status &= ~0x10;	/* DSC */
					return status;
				}
			default:
				INFO_LOG(NAOMI, "systemsp::read(%x) CF IDE unknown reg\n", addr);
				return 0xffffffff;
			}
		}
		else if ((sp->bank & 0x3f00) == 0x3b00)
		{
			/* CF IDE AltStatus/Device Ctrl register */
			if ((addr & 0xffff) == 0x18)
				return sp->ata.status;
			INFO_LOG(NAOMI, "systemsp::read(%x) CF IDE AltStatus unknown addr\n", addr);
			return 0;
		}
		else if ((sp->bank & 0x3f00) == 0x3d00)
		{
			/* Network aka Media board shared buffer/RAM */
			DEBUG_LOG(NAOMI, "systemsp::read(%x) Network shared RAM. offset %x\n", addr, offset);
			return 0xffffffff;
		}
		else if ((sp->bank & 0x3f00) == 0x3f00)
			/* Network board present flag (0x01) */
			return 0;
		{
			u8 b[4];
			u16 w;
			u32 l;
			memset(b, 0xff, sizeof(b));
			sp_read(&sp->m4.base, offset, sz, b);
			if (sz == 1)
				return b[0];
			if (sz == 2)
			{
				memcpy(&w, b, sizeof(w));
				return w;
			}
			memcpy(&l, b, sizeof(l));
			return l;
		}
	}
	else if (addr == 0x10000)
		/* bank register */
		return sp->bank;
	else if (addr < 0x10100)
	{
		/* IRQ pending/reset, ATA control */
		switch (addr - 0x10000)
		{
		case 0x30:
			return 0;
		case 0x80:
			{
				/* interrupt status
				 * b0: UART1
				 * b1: UART2
				 * b3: DIMM
				 * b4: ATA controller */
				const u8 intPending = sp->ata.interruptPending;
				sp->ata.interruptPending = 0;
				sp_update_interrupt(sp, 0);
				return intPending;
			}
		case 0x84:
			/* interrupt mask?
			 * 10084: (dinoking) bit0,1 reset, sometimes set */
			return sp->ata.reg84;
		default:
			return 0;
		}
	}
	else if (addr < 0x10128)
	{
		/* I/O chip for inputs */
		switch (addr - 0x10100)
		{
		case 0x0: /* IN_PORT0 (CN9 17-24) */
			/* (the "Insert Card" button of a game with a barcode reader:
			 * there is no I/O board on the maple bus to read it with the
			 * others) */
			naomi_periph_poll();
			return io_cn9_17_24(sp);
		case 0x4: /* IN_PORT1 (CN9 41-48) */
			return io_cn9_41_48(sp);
		case 0x8: /* IN_PORT3 (CN9 25-32) */
			return io_cn9_25_32(sp);
		case 0xc: /* IN CN9 33-40 */
			return io_cn9_33_40(sp);
		case 0x10: /* IN_PORT4 (CN9 49-56) */
			return io_cn9_49_56(sp);
		case 0x18: /* IN_PORT2 (DIP switches and jumpers, and P1 service for older pcb rev) */
			/* DIP switches
			 * 0: unknown, active low
			 * 1: unknown, active low
			 * 2: monitor (1: 31 kHz, 0: 15 kHz)
			 * 3: unknown, must be on (active low)
			 * 4: JP5 (system service)
			 * 5: JP6 (system test)
			 * 6: JP7
			 * 7: JP8 */
			return 0xf7;
		case 0x20: /* IN G_PORT CN10 9-16 */
			return io_cn10_9_16(sp);
		case 0x24: /* bios, write too */
		default:
			return 0;
		}
	}
	else if (addr == 0x10128)
		/* eeprom */
		return eeprom_read_do(&sp->eeprom) << 4;
	else if (addr == 0x10150)
		/* CFG rom board debug flags
		 * bit 0 - romboard type, 1 = M4
		 * bit 1 - debug mode (enable easter eggs in BIOS, can boot game
		 * without proper eeproms/settings) */
		return 3; /* M4 board type, debug on */
	else if (addr < 0x10180)
	{
		/* unknown */
	}
	else if (addr < 0x101c0)
		/* custom UART 1 */
		return port_read(&sp->uart1, addr);
	else if (addr < 0x101f0)
		/* custom UART 2 */
		return port_read(&sp->uart2, addr);
	INFO_LOG(NAOMI, "systemsp::readMemArea0<%d>: Unknown addr %x\n", (int)sz, addr);
	return 0xffffffff;
}

/* (@v masked to @sz bytes) */
static void write_area0(system_sp *sp, u32 addr, u32 v, u32 sz)
{
	addr &= 0x1fffff;
	if (addr < 0x10000)
	{
		/* banked access to ROM/NET board address space, mainly backup SRAM and ATA */
		const u32 offset = (addr & 0xffff) | ((sp->bank & 0x3fff) << 16);
		if ((sp->bank & 0x3f00) == 0x3900)
		{
			/* SRAM: an 8-bit device on a 16-bit bus */
			if (sz == 1 && (offset & 1))
				return;
			if (sz == 4)
				memchip_write(&sys_nvmem_sram, offset / 2 + 1, (u8)(v >> 16), 1);
			memchip_write(&sys_nvmem_sram, offset / 2, (u8)v, 1);
			return;
		}
		else if ((sp->bank & 0x3f00) == 0x3a00)
		{
			/* CF IDE registers */
			switch (addr & 0xffff)
			{
			case 0x00: /* WR data */
				break;
			case 0x04: /* features */
				sp->ata.features = (u8)v;
				break;
			case 0x08: /* sector count */
				sp->ata.sectorCount = (u8)v;
				break;
			case 0x0c: /* sector no */
				sp->ata.sectorNumber = (u8)v;
				break;
			case 0x10: /* cylinder (lsb) */
				sp->ata.cylinder = (u16)((sp->ata.cylinder & 0xff00) | (v & 0xff));
				break;
			case 0x14: /* cylinder (msb) */
				sp->ata.cylinder = (u16)((sp->ata.cylinder & 0xff) | ((v & 0xff) << 8));
				break;
			case 0x18: /* select card/head */
				sp->ata.driveHead = (u8)v | 0xa0;
				break;
			case 0x1c: /* command */
				switch ((u8)v)
				{
				case 0x20:
					/* read sectors */
					sp->ata.status &= ~0x40;	/* RDY */
					sp->ata.status |= 0x80 | 0x08;	/* BSY, DRQ (FIXME should be done in the callback) */
					sh4_sched_request(ataSchedId, 2000); /* 10 us */
					/* (what the decrypting window gives starts again with each command) */
					encReset(sp);
					readSectors(sp);
					break;
				case 0xe1:
					/* idle immediate */
					sp->ata.status |= 0x80;		/* BSY */
					sp->ata.status &= ~0x40;	/* RDY */
					sh4_sched_request(ataSchedId, 2000); /* 10 us */
					break;
				default:
					INFO_LOG(NAOMI, "systemsp::write(%x) CF ATA command unknown: %x\n", addr, v);
					break;
				}
				break;
			default:
				INFO_LOG(NAOMI, "systemsp::write(%x) CF ATA unknown reg = %x\n", addr, v);
				break;
			}
			return;
		}
		else if ((sp->bank & 0x3f00) == 0x3b00)
		{
			if ((addr & 0xffff) == 0x18)
			{
				/* CF IDE AltStatus/Device Ctrl register */
				if ((sp->ata.devCtrl & 4) && !(v & 4))
				{
					/* software reset */
					sp->ata.bufferIndex = ~0u;
					sp->ata.error = 0;
					sp->ata.status &= ~(0x08 | 0x01);	/* DRQ, ERR */
				}
				sp->ata.devCtrl = v & 0x86;
			}
			else
				INFO_LOG(NAOMI, "systemsp::write(%x) CF IDE unknown reg %x data %x\n", addr, offset, v);
			return;
		}
		else if ((sp->bank & 0x3f00) == 0x3d00)
			/* Network aka Media board shared buffer/RAM */
			return;
		else if ((sp->bank & 0x3f00) == 0x3f00)
			/* Network board present flag (0x01) */
			return;
	}
	else if (addr == 0x10000)
	{
		/* bank register */
		sp->bank = (u16)v;
		return;
	}
	else if (addr < 0x10100)
		/* IRQ pending/reset, ATA control
		 * (0x10084 is read back as written as the game has it (the
		 * interrupt mask?), but nothing has ever been kept of what is
		 * written to it: left so) */
		return;
	else if (addr < 0x10128)
	{
		/* I/O chip for outputs */
		switch (addr - 0x10100)
		{
		case 0x8: /* OUT_PORT3 (CN9 25-32)? */
			break;
		case 0xc: /* IO-1 CN9 33-40 */
			io_set_cn9_33_40(sp, (u8)v);
			break;
		case 0x10: /* OUT_PORT4 (CN9 49-56) */
			/* 0: (P1 coin meter)
			 * 1: (P2 coin meter)
			 * giant tetris:
			 * 2: P1 start lamp
			 * 3: P2 start lamp
			 * 4: P1 button 1 lamp
			 * 5: P2 button 1 lamp
			 * 6: P1 button 2 lamp
			 * 7: P2 button 2 lamp
			 * dinosaur king, love & berry:
			 * 2: coin blocker 1 (not used)
			 * 3: coin blocker 2 (not used)
			 * 4: cd1 card pickout
			 * 5: cd2 card pickout (not used)
			 * 6: cd1 jam reset
			 * 7: cd2 jam reset (not used)
			 * hopper:
			 * 4: payout?
			 * 7: ? */
			io_set_cn9_49_56(sp, (u8)v);
			break;
		case 0x14: /* OUT CN10 17-24 */
			/* dinosaur king, love & berry:
			 * 0: 232c select1 (not used)
			 * 1: 232c select2 (not used)
			 * 2: 232c reset (not used)
			 * 3: card trigger (not used)
			 * 4: rfid chip1 reset
			 * 5: rfid chip2 reset
			 * 6: rfid chip1 empty lamp
			 * 7: rfid chip2 empty lamp */
			io_set_cn10_17_24(sp, (u8)v);
			break;
		case 0x24: /* read too */
		default:
			break;
		}
		return;
	}
	else if (addr == 0x10128)
	{
		/* eeprom */
		sp->eeprom.di = (v & 1) != 0;
		sp->eeprom.cs = (v & 2) != 0;
		eeprom_write_clk(&sp->eeprom, (v & 4) != 0);
		/* (what has been written goes to its file once the chip is let go) */
		if (!(v & 2) && sp->eeprom.dirty)
			sp_save_eeprom(sp);
		return;
	}
	else if (addr < 0x10180)
		/* rom board dip switches? */
		return;
	else if (addr < 0x101c0)
	{
		/* custom UART 1 */
		port_write(&sp->uart1, addr, (u8)v);
		return;
	}
	else if (addr < 0x101f0)
	{
		/* custom UART 2 */
		port_write(&sp->uart2, addr, (u8)v);
		return;
	}
	INFO_LOG(NAOMI, "systemsp::writeMemArea0<%d>: Unknown addr %x = %x\n", (int)sz, addr, (int)v);
}

u32 systemsp_read_area0(u32 addr, u32 sz)
{
	if (sz != 1 && sz != 2)
		sz = 4;
	if (sp_live == NULL)
		return sz_mask(sz);
	return read_area0(sp_live, addr, sz) & sz_mask(sz);
}

void systemsp_write_area0(u32 addr, u32 data, u32 sz)
{
	if (sz != 1 && sz != 2)
		sz = 4;
	if (sp_live != NULL)
		write_area0(sp_live, addr, data & sz_mask(sz), sz);
}

/*
 * The board's files
 */

/* Where the game's EEPROM would be, without its ".eeprom", and @suffix */
static void savePath(char *dst, size_t len, const char *suffix)
{
	size_t n;
	strlcpy(dst, eeprom_file, len);
	n = strlen(dst);
	if (n >= 7 && memcmp(&dst[n - 7], ".eeprom", 7) == 0)
		dst[n - 7] = '\0';
	strlcat(dst, suffix, len);
}

static const char *regionSuffix(int region)
{
	switch (region)
	{
	case 0:
		return "-jp";
	case 1:
		return "-us";
	default:
		return "-exp";
	}
}

/* The RFID chips' files, by port number (1 or 2) */
static void getCardPath(system_sp *sp, char *dst, size_t len, int index)
{
	savePath(dst, len, regionSuffix(sp->region));
	strlcat(dst, index == 1 ? ".rfid1" : ".rfid2", len);
}

static void eepromPath(system_sp *sp, char *dst, size_t len)
{
	savePath(dst, len, regionSuffix(sp->region));
	strlcat(dst, ".eeprom", len);
}

static void sp_save_eeprom(system_sp *sp)
{
	if (retro_atomic_load_acquire_int(&sp->eepromFailed) && retro_atomic_exchange_int(&sp->eepromFailed, 0))
		WARN_LOG(NAOMI, "System SP: cannot save the EEPROM\n");
	eepromPath(sp, sp->path[0], sizeof(sp->path[0]));
	if (save_writer_put_file(sp->path[0], sp->eeprom.data, sizeof(sp->eeprom.data), &sp->eepromFailed))
		sp->eeprom.dirty = false;
}

static void sp_save_flash(system_sp *sp)
{
	RFILE *f;
	savePath(sp->path[0], sizeof(sp->path[0]), ".flash");
	f = filestream_open(sp->path[0], RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f == NULL || filestream_write(f, sp->m4.base.rom_ptr, sp->m4.base.rom_size) != (int64_t)sp->m4.base.rom_size)
		WARN_LOG(NAOMI, "System SP: cannot save the flash to %s\n", sp->path[0]);
	else
		INFO_LOG(NAOMI, "System SP: flash saved to %s\n", sp->path[0]);
	if (f != NULL)
		filestream_close(f);
	sp->flashDirty = false;
}

/* The game's CompactFlash card: <set>/<media>.chd beside the set, or
 * <parent>/<media>.chd */
static bool openChd(system_sp *sp)
{
	char *path = sp->path[0];
	const size_t len = sizeof(sp->path[0]);
	char *dot;
	strlcpy(path, g_roms_dir, len);
	strlcat(path, g_base_name, len);
	/* (the set's file name without its extension) */
	dot = strrchr(path + strlen(g_roms_dir), '.');
	if (dot != NULL)
		*dot = '\0';
	strlcat(path, "/", len);
	strlcat(path, sp->mediaName, len);
	strlcat(path, ".chd", len);
	sp->chd = chd_image_open(path, NULL, 0);
	if (sp->chd == NULL && sp->parentName != NULL)
	{
		char *ppath = sp->path[1];
		strlcpy(ppath, g_roms_dir, len);
		strlcat(ppath, sp->parentName, len);
		strlcat(ppath, "/", len);
		strlcat(ppath, sp->mediaName, len);
		strlcat(ppath, ".chd", len);
		sp->chd = chd_image_open(ppath, NULL, 0);
	}
	if (sp->chd == NULL)
	{
		ERROR_LOG(NAOMI, "System SP: cannot open the CompactFlash image %s: the game cannot start without it\n", path);
		return false;
	}
	sp->hunkbytes = chd_image_hunk_bytes(sp->chd);
	if (sp->hunkbytes == 0 || sp->hunkbytes % SECTOR_SIZE != 0)
	{
		ERROR_LOG(NAOMI, "System SP: %s is not a CompactFlash image\n", path);
		chd_image_close(sp->chd);
		sp->chd = NULL;
		return false;
	}
	sp->chdSize = (u64)sp->hunkbytes * chd_image_hunk_count(sp->chd);
	free(sp->hunkmem);
	sp->hunkmem = (u8 *)malloc(sp->hunkbytes);
	if (sp->hunkmem == NULL)
	{
		ERROR_LOG(NAOMI, "System SP: no memory for %s\n", path);
		chd_image_close(sp->chd);
		sp->chd = NULL;
		return false;
	}
	sp->hunknum = ~0u;
	INFO_LOG(NAOMI, "System SP: CompactFlash image %s\n", path);
	return true;
}

/*
 * The game's header (RomBootID) on a CompactFlash card: the file that the
 * card's 1STREAD.BIN names, in the first partition, a FAT16 one. Both are
 * encrypted, the header twice over but for one game (tetgiano).
 */
#define SECSIZE 512

typedef struct boot_fat
{
	u8 sectorsPerCluster;
	u16 rootFolderSize;
	u64 rootOffset;
	u64 dataStartOffset;
} boot_fat;

static void boot_decrypt(system_sp *sp, u8 *p, u32 len)
{
	u32 i;
	encReset(sp);
	for (i = 0; i + 1 < len; i += 2)
	{
		const u16 w = sp_decrypt(sp, le16(&p[i]));
		p[i] = (u8)w;
		p[i + 1] = w >> 8;
	}
	encReset(sp);
}

/* (a file in the root folder, all of whose clusters follow each other,
 * named @name, @nameLen long) */
static bool boot_open_file(system_sp *sp, const boot_fat *fat, const char *name, size_t nameLen,
		u64 *offset, u32 *size)
{
	u32 i;
	for (i = 0; i < fat->rootFolderSize; i++)
	{
		u8 entry[32];
		char entryName[12];
		size_t n, e;
		u8 b, attributes;
		u16 cluster;
		if (!readCf(sp, fat->rootOffset * SECSIZE + i * 32, 32, entry))
			return false;
		b = entry[0];
		if (b == 0)
			/* end of directory */
			return false;
		if (b == 0xe5 || b == 0x2e)
			/* unused or ./.. entry */
			continue;
		attributes = entry[11];
		if (attributes == 0xf || (attributes & (0x8 | 0x4)))
			/* long name or volume name/system */
			continue;
		n = 8;
		while (n > 0 && entry[n - 1] == ' ')
			n--;
		memcpy(entryName, entry, n);
		e = 3;
		while (e > 0 && entry[8 + e - 1] == ' ')
			e--;
		if (e > 0)
		{
			entryName[n++] = '.';
			memcpy(&entryName[n], &entry[8], e);
			n += e;
		}
		if (n != nameLen || memcmp(entryName, name, n) != 0)
			continue;
		cluster = le16(&entry[26]);
		if (cluster < 2)
			return false;
		*size = le32(&entry[28]);
		*offset = (fat->dataStartOffset + (u64)(cluster - 2) * fat->sectorsPerCluster) * SECSIZE;
		return true;
	}
	return false;
}

static bool boot_load(system_sp *sp, u8 *bootId)
{
	u8 sec[SECSIZE];
	u8 buffer[0x500];
	char imageName[0x80 + 16];
	boot_fat fat;
	size_t nameLen, i;
	u32 start, size;
	u16 resSectors, fatSize;
	u8 fatCount;
	u64 offset;

	/* master boot record */
	if (!readCf(sp, 0, SECSIZE, sec))
		return false;
	if (sec[510] != 0x55 || sec[511] != 0xaa)
		return false;
	/* partition 1: FAT16B? */
	if (sec[446 + 4] != 6)
		return false;
	start = le32(&sec[446 + 8]);

	/* the partition's Bios Parameter Block */
	if (!readCf(sp, (u64)start * SECSIZE, SECSIZE, sec))
		return false;
	fat.sectorsPerCluster = sec[13];
	resSectors = le16(&sec[14]);
	fatCount = sec[16];
	fat.rootFolderSize = le16(&sec[17]); /* in entries */
	fatSize = le16(&sec[22]); /* in sectors */
	/* FATs start after reserved sectors */
	fat.rootOffset = (u64)start + resSectors + (u64)fatSize * fatCount;
	fat.dataStartOffset = fat.rootOffset + (u64)fat.rootFolderSize * 32 / SECSIZE;

	if (!boot_open_file(sp, &fat, "1STREAD.BIN", 11, &offset, &size) || size <= 0x80)
		return false;
	if (!readCf(sp, offset, 0x100, buffer))
		return false;
	boot_decrypt(sp, buffer, 0x100);
	if (memcmp(&buffer[0], "SPCF", 4))
		/* wrong signature */
		return false;
	nameLen = boundedLength((const char *)&buffer[0x80], 0x80);
	memcpy(imageName, &buffer[0x80], nameLen);
	if (nameLen > 12)
	{
		/* make short 8.3 name: its first 6, "~1" and what is from its dot on */
		const char *dot = (const char *)memchr(&buffer[0x80], '.', nameLen);
		const size_t ext = dot != NULL ? nameLen - (size_t)(dot - (const char *)&buffer[0x80]) : 0;
		memcpy(&imageName[6], "~1", 2);
		if (ext != 0)
			memcpy(&imageName[8], dot, ext);
		nameLen = 8 + ext;
	}
	for (i = 0; i < nameLen; i++)
		if (imageName[i] >= 'a' && imageName[i] <= 'z')
			imageName[i] = (char)(imageName[i] - 'a' + 'A');

	if (!boot_open_file(sp, &fat, imageName, nameLen, &offset, &size) || size < sizeof(buffer))
		return false;
	if (!readCf(sp, offset, sizeof(buffer), buffer))
		return false;
	boot_decrypt(sp, buffer, sizeof(buffer));
	if (memcmp(&buffer[0], "SystemSP", 8))
		/* Yes, it is encrypted twice (except tetgiano) */
		boot_decrypt(sp, buffer, sizeof(buffer));
	memcpy(bootId, buffer, sizeof(buffer));
	return true;
}

/*
 * The board
 */
static system_sp *SP(cartridge *c)
{
	return (system_sp *)c;
}

static void sp_reset(cartridge *c)
{
	system_sp *sp = SP(c);
	m4_cart_reset(c);
	sp->bank = 0;
	memset(&sp->ata, 0, sizeof(sp->ata));
	sp->ata.status = 0x50;		/* RDY, DSC */
	if (sp->chd == NULL)
		sp->ata.status = 0x30;	/* no card: not ready, drive fault */
	sp->ata.driveHead = 0xa0;
	sp->flash.cmdState = FLASH_INIT;
	sp->flash.progAddress = ~0u;
	sp->flash.wordCount = 0;
	encReset(sp);
	eeprom_reset(&sp->eeprom);
	/* the network board's memory */
	net_clear(sp);
	sp->inetaddr = 0x641ea8c0;	/* 192.168.30.100 */
	sp->netmask = 0x00ffffff;	/* 255.255.255.0 */
	sp->sockets = 0;
	if (sp->uart1.pipe != NULL)
		sp->uart1.pipe->ops->reset(sp->uart1.pipe);
	if (sp->uart2.pipe != NULL)
		sp->uart2.pipe->ops->reset(sp->uart2.pipe);
	io_reset(sp);
	if (ataSchedId >= 0)
		sh4_sched_request(ataSchedId, -1);
}

static bool name_is(const system_sp *sp, const char *name)
{
	return strcmp(sp->gameName, name) == 0;
}

static bool name_starts(const system_sp *sp, const char *prefix)
{
	return strncmp(sp->gameName, prefix, strlen(prefix)) == 0;
}

static bool sp_init(cartridge *c)
{
	system_sp *sp = SP(c);
	/* The ROM board's battery-backed SRAM is 128 KB, a NAOMI's 32: it is
	 * the machine's (sys_nvmem_sram), saved to its file and in the state
	 * as a NAOMI's is. */
	const u32 sramSize = 128 * 1024;
	m4_cart_init(c);
	if (sys_nvmem_sram.size != sramSize)
	{
		memchip_allocate(&sys_nvmem_sram, sramSize);
		memchip_load(&sys_nvmem_sram, nvmem_file);
		BBSRAM_SIZE = sramSize;
	}

	sp->haveBootId = false;
	if (sp->mediaName != NULL)
	{
		RFILE *f;
		if (!openChd(sp))
			return false;
		sp->haveBootId = boot_load(sp, sp->bootId);
		if (!sp->haveBootId)
			WARN_LOG(NAOMI, "System SP: the game's header is not on the CompactFlash card\n");

		/* What the BIOS put in the flash from the card the last time: it
		 * does not do it again if it is there. */
		savePath(sp->path[0], sizeof(sp->path[0]), ".flash");
		f = filestream_open(sp->path[0], RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (f != NULL)
		{
			if (filestream_get_size(f) == (int64_t)c->rom_size
					&& filestream_read(f, c->rom_ptr, c->rom_size) == (int64_t)c->rom_size)
				INFO_LOG(NAOMI, "System SP: flash loaded from %s\n", sp->path[0]);
			else
				memset(c->rom_ptr, 0xff, c->rom_size);
			filestream_close(f);
		}
	}
	else if (c->rom_size >= 0x500)
	{
		memcpy(sp->bootId, c->rom_ptr, 0x500);
		if (memcmp(c->rom_ptr, "NAOMI", 5) && memcmp(c->rom_ptr, "Naomi2", 6) && memcmp(c->rom_ptr, "SystemSP", 8))
		{
			u32 i;
			encReset(sp);
			for (i = 0; i < 0x500; i += 2)
			{
				const u16 w = sp_decrypt(sp, le16(&c->rom_ptr[i]));
				sp->bootId[i] = (u8)w;
				sp->bootId[i + 1] = w >> 8;
			}
			encReset(sp);
		}
		sp->haveBootId = true;
	}

	/* The region: the one asked for, if the game runs there */
	sp->region = settings.dreamcast.region < 2 ? (int)settings.dreamcast.region : 2;
	if (sp->haveBootId)
	{
		const u8 country = sp->bootId[0x428];
		if (country != 0 && (country & (2 << sp->region)) == 0)
		{
			if (country & 4)
				sp->region = 1;
			else if (country & 8)
				sp->region = 2;
			else if (country & 2)
				sp->region = 0;
			NOTICE_LOG(NAOMI, "System SP: the game does not run in that region: region %d\n", sp->region);
		}
	}

	/* dinoki4 doesn't use rfid chips. dinokich and loveber3cn have an IC card reader instead */
	sp->ioKind = IO_DEFAULT;
	if (name_is(sp, "dinokich") || name_is(sp, "loveber3cn"))
	{
		getCardPath(sp, sp->path[0], sizeof(sp->path[0]), 1);
		getCardPath(sp, sp->path[1], sizeof(sp->path[1]), 2);
		sp->dinokich = dinokich_reader_open(sp->gameName, sp->path[0], sp->path[1],
				port_update_status, &sp->uart1);
		sp->uart1.pipe = dinokich_reader_pipe(sp->dinokich);
	}
	else if ((name_starts(sp, "dinoki") && !name_is(sp, "dinoki4"))
			|| name_starts(sp, "loveber"))
	{
		getCardPath(sp, sp->path[0], sizeof(sp->path[0]), 1);
		sp->uart1.pipe = sp_rfid_open(1, sp->gameName, sp->region, sp->path[0], port_update_status, &sp->uart1);
		getCardPath(sp, sp->path[0], sizeof(sp->path[0]), 2);
		sp->uart2.pipe = sp_rfid_open(2, sp->gameName, sp->region, sp->path[0], port_update_status, &sp->uart2);
	}
	else if (name_is(sp, "isshoni"))
	{
		sp->uart1.pipe = sp_touchscreen_open(port_update_status, &sp->uart1);
		sp->ioKind = IO_ISSHONI;
	}
	else if (name_is(sp, "manpuku"))
		sp->ioKind = IO_MANPUKU;
	else if (name_starts(sp, "kingyo") || name_is(sp, "shateki"))
		sp->ioKind = IO_KINGYO;
	else if (name_is(sp, "magicpop")
			|| name_is(sp, "ochaken")
			|| name_is(sp, "puyomedal")
			|| name_is(sp, "unomedal")
			|| name_is(sp, "westdrmg"))
		sp->ioKind = IO_MEDAL;
	else if (name_is(sp, "btlracer"))
		sp->ioKind = IO_BATTLE_RACER;
	if (name_starts(sp, "dinoki") || name_starts(sp, "loveber"))
		sp->ioKind = IO_CARD_READER;

	sp_reset(c);
	return true;
}

/* @len bytes at @s, without the spaces and zeroes after them: how many */
static u32 sp_id_length(const u8 *s, u32 len)
{
	while (len != 0 && (s[len - 1] == ' ' || s[len - 1] == '\0'))
		len--;
	return len;
}

static void sp_game_id(cartridge *c, char *id)
{
	system_sp *sp = SP(c);
	const u8 *title;
	u32 len;
	if (!sp->haveBootId)
	{
		if (sp->mediaName != NULL)
			strlcpy(id, sp->gameName, CART_GAME_ID_SIZE);
		else
			m4_cart_game_id(c, id);
		return;
	}
	/* the game's title, as the System SP's BIOS has it: the second */
	title = &sp->bootId[0x50];
	len = sp_id_length(title, 0x20);
	if (len == 0)
	{
		title = &sp->bootId[0x30];
		len = sp_id_length(title, 0x20);
	}
	memcpy(id, title, len);
	id[len] = '\0';
}

static void sp_set_key_data(cartridge *c, u8 *key_data)
{
	system_sp *sp = SP(c);
	if (key_data != NULL)
	{
		sp->subkey1 = (u16)((key_data[0x5e2] << 8) | key_data[0x5e0]);
		sp->subkey2 = (u16)((key_data[0x5e6] << 8) | key_data[0x5e4]);
	}
	m4_cart_set_key_data(c, key_data);
}

/* inet_addr() of a dotted quad: the address with its first number in its
 * lowest byte, as it is in memory */
static u32 parseInetAddr(const char *s, size_t len)
{
	u32 addr = 0;
	int part = 0;
	u32 value = 0;
	bool digits = false;
	size_t i;
	for (i = 0; i <= len; i++)
	{
		const char c = i < len ? s[i] : '\0';
		if (c >= '0' && c <= '9')
		{
			value = value * 10 + (u32)(c - '0');
			if (value > 255)
				return 0xffffffff;
			digits = true;
		}
		else if (c == '.' || c == '\0')
		{
			if (!digits || part > 3)
				return 0xffffffff;
			addr |= value << (part * 8);
			part++;
			value = 0;
			digits = false;
			if (c == '\0')
				break;
		}
		else
			return 0xffffffff;
	}
	return part == 4 ? addr : 0xffffffff;
}

/* TODO implement actual networking features. Sockets are numbers given
 * out and taken back; nothing goes on any network. */
static void process(system_sp *sp)
{
	u8 reqId, netmem4;
	int cmd;
	sp->m4.base.dma_offset |= 0x40000000; /* needed by BIOS. why? */
	reqId = net_r8(sp, 0x200);
	cmd = net_r16(sp, 0x202);
	netmem4 = net_r8(sp, 4); /* FIXME memset should be done later on */
	memset(&sp->netmem[0], 0, 32);
	sp->netPages[0] |= 1;
	net_w8(sp, 0, reqId);
	net_w8(sp, 1, 0);
	switch (cmd)
	{
	case 1: /* init? nop? */
		net_w16(sp, 2, NET_OK);
		break;
	case 0x100: /* get dimm status */
		net_w16(sp, 2, NET_OK);
		/* 0 initializing
		 * 1 checking network
		 * 2 ?
		 * 3 testing prg
		 * 4 loading prg
		 * 5 ready */
		net_w32(sp, 4, 5);
		break;
	case 0x101: /* get firmware version */
		net_w16(sp, 2, NET_OK);
		net_w8(sp, 4, 0x25);	/* minor */
		net_w8(sp, 5, 1);		/* major */
		break;
	case 0x104: /* get network type and address */
		net_w16(sp, 2, NET_OK);
		net_w32(sp, 4, 5);	/* 4: ether (dhcp), 5: ether (static ip) */
		net_w32(sp, 0x24, sp->inetaddr);
		net_w32(sp, 0x28, sp->netmask);
		break;

	case 0x204: /* set network type and address */
		/* 4: type (0 none, 3,4 ether) */
		sp->inetaddr = net_r32(sp, 0x24);
		sp->netmask = net_r32(sp, 0x28);
		INFO_LOG(NAOMI, "process: set net type %d ip %08x mask %08x\n", netmem4, sp->inetaddr, sp->netmask);
		net_w16(sp, 2, NET_OK);
		break;

	case 0x301: /* network test */
		{
			/* 204: 4
			 * (in the system's RAM, the only place it can be) */
			static const char text[] = "CHECKING NETWORK\nPRETENDING... :P\n--- COMPLETED---\n";
			const u32 addr = net_r32(sp, 0x208);
			u32 i;
			if (((addr >> 26) & 7) == 3)
				for (i = 0; i < sizeof(text); i++)
					mem_b.data[(addr + i) & RAM_MASK] = text[i];
			net_w16(sp, 2, NET_OK);
			net_w32(sp, 4, 1); /* TODO how to signal the test progress/completion? */
		}
		break;

	case 0x401: /* accept */
		net_w16(sp, 2, NET_ERROR);
		break;
	case 0x402: /* bind */
		net_w16(sp, 2, NET_OK);
		break;
	case 0x403: /* close socket */
		{
			const int sockidx = (int)net_r32(sp, 0x208);
			if (sockidx < 1 || sockidx > 63 || !(sp->sockets & BIT64(sockidx)))
				net_w16(sp, 2, NET_ERROR);
			else
			{
				sp->sockets &= ~BIT64(sockidx);
				net_w16(sp, 2, NET_OK);
			}
		}
		break;
	case 0x404: /* connect */
		net_w16(sp, 2, NET_OK);
		break;
	case 0x406: /* inet_addr */
		{
			const u32 addr = net_r32(sp, 0x208) & (NETMEM_SIZE - 1);
			const char *s = (const char *)&sp->netmem[addr];
			const u32 ipaddr = parseInetAddr(s, boundedLength(s, NETMEM_SIZE - addr));
			net_w16(sp, 2, NET_OK);
			net_w32(sp, 4, ipaddr);
		}
		break;
	case 0x408: /* listen */
		net_w16(sp, 2, NET_OK);
		break;
	case 0x409: /* recv */
		net_w16(sp, 2, NET_OK);
		break;
	case 0x40a: /* send */
		{
			const u32 len = net_r32(sp, 0x210);
			net_w16(sp, 2, NET_OK);
			net_w32(sp, 4, len); /* ?? */
		}
		break;
	case 0x40b: /* openSocket */
		{
			int sockidx = -1;
			int i;
			for (i = 1; i <= 63; i++)
				if (!(sp->sockets & BIT64(i)))
				{
					sp->sockets |= BIT64(i);
					sockidx = i;
					break;
				}
			net_w16(sp, 2, sockidx != -1 ? NET_OK : NET_ERROR);
			if (sockidx != -1)
				net_w32(sp, 4, (u32)sockidx);
		}
		break;
	case 0x40e: /* setsockopt */
		net_w16(sp, 2, NET_OK);
		break;
	case 0x410: /* settimeout */
		net_w16(sp, 2, NET_OK);
		break;
	case 0x411: /* geterrno */
		net_w16(sp, 2, NET_OK);
		net_w32(sp, 4, 111);	/* FIXME errno values TBD */
		break;
	case 0x414: /* getParambyDHCP? */
		net_w16(sp, 2, NET_OK);
		net_w32(sp, 4, 1);	/* ??? */
		break;
	case 0x415: /* modifyMyIPaddr? */
		net_w16(sp, 2, NET_OK);
		break;

	default:
		WARN_LOG(NAOMI, "Unknown netdimm command: %x\n", cmd);
		net_w16(sp, 2, NET_ERROR);
		break;
	}
	sp_update_interrupt(sp, INT_DIMM);
}

static u32 sp_read_mem(cartridge *c, u32 address, u32 size)
{
	if ((address & 0xff) == 0x4c)
		/* DIMM status */
		return DIMM_STATUS & ~(((SB_ISTEXT >> 3) & 1) << 8);
	return m4_cart_read_mem(c, address, size);
}

static void sp_write_mem(cartridge *c, u32 address, u32 data, u32 size)
{
	if ((address & 0xff) == 0x4c)
	{
		/* DIMM status */
		if (data & 0x100)
			asic_CancelInterrupt(holly_EXP_PCI);
		if ((data & 2) == 0)
			/* irq to dimm */
			process(SP(c));
	}
	else
		naomi_board_write_mem(c, address, data, size);
}

static bool sp_read(cartridge *c, u32 offset, u32 size, void *dst)
{
	system_sp *sp = SP(c);
	if ((offset & 0x3f000000) == 0x3f000000)
	{
		/* network card present */
		const u32 rc = 1;
		memcpy(dst, &rc, min_u32(size, 4));
		return true;
	}
	if ((offset & 0x3f000000) == 0x3d000000)
	{
		/* network card mem */
		switch (size)
		{
		case 4:
			{
				const u32 v = net_r32(sp, offset);
				memcpy(dst, &v, sizeof(v));
			}
			break;
		case 2:
			{
				const u16 v = net_r16(sp, offset);
				memcpy(dst, &v, sizeof(v));
			}
			break;
		case 1:
		default:
			*(u8 *)dst = net_r8(sp, offset);
			break;
		}
		return true;
	}
	return m4_cart_read(c, offset, size, dst);
}

static void flashChanged(system_sp *sp, u32 op, u32 offset, u32 data)
{
	sp->flash.changes++;
	sp->flash.changesHash = (sp->flash.changesHash ^ (op << 28) ^ offset ^ (data << 7)) * 0x01000193u;
	sp->flashDirty = true;
}

/* (a whole word, inside the flash) */
static void flash_program(system_sp *sp, u32 offset, u32 data)
{
	const u16 w = (u16)data;
	memcpy(&sp->m4.base.rom_ptr[offset & (sp->m4.base.rom_size - 2)], &w, sizeof(w));
}

/* @size bytes at @offset on, in the flash, erased: a chip's or a sector's */
static void flash_erase(system_sp *sp, u32 offset, u32 data, u32 size, u32 op)
{
	const u32 start = offset & 0x1fffffff & ~(size - 1);
	if (start < sp->m4.base.rom_size)
	{
		memset(&sp->m4.base.rom_ptr[start], 0xff, min_u32(size, sp->m4.base.rom_size - start));
		flashChanged(sp, op, offset, data);
	}
}

static bool sp_write(cartridge *c, u32 offset, u32 size, u32 data)
{
	system_sp *sp = SP(c);
	if ((offset & 0x3f000000) == 0x3d000000)
	{
		u32 idx;
		/* network card mem */
		switch (size)
		{
		case 4:
			net_w32(sp, offset, data);
			break;
		case 2:
			net_w16(sp, offset, (u16)data);
			break;
		case 1:
		default:
			net_w8(sp, offset, (u8)data);
			break;
		}
		idx = offset & (NETMEM_SIZE - 2);
		/* FIXME */
		if (idx == 2)
		{
			memcpy(&sp->netmem[idx + 0x200], &data, min_u32(size, 4));
			sp->netPages[0] |= 1;
		}
		return true;
	}
	switch (sp->flash.cmdState)
	{
	case FLASH_INIT:
		if ((offset & 0xfff) == 0xaaa && data == 0xaa)
		{
			sp->flash.cmdState = FLASH_AAA_AA_1;
			return true;
		}
		else if (offset == sp->flash.progAddress && data == 0x29)
		{
			/* write buffer to flash */
			sp->flash.progAddress = ~0u;
			return true;
		}
		break;
	case FLASH_AAA_AA_1:
		if (((offset & 0xfff) == 0x555 || (offset & 0xfff) == 0x554) && data == 0x55)
		{
			sp->flash.cmdState = FLASH_555_55_1;
			return true;
		}
		sp->flash.cmdState = FLASH_INIT;
		break;
	case FLASH_555_55_1:
		if ((offset & 0xfff) == 0xaaa)
		{
			if (data == 0xa0)
			{
				sp->flash.cmdState = FLASH_PROGRAM;
				return true;
			}
			else if (data == 0x80)
			{
				sp->flash.cmdState = FLASH_AAA_80;
				return true;
			}
		}
		else if (data == 0x25)
		{
			sp->flash.cmdState = FLASH_WRITE_BUF_1;
			sp->flash.progAddress = offset;
			return true;
		}
		sp->flash.cmdState = FLASH_INIT;
		break;
	case FLASH_PROGRAM:
		flash_program(sp, offset, data);
		flashChanged(sp, 1, offset, data);
		sp->flash.cmdState = FLASH_INIT;
		return true;
	case FLASH_WRITE_BUF_1:
		sp->flash.wordCount = (u16)(data + 1);
		sp->flash.cmdState = FLASH_WRITE_BUF_2;
		return true;
	case FLASH_WRITE_BUF_2:
		flash_program(sp, offset, data);
		flashChanged(sp, 2, offset, data);
		if (--sp->flash.wordCount == 0)
			sp->flash.cmdState = FLASH_INIT;
		return true;
	case FLASH_AAA_80:
		if ((offset & 0xfff) == 0xaaa && data == 0xaa)
		{
			sp->flash.cmdState = FLASH_AAA_AA_2;
			return true;
		}
		INFO_LOG(NAOMI, "Unexpected command %x %x in state aaa_80\n", offset, data);
		sp->flash.cmdState = FLASH_INIT;
		break;
	case FLASH_AAA_AA_2:
		if (((offset & 0xfff) == 0x555 || (offset & 0xfff) == 0x554) && data == 0x55)
		{
			sp->flash.cmdState = FLASH_555_55_2;
			return true;
		}
		INFO_LOG(NAOMI, "Unexpected command %x %x in state aaa_aa_2\n", offset, data);
		sp->flash.cmdState = FLASH_INIT;
		break;
	case FLASH_555_55_2:
		if ((offset & 0xfff) == 0xaaa && data == 0x10)
		{
			/* Erase chip: a chip is 64 MB */
			flash_erase(sp, offset, data, 64 * 1024 * 1024, 3);
			sp->flash.cmdState = FLASH_INIT;
			return true;
		}
		else if (data == 0x30)
		{
			/* Erase sector: the chips' sectors are 128 KB, as their CFI
			 * data says (m4cartridge.c: 512 sectors of 0x200 * 256 bytes) */
			flash_erase(sp, offset, data, 128 * 1024, 4);
			sp->flash.cmdState = FLASH_INIT;
			return true;
		}
		INFO_LOG(NAOMI, "Unexpected command %x %x in state 555_55_2\n", offset, data);
		sp->flash.cmdState = FLASH_INIT;
		break;
	default:
		sp->flash.cmdState = FLASH_INIT;
		break;
	}

	return m4_cart_write(c, offset, size, data);
}

static void sp_close(cartridge *c)
{
	system_sp *sp = SP(c);
	if (ataSchedId >= 0)
		sh4_sched_request(ataSchedId, -1);
	if (sp->mediaName != NULL && sp->flashDirty)
		sp_save_flash(sp);
	if (sp->eeprom.dirty)
		sp_save_eeprom(sp);
	/* (the flags are this one's: the writer is done with them first) */
	save_writer_drain();
	if (sp->chd != NULL)
		chd_image_close(sp->chd);
	sp->chd = NULL;
	sp_live = NULL;
	m4_cart_destruct(c);
	port_close(&sp->uart2);
	port_close(&sp->uart1);
	sp->dinokich = NULL;
	free(sp->hunkmem);
	sp->hunkmem = NULL;
	free(sp->netmem);
	sp->netmem = NULL;
}

static const cartridge_ops systemsp_ops = {
	sp_init,
	sp_reset,
	sp_read_mem,
	sp_write_mem,
	sp_read,
	sp_write,
	cart_get_ptr,
	m4_cart_get_dma_ptr,
	m4_cart_advance_ptr,
	sp_game_id,
	m4_cart_serialize,
	m4_cart_unserialize,
	cart_no_state,
	cart_no_state,
	m4_cart_set_key,
	sp_set_key_data,
	m4_cart_dma_offset_changed,
	m4_cart_pio_offset_changed,
	sp_close
};

/* mb_serial.ic57 */
static const u8 sp_eeprom_data[0x80] = {
	0xf5, 0x90, 0x53, 0x45, 0x47, 0x41, 0x20, 0x45, 0x4e, 0x54, 0x45, 0x52,
	0x50, 0x52, 0x49, 0x53, 0x45, 0x53, 0x2c, 0x4c, 0x54, 0x44, 0x2e, 0x00,
	0x4e, 0x41, 0x4f, 0x4d, 0x49, 0x00, 0x00, 0x00, 0x41, 0x41, 0x46, 0x45,
	0x30, 0x31, 0x44, 0x31, 0x35, 0x39, 0x32, 0x34, 0x38, 0x31, 0x36, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

cartridge *systemsp_cart_open(u32 size, const char *game_name, const char *media_name, const char *parent_name)
{
	system_sp *sp = &sp_board;
	u8 *netmem = (u8 *)calloc(1, NETMEM_SIZE);
	if (netmem == NULL)
		return NULL;
	memset(sp, 0, sizeof(*sp));
	m4_cart_construct(&sp->m4, &systemsp_ops, size);
	strlcpy(sp->gameName, game_name, sizeof(sp->gameName));
	sp->mediaName = media_name;
	sp->parentName = parent_name;
	sp->hunknum = ~0u;
	sp->uart1.index = 1;
	sp->uart2.index = 2;
	sp->ioKind = IO_DEFAULT;
	io_reset(sp);
	retro_atomic_int_init(&sp->eepromFailed, 0);
	encInit();
	ataSchedId = sh4_sched_register(0, ataSchedCallback);
	sp->netmem = netmem;
	sp->inetaddr = 0x641ea8c0;	/* 192.168.30.100 */
	sp->netmask = 0x00ffffff;	/* 255.255.255.0 */
	memcpy(sp->eeprom.data, sp_eeprom_data, sizeof(sp_eeprom_data));
	sp_live = sp;
	return &sp->m4.base;
}

int systemsp_cart_region(void)
{
	return sp_live != NULL ? sp_live->region : 0;
}

/* The EEPROM's file for the region, or the BIOS's default EEPROM for it
 * (naomi_default_eeprom, once the BIOS is loaded), or the board's own when
 * there is neither */
void systemsp_cart_load_eeprom(void)
{
	system_sp *sp = sp_live;
	RFILE *f;
	bool loaded = false;
	if (sp == NULL)
		return;
	eepromPath(sp, sp->path[0], sizeof(sp->path[0]));
	f = filestream_open(sp->path[0], RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f != NULL)
	{
		u8 data[sizeof(sp->eeprom.data)];
		if (filestream_read(f, data, sizeof(data)) == (int64_t)sizeof(data))
		{
			memcpy(sp->eeprom.data, data, sizeof(data));
			loaded = true;
			INFO_LOG(NAOMI, "System SP: EEPROM loaded from %s\n", sp->path[0]);
		}
		filestream_close(f);
	}
	if (!loaded && naomi_default_eeprom != NULL)
		memcpy(sp->eeprom.data, naomi_default_eeprom, sizeof(sp->eeprom.data));
	sp->eeprom.dirty = false;
}

/*
 * The state
 */

/* The serial devices' state is this long whatever they are: what is not
 * used of it is zeroes. */
static void serializePipe(sp_port *port, void **data, unsigned int *total_size)
{
	static const u8 zeroes[PIPE_STATE_SIZE] = {0};
	const unsigned int start = *total_size;
	unsigned int used;
	if (port->pipe != NULL)
		port->pipe->ops->serialize(port->pipe, data, total_size);
	used = *total_size - start;
	if (used < PIPE_STATE_SIZE)
		LIBRETRO_SA(zeroes, PIPE_STATE_SIZE - used);
}

static void unserializePipe(sp_port *port, void **data, unsigned int *total_size)
{
	static u8 skip[PIPE_STATE_SIZE];
	const unsigned int start = *total_size;
	unsigned int used;
	if (port->pipe != NULL)
		port->pipe->ops->unserialize(port->pipe, data, total_size);
	used = *total_size - start;
	if (used < PIPE_STATE_SIZE)
		LIBRETRO_USA(skip, PIPE_STATE_SIZE - used);
}

/* The ROM board, as a state has it from V24:
 *   - the bank register, the CompactFlash controller's registers and sector
 *     buffer, and when its next interrupt is due;
 *   - where a command to the flash has got to, and how many times the
 *     flash was written since the game was loaded (and a hash of it): the
 *     flash itself is not in the state, it is in its file (.flash) for a
 *     game on a CompactFlash card, and the ROMs for one that is not;
 *   - the decrypting window's place in its cipher;
 *   - the serial EEPROM, its contents and its command so far;
 *   - what is plugged into the two serial ports (an RFID reader's card and
 *     what it has to send, a touchscreen's), PIPE_STATE_SIZE bytes each;
 *   - the motors and sensors of a medal or hopper cabinet;
 *   - the network board: its address, mask and open sockets, and the pages
 *     of its memory that were written to, NETMEM_STATE_PAGES at most.
 * 35,146 bytes in all. */
static void serializeV24(system_sp *sp, void **data, unsigned int *total_size)
{
	static const u8 zeroes[NETMEM_PAGE] = {0};
	u16 pages[NETMEM_STATE_PAGES];
	u32 count = 0;
	u32 i, b;

	LIBRETRO_S(sp->bank);
	LIBRETRO_S(sp->ata.features);
	LIBRETRO_S(sp->ata.cylinder);
	LIBRETRO_S(sp->ata.sectorCount);
	LIBRETRO_S(sp->ata.sectorNumber);
	LIBRETRO_S(sp->ata.status);
	LIBRETRO_S(sp->ata.error);
	LIBRETRO_S(sp->ata.driveHead);
	LIBRETRO_S(sp->ata.devCtrl);
	LIBRETRO_S(sp->ata.interruptPending);
	LIBRETRO_S(sp->ata.reg84);
	LIBRETRO_SA(sp->ata.buffer, SECTOR_SIZE);
	LIBRETRO_S(sp->ata.bufferIndex);
	sp_save_event(ataSchedId, data, total_size);

	LIBRETRO_S(sp->flash.cmdState);
	LIBRETRO_S(sp->flash.progAddress);
	LIBRETRO_S(sp->flash.wordCount);
	LIBRETRO_S(sp->flash.changes);
	LIBRETRO_S(sp->flash.changesHash);
	LIBRETRO_S(sp->encIv);
	LIBRETRO_S(sp->encCounter);

	eeprom_serialize(&sp->eeprom, data, total_size);
	serializePipe(&sp->uart1, data, total_size);
	serializePipe(&sp->uart2, data, total_size);
	io_serialize(sp, data, total_size);

	LIBRETRO_S(sp->inetaddr);
	LIBRETRO_S(sp->netmask);
	LIBRETRO_S(sp->sockets);
	for (i = 0; i < NETMEM_WORDS; i++)
		for (b = 0; b < 64 && sp->netPages[i] != 0; b++)
			if (sp->netPages[i] & BIT64(b))
			{
				if (count < NETMEM_STATE_PAGES)
					pages[count++] = (u16)(i * 64 + b);
				else if (!sp->netPagesWarned)
				{
					WARN_LOG(NAOMI, "System SP: more of the network board's memory is used than a state holds\n");
					sp->netPagesWarned = true;
				}
			}
	for (i = count; i < NETMEM_STATE_PAGES; i++)
		pages[i] = 0xffff;
	LIBRETRO_S(count);
	LIBRETRO_SA(pages, NETMEM_STATE_PAGES);
	for (i = 0; i < NETMEM_STATE_PAGES; i++)
	{
		if (i < count)
			LIBRETRO_SA(&sp->netmem[pages[i] * NETMEM_PAGE], NETMEM_PAGE);
		else
			LIBRETRO_SA(zeroes, NETMEM_PAGE);
	}
}

static bool unserializeV24(system_sp *sp, void **data, unsigned int *total_size)
{
	static u8 skip[NETMEM_PAGE];
	const u32 changes = sp->flash.changes;
	const u32 changesHash = sp->flash.changesHash;
	u16 pages[NETMEM_STATE_PAGES];
	u32 count = 0;
	u32 i;

	LIBRETRO_US(sp->bank);
	LIBRETRO_US(sp->ata.features);
	LIBRETRO_US(sp->ata.cylinder);
	LIBRETRO_US(sp->ata.sectorCount);
	LIBRETRO_US(sp->ata.sectorNumber);
	LIBRETRO_US(sp->ata.status);
	LIBRETRO_US(sp->ata.error);
	LIBRETRO_US(sp->ata.driveHead);
	LIBRETRO_US(sp->ata.devCtrl);
	LIBRETRO_US(sp->ata.interruptPending);
	LIBRETRO_US(sp->ata.reg84);
	LIBRETRO_USA(sp->ata.buffer, SECTOR_SIZE);
	LIBRETRO_US(sp->ata.bufferIndex);
	sp_load_event(ataSchedId, 2000, data, total_size);

	LIBRETRO_US(sp->flash.cmdState);
	LIBRETRO_US(sp->flash.progAddress);
	LIBRETRO_US(sp->flash.wordCount);
	LIBRETRO_US(sp->flash.changes);
	LIBRETRO_US(sp->flash.changesHash);
	if (sp->flash.cmdState > FLASH_555_55_2)
		sp->flash.cmdState = FLASH_INIT;
	if (sp->flash.changes != changes || sp->flash.changesHash != changesHash)
		WARN_LOG(NAOMI, "System SP: the flash has been written to since this state was saved, or before it was: "
				"the game may not go on as it did\n");
	LIBRETRO_US(sp->encIv);
	LIBRETRO_US(sp->encCounter);
	if (sp->encCounter >= 16)
		sp->encCounter = 0;

	eeprom_unserialize(&sp->eeprom, data, total_size);
	unserializePipe(&sp->uart1, data, total_size);
	unserializePipe(&sp->uart2, data, total_size);
	io_unserialize(sp, data, total_size);

	LIBRETRO_US(sp->inetaddr);
	LIBRETRO_US(sp->netmask);
	LIBRETRO_US(sp->sockets);
	sp->sockets &= ~(u64)1;
	LIBRETRO_US(count);
	LIBRETRO_USA(pages, NETMEM_STATE_PAGES);
	if (count > NETMEM_STATE_PAGES)
		count = NETMEM_STATE_PAGES;
	/* what is not in the state was never written to */
	net_clear(sp);
	for (i = 0; i < NETMEM_STATE_PAGES; i++)
	{
		if (i < count && pages[i] < NETMEM_SIZE / NETMEM_PAGE)
		{
			LIBRETRO_USA(&sp->netmem[pages[i] * NETMEM_PAGE], NETMEM_PAGE);
			sp->netPages[pages[i] / 64] |= BIT64(pages[i] % 64);
		}
		else
			LIBRETRO_USA(skip, NETMEM_PAGE);
	}
	return true;
}

static system_sp *stateBoard(void)
{
	if (SYSTEM_IS_NAOMI() && naomi_cart_systemsp)
		return sp_live;
	return NULL;
}

void systemsp_serialize_v24(void **data, unsigned int *total_size)
{
	system_sp *sp = stateBoard();
	if (sp != NULL)
		serializeV24(sp, data, total_size);
}

bool systemsp_unserialize_v24(void **data, unsigned int *total_size)
{
	system_sp *sp = stateBoard();
	if (sp != NULL)
		return unserializeV24(sp, data, total_size);
	return true;
}

/* A state from before V24 is never one of a System SP's, which could not
 * be run before: nothing to do. */
void systemsp_state_before_v24(void)
{
}

static dinokich_reader *stateDinokich(void)
{
	system_sp *sp = stateBoard();
	return sp != NULL ? sp->dinokich : NULL;
}

void systemsp_serialize_v25(void **data, unsigned int *total_size)
{
	dinokich_reader *reader = stateDinokich();
	if (reader != NULL)
		dinokich_reader_serialize_v25(reader, data, total_size);
}

bool systemsp_unserialize_v25(void **data, unsigned int *total_size)
{
	dinokich_reader *reader = stateDinokich();
	if (reader != NULL)
		dinokich_reader_unserialize_v25(reader, data, total_size);
	return true;
}

void systemsp_state_before_v25(void)
{
	dinokich_reader *reader = stateDinokich();
	if (reader != NULL)
		dinokich_reader_state_before_v25(reader);
}
