/*
	Created on: Mar 15, 2020
	Copyright 2020 flyinghead
	This file is part of flycast.
    flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
    flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
/* (the network's sockets: what a C89 compile of the C library's headers
 * leaves out) */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <stdio.h>
#include <string.h>
#include <retro_miscellaneous.h>
#include <rthreads/rthreads.h>
#include <features/features_cpu.h>
#include "naomi_m3comm.h"
#include "naomi_regs.h"
#include "lockfree.h"
#include "network/naomi_network.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_mem.h"

/* What the emulation thread and the network thread tell each other.
 *
 * comm_ram and m68k_ram belong to the emulation thread; the network thread
 * never touches them. It keeps its own picture of the ring (net_ring: this
 * board's slot, then the packet last received) and the two exchange whole
 * pieces of it through triple buffers, so neither waits for the other and
 * nothing is ever seen half written:
 *
 *   rx  the packet the network thread received last, copied into comm_ram
 *       by the emulation thread when the game next looks;
 *   tx  this board's slot as the game last wrote it, picked up by the
 *       network thread when it next has the token.
 *
 * The rest is single words. */
#define COMM_RAM_SIZE (128 * 1024)
#define PACKET_MAX (COMM_RAM_SIZE - 0x100)
#define SLOT_MAX 0x10000
enum { NET_DOWN = 0, NET_UP, NET_SEEN };

static u16 comm_ctrl = 0xC000;
static u16 comm_offset;
static u16 comm_status0;
static u16 comm_status1;
static u8 m68k_ram[128 * 1024];
static u8 comm_ram[COMM_RAM_SIZE];

static int slot_count;
static int slot_id;

static retro_atomic_int_t network_stopping;
static sthread_t *thread;
static naomi_network network;
static bool network_made;

static tribuf rx;
static u8 rx_buf[3][PACKET_MAX];
static tribuf tx;
static u8 tx_buf[3][SLOT_MAX];
static u8 net_ring[COMM_RAM_SIZE];	/* network thread only */
static int net_thread_slot_count;	/* network thread only */
static int net_thread_slot_id;		/* network thread only */
static u16 seen_packet_number;		/* emulation thread only */

static retro_atomic_int_t net_slot_size;	/* set by the game (m68k ram 0x204) */
static retro_atomic_int_t net_state;		/* NET_UP once connected, NET_SEEN once the game side knows */
static retro_atomic_int_t net_slot_count;
static retro_atomic_int_t net_slot_id;
static retro_atomic_int_t net_packet_number;

static u16 swap16(u16 w)
{
	return (u16)((w >> 8) | (w << 8));
}

/* The slot size the game set */
static u16 game_slot_size(void)
{
	return swap16(*(u16 *)&m68k_ram[0x204]);
}

static naomi_network *net(void)
{
	if (!network_made)
	{
		naomi_network_create(&network);
		network_made = true;
	}
	return &network;
}

void m3comm_close_network(void)
{
	retro_atomic_store_release_int(&network_stopping, 1);
	naomi_network_shutdown(net());
	if (thread != NULL)
	{
		sthread_join(thread);
		thread = NULL;
	}
	/* The network thread is gone: its sockets can be closed from here. */
	naomi_network_close_sockets(net());
}

void m3comm_term(void)
{
	m3comm_close_network();
	naomi_network_terminate(net());
}

/* Emulation thread: the board as it is when the machine is switched on, for
 * a reset of the machine. (The game's own reset of the board's CPU is not
 * this: the game has written that CPU's RAM by then.) */
void m3comm_reset(void)
{
	m3comm_close_network();
	comm_ctrl = 0xC000;
	comm_offset = 0;
	comm_status0 = 0;
	comm_status1 = 0;
	memset(m68k_ram, 0, sizeof(m68k_ram));
	memset(comm_ram, 0, sizeof(comm_ram));
	slot_count = 0;
	slot_id = 0;
	seen_packet_number = 0;
	/* No network thread now: the link is down until the game starts it,
	 * with nothing of the last one for sync_network() to pick up */
	retro_atomic_int_init(&net_state, NET_DOWN);
	retro_atomic_int_init(&net_slot_size, 0);
}

/* Emulation thread. Games write the board's memories and read them back
 * later, frames later at times; and which way a DMA goes is a bit of its
 * control word. (A link to other machines is not in a state.) */
