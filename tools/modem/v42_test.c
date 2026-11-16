/*
 * The far end of a synchronous modem connection (core/hw/modem/v42.c), by
 * itself: V.14 framing both ways, HDLC frames, the V.42 protocol from the
 * detection phase to an I-frame exchange (compressed or not), rejected
 * frames, timers and the frames it is sent that are wrong, the V.14 mode
 * after the detection phase times out, the V.8 bis exchange, and the save
 * state of each and what is wrong in one. What they send and give out, and
 * their states, are hashed, and must be what they always were
 * (v42_expected.txt).
 *
 * The test goes through tv_*(): the parts as the C file has them here; as
 * the classes had them, in the shim the old code was checked with.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "hw/modem/v42bis.h"

/* (no log) */
retro_log_printf_t log_cb;
void os_DebugBreak(void) { printf("FAIL: os_DebugBreak\n"); exit(1); }

/* The time, and a machine to have one */
static u64 now64 = 1000;
static int machine;
void *p_sh4rcb = &machine;
u64 sh4_sched_now64(void) { return now64; }

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

/* The network's side: what it has to send, and what it is given */
static u8 in_buf[8192];
static unsigned in_len, in_pos;
static u32 out_hash = 2166136261u;
static unsigned out_count;

int t_in_read(void)
{
	return in_pos < in_len ? in_buf[in_pos++] : -1;
}
int t_in_available(void)
{
	return (int)(in_len - in_pos);
}
void t_out_write(u8 v)
{
	out_hash = (out_hash ^ v) * 16777619u;
	out_count++;
}

/* The parts, one of each */
void tv_setup(void);
void tv_v14e_write(u8 v);
void tv_v14e_flush(void);
void tv_v14e_reset(void);
int tv_v14e_read(void);
int tv_v14e_available(void);
void tv_v14e_stop_bits(int n);
void tv_v14e_serialize(void **data, unsigned int *total_size);
void tv_v14e_unserialize(void **data, unsigned int *total_size);
void tv_v14d_write(u8 v);
int tv_v14d_read(void);
int tv_v14d_stop_bits(void);
void tv_v14d_reset(void);
void tv_v14d_serialize(void **data, unsigned int *total_size);
void tv_v14d_unserialize(void **data, unsigned int *total_size);
int tv_he_frame(const u8 *data, unsigned len);
void tv_he_flag(void);
void tv_he_reset(void);
int tv_he_read(void);
int tv_he_available(void);
void tv_he_serialize(void **data, unsigned int *total_size);
void tv_he_unserialize(void **data, unsigned int *total_size);
void tv_hd_write(u8 v);
int tv_hd_ready(void);
const u8 *tv_hd_frame(void);
unsigned tv_hd_size(void);
void tv_hd_taken(void);
void tv_hd_reset(void);
void tv_hd_serialize(void **data, unsigned int *total_size);
void tv_hd_unserialize(void **data, unsigned int *total_size);
int tv_v42_read(void);
void tv_v42_write(u8 v);
void tv_v42_reset(void);
void tv_v42_serialize(void **data, unsigned int *total_size);
void tv_v42_unserialize(void **data, unsigned int *total_size);
void tv_v8_tone(u8 tone);
u8 tv_v8_detect(void);
int tv_v8_read(void);
int tv_v8_available(void);
void tv_v8_write(u8 v);
void tv_v8_reset(void);
int tv_v8_completed(void);
void tv_v8_serialize(void **data, unsigned int *total_size);
void tv_v8_unserialize(void **data, unsigned int *total_size);

#ifndef V42_TEST_OLD
#include "hw/modem/v42.h"

static int s_read(void *ctx) { (void)ctx; return t_in_read(); }
static int s_available(void *ctx) { (void)ctx; return t_in_available(); }
static void s_write(void *ctx, u8 v) { (void)ctx; t_out_write(v); }
static const modem_stream tin = { s_read, s_available, NULL, NULL };
static const modem_stream tout = { NULL, NULL, s_write, NULL };

static v14_encoder v14e;
static v14_decoder v14d;
static hdlc_encoder he;
static u8 he_buf[256];
static hdlc_decoder hd;
static u8 hd_frame[128];
static v42_protocol v42;
static v8bis_protocol v8;

