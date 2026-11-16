/*
	Created on: Sep 15, 2018

	Copyright 2018 flyinghead

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

#if defined(_WIN32) && !defined(FD_SETSIZE)
/* The pico thread waits on all of its sockets at once; Winsock's own
 * limit for a select() is 64. */
#define FD_SETSIZE 256
#endif
#if !defined(_MSC_VER) && !defined(TARGET_NO_THREADS)

#ifdef __MINGW32__
#define _POSIX_SOURCE
#endif
/* getaddrinfo(), struct addrinfo and the timers, which a C89 compile of
 * the C library's headers leaves out */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "hw/sh4/sh4_sched.h"

#ifdef __LIBRETRO__
#include <retro_timers.h>
#endif
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>

/* (picoTCP's headers have C99's inline; C89 has it as __inline) */
#if !defined(__cplusplus) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L) && !defined(inline)
#define PICOPPP_INLINE
#define inline __inline
#endif
#include <pico_stack.h>
#include <pico_dev_ppp.h>
#include <pico_socket.h>
#include <pico_socket_tcp.h>
#include <pico_ipv4.h>
#include <pico_tcp.h>
#include <pico_dhcp_server.h>
#ifdef PICOPPP_INLINE
#undef inline
#undef PICOPPP_INLINE
#endif

#include "network/net_platform.h"
#include "hw/modem/dns.h"

#include "picoppp.h"
#include "netservice.h"
#include "netthread.h"
#include "network/miniupnp.h"
#include "reios/reios.h"

#define RESOLVER1_OPENDNS_COM "208.67.222.222"
#define AFO_ORIG_IP 0x83f2fb3f		/* 63.251.242.131 in network order */
#define IGP_ORIG_IP 0xef2bd2cc		/* 204.210.43.239 in network order */

static struct pico_device *pico_dev;

/* Bytes between the emulated modem (emulation thread) and the PPP stack
 * (pico thread). Each ring has one writer and one reader and takes no
 * lock. in_ring is kept small on purpose: when it fills, the stack waits
 * for the modem to read, which is the flow control the link needs. */
#define IN_RING_BYTES  1024
#define OUT_RING_BYTES 16384
static retro_spsc_t in_ring;	/* stack -> modem */
static retro_spsc_t out_ring;	/* modem -> stack */
static bool rings_ready;	/* emulation thread only */
/* Where the stack sleeps while in_ring is full (made with the rings) */
static retro_eventcount_t in_ring_ec;
/* Set by a network thread that does not sleep on in_ring_ec (DCNet) when
 * in_ring had no room: the modem wakes it once it has read some. */
static retro_atomic_int_t in_ring_blocked;

/* Ethernet frames, for the broadband adapter. The adapter is emulated on
 * the emulation thread and the stack runs on the pico thread, and neither
 * may call into the other: the adapter's registers, its receive ring in
 * guest memory and its interrupt belong to one, the stack's queues to the
 * other. So a frame crosses as bytes in a ring, a 16-bit length and then
 * the frame, written whole or not at all. */
#define ETH_RING_BYTES 65536
#define ETH_FRAME_MAX NET_ETH_FRAME_MAX
static retro_spsc_t eth_out_ring;	/* adapter -> stack */
static retro_spsc_t eth_in_ring;	/* stack -> adapter */
/* Set by the pico thread when the adapter's ring had no room for a frame;
 * the emulation thread wakes it once it has taken some out. */
static retro_atomic_int_t eth_in_blocked;

/* What the thread is for, set before it starts: the broadband adapter
 * rather than the modem, DCNet rather than picoTCP. */
static bool thread_bba;
static bool thread_dcnet;
/* PPP: the stack says nothing until the Dreamcast has sent something. What
 * it sends before is dropped: Windows CE games, and a V.42 negotiation,
 * would take it for line noise. (pico thread) */
static bool ppp_passive;

/* The pico thread does not tick on a timer. It waits, in select(), for the
 * things that give it work: its native sockets, the stack's next timer,
 * and this socket, which the emulation thread writes a byte to when it has
 * handed the stack something (modem bytes, an Ethernet frame) or wants the
 * thread to stop. It is a datagram socket on the loopback interface
 * connected to itself.
 *
 * So that the emulation thread does not make a system call for every byte
 * the modem sends, it only writes when the pico thread says it is waiting.
 * pico_kicks counts what has been handed over: the pico thread notes it
 * before it looks for work and, having said it is about to wait, does not
 * wait if the count has moved since. */
static sock_t wake_sock = INVALID_SOCKET;	/* made by the emulation thread before the pico thread starts */
static retro_atomic_int_t pico_waiting;
static retro_atomic_int_t pico_kicks;

static void wake_pico(void)
{
	retro_atomic_fetch_add_int(&pico_kicks, 1);
	if (retro_atomic_load_acquire_int(&pico_waiting))
		send(wake_sock, "", 1, MSG_NOSIGNAL);
}

static struct pico_ip4 dcaddr;
static struct pico_ip4 dnsaddr;
static struct pico_socket *pico_tcp_socket, *pico_udp_socket;

static struct pico_ip4 afo_ip;

/* ---- The connections relayed (pico thread) ----
 *
 * A TCP connection of the Dreamcast's is a socket of the stack's and a
 * native one, and what crosses goes through the pair's own two buffers
 * and no other memory: what the stack has received is read straight into
 * one and sent from there, what the native socket has received is read
 * straight into the other and given to the stack from there. Whatever a
 * side does not take yet stays where it is, and no more is read for it
 * than its buffer has room for: so the stack's window holds the
 * Dreamcast back when the far end is slow, as the native socket's holds
 * the far end back when the Dreamcast is. A fixed number of them, made
 * with the thread's statics: nothing is allocated for a connection.
 *
 * The buffer towards the native socket is a ring, and keeps room for all
 * the stack can hold of a socket's (PICO_DEFAULT_SOCKETQ): when the stack
 * ends a socket - the Dreamcast's close come after its own - what it
 * still had is read all at once, and goes to the native socket after. */
#define TCP_PAIRS 32
#define IN_CHUNK 536		/* native -> Dreamcast: a segment the Dreamcast takes whole */
#define OUT_RESERVE PICO_DEFAULT_SOCKETQ
#define OUT_RING (2 * OUT_RESERVE)	/* Dreamcast -> native */

/* (draining: the stack's side has ended, and what the Dreamcast sent before
 * it did is still going to the native socket, which is closed after) */
enum { PAIR_FREE, PAIR_CONNECTING, PAIR_OPEN, PAIR_DRAINING };

typedef struct socket_pair
{
	int state;
	struct pico_socket *pico_sock;
	sock_t native_sock;		/* (connecting: the connection being made) */
	bool shutdown;			/* the stack's side shut, the native one having ended */
	bool native_eof;		/* the far end has closed its side: sent the Dreamcast, once all before it is,
					 * and the native socket is kept for what the Dreamcast still sends */
	bool shut_wr;			/* the Dreamcast has closed its side: passed on once all it sent has gone */
	bool reading;			/* dc_to_native() is taking what the Dreamcast sent: a close waits for it */
	bool ended;			/* the stack's socket has ended (its last event) while it was being read */
	u32 in_off, in_len;		/* in in_buf: from the native socket, not taken by the stack yet */
	u32 out_off, out_len;	/* in the out_buf ring: from the Dreamcast, not taken by the native socket yet */
	u8 in_buf[IN_CHUNK];
	u8 out_buf[OUT_RING];
} socket_pair;

static socket_pair tcp_pairs[TCP_PAIRS];

/* UDP: the Dreamcast's source port -> the native socket that sends for it */
#define UDP_SOCKETS 64
static struct { u16 port; sock_t fd; } udp_sockets[UDP_SOCKETS];
static unsigned udp_count;

