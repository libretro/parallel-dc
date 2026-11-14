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
/* getaddrinfo() and struct addrinfo, which a C89 compile of the C
 * library's headers leaves out */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <errno.h>
#include <string.h>
#include "naomi_network.h"
#include "types.h"
#include <features/features_cpu.h>

static bool net_stopping(naomi_network *net)
{
	return retro_atomic_load_acquire_int(&net->network_stopping) != 0;
}

static bool net_is_master(const naomi_network *net)
{
	return net->slot_id == 0;
}

void naomi_network_create(naomi_network *net)
{
	int i;
	memset(net, 0, sizeof(*net));
	net->server_ip.s_addr = INADDR_NONE;
	net->server_sock = INVALID_SOCKET;
	net->beacon_sock = INVALID_SOCKET;
	for (i = 0; i < NAOMI_NET_SLAVES_MAX; i++)
		net->slaves[i] = INVALID_SOCKET;
	net->client_sock = INVALID_SOCKET;
	net->wake_sock = INVALID_SOCKET;
	retro_atomic_int_init(&net->network_stopping, 0);
}

static sock_t create_and_bind(int protocol)
{
	int option = 1;
	struct sockaddr_in serveraddr;
	sock_t sock = socket(AF_INET, protocol == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM, protocol);
	if (sock == INVALID_SOCKET)
	{
		ERROR_LOG(NETWORK, "Cannot create server socket\n");
		return INVALID_SOCKET;
	}
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&option, sizeof(option));

	memset(&serveraddr, 0, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = htons(NAOMI_NET_SERVER_PORT);

	if (bind(sock, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
	{
		ERROR_LOG(NETWORK, "NaomiServer: bind() failed. errno=%d\n", get_last_error());
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
	else
		set_non_blocking(sock);

	return sock;
}

bool naomi_network_arm_wake(naomi_network *net)
{
#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0)
		return false;
#endif
	if (net->wake_sock == INVALID_SOCKET)
	{
		/* A datagram socket on the loopback interface, connected to
		 * itself: a byte sent to it makes it readable. */
		struct sockaddr_in addr;
		socklen_t len = sizeof(addr);
		sock_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

		if (sock == INVALID_SOCKET)
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
		net->wake_sock = sock;
	}
	else
	{
		/* What the last shutdown left in it */
		char c;
		while (recv(net->wake_sock, &c, 1, 0) > 0)
			;
	}
	retro_atomic_store_release_int(&net->network_stopping, 0);
	return true;
}

/* >0: one of @socks is readable; 0: timed out (@usec < 0: no timeout) or
 * told to stop */
static int wait_readable(naomi_network *net, const sock_t *socks, int count, int64_t usec)
{
	fd_set readable;
	struct timeval tv;
	int max_fd = -1;
	int found = 0;
	int i;

	if (net_stopping(net))
		return 0;

	FD_ZERO(&readable);
	for (i = 0; i < count; i++)
	{
		if (socks[i] == INVALID_SOCKET || !fits_select(socks[i]))
			continue;
		FD_SET(socks[i], &readable);
		if ((int)socks[i] > max_fd)
			max_fd = (int)socks[i];
	}
	if (net->wake_sock != INVALID_SOCKET && fits_select(net->wake_sock))
	{
		FD_SET(net->wake_sock, &readable);
		if ((int)net->wake_sock > max_fd)
			max_fd = (int)net->wake_sock;
	}
	else if (usec < 0)
		/* No wake socket (it could not be made on this platform): nothing
		 * would end a wait without a time, so give it one and let the
		 * caller look at the stop flag again. */
		usec = 100 * 1000;

	if (usec >= 0)
	{
		tv.tv_sec = (long)(usec / 1000000);
		tv.tv_usec = (long)(usec % 1000000);
	}
	if (select(max_fd + 1, &readable, NULL, NULL, usec >= 0 ? &tv : NULL) <= 0)
		return 0;
	for (i = 0; i < count; i++)
		if (socks[i] != INVALID_SOCKET && fits_select(socks[i]) && FD_ISSET(socks[i], &readable))
			found++;
	return found;
}

void naomi_network_wait_stop(naomi_network *net, int64_t usec)
{
	wait_readable(net, NULL, 0, usec);
}

void naomi_network_wait_for_data(naomi_network *net)
{
	if (net_is_master(net))
		wait_readable(net, net->slaves, net->slave_count, -1);
	else
		wait_readable(net, &net->client_sock, 1, -1);
}

static bool create_server_socket(naomi_network *net)
{
	if (net->server_sock != INVALID_SOCKET)
		return true;

	net->server_sock = create_and_bind(IPPROTO_TCP);
	if (net->server_sock == INVALID_SOCKET)
		return false;

	if (listen(net->server_sock, 5) < 0)
	{
		ERROR_LOG(NETWORK, "NaomiServer: listen() failed. errno=%d\n", get_last_error());
		closesocket(net->server_sock);
		net->server_sock = INVALID_SOCKET;
		return false;
	}
	return true;
}

static bool create_beacon_socket(naomi_network *net)
{
	if (net->beacon_sock == INVALID_SOCKET)
		net->beacon_sock = create_and_bind(IPPROTO_UDP);

	return net->beacon_sock != INVALID_SOCKET;
}

bool naomi_network_init(naomi_network *net)
{
#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0)
	{
		ERROR_LOG(NETWORK, "WSAStartup failed. errno=%d\n", get_last_error());
		return false;
	}
#endif
	if (settings.network.ActAsServer)
	{
#ifdef ENABLE_MODEM
		miniupnp_init(&net->miniupnp);
		miniupnp_add_port_mapping(&net->miniupnp, NAOMI_NET_SERVER_PORT, true);
#endif
		return create_beacon_socket(net) && create_server_socket(net);
	}
	return true;
}

/* Broadcast queries on the beacon socket answered */
static void process_beacon(naomi_network *net)
{
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	char buf[6];
	ssize_t n;
	memset(&addr, 0, sizeof(addr));
	do {
		memset(buf, '\0', sizeof(buf));
		if ((n = recvfrom(net->beacon_sock, buf, sizeof(buf), 0, (struct sockaddr *)&addr, &addrlen)) == -1)
		{
			if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
				WARN_LOG(NETWORK, "NaomiServer: Error receiving datagram. errno=%d\n", get_last_error());
		}
		else
		{
			DEBUG_LOG(NETWORK, "NaomiServer: beacon received %ld bytes\n", (long)n);
			if (n == sizeof(buf) && !strncmp(buf, "flycast", n))
				sendto(net->beacon_sock, buf, n, MSG_NOSIGNAL, (const struct sockaddr *)&addr, addrlen);
		}
	} while (n != -1);
}

/* The ad hoc server on the local network, found by broadcast */
static bool find_server(naomi_network *net)
{
	int broadcast = 1;
	struct sockaddr_in addr;
	struct sockaddr server_addr;
	int i;
	sock_t sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == INVALID_SOCKET)
	{
		ERROR_LOG(NETWORK, "Datagram socket creation error. errno=%d\n", get_last_error());
		return false;
	}

	/* Allow broadcast packets to be sent */
	if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast, sizeof(broadcast)) == -1)
	{
		ERROR_LOG(NETWORK, "setsockopt(SO_BROADCAST) failed. errno=%d\n", get_last_error());
		closesocket(sockfd);
		return false;
	}

	set_non_blocking(sockfd);

	addr.sin_family = AF_INET;
	addr.sin_port = htons(NAOMI_NET_SERVER_PORT);
	addr.sin_addr.s_addr = INADDR_BROADCAST;
	memset(addr.sin_zero, '\0', sizeof(addr.sin_zero));

	for (i = 0; i < 3 && !net_stopping(net); i++)
	{
		char buf[6];
		socklen_t addrlen;
		if (sendto(sockfd, "flycast", 6, MSG_NOSIGNAL, (struct sockaddr *)&addr, sizeof addr) == -1)
		{
			WARN_LOG(NETWORK, "Send datagram failed. errno=%d\n", get_last_error());
			/* Try again in a tenth of a second */
			naomi_network_wait_stop(net, 100 * 1000);
			continue;
		}

		/* The answer, for up to a second */
		if (wait_readable(net, &sockfd, 1, 1000 * 1000) <= 0)
		{
			INFO_LOG(NETWORK, "Recv datagram timeout. i=%d\n", i);
			continue;
		}

		memset(&server_addr, '\0', sizeof(server_addr));
		addrlen = sizeof(server_addr);
		if (recvfrom(sockfd, buf, sizeof(buf), 0, &server_addr, &addrlen) == -1)
		{
			WARN_LOG(NETWORK, "Recv datagram failed. errno=%d\n", get_last_error());
			continue;
		}
		net->server_ip = ((struct sockaddr_in *)&server_addr)->sin_addr;
		inet_ntop(AF_INET, &net->server_ip, net->server_name, INET_ADDRSTRLEN);
		break;
	}
	closesocket(sockfd);
	if (net->server_ip.s_addr == INADDR_NONE)
	{
		WARN_LOG(NETWORK, "Network Error: Can't find ad-hoc server on local network\n");
		return false;
	}
	INFO_LOG(NETWORK, "Found ad-hoc server at %s\n", net->server_name);

	return true;
}

