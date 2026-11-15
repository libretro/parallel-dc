/* What is plugged into the System SP's serial ports
 * (core/hw/naomi/systemsp_serial.c), by itself: the RFID chip
 * reader/writers of Dinosaur King and Love and Berry, each command the
 * games send, the card's counters run down until a new card is made, in
 * each region; and the touchscreen of Isshoni Wanwan Waiwai Puppy, each
 * command, where it is touched and its timer; and their parts of a save
 * state, read back and not right. What they answer and keep is printed,
 * for sp_serial_expected.txt (sh tools/naomi/sp_serial_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include <streams/file_stream.h>
#include "hw/naomi/systemsp_serial.h"
#include "hw/sh4/sh4_sched.h"
#include "savewriter.h"

/* (no log) */
retro_log_printf_t log_cb;
void os_DebugBreak(void) { printf("FAIL: os_DebugBreak\n"); exit(1); }

static u32 hash(const u8 *p, size_t n)
{
	u32 h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

/* The pad and the light gun */
u32 kcode[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
f32 mo_x_abs[4], mo_y_abs[4];

/* The scheduler */
sched_list sch_list[SH4_SCHED_SLOTS];
static u32 now = 1000;
static int registered;
u32 sh4_sched_now(void) { return now; }
int sh4_sched_register(int tag, sh4_sched_callback *ssc)
{
	sch_list[registered].cb = ssc;
	sch_list[registered].tag = tag;
	sch_list[registered].start = -1;
	sch_list[registered].end = -1;
	printf("sched register %d\n", registered);
	return registered++;
}
void sh4_sched_request(int id, int cycles)
{
	printf("sched request %d %d\n", id, cycles);
	if (cycles == -1)
	{
		sch_list[id].start = -1;
		sch_list[id].end = -1;
	}
	else
	{
		sch_list[id].start = (int)now;
		sch_list[id].end = (int)(now + cycles);
	}
}

/* The cards' files: "card1" there, "card2" not */
static u8 card1[128];
RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	(void)mode;
	(void)hints;
	return !strcmp(path, "card1") ? (RFILE *)card1 : NULL;
}
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
	memcpy(data, stream, (size_t)len);
	return len;
}
int filestream_close(RFILE *stream) { (void)stream; return 0; }

/* (what is saved, all of it, hashed together: printed with what is sent) */
static u32 saves, saved = 2166136261u;
int save_writer_put_file(const char *path, const void *data, size_t len, retro_atomic_int_t *failed)
{
	(void)failed;
	saved = (saved ^ hash((const u8 *)path, strlen(path))) * 16777619u;
	saved = (saved ^ hash((const u8 *)data, len)) * 16777619u;
	saves++;
	return 1;
}
void save_writer_drain(void) { printf("drain\n"); }

bool ra_serialize(const void *src, unsigned int size, void **dest, unsigned int *total)
{
	if (*dest) { memcpy(*dest, src, size); *(u8 **)dest += size; }
	*total += size;
	return true;
}
bool ra_unserialize(void *dst, unsigned int size, void **src, unsigned int *total)
{
	memcpy(dst, *src, size); *(u8 **)src += size; *total += size;
	return true;
}

static int updates;
static void update_status(void *port) { (void)port; updates++; }

static sp_pipe *pipe_in;

static void drain(const char *what)
{
	static u8 out[2048];
	int n = 0, i;
	while (pipe_in->ops->available(pipe_in) > 0 && n < (int)sizeof(out))
		out[n++] = pipe_in->ops->read(pipe_in);
	printf("%s: %d bytes %08x (%d updates, %u saved %08x):", what, n, hash(out, n), updates, saves, saved);
	for (i = 0; i < n && i < 24; i++)
		printf(" %02x", out[i]);
	printf("\n");
	updates = 0;
}

static void send(const u8 *p, u32 n, const char *what)
{
	u32 i;
	for (i = 0; i < n; i++)
		pipe_in->ops->write(pipe_in, p[i]);
	if (what != NULL)
		drain(what);
}

/* The device's state; @patch, if not NULL, done to it before it is read
 * back (@at, the offset from its end when negative) */
