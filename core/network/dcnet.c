/*
	Copyright 2025 flyinghead

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
/* DCNet: the Dreamcast's line goes over the Internet to a relay of the
 * community's servers, which answers the modem's PPP or the adapter's
 * Ethernet itself, so that nothing of the network is emulated here.
 *
 * It runs on the network thread in place of picoTCP, with the same rings
 * to the emulated modem and adapter, and waits the same way: in select(),
 * on its sockets and on the emulation thread's wake socket. Its sockets
 * do not block. */
/* getaddrinfo() and struct addrinfo, which a C89 compile of the C
 * library's headers leaves out */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <errno.h>
#include <string.h>
#include "types.h"
#include "netthread.h"
#include "picoppp.h"
#include "reios/reios.h"
#include <libretro.h>
#include <features/features_cpu.h>

#if !defined(_MSC_VER) && !defined(TARGET_NO_THREADS)

#define DCNET_HOST "dcnet.flyca.st"
#define PPP_PORT 7654
#define TAP_PORT 7655
#define POWER_SMASH_PPP_PORT 7656
/* Access points: the main server lists them, and the one that answers
 * fastest is used */
#define AP_PORT 7655
/* 0xDC15C001, as the other clients send it */
static const u8 AP_MAGIC[4] = { 0x01, 0xc0, 0x15, 0xdc };
enum { AP_PING = 1, AP_PONG = 2, AP_DISCOVER = 3 };
/* The list comes in one datagram: an address (4), a name's length (1) and
 * the name each */
#define AP_DATAGRAM_MAX 512
#define AP_MAX ((AP_DATAGRAM_MAX - 5) / 5)

#define MAX_ETH_FRAME_SIZE 1514
/* What waits for the socket, or for the modem or the adapter, at most */
#define MAX_PENDING (64 * 1024)
#define RECV_SIZE 2048
/* A Power Smash frame without its flag yet is passed on as it is from this
 * size */
#define SMASH_UNFRAMED_MAX 1542
/* What is received is kept until there is all of a frame: of the adapter's
 * (2 + MAX_ETH_FRAME_SIZE), or of Power Smash's */
#define FROM_NET_MAX (SMASH_UNFRAMED_MAX + RECV_SIZE)
/* For the modem: what it had no room for, or Power Smash's frames with a
 * flag put before them (one more for two bytes at most) */
#define TO_MODEM_MAX (2 * FROM_NET_MAX)

typedef struct
{
	u32 addr;		/* network order */
	u16 name;		/* in the list's datagram */
	u8 name_len;
	int ping;
	int count;
} access_point;

/* The network thread's own: it runs one at a time */
static u8 ap_list[AP_DATAGRAM_MAX];
static access_point aps[AP_MAX];
static u8 to_net[MAX_PENDING];
static u8 from_net[FROM_NET_MAX];
static u8 to_modem[TO_MODEM_MAX];
static u8 scratch[RECV_SIZE];
static u8 eth_frame[NET_ETH_FRAME_MAX];
static u8 held_frame[NET_ETH_FRAME_MAX];

static u64 now_ms(void)
{
	return (u64)cpu_features_get_time_usec() / 1000;
}

static void close_socket(sock_t *s)
{
	if (VALID(*s))
		closesocket(*s);
	*s = INVALID_SOCKET;
}

static void send_ping(sock_t s, u32 addr)
{
	u8 buf[13];
	struct sockaddr_in to;
	const u64 now = now_ms();

	memcpy(&buf[0], AP_MAGIC, 4);
	buf[4] = AP_PING;
	memcpy(&buf[5], &now, sizeof(now));		/* given back as it is */
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(AP_PORT);
	to.sin_addr.s_addr = addr;
	sendto(s, (const char *)buf, sizeof(buf), MSG_NOSIGNAL, (const struct sockaddr *)&to, sizeof(to));
}

/* The address to connect to (network order): the access point that answers
 * fastest, the main server if it lists none, 0 if there is no server. A
 * second at most. The access point's name in @name (none: NULL), until the
 * next call. */
