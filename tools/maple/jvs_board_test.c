/* The NAOMI's JVS I/O boards (core/hw/maple/jvs_board.c) on requests,
 * inputs, cabinets and games made at random: every answer, everything a
 * board asks of the rest of the machine (force feedback, the printer, the
 * pads), and the boards' parts of save states, written and read, said into
 * a hash printed every so many cabinets. What it prints must be what it
 * always was (jvs_board_expected.txt; jvs_board_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_state.h"
#include "hw/naomi/naomi_cart.h"
#include "log/Log.h"
#ifdef JVS_BOARD_TEST_OLD
/* (the classes these were, through a shim) */
void old_create(void);
void old_destroy(void);
u32 old_count(void);
u32 old_message(u32 index, const u8 *in, u32 len, u8 *out);
bool old_board_serialize(u32 index, void **data, unsigned int *total_size);
bool old_board_unserialize(u32 index, void **data, unsigned int *total_size);
extern bool t_no_device;
void maple_jvs_serialize_v21(void **data, unsigned int *total_size);
void maple_jvs_unserialize_v21(void **data, unsigned int *total_size);
void maple_jvs_state_before_v21(void);
void maple_jvs_serialize_v25(void **data, unsigned int *total_size);
bool maple_jvs_unserialize_v25(void **data, unsigned int *total_size);
void maple_jvs_state_before_v25(void);
void maple_jvs_serialize_v27(void **data, unsigned int *total_size);
bool maple_jvs_unserialize_v27(void **data, unsigned int *total_size);
void maple_jvs_state_before_v27(void);
#else
#include "hw/maple/jvs_board.h"
#endif
#include "libretro/pad_state.h"

/* ---- The machine around the boards ---- */

settings_t settings;
retro_log_printf_t log_cb;
char naomi_game_id[64];
InputDescriptors *naomi_game_inputs;
char EEPROM[0x100];
u32 kcode[4];
u8 rt[4];
u8 lt[4];
s8 joyx[4], joyy[4];
s8 joyrx[4], joyry[4];
f32 mo_x_abs[4];
f32 mo_y_abs[4];
f32 mo_x_delta[4];
f32 mo_y_delta[4];

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

static void mixf(float f)
{
	u32 u;
	memcpy(&u, &f, 4);
	mix(u);
}

static u32 seed = 0x9e3779b9u;

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

static bool printer;

bool printer_present(void)
{
	mix(0x300);
	return printer;
}

void printer_print(char c)
{
	mix(0x301);
	mix((u8)c);
}

void naomi_ffb_rumble(int kind, float level, u32 duration_ms)
{
	mix(0x302);
	mix((u32)kind);
	mixf(level);
	mix(duration_ms);
}

/* (the pads read again: the first port's buttons move, as a frontend's may) */
void mcfg_get_input(const mcfg_map_t *map, PlainJoystickState *pjs)
{
	mix(0x303);
	mix(map == NULL ? 0 : 1);
	mix(pjs->kcode);
	kcode[0] ^= hash & 0x0101;
}

/* The serializer: a run of bytes */
static u8 state[4096];
static u8 *state_at;

bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	(void)dest;
	memcpy(state_at, src, src_size);
	state_at += src_size;
	*total_size += src_size;
	return true;
}

bool ra_unserialize(void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	(void)dest;
	memcpy(src, state_at, src_size);
	state_at += src_size;
	*total_size += src_size;
	return true;
}

#ifndef JVS_BOARD_TEST_OLD
static jvs_boards boards;
static bool no_device;

jvs_boards *maple_jvs_boards(void)
{
	return no_device ? NULL : &boards;
}

static void t_create(void)
{
	memset(&boards, 0, sizeof(boards));
	jvs_boards_create(&boards);
	jvs_boards_create(&boards);	/* (made once) */
}

static u32 t_count(void)
{
	return boards.count;
}