/* Ten seconds */
#define NET_TIMEOUT ((retro_time_t)10 * 1000 * 1000)

static bool start_master(naomi_network *net)
{
	retro_time_t start_time = cpu_features_get_time_usec();
	u8 buf[2];
	int i;

	NOTICE_LOG(NETWORK, "Waiting for slave connections\n");
	while (cpu_features_get_time_usec() - start_time < NET_TIMEOUT)
	{
		struct sockaddr_in src_addr;
		socklen_t addr_len = sizeof(src_addr);
		sock_t client;

		if (net_stopping(net))
		{
			for (i = 0; i < net->slave_count; i++)
				if (net->slaves[i] != INVALID_SOCKET)
					closesocket(net->slaves[i]);
			return false;
		}

		process_beacon(net);

		memset(&src_addr, 0, addr_len);
		client = accept(net->server_sock, (struct sockaddr *)&src_addr, &addr_len);
		if (client == INVALID_SOCKET)
		{
			if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
				perror("accept");
		}
		else
		{
			NOTICE_LOG(NETWORK, "Slave connection accepted\n");
			set_no_sigpipe(client);
			net->slaves[net->slave_count++] = client;
			if (net->slave_count == NAOMI_NET_SLAVES_MAX)
				break;
		}
		/* Until someone connects or asks who is serving, or time is up */
		{
			sock_t waiting[2];
			const retro_time_t left = NET_TIMEOUT - (cpu_features_get_time_usec() - start_time);
			waiting[0] = net->server_sock;
			waiting[1] = net->beacon_sock;
			if (left > 0)
				wait_readable(net, waiting, 2, left);
		}
	}
	net->slot_id = 0;
	net->slot_count = net->slave_count + 1;
	buf[0] = (u8)net->slot_count;
	for (i = 0; i < net->slave_count; i++)
	{
		buf[1] = (u8)(i + 1);
		send(net->slaves[i], (const char *)buf, 2, MSG_NOSIGNAL);
		set_non_blocking(net->slaves[i]);
		set_tcp_nodelay(net->slaves[i]);
	}
	NOTICE_LOG(NETWORK, "Master starting: %d slaves\n", net->slave_count);

	return net->slave_count != 0;
}