static u32 find_access_point(const char **name, int *name_len)
{
	u8 buf[AP_DATAGRAM_MAX];
	u8 discover[5];
	struct sockaddr_in to;
	const u32 main_addr = netthread_resolve(DCNET_HOST);
	sock_t s;
	unsigned count = 0, i;
	bool listed = false, pinged_again = false, done = false;
	u64 start;
	const access_point *best = NULL;
	int best_ping = 1000000;

	*name = NULL;
	*name_len = 0;
	if (main_addr == 0)
	{
		WARN_LOG(MODEM, "DCNet: can't resolve %s\n", DCNET_HOST);
		return 0;
	}
	s = selectable_socket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
	if (!VALID(s))
		return main_addr;
	set_non_blocking(s);

	memcpy(discover, AP_MAGIC, 4);
	discover[4] = AP_DISCOVER;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(AP_PORT);
	to.sin_addr.s_addr = main_addr;
	sendto(s, (const char *)discover, sizeof(discover), MSG_NOSIGNAL, (const struct sockaddr *)&to, sizeof(to));

	start = now_ms();
	while (!done && netthread_running())
	{
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		const int kicks = netthread_kicks();
		const u64 elapsed = now_ms() - start;
		int len;
		if (elapsed >= 1000)
			break;
		if (elapsed >= 500 && !pinged_again)
		{
			/* Again those that have not answered */
			pinged_again = true;
			for (i = 0; i < count; i++)
				if (aps[i].count == 0)
					send_ping(s, aps[i].addr);
		}
		len = (int)recvfrom(s, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
		if (len < 0)
		{
			fd_set readable, writable;
			const u64 phase_end = pinged_again ? 1000 : 500;
			FD_ZERO(&readable);
			FD_ZERO(&writable);
			FD_SET(s, &readable);
			netthread_wait(kicks, &readable, &writable, (int)s, (int)(phase_end - elapsed));
			continue;
		}
		if (len < 5 || from.sin_port != htons(AP_PORT))
			continue;
		if (!listed && buf[4] == AP_DISCOVER && from.sin_addr.s_addr == main_addr
				&& memcmp(buf, AP_MAGIC, 4) == 0)
		{
			/* address (4), name length (1), name: as many as the datagram holds */
			int p;
			listed = true;
			memcpy(ap_list, buf, len);
			for (p = 5; p + 5 <= len && count < AP_MAX; )
			{
				access_point *ap = &aps[count];
				const int l = ap_list[p + 4];
				if (p + 5 + l > len)
					break;
				memcpy(&ap->addr, &ap_list[p], 4);
				ap->name = (u16)(p + 5);
				ap->name_len = (u8)l;
				ap->ping = 0;
				ap->count = 0;
				count++;
				p += 5 + l;
			}
			if (count > 1)
			{
				for (i = 0; i < count; i++)
					send_ping(s, aps[i].addr);
			}
			else
				done = true;
		}
		else if (len == 13 && buf[4] == AP_PONG)
		{
			u64 ts;
			int ping;
			memcpy(&ts, &buf[5], sizeof(ts));
			ping = (int)(now_ms() - ts);
			for (i = 0; i < count; i++)
			{
				access_point *ap = &aps[i];
				if (ap->addr != from.sin_addr.s_addr)
					continue;
				ap->ping += ping;
				ap->count++;
				if (ap->count < 5)
					send_ping(s, ap->addr);
				else
					/* five answers from one access point: enough */
					done = true;
				break;
			}
		}
	}
	closesocket(s);

	if (count == 0)
		return main_addr;
	for (i = 0; i < count; i++)
	{
		const access_point *ap = &aps[i];
		int ping;
		if (ap->count == 0)
		{
			INFO_LOG(MODEM, "DCNet AP %.*s: no answer\n", ap->name_len, (const char *)&ap_list[ap->name]);
			continue;
		}
		ping = ap->ping / ap->count;
		INFO_LOG(MODEM, "DCNet AP %.*s: ping %d ms (%d pings)\n", ap->name_len, (const char *)&ap_list[ap->name], ping, ap->count);
		if (ping < best_ping)
		{
			best_ping = ping;
			best = ap;
		}
	}
	if (best == NULL)
		best = &aps[0];
	*name = (const char *)&ap_list[best->name];
	*name_len = best->name_len;
	return best->addr;
}

/* A connection to @port of the access point, INVALID_SOCKET if none can be
 * had */
static sock_t dcnet_connect(u16 port)
{
	const char *ap_name;
	int ap_name_len;
	const u32 addr = find_access_point(&ap_name, &ap_name_len);
	sock_t s;
	struct sockaddr_in to;
	u64 start;
#ifdef _WIN32
	char value = 0;
#else
	int value = 0;
#endif
	socklen_t l = sizeof(value);

	if (addr == 0 || !netthread_running())
		return INVALID_SOCKET;

	s = selectable_socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
	if (!VALID(s))
		return INVALID_SOCKET;
	set_non_blocking(s);
	set_no_sigpipe(s);
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	to.sin_addr.s_addr = addr;
	if (connect(s, (const struct sockaddr *)&to, sizeof(to)) < 0
			&& get_last_error() != EINPROGRESS && get_last_error() != L_EWOULDBLOCK)
	{
		perror("DCNet connect");
		closesocket(s);
		return INVALID_SOCKET;
	}
	/* Ten seconds at most, and no longer than the thread is wanted */
	start = now_ms();
	for (;;)
	{
		fd_set readable, writable, failed;
		struct timeval tv;
		const int kicks = netthread_kicks();
		if (!netthread_running() || now_ms() - start >= 10000)
		{
			if (netthread_running())
				WARN_LOG(MODEM, "DCNet: no connection after 10 s\n");
			closesocket(s);
			return INVALID_SOCKET;
		}
		FD_ZERO(&readable);
		FD_ZERO(&writable);
		FD_ZERO(&failed);
		FD_SET(s, &writable);
		FD_SET(s, &failed);
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		if (select((int)s + 1, NULL, &writable, &failed, &tv) > 0)
			break;
		FD_ZERO(&writable);
		FD_SET(s, &writable);
		netthread_wait(kicks, &readable, &writable, (int)s, 100);
	}
	if (getsockopt(s, SOL_SOCKET, SO_ERROR, &value, &l) < 0 || value != 0)
	{
		if (ap_name)
			WARN_LOG(MODEM, "DCNet: connection to %.*s refused\n", ap_name_len, ap_name);
		else
			WARN_LOG(MODEM, "DCNet: connection to %s refused\n", DCNET_HOST);
		closesocket(s);
		return INVALID_SOCKET;
	}
	set_tcp_nodelay(s);
	if (ap_name)
		NOTICE_LOG(MODEM, "Connected to DCNet via %.*s\n", ap_name_len, ap_name);
	else
		NOTICE_LOG(MODEM, "Connected to DCNet\n");
	return s;
}

/* A DHCP release from the Dreamcast: the game is done with the network
 * (the connection is closed; its next frame opens another) */
static bool is_dhcp_release(const u8 *f, u32 len)
{
	u32 i;
	if (len < 0x11d
			|| f[0xc] != 0x08 || f[0xd] != 0x00	/* type: IPv4 */
			|| f[0x17] != 0x11					/* UDP */
			|| ((f[0x22] << 8) | f[0x23]) != 68	/* src port: dhcpc */
			|| ((f[0x24] << 8) | f[0x25]) != 67)	/* dest port: dhcps */
		return false;
	/* The options, after the magic cookie */
	for (i = 0x11a; i < len && f[i] != 0xff; )
	{
		if (f[i] == 0)
		{
			/* pad */
			i++;
			continue;
		}
		if (i + 2 >= len)
			break;
		if (f[i] == 53 && f[i + 2] == 7)	/* message type: release */
			return true;
		i += f[i + 1] + 2;
	}
	return false;
}

/* @len bytes of @buf after the @*used bytes of @dst, which has room for
 * them */
static void append(u8 *dst, size_t *used, const u8 *buf, size_t len)
{
	memcpy(dst + *used, buf, len);
	*used += len;
}

/* The bytes to send: from @to_net_head, @to_net_len of them. They are moved
 * to the start when there is not room after them for @len more. */
static size_t to_net_head, to_net_len;

static void to_net_add(const u8 *buf, size_t len)
{
	if (to_net_head + to_net_len + len > sizeof(to_net))
	{
		memmove(to_net, to_net + to_net_head, to_net_len);
		to_net_head = 0;
	}
	memcpy(to_net + to_net_head + to_net_len, buf, len);
	to_net_len += len;
}

static void to_net_add_frame(const u8 *frame, u32 size)
{
	u8 len[2];
	len[0] = (u8)size;
	len[1] = (u8)(size >> 8);
	to_net_add(len, 2);
	to_net_add(frame, size);
}

void dcnet_run(bool bba)
{
	char game_id[sizeof(ip_meta.product_number) + 1];
	bool power_smash;
	u16 port;
	sock_t sock = INVALID_SOCKET;
	/* The modem's line is made once: when it is lost, it is lost until the
	 * game dials again */
	bool line_lost = false;
	/* The adapter's connection is opened by a frame of the game's, kept here
	 * meanwhile */
	u32 held_size = 0;
	u64 last_attempt = 0;
	bool attempted = false;
	/* from the socket: frames (adapter) or bytes to frame (Power Smash) */
	size_t from_net_len = 0, from_net_pos = 0;
	/* for the modem, when its ring is full: from @to_modem_head */
	size_t to_modem_head = 0, to_modem_len = 0;

	ip_meta_product_id(&ip_meta, game_id, NULL);
	/* These games want their PPP frames each with a flag at both ends, which
	 * a server on its own port sees to */
	power_smash = !bba
			&& (strcmp(game_id, "HDR-0113") == 0		/* Power Smash */
				|| strcmp(game_id, "HDR-0091") == 0		/* Pro Yakyuu Team de Asobou Net! */
				|| strcmp(game_id, "HDR-0040") == 0		/* Virtual-On Oratorio Tangram */
				|| strcmp(game_id, "T34201M") == 0);	/* Frame Gride */
	port = bba ? TAP_PORT : power_smash ? POWER_SMASH_PPP_PORT : PPP_PORT;
	to_net_head = to_net_len = 0;

	while (netthread_running())
	{
		const int kicks = netthread_kicks();
		bool closing = false, room;
		fd_set readable, writable;

		if (!VALID(sock))
		{
			if (!bba)
			{
				if (line_lost)
				{
					/* What the modem sends goes nowhere */
					while (netthread_modem_take(scratch, 512) != 0)
						;
				}
				else
				{
					sock = dcnet_connect(port);
					if (!VALID(sock))
					{
						line_lost = true;
						continue;
					}
				}
			}
			else
			{
				if (held_size == 0)
					held_size = netthread_eth_take(held_frame);
				if (held_size != 0 && is_dhcp_release(held_frame, held_size))
					held_size = 0;
				/* One attempt each five seconds at most: the frames in
				 * between are lost */
				if (held_size != 0 && attempted && now_ms() - last_attempt < 5000)
					held_size = 0;
				if (held_size != 0)
				{
					attempted = true;
					last_attempt = now_ms();
					sock = dcnet_connect(port);
					if (VALID(sock))
					{
						static const u8 prolog[] = { 'D', 'C', 'N', 'E', 'T', 1 };
						to_net_head = to_net_len = 0;
						from_net_len = from_net_pos = 0;
						to_net_add_frame(prolog, sizeof(prolog));
						if (held_size <= MAX_ETH_FRAME_SIZE)
							to_net_add_frame(held_frame, held_size);
					}
					held_size = 0;
					continue;
				}
			}
			if (!VALID(sock))
			{
				FD_ZERO(&readable);
				FD_ZERO(&writable);
				netthread_wait(kicks, &readable, &writable, -1, 1000);
				continue;
			}
		}

		/* To the network */
		if (bba)
		{
			while (to_net_len + 2 + NET_ETH_FRAME_MAX <= MAX_PENDING)
			{
				const u32 size = netthread_eth_take(eth_frame);
				if (size == 0)
					break;
				if (size > MAX_ETH_FRAME_SIZE)
					continue;
				if (is_dhcp_release(eth_frame, size))
				{
					INFO_LOG(MODEM, "DCNet: DHCP release\n");
					closing = true;
					break;
				}
				to_net_add_frame(eth_frame, size);
			}
		}
		else
		{
			while (to_net_len < MAX_PENDING)
			{
				size_t want = MAX_PENDING - to_net_len, n;
				if (want > sizeof(scratch))
					want = sizeof(scratch);
				n = netthread_modem_take(scratch, want);
				if (n == 0)
					break;
				to_net_add(scratch, n);
			}
		}
		if (to_net_len != 0)
		{
			const int r = (int)send(sock, (const char *)to_net + to_net_head, to_net_len, MSG_NOSIGNAL);
			if (r < 0)
			{
				if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
				{
					perror("DCNet send");
					closing = true;
				}
			}
			else
			{
				to_net_head += r;
				to_net_len -= r;
				if (to_net_len == 0)
					to_net_head = 0;
			}
		}

		/* From the network: what was received before first */
		if (!bba)
		{
			if (to_modem_len != 0)
			{
				const size_t n = netthread_modem_give(to_modem + to_modem_head, to_modem_len);
				to_modem_head += n;
				to_modem_len -= n;
				if (to_modem_len == 0)
					to_modem_head = 0;
			}
		}
		else
		{
			while (from_net_len - from_net_pos >= 2)
			{
				const u32 size = from_net[from_net_pos] | (from_net[from_net_pos + 1] << 8);
				if (size > MAX_ETH_FRAME_SIZE)
				{
					ERROR_LOG(MODEM, "DCNet: invalid Ethernet frame size: %d\n", (int)size);
					closing = true;
					break;
				}
				if (from_net_len - from_net_pos < 2 + size)
					break;
				if (!netthread_eth_give(&from_net[from_net_pos + 2], size))
					break;
				from_net_pos += 2 + size;
			}
			if (from_net_pos != 0)
			{
				from_net_len -= from_net_pos;
				memmove(from_net, from_net + from_net_pos, from_net_len);
				from_net_pos = 0;
			}
		}
		/* Room for more: everything received has gone, but a part of a frame */
		room = !closing && to_modem_len == 0
				&& (!bba || from_net_len < 2 + MAX_ETH_FRAME_SIZE);
		if (room)
		{
			const int r = (int)recv(sock, (char *)scratch, sizeof(scratch), 0);
			if (r == 0)
			{
				INFO_LOG(MODEM, "DCNet: connection closed by the server\n");
				closing = true;
			}
			else if (r < 0)
			{
				if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
				{
					perror("DCNet recv");
					closing = true;
				}
			}
			else if (bba)
				append(from_net, &from_net_len, scratch, r);
			else if (!power_smash)
			{
				const size_t n = netthread_modem_give(scratch, r);
				append(to_modem, &to_modem_len, scratch + n, r - n);
			}
			else
			{
				/* Each frame ends with its flag: one is put before it too,
				 * if it has none */
				size_t start = 0, i;
				append(from_net, &from_net_len, scratch, r);
				for (i = 1; i < from_net_len; i++)
				{
					if (from_net[i] != '~')
						continue;
					if (from_net[start] != '~')
						to_modem[to_modem_len++] = '~';
					append(to_modem, &to_modem_len, from_net + start, i + 1 - start);
					start = i + 1;
					i++;
				}
				from_net_len -= start;
				memmove(from_net, from_net + start, from_net_len);
				if (from_net_len >= SMASH_UNFRAMED_MAX)
				{
					/* no flag in sight: as it is */
					append(to_modem, &to_modem_len, from_net, from_net_len);
					from_net_len = 0;
				}
				if (to_modem_len != 0)
				{
					const size_t n = netthread_modem_give(to_modem, to_modem_len);
					to_modem_head = n;
					to_modem_len -= n;
					if (to_modem_len == 0)
						to_modem_head = 0;
				}
			}
			if (r > 0)
				/* there may be more */
				continue;
		}
		if (closing)
		{
			close_socket(&sock);
			to_net_head = to_net_len = 0;
			from_net_len = from_net_pos = 0;
			to_modem_head = to_modem_len = 0;
			if (!bba)
				line_lost = true;
			continue;
		}

		FD_ZERO(&readable);
		FD_ZERO(&writable);
		if (room)
			FD_SET(sock, &readable);
		if (to_net_len != 0)
			FD_SET(sock, &writable);
		netthread_wait(kicks, &readable, &writable, (int)sock, 1000);
	}
	close_socket(&sock);
}

#endif