void tv_setup(void)
{
	v14_encoder_init(&v14e);
	v14_decoder_init(&v14d);
	hdlc_encoder_init(&he, he_buf, sizeof(he_buf));
	hdlc_decoder_init(&hd, hd_frame, sizeof(hd_frame));
	v42_init(&v42, &tin, &tout);
	v8bis_init(&v8);
}
void tv_v14e_write(u8 v) { v14_encoder_write(&v14e, v); }
void tv_v14e_flush(void) { v14_encoder_flush(&v14e); }
void tv_v14e_reset(void) { v14_encoder_reset(&v14e); }
int tv_v14e_read(void) { return v14_encoder_read(&v14e); }
int tv_v14e_available(void) { return v14_encoder_available(&v14e); }
void tv_v14e_stop_bits(int n) { v14_encoder_set_stop_bits(&v14e, n); }
void tv_v14e_serialize(void **d, unsigned int *t) { v14_encoder_serialize(&v14e, d, t); }
void tv_v14e_unserialize(void **d, unsigned int *t) { v14_encoder_unserialize(&v14e, d, t); }
void tv_v14d_write(u8 v) { v14_decoder_write(&v14d, v); }
int tv_v14d_read(void) { return v14_decoder_read(&v14d); }
int tv_v14d_stop_bits(void) { return v14_decoder_received_stop_bits(&v14d); }
void tv_v14d_reset(void) { v14_decoder_reset(&v14d); }
void tv_v14d_serialize(void **d, unsigned int *t) { v14_decoder_serialize(&v14d, d, t); }
void tv_v14d_unserialize(void **d, unsigned int *t) { v14_decoder_unserialize(&v14d, d, t); }
int tv_he_frame(const u8 *data, unsigned len) { return hdlc_encoder_send_frame(&he, data, len); }
void tv_he_flag(void) { hdlc_encoder_send_flag(&he); }
void tv_he_reset(void) { hdlc_encoder_reset(&he); }
int tv_he_read(void) { return hdlc_encoder_read(&he); }
int tv_he_available(void) { return hdlc_encoder_available(&he); }
void tv_he_serialize(void **d, unsigned int *t) { hdlc_encoder_serialize(&he, d, t); }
void tv_he_unserialize(void **d, unsigned int *t) { hdlc_encoder_unserialize(&he, d, t); }
void tv_hd_write(u8 v) { hdlc_decoder_write(&hd, v); }
int tv_hd_ready(void) { return hdlc_decoder_frame_available(&hd); }
const u8 *tv_hd_frame(void) { return hdlc_decoder_frame(&hd); }
unsigned tv_hd_size(void) { return hdlc_decoder_frame_size(&hd); }
void tv_hd_taken(void) { hdlc_decoder_frame_taken(&hd); }
void tv_hd_reset(void) { hdlc_decoder_reset(&hd); }
void tv_hd_serialize(void **d, unsigned int *t) { hdlc_decoder_serialize(&hd, d, t); }
void tv_hd_unserialize(void **d, unsigned int *t) { hdlc_decoder_unserialize(&hd, d, t); }
int tv_v42_read(void) { return v42_read(&v42); }
void tv_v42_write(u8 v) { v42_write(&v42, v); }
void tv_v42_reset(void) { v42_reset(&v42); }
void tv_v42_serialize(void **d, unsigned int *t) { v42_serialize(&v42, d, t); }
void tv_v42_unserialize(void **d, unsigned int *t) { v42_unserialize(&v42, d, t); }
void tv_v8_tone(u8 tone) { v8bis_emit_tone(&v8, tone); }
u8 tv_v8_detect(void) { return v8bis_detect_tone(&v8); }
int tv_v8_read(void) { return v8bis_read(&v8); }
int tv_v8_available(void) { return v8bis_available(&v8); }
void tv_v8_write(u8 v) { v8bis_write(&v8, v); }
void tv_v8_reset(void) { v8bis_reset(&v8); }
int tv_v8_completed(void) { return v8bis_completed(&v8); }
void tv_v8_serialize(void **d, unsigned int *t) { v8bis_serialize(&v8, d, t); }
void tv_v8_unserialize(void **d, unsigned int *t) { v8bis_unserialize(&v8, d, t); }
#endif

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

static u8 state[200000];
static unsigned state_size;

typedef void (*ser_fn)(void **data, unsigned int *total_size);

static void save(const char *what, ser_fn fn)
{
	void *p = state;
	unsigned int total = 0;
	fn(&p, &total);
	state_size = total;
	if (what != NULL)
		printf(" state %s %u %08x\n", what, total, hash(state, total));
}

static void load(ser_fn fn)
{
	void *p = state;
	unsigned int total = 0;
	fn(&p, &total);
	if (total != state_size)
		printf(" FAIL: loaded %u of %u\n", total, state_size);
}

/*
 * V.14
 */