static void state(const char *what, int at, const u8 *patch, u32 n)
{
	static u8 buf[SP_PIPE_STATE_SIZE];
	void *p = buf;
	unsigned int total = 0;
	pipe_in->ops->serialize(pipe_in, &p, &total);
	/* (not what was made from the clock, to be made known) */
	if (at != -132)
		printf("%s: state %u bytes %08x\n", what, total, hash(buf, total));
	if (patch != NULL)
		memcpy(buf + (at < 0 ? (int)total + at : at), patch, n);
	p = buf;
	total = 0;
	pipe_in->ops->unserialize(pipe_in, &p, &total);
	p = buf;
	total = 0;
	pipe_in->ops->serialize(pipe_in, &p, &total);
	printf("%s: read %08x\n", what, hash(buf, total));
	drain(what);
}

static void rfid(const char *game, int region, int index)
{
	static const u8 test_on[] = { 0x5e, 0, 0 };
	static const u8 test_off[] = { 0x5e, 0x80, 0 };
	static const u8 sel0[] = { 0x2e, 1, 2, 3, 4, 0, 0, 0, 0 };
	static const u8 sel1[] = { 0x2e, 1, 2, 3, 4, 5, 6, 7, 8 };
	static const u8 count[4][2] = { { 0x6e, 0xc0 }, { 0x6e, 0x30 }, { 0x6e, 0x0c }, { 0x6e, 0x03 } };
	static const u8 row_st[] = { 0xee, 1, 2, 3, 4 };
	static const u8 row[] = { 0xce, 9, 8, 7, 6 };
	static const u8 row_nop[] = { 0x8e, 5, 5, 5, 5 };
	static const u8 halt[] = { 0xae, 1, 2, 3, 4 };
	static const u8 change[] = { 0x3e, 1 };
	static const u8 one[] = { 0 };
	char what[64];
	int i;

	printf("== %s, region %d, port %d\n", game, region, index);
	pipe_in = sp_rfid_open(index, game, region, index == 1 ? "card1" : "card2", update_status, NULL);
	{
		/* (a card made new: the template's own bytes) */
		static u8 buf[SP_PIPE_STATE_SIZE];
		void *p = buf;
		unsigned int total = 0;
		pipe_in->ops->serialize(pipe_in, &p, &total);
		if (index == 2)
			printf("new card: mode %02x %02x %02x %02x counters %02x %02x %02x %02x\n",
				buf[total - 4 - 128 + 12], buf[total - 4 - 128 + 13], buf[total - 4 - 128 + 14], buf[total - 4 - 128 + 15],
				buf[total - 4 - 128 + 16], buf[total - 4 - 128 + 17], buf[total - 4 - 128 + 18], buf[total - 4 - 128 + 19]);
		/* the rest from a known card and start */
		{
			u8 known[132];
			u32 seed = 12345u + (u32)region + (u32)index * 100;
			memcpy(known, card1, 128);
			known[0] = (u8)index;
			memcpy(known + 128, &seed, 4);
			state("known", -132, known, sizeof(known));
		}
	}
	send((const u8 *)"\x7e", 1, "reset");
	send(one, 1, "unknown");
	send((const u8 *)"\x4e", 1, "request, off");
	send(test_on, sizeof(test_on), "test");
	send((const u8 *)"\x4e", 1, "request");
	send((const u8 *)"\x4e", 1, "request again");
	send(sel0, sizeof(sel0), "select");
	send((const u8 *)"\x0e", 1, "read");
	send(sel1, sizeof(sel1), "select with the key");
	send((const u8 *)"\x0e", 1, "read all");
	send(test_on, sizeof(test_on), "test, on");
	send(change, sizeof(change), "change");
	send(row_st, sizeof(row_st), "write, first row");
	for (i = 0; i < 34; i++)
		send(row, sizeof(row), i == 33 ? "write, past the last row" : NULL);
	drain("rows written");
	send(row_nop, sizeof(row_nop), "write nothing");
	send((const u8 *)"\x0e", 1, "read all");
	/* the counters: the last run down to 0, a new card */
	for (i = 0; i < 3; i++)
	{
		sprintf(what, "count %d", i);
		send(count[i], 2, what);
	}
	for (i = 0; i < 0x24; i++)
		send(count[3], 2, i == 0x22 ? "count, the last but one" : i == 0x23 ? "count, the last" : NULL);
	drain("counted");
	send((const u8 *)"\x0e", 1, "read the new card");
	send(halt, sizeof(halt), "halt");
	for (i = 0; i < 0x25; i++)
		send(count[3], 2, NULL);
	drain("counted down again");
	/* a command on its way, in a state */
	send(halt, 2, NULL);
	state("state", 0, NULL, 0);
	send(halt + 2, 3, "the command finished");
	{
		/* not right: what is expected and what has come past the buffer,
		 * a state beyond the last */
		static const u8 bad[] = { 200, 200 };
		static const u8 bad_state[] = { 9 };
		send(halt, 2, NULL);
		state("state not right", 2 + 2 + 512, bad, 2);
		state("state not right", 2 + 2 + 512 + 2 + 10, bad_state, 1);
		send(halt, sizeof(halt), "after it");
	}
	{
		static const u16 bad_queue[2] = { 600, 600 };
		state("queue not right", 0, (const u8 *)bad_queue, 4);
	}
	send(test_off, sizeof(test_off), "test, off");
	send((const u8 *)"\x4e", 1, "request, off");
	pipe_in->ops->reset(pipe_in);
	drain("power on");
	pipe_in->ops->close(pipe_in);
}

