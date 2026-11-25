/* The network thread (core/network/picoppp.c + dcnet.c) under
 * ThreadSanitizer: picoTCP for the modem (an LCP request answered) and
 * the adapter (ARP), and DCNet against a stand-in relay on the loopback
 * interface (the name lookup of dcnet.flyca.st answered with 127.0.0.1);
 * and picoTCP's connections relayed for the adapter, the test a TCP of the
 * Dreamcast's own: both ways, slow readers at each end, half-closed by
 * either side, refused, from outside, and UDP. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <features/features_cpu.h>
#include <rthreads/rthreads.h>
#include <retro_timers.h>
#include <retro_atomic.h>

#include "types.h"
#include "reios/reios.h"
#include "network/picoppp.h"
#include "network/netservice.h"
#include "network/miniupnp.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

settings_t settings;
ip_meta_t ip_meta;
u8 net_device = NET_DEVICE_MODEM;
u8 net_device_option = NET_DEVICE_MODEM;
bool net_dcnet_option = false;
/* (no log) */
retro_log_printf_t log_cb;
bool miniupnp_init(miniupnp_t *u) { (void)u; return false; }
void miniupnp_term(miniupnp_t *u) { (void)u; }
bool miniupnp_add_port_mapping(miniupnp_t *u, int port, bool tcp) { (void)u; (void)port; (void)tcp; return false; }

/* ---- bytes: a buffer that grows ---- */
typedef struct bytes
{
	u8 *p;
	size_t n, cap;
} bytes;

static void bytes_room(bytes *b, size_t n)
{
	if (b->n + n > b->cap)
	{
		size_t cap = b->cap ? b->cap : 256;
		while (cap < b->n + n)
			cap *= 2;
		b->p   = (u8 *)realloc(b->p, cap);
		b->cap = cap;
	}
}

static void bytes_add(bytes *b, const void *src, size_t n)
{
	bytes_room(b, n);
	if (n)
		memcpy(b->p + b->n, src, n);
	b->n += n;
}

static void bytes_drop(bytes *b, size_t n)
{
	memmove(b->p, b->p + n, b->n - n);
	b->n -= n;
}

static void bytes_free(bytes *b)
{
	free(b->p);
	b->p = NULL;
	b->n = b->cap = 0;
}

static bool bytes_eq(const bytes *a, const bytes *b)
{
	return a->n == b->n && (a->n == 0 || !memcmp(a->p, b->p, a->n));
}

/* dcnet.flyca.st and dns.flyca.st are this machine */
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
	static struct sockaddr_in sa;
	static struct addrinfo ai;
	(void)node; (void)service; (void)hints;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	memset(&ai, 0, sizeof(ai));
	ai.ai_family = AF_INET;
	ai.ai_addr = (struct sockaddr *)&sa;
	ai.ai_addrlen = sizeof(sa);
	*res = &ai;
	return 0;
}
void freeaddrinfo(struct addrinfo *ai) { (void)ai; }

/* While set, the stream sockets the core makes have a small send buffer:
 * a reader that is slow to take what is sent soon leaves the relay with
 * bytes it has to keep */
static retro_atomic_int_t small_sndbuf;
int socket(int domain, int type, int protocol)
{
	int fd = (int)syscall(SYS_socket, domain, type, protocol);
	if (fd >= 0 && retro_atomic_load_acquire_int(&small_sndbuf) && (type & 0xf) == SOCK_STREAM)
	{
		int n = 4096;
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &n, sizeof(n));
	}
	return fd;
}

static u16 fcs16(const u8 *p, int len)
{
	u16 fcs = 0xffff;
	while (len--)
	{
		int i;
		fcs ^= *p++;
		for (i = 0; i < 8; i++)
			fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
	}
	return (u16)~fcs;
}
static void send_escaped(u8 b)
{
	if (b < 0x20 || b == 0x7d || b == 0x7e) { write_pico(0x7d); write_pico(b ^ 0x20); }
	else write_pico(b);
}

static void pico_modem(void)
{
	static const u8 lcp[] = { 0xff, 0x03, 0xc0, 0x21, 0x01, 0x01, 0x00, 0x04 };
	u16 fcs;
	retro_time_t t0, answered = 0, stopped;
	int got = 0;
	unsigned i;

	strcpy(settings.network.dns, "8.8.8.8");
	net_device = NET_DEVICE_MODEM;
	net_dcnet_option = false;
	CHECK(start_pico());
	retro_sleep(500);
	/* Passive: nothing before the Dreamcast speaks */
	CHECK(read_pico() < 0);
	fcs = fcs16(lcp, sizeof(lcp));
	t0 = cpu_features_get_time_usec();
	write_pico(0x7e);
	for (i = 0; i < sizeof(lcp); i++) send_escaped(lcp[i]);
	send_escaped(fcs & 0xff); send_escaped(fcs >> 8);
	write_pico(0x7e);
	while (cpu_features_get_time_usec() - t0 < 2 * 1000 * 1000)
	{
		if (read_pico() >= 0) { if (!got) answered = cpu_features_get_time_usec() - t0; got++; }
		else if (got) break;
	}
	CHECK(got > 0);
	CHECK(answered < 200 * 1000);
	t0 = cpu_features_get_time_usec();
	stop_pico();
	stopped = cpu_features_get_time_usec() - t0;
	/* (told, not waited for: a wait for the thread is a second's tick of
	 * its at least; a few milliseconds of a loaded machine are not one) */
	CHECK(stopped < 50 * 1000);
	printf("pico modem: %d bytes answered in %ld us, stop %ld us\n", got, (long)answered, (long)stopped);
}