/* The game's ports listened on: port -> socket */
#define TCP_LISTENING 10
static struct { u16 port; sock_t fd; } tcp_listening[TCP_LISTENING];
static unsigned listening_count;

static socket_pair *pair_find(const struct pico_socket *s)
{
	unsigned i;
	for (i = 0; i < TCP_PAIRS; i++)
		if (tcp_pairs[i].state != PAIR_FREE && tcp_pairs[i].pico_sock == s)
			return &tcp_pairs[i];
	return NULL;
}

/* A pair of @pico and @native, empty; NULL if every one is in use */
static socket_pair *pair_add(struct pico_socket *pico, sock_t native, int state)
{
	unsigned i;
	for (i = 0; i < TCP_PAIRS; i++)
	{
		socket_pair *p = &tcp_pairs[i];
		if (p->state != PAIR_FREE)
			continue;
		p->state = state;
		p->pico_sock = pico;
		p->native_sock = native;
		p->shutdown = p->shut_wr = p->reading = p->native_eof = p->ended = false;
		p->in_off = p->in_len = p->out_off = p->out_len = 0;
		return p;
	}
	INFO_LOG(MODEM, "Too many TCP connections: one is dropped\n");
	return NULL;
}

/* The pair ended: both its sockets closed */
static void pair_close(socket_pair *p)
{
	struct pico_socket *s = p->pico_sock;
	const sock_t fd = p->native_sock;
	p->state = PAIR_FREE;
	p->pico_sock = NULL;
	p->native_sock = INVALID_SOCKET;
	if (s != NULL)
		pico_socket_close(s);
	if (VALID(fd))
		closesocket(fd);
}

/* Passes the Dreamcast's close on, once nothing it sent before closing is
 * left to read or to send: the native socket's side shut, and the stack
 * told nothing more is read (which drops whatever it still has) */
static void pass_close(socket_pair *p)
{
	if (p->shut_wr && !p->reading && p->out_len == 0)
	{
		p->shut_wr = false;
		if (VALID(p->native_sock))
			shutdown(p->native_sock, SHUT_WR);
		pico_socket_shutdown(p->pico_sock, PICO_SHUT_RD);
	}
}

/* What is in out_buf, to the native socket, as much as it takes: 1 if all
 * of it, 0 if some is left, -1 if the connection is lost */
static int send_out(socket_pair *p)
{
	while (p->out_len != 0)
	{
		const u32 run = p->out_len < OUT_RING - p->out_off ? p->out_len : OUT_RING - p->out_off;
		int n = (int)send(p->native_sock, (const char *)p->out_buf + p->out_off, run, MSG_NOSIGNAL);
		if (n < 0)
		{
			if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
				return -1;
			return 0;
		}
		p->out_off = (p->out_off + (u32)n) % OUT_RING;
		p->out_len -= (u32)n;
		if ((u32)n < run)
			return 0;
	}
	return 1;
}

/* The stack's socket has ended: what the native socket has not taken yet
 * still goes to it before it is closed */
static void pair_ended(socket_pair *p)
{
	if (p->out_len != 0 && VALID(p->native_sock))
	{
		pico_socket_close(p->pico_sock);
		p->pico_sock = NULL;
		p->state = PAIR_DRAINING;
	}
	else
		pair_close(p);
}

/* What the Dreamcast sent, to the native socket: what is still in out_buf
 * first, then read from the stack into it and sent, until the stack has
 * no more or the native socket takes no more (it is woken when it has
 * room again: see wait_for_work()). False if the connection is lost. */
static bool dc_to_native(socket_pair *p)
{
	/* When the Dreamcast closes with bytes still unread, the stack says so
	 * from inside the read that comes next, whatever that read leaves
	 * behind. The close is not passed on from there (see tcp_callback): it
	 * would go out ahead of the bytes this read returns, and of the ones
	 * after them. So it waits until all there is has been read and sent. */
	bool drained = false;
	p->reading = true;
	for (;;)
	{
		int r;
		u32 room, tail, run;
		if (send_out(p) < 0)
		{
			p->reading = false;
			return false;
		}
		/* All there is, as far as there is room (keeping the stack's worth
		 * for its end): a read event can stand for more than one read's
		 * worth, and no other comes until more arrives */
		room = OUT_RING - p->out_len;
		if (!p->ended)
			room = room > OUT_RESERVE ? room - OUT_RESERVE : 0;
		if (room == 0)
			break;
		tail = (p->out_off + p->out_len) % OUT_RING;
		run = room < OUT_RING - tail ? room : OUT_RING - tail;
		r = pico_socket_read(p->pico_sock, p->out_buf + tail, (int)run);
		if (p->state != PAIR_OPEN)
			return true;	/* (gone from inside the read) */
		if (r <= 0)
		{
			drained = true;
			break;
		}
		p->out_len += (u32)r;
	}
	p->reading = false;
	if (drained)
		pass_close(p);
	/* (ended: all of it read, whatever is left to send) */
	if (p->ended)
		pair_ended(p);
	return true;
}

static void read_from_dc_socket(socket_pair *p)
{
	if (!dc_to_native(p))
	{
		perror("tcp_callback send");
		pair_close(p);
	}
}

/* What the native socket has received, to the stack: what is still in
 * in_buf first, else read into it */
static void native_to_dc(socket_pair *p)
{
	const u8 *data;
	u32 len;
	bool fresh = false;
	int r2;

	if (p->in_len != 0)
	{
		len = p->in_len;
		data = p->in_buf + p->in_off;
	}
	else
	{
		int r;
		if (!VALID(p->native_sock) || p->native_eof)
		{
			/* the end, once the stack has sent all there was: of both
			 * sides when the native socket is lost, of the far end's when
			 * it has only closed its own */
			if (!p->shutdown && p->pico_sock->q_out.size == 0)
			{
				pico_socket_shutdown(p->pico_sock, VALID(p->native_sock) ? PICO_SHUT_WR : PICO_SHUT_RDWR);
				p->shutdown = true;
			}
			return;
		}
		r = (int)recv(p->native_sock, (char *)p->in_buf, IN_CHUNK, 0);
		if (r == 0)
		{
			INFO_LOG(MODEM, "Socket[%d] recv(%u) returned 0 -> EOF\n", short_be(p->pico_sock->remote_port), (unsigned)IN_CHUNK);
			p->native_eof = true;
			return;
		}
		if (r < 0)
		{
			if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
			{
				perror("recv tcp socket");
				closesocket(p->native_sock);
				p->native_sock = INVALID_SOCKET;
			}
			return;
		}
		len = (u32)r;
		data = p->in_buf;
		p->in_off = 0;
		fresh = true;
		/* Visual Concepts sport games (only as it is received) */
		if (p->pico_sock->remote_port == short_be(5011) && len >= 5 && p->in_buf[0] == 1)
			memcpy(&p->in_buf[1], &p->pico_sock->local_addr.ip4.addr, 4);
	}

	r2 = pico_socket_send(p->pico_sock, data, (int)len);
	if (r2 < 0)
		INFO_LOG(MODEM, "error TCP sending: %s\n", strerror(pico_err));
	else if (r2 < (int)len)
	{
		/* (the rest stays where it is, for the next time) */
		if (r2 > 0 || fresh)
		{
			p->in_off += (u32)r2;
			p->in_len = len - (u32)r2;
		}
	}
	else
		p->in_len = 0;
}

