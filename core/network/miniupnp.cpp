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

bool MiniUPnP::Init()
{
	char wan[64];
	found = false;
	count = 0;
	if (!network_init() || !net_upnp_discover(&gateway, UPNP_TIMEOUT_MS))
	{
		INFO_LOG(MODEM, "UPnP: no Internet gateway found");
		return false;
	}
	found = true;
	if (net_upnp_external_address(&gateway, wan, sizeof(wan), UPNP_TIMEOUT_MS))
		INFO_LOG(MODEM, "UPnP: this host %s, external address %s", gateway.lan_addr, wan);
	else
		INFO_LOG(MODEM, "Cannot determine external IP address");
	return true;
}

void MiniUPnP::Term()
{
	unsigned i;
	if (found)
		for (i = 0; i < count; i++)
			net_upnp_delete_port_mapping(&gateway, mapped[i].port, !mapped[i].tcp, UPNP_TIMEOUT_MS);
	count = 0;
	found = false;
}

bool MiniUPnP::AddPortMapping(int port, bool tcp)
{
	if (!found || port <= 0 || count == MOST)
		return false;
	if (!net_upnp_add_port_mapping(&gateway, (unsigned)port, !tcp, "Flycast", UPNP_TIMEOUT_MS))
	{
		INFO_LOG(MODEM, "Port %d redirection failed", port);
		return false;
	}
	mapped[count].port = (unsigned)port;
	mapped[count].tcp = tcp;
	count++;
	return true;
}