static const u8 arp_req[60] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  0x08, 0x06,
	0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x01,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  192, 168, 169, 2,
	0, 0, 0, 0, 0, 0,                    192, 168, 169, 1,
};

static void pico_bba(void)
{
	int replies = 0;
	u8 frame[NET_ETH_FRAME_MAX];
	retro_time_t t0;

	net_device = NET_DEVICE_BBA;
	CHECK(start_pico());	/* (joins the last one) */
	retro_sleep(500);
	t0 = cpu_features_get_time_usec();
	pico_receive_eth_frame(arp_req, sizeof(arp_req));
	while (!replies && cpu_features_get_time_usec() - t0 < 2000000)
	{
		u32 n = net_eth_take(frame);
		if (n >= 42 && frame[12] == 0x08 && frame[13] == 0x06 && frame[21] == 2) replies++;
	}
	CHECK(replies == 1);
	t0 = cpu_features_get_time_usec();
	while (cpu_features_get_time_usec() - t0 < 1500 * 1000)
	{
		pico_receive_eth_frame(arp_req, sizeof(arp_req));
		net_eth_take(frame);
	}
	stop_pico();
	printf("pico bba: ARP answered\n");
}

/* ---- the relay ---- */
static retro_atomic_int_t relay_stop;
static int udp_fd, ppp_fd, tap_fd, smash_fd;
static retro_atomic_int_t modem_bytes_seen, frames_seen, prolog_seen;
static retro_atomic_int_t pings_seen, connections, closed;

#define LOAD(a) retro_atomic_load_acquire_int(&(a))
#define BUMP(a, n) retro_atomic_fetch_add_int(&(a), (n))

/* Power Smash's port: frames sent with a flag at their end only, and one
 * cut in two, and the connection kept until the other end closes it */
static void serve_smash(int c)
{
	static const char *const parts[] = { "abc~", "~def~gh", "i~" };
	unsigned i;
	for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
	{
		send(c, parts[i], strlen(parts[i]), MSG_NOSIGNAL);
		usleep(50000);
	}
	while (!LOAD(relay_stop))
	{
		fd_set r2;
		struct timeval t2 = { 0, 100000 };
		u8 buf[256];
		FD_ZERO(&r2); FD_SET(c, &r2);
		if (select(c + 1, &r2, NULL, NULL, &t2) <= 0) continue;
		if (recv(c, buf, sizeof(buf), 0) <= 0) break;
	}
	close(c);
}

static void serve(int c, bool tap)
{
	bytes in = { NULL, 0, 0 };
	while (!LOAD(relay_stop))
	{
		fd_set r2;
		struct timeval t2 = { 0, 100000 };
		u8 buf[4096];
		int n, i;
		FD_ZERO(&r2); FD_SET(c, &r2);
		if (select(c + 1, &r2, NULL, NULL, &t2) <= 0) continue;
		n = (int)recv(c, buf, sizeof(buf), 0);
		if (n <= 0) break;
		if (!tap)
		{
			/* PPP: echoed, each byte plus one */
			BUMP(modem_bytes_seen, n);
			for (i = 0; i < n; i++) buf[i]++;
			send(c, buf, n, MSG_NOSIGNAL);
			continue;
		}
		bytes_add(&in, buf, n);
		while (in.n >= 2)
		{
			unsigned len = in.p[0] | (in.p[1] << 8);
			if (in.n < 2 + len) break;
			if (len == 6 && !memcmp(in.p + 2, "DCNET\1", 6)) BUMP(prolog_seen, 1);
			else { BUMP(frames_seen, 1); send(c, in.p, 2 + len, MSG_NOSIGNAL); }	/* echoed */
			bytes_drop(&in, 2 + len);
		}
	}
	bytes_free(&in);
	BUMP(closed, 1);
	close(c);
}

static int max4(int a, int b, int c, int d)
{
	int m = a > b ? a : b;
	m = m > c ? m : c;
	return m > d ? m : d;
}

static void *relay(void *arg)
{
	(void)arg;
	while (!LOAD(relay_stop))
	{
		fd_set rd;
		struct timeval tv = { 0, 100000 };
		FD_ZERO(&rd); FD_SET(udp_fd, &rd); FD_SET(ppp_fd, &rd); FD_SET(tap_fd, &rd); FD_SET(smash_fd, &rd);
		if (select(max4(udp_fd, smash_fd, ppp_fd, tap_fd) + 1, &rd, NULL, NULL, &tv) <= 0) continue;
		if (FD_ISSET(udp_fd, &rd))
		{
			u8 buf[64];
			struct sockaddr_in from;
			socklen_t fl = sizeof(from);
			int n = (int)recvfrom(udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
			if (n == 5 && buf[4] == 3)
			{
				/* DISCOVER: two access points, this one and one that never
				 * answers (127.0.0.3): this one is chosen */
				u8 list[5 + 2 * (5 + 4)];
				u8 *p = list + 5;
				u32 a = htonl(0x7f000003);
				memcpy(list, buf, 5);
				memcpy(p, &a, 4); p[4] = 4; memcpy(p + 5, "dead", 4); p += 9;
				a = htonl(INADDR_LOOPBACK);
				memcpy(p, &a, 4); p[4] = 4; memcpy(p + 5, "live", 4); p += 9;
				sendto(udp_fd, list, p - list, 0, (struct sockaddr *)&from, fl);
			}
			else if (n == 13 && buf[4] == 1)
			{
				/* PING: a PONG with its time as it was */
				BUMP(pings_seen, 1);
				buf[4] = 2;
				sendto(udp_fd, buf, 13, 0, (struct sockaddr *)&from, fl);
			}
		}
		if (FD_ISSET(ppp_fd, &rd)) { int c = accept(ppp_fd, NULL, NULL); if (c >= 0) { BUMP(connections, 1); serve(c, false); } }
		if (FD_ISSET(tap_fd, &rd)) { int c = accept(tap_fd, NULL, NULL); if (c >= 0) { BUMP(connections, 1); serve(c, true); } }
		if (FD_ISSET(smash_fd, &rd)) { int c = accept(smash_fd, NULL, NULL); if (c >= 0) { BUMP(connections, 1); serve_smash(c); } }
	}
	return NULL;
}

static void loopback_addr(struct sockaddr_in *a, u16 port)
{
	memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a->sin_port = htons(port);
}

static int listen_on(int type, u16 port)
{
	int fd = socket(AF_INET, type, 0);
	int one = 1;
	struct sockaddr_in a;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	loopback_addr(&a, port);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) return -1;
	if (type == SOCK_STREAM && listen(fd, 4) < 0) return -1;
	return fd;
}

