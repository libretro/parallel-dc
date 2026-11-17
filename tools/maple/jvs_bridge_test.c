/* The NAOMI's I/O board bridge (core/hw/maple/jvs_bridge.c), with its
 * boards (jvs_board.c), on maple frames made at random: every command and
 * JVS sub-command, cut short and too long, requests stored and sent on,
 * answers waiting on the channels, the EEPROM read from its file, set up
 * for the cabinet, written and read, the program for the bridge's
 * processor, the RS-422 port, and the bridge's part of a state, written
 * and read back changed. Every answer and everything asked of the rest of
 * the machine is said into a hash printed every so many cabinets; what it
 * prints must be what it always was (jvs_bridge_expected.txt;
 * jvs_bridge_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <streams/file_stream.h>
#include "types.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_state.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/serial_port.h"
#include "log/Log.h"
#include "libretro/pad_state.h"
#ifdef JVS_BRIDGE_TEST_OLD
/* (the class this was, through a shim) */
void old_create(void);
u32 old_dma(const u32 *in, u32 len, u32 *out);
bool old_serialize(void **data, unsigned int *total_size);
bool old_unserialize(void **data, unsigned int *total_size);
void old_set_device(bool there);
bool *old_dirty(void);
void naomi_mie_set_pipe(serial_pipe *pipe);
void naomi_jvs_serialize_v23(void **data, unsigned int *total_size);
bool naomi_jvs_unserialize_v23(void **data, unsigned int *total_size);
void maple_jvs_firmware_reset(void);
void maple_naomi_eeprom_load(void);
void maple_jvs_serialize_v27(void **data, unsigned int *total_size);
#else
#include "hw/maple/jvs_bridge.h"
#include "hw/naomi/naomi_periph.h"
#include "hw/naomi/naomi.h"
#endif

/* ---- The machine around the bridge ---- */

settings_t settings;
retro_log_printf_t log_cb;
char naomi_game_id[64];
InputDescriptors *naomi_game_inputs;
u8 *naomi_default_eeprom;
char eeprom_file[64] = "eeprom";
u32 kcode[4];
u8 rt[4];
u8 lt[4];
s8 joyx[4], joyy[4];
s8 joyrx[4], joyry[4];
f32 mo_x_abs[4];
f32 mo_y_abs[4];
f32 mo_x_delta[4];
f32 mo_y_delta[4];
#ifndef JVS_BRIDGE_TEST_OLD
bool enable_naomi_15khz_dipswitch;
bool naomi_bios_multiregion;
#endif

static u32 hash = 2166136261u;

void mix(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		hash ^= (v >> (i * 8)) & 0xff;
		hash *= 16777619u;
	}
}

static u32 seed = 0x1b873593u;

static u32 rnd(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

static u32 rn(u32 n)
{
	return rnd() % n;
}

bool printer_present(void)
{
	return false;
}

void printer_print(char c)
{
	mix((u8)c);
}

void naomi_ffb_rumble(int kind, float level, u32 duration_ms)
{
	(void)level;
	mix(0x302);
	mix((u32)kind);
	mix(duration_ms);
}

void mcfg_get_input(const mcfg_map_t *map, PlainJoystickState *pjs)
{
	(void)map;
	(void)pjs;
	mix(0x303);
}

void naomi_periph_poll(void)
{
	mix(0x304);
}

/* The EEPROM's file, there or not */
static bool file_there;
static u8 file_bytes[0x80];

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	(void)mode;
	(void)hints;
	mix(0x305);
	mix((u32)strlen(path));
	return file_there ? (RFILE *)file_bytes : NULL;
}

int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
	memcpy(data, stream, (size_t)len);
	return len;
}

int filestream_close(RFILE *stream)
{
	(void)stream;
	return 0;
}

/* The game's header: its ID, coin settings and cabinets */
static bool header_there;
static u8 header_bytes[0x500];

