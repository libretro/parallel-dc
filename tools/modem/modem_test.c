/*
 * The Dreamcast's modem (core/hw/modem/modem.c), with the far end of a
 * synchronous connection (v42.c, v42bis.c), driven through its registers as
 * a game does: the reset and the self tests, each register written and
 * read, the DSP's memory, a module download, a soft reset, the V.8 bis
 * detector, dialling, ringing and the handshake, connections at each speed
 * the game can ask for (V.8 AUTO among them), asynchronous and synchronous,
 * the bytes both ways and the FIFOs, the line lost, and the save state:
 * loaded on the same line (run-ahead), on another, and wrong. What it does
 * is printed, and must be what it always was (modem_expected.txt).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "hw/modem/modem.h"
#include "reios/reios.h"
#include "hw/sh4/sh4_sched.h"
#include <features/features_cpu.h>

#ifdef MODEM_TEST_OLD
/* (the old code's C++ functions, through a shim) */
#define modem_serialize_v21 modem_old_serialize_v21
#define modem_unserialize_v21 modem_old_unserialize_v21
#define modem_state_before_v21 modem_old_state_before_v21
void modem_old_serialize_v21(void **data, unsigned int *total_size);
bool modem_old_unserialize_v21(void **data, unsigned int *total_size);
void modem_old_state_before_v21(void);
#endif

/* (no log) */
retro_log_printf_t log_cb;
void os_DebugBreak(void) { printf("FAIL: os_DebugBreak\n"); exit(1); }

settings_t settings;
ip_meta_t ip_meta;
/* (none when the modem's protocols are made: the machine is made after) */
static int machine;
void *p_sh4rcb;

static u32 fnv(u32 h, u8 v)
{
	return (h ^ v) * 16777619u;
}

static u32 hash(const u8 *p, unsigned n)
{
	u32 h = 2166136261u;
	unsigned i;
	for (i = 0; i < n; i++)
		h = fnv(h, p[i]);
	return h;
}

static int irq = -1;
void asic_RaiseInterrupt(int inter)
{
	(void)inter;
	if (irq != 1)
		printf("  irq on\n");
	irq = 1;
}
void asic_CancelInterrupt(int inter)
{
	(void)inter;
	if (irq != 0)
		printf("  irq off\n");
	irq = 0;
}

/* The scheduler: the modem's event, and the time */
static u64 now64 = 1000000;
static sh4_sched_callback *callback;
static int due;
int sh4_sched_register(int tag, sh4_sched_callback *ssc)
{
	(void)tag;
	callback = ssc;
	return 3;
}
void sh4_sched_request(int id, int cycles)
{
	if (id != 3)
		printf("FAIL: event %d\n", id);
	due = cycles;
}
bool sh4_sched_is_scheduled(int id)
{
	(void)id;
	return due > 0;
}
u64 sh4_sched_now64(void) { return now64; }

/* The modem's next step: its time comes */
static int step(void)
{
	int r;
	if (due <= 0)
		return 0;
	now64 += (u64)due;
	due = 0;
	r = callback(0, 0, 0);
	if (r > 0)
		due = r;
	return 1;
}

static void steps(int n)
{
	while (n-- > 0 && step())
		;
}

/* The line: what the network sends, what it is sent */
static u8 net_in[65536];
static unsigned net_len, net_pos;
static u32 net_out_hash = 2166136261u;
static unsigned net_out_count;
static int pico_up;
bool start_pico(void) { printf("  start pico\n"); pico_up = 1; return true; }
void stop_pico(void) { if (pico_up) printf("  stop pico\n"); pico_up = 0; }
int read_pico(void) { return net_pos < net_len ? net_in[net_pos++] : -1; }
void write_pico(u8 b) { net_out_hash = fnv(net_out_hash, b); net_out_count++; }
static retro_time_t usec = 5000;
retro_time_t cpu_features_get_time_usec(void) { return usec += 17; }

