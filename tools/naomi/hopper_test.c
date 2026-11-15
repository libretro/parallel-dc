/* The hopper boards on the SCIF (core/hw/naomi/hopper.c), by themselves:
 * the SEGA 837-14438 (Kick'4'Cash) and the NAOMI SWP board each sent every
 * message it knows, in each currency, and some it does not or that are
 * not messages; the coin switch; the board's file saved and found again;
 * and its part of a save state, written, read back and from before V24.
 * What the board answers and keeps is printed, for hopper_expected.txt
 * (sh tools/naomi/hopper_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/serial_port.h"
#include "hw/naomi/hopper.h"
#include "hw/naomi/naomi_periph.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/sh4/modules/modules.h"
#include "hw/maple/maple_devs.h"

/* (no log) */
retro_log_printf_t log_cb;
void os_DebugBreak(void) { printf("FAIL: os_DebugBreak\n"); exit(1); }

char naomi_game_id[128];

/* The board's file: what was saved last, if anything */
static u8 file[256];
static size_t file_size;
size_t periph_load_file(const char *suffix, void *buf, size_t size)
{
	printf("load %s: %u bytes\n", suffix, (unsigned)file_size);
	memset(buf, 0, size);
	if (file_size == 0)
		return 0;
	memcpy(buf, file, file_size < size ? file_size : size);
	return file_size;
}