static void v14_tests(void)
{
	static const int stops[] = { 1, 2, 9, 17 };
	unsigned i, k;
	printf("v14\n");
	for (k = 0; k < 4; k++)
	{
		u32 h = 2166136261u;
		unsigned n = 0;
		int c;
		tv_v14e_reset();
		tv_v14e_stop_bits(stops[k]);
		for (i = 0; i < 40; i++)
			tv_v14e_write((u8)(i * 37 + k));
		if (k == 1)
			save("encoder", tv_v14e_serialize);
		tv_v14e_flush();
		tv_v14e_flush();
		while ((c = tv_v14e_read()) != -1)
		{
			h = fnv(h, (u8)c);
			n++;
			tv_v14d_write((u8)c);
			if (n % 7 == 0)
				printf(" d%u %d\n", n, tv_v14d_stop_bits());
		}
		printf(" stop %d: %u %08x avail %d\n", stops[k], n, h, tv_v14e_available());
		h = 2166136261u;
		n = 0;
		while ((c = tv_v14d_read()) != -1)
		{
			h = fnv(h, (u8)c);
			n++;
		}
		printf(" decoded %u %08x stop bits %d\n", n, h, tv_v14d_stop_bits());
	}
	/* more characters than are counted */
	tv_v14d_reset();
	for (i = 0; i < 80000; i++)
	{
		tv_v14d_write(0x00);
		tv_v14d_write(0xfe);
		tv_v14d_read();
	}
	save("decoder many", tv_v14d_serialize);
	/* bits that are not V.14: a stop bit missing, runs of ones */
	for (i = 0; i < 64; i++)
		tv_v14d_write((u8)(i * 91 + 3));
	save("decoder", tv_v14d_serialize);
	for (i = 0; i < 20000; i++)
		tv_v14d_write(0xff);
	save("decoder idle", tv_v14d_serialize);
	tv_v14d_write(0x00);
	save("decoder start", tv_v14d_serialize);
	tv_v14d_reset();
	save("decoder reset", tv_v14d_serialize);
	/* states that are wrong */
	tv_v14e_reset();
	tv_v14e_write(0x55);
	save("encoder one", tv_v14e_serialize);
	state[0] = 0xff;
	state[1] = 0xff;
	state[state_size - 1] = 0;
	state[state_size - 2] = 0xff;
	load(tv_v14e_unserialize);
	save("encoder bad", tv_v14e_serialize);
	state[state_size - 1] = 17;
	load(tv_v14e_unserialize);
	save("encoder bad stops", tv_v14e_serialize);
	tv_v14e_write(0x0f);
	printf(" %d %d\n", tv_v14e_available(), tv_v14e_read());
	save("decoder", tv_v14d_serialize);
	{
		const unsigned st = 2 + 16 + 1;
		s32 v = 0x20000;
		state[st] = 7;
		state[st + 1] = 9;
		memcpy(&state[st + 2], &v, 4);
		v = 0x7fffffff;
		memcpy(&state[st + 6], &v, 4);
		memcpy(&state[st + 10], &v, 4);
		state[0] = 99;
		load(tv_v14d_unserialize);
		save("decoder bad", tv_v14d_serialize);
		state[st] = 1;
		state[st + 1] = 8;
		load(tv_v14d_unserialize);
		save("decoder bad 2", tv_v14d_serialize);
		state[st] = 2;
		load(tv_v14d_unserialize);
		save("decoder bad 3", tv_v14d_serialize);
		state[st] = 3;
		v = -5;
		memcpy(&state[st + 2], &v, 4);
		load(tv_v14d_unserialize);
		save("decoder bad 4", tv_v14d_serialize);
	}
}

/*
 * HDLC
 */
static void hdlc_feed(int (*rd)(void), int each)
{
	int c;
	while ((c = rd()) != -1)
	{
		tv_hd_write((u8)c);
		if (tv_hd_ready())
		{
			printf(" frame %u %08x\n", tv_hd_size(), hash(tv_hd_frame(), tv_hd_size()));
			if (each)
				tv_hd_taken();
		}
	}
}