void m3comm_serialize(void **data, unsigned int *total_size)
{
	u16 regs[4];
	regs[0] = comm_ctrl;
	regs[1] = comm_offset;
	regs[2] = comm_status0;
	regs[3] = comm_status1;

	LIBRETRO_SA(regs, 4);
	LIBRETRO_SA(m68k_ram, sizeof(m68k_ram));
	LIBRETRO_SA(comm_ram, sizeof(comm_ram));
}

bool m3comm_unserialize(void **data, unsigned int *total_size)
{
	u16 regs[4];

	if (!LIBRETRO_USA(regs, 4) || !LIBRETRO_USA(m68k_ram, sizeof(m68k_ram))
			|| !LIBRETRO_USA(comm_ram, sizeof(comm_ram)))
		return false;
	/* (every value is one the board can have: an offset is inside both memories) */
	comm_ctrl = regs[0];
	comm_offset = regs[1];
	comm_status0 = regs[2];
	comm_status1 = regs[3];
	/* the slot size the network thread goes by, if there is one */
	retro_atomic_store_release_int(&net_slot_size, game_slot_size());
	return true;
}

/* Network thread. */
static void connect_network(void)
{
	if (naomi_network_start(&network))
	{
		net_thread_slot_count = network.slot_count;
		net_thread_slot_id = network.slot_id;
		retro_atomic_store_release_int(&net_slot_count, network.slot_count);
		retro_atomic_store_release_int(&net_slot_id, network.slot_id);
		/* The game side sets the board up when it next looks: sync_network() */
		retro_atomic_store_release_int(&net_state, NET_UP);
	}
	else
	{
		retro_atomic_store_release_int(&network_stopping, 1);
		naomi_network_shutdown(&network);
	}
}

/* Network thread: take a packet off the ring and pass it to the game. */
static void receive_network(void)
{
	const u32 slot_size = (u32)retro_atomic_load_acquire_int(&net_slot_size);
	const u32 packet_size = slot_size * net_thread_slot_count;

	if (packet_size == 0 || slot_size + packet_size > PACKET_MAX)
		return;

	if (naomi_network_receive(&network, &net_ring[slot_size], packet_size))
	{
		memcpy(rx_buf[tribuf_back(&rx)], &net_ring[slot_size], packet_size);
		retro_atomic_store_release_int(&net_packet_number, network.packet_number);
		tribuf_publish(&rx);
	}
}

/* Network thread: with the token, send this board's slot as the game last
 * wrote it, followed by what came in from the others. */
static void send_network(void)
{
	if (network.got_token)
	{
		const u32 slot_size = (u32)retro_atomic_load_acquire_int(&net_slot_size);
		const u32 packet_size = slot_size * net_thread_slot_count;

		if (packet_size == 0 || slot_size + packet_size > PACKET_MAX)
			return;

		if (tribuf_take(&tx))
			memcpy(net_ring, tx_buf[tribuf_front(&tx)], MIN(slot_size, (u32)SLOT_MAX));
		naomi_network_send(&network, net_ring, packet_size);
		retro_atomic_store_release_int(&net_packet_number, network.packet_number);
	}
}

static void connected_state(void)
{
	u32 slot_size;

	memset(&comm_ram[0xf000], 0, 16);
	comm_ram[0xf000] = 1;
	comm_ram[0xf001] = 1;
	comm_ram[0xf002] = m68k_ram[0x204];
	comm_ram[0xf003] = m68k_ram[0x205];

	slot_size = game_slot_size();

	memset(&comm_ram[0], 0, 32);
	/* 80000 */
	comm_ram[0] = 0;
	comm_ram[1] = slot_id == 0 ? 0 : 1;
	/* 80002 */
	comm_ram[2] = 0x01;
	comm_ram[3] = 0x01;
	/* 80004 */
	if (slot_id == 0)
	{
		comm_ram[4] = 0;
		comm_ram[5] = 0;
	}
	else
	{
		comm_ram[4] = 1;
		comm_ram[5] = 1;
	}
	/* 80006: packet number */
	comm_ram[6] = 0;
	comm_ram[7] = 0;
	/* 80008 */
	comm_ram[8] = slot_id == 0 ? 0x78 : 0x73;
	comm_ram[9] = slot_id == 0 ? 0x30 : 0xa2;
	/* 8000A: offset of recvd data */
	*(u16 *)(comm_ram + 10) = (u16)(0x100 + slot_size);
	/* 8000C: recvd data size */
	*(u16 *)(comm_ram + 12) = (u16)(slot_size * slot_count);
	/* 8000E: offset of sent data */
	*(u16 *)(comm_ram + 14) = 0x100;
	/* 80010: sent data size (FIXME wrungp uses 100, others 80) */
	*(u16 *)(comm_ram + 16) = (u16)(0x80 + slot_size * slot_count);

	comm_status0 = 0xff01;	/* But 1 at connect time before f000 is read */
	comm_status1 = (u16)((slot_count << 8) | slot_id);
}