static void touch(void)
{
	static const u8 name[] = "\001OI\r";
	static const u8 verify[] = "\001NM\r";
	static const u8 rst[] = "\001UV\r";
	static const u8 run[] = "\001R\r";
	static const u8 z[] = "\001Z\r";
	static const u8 other[] = "\001X\r";
	static const u8 one[] = "\001\r";
	static const u8 nothing[] = "\r";
	static const u8 pos[] = "Ua0000000?";
	static const u8 bad[] = "Ub0000000?";
	sched_list *ev;
	int id;

	printf("== touchscreen\n");
	pipe_in = sp_touchscreen_open(update_status, NULL);
	id = registered - 1;
	ev = &sch_list[id];
	send(name, 4, "name");
	send(verify, 4, "verify");
	send(rst, 4, "reset");
	send(other, 3, "other");
	send(one, 2, "one byte");
	send(nothing, 1, "nothing");
	mo_x_abs[0] = 100;
	mo_y_abs[0] = 200;
	send(z, 3, "where");
	send(z, 3, "where, the same");
	kcode[0] &= ~(1u << 9);
	send(z, 3, "where, touched");
	mo_x_abs[0] = -50;
	mo_y_abs[0] = 900;
	send(pos, 10, "position");
	send(bad, 10, "not a position");
	send(run, 3, "run");
	mo_x_abs[0] = 640;
	mo_y_abs[0] = 0;
	printf("timer: %d\n", ev->cb(ev->tag, 0, 0));
	drain("timer");
	printf("timer: %d\n", ev->cb(ev->tag, 0, 0));
	drain("timer, the same");
	send(name, 2, NULL);
	state("state", 0, NULL, 0);
	send(name + 2, 2, "the command finished");
	{
		/* a timer far off, and a command longer than any */
		s32 far[2];
		static const u8 len[] = { 200 };
		far[0] = (s32)now;
		far[1] = (s32)(now + 100000000);
		state("timer far off", -8, (const u8 *)far, 8);
		printf("timer at %d %d\n", ev->start, ev->end);
		state("command not right", 2 + 2 + 512, len, 1);
		send(name, 4, "after it");
	}
	pipe_in->ops->reset(pipe_in);
	drain("power on");
	pipe_in->ops->close(pipe_in);
	printf("timer after it: %d\n", ev->cb(ev->tag, 0, 0));
}

int main(void)
{
	static const char *const games[] = { "dinoking", "loveber" };
	int g, region;
	u32 i;

	for (i = 0; i < sizeof(card1); i++)
		card1[i] = (u8)(i * 7 + 3);
	card1[16] = card1[17] = card1[18] = 5;
	card1[19] = 0x23;
	for (g = 0; g < 2; g++)
		for (region = 0; region < 3; region++)
		{
			rfid(games[g], region, 1);
			rfid(games[g], region, 2);
		}
	touch();
	return 0;
}
