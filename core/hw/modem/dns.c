/*
	Created on: Sep 24, 2018

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
#include "types.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "dns.h"

/* What a message is made of (RFC 1035 4.1), as bytes: the header, the
 * question's type and class after its name, and an answer's type, class,
 * time to live and length after its name. */
#define DNS_HEADER_SIZE 12
#define DNS_QSUFFIX_SIZE 4
#define DNS_RSUFFIX_SIZE 10
#define DNS_TYPE_A 1
#define DNS_CLASS_IN 1
/* (bit 8 of the flags: recursion is desired) */
#define DNS_FLAGS_RD 0x0100

static sock_t sock_fd = INVALID_SOCKET;
static unsigned short qid;
static int qname_len;

static void put16(u8 *p, unsigned v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

static unsigned get16(const u8 *p)
{
	return ((unsigned)p[0] << 8) | p[1];
}

int dns_query_build(u8 *buf, size_t size, u16 id, const char *name, int *name_len)
{
	size_t host_len = strlen(name);
	u8 *qname = buf + DNS_HEADER_SIZE;
	u8 *label = qname;
	size_t i;

	/* the name in labels is one byte longer than as it is written, with its
	 * zero byte at the end */
	if (host_len == 0 || host_len > DNS_NAME_MAX - 2
			|| size < DNS_HEADER_SIZE + host_len + 2 + DNS_QSUFFIX_SIZE)
		return 0;
	/* (the id as it was always sent: in the host's order) */
	memcpy(buf, &id, 2);
	put16(buf + 2, DNS_FLAGS_RD);
	put16(buf + 4, 1);		/* One question */
	put16(buf + 6, 0);
	put16(buf + 8, 0);
	put16(buf + 10, 0);

	/* each label behind its length, written where the name is copied to */
	for (i = 0; i <= host_len; i++)
	{
		const char c = name[i];
		if (c == '.' || c == '\0')
		{
			const size_t len = (size_t)(qname + 1 + i - label - 1);
			if (len == 0 || len > 63)
				return 0;
			*label = (u8)len;
			label = qname + 1 + i;
		}
		else
			qname[1 + i] = (u8)c;
	}
	*label = 0;
	*name_len = (int)host_len + 2;
	put16(qname + *name_len, DNS_TYPE_A);		/* Address record */
	put16(qname + *name_len + 2, DNS_CLASS_IN);
	return DNS_HEADER_SIZE + *name_len + DNS_QSUFFIX_SIZE;
}

void get_host_by_name(const char *host, u32 dnsaddr)
{
	struct sockaddr_in dest;
	u8 buf[DNS_HEADER_SIZE + DNS_NAME_MAX + DNS_QSUFFIX_SIZE];
	int len, name_len;

	DEBUG_LOG(MODEM, "get_host_by_name: %s\n", host);
	if (!VALID(sock_fd))
	{
		sock_fd = selectable_socket(socket(AF_INET , SOCK_DGRAM , IPPROTO_UDP));
		set_non_blocking(sock_fd);
		/* (the queries' ids start where the time says) */
		qid = (unsigned short)time(NULL);
	}
	len = dns_query_build(buf, sizeof(buf), qid++, host, &name_len);
	if (len == 0)
		return;
	qname_len = name_len;

	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(53);
	dest.sin_addr.s_addr = dnsaddr;

	if (sendto(sock_fd, (const char *)buf, len, MSG_NOSIGNAL, (struct sockaddr *)&dest, sizeof(dest)) < 0)
		perror("DNS sendto failed");
}

sock_t get_dns_socket(void)
{
	return sock_fd;
}

/* The length of the name at @p: labels, each behind its length, up to a zero
 * byte or to a pointer to an earlier name (two bytes). 0 if the name does
 * not end before @end. */
static int dns_name_len(const u8 *p, const u8 *end)
{
	const u8 *start = p;

	while (p < end)
	{
		if ((*p & 0xC0) == 0xC0)
			return end - p >= 2 ? (int)(p - start) + 2 : 0;
		if (*p == 0)
			return (int)(p - start) + 1;
		/* A label is 63 bytes at most: a length above that is of a kind
		 * not known here */
		if (*p & 0xC0)
			return 0;
		if (end - p <= *p)
			return 0;
		p += *p + 1;
	}
	return 0;
}

int dns_answer_parse(const u8 *msg, int len, int name_len, u32 *address)
{
	/* Every count, length and name in it is the sender's word: nothing is
	 * read past the bytes that were received. The answers are read where
	 * they were received. */
	const u8 *end = msg + len;
	const u8 *reader;
	unsigned ancount, i;

	if (len < DNS_HEADER_SIZE + name_len + DNS_QSUFFIX_SIZE)
		return -1;
	ancount = get16(msg + 6);
	/* move to the first answer */
	reader = msg + DNS_HEADER_SIZE + name_len + DNS_QSUFFIX_SIZE;

	for (i = 0; i < ancount; i++)
	{
		unsigned rtype, rdlength;
		/* FIXME Check name? */
		const int nlen = reader < end ? dns_name_len(reader, end) : 0;
		if (nlen == 0 || end - reader - nlen < DNS_RSUFFIX_SIZE)
			return -1;
		reader += nlen;
		rtype = get16(reader);
		rdlength = get16(reader + 8);
		reader += DNS_RSUFFIX_SIZE;
		if ((size_t)(end - reader) < rdlength)
			return -1;
		if (rtype == DNS_TYPE_A && rdlength >= 4) /* Address record */
		{
			memcpy(address, reader, 4);
			return 0;
		}
		reader += rdlength;
	}
	return -1;
}

int get_dns_answer(u32 *address, u32 dnsaddr)
{
	struct sockaddr_in peer;
	socklen_t peer_len = sizeof(peer);
	u8 buf[1024];
	int r;

	r = recvfrom(sock_fd, (char *)buf, sizeof(buf), 0, (struct sockaddr*)&peer , &peer_len);
	if (r < 0)
	{
		if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
			perror("DNS recvfrom failed");
		return -1;
	}
	if (peer.sin_addr.s_addr != dnsaddr)
		return -1;
	return dns_answer_parse(buf, r, qname_len, address);
}