static void net_has(const u8 *p, unsigned n)
{
	memcpy(net_in + net_len, p, n);
	net_len += n;
}

bool ra_serialize(const void *src, unsigned int size, void **dest, unsigned int *total)
{
	if (*dest != NULL)
	{
		memcpy(*dest, src, size);
		*dest = (u8 *)*dest + size;
	}
	*total += size;
	return true;
}
bool ra_unserialize(void *dst, unsigned int size, void **src, unsigned int *total)
{
	memcpy(dst, *src, size);
	*src = (u8 *)*src + size;
	*total += size;
	return true;
}

static void save(const char *what);
static void load(const char *what);

/*
 * The registers
 */
#define MODEM_BASE 0x00600000

static u32 rd(u32 reg)
{
	return ModemReadMem_A0_006(MODEM_BASE + (0x100 + reg) * 4, 1);
}

static void wr(u32 reg, u32 v)
{
	ModemWriteMem_A0_006(MODEM_BASE + (0x100 + reg) * 4, v, 1);
}

static u32 modem_regs_peek(u32 reg)
{
	return rd(reg);
}

static void dump(const char *what)
{
	u32 r;
	printf(" %s:", what);
	for (r = 0; r < 0x21; r++)
		printf(" %02x", rd(r));
	printf(" due %d\n", due);
}

/* Without the reads' own changes */
static u8 state[64 * 1024];
static u32 quiet = 2166136261u;
static unsigned state_size;

static void save(const char *what)
{
	void *p = state;
	unsigned int total = 0;
	modem_serialize_v21(&p, &total);
	modem_serialize_v24(&p, &total);
	state_size = total;
	if (what == NULL)
		quiet = fnv(quiet, (u8)hash(state, total));
	else
		printf(" state %s %u %08x %08x\n", what, total, hash(state, total), quiet);
}

static void load(const char *what)
{
	void *p = state;
	unsigned int total = 0;
	bool ok = modem_unserialize_v21(&p, &total);
	ok = modem_unserialize_v24(&p, &total) && ok;
	printf(" load %s %d %u\n", what, ok, total);
}

static void dsp_write(u32 addr, u32 v, int word)
{
	wr(0x18, v & 0xff);
	if (word)
		wr(0x19, v >> 8);
	wr(0x1c, addr & 0xff);
	wr(0x1d, 0x80 | 0x20 | (addr >> 8));
}

static u32 dsp_read(u32 addr)
{
	wr(0x1c, addr & 0xff);
	wr(0x1d, 0x80 | (addr >> 8));
	return rd(0x18) | (rd(0x19) << 8);
}

/* The modem reset, and its self tests over */
static void reset_modem(void)
{
	wr(0x20, 0);
	dump("reset low");
	wr(0x20, 1);
	dump("self test");
	rd(0x10);
	steps(1);
	dump("dsp test");
	steps(1);
	dump("normal");
}