static u32 t_message(u32 index, const u8 *in, u32 len, u8 *out)
{
	static const mcfg_map_t map = { NULL, 0 };
	if (boards.cabinet_order)
		jvs_boards_order(&boards);
	return jvs_board_message(&boards, index, &map, in, len, out);
}

static bool t_board_serialize(u32 index, void **data, unsigned int *total_size)
{
	return jvs_board_serialize(&boards.board[index], data, total_size);
}

static bool t_board_unserialize(u32 index, void **data, unsigned int *total_size)
{
	return jvs_board_unserialize(&boards.board[index], data, total_size);
}

static void t_set_no_device(bool none)
{
	no_device = none;
}
#else
static void t_create(void)
{
	old_destroy();
	old_create();
}

#define t_count old_count
#define t_message old_message
#define t_board_serialize old_board_serialize
#define t_board_unserialize old_board_unserialize

static void t_set_no_device(bool none)
{
	t_no_device = none;
}
#endif

/* ---- What is made at random ---- */

static const char *const games[] = {
	"SOME GAME", "MKG TKOB", "MUSHIKING THE KING OF BEETLE", "MUSHIUSA '04 1ST VER0.900-",
	"SHOOTOUT POOL", "KICK '4' CASH", "CRACKIN'DJ", "CRACKIN'DJ PART2", "VIRTUA ATHLETE",
	"ROYAL RUMBLE", "BEACH SPIKERS JAPAN", "MJ JAPAN", "RING OUT 4X4"
};

static InputDescriptors inputs;

static void make_inputs(void)
{
	int i, n;
	memset(&inputs, 0, sizeof(inputs));
	n = (int)rn(8);
	for (i = 0; i < n; i++)
	{
		ButtonDescriptor *b = &inputs.buttons[i];
		b->mask = 1u << rn(18);
		b->name = "b";
		switch (rn(4))
		{
		case 0: b->target = 0xffffffff; b->p2_mask = rn(2) ? 0 : 1u << rn(16); break;
		case 1: b->p2_mask = 1u << rn(16); break;
		case 2: b->target = 1u << rn(16); break;
		default: break;
		}
	}
	n = (int)rn(7);
	/* (past the last, nothing is read but its name: what else is there is no matter) */
	for (i = n; i < 8; i++)
		inputs.axes[i].type = rn(2) ? Full : Half;
	for (i = 0; i < n; i++)
	{
		AxisDescriptor *a = &inputs.axes[i];
		a->name = "a";
		a->type = rn(2) ? Full : Half;
		a->axis = rn(8);
		a->inverted = rn(2);
	}
}

static void make_pads(void)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		kcode[i] = rn(3) == 0 ? 0xffffffff : rnd() | (rnd() & rnd());
		rt[i] = (u8)rnd();
		lt[i] = (u8)rnd();
		joyx[i] = (s8)rnd();
		joyy[i] = (s8)rnd();
		joyrx[i] = (s8)rnd();
		joyry[i] = (s8)rnd();
		mo_x_abs[i] = rn(4) == 0 ? (float)((int)rn(900) - 100) : (float)rn(640 * 4) / 4.f;
		mo_y_abs[i] = rn(4) == 0 ? (float)((int)rn(700) - 100) : (float)rn(480 * 4) / 4.f;
		mo_x_delta[i] = rn(3) == 0 ? 0.f : (float)((int)rn(2001) - 1000) / 4.f;
		mo_y_delta[i] = rn(3) == 0 ? 0.f : (float)((int)rn(2001) - 1000) / 4.f;
	}
}