static bool open_relay(void)
{
	udp_fd = listen_on(SOCK_DGRAM, 7655);
	ppp_fd = listen_on(SOCK_STREAM, 7654);
	tap_fd = listen_on(SOCK_STREAM, 7655);
	smash_fd = listen_on(SOCK_STREAM, 7656);
	return udp_fd >= 0 && ppp_fd >= 0 && tap_fd >= 0 && smash_fd >= 0;
}

static void dcnet_modem(pthread_t *th)
{
	static const char msg[] = "~hello dcnet~";
	const char *m;
	bytes got = { NULL, 0, 0 };
	retro_time_t t0;
	bool same;
	size_t i;
	int sent = 0, recvd = 0;

	net_device = NET_DEVICE_MODEM;
	net_dcnet_option = true;
	pthread_create(th, NULL, relay, NULL);
	CHECK(start_pico());
	retro_sleep(300);
	for (m = msg; *m; m++) write_pico(*m);
	t0 = cpu_features_get_time_usec();
	while (got.n < strlen(msg) && cpu_features_get_time_usec() - t0 < 3000000)
	{
		int c = read_pico();
		if (c >= 0) { u8 b = (u8)c; bytes_add(&got, &b, 1); }
	}
	CHECK(got.n == strlen(msg));
	same = got.n == strlen(msg);
	for (i = 0; same && i < got.n; i++) same = got.p[i] == (u8)(msg[i] + 1);
	CHECK(same);
	bytes_free(&got);
	/* a lot both ways: the ring to the modem fills, and the thread waits for the modem */
	t0 = cpu_features_get_time_usec();
	while (cpu_features_get_time_usec() - t0 < 1500000)
	{
		int k;
		for (k = 0; k < 64; k++) { write_pico('a' + (sent % 26)); sent++; }
		for (k = 0; k < 32; k++) if (read_pico() >= 0) recvd++;
	}
	retro_sleep(200);
	while (read_pico() >= 0) recvd++;
	stop_pico();
	printf("dcnet modem: echo ok, %d sent, %d received (relay saw %d)\n", sent, recvd, LOAD(modem_bytes_seen));
	CHECK(recvd > 0);
	/* the access point that answered, five times */
	CHECK(LOAD(pings_seen) == 5);
}

/* Power Smash: its frames each with a flag at both ends, however they came */
static void dcnet_power_smash(void)
{
	static const char want[] = "~abc~~def~~ghi~";
	bytes got = { NULL, 0, 0 };
	retro_time_t t0;

	memcpy(ip_meta.product_number, "HDR-0113  ", 10);
	net_device = NET_DEVICE_MODEM;
	CHECK(start_pico());
	t0 = cpu_features_get_time_usec();
	while (got.n < strlen(want) && cpu_features_get_time_usec() - t0 < 3000000)
	{
		int c = read_pico();
		if (c >= 0) { u8 b = (u8)c; bytes_add(&got, &b, 1); }
	}
	stop_pico();
	memset(ip_meta.product_number, ' ', 10);
	CHECK(got.n == strlen(want) && !memcmp(got.p, want, strlen(want)));
	printf("dcnet power smash: %.*s\n", (int)got.n, got.n ? (const char *)got.p : "");
	bytes_free(&got);
}