static void registers(void)
{
	u32 r, i;
	printf("registers\n");
	printf(" id %x %x %x\n", ModemReadMem_A0_006(MODEM_BASE, 4), ModemReadMem_A0_006(MODEM_BASE + 4, 4),
			ModemReadMem_A0_006(MODEM_BASE + 0x3fc, 4));
	printf(" unknown %x\n", ModemReadMem_A0_006(MODEM_BASE + 0x121 * 4, 1));
	ModemWriteMem_A0_006(MODEM_BASE + 8, 1, 1);
	ModemWriteMem_A0_006(MODEM_BASE + 0x130 * 4, 1, 1);
	wr(0x09, 1);
	dump("invalid");
	reset_modem();
	for (r = 0; r < 0x20; r++)
	{
		if (r == 0x10 || r == 0x1f || r == 0x14 || r == 0x1d || r == 0x1a)
			continue;
		wr(r, 0xff);
		printf(" w %02x ff: %02x", r, rd(r));
		wr(r, 0x00);
		printf(" 00: %02x\n", rd(r));
	}
	dump("written");
	/* the DSP's memory */
	dsp_write(0x100, 0x1234, 1);
	dsp_write(0x101, 0x5678, 1);
	dsp_write(0xfff, 0x9abc, 1);
	dsp_write(0x200, 0xdef0, 0);
	dsp_write(0x26b, 0x01, 0);
	printf(" eqm %x\n", dsp_read(0x26f));
	dsp_write(0x26f, 0x00, 0);
	dsp_write(0x26c, 0x01, 0);
	printf(" eqm %x\n", dsp_read(0x26f));
	dsp_write(0x26f, 0x00, 0);
	dsp_write(0x26c, 0x100, 1);
	printf(" dsp %x %x %x %x %x %x %x %x\n", dsp_read(0x100), dsp_read(0x101), dsp_read(0x102), dsp_read(0xfff),
			dsp_read(0x000), dsp_read(0x200), dsp_read(0x26f), dsp_read(0x247));
	/* the status interrupt: a status bit the game asked to be told of */
	dsp_write(0x241, 0xff, 0);
	dsp_write(0x245, 0xff, 0);
	wr(0x1f, 0x10 | 0x04 | 0x08);
	dump("nsie");
	wr(0x1f, 0x10 | 0x04 | 0x01);
	dump("newc");
	wr(0x1f, 0x10 | 0x08);
	dump("news");
	wr(0x08, 0x01);
	dump("rts");
	wr(0x1f, 0x00);
	wr(0x08, 0x00);
	/* a module download */
	wr(0x1b, 0x00);
	wr(0x14, 0x4f);
	for (i = 0; i < 300; i++)
		wr(0x10, i * 7);
	dump("downloading");
	wr(0x14, 0x00);
	dump("downloaded");
	wr(0x14, 0x5f);
	wr(0x14, 0x12);
	wr(0x14, 0x00);
	dump("download 2");
	save("registers");
	/* a soft reset */
	wr(0x1a, 0x80);
	wr(0x1f, 0x01);
	dump("soft reset");
	wr(0x09, 0x01);
	steps(1);
	dump("soft reset dsp");
	steps(1);
	dump("soft reset done");
}

static void reads_rx(int n)
{
	u32 h = 2166136261u;
	int i, got = 0;
	for (i = 0; i < n; i++)
	{
		if (rd(0x1e) & 1)
		{
			h = fnv(h, (u8)rd(0x00));
			got++;
		}
		steps(1);
	}
	printf(" rx %d %08x status %02x %02x %02x %02x net out %u %08x\n", got, h, rd(0x01), rd(0x0c), rd(0x0d), rd(0x1e),
			net_out_count, net_out_hash);
	save("rx");
}

/* Steps with nothing read: the receive FIFO fills */
static void no_reads(int n)
{
	int i;
	for (i = 0; i < n; i++)
	{
		steps(1);
		if (i % 4 == 0)
			printf(" fill %d: %02x %02x %02x\n", i, modem_regs_peek(0x01), modem_regs_peek(0x0c), modem_regs_peek(0x1e));
	}
}

static void dial(void)
{
	wr(0x10, 0x31);
	dump("dialing");
	steps(1);
	wr(0x10, 0x32);
	steps(1);
	dump("dialed");
	steps(1);
	dump("ringing");
	steps(1);
	dump("rung");
}

/* A call, with @conf asked for, asynchronous or not */
static void call(u32 conf, int asyn, int data)
{
	printf("call %02x %d %d\n", conf, asyn, data);
	reset_modem();
	wr(0x12, conf);
	wr(0x08, asyn ? 0x80 : 0x00);
	wr(0x04, 0x10);
	wr(0x0e, 0x20);
	wr(0x16, 0x55);
	wr(0x09, data ? 0x04 : 0x00);
	wr(0x1e, 0x24);
	wr(0x02, 0x08);
	dial();
	if (data)
		wr(0x09, 0x05);
	else
	{
		/* (the handshake started by AUTO, and DATA set by it) */
		wr(0x09, 0x01);
		dump("dtr");
		wr(0x15, 0x08);
	}
	dump("handshake");
	steps(1);
	dump("pre connected");
	steps(1);
	dump("connected");
}