bool naomi_cart_GetBootHeader(u8 *header, u32 size)
{
	mix(0x306);
	if (!header_there)
		return false;
	memcpy(header, header_bytes, size);
	return true;
}

/* The RS-422 port's device: bytes waiting, and those written said */
static int pipe_waiting;

static void pipe_write(serial_pipe *pipe, u8 data)
{
	(void)pipe;
	mix(0x307);
	mix(data);
}

static int pipe_available(serial_pipe *pipe)
{
	(void)pipe;
	return pipe_waiting;
}

static u8 pipe_read(serial_pipe *pipe)
{
	(void)pipe;
	pipe_waiting--;
	return (u8)(pipe_waiting * 37 + 5);
}

static const serial_pipe_ops pipe_ops = { pipe_write, pipe_available, pipe_read };
static serial_pipe the_pipe = { &pipe_ops };

/* The serializer: a run of bytes */
static u8 state[0x20000];
static u8 *state_at;

bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	state_at = (u8 *)*dest;
	memcpy(state_at, src, src_size);
	state_at += src_size;
	*dest = state_at;
	*total_size += src_size;
	return true;
}

bool ra_unserialize(void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	/* (where *dest says: it is moved back by the reader too) */
	state_at = (u8 *)*dest;
	memcpy(src, state_at, src_size);
	state_at += src_size;
	*dest = state_at;
	*total_size += src_size;
	return true;
}

#ifndef JVS_BRIDGE_TEST_OLD
static jvs_bridge bridge;
static bool device_there = true;

jvs_bridge *maple_jvs_bridge(void)
{
	return device_there ? &bridge : NULL;
}

static void t_create(void)
{
	jvs_bridge_init(&bridge);
}

static u32 t_dma(const u32 *in, u32 len, u32 *out)
{
	static const mcfg_map_t map = { NULL, 0 };
	return jvs_bridge_dma(&bridge, &map, in, len, out);
}

static bool t_serialize(void **data, unsigned int *total_size)
{
	return jvs_bridge_serialize(&bridge, data, total_size);
}

static bool t_unserialize(void **data, unsigned int *total_size)
{
	return jvs_bridge_unserialize(&bridge, data, total_size);
}

static void t_set_device(bool there)
{
	device_there = there;
}

static bool *t_dirty(void)
{
	return &bridge.eeprom_dirty;
}
#else
#define t_create old_create
#define t_dma old_dma
#define t_serialize old_serialize
#define t_unserialize old_unserialize
#define t_set_device old_set_device
#define t_dirty old_dirty
#endif

/* ---- What is made at random ---- */

static const char *const games[] = {
	"SOME GAME", "VIRTUA ATHLETE", "ROYAL RUMBLE", "BEACH SPIKERS JAPAN", "MJ JAPAN", "hotd2p",
	"MUSHIKING THE KING OF BEETLE", "CRACKIN'DJ"
};

static const u32 setups[] = {
	JVS_Default, JVS_FourPlayers, JVS_RotaryEncoders, JVS_DualIOBoards4P, JVS_DualIOBoards4P,
	JVS_LightGun, JVS_Mazan, JVS_WorldKicksPCB, JVS_OutTrigger, JVS_F355, JVS_WorldKicksPCB
};

/* A request for a board */
static u32 jvs_request(u8 *p, u32 room)
{
	static const u8 simple[] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0xF0 };
	u32 len = 0;
	if (room < 8)
		return 0;
	switch (rn(5))
	{
	case 0:
		p[len++] = simple[rn(sizeof(simple))];
		break;
	case 1:
		p[len++] = 0xF1;
		p[len++] = (u8)(1 + rn(2));
		break;
	case 2:
		p[len++] = 0x20;
		p[len++] = (u8)rn(3);
		p[len++] = 2;
		p[len++] = 0x21;
		p[len++] = (u8)rn(3);
		break;
	case 3:
		p[len++] = 0x22;
		p[len++] = (u8)rn(8);
		break;
	default:
		{
			/* (now and then as long as a request can be, with one stored before it) */
			const u32 most = rn(6) == 0 ? 120 + rn(136) : 6 + rn(30);
			while (len < room && len < most)
				p[len++] = (u8)rnd();
		}
		break;
	}
	return len;
}