/* A request: one of the board's commands, or reads and writes one after another */
static u32 make_request(u8 *in)
{
	static const u8 cmds[] = { 0xF0, 0xF1, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x70, 0x74, 0x99, 0x40 };
	u32 len = 0, n, i;
	if (rn(3) == 0)
	{
		in[len++] = cmds[rn(sizeof(cmds))];
		if (in[0] == 0xF1)
			in[len++] = (u8)(1 + rn(3));
		if (in[0] == 0x70)
		{
			in[len++] = (u8)rnd();
			in[len++] = (u8)(rn(2) ? 3 : rnd());
			in[len++] = (u8)rnd();
			in[len++] = (u8)(rn(2) ? 0x10 + rn(2) : rnd());
		}
		if (in[0] == 0x74)
		{
			in[len++] = (u8)rn(8);
			for (i = 0; i < 6; i++)
				in[len++] = (u8)rnd();
		}
		if (rn(6) == 0)
			len = 0;
		return len;
	}
	n = 1 + rn(8);
	for (i = 0; i < n && len < 240; i++)
	{
		u32 k;
		switch (rn(12))
		{
		case 0: in[len++] = 0x20; in[len++] = (u8)(rn(4) == 0 ? rnd() : rn(5)); in[len++] = (u8)rn(3); break;
		case 1: in[len++] = 0x21; in[len++] = (u8)(rn(4) == 0 ? rnd() : rn(6)); break;
		case 2: in[len++] = 0x22; in[len++] = (u8)(rn(4) == 0 ? rnd() : rn(20)); break;
		case 3: in[len++] = 0x23; in[len++] = (u8)(rn(4) == 0 ? rnd() : rn(4) == 0 ? 2 : rn(10)); break;
		case 4: in[len++] = 0x25; in[len++] = (u8)rn(6); break;
		case 5:
			in[len++] = 0x32;
			k = rn(4);
			in[len++] = (u8)k;
			while (k-- > 0 && len < 250)
				in[len++] = (u8)(rn(2) ? rnd() : 0xff);
			break;
		case 6: in[len++] = 0x33; in[len++] = (u8)rn(3); in[len++] = (u8)rnd(); in[len++] = (u8)rnd(); break;
		case 7: in[len++] = 0x30; in[len++] = (u8)rn(6); in[len++] = (u8)rn(2); in[len++] = (u8)rnd(); break;
		case 8:
			in[len++] = 0x74;
			k = rn(6);
			in[len++] = (u8)(rn(8) == 0 ? 200 : k);
			while (k-- > 0 && len < 250)
				in[len++] = (u8)rnd();
			break;
		case 9: in[len++] = (u8)rnd(); break;
		default:
			/* a drive board's command: outputs 15 to 22 */
			in[len++] = 0x32;
			in[len++] = 3;
			in[len++] = (u8)rnd();
			k = (u8)~(rn(2) ? rnd() : (u32)"\x00\x01\x02\xf0\xfe\xff\xf1\xfa\xfb\xfc\xfd\x44\x4a\x85\xe3\xea\xd5"[rn(17)]);
			/* the byte, the other way round, on the last 8 bits */
			{
				u8 r = (u8)k;
				r = (u8)((r & 0xF0) >> 4 | (r & 0x0F) << 4);
				r = (u8)((r & 0xCC) >> 2 | (r & 0x33) << 2);
				r = (u8)((r & 0xAA) >> 1 | (r & 0x55) << 1);
				in[len++] = (u8)(r >> 6);
				in[len++] = (u8)(r << 2);
			}
			break;
		}
	}
	if (rn(4) == 0)
		in[0] = (u8)(0x20 + rn(0x19));
	if (len > 255)
		len = 255;
	return len;
}