static void dcnet_bba(void)
{
	u8 fr[100];
	u8 back[NET_ETH_FRAME_MAX];
	int got = 0, i;
	retro_time_t t0;

	net_device = NET_DEVICE_BBA;
	CHECK(start_pico());
	retro_sleep(100);
	for (i = 0; i < 100; i++) fr[i] = i;
	pico_receive_eth_frame(fr, sizeof(fr));
	t0 = cpu_features_get_time_usec();
	while (got < 50 && cpu_features_get_time_usec() - t0 < 3000000)
	{
		u32 n;
		if (got < 50 && (cpu_features_get_time_usec() - t0) % 1000 < 50) pico_receive_eth_frame(fr, sizeof(fr));
		n = net_eth_take(back);
		if (n == sizeof(fr) && !memcmp(back, fr, n)) got++;
	}
	CHECK(LOAD(prolog_seen) == 1);
	CHECK(got > 10);
	printf("dcnet bba: prolog %d, %d frames echoed (relay saw %d)\n", LOAD(prolog_seen), got, LOAD(frames_seen));
	/* the game's DHCP release: the connection is closed, and the next frame
	 * (within five seconds of the last connection) opens none */
	{
		static u8 release[0x120];
		int before, conns;
		memset(release, 0, sizeof(release));
		release[0xc] = 0x08; release[0xd] = 0x00;
		release[0x17] = 0x11;
		release[0x23] = 68;
		release[0x25] = 67;
		release[0x11a] = 53; release[0x11b] = 1; release[0x11c] = 7;	/* message type: release */
		release[0x11d] = 0xff;
		before = LOAD(closed);
		conns = LOAD(connections);
		pico_receive_eth_frame(release, sizeof(release));
		t0 = cpu_features_get_time_usec();
		while (LOAD(closed) == before && cpu_features_get_time_usec() - t0 < 2000000)
			retro_sleep(10);
		CHECK(LOAD(closed) == before + 1);
		pico_receive_eth_frame(fr, sizeof(fr));
		retro_sleep(300);
		CHECK(LOAD(connections) == conns);
		printf("dcnet bba: closed on a DHCP release, %d connections\n", LOAD(connections));
	}
	net_thread_join();
}


/* ---- the adapter's connections relayed: TCP both ways, UDP ----
 *
 * The test is the Dreamcast behind the broadband adapter, with a TCP of
 * its own a frame at a time, and the other ends are sockets of its own on
 * the loopback interface. What crosses must come out whole and in order
 * both ways, a slow reader at either end included (the relay keeps what
 * one end has not taken yet), and each side's close must come after all
 * it sent. */
static const u8 g_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static u8 p_mac[6];			/* the stack's, from its frames */
static const u8 g_ip[4] = { 192, 168, 169, 2 };