static void async_call(void)
{
	static const u8 hello[] = "hello from the network, as it comes over the line";
	unsigned i;
	call(0x76, 1, 1);
	net_has(hello, sizeof(hello));
	reads_rx(20);
	save("async 1");
	/* bytes sent, the transmitter's FIFO */
	wr(0x08, 0x81);
	for (i = 0; i < 20; i++)
	{
		wr(0x10, 'A' + i);
		printf(" sent %u: %02x %02x %02x\n", i, rd(0x01), rd(0x0d), rd(0x1e));
	}
	reads_rx(40);
	net_has(hello, sizeof(hello));
	no_reads(30);
	wr(0x1e, 0x20);
	dump("rx irq off");
	reads_rx(30);
	wr(0x1e, 0x24);
	wr(0x04, 0x00);
	wr(0x10, 'Z');
	dump("no fifo");
	reads_rx(30);
	/* run-ahead: the state of a moment ago loaded on the same line */
	save("async 2");
	{
		u8 copy[sizeof(state)];
		const unsigned n = state_size;
		const u64 t = now64;
		const int d = due;
		memcpy(copy, state, n);
		for (i = 0; i < 12; i++)
		{
			if (i % 3 != 1)
				net_has(hello + i, 1);
			steps(1);
			if (i % 2 == 0)
				rd(0x00);
		}
		for (i = 0; i < 5; i++)
			wr(0x10, '0' + i);
		reads_rx(10);
		memcpy(state, copy, n);
		now64 = t;
		due = d;
		load("again");
		for (i = 0; i < 12; i++)
		{
			if (i == 8)
				net_has(hello + 20, 3);
			steps(1);
			save(NULL);
			if (i % 2 == 0)
				rd(0x00);
		}
		for (i = 0; i < 5; i++)
			wr(0x10, '0' + i);
		reads_rx(10);
	}
	/* DTR dropped and up again */
	wr(0x09, 0x04);
	dump("dtr off");
	wr(0x09, 0x05);
	dump("dtr on");
	wr(0x1a, 0x02);
	wr(0x06, 0x18);
	wr(0x11, 0x60);
	save("async 3");
	/* a state from too long ago on the line */
	{
		u8 copy[sizeof(state)];
		const unsigned n = state_size;
		memcpy(copy, state, n);
		for (i = 0; i < 5000; i++)
			net_in[net_len++] = (u8)(i * 7);
		reads_rx(5000);
		memcpy(state, copy, n);
		load("too long ago");
		dump("too long ago");
	}
	/* the reset line high again: nothing */
	wr(0x20, 1);
	wr(0x20, 3);
	dump("reset high");
}

/* The game's side of V.42: V.14 framed characters */
static void game_sends_v14(const u8 *p, unsigned n, int stops)
{
	u8 bits[64];
	unsigned nbits = 0, i, j;
	u8 cur = 0;
	int k, count = 0;
	for (i = 0; i < n; i++)
	{
		u8 c = p[i];
		bits[nbits++] = 0;
		for (j = 0; j < 8; j++)
		{
			bits[nbits++] = c & 1;
			c >>= 1;
		}
		for (k = 0; k < stops; k++)
			bits[nbits++] = 1;
		for (j = 0; j < nbits; j++)
		{
			cur = (u8)((cur >> 1) | (bits[j] << 7));
			if (++count == 8)
			{
				wr(0x10, cur);
				count = 0;
				cur = 0;
			}
		}
		nbits = 0;
	}
	while (count != 0)
	{
		cur = (u8)((cur >> 1) | 0x80);
		if (++count == 8)
		{
			wr(0x10, cur);
			count = 0;
		}
	}
}