static u32 hash(const u8 *p, size_t n)
{
	u32 h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

void periph_save_file(const char *suffix, const void *buf, size_t size, retro_atomic_int_t *failed)
{
	(void)failed;
	printf("save %s: %u bytes %08x\n", suffix, (unsigned)size, hash((const u8 *)buf, size));
	memcpy(file, buf, size < sizeof(file) ? size : sizeof(file));
	file_size = size;
}

/* The coin switch (active low) */
static u32 kcode = 0xffffffff;
u32 mcfg_ArcadeKcode(int port) { (void)port; return kcode; }

/* The port: the pipe plugged in, and what it was asked to look at */
static serial_pipe *pipe_in;
static int updates;
void scif_set_pipe(serial_pipe *pipe) { pipe_in = pipe; printf("pipe %s\n", pipe ? "set" : "unset"); }
void scif_update_status(void) { updates++; }

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

/* What the board has for the game: its length and hash, and its first bytes */
static void drain(const char *what)
{
	static u8 out[8192];
	int n = 0, i;
	while (pipe_in != NULL && pipe_in->ops->available(pipe_in) > 0 && n < (int)sizeof(out))
		out[n++] = pipe_in->ops->read(pipe_in);
	printf("%s: %d bytes %08x (%d updates):", what, n, hash(out, n), updates);
	for (i = 0; i < n && i < 40; i++)
		printf(" %02x", out[i]);
	printf("\n");
	updates = 0;
}

/* A message: 'H', the command, its length, @len bytes of payload (@p, or
 * counting up from @seed), a checksum */
static void message(u8 command, const u8 *p, u32 len, u8 seed)
{
	const u32 total = len + 5;
	u8 sum = 'H' + command + (u8)total + (u8)(total >> 8);
	u32 i;
	pipe_in->ops->write(pipe_in, 'H');
	pipe_in->ops->write(pipe_in, command);
	pipe_in->ops->write(pipe_in, (u8)total);
	pipe_in->ops->write(pipe_in, (u8)(total >> 8));
	for (i = 0; i < len; i++)
	{
		u8 b = p != NULL ? p[i] : (u8)(seed + i * 7);
		pipe_in->ops->write(pipe_in, b);
		sum += b;
	}
	pipe_in->ops->write(pipe_in, sum);
}

/* A message whose payload is words: @w, @n of them, from its fifth byte
 * (the first being where the message starts) */
static void words_message(u8 command, const u32 *w, u32 n)
{
	u8 p[64];
	memset(p, 0, sizeof(p));
	memcpy(p, w, n * 4);
	message(command, p, n * 4, 0);
}

static void coin(const char *what)
{
	kcode &= ~NAOMI_COIN_KEY;
	hopper_poll();
	hopper_poll();
	kcode |= NAOMI_COIN_KEY;
	hopper_poll();
	drain(what);
}

static void state(const char *what)
{
	static u8 buf[8192];
	void *p = buf;
	unsigned int total = 0;
	hopper_serialize_v24(&p, &total);
	printf("%s: state %u bytes %08x\n", what, total, hash(buf, total));
	p = buf;
	total = 0;
	printf("%s: read back %d\n", what, (int)hopper_unserialize_v24(&p, &total));
	drain(what);
}

/* A state whose message coming in cannot be (10 bytes of one of 8): none
 * is, and the next is read from its start */
static void bad_state(u8 command)
{
	static u8 buf[8192];
	void *p = buf;
	unsigned int total = 0;
	const u16 len = 10, expected = 8;
	hopper_serialize_v24(&p, &total);
	memcpy(buf + 23 * 4 + 256, &len, 2);
	memcpy(buf + 23 * 4 + 256 + 2, &expected, 2);
	p = buf;
	total = 0;
	printf("bad state: read back %d\n", (int)hopper_unserialize_v24(&p, &total));
	message(command, NULL, 4, 3);
	drain("bad state, then a message");
}

/* The configuration message, its settings @seed onwards: 0x2c bytes from
 * its third (837-14438) or 0x28 (SWP) */
static void config(u8 command, u32 len)
{
	u8 p[0x40];
	u32 i;
	for (i = 0; i < sizeof(p); i++)
		p[i] = (u8)(i * 13 + 1);
	message(command, p, len - 5, 0);
}

static void board(const char *id, int sega)
{
	static const u8 currencies[] = { 0, 1, 2, 3, 4, 5, 0xff };
	char what[64];
	u32 i;
	/* the commands, the board's own numbering */
	const u8 power = sega ? 0x41 : 0x20, version = 0x40;
	const u8 status = sega ? 0x42 : 0x21, start = sega ? 0x43 : 0x22, end = sega ? 0x44 : 0x23;
	const u8 test = sega ? 0x45 : 0x24, cfg = sega ? 0x4b : 0x2a;
	u8 p[8];

	strcpy(naomi_game_id, id);
	file_size = 0;
	hopper_init();
	printf("== %s: active %d\n", id, (int)hopper_active());

	/* the coin switch, before the game has started the board: nothing */
	coin("coin before start");

	if (sega)
	{
		message(version, NULL, 3, 1);
		drain("version");
	}
	for (i = 0; i < sizeof(currencies); i++)
	{
		memset(p, 0, sizeof(p));
		p[4] = currencies[i];	/* the message's ninth byte */
		message(power, p, sizeof(p), 0);
		sprintf(what, "power on, currency %d", currencies[i]);
		drain(what);
	}
	message(status, NULL, 4, 3);
	drain("status");
	{
		static const u32 bets[][3] = { { 100, 0, 0 }, { 300, 50, 10 }, { 0, 5000, 7 } };
		for (i = 0; i < 3; i++)
		{
			words_message(start, bets[i], 3);
			sprintf(what, "game start %u", i);
			drain(what);
			coin("coin");
		}
	}
	{
		const u32 pay[2] = { 0, 250 };
		words_message(end, pay, 2);
		drain("game end");
	}
	message(test, NULL, 6, 9);
	drain("test");
	config(cfg, sega ? 0x2c : 0x28);
	drain("configuration");
	/* (pay out on its own, now: pounds) */
	memset(p, 0, sizeof(p));
	p[4] = 1;
	message(power, p, sizeof(p), 0);
	drain("power on, pounds");
	{
		const u32 pay[2] = { 0, 500 };
		words_message(end, pay, 2);
		drain("game end, paid out");
	}
	if (!sega)
	{
		message(0x25, NULL, 2, 4);
		drain("backup clear");
		{
			const u32 b = 777;
			words_message(0x26, &b, 1);
			drain("bet");
		}
		message(0x29, NULL, 4, 1);
		message(0x2f, NULL, 4, 1);
		message(0x31, NULL, 4, 1);
		drain("switch, test data, coin ack");
	}
	else
	{
		message(0x4a, NULL, 4, 1);
		message(0x61, NULL, 4, 1);
		message(0x62, NULL, 4, 1);
		drain("switch, coin acks");
	}
	/* not messages, and one it does not know */
	pipe_in->ops->write(pipe_in, 'x');
	pipe_in->ops->write(pipe_in, 'H');
	pipe_in->ops->write(pipe_in, 0x20);
	pipe_in->ops->write(pipe_in, 3);
	pipe_in->ops->write(pipe_in, 0);
	pipe_in->ops->write(pipe_in, 'H');
	pipe_in->ops->write(pipe_in, 0x20);
	pipe_in->ops->write(pipe_in, 0x10);
	pipe_in->ops->write(pipe_in, 0x01);
	message(0x99, NULL, 2, 0);
	drain("not messages, unknown");

	/* a message half way, a reply not read, in a state */
	message(status, NULL, 4, 3);
	pipe_in->ops->write(pipe_in, 'H');
	pipe_in->ops->write(pipe_in, status);
	pipe_in->ops->write(pipe_in, 9);
	state("state");
	pipe_in->ops->write(pipe_in, 0);
	for (i = 0; i < 4; i++)
		pipe_in->ops->write(pipe_in, (u8)i);
	drain("the message finished");
	bad_state(status);
	/* a long reply not read, round the end of the ring (where the ring
	 * starts is where the bytes so far have brought it: the power on's
	 * after those brings it past the end) */
	for (i = 0; i < 40; i++)
		message(status, NULL, 4, 3);
	drain("statuses");
	memset(p, 0, sizeof(p));
	message(power, p, sizeof(p), 0);
	state("state, round the ring");
	message(status, NULL, 4, 3);
	hopper_state_before_v24();
	drain("state before V24");
	coin("coin after state before V24");

	/* again: its file found */
	hopper_init();
	message(status, NULL, 4, 3);
	drain("status, from its file");
	hopper_term();
	printf("active %d\n", (int)hopper_active());
}

int main(void)
{
	board("KICK '4' CASH", 1);
	board("CLUB KART PRIZE", 0);
	return 0;
}