typedef struct GamePortList {
	const char *gameId[10];
	uint16_t udpPorts[10];
	uint16_t tcpPorts[10];
} GamePortList;
static GamePortList GamesPorts[] = {
	{ /* Alien Front Online */
		{ "MK-51171" },
		{ 7980 },
		{ 0 },
	},
	{ /* ChuChu Rocket */
		{ "MK-51049", "HDR-0039", "MK-5104950" },
		{ 9789 },
		{ 0 },
	},
	{
		{
			"MK-51037", "HDR-0106",	/* Daytona USA */
			"HDR-0073",				/* Sega Tetris */
			"GENERIC", "T44501M"	/* Golf Shiyouyo 2 */
									/* (the dreamcastlive patched versions are id'ed as GENERIC) */
		},
		{ 12079, 20675 },
		{ 0 },
	},
	{ /* Dee Dee Planet */
		{ "HDR-0041" },
		{ 9879 },
		{ 0 },
	},
	{ /* Driving Strikers online demo */
		{ "IND-161053" },
		{ 30099 },
		{ 0 },
	},
	{ /* Floigan Bros */
		{ "MK-51114" },
		{ 0 },
		{ 37001 },
	},
	{ /* Internet Game Pack */
		{ "MK-51138" },
		{ 5656 },
		{ 5011, 10500, 10501, 10502, 10503 },
	},
	{ /* NBA 2K1,2K2 / NFL 2K1,2K2 / NCAA 2K2 */
		{ "MK-51063", "HDR-0150",					/* NBA 2K1 */
		  "MK-51178", "HDR-0197", "MK-5117850",   /* NBA 2K2 */
		  "MK-51062", "HDR-0144",					/* NFL 2K1 */
		  "MK-51168", "HDR-0196",					/* NFL 2K2 */
		  "MK-51176" },							/* NCAA 2K2 */
		{ 5502, 5503, 5656 },
		{ 5011, 6666 },
	},
	{ /* The Next Tetris */
		{ "T40214N", "T17717D 50" },
		{ 3512 },
		{ 3512 },
	},
	{ /* Ooga Booga */
		{ "MK-51140" },
		{ 6001 },
		{ 0 },
	},
	{ /* PBA Tour Bowling 2001 */
		{ "T26702N" },
		{ 2300, 6500, 47624, 13139 }, /* FIXME 2300-2400 ? */
		{ 2300, 47624 },			  /* FIXME 2300-2400 ? */
	},
	{ /* Planet Ring */
		{ "MK-5114864", "MK-5112550" },
		{ 7648, 1285, 1028 },
		{ 0 },
	},
	{ /* StarLancer */
		{ "T40209N", "T17723D 05" },
		{ 2300, 6500, 47624 }, /* FIXME 2300-2400 ? */
		{ 2300, 47624 },	   /* FIXME 2300-2400 ? */
	},
	{ /* World Series Baseball 2K2 */
		{ "MK-51152", "HDR-0198" },
		{ 37171, 13713 },
		{ 0 },
	},
	{ /* Worms World Party */
		{ "T22904N", "T7016D  50" },
		{ 0 },
		{ 17219 },
	},
};


static bool pico_stack_inited;
static retro_atomic_int_t pico_thread_running;
extern int dont_reject_opt_vj_hack;

static int modem_read(struct pico_device *dev, void *data, int len)
{
	int r = (int)retro_spsc_read(&out_ring, data, len);
	(void)dev;
	if (r > 0)
		ppp_passive = false;
	return r;
}

static int modem_write(struct pico_device *dev, const void *data, int len)
{
	const u8 *p = (const u8 *)data;
	int left = len;
	(void)dev;

	if (ppp_passive)
		return len;

	while (left > 0)
	{
		int key;
		size_t n = retro_spsc_write(&in_ring, p, left);

		p += n;
		left -= (int)n;
		if (left == 0)
			break;

		/* Full: sleep until the modem has read some of it. */
		key = retro_eventcount_prepare_wait(&in_ring_ec);
		if (!retro_atomic_load_acquire_int(&pico_thread_running))
		{
			retro_eventcount_cancel_wait(&in_ring_ec);
			return 0;
		}
		if (retro_spsc_write_avail(&in_ring) > 0)
			retro_eventcount_cancel_wait(&in_ring_ec);
		else
			retro_eventcount_commit_wait_timeout(&in_ring_ec, key, 5000);
	}

	return len;
}

void write_pico(u8 b)
{
	if (rings_ready)
	{
		retro_spsc_write(&out_ring, &b, 1);
		wake_pico();
	}
}
int read_pico(void)
{
	u8 b;

	if (!rings_ready || retro_spsc_read(&in_ring, &b, 1) != 1)
		return -1;
	retro_eventcount_notify(&in_ring_ec);
	if (retro_atomic_load_acquire_int(&in_ring_blocked))
	{
		retro_atomic_store_release_int(&in_ring_blocked, 0);
		wake_pico();
	}
	return b;
}