static u16 csum_add(u32 sum, const u8 *p, size_t n)
{
	size_t i;
	for (i = 0; i + 1 < n; i += 2)
		sum += (p[i] << 8) | p[i + 1];
	if (n & 1)
		sum += p[n - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (u16)sum;
}
static void put16(u8 *p, u16 v) { p[0] = v >> 8; p[1] = (u8)v; }
static void put32b(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (u8)v; }
static u16 get16(const u8 *p) { return (p[0] << 8) | p[1]; }
static u32 get32b(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

/* An IPv4 datagram of @proto from the guest to @dst, @payload after its
 * header (@len bytes, the TCP or UDP header among them) */
static void send_ip(const u8 *dst, u8 proto, const u8 *payload, size_t len)
{
	static u16 ipid;
	u8 f[NET_ETH_FRAME_MAX];
	u8 ph[12];
	u8 *ip = f + 14, *t;
	size_t at, n;
	u16 c;

	memcpy(f, p_mac, 6);
	memcpy(f + 6, g_mac, 6);
	f[12] = 0x08; f[13] = 0x00;
	memset(ip, 0, 20);
	ip[0] = 0x45;
	put16(ip + 2, (u16)(20 + len));
	put16(ip + 4, ipid++);
	ip[8] = 64;
	ip[9] = proto;
	memcpy(ip + 12, g_ip, 4);
	memcpy(ip + 16, dst, 4);
	put16(ip + 10, (u16)~csum_add(0, ip, 20));
	memcpy(ip + 20, payload, len);
	/* the transport checksum over the pseudo header */
	memcpy(ph, g_ip, 4); memcpy(ph + 4, dst, 4); ph[8] = 0; ph[9] = proto; put16(ph + 10, (u16)len);
	t = ip + 20;
	at = proto == 6 ? 16 : 6;
	t[at] = t[at + 1] = 0;
	c = (u16)~csum_add(csum_add(0, ph, 12), t, len);
	if (proto == 17 && c == 0)
		c = 0xffff;
	put16(t + at, c);
	n = 14 + 20 + len;
	if (n < 60) { memset(f + n, 0, 60 - n); n = 60; }
	pico_receive_eth_frame(f, (u32)n);
}

typedef struct gtcp
{
	u8 dst[4];
	u16 lport, rport;
	u32 snd_una, snd_nxt, rcv_nxt;
	u32 peer_wnd;
	u16 my_wnd;			/* what is advertised */
	bool established, fin_rcvd, fin_sent, rst;
	bytes rx;			/* what arrived, in order */
	bytes tx;			/* what is to be sent, from snd_una's place on */
	size_t tx_base;		/* tx[0]'s sequence offset from the first data byte */
	u32 iss, irs;
	retro_time_t last_progress;
	bool passive;			/* the other end opened it */
} gtcp;

static void tcp_seg(gtcp *c, u8 flags, u32 seq, const u8 *data, size_t len)
{
	u8 t[20 + 1460];
	memset(t, 0, 20);
	put16(t, c->lport);
	put16(t + 2, c->rport);
	put32b(t + 4, seq);
	put32b(t + 8, (flags & 0x10) ? c->rcv_nxt : 0);
	t[12] = 5 << 4;
	t[13] = flags;
	put16(t + 14, c->my_wnd);
	if (len)
		memcpy(t + 20, data, len);
	send_ip(c->dst, 6, t, 20 + len);
}

#define MAX_CONNS 16
#define MAX_UDP   16
static gtcp *g_conns[MAX_CONNS];
static size_t g_nconns;
static bytes g_udp_in[MAX_UDP];	/* UDP datagrams to the guest: port (2), data */
static size_t g_nudp;
static int g_syns;				/* SYNs from the stack to ports nobody listens on */
static u16 g_listen_port;			/* the guest's open port, for connections from outside */

static gtcp *new_conn(void)
{
	gtcp *c = (gtcp *)calloc(1, sizeof(*c));
	if (g_nconns < MAX_CONNS)
		g_conns[g_nconns++] = c;
	return c;
}

static gtcp *find_conn(u16 lport, u16 rport, const u8 *src)
{
	size_t i;
	for (i = 0; i < g_nconns; i++)
		if (g_conns[i]->lport == lport && g_conns[i]->rport == rport && !memcmp(g_conns[i]->dst, src, 4))
			return g_conns[i];
	return NULL;
}

/* Every frame the stack has for the guest, handled */
static void guest_poll(void)
{
	u8 f[NET_ETH_FRAME_MAX];
	u32 n;
	while ((n = net_eth_take(f)) != 0)
	{
		const u8 *ip, *t, *data;
		size_t ihl, total, thl, dlen;
		u8 flags;
		u32 seq, ack;
		gtcp *c;
		bool ack_it = false;

		if (n >= 42 && f[12] == 0x08 && f[13] == 0x06)
		{
			memcpy(p_mac, f + 6, 6);
			/* a request for the guest's address: answered */
			if (f[21] == 1 && !memcmp(f + 38, g_ip, 4))
			{
				u8 r[60];
				memset(r, 0, sizeof(r));
				memcpy(r, f + 6, 6); memcpy(r + 6, g_mac, 6);
				r[12] = 0x08; r[13] = 0x06;
				r[14] = 0; r[15] = 1; r[16] = 8; r[17] = 0; r[18] = 6; r[19] = 4; r[20] = 0; r[21] = 2;
				memcpy(r + 22, g_mac, 6); memcpy(r + 28, g_ip, 4);
				memcpy(r + 32, f + 22, 6); memcpy(r + 38, f + 28, 4);
				pico_receive_eth_frame(r, sizeof(r));
			}
			continue;
		}
		if (n < 34 || f[12] != 0x08 || f[13] != 0x00)
			continue;
		memcpy(p_mac, f + 6, 6);
		ip = f + 14;
		ihl = (ip[0] & 15) * 4;
		total = get16(ip + 2);
		t = ip + ihl;
		if (ip[9] == 17)
		{
			/* (the source port, then the data) */
			if (g_nudp < MAX_UDP)
			{
				bytes *d = &g_udp_in[g_nudp++];
				bytes_add(d, t, 2);
				bytes_add(d, t + 8, (ip + total) - (t + 8));
			}
			continue;
		}
		if (ip[9] != 6)
			continue;
		thl = (t[12] >> 4) * 4;
		flags = t[13];
		seq = get32b(t + 4);
		ack = get32b(t + 8);
		data = t + thl;
		dlen = ip + total - data;
		c = find_conn(get16(t + 2), get16(t), ip + 12);
		if (c == NULL)
		{
			if ((flags & 0x12) == 0x02 && get16(t + 2) == g_listen_port && g_listen_port)
			{
				/* a connection from outside: answered */
				c = new_conn();
				memcpy(c->dst, ip + 12, 4);
				c->lport = get16(t + 2);
				c->rport = get16(t);
				c->my_wnd = 8192;
				c->passive = true;
				c->iss = 0x5000;
				c->irs = seq;
				c->rcv_nxt = seq + 1;
				c->snd_una = c->iss;
				c->snd_nxt = c->iss + 1;
				c->peer_wnd = get16(t + 14);
				c->tx_base = 0;
				c->last_progress = cpu_features_get_time_usec();
				tcp_seg(c, 0x12, c->iss, NULL, 0);
			}
			else if (flags & 0x02)
				g_syns++;
			continue;
		}
		if (flags & 0x04)
		{
			c->rst = true;
			continue;
		}
		if (!c->established && (flags & 0x12) == 0x12)
		{
			c->established = true;
			c->irs = seq;
			c->rcv_nxt = seq + 1;
			c->snd_una = ack;
			c->peer_wnd = get16(t + 14);
			tcp_seg(c, 0x10, c->snd_nxt, NULL, 0);
			continue;
		}
		if (c->passive && !c->established && (flags & 0x10) && ack == c->iss + 1)
		{
			c->established = true;
			c->snd_una = ack;
		}
		if (flags & 0x10)
		{
			if ((s32)(ack - c->snd_una) > 0 && (s32)(ack - c->snd_nxt) <= 0)
			{
				c->snd_una = ack;
				c->last_progress = cpu_features_get_time_usec();
			}
			c->peer_wnd = get16(t + 14);
		}
		if (dlen > 0 || (flags & 0x01))
		{
			if (seq == c->rcv_nxt)
			{
				/* (no more than the window: what is past it is sent again) */
				size_t take = dlen;
				bytes_add(&c->rx, data, take);
				c->rcv_nxt += (u32)take;
				if ((flags & 0x01) && take == dlen)
				{
					c->fin_rcvd = true;
					c->rcv_nxt++;
				}
			}
			ack_it = true;
		}
		if (ack_it)
			tcp_seg(c, 0x10, c->snd_nxt, NULL, 0);
	}
}

/* What the guest has to send: as much as the window lets, and from the
 * first byte not acknowledged again when nothing has moved for a while */
static void guest_push(gtcp *c)
{
	const u32 first_data = c->iss + 1;
	if (!c->established)
		return;
	if (cpu_features_get_time_usec() - c->last_progress > 300000 && c->snd_nxt != c->snd_una)
	{
		c->snd_nxt = c->snd_una;
		c->last_progress = cpu_features_get_time_usec();
	}
	for (;;)
	{
		const size_t off = c->snd_nxt - first_data;	/* in the stream */
		const size_t end = c->tx_base + c->tx.n;
		const u32 in_flight = c->snd_nxt - c->snd_una;
		size_t len;
		if (off >= end || in_flight >= c->peer_wnd)
			break;
		len = end - off;
		if (len > 1000) len = 1000;
		if (len > c->peer_wnd - in_flight) len = c->peer_wnd - in_flight;
		/* (the close on the last of it, so that the stack has it while
		 * there is still some to read) */
		if (c->fin_sent && off + len == end)
		{
			tcp_seg(c, 0x19, c->snd_nxt, c->tx.p + (off - c->tx_base), len);
			c->snd_nxt += (u32)len + 1;
			break;
		}
		tcp_seg(c, 0x18, c->snd_nxt, c->tx.p + (off - c->tx_base), len);
		c->snd_nxt += (u32)len;
	}
	if (c->fin_sent && c->snd_nxt == first_data + c->tx_base + c->tx.n + 1)
		return;
	/* the close, after all of it */
	if (c->fin_sent && c->snd_nxt == first_data + c->tx_base + c->tx.n)
	{
		tcp_seg(c, 0x11, c->snd_nxt, NULL, 0);
		c->snd_nxt++;
	}
	else if (c->fin_sent && c->snd_una == first_data + c->tx_base + c->tx.n + 1)
		;	/* acknowledged */
	else if (c->fin_sent && c->snd_nxt == first_data + c->tx_base + c->tx.n + 1
			&& cpu_features_get_time_usec() - c->last_progress > 300000)
	{
		c->snd_nxt--;
		c->last_progress = cpu_features_get_time_usec();
	}
}

static gtcp *guest_connect(const u8 *dst, u16 port, u16 lport, u16 wnd)
{
	gtcp *c = new_conn();
	memcpy(c->dst, dst, 4);
	c->lport = lport;
	c->rport = port;
	c->my_wnd = wnd;
	c->iss = 0x1000 * lport;
	c->snd_una = c->iss;
	c->snd_nxt = c->iss + 1;
	c->tx_base = 0;
	c->last_progress = cpu_features_get_time_usec();
	tcp_seg(c, 0x02, c->iss, NULL, 0);
	return c;
}

/* What guest_run() waits for */
typedef bool (*guest_done_fn)(gtcp *c);
static bool never(gtcp *c) { (void)c; return false; }
static bool is_established(gtcp *c) { return c->established; }
static bool got_fin(gtcp *c) { return c->fin_rcvd; }
static bool refused(gtcp *c) { return c->rst || c->fin_rcvd; }
/* all sent, and the close, acknowledged */
static bool all_acked(gtcp *c) { return c->snd_una == c->iss + 1 + c->tx.n + 1; }
static bool udp_arrived(gtcp *c) { (void)c; return g_nudp != 0; }

/* The guest's side runs until @done says so of @c, or @ms */
static bool guest_run(guest_done_fn done, gtcp *c, int ms)
{
	retro_time_t t0 = cpu_features_get_time_usec(), syn = t0;
	while (!done(c))
	{
		size_t i;
		if (cpu_features_get_time_usec() - t0 > (retro_time_t)ms * 1000)
			return false;
		guest_poll();
		for (i = 0; i < g_nconns; i++)
		{
			gtcp *k = g_conns[i];
			/* a SYN not answered: sent again */
			if (!k->established && !k->passive && !k->rst && cpu_features_get_time_usec() - syn > 300000)
				tcp_seg(k, 0x02, k->iss, NULL, 0);
			guest_push(k);
		}
		if (cpu_features_get_time_usec() - syn > 300000)
			syn = cpu_features_get_time_usec();
		retro_sleep(1);
	}
	return true;
}

static const u8 loopback[4] = { 127, 0, 0, 1 };
static int tcp_server(u16 port, int rcvbuf)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	struct sockaddr_in a;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (rcvbuf) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	loopback_addr(&a, port);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) { close(fd); return -1; }
	return fd;
}
static int accept_within(int lfd, int ms)
{
	fd_set r;
	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	FD_ZERO(&r); FD_SET(lfd, &r);
	if (select(lfd + 1, &r, NULL, NULL, &tv) <= 0) return -1;
	return accept(lfd, NULL, NULL);
}
/* All a socket has until its end, or @ms without anything, after @got */
static void read_all(int fd, int ms, bool *eof, bytes *got)
{
	*eof = false;
	for (;;)
	{
		fd_set r;
		struct timeval tv;
		u8 buf[4096];
		int n;
		tv.tv_sec = ms / 1000;
		tv.tv_usec = (ms % 1000) * 1000;
		FD_ZERO(&r); FD_SET(fd, &r);
		if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) break;
		n = (int)recv(fd, buf, sizeof(buf), 0);
		if (n <= 0) { *eof = n == 0; break; }
		bytes_add(got, buf, n);
	}
}
static void pattern(bytes *v, size_t n, u32 seed)
{
	size_t i;
	v->n = 0;
	bytes_room(v, n);
	v->n = n;
	for (i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; v->p[i] = (u8)(seed >> 16); }
}