static void hdlc_tests(void)
{
	u8 f[300];
	unsigned i, n;
	printf("hdlc\n");
	for (i = 0; i < sizeof(f); i++)
		f[i] = (u8)(i * 13 + (i >> 3));
	/* frames of each length, ones to be stuffed in them */
	for (n = 0; n < 140; n += 7)
	{
		f[0] = (u8)n;
		printf(" send %u: %d", n, tv_he_frame(f, n));
		printf(" %d\n", tv_he_available());
		hdlc_feed(tv_he_read, 1);
	}
	memset(f, 0xff, 64);
	printf(" ones: %d\n", tv_he_frame(f, 64));
	hdlc_feed(tv_he_read, 0);
	for (i = 0; i < 64; i++)
		f[i] = (u8)(0x7e ^ (i & 1 ? 0x81 : 0));
	tv_he_flag();
	tv_he_frame(f, 64);
	tv_he_flag();
	hdlc_feed(tv_he_read, 1);
	/* no room */
	printf(" big: %d %d\n", tv_he_frame(f, 220), tv_he_available());
	for (i = 0; i < 3; i++)
		printf(" again: %d %d\n", tv_he_frame(f, 60), tv_he_available());
	save("encoder full", tv_he_serialize);
	hdlc_feed(tv_he_read, 1);
	for (i = 0; i < 12; i++)
	{
		unsigned k;
		for (n = 150; n < 260; n++)
		{
			tv_he_reset();
			for (k = 0; k < i * 3; k++)
				tv_he_flag();
			if (!tv_he_frame(f, n))
				break;
		}
		printf(" room %u: %u\n", i, n);
	}
	tv_he_reset();
	/* a frame cut in the middle of a byte, saved */
	tv_he_frame(f, 33);
	for (i = 0; i < 10; i++)
		tv_hd_write((u8)tv_he_read());
	save("decoder mid", tv_hd_serialize);
	tv_hd_reset();
	load(tv_hd_unserialize);
	hdlc_feed(tv_he_read, 1);
	/* a frame not taken: gone with the next byte */
	tv_he_frame(f, 9);
	hdlc_feed(tv_he_read, 0);
	tv_hd_write(0xff);
	printf(" not taken %d\n", tv_hd_ready());
	/* an idle line */
	for (i = 0; i < 4; i++)
		tv_hd_write(0xff);
	save("decoder idle", tv_hd_serialize);
	/* too long, too short, a bad CRC, an abort */
	tv_he_reset();
	for (i = 0; i < 3; i++)
	{
		tv_he_frame(f, 70);
		hdlc_feed(tv_he_read, 1);
	}
	f[0] = 1;
	tv_he_frame(f, 1);
	tv_he_frame(f, 0);
	hdlc_feed(tv_he_read, 1);
	tv_he_frame(f, 30);
	for (i = 0; (int)i < 20; i++)
		tv_hd_write((u8)tv_he_read());
	tv_hd_write(0x01);
	tv_hd_write(0xff);
	tv_hd_write(0x00);
	hdlc_feed(tv_he_read, 1);
	tv_he_frame(f, 30);
	for (i = 0; i < 15; i++)
		tv_hd_write((u8)tv_he_read());
	tv_hd_write(0xfe);
	tv_hd_write(0xff);
	tv_hd_write(0x01);
	hdlc_feed(tv_he_read, 1);
	tv_he_frame(f, 30);
	for (i = 0; i < 5; i++)
		tv_hd_write((u8)tv_he_read());
	tv_hd_write(0x55);
	tv_hd_write(0x33);
	hdlc_feed(tv_he_read, 1);
	/* 200 bytes into 128 */
	tv_he_frame(f, 200);
	save("decoder full", tv_hd_serialize);
	hdlc_feed(tv_he_read, 1);
	save("decoder over", tv_hd_serialize);
	/* the states, wrong */
	tv_he_frame(f, 3);
	tv_he_read();
	save("encoder", tv_he_serialize);
	state[0] = 0x00;
	state[1] = 0x40;
	state[state_size - 1] = 0xff;
	load(tv_he_unserialize);
	save("encoder bad", tv_he_serialize);
	hdlc_feed(tv_he_read, 1);
	save("decoder", tv_hd_serialize);
	state[0] = 0x50;
	state[1] = 0;
	state[state_size - 3] = 30;
	state[state_size - 2] = 30;
	load(tv_hd_unserialize);
	save("decoder bad", tv_hd_serialize);
	state[0] = 0x10;
	load(tv_hd_unserialize);
	save("decoder bad 2", tv_hd_serialize);
	state[0] = 0x80;
	load(tv_hd_unserialize);
	save("decoder full size", tv_hd_serialize);
	/* overflowed, with nothing in it: the frame after is good */
	state[0] = 0;
	state[state_size - 4] = 1;
	state[state_size - 3] = 0;
	state[state_size - 2] = 0;
	load(tv_hd_unserialize);
	tv_he_reset();
	tv_he_frame(f, 9);
	hdlc_feed(tv_he_read, 1);
}

/*
 * V.42
 */
static u32 rd_hash;
static unsigned rd_count;
static u32 quiet_hash = 2166136261u;

/* What the game is sent, @n bytes of it */
static void game_reads(unsigned n, const char *what)
{
	u32 h = 2166136261u;
	unsigned i;
	int first[6];
	for (i = 0; i < n; i++)
	{
		const int c = tv_v42_read();
		if (i < 6)
			first[i] = c;
		h = fnv(fnv(h, (u8)c), c < 0);
		rd_hash = fnv(rd_hash, (u8)c);
		rd_count++;
		/* (what it sends, in frames, the game's side decodes) */
		tv_hd_write((u8)c);
		if (tv_hd_ready())
		{
			const u8 *fr = tv_hd_frame();
			const unsigned sz = tv_hd_size();
			if (what != NULL)
				printf("  got %u: %02x %02x %02x %08x\n", sz, sz > 0 ? fr[0] : 0, sz > 1 ? fr[1] : 0,
						sz > 2 ? fr[2] : 0, hash(fr, sz));
			else
				quiet_hash = fnv(quiet_hash, (u8)hash(fr, sz));
			tv_hd_taken();
		}
	}
	if (what != NULL)
		printf(" read %s %u: %02x %02x %02x %02x %08x out %u %08x in %u\n", what, n,
			first[0] & 0xff, first[1] & 0xff, first[2] & 0xff, first[3] & 0xff, h,
			out_count, out_hash, in_pos);
}