static u32 make_frame(u32 *frame)
{
	static const u8 cmds[] = { 0x84, 0x86, 0x86, 0x86, 0x86, 0x86, 0x80, 0x80, 0x82, 0x01, 0x02, 0x03, 0x04, 0x09, 0x0C };
	static const u8 subs[] = {
		0x13, 0x15, 0x17, 0x19, 0x21, 0x35, 0x27, 0x33, 0x0B, 0x03, 0x31, 0x01, 0x41, 0x47, 0x4d,
		0x49, 0x4b, 0x4f, 0x15, 0x19, 0x27, 0x13, 0x77
	};
	u8 *b = (u8 *)frame;
	u8 *in = b + 4;
	u32 n = 0, i;
	memset(b, 0, 1024);
	b[0] = cmds[rn(sizeof(cmds))];
	b[1] = (u8)rnd();
	b[2] = (u8)rnd();
	b[3] = (u8)rnd();
	switch (b[0])
	{
	case 0x86:
		in[0] = subs[rn(sizeof(subs))];
		switch (in[0])
		{
		case 0x13:
			in[1] = (u8)(rn(4) == 0 ? rn(40) : 1 + rn(2));
			in[2] = (u8)jvs_request(in + 3, 250);
			n = 3 + in[2];
			break;
		case 0x27:
		case 0x35:
			{
				const u32 count = rn(4);
				u32 at = 7;
				in[5] = (u8)rn(40);
				in[6] = (u8)count;
				for (i = 0; i < count; i++)
				{
					in[at] = (u8)(rn(3) == 0 ? 0xff : 1 + rn(2));
					in[at + 1] = (u8)jvs_request(in + at + 2, 100);
					at += 2 + in[at + 1];
				}
				n = at;
			}
			break;
		case 0x0B:
			in[1] = (u8)rnd();
			in[2] = (u8)rn(0x90);
			for (i = 0; i < 0x90; i++)
				in[4 + i] = (u8)rnd();
			n = 4 + rn(0x90);
			break;
		default:
			in[1] = (u8)(rn(2) ? rn(40) : rnd());
			in[4] = (u8)rnd();
			in[5] = (u8)rn(40);
			in[6] = (u8)(rn(4) == 0 ? 0xff : rn(4));
			in[7] = (u8)jvs_request(in + 8, 250);
			n = 8 + in[7];
			break;
		}
		break;
	case 0x80:
		in[0] = (u8)(rn(2) ? 0xff : rnd());
		in[1] = (u8)(rn(6) == 0 ? 0xff : rnd());
		in[2] = (u8)(rn(4) == 0 ? 0xff : rnd());
		in[3] = (u8)rnd();
		for (i = 4; i < 0x20; i++)
			in[i] = (u8)rnd();
		n = 0x20;
		break;
	default:
		n = rn(16);
		break;
	}
	/* cut short, now and then, or longer than its command */
	switch (rn(8))
	{
	case 0: n = rn(n + 1); break;
	case 1: n += rn(16); break;
	default: break;
	}
	/* (an EEPROM write shorter than its head made the bridge copy what
	 * was not there: it crashed) */
	if (b[0] == 0x86 && in[0] == 0x0B && n < 4)
		n = 4;
	return 4 + n;
}