typedef struct slow_reader { int fd; int delay_ms; bytes got; bool eof; } slow_reader;
static void *slow_read(void *arg)
{
	slow_reader *s = (slow_reader *)arg;
	retro_sleep(s->delay_ms);
	read_all(s->fd, 3000, &s->eof, &s->got);
	return NULL;
}

static void pico_bba_relay(void)
{
	size_t i;

	net_device = NET_DEVICE_BBA;
	net_dcnet_option = false;
	memcpy(ip_meta.product_number, "T22904N   ", 10);	/* Worms World Party: TCP 17219 open */
	g_listen_port = 17219;
	/* (the relay's own sockets made with small send buffers: see socket()) */
	retro_atomic_store_release_int(&small_sndbuf, 1);
	CHECK(start_pico());
	retro_sleep(300);
	/* the stack's address: from the answer to an ARP of the guest's */
	{
		retro_time_t t0 = cpu_features_get_time_usec();
		while (p_mac[0] == 0 && p_mac[5] == 0 && cpu_features_get_time_usec() - t0 < 3000000)
		{
			pico_receive_eth_frame(arp_req, sizeof(arp_req));
			retro_sleep(20);
			guest_poll();
		}
		CHECK(p_mac[0] || p_mac[5]);
	}

	/* 1. the guest sends 300 KB to a reader that waits a second and has a
	 * small buffer, and closes: all of it arrives before the end; the other
	 * end answers with 200 KB and closes, the guest's window small */
	{
		int lfd = tcp_server(17301, 4096);
		int fd;
		gtcp *c;
		bytes down = { NULL, 0, 0 };
		slow_reader sr;
		pthread_t th;
		size_t sent = 0;

		CHECK(lfd >= 0);
		c = guest_connect(loopback, 17301, 40001, 2048);
		CHECK(guest_run(is_established, c, 8000));
		fd = accept_within(lfd, 2000);
		CHECK(fd >= 0);
		pattern(&c->tx, 300 * 1024, 1);
		c->fin_sent = true;
		memset(&sr, 0, sizeof(sr));
		sr.fd = fd;
		sr.delay_ms = 1000;
		pthread_create(&th, NULL, slow_read, &sr);
		guest_run(all_acked, c, 15000);
		pthread_join(th, NULL);
		CHECK(bytes_eq(&sr.got, &c->tx));
		CHECK(sr.eof);
		pattern(&down, 200 * 1024, 2);
		while (sent < down.n)
		{
			int n = (int)send(fd, down.p + sent, down.n - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (n > 0) sent += n;
			guest_run(never, c, 5);
		}
		shutdown(fd, SHUT_WR);
		CHECK(guest_run(got_fin, c, 15000));
		CHECK(bytes_eq(&c->rx, &down));
		printf("relay tcp: %zu bytes up (slow reader), %zu down (small window), both closed after\n", sr.got.n, c->rx.n);
		bytes_free(&sr.got);
		bytes_free(&down);
		close(fd);
		close(lfd);
	}

	/* 1b. a little sent and closed at once, before the relay's own
	 * connection is made: it all arrives, then the end */
	{
		int lfd = tcp_server(17303, 0);
		int fd;
		gtcp *c;
		bool eof = false;
		bytes got = { NULL, 0, 0 };

		CHECK(lfd >= 0);
		c = guest_connect(loopback, 17303, 40003, 8192);
		pattern(&c->tx, 100, 5);
		c->fin_sent = true;
		CHECK(guest_run(all_acked, c, 5000));
		fd = accept_within(lfd, 2000);
		CHECK(fd >= 0);
		if (fd >= 0)
			read_all(fd, 2000, &eof, &got);
		CHECK(bytes_eq(&got, &c->tx));
		CHECK(eof);
		printf("relay tcp: %zu bytes sent before the connection was made, then the end\n", got.n);
		bytes_free(&got);
		if (fd >= 0)
			close(fd);
		close(lfd);
	}

	/* 1c. sent and closed while the far end reads nothing: the relay has
	 * it all, the close with it, before the far end reads a byte; then it
	 * all arrives, and the end only after it */
	{
		int lfd = tcp_server(17304, 4096);
		int fd;
		gtcp *c;
		bool eof = false;
		bytes got = { NULL, 0, 0 };

		CHECK(lfd >= 0);
		c = guest_connect(loopback, 17304, 40004, 8192);
		CHECK(guest_run(is_established, c, 8000));
		fd = accept_within(lfd, 2000);
		CHECK(fd >= 0);
		pattern(&c->tx, 24 * 1024, 6);
		c->fin_sent = true;
		CHECK(guest_run(all_acked, c, 8000));
		if (fd >= 0)
			read_all(fd, 3000, &eof, &got);
		CHECK(bytes_eq(&got, &c->tx));
		CHECK(eof);
		printf("relay tcp: %zu bytes and the close kept for a far end reading nothing, then the end\n", got.n);
		bytes_free(&got);
		if (fd >= 0)
			close(fd);
		close(lfd);
	}

	/* 2. a port nobody listens on: the guest's connection is refused */
	{
		gtcp *c = guest_connect(loopback, 17399, 40002, 8192);
		CHECK(guest_run(refused, c, 5000));
		printf("relay tcp: refused (%s)\n", c->rst ? "reset" : "closed");
	}

	/* 3. a connection from outside to the game's port: the guest answers,
	 * both ways, the outside closing its side first and reading on */
	{
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		int rb = 4096;
		struct sockaddr_in a;
		gtcp *c = NULL;
		retro_time_t t0;

		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
		loopback_addr(&a, 17219);
		CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
		t0 = cpu_features_get_time_usec();
		while (c == NULL && cpu_features_get_time_usec() - t0 < 8000000)
		{
			guest_run(never, NULL, 5);
			for (i = 0; i < g_nconns; i++)
				if (g_conns[i]->passive && g_conns[i]->established)
					c = g_conns[i];
		}
		CHECK(c != NULL);
		if (c != NULL)
		{
			bytes in = { NULL, 0, 0 }, out = { NULL, 0, 0 }, got = { NULL, 0, 0 };
			bool eof;

			pattern(&in, 50000, 3);
			pattern(&out, 300 * 1024, 4);
			send(fd, in.p, in.n, MSG_NOSIGNAL);
			shutdown(fd, SHUT_WR);
			/* (the guest answers once the outside's end has reached it: the
			 * outside still reads what comes after it closed its side) */
			CHECK(guest_run(got_fin, c, 10000));
			bytes_add(&c->tx, out.p, out.n);
			c->fin_sent = true;
			t0 = cpu_features_get_time_usec();
			/* (the outside slow to read: the relay keeps what it has not
			 * taken, past the stack's end of the connection) */
			guest_run(never, c, 1000);
			while (cpu_features_get_time_usec() - t0 < 15000000 && !(c->fin_rcvd && got.n >= out.n))
			{
				u8 buf[4096];
				int n;
				guest_run(never, c, 5);
				n = (int)recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
				if (n > 0) bytes_add(&got, buf, n);
			}
			read_all(fd, 2000, &eof, &got);
			CHECK(bytes_eq(&c->rx, &in));
			CHECK(c->fin_rcvd);
			CHECK(bytes_eq(&got, &out));
			if (!bytes_eq(&got, &out)) printf("DBG una %u nxt %u wnd %u rst %d\n", c->snd_una - c->iss - 1, c->snd_nxt - c->iss - 1, c->peer_wnd, c->rst);
			printf("relay tcp from outside: %zu in, %zu out\n", c->rx.n, got.n);
			bytes_free(&in);
			bytes_free(&out);
			bytes_free(&got);
		}
		close(fd);
	}

	/* 4. UDP: a datagram out, an answer back, from the port it went to */
	{
		int ufd = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in a, from;
		socklen_t fl = sizeof(from);
		u8 u[8 + 32];
		u8 buf[64];
		fd_set r;
		struct timeval tv = { 2, 0 };
		int n;

		loopback_addr(&a, 17302);
		CHECK(bind(ufd, (struct sockaddr *)&a, sizeof(a)) == 0);
		memset(u, 0, sizeof(u));
		put16(u, 40010); put16(u + 2, 17302); put16(u + 4, sizeof(u));
		memcpy(u + 8, "datagram from the dreamcast ....", 32);
		send_ip(loopback, 17, u, sizeof(u));
		FD_ZERO(&r); FD_SET(ufd, &r);
		n = select(ufd + 1, &r, NULL, NULL, &tv) > 0 ? (int)recvfrom(ufd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl) : -1;
		CHECK(n == 32 && !memcmp(buf, u + 8, 32));
		/* (from the port the Dreamcast sent from, which was to be had) */
		CHECK(n > 0 && ntohs(from.sin_port) == 40010);
		if (n > 0)
			sendto(ufd, "answer", 6, 0, (struct sockaddr *)&from, fl);
		for (i = 0; i < g_nudp; i++)
			bytes_free(&g_udp_in[i]);
		g_nudp = 0;
		CHECK(guest_run(udp_arrived, NULL, 3000));
		CHECK(g_nudp != 0 && g_udp_in[0].n == 8 && get16(g_udp_in[0].p) == 17302 && !memcmp(g_udp_in[0].p + 2, "answer", 6));
		printf("relay udp: out %d, back %zu\n", n, g_nudp == 0 ? (size_t)0 : g_udp_in[0].n - 2);
		close(ufd);
	}

	retro_atomic_store_release_int(&small_sndbuf, 0);
	stop_pico();
	net_thread_join();
	for (i = 0; i < g_nconns; i++)
	{
		bytes_free(&g_conns[i]->rx);
		bytes_free(&g_conns[i]->tx);
		free(g_conns[i]);
	}
	g_nconns = 0;
	for (i = 0; i < g_nudp; i++)
		bytes_free(&g_udp_in[i]);
	g_nudp = 0;
	g_listen_port = 0;
	memset(ip_meta.product_number, ' ', 10);
}

int main(void)
{
	pthread_t th;

	retro_atomic_int_init(&small_sndbuf, 0);
	retro_atomic_int_init(&relay_stop, 0);
	retro_atomic_int_init(&modem_bytes_seen, 0);
	retro_atomic_int_init(&frames_seen, 0);
	retro_atomic_int_init(&prolog_seen, 0);
	retro_atomic_int_init(&pings_seen, 0);
	retro_atomic_int_init(&connections, 0);
	retro_atomic_int_init(&closed, 0);
	pico_modem();
	pico_bba();
	pico_bba_relay();
	if (!open_relay()) { printf("no relay ports\n"); return 1; }
	dcnet_modem(&th);
	dcnet_power_smash();
	dcnet_bba();
	retro_atomic_store_release_int(&relay_stop, 1);
	pthread_join(th, NULL);
	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures != 0;
}