/* Emulation thread: bring comm_ram up to date with the network before the
 * game reads or writes it. */
static void sync_network(void)
{
	u32 slot_size, packet_size;
	u16 packet_number;
	int state = retro_atomic_load_acquire_int(&net_state);

	if (state == NET_DOWN)
		return;
	if (state == NET_UP)
	{
		slot_count = retro_atomic_load_acquire_int(&net_slot_count);
		slot_id = retro_atomic_load_acquire_int(&net_slot_id);
		connected_state();
		retro_atomic_store_release_int(&net_state, NET_SEEN);
	}

	slot_size = game_slot_size();
	packet_size = slot_size * slot_count;

	if (packet_size != 0 && slot_size + packet_size <= PACKET_MAX && tribuf_take(&rx))
		memcpy(&comm_ram[0x100 + slot_size], rx_buf[tribuf_front(&rx)], packet_size);

	packet_number = (u16)retro_atomic_load_acquire_int(&net_packet_number);
	if (packet_number != seen_packet_number)
	{
		seen_packet_number = packet_number;
		*(u16 *)&comm_ram[6] = swap16(packet_number);
	}
}

/* Emulation thread: the game has written its slot; pass it to the network
 * thread. */
static void publish_slot(void)
{
	const u32 slot_size = MIN((u32)game_slot_size(), (u32)SLOT_MAX);

	memcpy(tx_buf[tribuf_back(&tx)], &comm_ram[0x100], slot_size);
	tribuf_publish(&tx);
}

u32 m3comm_read(u32 address, u32 size)
{
	switch (address & 255)
	{
	case NAOMI_COMM2_CTRL_addr & 255:
		return comm_ctrl;

	case NAOMI_COMM2_OFFSET_addr & 255:
		return comm_offset;

	case NAOMI_COMM2_DATA_addr & 255:
	{
		u16 value;
		if (comm_ctrl & 1)
			value = *(u16 *)&m68k_ram[comm_offset];
		else
		{
			sync_network();
			value = *(u16 *)&comm_ram[comm_offset];
		}
		value = swap16(value);
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_DATA %s read @ %04x: %x\n", (comm_ctrl & 1) ? "m68k ram" : "comm ram", comm_offset, value);
		comm_offset += 2;
		return value;
	}

	case NAOMI_COMM2_STATUS0_addr & 255:
		sync_network();
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_STATUS0 read %x\n", comm_status0);
		return comm_status0;

	case NAOMI_COMM2_STATUS1_addr & 255:
		sync_network();
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_STATUS1 read %x\n", comm_status1);
		return comm_status1;

	default:
		DEBUG_LOG(NAOMI, "m3comm_read unmapped: %08x sz %d\n", address, size);
		return 0xffffffff;
	}
}

static void network_thread(void *unused)
{
	retro_time_t token_time;

	connect_network();

	token_time = cpu_features_get_time_usec();
	while (!retro_atomic_load_acquire_int(&network_stopping))
	{
		naomi_network_pipe_slaves(&network);
		receive_network();

		if (net_thread_slot_id == 0 && network.got_token)
		{
			/* The master lets the token round the ring a hundred times a
			 * second at most: it holds it until ten milliseconds have
			 * passed since it last sent it on, or until it is told to
			 * stop. */
			const retro_time_t held = cpu_features_get_time_usec() - token_time;
			if (held < 10000)
				naomi_network_wait_stop(&network, 10000 - held);
			token_time = cpu_features_get_time_usec();
		}

		send_network();

		/* Without the token there is nothing to do until a packet comes
		 * in: wait for one, instead of going round again to look. With it
		 * still in hand the send did not go through; give the socket a
		 * moment. */
		if (!network.got_token)
			naomi_network_wait_for_data(&network);
		else if (net_thread_slot_id != 0)
			naomi_network_wait_stop(&network, 1000);
	}
	DEBUG_LOG(NAOMI, "Network thread exiting\n");
}

