/*
	Copyright 2020 flyinghead

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
#pragma once
#include <boolean.h>
#include <retro_common_api.h>
#include <net/net_upnp.h>

RETRO_BEGIN_DECLS

/* The gateway's ports forwarded to this host while the network is up, and
 * given back when it goes (libretro-common's net/net_upnp.h).
 * miniupnp_init() and miniupnp_add_port_mapping() wait on the network, up
 * to a few seconds: they are called off the emulation thread. A miniupnp_t
 * starts zeroed. */
#define MINIUPNP_MOST 32

typedef struct miniupnp
{
	struct net_upnp_gateway gateway;
	bool found;
	unsigned count;
	struct
	{
		unsigned port;
		bool tcp;
	} mapped[MINIUPNP_MOST];
} miniupnp_t;

/* The gateway looked for: false when there is none */
bool miniupnp_init(miniupnp_t *u);
/* The ports mapped given back */
void miniupnp_term(miniupnp_t *u);
bool miniupnp_add_port_mapping(miniupnp_t *u, int port, bool tcp);

RETRO_END_DECLS
