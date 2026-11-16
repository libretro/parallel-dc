/* The network thread (core/network/picoppp.c + dcnet.c) under
 * ThreadSanitizer: picoTCP for the modem (an LCP request answered) and
 * the adapter (ARP), and DCNet against a stand-in relay on the loopback
 * interface (the name lookup of dcnet.flyca.st answered with 127.0.0.1);
 * and picoTCP's connections relayed for the adapter, the test a TCP of the
 * Dreamcast's own: both ways, slow readers at each end, half-closed by
 * either side, refused, from outside, and UDP. */
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <vector>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <features/features_cpu.h>
#include <rthreads/rthreads.h>
#include <retro_timers.h>

#include "types.h"
#include "stdclass.h"
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
bool miniupnp_init(miniupnp_t *) { return false; }
void miniupnp_term(miniupnp_t *) {}
bool miniupnp_add_port_mapping(miniupnp_t *, int, bool) { return false; }

// dcnet.flyca.st and dns.flyca.st are this machine
extern "C" int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
	static struct sockaddr_in sa;
	static struct addrinfo ai;
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
extern "C" void freeaddrinfo(struct addrinfo *) {}

/* While set, the stream sockets the core makes have a small send buffer:
 * a reader that is slow to take what is sent soon leaves the relay with
 * bytes it has to keep */
static std::atomic<int> small_sndbuf(0);
extern "C" int socket(int domain, int type, int protocol)
{
	int fd = (int)syscall(SYS_socket, domain, type, protocol);
	if (fd >= 0 && small_sndbuf && (type & 0xf) == SOCK_STREAM)
	{
		int n = 4096;
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &n, sizeof(n));
	}
	return fd;
}

static u16 fcs16(const u8 *p, int len)
{
	u16 fcs = 0xffff;
	while (len--) { fcs ^= *p++; for (int i = 0; i < 8; i++) fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1; }
	return (u16)~fcs;
}
static void send_escaped(u8 b)
{
	if (b < 0x20 || b == 0x7d || b == 0x7e) { write_pico(0x7d); write_pico(b ^ 0x20); }
	else write_pico(b);
}

static void pico_modem()
{
	static const u8 lcp[] = { 0xff, 0x03, 0xc0, 0x21, 0x01, 0x01, 0x00, 0x04 };
	strcpy(settings.network.dns, "8.8.8.8");
	net_device = NET_DEVICE_MODEM;
	net_dcnet_option = false;
	CHECK(start_pico());
	retro_sleep(500);
	// Passive: nothing before the Dreamcast speaks
	CHECK(read_pico() < 0);
	u16 fcs = fcs16(lcp, sizeof(lcp));
	retro_time_t t0 = cpu_features_get_time_usec(), answered = 0;
	int got = 0;
	write_pico(0x7e);
	for (unsigned i = 0; i < sizeof(lcp); i++) send_escaped(lcp[i]);
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
	retro_time_t stopped = cpu_features_get_time_usec() - t0;
	/* (told, not waited for: a wait for the thread is a second's tick of
	 * its at least; a few milliseconds of a loaded machine are not one) */
	CHECK(stopped < 50 * 1000);
	printf("pico modem: %d bytes answered in %ld us, stop %ld us\n", got, (long)answered, (long)stopped);
}

static void pico_bba()
{
	static const u8 arp[60] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  0x08, 0x06,
		0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x01,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  192, 168, 169, 2,
		0, 0, 0, 0, 0, 0,                    192, 168, 169, 1,
	};
	net_device = NET_DEVICE_BBA;
	CHECK(start_pico());	// (joins the last one)
	retro_sleep(500);
	int replies = 0;
	u8 frame[NET_ETH_FRAME_MAX];
	retro_time_t t0 = cpu_features_get_time_usec();
	pico_receive_eth_frame(arp, sizeof(arp));
	while (!replies && cpu_features_get_time_usec() - t0 < 2000000)
	{
		u32 n = net_eth_take(frame);
		if (n >= 42 && frame[12] == 0x08 && frame[13] == 0x06 && frame[21] == 2) replies++;
	}
	CHECK(replies == 1);
	t0 = cpu_features_get_time_usec();
	while (cpu_features_get_time_usec() - t0 < 1500 * 1000)
	{
		pico_receive_eth_frame(arp, sizeof(arp));
		net_eth_take(frame);
	}
	stop_pico();
	printf("pico bba: ARP answered\n");
}