static void make_file(void)
{
	u32 i;
	file_there = rn(3) != 0;
	for (i = 0; i < sizeof(file_bytes); i++)
		file_bytes[i] = (u8)rnd();
	header_there = rn(4) != 0;
	for (i = 0; i < sizeof(header_bytes); i++)
		header_bytes[i] = (u8)(rn(3) == 0 ? rnd() : rn(4));
	if (rn(2))
		memcpy(header_bytes + 0x134, file_bytes + 3, 4);	/* the game the file is set up for */
	else if (rn(4) == 0)
		memcpy(header_bytes + 0x134, file_bytes + 3, 3);	/* ...all but its last letter */
	header_bytes[0x429] = (u8)(rn(3) == 0 ? rnd() : rn(16));
	if (rn(2))
		header_bytes[0x1e0] = 1;
}

/* The bridge's part of a state, the program's: written, mixed, read back changed */
static void states(void)
{
	unsigned int total = 0, total2 = 0;
	void *data = state;
	u32 i, k;
	state_at = state;
	t_serialize(&data, &total);
	naomi_jvs_serialize_v23(&data, &total);
	maple_jvs_serialize_v27(&data, &total);
	mix(*t_dirty());
	if (rn(2))
		*t_dirty() = false;	/* (as a save of the EEPROM leaves it) */
	mix(total);
	for (i = 0; i < total; i++)
		mix(state[i]);
	switch (rn(4))
	{
	case 0:
		maple_jvs_firmware_reset();
		break;
	default:
		k = rn(8);
		for (i = 0; i < k; i++)
			state[rn(total)] = (u8)rnd();
		/* (the bridge's flag, a bool, as the bridge writes it: the program
		 * read another way, as half the time) */
		state[0] = (u8)rn(2);
		/* (the count of boards, a size_t: as a 32-bit build writes it, now and then) */
		if (rn(4) == 0)
		{
			const u32 at = 1 + 32 * 256 + 32 * 4 + 32 * 258;
			memmove(state + at + 4, state + at + 8, total - at - 8);
			total -= 4;
		}
		data = state;
		mix(t_unserialize(&data, &total2));
		mix(total2);
		mix(naomi_jvs_unserialize_v23(&data, &total2));
		break;
	}
}

int main(int argc, char **argv)
{
	const u32 cabinets = argc > 1 ? (u32)atoi(argv[1]) : 1500;
	static u32 in[1024 / 4 + 16], out[4096 / 4];
	u32 c, m;
	t_create();
	for (c = 0; c < cabinets; c++)
	{
		u32 frames;
		settings.mapping.JammaSetup = (enum JVS)setups[rn(sizeof(setups) / sizeof(setups[0]))];
		strcpy(naomi_game_id, games[rn(sizeof(games) / sizeof(games[0]))]);
		naomi_game_inputs = NULL;
		enable_naomi_15khz_dipswitch = rn(2);
		naomi_bios_multiregion = rn(2);
		make_file();
		naomi_default_eeprom = rn(2) ? NULL : header_bytes + 0x100;
		naomi_mie_set_pipe(rn(3) == 0 ? &the_pipe : NULL);
		t_set_device(rn(8) != 0);
		if (rn(2))
			EEPROM_loaded = false;
		if (rn(2))
			maple_naomi_eeprom_load();
		t_create();
		frames = 20 + rn(80);
		for (m = 0; m < frames; m++)
		{
			u32 len, n, i;
			for (i = 0; i < 4; i++)
				kcode[i] = rnd() | rnd();
			pipe_waiting = rn(4) == 0 ? (int)rn(300) : (int)rn(5);
			len = make_frame(in);
			memset(out, 0xcc, sizeof(out));
			n = t_dma(in, len, out);
			mix(n);
			for (i = 0; i < n; i++)
				mix(((const u8 *)out)[i]);
			mix((u32)pipe_waiting);
			if (rn(12) == 0)
				states();
			if (rn(30) == 0)
				EEPROM_loaded = false;
		}
		states();
		for (m = 0; m < 0x80; m++)
			mix((u8)EEPROM[m]);
		if (c % 150 == 149)
			printf("%u %08x\n", c + 1, hash);
	}
	printf("jvs_bridge_test: %u cabinets %08x\n", cabinets, hash);
	return 0;
}
