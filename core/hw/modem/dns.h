#pragma once
#include "types.h"
#include "network/net_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The modem's own name lookups, for what a game dials by name (dns.c):
 * a query for @name's IPv4 address goes to the DNS server at @dnsaddr, and
 * its answer is read when the socket has one. Addresses are as on the
 * wire, in network order. */
void get_host_by_name(const char *name, u32 dnsaddr);
/* 0 with the address in @address, -1 if there is none (yet) */
int get_dns_answer(u32 *address, u32 dnsaddr);
/* INVALID_SOCKET before the first query */
sock_t get_dns_socket(void);

/* The longest name asked for, in labels with its zero byte */
#define DNS_NAME_MAX 128

/* The query for @name's address into @buf, of @size bytes: its length, and
 * the name's in @name_len, or 0 for a name that does not make one */
int dns_query_build(u8 *buf, size_t size, u16 id, const char *name, int *name_len);
/* The first address answered in the message @msg of @len bytes to a query
 * whose name took @name_len bytes: 0 with it in @address, or -1 */
int dns_answer_parse(const u8 *msg, int len, int name_len, u32 *address);

#ifdef __cplusplus
}
#endif