/* ---- the relay ---- */
static std::atomic<int> relay_stop(0);
static int udp_fd, ppp_fd, tap_fd, smash_fd;
static std::atomic<int> modem_bytes_seen(0), frames_seen(0), prolog_seen(0);
static std::atomic<int> pings_seen(0), connections(0), closed(0);

/* Power Smash's port: frames sent with a flag at their end only, and one
 * cut in two, and the connection kept until the other end closes it */
static void serve_smash(int c)
{
	static const char *const parts[] = { "abc~", "~def~gh", "i~" };
	for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
	{
		send(c, parts[i], strlen(parts[i]), MSG_NOSIGNAL);
		usleep(50000);
	}
	while (!relay_stop)
	{
		fd_set r2; FD_ZERO(&r2); FD_SET(c, &r2);
		timeval t2 = { 0, 100000 };
		if (select(c + 1, &r2, NULL, NULL, &t2) <= 0) continue;
		u8 buf[256];
		if (recv(c, buf, sizeof(buf), 0) <= 0) break;
	}
	close(c);
}

static void serve(int c, bool tap)
{
	std::vector<u8> in;
	while (!relay_stop)
	{
		fd_set r2; FD_ZERO(&r2); FD_SET(c, &r2);
		timeval t2 = { 0, 100000 };
		if (select(c + 1, &r2, NULL, NULL, &t2) <= 0) continue;
		u8 buf[4096];
		int n = recv(c, buf, sizeof(buf), 0);
		if (n <= 0) break;
		if (!tap)
		{
			// PPP: echoed, each byte plus one
			modem_bytes_seen += n;
			for (int i = 0; i < n; i++) buf[i]++;
			send(c, buf, n, MSG_NOSIGNAL);
			continue;
		}
		in.insert(in.end(), buf, buf + n);
		while (in.size() >= 2)
		{
			unsigned len = in[0] | (in[1] << 8);
			if (in.size() < 2 + len) break;
			if (len == 6 && !memcmp(&in[2], "DCNET\1", 6)) prolog_seen++;
			else { frames_seen++; send(c, &in[0], 2 + len, MSG_NOSIGNAL); }	// echoed
			in.erase(in.begin(), in.begin() + 2 + len);
		}
	}
	closed++;
	close(c);
}

static void *relay(void *)
{
	while (!relay_stop)
	{
		fd_set rd; FD_ZERO(&rd); FD_SET(udp_fd, &rd); FD_SET(ppp_fd, &rd); FD_SET(tap_fd, &rd); FD_SET(smash_fd, &rd);
		timeval tv = { 0, 100000 };
		if (select(std::max(std::max(udp_fd, smash_fd), std::max(ppp_fd, tap_fd)) + 1, &rd, NULL, NULL, &tv) <= 0) continue;
		if (FD_ISSET(udp_fd, &rd))
		{
			u8 buf[64]; sockaddr_in from; socklen_t fl = sizeof(from);
			int n = recvfrom(udp_fd, buf, sizeof(buf), 0, (sockaddr *)&from, &fl);
			if (n == 5 && buf[4] == 3)
			{
				/* DISCOVER: two access points, this one and one that never
				 * answers (127.0.0.3): this one is chosen */
				u8 list[5 + 2 * (5 + 4)];
				memcpy(list, buf, 5);
				u8 *p = list + 5;
				u32 a = htonl(0x7f000003);
				memcpy(p, &a, 4); p[4] = 4; memcpy(p + 5, "dead", 4); p += 9;
				a = htonl(INADDR_LOOPBACK);
				memcpy(p, &a, 4); p[4] = 4; memcpy(p + 5, "live", 4); p += 9;
				sendto(udp_fd, list, p - list, 0, (sockaddr *)&from, fl);
			}
			else if (n == 13 && buf[4] == 1)
			{
				// PING: a PONG with its time as it was
				pings_seen++;
				buf[4] = 2;
				sendto(udp_fd, buf, 13, 0, (sockaddr *)&from, fl);
			}
		}
		if (FD_ISSET(ppp_fd, &rd)) { int c = accept(ppp_fd, NULL, NULL); if (c >= 0) { connections++; serve(c, false); } }
		if (FD_ISSET(tap_fd, &rd)) { int c = accept(tap_fd, NULL, NULL); if (c >= 0) { connections++; serve(c, true); } }
		if (FD_ISSET(smash_fd, &rd)) { int c = accept(smash_fd, NULL, NULL); if (c >= 0) { connections++; serve_smash(c); } }
	}
	return NULL;
}

