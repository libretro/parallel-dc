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
#include <net/net_compat.h>
#include "types.h"
#include "miniupnp.h"

/* how long to wait for the gateway, and for each of its answers */
#define UPNP_TIMEOUT_MS 2000

bool miniupnp_init(miniupnp_t *u)
{
	char wan[64];
	u->found = false;
	u->count = 0;
	if (!network_init() || !net_upnp_discover(&u->gateway, UPNP_TIMEOUT_MS))
	{
		INFO_LOG(MODEM, "UPnP: no Internet gateway found\n");
		return false;
	}
	u->found = true;
	if (net_upnp_external_address(&u->gateway, wan, sizeof(wan), UPNP_TIMEOUT_MS))
		INFO_LOG(MODEM, "UPnP: this host %s, external address %s\n", u->gateway.lan_addr, wan);
	else
		INFO_LOG(MODEM, "Cannot determine external IP address\n");
	return true;
}

void miniupnp_term(miniupnp_t *u)
{
	unsigned i;
	if (u->found)
		for (i = 0; i < u->count; i++)
			net_upnp_delete_port_mapping(&u->gateway, u->mapped[i].port, !u->mapped[i].tcp, UPNP_TIMEOUT_MS);
	u->count = 0;
	u->found = false;
}

bool miniupnp_add_port_mapping(miniupnp_t *u, int port, bool tcp)
{
	if (!u->found || port <= 0 || u->count == MINIUPNP_MOST)
		return false;
	if (!net_upnp_add_port_mapping(&u->gateway, (unsigned)port, !tcp, "Flycast", UPNP_TIMEOUT_MS))
	{
		INFO_LOG(MODEM, "Port %d redirection failed\n", port);
		return false;
	}
	u->mapped[u->count].port = (unsigned)port;
	u->mapped[u->count].tcp = tcp;
	u->count++;
	return true;
}