/* The game sends @n characters, V.14 framed with @stops stop bits */
static void game_sends_v14(const u8 *p, unsigned n, int stops)
{
	unsigned i;
	int c;
	tv_v14e_reset();
	tv_v14e_stop_bits(stops);
	for (i = 0; i < n; i++)
		tv_v14e_write(p[i]);
	tv_v14e_stop_bits(1);
	tv_v14e_flush();
	while ((c = tv_v14e_read()) != -1)
		tv_v42_write((u8)c);
}

/* The game sends a frame */
static void game_sends(const u8 *p, unsigned n)
{
	int c;
	tv_he_reset();
	tv_he_flag();
	tv_he_frame(p, n);
	tv_he_flag();
	while ((c = tv_he_read()) != -1)
		tv_v42_write((u8)c);
}

static void game_sends_raw(u8 v)
{
	tv_v42_write(v);
}

static void net_has(const char *s, unsigned n)
{
	memmove(in_buf, in_buf + in_pos, in_len - in_pos);
	in_len -= in_pos;
	in_pos = 0;
	memcpy(in_buf + in_len, s, n);
	in_len += n;
}

/* The detection phase: the game's ODP, with 17 stop bits or 9 (the
 * characters of one call follow each other, the first one after the line
 * was idle), and the ADP answered; then the first flag */
static void detection(int variant)
{
	static const u8 odp[] = { 0x11, 0x91, 0x11, 0x91, 0x11 };
	static const u8 same[] = { 0x11, 0x11, 0x11, 0x11 };
	static const u8 flag[] = { 0x7e };
	game_reads(8, "none");
	if (variant == 0)
	{
		game_sends_v14(odp, 4, 17);
		game_reads(8, "three of 17");
		game_sends_v14(flag, 1, 1);
		game_reads(4, "establish");
		return;
	}
	game_sends_v14(odp, 1, 1);
	game_reads(4, "not odp");
	game_sends_v14(same, 4, 9);
	game_reads(8, "same parity");
	game_sends_v14(odp, 5, 9);
	game_reads(8, "adp");
	game_sends_v14(odp, 4, 9);
	game_reads(8, "three");
	game_sends_v14(flag, 1, 1);
	game_reads(4, "establish");
}