/* The boards' parts of a state: written, mixed, and read back changed */
static void states(void)
{
	unsigned int total = 0;
	void *data = NULL;
	u32 i, n;
	memset(state, 0, sizeof(state));
	state_at = state;
	maple_jvs_serialize_v21(&data, &total);
	maple_jvs_serialize_v25(&data, &total);
	maple_jvs_serialize_v27(&data, &total);
	for (i = 0; i < t_count(); i++)
		t_board_serialize(i, &data, &total);
	mix(total);
	for (i = 0; i < total; i++)
		mix(state[i]);
	switch (rn(5))
	{
	case 0:
		maple_jvs_state_before_v21();
		maple_jvs_state_before_v25();
		maple_jvs_state_before_v27();
		break;
	case 1:
		maple_jvs_state_before_v25();
		maple_jvs_state_before_v27();
		break;
	default:
		/* bytes changed: counts, flags, encoders not where one can be */
		n = rn(6);
		for (i = 0; i < n; i++)
			state[rn(total)] = (u8)rnd();
		if (rn(4) == 0)
			memcpy(state + 2 * 22 + 4 * rn(8), "\x00\x00\xc0\x7f", 4);	/* a NaN */
		/* (a board's light gun flag, a bool, as the boards write it: what
		 * any other byte was read as was the compiler's to say) */
		for (i = 0; i < t_count(); i++)
			state[2 * 22 + 32 + 2 + 12 + 2 * i + 1] &= 1;
		state_at = state;
		total = 0;
		maple_jvs_unserialize_v21(&data, &total);
		mix(maple_jvs_unserialize_v25(&data, &total));
		mix(maple_jvs_unserialize_v27(&data, &total));
		for (i = 0; i < t_count(); i++)
			mix(t_board_unserialize(i, &data, &total));
		mix(total);
		break;
	}
}

static const u32 setups[] = {
	JVS_Default, JVS_FourPlayers, JVS_RotaryEncoders, JVS_SegaMarineFishing, JVS_DualIOBoards4P,
	JVS_Analog, JVS_LightGun, JVS_Mazan, JVS_GunSurvivor, JVS_DogWalking, JVS_TouchDeUno,
	JVS_WorldKicks, JVS_WorldKicksPCB, JVS_Keyboard, JVS_OutTrigger, JVS_LightGunAsAnalog,
	JVS_WaveRunnerGP, JVS_F355, JVS_TokyoBus, JVS_EighteenWheeler, JVS_SambaDeAmigo
};

int main(int argc, char **argv)
{
	const u32 cabinets = argc > 1 ? (u32)atoi(argv[1]) : 3000;
	u32 c, m;
	u8 in[256 + 4], out[256];
	for (c = 0; c < cabinets; c++)
	{
		u32 messages;
		settings.mapping.JammaSetup = (enum JVS)setups[rn(sizeof(setups) / sizeof(setups[0]))];
		strcpy(naomi_game_id, games[rn(sizeof(games) / sizeof(games[0]))]);
		/* (the games with boards of their own, more often) */
		if (rn(3) == 0)
		{
			static const char *const own[] = {
				"MUSHIKING THE KING OF BEETLE", "SHOOTOUT POOL", "KICK '4' CASH", "CRACKIN'DJ", "VIRTUA ATHLETE"
			};
			const u32 k = rn(5);
			settings.mapping.JammaSetup = k == 0 ? JVS_Default : k == 4 ? JVS_DualIOBoards4P : JVS_RotaryEncoders;
			strcpy(naomi_game_id, own[k]);
		}
		make_inputs();
		naomi_game_inputs = rn(3) == 0 ? NULL : &inputs;
		printer = rn(3) == 0;
		EEPROM[8] = (char)rnd();
		t_set_no_device(rn(10) == 0);
		t_create();
		mix(t_count());
		messages = 10 + rn(60);
		for (m = 0; m < messages; m++)
		{
			u32 len, n, i, index;
			if (m % 7 == 0)
				make_pads();
			if (rn(20) == 0)
				EEPROM[8] = (char)rnd();
			memset(in, 0, sizeof(in));
			len = make_request(in);
			index = rn(t_count());
			memset(out, 0xcc, sizeof(out));
			n = t_message(index, in, len, out);
			mix(index);
			mix(n);
			for (i = 0; i < n; i++)
				mix(out[i]);
			for (i = 0; i < 4; i++)
			{
				mixf(mo_x_delta[i]);
				mixf(mo_y_delta[i]);
				mix(kcode[i]);
			}
			if (rn(15) == 0)
				states();
		}
		states();
		if (c % 250 == 249)
			printf("%u %08x\n", c + 1, hash);
	}
	printf("jvs_board_test: %u cabinets %08x\n", cabinets, hash);
	return 0;
}
