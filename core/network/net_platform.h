#ifndef _NET_PLATFORM_H
#define _NET_PLATFORM_H

#include <string.h>
#include "types.h"

/* The platform's sockets are libretro-common's (net_compat.h); what the
 * network code needs on top of them is here */
#include <net/net_compat.h>
#include <net/net_socket.h>

#ifdef HAVE_LIBNX
#include <switch.h>
#endif

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
/* libnx's <netinet/in.h> has no INADDR_LOOPBACK. 127.0.0.1, host order. */
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001
#endif

#ifndef _WIN32
#define closesocket close
typedef int sock_t;
#define VALID(s) ((s) >= 0)
#define L_EWOULDBLOCK EWOULDBLOCK
#define L_EAGAIN EAGAIN
#define get_last_error() (errno)
#define INVALID_SOCKET (-1)
#define perror(s) do { INFO_LOG(MODEM, "%s: %s", (s) != NULL ? (s) : "", strerror(get_last_error())); } while (false)
#else
typedef SOCKET sock_t;
#define VALID(s) ((s) != INVALID_SOCKET)
#define L_EWOULDBLOCK WSAEWOULDBLOCK
#define L_EAGAIN WSAEWOULDBLOCK
#define get_last_error() (WSAGetLastError())
#define perror(s) do { INFO_LOG(MODEM, "%s: Winsock error: %d\n", (s) != NULL ? (s) : "", WSAGetLastError()); } while (false)
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#define SHUT_RD SD_RECEIVE
#define SHUT_RDWR SD_BOTH
#endif
#endif

/* Writing to a TCP connection the other end has closed raises SIGPIPE, and
 * a SIGPIPE nothing handles ends the whole process. The signal's handling
 * belongs to the frontend, so it is never raised instead: by MSG_NOSIGNAL
 * on each send where the system has it (net_compat.h makes it 0 where it
 * has not), and by an option set once on the socket where it has that
 * (Apple, the BSDs). Windows has no such signal. */
static TYPES_INLINE void set_no_sigpipe(sock_t fd)
{
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
	int optval = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const void *)&optval, sizeof(optval));
#else
	(void)fd;
#endif
}

/* A listening port can be bound again as soon as its socket is closed,
 * though connections accepted on it are still winding down: without this
 * the bind fails for a minute or more after a game is restarted. Not on
 * Windows: a port can be bound again there without it, and with it any
 * other program could bind the port while it is in use here. */
static TYPES_INLINE void set_reuse_addr(sock_t fd)
{
#ifndef _WIN32
	int optval = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(optval));
#else
	(void)fd;
#endif
}

/* Whether a socket can be waited on with select(). A descriptor set has
 * room for the numbers below FD_SETSIZE and FD_SET does not look: a higher
 * one is written outside the set. Windows' sets hold the sockets themselves
 * and FD_SET leaves out what does not fit. */
static TYPES_INLINE bool fits_select(sock_t fd)
{
#ifndef _WIN32
	return fd < FD_SETSIZE;
#else
	(void)fd;
	return true;
#endif
}

/* The socket if it is valid and select() can wait on it; otherwise it is
 * closed and INVALID_SOCKET is returned. */
static TYPES_INLINE sock_t selectable_socket(sock_t fd)
{
	if (VALID(fd) && !fits_select(fd))
	{
		closesocket(fd);
		return INVALID_SOCKET;
	}
	return fd;
}

static TYPES_INLINE void set_non_blocking(sock_t fd)
{
	socket_nonblock((int)fd);
}

static TYPES_INLINE void set_tcp_nodelay(sock_t fd)
{
	int optval = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&optval, sizeof(optval));
}

#endif
