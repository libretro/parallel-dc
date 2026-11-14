/*
	Created on: Apr 12, 2020
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
#pragma once
#include "types.h"
#include <retro_atomic.h>
#include "net_platform.h"
#include "miniupnp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The link between NAOMI boards: a ring of TCP connections, the master
 * (slot 0) the server the others connect to. The packets go round it with
 * a token: the board that has it sends, the next receives. */

#define NAOMI_NET_SERVER_PORT 37391
/* (the master and up to three slaves) */
#define NAOMI_NET_SLAVES_MAX 3

typedef struct naomi_network
{
	struct in_addr server_ip;
	char server_name[INET_ADDRSTRLEN];
	/* server */
	sock_t server_sock;
	sock_t beacon_sock;
	sock_t slaves[NAOMI_NET_SLAVES_MAX];
	int slave_count;
	/* client */
	sock_t client_sock;
	/* both */
	int slot_count;
	int slot_id;
	bool got_token;
	u16 packet_number;
	retro_atomic_int_t network_stopping;
	sock_t wake_sock;
	miniupnp_t miniupnp;
} naomi_network;

/* As one is made: no sockets, no server known */
void naomi_network_create(naomi_network *net);
bool naomi_network_init(naomi_network *net);
bool naomi_network_start(naomi_network *net);
void naomi_network_pipe_slaves(naomi_network *net);
bool naomi_network_receive(naomi_network *net, u8 *data, u32 size);
void naomi_network_send(naomi_network *net, const u8 *data, u32 size);
/* The sockets belong to the thread that runs naomi_network_start(),
 * _receive() and _send(). Another thread stops it with _shutdown(), which
 * only raises a flag that thread looks at wherever it waits, and closes the
 * sockets with _close_sockets() or _terminate() once that thread is gone. */
void naomi_network_shutdown(naomi_network *net);
void naomi_network_close_sockets(naomi_network *net);
void naomi_network_terminate(naomi_network *net);

/* The thread that owns the sockets never sleeps for a fixed time and never
 * spins. Wherever it has to wait it waits in select() for the sockets it
 * is waiting on, together with a socket _shutdown() writes to, so a wait
 * ends the moment there is something to do or the network is told to stop.
 *
 * _arm_wake() makes that socket; call it before the thread is started. */
bool naomi_network_arm_wake(naomi_network *net);
/* Until one of the ring's sockets has something to read. */
void naomi_network_wait_for_data(naomi_network *net);
/* For @usec, or until _shutdown(). */
void naomi_network_wait_stop(naomi_network *net, int64_t usec);

#ifdef __cplusplus
}
#endif