static void sync_call(void)
{
	static const u8 odp[] = { 0x11, 0x91, 0x11, 0x91 };
	static const u8 flag[] = { 0x7e };
	static const u8 data[] = "data from the network";
	printf("sync\n");
	call(0xc8, 0, 1);
	wr(0x08, 0x01);
	reads_rx(10);
	game_sends_v14(odp, 4, 9);
	reads_rx(20);
	game_sends_v14(flag, 1, 1);
	reads_rx(10);
	net_has(data, sizeof(data));
	reads_rx(40);
	save("sync");
	/* the detection timed out: V.14 */
	call(0xce, 0, 1);
	wr(0x08, 0x01);
	steps(3000);
	game_sends_v14(data, 5, 1);
	net_has(data, sizeof(data));
	reads_rx(30);
	save("sync v14");
}

static void fractions(void)
{
	unsigned i;
	printf("fractions\n");
	call(0xe8, 1, 1);
	net_has((const u8 *)"12345", 5);
	steps(6);
	printf(" rxhf %02x\n", rd(0x01));
	dsp_write(0x241, 0xff, 0);
	wr(0x09, 0x04);
	dump("dsr off, not asked");
	wr(0x04, 0x00);
	wr(0x08, 0x81);
	for (i = 0; i < 6; i++)
	{
		wr(0x10, 'a' + i);
		steps(1);
		printf(" tx %02x %02x\n", rd(0x0d), rd(0x01));
		steps(1);
		printf(" tx %02x %02x\n", rd(0x0d), rd(0x01));
	}
}

static void speeds(void)
{
	static const u32 confs[] = {
		0xe0, 0xe5, 0xf6, 0x90, 0x95, 0x9d, 0xc0, 0xc3, 0xce, 0xcf, 0x31, 0x32, 0x34, 0x38, 0x72, 0x74,
		0x75, 0x78, 0x71, 0x70, 0xb1, 0xb2, 0xb4, 0xb8, 0x14, 0x12, 0x11, 0x02, 0x01, 0x84, 0x82, 0x52,
		0x51, 0xa0, 0xa8, 0x23, 0x62, 0x60, 0xa4, 0xa1, 0x55
	};
	unsigned i;
	printf("speeds\n");
	for (i = 0; i < sizeof(confs) / sizeof(confs[0]); i++)
	{
		call(confs[i], 1, i & 1);
		steps(3);
		dump("speed");
		save("speed");
	}
}

/* V.8 AUTO: the speed from what the game has in the DSP's memory, and
 * the status interrupts the game asks for, one at a time */
static void v8auto(void)
{
	static const char *const games[] = { "HDR-0113  ", "HDR-0091  ", "T-12345   ", "HDR-0091" };
	static const u32 masks[][3] = {
		{ 0x80, 0, 0 }, { 0, 0x1f, 0 }, { 0, 0, 1 }, { 0, 0, 0 }, { 0x80, 0x1f, 1 }, { 0, 0, 0 }
	};
	unsigned i;
	printf("v8 auto\n");
	for (i = 0; i < 6; i++)
	{
		memset(ip_meta.product_number, 0, sizeof(ip_meta.product_number));
		memcpy(ip_meta.product_number, games[i & 3], strlen(games[i & 3]));
		reset_modem();
		dsp_write(0x6a3, i == 3 ? 0 : 1, 0);
		dsp_write(0x6a2, (i == 5 ? 0 : 0x20) | 0x15, 0);
		dsp_write(0x309, 0xc6, 0);
		dsp_write(0x304, 0xa0, 0);
		dsp_write(0x089, masks[i][0], 0);
		dsp_write(0x242, masks[i][1], 0);
		dsp_write(0x38a, masks[i][2], 0);
		dsp_write(0x241, 0, 0);
		dsp_write(0x245, 0, 0);
		dsp_write(0x247, 0, 0);
		dsp_write(0x244, 0, 0);
		dsp_write(0x243, 0, 0);
		wr(0x12, 0xaa);
		wr(0x08, 0x80);
		wr(0x09, 0x04);
		dial();
		wr(0x09, 0x05);
		steps(1);
		wr(0x1f, 0x18);
		dump("v8 auto nsie");
		printf(" dsp %x %x %x\n", dsp_read(0x302), dsp_read(0x2e4), dsp_read(0x6c2));
		steps(1);
		dump("v8 auto");
		printf(" dsp %x %x %x %x\n", dsp_read(0x302), dsp_read(0x2e4), dsp_read(0x6c2), dsp_read(0x208));
		/* NEWS let go: NSIA with it */
		wr(0x1f, 0x10);
		dump("news off");
		save("v8 auto");
	}
}