static void start_thread(void)
{
	retro_atomic_int_init(&network_stopping, 0);
	/* No network thread at this point: set up what it shares with this one */
	tribuf_reset(&rx);
	tribuf_reset(&tx);
	memset(net_ring, 0, sizeof(net_ring));
	net_thread_slot_count = 0;
	net_thread_slot_id = 0;
	seen_packet_number = 0;
	retro_atomic_int_init(&net_state, NET_DOWN);
	retro_atomic_int_init(&net_slot_count, 0);
	retro_atomic_int_init(&net_slot_id, 0);
	retro_atomic_int_init(&net_packet_number, 0);
	retro_atomic_int_init(&net_slot_size, game_slot_size());
	publish_slot();
	/* A link needs a board that serves it, or the address of one that
	 * does, and nothing sets either: with neither the network thread could
	 * only ask the local network, by broadcast and for ten seconds, for a
	 * server no machine running this core can be. The board stays as it is
	 * without a link, which is what the game saw at the end of that. */
	if (!settings.network.ActAsServer && (settings.network.server[0] == 0))
		return;
	/* The socket shutdown ends the network thread's waits with */
	naomi_network_arm_wake(net());
	thread = sthread_create(network_thread, NULL);
}

void m3comm_write(u32 address, u32 data, u32 size)
{
	switch (address & 255)
	{
	case NAOMI_COMM2_CTRL_addr & 255:
		/* bit 0: access RAM is 0 - communication RAM / 1 - M68K RAM
		 * bit 1: comm RAM bank (seems R/O for SH4)
		 * bit 5: M68K Reset
		 * bit 6: ???
		 * bit 7: might be M68K IRQ 5 or 2
		 * bit 14: G1 DMA bus master 0 - active / 1 - disabled
		 * bit 15: 0 - enable / 1 - disable this device ??? */
		if (data & (1 << 5))
		{
			DEBUG_LOG(NAOMI, "NAOMI_COMM2_CTRL m68k reset\n");
			m3comm_close_network();
			memset(&comm_ram[0], 0, 32);
			comm_status0 = 0; /* varies... */
			comm_status1 = 0;
			start_thread();
		}
		comm_ctrl = (u16)(data & ~(1 << 5));
		return;

	case NAOMI_COMM2_OFFSET_addr & 255:
		comm_offset = (u16)data;
		return;

	case NAOMI_COMM2_DATA_addr & 255:
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_DATA written @ %04x %04x\n", comm_offset, (u16)data);
		data = swap16((u16)data);
		if (comm_ctrl & 1)
		{
			*(u16 *)&m68k_ram[comm_offset] = (u16)data;
			/* The slot size, which the network thread goes by as well */
			if ((comm_offset & ~1) == 0x204)
				retro_atomic_store_release_int(&net_slot_size, game_slot_size());
		}
		else
		{
			sync_network();
			*(u16 *)&comm_ram[comm_offset] = (u16)data;
			/* (the slot: from 0x100 up, as far as an offset goes) */
			if (comm_offset >= 0x100)
				publish_slot();
		}
		comm_offset += 2;
		return;

	case NAOMI_COMM2_STATUS0_addr & 255:
		comm_status0 = (u16)data;
		return;

	case NAOMI_COMM2_STATUS1_addr & 255:
		comm_status1 = (u16)data;
		return;

	default:
		break;
	}
	DEBUG_LOG(NAOMI, "m3comm_write: %x <= %x sz %d\n", address, data, size);
}

bool m3comm_dma_start(u32 addr, u32 data)
{
	u32 i;
	if (comm_ctrl & 0x4000)
		return false;

	DEBUG_LOG(NAOMI, "NaomiM3Comm: DMA addr %08X <-> %04x len %d %s\n", SB_GDSTAR, comm_offset, SB_GDLEN, SB_GDDIR == 0 ? "OUT" : "IN");
	sync_network();
	if (SB_GDDIR == 0)
	{
		/* Network write */
		for (i = 0; i < SB_GDLEN; i++)
			comm_ram[comm_offset++] = ReadMem8_nommu(SB_GDSTAR + i);
		publish_slot();
	}
	else
	{
		/* Network read */
		if (SB_GDLEN == 32 && (comm_ctrl & 1) == 0)
		{
			char buf[32 * 5 + 1];
			buf[0] = 0;
			for (i = 0; i < SB_GDLEN; i++)
			{
				u8 value = comm_ram[comm_offset + i];
				sprintf(buf + strlen(buf), "%02x ", value);
			}
			DEBUG_LOG(NAOMI, "Comm RAM read @%x: %s\n", comm_offset, buf);
		}
		for (i = 0; i < SB_GDLEN; i++)
			WriteMem8_nommu(SB_GDSTAR + i, comm_ram[comm_offset++]);
	}
	return true;
}