static void v42_session(int compressed)
{
	static const u8 sabme[] = { 0x03, 0x7f };
	static const u8 xid_plain[] = {
		0x03, 0xbf, 0x82,
		0x80, 0x00, 0x17,
			0x03, 0x01, 0x8a,
			0x05, 0x02, 0x40, 0x00,		/* tx: 16384 bits */
			0x06, 0x02, 0x04, 0x00,		/* rx: 1024 bits */
			0x07, 0x01, 0x20,			/* tx window 32 */
			0x08, 0x01, 0x07,			/* rx window 7 */
			0x09, 0x01, 0x01,			/* unknown */
			0x07, 0x04, 0x01,			/* past the group's end */
		0x55, 0x00, 0x02, 0x01, 0x02,	/* unknown group */
		0xf0, 0x00, 0x12,
			0x00, 0x03, 'V', '4', '2',
			0x01, 0x01, 0x02,			/* one way only */
			0x02, 0x08, 1, 2, 3, 4, 5, 6, 7, 8,
		0xff, 0x00, 0x02, 0xaa, 0xbb
	};
	static const u8 xid_comp[] = {
		0x03, 0xbf, 0x82,
		0x80, 0x00, 0x0a,
			0x05, 0x02, 0x00, 0x40,		/* tx: 64 bits */
			0x07, 0x01, 0x03,			/* tx window 3 */
			0x0a, 0x01, 0x00,
		0xf0, 0x00, 0x11,
			0x00, 0x03, 'V', '4', '2',
			0x01, 0x01, 0x03,
			0x02, 0x04, 0x00, 0x00, 0x10, 0x00,	/* 4096 codewords */
			0x03, 0x01, 0xfb,			/* 251 a string */
		0x80, 0x00, 0x10,			/* a group longer than the frame */
			0x07, 0x01, 0x05
	};
	u8 f[300];
	unsigned i, seq = 0;
	printf("v42 %d\n", compressed);
	tv_v42_reset();
	tv_hd_reset();
	detection(compressed);
	save("v42 establish", tv_v42_serialize);
	/* an I-frame before the link is set up: not taken */
	f[0] = 0x03;
	f[1] = 0x00;
	f[2] = 0x00;
	f[3] = 'x';
	game_sends(f, 4);
	game_reads(10, "i too soon");
	if (compressed)
		game_sends(xid_comp, sizeof(xid_comp));
	else
		game_sends(xid_plain, sizeof(xid_plain));
	game_reads(80, "xid");
	game_sends(sabme, sizeof(sabme));
	game_reads(10, "ua");
	if (compressed == 2)
	{
		/* a reserved command after the escape: the link is released */
		f[0] = 0x03;
		f[1] = 0x00;
		f[2] = 0x00;
		f[3] = 0x00;
		f[4] = 0x07;
		f[5] = 'a';
		game_sends(f, 6);
		game_reads(20, "v42bis error");
		f[1] = 0x02;
		game_sends(f, 6);
		game_reads(20, "released");
		save("v42 released", tv_v42_serialize);
		return;
	}
	/* data from the network, in frames */
	for (i = 0; i < 600; i++)
		f[i % 300] = (u8)(i < 300 ? 'a' + i % 7 : i * 7);
	net_has((const char *)f, 300);
	game_reads(300, "data");
	save("v42 sending", tv_v42_serialize);
	/* frames from the game */
	for (i = 0; i < 4; i++)
	{
		unsigned k;
		f[0] = 0x03;
		f[1] = (u8)(seq << 1);
		f[2] = (u8)((i * 2) << 1 | (i == 2));
		if (compressed)
		{
			v42b_compressor c;
			int b;
			unsigned n = 3;
			v42b_compressor_init(&c);
			v42b_compressor_set_params(&c, 2048, 250);
			v42b_compressor_reset(&c);
			for (k = 0; k < 40; k++)
				v42b_compressor_write(&c, (u8)('A' + (k + i) % 5));
			v42b_compressor_flush(&c);
			while ((b = v42b_compressor_read(&c)) != -1)
				f[n++] = (u8)b;
			if (i == 0)
				game_sends(f, n);
			else
			{
				/* (the dictionary is not the same: errors, or not) */
				f[3] = 0xff;
				f[4] = 0xff;
				game_sends(f, n);
			}
		}
		else
		{
			for (k = 3; k < 3 + 20 + i; k++)
				f[k] = (u8)(k * i + 1);
			game_sends(f, 3 + 20 + i);
		}
		seq++;
		game_reads(30, "after i");
	}
	/* out of sequence */
	f[0] = 0x03;
	f[1] = (u8)(40 << 1);
	f[2] = 0;
	game_sends(f, 10);
	game_reads(20, "reject sent");
	/* supervisory frames: RNR, RR, REJ, SREJ, a bad ack */
	f[1] = 0x05;
	f[2] = 2 << 1;
	game_sends(f, 3);
	net_has("more data to send now", 21);
	game_reads(20, "rnr");
	f[1] = 0x01;
	game_sends(f, 3);
	game_reads(60, "rr");
	f[1] = 0x09;
	f[2] = 2 << 1;
	game_sends(f, 3);
	game_reads(200, "rej");
	save(NULL, tv_v42_serialize);
	f[1] = 0x0d;
	f[2] = (u8)(state[3] << 1);
	game_sends(f, 3);
	save("v42 srej", tv_v42_serialize);
	f[1] = 0x01;
	f[2] = 100 << 1;
	game_sends(f, 3);
	f[1] = 0x09;
	f[2] = 120 << 1;
	game_sends(f, 3);
	game_reads(20, "bad acks");
	/* not ready, then ready again by a REJ, and by a UA */
	f[1] = 0x05;
	f[2] = 2 << 1;
	game_sends(f, 3);
	net_has("abcdefghijklmnopqrstuvwxyz", 26);
	game_reads(20, "rnr 2");
	f[1] = 0x09;
	game_sends(f, 3);
	game_reads(40, "rej ready");
	f[1] = 0x05;
	game_sends(f, 3);
	net_has("abcdefghijklmnopqrstuvwxyz", 26);
	game_reads(20, "rnr 3");
	f[1] = 0x63;
	game_sends(f, 2);
	game_reads(40, "ua ready");
	/* frames from an address that is not this link's */
	f[0] = 0x02;
	f[1] = 0x7f;
	game_sends(f, 2);
	f[0] = 0x07;
	game_sends(f, 2);
	save("v42 wrong address", tv_v42_serialize);
	f[1] = 0x01;
	f[0] = 0x03;
	game_sends(f, 1);
	f[1] = 0x00;
	game_sends(f, 2);
	f[1] = 0x0f;	/* DM */
	game_sends(f, 2);
	f[1] = 0x03;	/* UI */
	game_sends(f, 2);
	f[1] = 0x87;	/* FRMR */
	game_sends(f, 2);
	f[1] = 0xe3;	/* TEST */
	game_sends(f, 2);
	f[1] = 0x63;	/* UA */
	game_sends(f, 2);
	f[1] = 0x6b;	/* not a command */
	game_sends(f, 2);
	f[1] = 0xaf;	/* XID, not of the format */
	f[2] = 0x81;
	game_sends(f, 3);
	game_sends(f, 2);
	game_reads(20, "wrong");
	/* the window full, acknowledged */
	for (i = 0; i < 3000; i++)
		f[i % 300] = (u8)(i * 3);
	for (i = 0; i < 10; i++)
		net_has((const char *)f, 300);
	game_reads(2000, "window");
	save("v42 window", tv_v42_serialize);
	f[0] = 0x03;
	f[1] = 0x01;
	f[2] = 0x7f << 1;
	game_sends(f, 3);
	game_reads(500, "acked");
	/* an acknowledgement 65 frames on: of none sent */
	net_has("0123456789", 10);
	game_reads(100, "unacked");
	save(NULL, tv_v42_serialize);
	f[0] = 0x03;
	f[1] = 0x01;
	f[2] = (u8)(((state[5] + 65) & 127) << 1);
	game_sends(f, 3);
	save("v42 far ack", tv_v42_serialize);
	if (compressed)
	{
		/* all of it sent, to the last bits: once the compressor has gone
		 * on to compressing */
		unsigned k;
		in_pos = in_len;
		for (k = 0; k < 6; k++)
			net_has("abcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabc", 60);
		for (k = 0; k < 120; k++)
		{
			game_reads(40, k % 30 == 0 ? "repeated" : NULL);
			save(NULL, tv_v42_serialize);
			f[0] = 0x03;
			f[1] = 0x01;
			f[2] = (u8)(state[3] << 1);
			game_sends(f, 3);
			if (k == 100)
				net_has("xyz", 3);
		}
		game_reads(100, "tail");
	}
	/* frames sent, rejected, and the frame after, a game's frame received
	 * in between, all acknowledged before they are sent again */
	{
		unsigned k, j;
		for (k = 0; k < 5; k++)
		{
			for (j = 0; j < 300; j++)
				f[j] = (u8)(j + k);
			net_has((const char *)f, 300);
			game_reads(400, "more");
			save("v42 sent", tv_v42_serialize);
			f[0] = 0x03;
			f[1] = (u8)(seq << 1);
			f[2] = 0;
			f[3] = 'q';
			game_sends(f, 4);
			seq = (seq + 1) & 127;
			f[1] = 0x09;
			f[2] = (u8)((state[5] + 1 + k) << 1);
			game_sends(f, 3);
			if (k == 3)
			{
				f[1] = 0x01;
				f[2] = (u8)(state[3] << 1);
				game_sends(f, 3);
			}
			game_reads(600, "resent");
		}
	}
	/* frames for long, each acknowledged: the numbers go round */
	{
		unsigned k;
		for (k = 0; k < 140; k++)
		{
			net_has("0123456789abcdefghij", 20);
			game_reads(30, k % 20 == 0 ? "round" : NULL);
			save(k % 20 == 0 ? "round" : NULL, tv_v42_serialize);
			f[0] = 0x03;
			f[1] = 0x01;
			f[2] = (u8)(state[3] << 1);
			game_sends(f, 3);
		}
	}
	/* the inactivity timer */
	in_pos = in_len;
	game_reads(50, "idle");
	now64 += 200000000ull;
	game_reads(50, "inactive");
	/* SABME again, DISC */
	game_sends(sabme, sizeof(sabme));
	game_reads(20, "sabme again");
	f[0] = 0x03;
	f[1] = 0x53;
	game_sends(f, 2);
	game_reads(20, "disc");
	f[1] = 0x00;
	game_sends(f, 10);
	game_reads(20, "after disc");
	save("v42 end", tv_v42_serialize);
	printf(" all %u %08x %08x\n", rd_count, rd_hash, quiet_hash);
}