static bool start_slave(naomi_network *net)
{
	retro_time_t start_time;

	if (settings.network.server[0] != 0)
	{
		struct addrinfo *result_addr;
		if (getaddrinfo(settings.network.server, 0, NULL, &result_addr))
			WARN_LOG(NETWORK, "Server %s is unknown\n", settings.network.server);
		else
		{
			struct addrinfo *ptr;
			for (ptr = result_addr; ptr != NULL; ptr = ptr->ai_next)
				if (ptr->ai_family == AF_INET)
				{
					net->server_ip = ((struct sockaddr_in *)ptr->ai_addr)->sin_addr;
					break;
				}
			freeaddrinfo(result_addr);
		}
	}

	NOTICE_LOG(NETWORK, "Connecting to server\n");
	start_time = cpu_features_get_time_usec();
	while (net->client_sock == INVALID_SOCKET && !net_stopping(net)
			&& cpu_features_get_time_usec() - start_time < NET_TIMEOUT)
	{
		struct sockaddr_in src_addr;

		if (net->server_ip.s_addr == INADDR_NONE && !find_server(net))
			continue;

		net->client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (net->client_sock != INVALID_SOCKET)
			set_no_sigpipe(net->client_sock);
		src_addr.sin_family = AF_INET;
		src_addr.sin_addr = net->server_ip;
		src_addr.sin_port = htons(NAOMI_NET_SERVER_PORT);
		if (connect(net->client_sock, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0)
		{
			ERROR_LOG(NETWORK, "Socket connect failed\n");
			closesocket(net->client_sock);
			net->client_sock = INVALID_SOCKET;
			/* Try again in a tenth of a second */
			naomi_network_wait_stop(net, 100 * 1000);
		}
		else
		{
			/* Wait for the server to start the game, for up to twice the
			 * timeout. */
			u8 buf[2];
			int got = 0;
			retro_time_t wait_start = cpu_features_get_time_usec();
			while (got < 2 && !net_stopping(net))
			{
				ssize_t l;
				const retro_time_t left = NET_TIMEOUT * 2 - (cpu_features_get_time_usec() - wait_start);
				if (left <= 0 || wait_readable(net, &net->client_sock, 1, left) <= 0)
					break;
				l = recv(net->client_sock, (char *)buf + got, 2 - got, 0);
				if (l <= 0)
					break;
				got += (int)l;
			}
			if (got < 2)
			{
				ERROR_LOG(NETWORK, "Connection failed: errno=%d\n", get_last_error());
				closesocket(net->client_sock);
				net->client_sock = INVALID_SOCKET;
				return false;
			}
			net->slot_count = buf[0];
			net->slot_id = buf[1];
			net->got_token = net->slot_id == 1;
			set_tcp_nodelay(net->client_sock);
			set_non_blocking(net->client_sock);
			return true;
		}
	}
	return false;
}

bool naomi_network_start(naomi_network *net)
{
	if (!naomi_network_init(net))
		return false;

	net->slot_id = 0;
	net->slot_count = 0;
	net->packet_number = 0;
	net->slave_count = 0;
	net->got_token = false;

	if (settings.network.ActAsServer)
		return start_master(net);
	return start_slave(net);
}

void naomi_network_pipe_slaves(naomi_network *net)
{
	char buf[1024];
	int i;
	/* The slot numbers are the server's word: one that is told it is the
	 * master has no ring of slaves to pass anything round */
	if (!net_is_master(net) || net->slot_count < 3 || net->slave_count < 2)
		return;
	for (i = 0; i < net->slave_count - 1; i++)
	{
		/* (all of what has come in, a piece at a time) */
		ssize_t l;
		while ((l = recv(net->slaves[i], buf, sizeof(buf), 0)) > 0)
		{
			send(net->slaves[i + 1], buf, l, MSG_NOSIGNAL);
			if (l < (ssize_t)sizeof(buf))
				break;
		}
		/* TODO handle errors */
	}
}

/* The ring's socket that went wrong: dropped, by a master */
static void drop_socket(naomi_network *net, int slave, sock_t sockfd, bool lose_token)
{
	if (!net_is_master(net))
		return;
	net->slaves[slave] = INVALID_SOCKET;
	closesocket(sockfd);
	if (lose_token)
		net->got_token = false;
}

/* The rest of what was begun: the packet's number can come in two pieces
 * as well as what follows it. */
static bool receive_rest(naomi_network *net, sock_t sockfd, u8 *to, u32 count)
{
	u32 received = 0;
	while (received != count && !net_stopping(net))
	{
		ssize_t got = recv(sockfd, (char *)(to + received), count - received, 0);
		if (got <= 0)
		{
			if (got < 0 && (get_last_error() == L_EAGAIN || get_last_error() == L_EWOULDBLOCK))
			{
				/* The rest of the packet is on its way */
				wait_readable(net, &sockfd, 1, -1);
				continue;
			}
			WARN_LOG(NETWORK, "receiveNetwork: read failed. errno=%d\n", get_last_error());
			drop_socket(net, net->slave_count - 1, sockfd, true);
			return false;
		}
		received += (u32)got;
	}
	return received == count;
}

bool naomi_network_receive(naomi_network *net, u8 *data, u32 size)
{
	sock_t sockfd;
	u16 pktnum;
	ssize_t l;

	if (net_is_master(net))
		sockfd = net->slave_count == 0 ? INVALID_SOCKET : net->slaves[net->slave_count - 1];
	else
		sockfd = net->client_sock;
	if (sockfd == INVALID_SOCKET)
		return false;

	l = recv(sockfd, (char *)&pktnum, sizeof(pktnum), 0);
	if (l <= 0)
	{
		if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
		{
			WARN_LOG(NETWORK, "receiveNetwork: read failed. errno=%d\n", get_last_error());
			drop_socket(net, net->slave_count - 1, sockfd, true);
		}
		return false;
	}

	if (l < (ssize_t)sizeof(pktnum) && !receive_rest(net, sockfd, (u8 *)&pktnum + l, sizeof(pktnum) - (u32)l))
		return false;
	net->packet_number = pktnum;

	if (!receive_rest(net, sockfd, data, size))
		return false;
	DEBUG_LOG(NETWORK, "[%d] Received %d bytes\n", net->slot_id, size);
	net->got_token = true;
	return true;
}

void naomi_network_send(naomi_network *net, const u8 *data, u32 size)
{
	sock_t sockfd;
	u16 pktnum;

	if (!net->got_token)
		return;

	if (net_is_master(net))
		sockfd = net->slave_count == 0 ? INVALID_SOCKET : net->slaves[0];
	else
		sockfd = net->client_sock;
	if (sockfd == INVALID_SOCKET)
		return;

	pktnum = (u16)(net->packet_number + 1);
	if (send(sockfd, (const char *)&pktnum, sizeof(pktnum), MSG_NOSIGNAL) < 2)
	{
		if (errno != L_EAGAIN && errno != L_EWOULDBLOCK)
		{
			WARN_LOG(NETWORK, "send failed. errno=%d\n", get_last_error());
			drop_socket(net, 0, sockfd, false);
		}
		return;
	}
	if (send(sockfd, (const char *)data, size, MSG_NOSIGNAL) < (ssize_t)size)
	{
		WARN_LOG(NETWORK, "send failed. errno=%d\n", get_last_error());
		drop_socket(net, 0, sockfd, false);
	}
	else
	{
		DEBUG_LOG(NETWORK, "[%d] Sent %d bytes\n", net->slot_id, size);
		net->got_token = false;
		net->packet_number = pktnum;
	}
}

void naomi_network_shutdown(naomi_network *net)
{
	retro_atomic_store_release_int(&net->network_stopping, 1);
	/* Ends whatever wait the network thread is in */
	if (net->wake_sock != INVALID_SOCKET)
		send(net->wake_sock, "", 1, MSG_NOSIGNAL);
}

void naomi_network_close_sockets(naomi_network *net)
{
	int i;
	for (i = 0; i < net->slave_count; i++)
	{
		if (net->slaves[i] != INVALID_SOCKET)
			closesocket(net->slaves[i]);
		net->slaves[i] = INVALID_SOCKET;
	}
	if (net->client_sock != INVALID_SOCKET)
	{
		closesocket(net->client_sock);
		net->client_sock = INVALID_SOCKET;
	}
}

void naomi_network_terminate(naomi_network *net)
{
	naomi_network_shutdown(net);
	naomi_network_close_sockets(net);
#ifdef ENABLE_MODEM
	if (settings.network.ActAsServer)
		miniupnp_term(&net->miniupnp);
#endif
	if (net->beacon_sock != INVALID_SOCKET)
	{
		closesocket(net->beacon_sock);
		net->beacon_sock = INVALID_SOCKET;
	}
	if (net->server_sock != INVALID_SOCKET)
	{
		closesocket(net->server_sock);
		net->server_sock = INVALID_SOCKET;
	}
}
