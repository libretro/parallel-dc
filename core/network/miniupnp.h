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
#include <net/net_upnp.h>

/* The gateway's ports forwarded to this host while the network is up, and
 * given back when it goes (libretro-common's net/net_upnp.h). Init() and
 * AddPortMapping() wait on the network, up to a few seconds: they are
 * called off the emulation thread. */
class MiniUPnP
{
public:
	MiniUPnP() : found(false), count(0) {}
	bool Init();
	void Term();
	bool AddPortMapping(int port, bool tcp);

private:
	enum { MOST = 32 };
	struct Mapping
	{
		unsigned port;
		bool tcp;
	};
	struct net_upnp_gateway gateway;
	bool found;
	unsigned count;
	Mapping mapped[MOST];
};