static void v42_v14_mode(void)
{
	unsigned i;
	printf("v42 v14\n");
	tv_v42_reset();
	game_reads(4, "before");
	now64 += 160000000ull;
	game_sends_v14((const u8 *)"hello", 5, 1);
	game_reads(4, "v14");
	net_has("from the network", 16);
	game_reads(40, "v14 data");
	game_sends_v14((const u8 *)"bye", 3, 2);
	for (i = 0; i < 200; i++)
		in_buf[i] = (u8)i;
	in_len = 200;
	in_pos = 0;
	game_reads(300, "v14 more");
	save("v42 v14", tv_v42_serialize);
}

static void v42_states(void)
{
	u8 copy[sizeof(state)];
	unsigned n;
	printf("v42 states\n");
	tv_v42_reset();
	detection(1);
	game_sends((const u8 *)"\x03\x7f", 2);
	game_reads(10, "ua");
	save("good", tv_v42_serialize);
	n = state_size;
	memcpy(copy, state, n);
	load(tv_v42_unserialize);
	save("same", tv_v42_serialize);
	/* the phase, the counts, the window, the frame sizes */
	state[0] = 9;
	state[2] = 7;
	state[3] = 0x85;
	state[4] = 0xff;
	state[5] = 0x80;
	state[7] = 0;
	state[8] = 0xff;
	state[11] = 0;
	state[12] = 0;
	state[29] = 0x04;
	state[30] = 0x01;
	state[31] = 0x03;
	state[32] = 0x01;
	load(tv_v42_unserialize);
	save("bad", tv_v42_serialize);
	memcpy(state, copy, n);
	state[7] = 3;
	state[3] = 10;
	state[5] = 1;
	state[11] = 0x01;
	state[12] = 0x01;
	load(tv_v42_unserialize);
	save("bad window", tv_v42_serialize);
	state[11] = 0x00;
	state[12] = 0x01;
	state[7] = 16;
	load(tv_v42_unserialize);
	save("bad size", tv_v42_serialize);
	game_reads(10, "after");
}