static int listen_on(int type, u16 port)
{
	int fd = socket(AF_INET, type, 0);
	int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
	if (bind(fd, (sockaddr *)&a, sizeof(a)) < 0) return -1;
	if (type == SOCK_STREAM && listen(fd, 4) < 0) return -1;
	return fd;
}

static bool open_relay()
{
	udp_fd = listen_on(SOCK_DGRAM, 7655);
	ppp_fd = listen_on(SOCK_STREAM, 7654);
	tap_fd = listen_on(SOCK_STREAM, 7655);
	smash_fd = listen_on(SOCK_STREAM, 7656);
	return udp_fd >= 0 && ppp_fd >= 0 && tap_fd >= 0 && smash_fd >= 0;
}

static void dcnet_modem(pthread_t *th)
{
	net_device = NET_DEVICE_MODEM;
	net_dcnet_option = true;
	pthread_create(th, NULL, relay, NULL);
	CHECK(start_pico());
	retro_sleep(300);
	const char msg[] = "~hello dcnet~";
	for (const char *p = msg; *p; p++) write_pico(*p);
	std::vector<u8> got;
	retro_time_t t0 = cpu_features_get_time_usec();
	while (got.size() < strlen(msg) && cpu_features_get_time_usec() - t0 < 3000000)
	{
		int c = read_pico();
		if (c >= 0) got.push_back(c);
	}
	CHECK(got.size() == strlen(msg));
	bool same = got.size() == strlen(msg);
	for (size_t i = 0; same && i < got.size(); i++) same = got[i] == (u8)(msg[i] + 1);
	CHECK(same);
	// a lot both ways: the ring to the modem fills, and the thread waits for the modem
	int sent = 0, recvd = 0;
	t0 = cpu_features_get_time_usec();
	while (cpu_features_get_time_usec() - t0 < 1500000)
	{
		for (int i = 0; i < 64; i++) { write_pico('a' + (sent % 26)); sent++; }
		for (int i = 0; i < 32; i++) if (read_pico() >= 0) recvd++;
	}
	retro_sleep(200);
	while (read_pico() >= 0) recvd++;
	stop_pico();
	printf("dcnet modem: echo ok, %d sent, %d received (relay saw %d)\n", sent, recvd, modem_bytes_seen.load());
	CHECK(recvd > 0);
	// the access point that answered, five times
	CHECK(pings_seen == 5);
}

/* Power Smash: its frames each with a flag at both ends, however they came */
static void dcnet_power_smash()
{
	static const char want[] = "~abc~~def~~ghi~";
	memcpy(ip_meta.product_number, "HDR-0113  ", 10);
	net_device = NET_DEVICE_MODEM;
	CHECK(start_pico());
	std::vector<u8> got;
	retro_time_t t0 = cpu_features_get_time_usec();
	while (got.size() < strlen(want) && cpu_features_get_time_usec() - t0 < 3000000)
	{
		int c = read_pico();
		if (c >= 0) got.push_back(c);
	}
	stop_pico();
	memset(ip_meta.product_number, ' ', 10);
	CHECK(got.size() == strlen(want) && !memcmp(&got[0], want, strlen(want)));
	printf("dcnet power smash: %.*s\n", (int)got.size(), got.empty() ? "" : (const char *)&got[0]);
}