/* An HDLC frame of @n bytes, as bits on the line, written to TBUFFER */
static void game_sends_frame(const u8 *p, unsigned n)
{
	u8 buf[64];
	u8 bits[1024];
	unsigned nb = 0, i, j, ones = 0;
	u16 crc = 0xffff;
	u8 cur = 0;
	int count = 0;
	memcpy(buf, p, n);
	for (i = 0; i < n; i++)
	{
		u16 word = (crc ^ buf[i]) & 0xff;
		word = (u16)(word ^ ((word << 4) & 0xff));
		word = (u16)((word << 8) ^ (word << 3) ^ (word >> 4));
		crc = (u16)((crc >> 8) ^ word);
	}
	crc ^= 0xffff;
	buf[n] = (u8)crc;
	buf[n + 1] = (u8)(crc >> 8);
	for (j = 0; j < 8; j++)
		bits[nb++] = (0x7e >> j) & 1;
	for (i = 0; i < n + 2; i++)
		for (j = 0; j < 8; j++)
		{
			const u8 b = (buf[i] >> j) & 1;
			bits[nb++] = b;
			ones = b ? ones + 1 : 0;
			if (ones == 5)
			{
				bits[nb++] = 0;
				ones = 0;
			}
		}
	for (j = 0; j < 16; j++)
		bits[nb++] = (0x7e >> (j & 7)) & 1;
	while (nb % 8 != 0)
		bits[nb++] = 1;
	for (i = 0; i < nb; i++)
	{
		cur = (u8)((cur >> 1) | (bits[i] << 7));
		if (++count == 8)
		{
			wr(0x10, cur);
			count = 0;
			cur = 0;
		}
	}
}

/* The V.8 bis detector: the game's tones, and the exchange */
static void v8bis(int on)
{
	static const u8 ms[] = { 0x11, 0xc9, 0x80, 0x80, 0x01, 0x80, 0x0e, 0x00, 0x01, 0x09, 0xb5, 0x02, 0x00, 0x94 };
	unsigned i;
	printf("v8bis %d\n", on);
	reset_modem();
	dsp_write(0x439, on, 0);
	wr(0x12, 0x81);
	wr(0x1f, 0x01);
	dump("v8bis");
	for (i = 0; i < 12; i++)
	{
		printf(" tone %02x", rd(0x16));
		now64 += 12000000ull;
	}
	printf("\n");
	wr(0x10, 0x33);
	wr(0x12, 0x76);
	wr(0x08, 0x81);
	dial();
	wr(0x09, 0x05);
	steps(2);
	dump("v8bis connected");
	printf(" secrxb %02x\n", rd(0x16));
	reads_rx(80);
	wr(0x08, 0x81);
	game_sends_frame(ms, sizeof(ms));
	dump("ms sent");
	reads_rx(20);
	dump("v8bis done");
	save("v8bis");
}