/*
 * V.8 bis
 */
static void v8_reads(const char *what)
{
	u32 h = 2166136261u;
	unsigned n = 0;
	int c;
	printf(" %s available %d", what, tv_v8_available());
	while ((c = tv_v8_read()) != -1)
	{
		h = fnv(h, (u8)c);
		n++;
		tv_hd_write((u8)c);
		if (tv_hd_ready())
		{
			printf(" [%u %02x]", tv_hd_size(), tv_hd_frame()[0]);
			tv_hd_taken();
		}
	}
	printf(" %u %08x done %d\n", n, h, tv_v8_completed());
}

static void v8_sends(const u8 *p, unsigned n)
{
	int c;
	tv_he_reset();
	tv_he_flag();
	tv_he_frame(p, n);
	tv_he_flag();
	while ((c = tv_he_read()) != -1)
		tv_v8_write((u8)c);
}

static void v8_tests(void)
{
	static const u8 ms_full[] = {
		0x11, 0xc9, 0x80, 0x80, 0x01, 0x80, 0x0e, 0x00, 0x01, 0x09, 0xb5, 0x02, 0x00, 0x94
	};
	static const u8 ms_v34[] = { 0x11, 0x88, 0x80, 0x80, 0x01, 0x80, 0x0a, 0x10, 0x80 };
	static const u8 ms_v32b[] = { 0x11, 0x81, 0x80, 0x80, 0x01, 0x80, 0x02, 0x20, 0x80 };
	static const u8 ms_low[] = { 0x11, 0x81, 0x01, 0x80, 0x80, 0x01, 0x80, 0x02, 0x00, 0x04 };
	static const u8 ms_v21[] = { 0x11, 0x81, 0x80, 0x80, 0x01, 0x80, 0x02, 0x00, 0x08 };
	static const u8 ms_v22b[] = { 0x11, 0x81, 0x80, 0x80, 0x01, 0x80, 0x02, 0x00, 0x82 };
	static const u8 ms_v32[] = { 0x11, 0x81, 0x80, 0x80, 0x01, 0x80, 0x02, 0x00, 0x81 };
	static const u8 ms_nodata[] = { 0x11, 0x88, 0x80, 0x80, 0x00, 0x80 };
	static const u8 ms_short[] = { 0x11 };
	static const u8 other[] = { 0x13, 0x80 };
	static const u8 *const msgs[] = { ms_full, ms_v34, ms_v32b, ms_low, ms_v21, ms_v22b, ms_v32, ms_nodata, ms_short, other };
	static const unsigned sizes[] = { sizeof(ms_full), sizeof(ms_v34), sizeof(ms_v32b), sizeof(ms_low),
		sizeof(ms_v21), sizeof(ms_v22b), sizeof(ms_v32), sizeof(ms_nodata), sizeof(ms_short), sizeof(other) };
	unsigned i, t;
	printf("v8bis\n");
	for (i = 0; i < sizeof(msgs) / sizeof(msgs[0]); i++)
	{
		tv_v8_reset();
		tv_hd_reset();
		v8_reads("start");
		tv_v8_tone(0x33);
		v8_sends(msgs[i], sizes[i]);
		v8_reads("tone too soon");
		for (t = 0; t < 12; t++)
		{
			printf(" %02x", tv_v8_detect());
			now64 += 12000000ull;
		}
		printf("\n");
		tv_v8_tone(0x32);
		v8_reads("other tone");
		tv_v8_tone(0x33);
		if (i == 0)
			save("v8 cl", tv_v8_serialize);
		v8_reads("cl");
		v8_sends(msgs[i], sizes[i]);
		printf(" completed %d\n", tv_v8_completed());
		v8_reads("ms");
		save("v8", tv_v8_serialize);
	}
	/* states that are wrong */
	state[state_size - 11] = 7;
	load(tv_v8_unserialize);
	save("v8 bad", tv_v8_serialize);
	state[state_size - 11] = 2;
	state[state_size - 10] = 0;
	state[state_size - 9] = 5;
	load(tv_v8_unserialize);
	save("v8 bad 2", tv_v8_serialize);
	v8_reads("bad");
}

int main(void)
{
	/* (made before there is a machine, as the modem's are) */
	p_sh4rcb = NULL;
	tv_setup();
	p_sh4rcb = &machine;
	save("v42 set up", tv_v42_serialize);
	save("v8 set up", tv_v8_serialize);
	v14_tests();
	hdlc_tests();
	tv_hd_reset();
	v42_session(0);
	v42_session(1);
	v42_session(2);
	v42_v14_mode();
	v42_states();
	v8_tests();
	return 0;
}