static void tcp_callback(uint16_t ev, struct pico_socket *s)
{
	if (ev & PICO_SOCK_EV_RD)
	{
		socket_pair *p = pair_find(s);
		if (p == NULL)
			INFO_LOG(MODEM, "Unknown socket: remote port %d\n", short_be(s->remote_port));
		else if (p->state == PAIR_OPEN)
			read_from_dc_socket(p);
		/* (connecting: read once it is made) */
	}

	if (ev & PICO_SOCK_EV_CONN)
	{
		struct pico_ip4 orig;
		uint16_t port;
		char peer[30];
		int yes = 1;

		struct pico_socket *sock_a = pico_socket_accept(s, &orig, &port);
		if (sock_a == NULL)
		{
			/* Also called for child sockets */
			if (pair_find(s) == NULL)
				INFO_LOG(MODEM, "pico_socket_accept: %s\n", strerror(pico_err));
		}
		else
		{
			sock_t sockfd;
			pico_ipv4_to_string(peer, sock_a->local_addr.ip4.addr);
			pico_socket_setoption(sock_a, PICO_TCP_NODELAY, &yes);
			pico_tcp_set_linger(sock_a, 10000);

			sockfd = selectable_socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
			if (!VALID(sockfd))
			{
				perror("socket");
				pico_socket_close(sock_a);
			}
			else
			{
				struct sockaddr_in serveraddr;
				memset(&serveraddr, 0, sizeof(serveraddr));
				serveraddr.sin_family = AF_INET;
				serveraddr.sin_addr.s_addr = sock_a->local_addr.ip4.addr;
				if (serveraddr.sin_addr.s_addr == AFO_ORIG_IP			/* Alien Front Online */
					|| serveraddr.sin_addr.s_addr == IGP_ORIG_IP)		/* Internet Game Pack */
				{
					serveraddr.sin_addr.s_addr = afo_ip.addr;		/* same ip for both for now */
				}

				serveraddr.sin_port = sock_a->local_port;
				set_non_blocking(sockfd);
				set_no_sigpipe(sockfd);
				if (connect(sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
				{
					if (get_last_error() != EINPROGRESS && get_last_error() != L_EWOULDBLOCK)
					{
						pico_ipv4_to_string(peer, sock_a->local_addr.ip4.addr);
						INFO_LOG(MODEM, "TCP connection to %s:%d failed: %s\n", peer, short_be(sock_a->local_port), strerror(get_last_error()));
						closesocket(sockfd);
						pico_socket_close(sock_a);
					}
					else if (pair_add(sock_a, sockfd, PAIR_CONNECTING) == NULL)
					{
						closesocket(sockfd);
						pico_socket_close(sock_a);
					}
				}
				else
				{
					set_tcp_nodelay(sockfd);
					if (pair_add(sock_a, sockfd, PAIR_OPEN) == NULL)
					{
						closesocket(sockfd);
						pico_socket_close(sock_a);
					}
				}
			}
		}
	}

	if (ev & PICO_SOCK_EV_FIN)
	{
		socket_pair *p = pair_find(s);
		if (p == NULL)
			INFO_LOG(MODEM, "PICO_SOCK_EV_FIN: Unknown socket: remote port %d\n", short_be(s->remote_port));
		else if (p->state == PAIR_OPEN)
		{
			/* What the Dreamcast sent before its end is read first (the
			 * stack ends a socket whose side was closed as the other's
			 * close comes, with what came with it still to read), and still
			 * goes to the native socket */
			p->ended = true;
			if (p->reading)
				;	/* (from inside a read: at its end) */
			else if (!VALID(p->native_sock))
				pair_close(p);
			else if (!dc_to_native(p))
				pair_close(p);
		}
		else
		{
			/* (connecting: the stack's socket is closing by itself) */
			closesocket(p->native_sock);
			p->native_sock = INVALID_SOCKET;
			p->pico_sock = NULL;
			p->state = PAIR_FREE;
		}
	}

	if (ev & PICO_SOCK_EV_ERR)
	{
		socket_pair *p = pair_find(s);
		INFO_LOG(MODEM, "Socket error received: %s\n", strerror(pico_err));
		if (p == NULL || p->state != PAIR_OPEN)
			INFO_LOG(MODEM, "PICO_SOCK_EV_ERR: Unknown socket: remote port %d\n", short_be(s->remote_port));
		else
			pair_close(p);
	}

	if (ev & PICO_SOCK_EV_CLOSE)
	{
		socket_pair *p = pair_find(s);
		if (p == NULL || p->state != PAIR_OPEN)
		{
			INFO_LOG(MODEM, "PICO_SOCK_EV_CLOSE: Unknown socket: remote port %d\n", short_be(s->remote_port));
		}
		else
		{
			/* Not before the bytes still waiting for the native socket have
			 * gone, nor those of a read this comes from the middle of, nor
			 * those the stack has after them */
			p->shut_wr = true;
			pass_close(p);
		}
	}
}

static sock_t find_udp_socket(uint16_t src_port)
{
	struct sockaddr_in saddr;
	sock_t sockfd;
	unsigned i;
#ifdef _WIN32
	u_long optl = 1;
#endif

	for (i = 0; i < udp_count; i++)
		if (udp_sockets[i].port == src_port)
			return udp_sockets[i].fd;
	if (udp_count == UDP_SOCKETS)
	{
		INFO_LOG(MODEM, "Too many UDP ports: port %d is not relayed\n", short_be(src_port));
		return INVALID_SOCKET;
	}

	sockfd = selectable_socket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
	if (!VALID(sockfd))
	{
		perror("socket");
		return INVALID_SOCKET;
	}
#ifndef _WIN32
	fcntl(sockfd, F_SETFL, O_NONBLOCK);
#else
	ioctlsocket(sockfd, FIONBIO, &optl);
#endif

	/* The port the Dreamcast sends from, when it can be had: a server may
	 * answer to the port the game is known to use and not to the one the
	 * datagram came from (Toy Racer). Otherwise any port the system has. */
	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port = src_port;
	if (bind(sockfd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
	{
		INFO_LOG(MODEM, "UDP port %d cannot be had: using another\n", short_be(src_port));
		saddr.sin_port = 0;
		if (bind(sockfd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
			perror("bind");
	}

	udp_sockets[udp_count].port = src_port;
	udp_sockets[udp_count].fd = sockfd;
	udp_count++;

	return sockfd;
}

static void udp_callback(uint16_t ev, struct pico_socket *s)
{
	if (ev & PICO_SOCK_EV_RD)
	{
		char buf[1510];
		struct pico_ip4 src_addr;
		uint16_t src_port;
		struct pico_msginfo msginfo;
		int r = 0;
		for (;;)
		{
			sock_t sockfd;
			r = pico_socket_recvfrom_extended(s, buf, sizeof(buf), &src_addr.addr, &src_port, &msginfo);

			if (r <= 0)
			{
				if (r < 0)
					INFO_LOG(MODEM, "error UDP recv: %s\n", strerror(pico_err));
				break;
			}
			DEBUG_LOG(MODEM, "udp_callback(read) recvd port %d: %d bytes\n", short_be(src_port), r);
			sockfd = find_udp_socket(src_port);
			if (VALID(sockfd))
			{
				struct sockaddr_in dst_addr;
				socklen_t addr_len = sizeof(dst_addr);
				memset(&dst_addr, 0, sizeof(dst_addr));
				dst_addr.sin_family = AF_INET;
				dst_addr.sin_addr.s_addr = msginfo.local_addr.ip4.addr;
				dst_addr.sin_port = msginfo.local_port;
				if (sendto(sockfd, buf, r, MSG_NOSIGNAL, (const struct sockaddr *)&dst_addr, addr_len) < 0)
					perror("sendto udp socket");
			}
		}
	}

	if (ev & PICO_SOCK_EV_ERR)
		INFO_LOG(MODEM, "UDP Callback error received\n");
}

static void read_native_sockets(void)
{
	int r;
	struct sockaddr_in src_addr;
	socklen_t addr_len;
	fd_set write_fds;
	int max_fd = -1;
	struct timeval tv;
	char buf[1500];
	struct pico_msginfo msginfo;
	unsigned i;

	/* Accept incoming TCP connections */
	for (i = 0; i < listening_count; i++)
	{
		sock_t sockfd;
		struct pico_socket *ps;
		addr_len = sizeof(src_addr);
		memset(&src_addr, 0, addr_len);
		sockfd = selectable_socket(accept(tcp_listening[i].fd, (struct sockaddr *)&src_addr, &addr_len));
		if (!VALID(sockfd))
		{
			if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
				perror("accept");
			continue;
		}
		ps = pico_socket_open(PICO_PROTO_IPV4, PICO_PROTO_TCP, &tcp_callback);
		if (ps == NULL)
		{
			INFO_LOG(MODEM, "pico_socket_open failed: error %d\n", pico_err);
			closesocket(sockfd);
			continue;
		}
		ps->local_addr.ip4.addr = src_addr.sin_addr.s_addr;
		ps->local_port = src_addr.sin_port;
		if (pico_socket_connect(ps, &dcaddr.addr, tcp_listening[i].port) != 0)
		{
			INFO_LOG(MODEM, "pico_socket_connect failed: error %d\n", pico_err);
			closesocket(sockfd);
			pico_socket_close(ps);
			continue;
		}
		set_non_blocking(sockfd);
		set_tcp_nodelay(sockfd);
		set_no_sigpipe(sockfd);

		if (pair_add(ps, sockfd, PAIR_OPEN) == NULL)
		{
			closesocket(sockfd);
			pico_socket_close(ps);
		}
	}

	/* Check connecting outbound TCP sockets */
	FD_ZERO(&write_fds);
	for (i = 0; i < TCP_PAIRS; i++)
		if (tcp_pairs[i].state == PAIR_CONNECTING)
		{
			FD_SET(tcp_pairs[i].native_sock, &write_fds);
			if ((int)tcp_pairs[i].native_sock > max_fd)
				max_fd = (int)tcp_pairs[i].native_sock;
		}
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	if (max_fd >= 0 && select(max_fd + 1, NULL, &write_fds, NULL, &tv) > 0)
	{
		for (i = 0; i < TCP_PAIRS; i++)
		{
			socket_pair *p = &tcp_pairs[i];
			int value = 0;
			socklen_t l = sizeof(value);
			if (p->state != PAIR_CONNECTING || !FD_ISSET(p->native_sock, &write_fds))
				continue;
			if (getsockopt(p->native_sock, SOL_SOCKET, SO_ERROR, (char *)&value, &l) < 0 || value)
			{
				char peer[30];
				pico_ipv4_to_string(peer, p->pico_sock->local_addr.ip4.addr);
				INFO_LOG(MODEM, "TCP connection to %s:%d failed: %s\n", peer, short_be(p->pico_sock->local_port), strerror(get_last_error()));
				pair_close(p);
			}
			else
			{
				set_tcp_nodelay(p->native_sock);
				p->state = PAIR_OPEN;
				read_from_dc_socket(p);
			}
		}
	}

	/* Read UDP sockets */
	for (i = 0; i < udp_count; i++)
	{
		if (!VALID(udp_sockets[i].fd))
			continue;

		addr_len = sizeof(src_addr);
		memset(&src_addr, 0, addr_len);
		r = (int)recvfrom(udp_sockets[i].fd, buf, sizeof(buf), 0, (struct sockaddr *)&src_addr, &addr_len);
		if (r > 0)
		{
			int r2;
			msginfo.dev = pico_dev;
			msginfo.tos = 0;
			msginfo.ttl = 0;
			msginfo.local_addr.ip4.addr = src_addr.sin_addr.s_addr;
			msginfo.local_port = src_addr.sin_port;
			DEBUG_LOG(MODEM, "read_native_sockets UDP received %d bytes from %08x:%d\n", r, long_be(msginfo.local_addr.ip4.addr), short_be(msginfo.local_port));
			r2 = pico_socket_sendto_extended(pico_udp_socket, buf, r, &dcaddr, udp_sockets[i].port, &msginfo);
			if (r2 < r)
				INFO_LOG(MODEM, "error UDP sending to %d: %s\n", short_be(udp_sockets[i].port), strerror(pico_err));
		}
		else if (r < 0 && get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
		{
			perror("recvfrom udp socket");
			continue;
		}
	}

	/* Read TCP sockets */
	for (i = 0; i < TCP_PAIRS; i++)
	{
		socket_pair *p = &tcp_pairs[i];
		if (p->state == PAIR_DRAINING)
		{
			/* (the end of it, once it has all gone: the native socket
			 * sends on what it has taken after it is closed) */
			if (send_out(p) != 0)
				pair_close(p);
			continue;
		}
		if (p->state != PAIR_OPEN)
			continue;
		if (p->out_len != 0 && VALID(p->native_sock) && !dc_to_native(p))
		{
			perror("tcp socket send");
			pair_close(p);
			continue;
		}
		if (p->state == PAIR_OPEN)
			native_to_dc(p);
	}
}

static void close_native_sockets(void)
{
	unsigned i;
	for (i = 0; i < udp_count; i++)
		closesocket(udp_sockets[i].fd);
	udp_count = 0;
	for (i = 0; i < TCP_PAIRS; i++)
		if (tcp_pairs[i].state != PAIR_FREE)
			pair_close(&tcp_pairs[i]);
}

static int modem_set_speed(struct pico_device *dev, uint32_t speed)
{
	(void)dev;
	(void)speed;
	return 0;
}

static void check_dns_entries(void)
{
	static uint32_t dns_query_start = 0;
	static uint32_t dns_query_attempts = 0;

	if (afo_ip.addr == 0)
	{
		if (!dns_query_start)
		{
			dns_query_start = PICO_TIME_MS();
			get_host_by_name("auriga.segasoft.com", dnsaddr.addr);	/* Alien Front Online server */
		}
		else
		{
			u32 answer;
			if (get_dns_answer(&answer, dnsaddr.addr) == 0)
			{
				char afoip[16];
				afo_ip.addr = answer;
				dns_query_attempts = 0;
				dns_query_start = 0;
				pico_ipv4_to_string(afoip, afo_ip.addr);
				INFO_LOG(MODEM, "AFO server IP is %s\n", afoip);
			}
			else
			{
				if (PICO_TIME_MS() - dns_query_start > 1000)
				{
					if (++dns_query_attempts >= 5)
					{
						u32 addr;
						pico_string_to_ipv4("146.185.135.179", &addr);	/* Default address */
						memcpy(&afo_ip.addr, &addr, sizeof(addr));
						dns_query_attempts = 0;
					}
					else
						dns_query_start = 0;	/* Retry */
				}
			}
		}
	}
}

static struct pico_device *pico_eth_create(void)
{
	static const u8 mac_addr[6] = { 0xc, 0xa, 0xf, 0xe, 0, 1 };
	struct pico_device *eth = (struct pico_device *)PICO_ZALLOC(sizeof(struct pico_device));
	if (!eth)
		return NULL;

	if (0 != pico_device_init(eth, "ETHPEER", mac_addr))
		return NULL;

	DEBUG_LOG(NETWORK, "Device %s created\n", eth->name);

	return eth;
}

/* A capture of every frame of the adapter, for debugging: never in a release */
/*#define BBA_PCAPNG_DUMP*/
static FILE *pcapngDump;

static void dumpFrame(const u8 *frame, u32 size)
{
#ifdef BBA_PCAPNG_DUMP
	u32 blockType, blockLen, roundedSize;
	if (pcapngDump == NULL)
	{
		char path[1024];
		u32 magic = 0x1A2B3C4D, version = 1;
		u64 sectionLength = ~(u64)0;
		const u32 linkType = 1, snapLen = 0;
		const char *home = getenv("HOME");
		if (home == NULL)
			return;
		snprintf(path, sizeof(path), "%s/bba.pcapng", home);
		pcapngDump = fopen(path, "wb");
		if (pcapngDump == NULL)
			return;
		blockType = 0x0A0D0D0A; /* Section Header Block */
		fwrite(&blockType, sizeof(blockType), 1, pcapngDump);
		blockLen = 28;
		fwrite(&blockLen, sizeof(blockLen), 1, pcapngDump);
		fwrite(&magic, sizeof(magic), 1, pcapngDump);
		fwrite(&version, sizeof(version), 1, pcapngDump);
		fwrite(&sectionLength, sizeof(sectionLength), 1, pcapngDump);
		fwrite(&blockLen, sizeof(blockLen), 1, pcapngDump);

		blockType = 1; /* Interface Description Block */
		fwrite(&blockType, sizeof(blockType), 1, pcapngDump);
		blockLen = 20;
		fwrite(&blockLen, sizeof(blockLen), 1, pcapngDump);
		fwrite(&linkType, sizeof(linkType), 1, pcapngDump);
		fwrite(&snapLen, sizeof(snapLen), 1, pcapngDump);
		fwrite(&blockLen, sizeof(blockLen), 1, pcapngDump);
	}
	blockType = 3; /* Simple Packet Block */
	fwrite(&blockType, sizeof(blockType), 1, pcapngDump);
	roundedSize = ((size + 3) & ~3) + 16;
	fwrite(&roundedSize, sizeof(roundedSize), 1, pcapngDump);
	fwrite(&size, sizeof(size), 1, pcapngDump);
	fwrite(frame, 1, size, pcapngDump);
	fwrite(frame, 1, roundedSize - size - 16, pcapngDump);
	fwrite(&roundedSize, sizeof(roundedSize), 1, pcapngDump);
#else
	(void)frame;
	(void)size;
#endif
}
static void closeDumpFile(void)
{
	if (pcapngDump != NULL)
	{
		fclose(pcapngDump);
		pcapngDump = NULL;
	}
}
/* Emulation thread: the adapter has sent a frame. */
void pico_receive_eth_frame(const u8 *frame, u32 size)
{
	u8 buf[2 + ETH_FRAME_MAX];

	if (!rings_ready || size == 0 || size > ETH_FRAME_MAX)
		return;
	/* No room: dropped, as a switch with a full queue would */
	if (retro_spsc_write_avail(&eth_out_ring) < size + 2)
		return;
	buf[0] = (u8)size;
	buf[1] = (u8)(size >> 8);
	memcpy(&buf[2], frame, size);
	retro_spsc_write(&eth_out_ring, buf, size + 2);
	wake_pico();
}

/* Pico thread: give the stack the frames the adapter has sent. */
/* No more than this many in one go. A guest that sends without stopping
 * refills the ring as fast as it is emptied: taken until the ring was
 * empty, its frames kept this thread here for good, and the stack's
 * timers and the sockets - which come after - never had their turn (an
 * ARP request in such a flood got its first answer and no other). */
#define ETH_FRAMES_A_PASS 256

/* True if there may be more waiting: the caller comes back for them once
 * the rest has had its turn, without sleeping first. */
static bool take_eth_frames(void)
{
	u8 buf[ETH_FRAME_MAX];
	u8 len[2];
	int taken;

	for (taken = 0; taken < ETH_FRAMES_A_PASS; taken++)
	{
		u32 size;
		if (retro_spsc_read(&eth_out_ring, len, 2) != 2)
			return false;

		size = len[0] | (len[1] << 8);

		if (size > ETH_FRAME_MAX || retro_spsc_read(&eth_out_ring, buf, size) != size)
			return false;
		dumpFrame(buf, size);
		if (pico_dev != NULL)
			pico_stack_recv(pico_dev, buf, size);
	}
	return true;
}
/* Pico thread: the stack has a frame for the adapter. */
static int send_eth_frame(struct pico_device *dev, void *data, int len)
{
	u8 buf[2 + ETH_FRAME_MAX];
	(void)dev;

	if (len <= 0 || len > ETH_FRAME_MAX)
		return len;		/* nothing the adapter could take: let the stack drop it */
	if (retro_spsc_write_avail(&eth_in_ring) < (size_t)len + 2)
	{
		/* The stack keeps the frame and tries again when it is next woken */
		retro_atomic_store_release_int(&eth_in_blocked, 1);
		if (retro_spsc_write_avail(&eth_in_ring) < (size_t)len + 2)
			return 0;
	}
	dumpFrame((const u8 *)data, len);
	buf[0] = (u8)len;
	buf[1] = (u8)(len >> 8);
	memcpy(&buf[2], data, len);
	retro_spsc_write(&eth_in_ring, buf, len + 2);
	return len;
}

/* Emulation thread: the next frame the stack has sent the adapter, into
 * @frame (ETH_FRAME_MAX bytes). Its size, 0 if there is none. */
u32 net_eth_take(u8 *frame)
{
	u8 len[2];
	u32 size;

	if (!rings_ready || retro_spsc_read(&eth_in_ring, len, 2) != 2)
		return 0;
	size = len[0] | (len[1] << 8);
	/* (frames are written whole: this cannot be short) */
	if (size > ETH_FRAME_MAX || retro_spsc_read(&eth_in_ring, frame, size) != size)
		size = 0;
	if (retro_atomic_load_acquire_int(&eth_in_blocked))
	{
		retro_atomic_store_release_int(&eth_in_blocked, 0);
		wake_pico();
	}
	return size;
}

#define WATCH(fd, set) do { FD_SET((fd), (set)); if ((int)(fd) > max_fd) max_fd = (int)(fd); } while (0)

/* Waits until the thread has something to do: a native socket to read,
 * accept on, finish connecting or send what it did not take, a timer of
 * the stack's come due, something handed over by the emulation thread, or
 * the order to stop. @kicks is pico_kicks as it was before the thread last
 * looked for work. */
static void wait_for_work(int kicks)
{
	fd_set readable, writable;
	struct timeval tv;
	char drain[64];
	int max_fd = (int)wake_sock;
	int idle = pico_stack_idle_ms();
	unsigned i;

	/* The last tick moved a frame: there may be more */
	if (idle == 0)
		return;
	/* Never longer than a second, whatever the stack's timers say */
	if (idle < 0 || idle > 1000)
		idle = 1000;

	FD_ZERO(&readable);
	FD_ZERO(&writable);
	FD_SET(wake_sock, &readable);
	for (i = 0; i < udp_count; i++)
		if (VALID(udp_sockets[i].fd))
			WATCH(udp_sockets[i].fd, &readable);
	for (i = 0; i < listening_count; i++)
		WATCH(tcp_listening[i].fd, &readable);
	for (i = 0; i < TCP_PAIRS; i++)
	{
		const socket_pair *p = &tcp_pairs[i];
		if (p->state == PAIR_CONNECTING || p->state == PAIR_DRAINING)
			WATCH(p->native_sock, &writable);
		if (p->state != PAIR_OPEN || !VALID(p->native_sock))
			continue;
		/* One with bytes the stack has not taken yet is not read until it
		 * has, nor one whose far end has closed */
		if (p->in_len == 0 && !p->native_eof)
			WATCH(p->native_sock, &readable);
		/* One that is owed bytes: woken when it has room for them */
		if (p->out_len != 0)
			WATCH(p->native_sock, &writable);
	}
	if (afo_ip.addr == 0 && VALID(get_dns_socket()))
		WATCH(get_dns_socket(), &readable);

	tv.tv_sec = idle / 1000;
	tv.tv_usec = (idle % 1000) * 1000;

	retro_atomic_fetch_add_int(&pico_waiting, 1);
	if (retro_atomic_load_acquire_int(&pico_thread_running)
			&& retro_atomic_load_acquire_int(&pico_kicks) == kicks)
		select(max_fd + 1, &readable, &writable, NULL, &tv);
	retro_atomic_store_release_int(&pico_waiting, 0);

	while (recv(wake_sock, drain, sizeof(drain), 0) > 0)
		;
}

/* For DCNet (dcnet.c), which runs on this thread in place of the stack:
 * the same rings, the same way of waiting. */
bool netthread_running(void)
{
	return retro_atomic_load_acquire_int(&pico_thread_running) != 0;
}

int netthread_kicks(void)
{
	return retro_atomic_load_acquire_int(&pico_kicks);
}

size_t netthread_modem_take(u8 *buf, size_t len)
{
	return retro_spsc_read(&out_ring, buf, len);
}

size_t netthread_modem_give(const u8 *buf, size_t len)
{
	size_t n = retro_spsc_write(&in_ring, buf, len);
	if (n < len)
	{
		/* Full: the modem wakes the thread when it has read some (read_pico()) */
		retro_atomic_store_release_int(&in_ring_blocked, 1);
		n += retro_spsc_write(&in_ring, buf + n, len - n);
	}
	return n;
}

u32 netthread_eth_take(u8 *frame)
{
	u8 len[2];
	u32 size;

	if (retro_spsc_read(&eth_out_ring, len, 2) != 2)
		return 0;
	size = len[0] | (len[1] << 8);
	if (size > ETH_FRAME_MAX || retro_spsc_read(&eth_out_ring, frame, size) != size)
		return 0;
	return size;
}

bool netthread_eth_give(const u8 *frame, u32 size)
{
	return send_eth_frame(NULL, (void *)frame, (int)size) != 0;
}

void netthread_wait(int kicks, fd_set *readable, fd_set *writable, int max_fd, int timeout_ms)
{
	struct timeval tv;
	char drain[64];

	WATCH(wake_sock, readable);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	retro_atomic_fetch_add_int(&pico_waiting, 1);
	if (retro_atomic_load_acquire_int(&pico_thread_running)
			&& retro_atomic_load_acquire_int(&pico_kicks) == kicks)
		select(max_fd + 1, readable, writable, NULL, &tv);
	retro_atomic_store_release_int(&pico_waiting, 0);

	while (recv(wake_sock, drain, sizeof(drain), 0) > 0)
		;
}

/* The IPv4 address of @host, 0 if it has none. (This can take seconds: the
 * emulation thread never waits for the network thread but at its next
 * start, see stop_pico().) */
u32 netthread_resolve(const char *host)
{
	u32 ip = 0;
	struct addrinfo hints;
	struct addrinfo *res = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	if (getaddrinfo(host, NULL, &hints, &res) == 0 && res != NULL)
	{
		struct addrinfo *ai;
		for (ai = res; ai != NULL; ai = ai->ai_next)
			if (ai->ai_family == AF_INET && ai->ai_addrlen >= sizeof(struct sockaddr_in))
			{
				ip = ((const struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr;
				break;
			}
		freeaddrinfo(res);
	}
	return ip;
}

/* The Dreamcast's DNS server: the one asked for, the current default for
 * the old one */
static void find_dns_server(void)
{
	const char *name = settings.network.dns;
	u32 ip = 0;
	if (strcmp(name, "46.101.91.123") == 0)
		name = "dns.flyca.st";
	if (pico_string_to_ipv4(name, &ip) != 0 || ip == 0)
		ip = netthread_resolve(name);
	if (ip != 0)
	{
		char s[16];
		memcpy(&dnsaddr.addr, &ip, sizeof(ip));
		pico_ipv4_to_string(s, dnsaddr.addr);
		NOTICE_LOG(MODEM, "%s IP is %s\n", name, s);
	}
	else
	{
		pico_string_to_ipv4("46.101.91.123", &ip);
		memcpy(&dnsaddr.addr, &ip, sizeof(ip));
		WARN_LOG(MODEM, "Can't resolve %s. Using 46.101.91.123\n", name);
	}
}

/* The game's product number, without the spaces after it */
static void game_id(char *id)
{
	size_t n = sizeof(ip_meta.product_number);
	memcpy(id, ip_meta.product_number, n);
	while (n > 0 && id[n - 1] == ' ')
		n--;
	id[n] = '\0';
}

static void pico_thread_func(void *arg)
{
	bool bba;
	const GamePortList *ports = NULL;
	char gameId[sizeof(ip_meta.product_number) + 1];
	miniupnp_t upnp;
	bool upnp_mapped = false;
	u32 addr;
	struct pico_ip4 inaddr_any;
	uint16_t listen_port = 0;
	int yes = 1;
	int ret;
	u32 i, j;
	(void)arg;

	if (thread_dcnet)
	{
		dcnet_run(thread_bba);
		return;
	}
	bba = thread_bba;
	/* (the PPP stack's first words are to wait for the Dreamcast's) */
	ppp_passive = !bba;
	if (!pico_stack_inited)
	{
		pico_stack_init();
		pico_stack_inited = true;
#if _WIN32
		{
			static WSADATA wsaData;
			if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0)
				WARN_LOG(MODEM, "WSAStartup failed\n");
		}
#endif
	}

	/* Find the network ports for the current game */
	game_id(gameId);
	for (i = 0; i < ARRAY_SIZE(GamesPorts) && ports == NULL; i++)
	{
		const GamePortList *game = &GamesPorts[i];
		for (j = 0; j < ARRAY_SIZE(game->gameId) && game->gameId[j] != NULL; j++)
		{
			if (strcmp(gameId, game->gameId[j]) == 0)
			{
				NOTICE_LOG(MODEM, "Found network ports for game %s\n", gameId);
				ports = game;
				break;
			}
		}
	}
	/* Web TV requires the VJ compression option, which picotcp doesn't support.
	 * This hack allows WebTV to connect although the correct fix would
	 * be to implement VJ compression. */
	dont_reject_opt_vj_hack = strcmp(gameId, "6107117") == 0 ? 1 : 0;

	/* Find the gateway (UPnP) and map network ports */
	memset(&upnp, 0, sizeof(upnp));
	if (ports != NULL)
	{
		if (!miniupnp_init(&upnp))
			WARN_LOG(MODEM, "UPNP Init failed\n");
		else
		{
			upnp_mapped = true;
			for (i = 0; i < ARRAY_SIZE(ports->udpPorts) && ports->udpPorts[i] != 0; i++)
				if (!miniupnp_add_port_mapping(&upnp, ports->udpPorts[i], false))
					WARN_LOG(MODEM, "UPNP AddPortMapping UDP %d failed\n", ports->udpPorts[i]);
			for (i = 0; i < ARRAY_SIZE(ports->tcpPorts) && ports->tcpPorts[i] != 0; i++)
				if (!miniupnp_add_port_mapping(&upnp, ports->tcpPorts[i], true))
					WARN_LOG(MODEM, "UPNP AddPortMapping TCP %d failed\n", ports->tcpPorts[i]);
		}
	}

	find_dns_server();

	/* Create ppp/eth device */
	if (!bba)
	{
		struct pico_ip4 ipaddr;
		/* PPP */
		pico_dev = pico_ppp_create();
		if (!pico_dev)
			return;
		pico_string_to_ipv4("192.168.167.2", &addr);
		memcpy(&dcaddr.addr, &addr, sizeof(addr));
		pico_ppp_set_peer_ip(pico_dev, dcaddr);
		pico_string_to_ipv4("192.168.167.1", &addr);
		memcpy(&ipaddr.addr, &addr, sizeof(addr));
		pico_ppp_set_ip(pico_dev, ipaddr);
		pico_ppp_set_dns1(pico_dev, dnsaddr);

		pico_ppp_set_serial_read(pico_dev, modem_read);
		pico_ppp_set_serial_write(pico_dev, modem_write);
		pico_ppp_set_serial_set_speed(pico_dev, modem_set_speed);
		pico_dev->proxied = 1;

		pico_ppp_connect(pico_dev);
	}
	else
	{
		struct pico_ip4 ipaddr, netmask;
		struct pico_dhcp_server_setting dhcpSettings;
		/* Ethernet */
		pico_dev = pico_eth_create();
		if (pico_dev == NULL)
			return;
		pico_dev->send = &send_eth_frame;
		pico_dev->proxied = 1;

		pico_string_to_ipv4("192.168.169.1", &addr);
		memcpy(&ipaddr.addr, &addr, sizeof(addr));
		pico_string_to_ipv4("255.255.255.0", &addr);
		memcpy(&netmask.addr, &addr, sizeof(addr));
		pico_ipv4_link_add(pico_dev, ipaddr, netmask);
		/* dreamcast IP */
		pico_string_to_ipv4("192.168.169.2", &addr);
		memcpy(&dcaddr.addr, &addr, sizeof(addr));

		memset(&dhcpSettings, 0, sizeof(dhcpSettings));
		dhcpSettings.dev = pico_dev;
		dhcpSettings.server_ip = ipaddr;
		dhcpSettings.lease_time = long_be(24 * 60 * 60); /* seconds */
		dhcpSettings.netmask = netmask;
		dhcpSettings.pool_start = addr;
		dhcpSettings.pool_end = addr;
		dhcpSettings.pool_next = addr;
		dhcpSettings.dns_server.addr = dnsaddr.addr;
		if (pico_dhcp_server_initiate(&dhcpSettings) != 0)
			WARN_LOG(MODEM, "DHCP server init failed\n");
	}

	pico_udp_socket = pico_socket_open(PICO_PROTO_IPV4, PICO_PROTO_UDP, &udp_callback);
	if (pico_udp_socket == NULL)
	{
		INFO_LOG(MODEM, "error opening UDP socket: %s\n", strerror(pico_err));
		return;
	}
	inaddr_any.addr = 0;
	ret = pico_socket_bind(pico_udp_socket, &inaddr_any, &listen_port);
	if (ret < 0)
		INFO_LOG(MODEM, "error binding UDP socket to port %u: %s\n", short_be(listen_port), strerror(pico_err));

	pico_tcp_socket = pico_socket_open(PICO_PROTO_IPV4, PICO_PROTO_TCP, &tcp_callback);
	if (pico_tcp_socket == NULL)
		INFO_LOG(MODEM, "error opening TCP socket: %s\n", strerror(pico_err));
	pico_socket_setoption(pico_tcp_socket, PICO_TCP_NODELAY, &yes);
	ret = pico_socket_bind(pico_tcp_socket, &inaddr_any, &listen_port);
	if (ret < 0)
		INFO_LOG(MODEM, "error binding TCP socket to port %u: %s\n", short_be(listen_port), strerror(pico_err));
	else if (pico_socket_listen(pico_tcp_socket, 10) != 0)
		INFO_LOG(MODEM, "error listening on port %u\n", short_be(listen_port));

	/* Open listening sockets */
	if (ports != NULL)
	{
		struct sockaddr_in saddr;
		socklen_t saddr_len = sizeof(saddr);
		memset(&saddr, 0, sizeof(saddr));
		saddr.sin_family = AF_INET;
		saddr.sin_addr.s_addr = INADDR_ANY;

		/* Bound to the port by find_udp_socket */
		for (i = 0; i < ARRAY_SIZE(ports->udpPorts) && ports->udpPorts[i] != 0; i++)
			find_udp_socket(short_be(ports->udpPorts[i]));

		for (i = 0; i < ARRAY_SIZE(ports->tcpPorts) && ports->tcpPorts[i] != 0; i++)
		{
			uint16_t port = short_be(ports->tcpPorts[i]);
			sock_t sockfd;
			saddr.sin_port = port;
			sockfd = selectable_socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
			if (!VALID(sockfd))
			{
				perror("socket");
				continue;
			}
			set_reuse_addr(sockfd);
			if (bind(sockfd, (struct sockaddr *)&saddr, saddr_len) < 0)
			{
				perror("bind");
				closesocket(sockfd);
				continue;
			}
			if (listen(sockfd, 5) < 0)
			{
				perror("listen");
				closesocket(sockfd);
				continue;
			}
			set_non_blocking(sockfd);
			tcp_listening[listening_count].port = port;
			tcp_listening[listening_count].fd = sockfd;
			listening_count++;
		}
	}

	while (retro_atomic_load_acquire_int(&pico_thread_running))
	{
		const int kicks = retro_atomic_load_acquire_int(&pico_kicks);
		const bool more_frames = take_eth_frames();

		read_native_sockets();
		pico_stack_tick();
		check_dns_entries();
		if (!more_frames)
			wait_for_work(kicks);
	}
	for (i = 0; i < listening_count; i++)
		closesocket(tcp_listening[i].fd);
	/* The next connection must not wait on, or accept from, these numbers */
	listening_count = 0;
	close_native_sockets();
	pico_socket_close(pico_tcp_socket);
	pico_socket_close(pico_udp_socket);

	if (pico_dev)
	{
		if (!bba)
			pico_ppp_destroy(pico_dev);
		else
		{
			closeDumpFile();
			pico_dhcp_server_destroy(pico_dev);
			pico_device_destroy(pico_dev);
		}
		pico_dev = NULL;
	}
	pico_stack_tick();
	if (upnp_mapped)
		miniupnp_term(&upnp);
}

/* (joined before the next one starts, and when the machine stops) */
static sthread_t *pico_thread;

static void pico_thread_join(void)
{
	if (pico_thread != NULL)
	{
		sthread_join(pico_thread);
		pico_thread = NULL;
	}
}

static bool make_wake_socket(void)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	sock_t sock;

	if (VALID(wake_sock))
		return true;
#ifdef _WIN32
	{
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0)
			return false;
	}
#endif
	sock = selectable_socket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
	if (!VALID(sock))
		return false;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0
			|| getsockname(sock, (struct sockaddr *)&addr, &len) < 0
			|| connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		closesocket(sock);
		return false;
	}
	set_non_blocking(sock);
	wake_sock = sock;
	return true;
}

bool start_pico(void)
{
	char gameId[sizeof(ip_meta.product_number) + 1];

	if (retro_atomic_load_relaxed_int(&pico_thread_running))
		return false;
	/* The last one, if it has not ended yet, is about to */
	pico_thread_join();
	/* The pico thread is not running, so both rings are this thread's
	 * alone: make them, or empty them of the last connection's bytes. */
	if (!rings_ready)
	{
		if (!retro_eventcount_init(&in_ring_ec))
			return false;
		if (!retro_spsc_init(&in_ring, IN_RING_BYTES))
		{
			retro_eventcount_free(&in_ring_ec);
			return false;
		}
		if (!retro_spsc_init(&out_ring, OUT_RING_BYTES))
		{
			retro_spsc_free(&in_ring);
			retro_eventcount_free(&in_ring_ec);
			return false;
		}
		if (!retro_spsc_init(&eth_out_ring, ETH_RING_BYTES))
		{
			retro_spsc_free(&in_ring);
			retro_spsc_free(&out_ring);
			retro_eventcount_free(&in_ring_ec);
			return false;
		}
		if (!retro_spsc_init(&eth_in_ring, ETH_RING_BYTES))
		{
			retro_spsc_free(&in_ring);
			retro_spsc_free(&out_ring);
			retro_spsc_free(&eth_out_ring);
			retro_eventcount_free(&in_ring_ec);
			return false;
		}
		rings_ready = true;
	}
	else
	{
		retro_spsc_clear(&in_ring);
		retro_spsc_clear(&out_ring);
		retro_spsc_clear(&eth_out_ring);
		retro_spsc_clear(&eth_in_ring);
	}
	retro_atomic_store_release_int(&eth_in_blocked, 0);
	retro_atomic_store_release_int(&in_ring_blocked, 0);
	if (!make_wake_socket())
		return false;
	thread_bba = net_device == NET_DEVICE_BBA;
	game_id(gameId);
	thread_dcnet = net_dcnet_option
			/* these games' servers are reached through DCNet only */
			|| strcmp(gameId, "HDR-0113") == 0		/* Power Smash */
			|| strcmp(gameId, "HDR-0091") == 0;	/* Pro Yakyuu Team de Asobou Net! */
	if (thread_dcnet)
		NOTICE_LOG(MODEM, "Network through DCNet\n");
	retro_atomic_store_release_int(&pico_waiting, 0);
	retro_atomic_store_release_int(&pico_thread_running, 1);
	pico_thread = sthread_create(pico_thread_func, NULL);
	return true;
}

/* The thread is told to stop and is not waited for: it may be looking up a
 * name or talking to the router (UPnP), which can take seconds. It is waited
 * for before the next one starts, and when the machine stops. */
void stop_pico(void)
{
	retro_atomic_store_release_int(&pico_thread_running, 0);
	if (rings_ready)
		retro_eventcount_notify(&in_ring_ec);
	if (VALID(wake_sock))
		send(wake_sock, "", 1, MSG_NOSIGNAL);
}

void net_thread_join(void)
{
	stop_pico();
	pico_thread_join();
}

#else

#include "types.h"
#include "picoppp.h"

bool start_pico(void) { return false; }
void stop_pico(void) { }
void net_thread_join(void) { }
void write_pico(u8 b) { (void)b; }
int read_pico(void) { return -1; }
u32 net_eth_take(u8 *frame) { (void)frame; return 0; }
void pico_receive_eth_frame(const u8 *frame, u32 size) { (void)frame; (void)size; }

#endif