static void lost_line(void)
{
	unsigned i;
	printf("lost line\n");
	call(0x76, 1, 1);
	save("connected");
	/* a state of another line: the one that is up is hung up */
	ModemTerm();
	dump("term");
	{
		u8 copy[sizeof(state)];
		const unsigned n = state_size;
		memcpy(copy, state, n);
		save("term");
		memcpy(state, copy, n);
		state_size = n;
	}
	load("other line");
	due = 111111;
	dump("dead");
	for (i = 0; i < 6000; i++)
	{
		if (i == 10)
		{
			wr(0x08, 0x81);
			wr(0x10, 'd');
			wr(0x10, 'e');
			printf(" dead line: net out %u\n", net_out_count);
		}
		steps(1);
		if (rd(0x14) == 0x96)
			break;
	}
	printf(" lost at %u\n", i);
	/* (nothing sent over a dead line) */
	wr(0x08, 0x81);
	dump("lost");
	/* a state from before the call, loaded after the line was made */
	reset_modem();
	wr(0x12, 0x76);
	wr(0x08, 0x80);
	wr(0x09, 0x04);
	dial();
	wr(0x09, 0x05);
	steps(1);
	save("before");
	{
		u8 copy[sizeof(state)];
		const unsigned n = state_size;
		const u64 t = now64;
		const int d = due;
		memcpy(copy, state, n);
		steps(1);
		dump("made");
		memcpy(state, copy, n);
		now64 = t;
		due = d;
		load("before");
		steps(1);
		dump("made again");
		/* and at another time: another line */
		memcpy(state, copy, n);
		now64 = t + 5;
		due = d;
		load("before, later");
		steps(1);
		dump("made later");
		net_has((const u8 *)"new line", 8);
		reads_rx(12);
	}
	/* the reset line pulled while connected */
	wr(0x20, 0);
	dump("reset pulled");
}

static void bad_states(void)
{
	float f;
	s32 v;
	const unsigned v24 = 0x21 + 0x1000 + 36;
	printf("bad states\n");
	call(0x76, 1, 1);
	save("good");
	state[0x21 + 0x1000] = 9;
	state[0x21 + 0x1000 + 1] = 9;
	state[0x21 + 0x1000 + 2] = 3;
	state[0x21 + 0x1000 + 3] = 3;
	state[v24] = 0x40;
	state[v24 + 1] = 0x02;
	f = -1.f;
	memcpy(&state[v24 + 258 + 18], &f, 4);
	v = 10;
	memcpy(&state[v24 + 258 + 18 + 4], &v, 4);
	f = 0.f;
	memcpy(&state[v24 + 258 + 18 + 8], &f, 4);
	state[v24 + 258 + 18 + 12] = 5;
	load("bad");
	save("after bad");
	dump("after bad");
	v = 0x7fffffff;
	memcpy(&state[v24 + 258 + 18 + 4], &v, 4);
	f = 100.f;
	memcpy(&state[v24 + 258 + 18], &f, 4);
	f = 17.f;
	memcpy(&state[v24 + 258 + 18 + 8], &f, 4);
	load("bad 2");
	save("after bad 2");
	/* before V24: what the receive buffer holds is all the FIFO has */
	wr(0x1e, 0x00);
	modem_state_before_v24();
	save("before v24");
	modem_state_before_v21();
	/* not a Dreamcast */
	settings.System = DC_PLATFORM_NAOMI;
	save("naomi");
	load("naomi");
	settings.System = DC_PLATFORM_DREAMCAST;
}

int main(void)
{
	settings.System = DC_PLATFORM_DREAMCAST;
	save("power on");
	p_sh4rcb = &machine;
	ModemInit();
	ModemTerm();
	registers();
	async_call();
	sync_call();
	speeds();
	fractions();
	v8auto();
	v8bis(1);
	v8bis(0);
	lost_line();
	bad_states();
	ModemTerm();
	dump("term");
	ModemTerm();
	save("end");
	return 0;
}