static void dcnet_bba()
{
	net_device = NET_DEVICE_BBA;
	CHECK(start_pico());
	retro_sleep(100);
	u8 fr[100];
	for (int i = 0; i < 100; i++) fr[i] = i;
	pico_receive_eth_frame(fr, sizeof(fr));
	u8 back[NET_ETH_FRAME_MAX];
	int got = 0;
	retro_time_t t0 = cpu_features_get_time_usec();
	while (got < 50 && cpu_features_get_time_usec() - t0 < 3000000)
	{
		if (got < 50 && (cpu_features_get_time_usec() - t0) % 1000 < 50) pico_receive_eth_frame(fr, sizeof(fr));
		u32 n = net_eth_take(back);
		if (n == sizeof(fr) && !memcmp(back, fr, n)) got++;
	}
	CHECK(prolog_seen == 1);
	CHECK(got > 10);
	printf("dcnet bba: prolog %d, %d frames echoed (relay saw %d)\n", prolog_seen.load(), got, frames_seen.load());
	/* the game's DHCP release: the connection is closed, and the next frame
	 * (within five seconds of the last connection) opens none */
	{
		static u8 release[0x120];
		memset(release, 0, sizeof(release));
		release[0xc] = 0x08; release[0xd] = 0x00;
		release[0x17] = 0x11;
		release[0x23] = 68;
		release[0x25] = 67;
		release[0x11a] = 53; release[0x11b] = 1; release[0x11c] = 7;	// message type: release
		release[0x11d] = 0xff;
		const int before = closed.load(), conns = connections.load();
		pico_receive_eth_frame(release, sizeof(release));
		t0 = cpu_features_get_time_usec();
		while (closed.load() == before && cpu_features_get_time_usec() - t0 < 2000000)
			retro_sleep(10);
		CHECK(closed.load() == before + 1);
		pico_receive_eth_frame(fr, sizeof(fr));
		retro_sleep(300);
		CHECK(connections.load() == conns);
		printf("dcnet bba: closed on a DHCP release, %d connections\n", connections.load());
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
	for (size_t i = 0; i + 1 < n; i += 2)
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
	u8 f[NET_ETH_FRAME_MAX];
	static u16 ipid;
	memcpy(f, p_mac, 6);
	memcpy(f + 6, g_mac, 6);
	f[12] = 0x08; f[13] = 0x00;
	u8 *ip = f + 14;
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
	u8 ph[12];
	memcpy(ph, g_ip, 4); memcpy(ph + 4, dst, 4); ph[8] = 0; ph[9] = proto; put16(ph + 10, (u16)len);
	u8 *t = ip + 20;
	const size_t at = proto == 6 ? 16 : 6;
	t[at] = t[at + 1] = 0;
	u16 c = (u16)~csum_add(csum_add(0, ph, 12), t, len);
	if (proto == 17 && c == 0)
		c = 0xffff;
	put16(t + at, c);
	size_t n = 14 + 20 + len;
	if (n < 60) { memset(f + n, 0, 60 - n); n = 60; }
	pico_receive_eth_frame(f, (u32)n);
}

struct gtcp
{
	u8 dst[4];
	u16 lport, rport;
	u32 snd_una, snd_nxt, rcv_nxt;
	u32 peer_wnd;
	u16 my_wnd;			/* what is advertised */
	bool established, fin_rcvd, fin_sent, rst;
	std::vector<u8> rx;		/* what arrived, in order */
	std::vector<u8> tx;		/* what is to be sent, from snd_una's place on */
	size_t tx_base;		/* tx[0]'s sequence offset from the first data byte */
	u32 iss, irs;
	retro_time_t last_progress;
	bool passive;			/* the other end opened it */
};

static void tcp_seg(gtcp &c, u8 flags, u32 seq, const u8 *data, size_t len)
{
	u8 t[20 + 1460];
	memset(t, 0, 20);
	put16(t, c.lport);
	put16(t + 2, c.rport);
	put32b(t + 4, seq);
	put32b(t + 8, (flags & 0x10) ? c.rcv_nxt : 0);
	t[12] = 5 << 4;
	t[13] = flags;
	put16(t + 14, c.my_wnd);
	if (len)
		memcpy(t + 20, data, len);
	send_ip(c.dst, 6, t, 20 + len);
}

static std::vector<gtcp *> g_conns;
static std::vector<std::vector<u8> > g_udp_in;	/* UDP datagrams to the guest: port (2), data */
static int g_syns;				/* SYNs from the stack to ports nobody listens on */
static u16 g_listen_port;			/* the guest's open port, for connections from outside */

static gtcp *find_conn(u16 lport, u16 rport, const u8 *src)
{
	for (size_t i = 0; i < g_conns.size(); i++)
		if (g_conns[i]->lport == lport && g_conns[i]->rport == rport && !memcmp(g_conns[i]->dst, src, 4))
			return g_conns[i];
	return NULL;
}

/* Every frame the stack has for the guest, handled */
static void guest_poll()
{
	u8 f[NET_ETH_FRAME_MAX];
	u32 n;
	while ((n = net_eth_take(f)) != 0)
	{
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
		const u8 *ip = f + 14;
		const size_t ihl = (ip[0] & 15) * 4;
		const size_t total = get16(ip + 2);
		const u8 *t = ip + ihl;
		if (ip[9] == 17)
		{
			std::vector<u8> d(t, t + 2);	/* (the source port, then the data) */
			d.insert(d.end(), t + 8, ip + total);
			g_udp_in.push_back(d);
			continue;
		}
		if (ip[9] != 6)
			continue;
		const size_t thl = (t[12] >> 4) * 4;
		const u8 flags = t[13];
		const u32 seq = get32b(t + 4), ack = get32b(t + 8);
		const u8 *data = t + thl;
		const size_t dlen = ip + total - data;
		gtcp *c = find_conn(get16(t + 2), get16(t), ip + 12);
		if (c == NULL)
		{
			if ((flags & 0x12) == 0x02 && get16(t + 2) == g_listen_port && g_listen_port)
			{
				/* a connection from outside: answered */
				c = new gtcp();
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
				g_conns.push_back(c);
				tcp_seg(*c, 0x12, c->iss, NULL, 0);
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
			tcp_seg(*c, 0x10, c->snd_nxt, NULL, 0);
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
		bool ack_it = false;
		if (dlen > 0 || (flags & 0x01))
		{
			if (seq == c->rcv_nxt)
			{
				/* (no more than the window: what is past it is sent again) */
				size_t take = dlen;
				c->rx.insert(c->rx.end(), data, data + take);
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
			tcp_seg(*c, 0x10, c->snd_nxt, NULL, 0);
	}
}

/* What the guest has to send: as much as the window lets, and from the
 * first byte not acknowledged again when nothing has moved for a while */
static void guest_push(gtcp &c)
{
	const u32 first_data = c.iss + 1;
	if (!c.established)
		return;
	if (cpu_features_get_time_usec() - c.last_progress > 300000 && c.snd_nxt != c.snd_una)
	{
		c.snd_nxt = c.snd_una;
		c.last_progress = cpu_features_get_time_usec();
	}
	for (;;)
	{
		const size_t off = c.snd_nxt - first_data;	/* in the stream */
		const size_t end = c.tx_base + c.tx.size();
		const u32 in_flight = c.snd_nxt - c.snd_una;
		if (off >= end || in_flight >= c.peer_wnd)
			break;
		size_t len = end - off;
		if (len > 1000) len = 1000;
		if (len > c.peer_wnd - in_flight) len = c.peer_wnd - in_flight;
		/* (the close on the last of it, so that the stack has it while
		 * there is still some to read) */
		if (c.fin_sent && off + len == end)
		{
			tcp_seg(c, 0x19, c.snd_nxt, &c.tx[off - c.tx_base], len);
			c.snd_nxt += (u32)len + 1;
			break;
		}
		tcp_seg(c, 0x18, c.snd_nxt, &c.tx[off - c.tx_base], len);
		c.snd_nxt += (u32)len;
	}
	if (c.fin_sent && c.snd_nxt == first_data + c.tx_base + c.tx.size() + 1)
		return;
	/* the close, after all of it */
	if (c.fin_sent && c.snd_nxt == first_data + c.tx_base + c.tx.size())
	{
		tcp_seg(c, 0x11, c.snd_nxt, NULL, 0);
		c.snd_nxt++;
	}
	else if (c.fin_sent && c.snd_una == first_data + c.tx_base + c.tx.size() + 1)
		;	/* acknowledged */
	else if (c.fin_sent && c.snd_nxt == first_data + c.tx_base + c.tx.size() + 1
			&& cpu_features_get_time_usec() - c.last_progress > 300000)
	{
		c.snd_nxt--;
		c.last_progress = cpu_features_get_time_usec();
	}
}

static gtcp *guest_connect(const u8 *dst, u16 port, u16 lport, u16 wnd)
{
	gtcp *c = new gtcp();
	memcpy(c->dst, dst, 4);
	c->lport = lport;
	c->rport = port;
	c->my_wnd = wnd;
	c->iss = 0x1000 * lport;
	c->snd_una = c->iss;
	c->snd_nxt = c->iss + 1;
	c->tx_base = 0;
	c->last_progress = cpu_features_get_time_usec();
	g_conns.push_back(c);
	tcp_seg(*c, 0x02, c->iss, NULL, 0);
	return c;
}

/* The guest's side runs until @done says so, or a few seconds */
template<class F> static bool guest_run(F done, int ms = 8000)
{
	retro_time_t t0 = cpu_features_get_time_usec(), syn = t0;
	while (!done())
	{
		if (cpu_features_get_time_usec() - t0 > (retro_time_t)ms * 1000)
			return false;
		guest_poll();
		for (size_t i = 0; i < g_conns.size(); i++)
		{
			gtcp &c = *g_conns[i];
			/* a SYN not answered: sent again */
			if (!c.established && !c.passive && !c.rst && cpu_features_get_time_usec() - syn > 300000)
				tcp_seg(c, 0x02, c.iss, NULL, 0);
			guest_push(c);
			/* the window opened again once the guest has read */
			if (c.my_wnd < 8192 && c.rx.size() > 0)
				;
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
	int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (rcvbuf) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
	if (bind(fd, (sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) { close(fd); return -1; }
	return fd;
}
static int accept_within(int lfd, int ms)
{
	fd_set r; FD_ZERO(&r); FD_SET(lfd, &r);
	timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	if (select(lfd + 1, &r, NULL, NULL, &tv) <= 0) return -1;
	return accept(lfd, NULL, NULL);
}
/* All a socket has until its end, or @ms without anything */
static std::vector<u8> read_all(int fd, int ms, bool *eof)
{
	std::vector<u8> got;
	*eof = false;
	for (;;)
	{
		fd_set r; FD_ZERO(&r); FD_SET(fd, &r);
		timeval tv = { ms / 1000, (ms % 1000) * 1000 };
		if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) break;
		u8 buf[4096];
		int n = recv(fd, buf, sizeof(buf), 0);
		if (n <= 0) { *eof = n == 0; break; }
		got.insert(got.end(), buf, buf + n);
	}
	return got;
}
static std::vector<u8> pattern(size_t n, u32 seed)
{
	std::vector<u8> v(n);
	for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; v[i] = (u8)(seed >> 16); }
	return v;
}

struct slow_reader { int fd; int delay_ms; std::vector<u8> got; bool eof; };
static void *slow_read(void *arg)
{
	slow_reader *s = (slow_reader *)arg;
	retro_sleep(s->delay_ms);
	s->got = read_all(s->fd, 3000, &s->eof);
	return NULL;
}

static void pico_bba_relay()
{
	net_device = NET_DEVICE_BBA;
	net_dcnet_option = false;
	memcpy(ip_meta.product_number, "T22904N   ", 10);	/* Worms World Party: TCP 17219 open */
	g_listen_port = 17219;
	/* (the relay's own sockets made with small send buffers: see socket()) */
	small_sndbuf = 1;
	CHECK(start_pico());
	retro_sleep(300);
	/* the stack's address: from the answer to an ARP of the guest's */
	{
		static const u8 arp[60] = {
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  0x08, 0x06,
			0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x01,
			0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  192, 168, 169, 2,
			0, 0, 0, 0, 0, 0,                    192, 168, 169, 1,
		};
		retro_time_t t0 = cpu_features_get_time_usec();
		while (p_mac[0] == 0 && p_mac[5] == 0 && cpu_features_get_time_usec() - t0 < 3000000)
		{
			pico_receive_eth_frame(arp, sizeof(arp));
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
		CHECK(lfd >= 0);
		gtcp *c = guest_connect(loopback, 17301, 40001, 2048);
		int fd = -1;
		CHECK(guest_run([&] { return c->established; }));
		fd = accept_within(lfd, 2000);
		CHECK(fd >= 0);
		std::vector<u8> up = pattern(300 * 1024, 1);
		c->tx = up;
		c->fin_sent = true;
		slow_reader sr = { fd, 1000, std::vector<u8>(), false };
		pthread_t th;
		pthread_create(&th, NULL, slow_read, &sr);
		guest_run([&] { return c->snd_una == c->iss + 1 + up.size() + 1; }, 15000);
		pthread_join(th, NULL);
		CHECK(sr.got == up);
		CHECK(sr.eof);
		std::vector<u8> down = pattern(200 * 1024, 2);
		size_t sent = 0;
		while (sent < down.size())
		{
			int n = send(fd, &down[sent], down.size() - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (n > 0) sent += n;
			guest_run([&] { return false; }, 5);
		}
		shutdown(fd, SHUT_WR);
		CHECK(guest_run([&] { return c->fin_rcvd; }, 15000));
		CHECK(c->rx == down);
		printf("relay tcp: %zu bytes up (slow reader), %zu down (small window), both closed after\n", sr.got.size(), c->rx.size());
		close(fd);
		close(lfd);
	}

	/* 1b. a little sent and closed at once, before the relay's own
	 * connection is made: it all arrives, then the end */
	{
		int lfd = tcp_server(17303, 0);
		CHECK(lfd >= 0);
		gtcp *c = guest_connect(loopback, 17303, 40003, 8192);
		std::vector<u8> up = pattern(100, 5);
		c->tx = up;
		c->fin_sent = true;
		CHECK(guest_run([&] { return c->snd_una == c->iss + 1 + up.size() + 1; }, 5000));
		int fd = accept_within(lfd, 2000);
		CHECK(fd >= 0);
		bool eof = false;
		std::vector<u8> got = fd >= 0 ? read_all(fd, 2000, &eof) : std::vector<u8>();
		CHECK(got == up);
		CHECK(eof);
		printf("relay tcp: %zu bytes sent before the connection was made, then the end\n", got.size());
		if (fd >= 0)
			close(fd);
		close(lfd);
	}

	/* 1c. sent and closed while the far end reads nothing: the relay has
	 * it all, the close with it, before the far end reads a byte; then it
	 * all arrives, and the end only after it */
	{
		int lfd = tcp_server(17304, 4096);
		CHECK(lfd >= 0);
		gtcp *c = guest_connect(loopback, 17304, 40004, 8192);
		CHECK(guest_run([&] { return c->established; }));
		int fd = accept_within(lfd, 2000);
		CHECK(fd >= 0);
		std::vector<u8> up = pattern(24 * 1024, 6);
		c->tx = up;
		c->fin_sent = true;
		CHECK(guest_run([&] { return c->snd_una == c->iss + 1 + up.size() + 1; }, 8000));
		bool eof = false;
		std::vector<u8> got = fd >= 0 ? read_all(fd, 3000, &eof) : std::vector<u8>();
		CHECK(got == up);
		CHECK(eof);
		printf("relay tcp: %zu bytes and the close kept for a far end reading nothing, then the end\n", got.size());
		if (fd >= 0)
			close(fd);
		close(lfd);
	}

	/* 2. a port nobody listens on: the guest's connection is refused */
	{
		gtcp *c = guest_connect(loopback, 17399, 40002, 8192);
		CHECK(guest_run([&] { return c->rst || c->fin_rcvd; }, 5000));
		printf("relay tcp: refused (%s)\n", c->rst ? "reset" : "closed");
	}

	/* 3. a connection from outside to the game's port: the guest answers,
	 * both ways, the outside closing its side first and reading on */
	{
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		int rb = 4096;
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
		sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(17219);
		CHECK(connect(fd, (sockaddr *)&a, sizeof(a)) == 0);
		gtcp *c = NULL;
		CHECK(guest_run([&] { for (size_t i = 0; i < g_conns.size(); i++) if (g_conns[i]->passive && g_conns[i]->established) { c = g_conns[i]; return true; } return false; }));
		if (c != NULL)
		{
			std::vector<u8> in = pattern(50000, 3), out = pattern(300 * 1024, 4);
			send(fd, &in[0], in.size(), MSG_NOSIGNAL);
			shutdown(fd, SHUT_WR);
			/* (the guest answers once the outside's end has reached it: the
			 * outside still reads what comes after it closed its side) */
			CHECK(guest_run([&] { return c->fin_rcvd; }, 10000));
			c->tx = out;
			c->fin_sent = true;
			bool eof;
			std::vector<u8> got;
			retro_time_t t0 = cpu_features_get_time_usec();
			/* (the outside slow to read: the relay keeps what it has not
			 * taken, past the stack's end of the connection) */
			guest_run([&] { return false; }, 1000);
			while (cpu_features_get_time_usec() - t0 < 15000000 && !(c->fin_rcvd && got.size() >= out.size()))
			{
				guest_run([&] { return false; }, 5);
				u8 buf[4096];
				int n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
				if (n > 0) got.insert(got.end(), buf, buf + n);
			}
			std::vector<u8> rest = read_all(fd, 2000, &eof);
			got.insert(got.end(), rest.begin(), rest.end());
			CHECK(c->rx == in);
			CHECK(c->fin_rcvd);
			CHECK(got == out);
			if (got != out) printf("DBG una %u nxt %u wnd %u rst %d\n", c->snd_una - c->iss - 1, c->snd_nxt - c->iss - 1, c->peer_wnd, c->rst);
			printf("relay tcp from outside: %zu in, %zu out\n", c->rx.size(), got.size());
		}
		close(fd);
	}

	/* 4. UDP: a datagram out, an answer back, from the port it went to */
	{
		int ufd = socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(17302);
		CHECK(bind(ufd, (sockaddr *)&a, sizeof(a)) == 0);
		u8 u[8 + 32];
		memset(u, 0, sizeof(u));
		put16(u, 40010); put16(u + 2, 17302); put16(u + 4, sizeof(u));
		memcpy(u + 8, "datagram from the dreamcast ....", 32);
		send_ip(loopback, 17, u, sizeof(u));
		u8 buf[64]; sockaddr_in from; socklen_t fl = sizeof(from);
		fd_set r; FD_ZERO(&r); FD_SET(ufd, &r); timeval tv = { 2, 0 };
		int n = select(ufd + 1, &r, NULL, NULL, &tv) > 0 ? recvfrom(ufd, buf, sizeof(buf), 0, (sockaddr *)&from, &fl) : -1;
		CHECK(n == 32 && !memcmp(buf, u + 8, 32));
		/* (from the port the Dreamcast sent from, which was to be had) */
		CHECK(n > 0 && ntohs(from.sin_port) == 40010);
		if (n > 0)
			sendto(ufd, "answer", 6, 0, (sockaddr *)&from, fl);
		g_udp_in.clear();
		CHECK(guest_run([&] { return !g_udp_in.empty(); }, 3000));
		CHECK(!g_udp_in.empty() && g_udp_in[0].size() == 8 && get16(&g_udp_in[0][0]) == 17302 && !memcmp(&g_udp_in[0][2], "answer", 6));
		printf("relay udp: out %d, back %zu\n", n, g_udp_in.empty() ? 0 : g_udp_in[0].size() - 2);
		close(ufd);
	}

	small_sndbuf = 0;
	stop_pico();
	net_thread_join();
	for (size_t i = 0; i < g_conns.size(); i++)
		delete g_conns[i];
	g_conns.clear();
	g_listen_port = 0;
	memset(ip_meta.product_number, ' ', 10);
}

int main()
{
	pico_modem();
	pico_bba();
	pico_bba_relay();
	pthread_t th;
	if (!open_relay()) { printf("no relay ports\n"); return 1; }
	dcnet_modem(&th);
	dcnet_power_smash();
	dcnet_bba();
	relay_stop = 1;
	pthread_join(th, NULL);
	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures != 0;
}
