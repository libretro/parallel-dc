/* The NAOMI's JVS I/O boards: see jvs_board.h */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <retro_miscellaneous.h>
#include "jvs_board.h"
#include "maple_devs.h"
#include "maple_cfg.h"
#include "maple_if.h"
#include "maple_state.h"
#include "libretro/pad_state.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/printer.h"
#include "hw/naomi/midiffb.h"
#include "log/Log.h"

#define LOGJVS(...) DEBUG_LOG(JVS, __VA_ARGS__)

/* ---- What the kinds of board differ in ---- */

static bool jvs_racing(const jvs_board *b)
{
	return b->kind == JVS_BOARD_837_13844_RACING || b->kind == JVS_BOARD_837_13844_18WHEELER;
}

static bool jvs_drive_board(const jvs_board *b)
{
	return b->kind == JVS_BOARD_837_13844_WRUNGP || jvs_racing(b);
}

static void jvs_board_make(jvs_board *b, u8 kind, u8 node_id, u8 first_player)
{
	memset(b, 0, sizeof(*b));
	b->kind = kind;
	b->node_id = node_id;
	b->first_player = first_player;
	b->damper_high = 8;
	b->power = 100;
	switch (kind)
	{
	case JVS_BOARD_837_13551:
	case JVS_BOARD_837_13551_NOANALOG:
	case JVS_BOARD_837_13551_MUSHIKING:
		b->player_count = 2;
		b->digital_in_count = 13;
		b->coin_input_count = 2;
		b->analog_count = kind == JVS_BOARD_837_13551_NOANALOG ? 0 : 8;
		b->output_count = 6;
		break;
	case JVS_BOARD_837_13551_4P:
		b->player_count = 4;
		b->digital_in_count = 12;
		b->coin_input_count = 4;
		b->output_count = 6;
		break;
	case JVS_BOARD_837_13938:
	case JVS_BOARD_837_13938_SHOOTOUT:
	case JVS_BOARD_837_13938_KICK4CASH:
	case JVS_BOARD_837_13938_CRACKINDJ:
		b->player_count = 1;
		b->digital_in_count = 9;
		b->encoder_count = 4;
		b->output_count = 8;
		break;
	case JVS_BOARD_NAMCO_JYU:
		b->player_count = 2;
		b->coin_input_count = 2;
		b->digital_in_count = 12;
		b->output_count = 16;
		b->light_gun_count = 2;
		break;
	case JVS_BOARD_NAMCO_FCB:
	case JVS_BOARD_NAMCO_FCA:
		b->player_count = 1;
		b->coin_input_count = kind == JVS_BOARD_NAMCO_FCB ? 2 : 1;	/* (2 makes Gun Survivor's BIOS crash) */
		b->digital_in_count = 16;
		b->output_count = 6;
		b->light_gun_count = kind == JVS_BOARD_NAMCO_FCB ? 1 : 0;
		b->analog_count = 7;
		b->encoder_count = 2;
		break;
	case JVS_BOARD_NAMCO_V226:
	case JVS_BOARD_NAMCO_V226_PCB:
		b->player_count = kind == JVS_BOARD_NAMCO_V226 ? 1 : 2;
		b->digital_in_count = 16;
		b->coin_input_count = 1;
		b->analog_count = 12;
		b->output_count = 6;
		break;
	default:	/* the 837-13844s */
		b->player_count = 2;
		b->coin_input_count = 2;
		b->digital_in_count = kind == JVS_BOARD_837_13844_ENCODERS ? 8 : 12;
		b->encoder_count = kind == JVS_BOARD_837_13844_ENCODERS ? 4 : 0;
		b->light_gun_count = kind == JVS_BOARD_837_13844_TOUCH ? 1 : 0;
		b->analog_count = 8;
		b->output_count = 22;
		break;
	}
}

static const char *jvs_board_id(const jvs_board *b)
{
	switch (b->kind)
	{
	case JVS_BOARD_837_13938:
	case JVS_BOARD_837_13938_SHOOTOUT:
	case JVS_BOARD_837_13938_KICK4CASH:
	case JVS_BOARD_837_13938_CRACKINDJ:
		return "SEGA ENTERPRISES,LTD.;837-13938 ENCORDER BD  ;Ver0.01;99/08";
	case JVS_BOARD_837_13844:
	case JVS_BOARD_837_13844_ENCODERS:
	case JVS_BOARD_837_13844_TOUCH:
	case JVS_BOARD_837_13844_WRUNGP:
	case JVS_BOARD_837_13844_RACING:
	case JVS_BOARD_837_13844_18WHEELER:
	case JVS_BOARD_837_13844_SAMBA:
		return "SEGA ENTERPRISES,LTD.;837-13844-01 I/O CNTL BD2 ;Ver1.00;99/07";
	case JVS_BOARD_NAMCO_JYU:
		return "namco ltd.;JYU-PCB;Ver1.00;JPN,2Coins 2Guns";
	case JVS_BOARD_NAMCO_FCB:
		return "namco ltd.;FCB;Ver1.0;JPN,Touch Panel & Multipurpose";
	case JVS_BOARD_NAMCO_FCA:
		return "namco ltd.;FCA-1;Ver1.01;JPN,Multipurpose + Rotary Encoder";
	default:	/* the 837-13551s, and World Kicks' boards that say they are one */
		return "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";
	}
}

/* The first byte of the outputs the game last set on board @node (1 or 2) */
static u8 jvs_output0(const jvs_boards *bs, u32 node)
{
	return node >= 1 && node <= bs->count ? bs->board[node - 1].output0 : 0;
}

/* ...the cabinet's test switch, as the board reports it. Mushiking's is
 * seen a read later than it goes down, or the BIOS menu cannot be used
 * with it. */
static bool jvs_read_test_switch(jvs_board *b)
{
	if (b->kind != JVS_BOARD_837_13551_MUSHIKING)
		return !(kcode[0] & NAOMI_TEST_KEY);
	if (kcode[0] & NAOMI_TEST_KEY)
	{
		b->test_down = 0;
		return false;
	}
	if (!b->test_down)
	{
		b->test_down = 1;
		return false;
	}
	return true;
}

/* ...a player's switches. Mushiking: the card dispenser's switches are on
 * the players' inputs; it says it has cards (P1 START) unless the test
 * switch is down, never that it is empty (P1 RIGHT) or jammed (P2 START).
 * Wave Runner GP: the drive board's RX0-7 are on the players' buttons. */
static u32 jvs_read_digital_in(jvs_board *b, int player_num)
{
	u32 rv;
	switch (b->kind)
	{
	case JVS_BOARD_NAMCO_V226:
		{
			const u32 keycode1 = ~kcode[0];
			const u32 keycode2 = ~kcode[1];
			const u32 keycode3 = ~kcode[2];
			const u32 keycode4 = ~kcode[3];
			/* the main button: start, left, right, button 1; enter; the service menu */
			return ((keycode1 & NAOMI_BTN0_KEY) << 6)
					| ((keycode2 & NAOMI_BTN0_KEY) << 2)
					| ((keycode3 & NAOMI_BTN0_KEY) << 1)
					| ((keycode4 & NAOMI_BTN0_KEY) >> 1)
					| ((keycode1 & NAOMI_BTN3_KEY) << 3)
					| (keycode1 & (NAOMI_SERVICE_KEY | NAOMI_UP_KEY | NAOMI_DOWN_KEY));
		}
	case JVS_BOARD_NAMCO_V226_PCB:
		{
			u32 keycode;
			u8 trigger;
			/* (how many players a request asks for is the game's to say) */
			if (player_num > 3)
				return 0;
			keycode = ~kcode[player_num];
			trigger = rt[player_num] >> 2;
			/* the ball button; the others; button 4 as button 0 (change) */
			return ((trigger & 0x20) << 3) | ((trigger & 0x10) << 5) | ((trigger & 0x08) << 7)
					| ((trigger & 0x04) << 9) | ((trigger & 0x02) << 11) | ((trigger & 0x01) << 13)
					| (keycode & (NAOMI_SERVICE_KEY | NAOMI_TEST_KEY | NAOMI_START_KEY))
					| ((keycode & NAOMI_BTN0_KEY) >> 4);
		}
	default:
		break;
	}
	rv = player_num > 3 ? 0 : ~kcode[player_num];
	if (b->kind == JVS_BOARD_NAMCO_JYU)
	{
		/* buttons 1 and 4 the other way round */
		u16 mapped = (u16)(rv | NAOMI_BTN1_KEY | NAOMI_BTN4_KEY);
		if (!(rv & NAOMI_BTN1_KEY))
			mapped &= ~NAOMI_BTN4_KEY;
		if (!(rv & NAOMI_BTN4_KEY))
			mapped &= ~NAOMI_BTN1_KEY;
		rv = mapped;
	}
	if (b->kind == JVS_BOARD_837_13551_MUSHIKING)
	{
		if (player_num == 0)
		{
			if (kcode[0] & NAOMI_TEST_KEY)
				rv |= NAOMI_START_KEY;
			rv &= ~NAOMI_RIGHT_KEY;
		}
		else if (player_num == 1)
			rv &= ~NAOMI_START_KEY;
	}
	else if (b->kind == JVS_BOARD_837_13844_WRUNGP && (player_num == 0 || player_num == 1))
	{
		const u8 bits = player_num == 0 ? b->drive_board >> 4 : b->drive_board & 0xf;
		rv |= NAOMI_BTN2_KEY | NAOMI_BTN3_KEY | NAOMI_BTN4_KEY | NAOMI_BTN5_KEY;
		if (bits & 1)
			rv &= ~NAOMI_BTN5_KEY;
		if (bits & 2)
			rv &= ~NAOMI_BTN4_KEY;
		if (bits & 4)
			rv &= ~NAOMI_BTN3_KEY;
		if (bits & 8)
			rv &= ~NAOMI_BTN2_KEY;
	}
	return rv;
}

/* World Kicks' sticks: x, then y as that read left it */
static u16 jvs_v226_x(jvs_board *b, int joy, int magnitude)
{
	s8 axis_x = joyx[joy];
	b->axis_y = joyy[joy];
	maple_limit_stick(&axis_x, &b->axis_y, magnitude);
	return magnitude == 64 ? (u16)((0xff - axis_x) << 8) : (u16)((u32)(int)axis_x << 8);
}

static u16 jvs_v226_y(const jvs_board *b)
{
	return (u16)((0xff - b->axis_y) << 8);
}

static u16 jvs_read_analog_base(int player_num, int player_axis, bool inverted)
{
	u16 v;
	switch (player_axis)
	{
	case 0:
		v = joyx[player_num];
		break;
	case 1:
		v = joyy[player_num];
		break;
	case 2:
		v = joyrx[player_num];
		break;
	case 3:
		v = joyry[player_num];
		break;
	case 4:
		v = rt[player_num];
		break;
	case 5:
		v = lt[player_num];
		break;
	default:
		return 0x8000;
	}
	return (u16)((u32)(inverted ? 0xff - v : v) << 8);
}

static u16 jvs_read_analog_axis(jvs_board *b, int player_num, int player_axis, bool inverted)
{
	switch (b->kind)
	{
	case JVS_BOARD_837_13844_18WHEELER:
		{
			/* the wheel does not turn all the way */
			u16 v = jvs_read_analog_base(player_num, player_axis, inverted);
			if (player_axis == 0)
			{
				if (v < 0x1000)
					v = 0x1000;
				if (v > 0xefff)
					v = 0xefff;
			}
			return v;
		}
	case JVS_BOARD_837_13844_SAMBA:
		{
			/* the maracas' sensors, as the game takes them: rounded, no
			 * more than 0xf000, the first X lower and the second higher */
			int v = jvs_read_analog_base(player_num, player_axis, inverted);
			v &= ~0xff;
			if (v > 0xf000)
				v = 0xf000;
			if (player_axis == 0)
				v = v - 0x2000 > 0 ? v - 0x2000 : 0;
			else if (player_axis == 2)
				v = v + 0x2000 < 0xf000 ? v + 0x2000 : 0xf000;
			return (u16)v;
		}
	case JVS_BOARD_NAMCO_FCB:
		if (b->init_in_progress)
			return 0;
		if (mo_x_abs[player_num] < 0 || mo_x_abs[player_num] > 639 || mo_y_abs[player_num] < 0 || mo_y_abs[player_num] > 479)
			return 0;
		return 0x8000;
	case JVS_BOARD_NAMCO_V226:
		if (player_num == 0 || player_num == 1)
		{
			const int joy = player_num * 2;
			switch (player_axis)
			{
			case 0:
				return jvs_v226_x(b, joy, 64);
			case 1:
				return jvs_v226_y(b);
			case 2:
				return jvs_v226_x(b, joy + 1, 64);
			case 3:
				return jvs_v226_y(b);
			}
		}
		else if (player_num == 2 && player_axis >= 0 && player_axis <= 3)
			return (u16)(rt[player_axis] << 8);
		return 0x8000;
	case JVS_BOARD_NAMCO_V226_PCB:
		/* (each player's stick: the second board has players 3 and 4) */
		if (player_num >= 0 && player_num < 4)
		{
			if (player_axis == 0)
				return jvs_v226_x(b, player_num, 48);
			if (player_axis == 1)
				return jvs_v226_y(b);
		}
		return 0x8000;
	default:
		return jvs_read_analog_base(player_num, player_axis, inverted);
	}
}

/* (std::max() and std::min(), not a number and all) */
static float jvs_fmax(float a, float b)
{
	return a < b ? b : a;
}

static float jvs_fmin(float a, float b)
{
	return b < a ? b : a;
}

/* Where a player's gun or finger is: the 640x480 screen position as it is.
 * The touchscreen's has the top bits set while it is touched (any value
 * from 0x1000 does, once the screen is calibrated: Touch de Uno 1 and 2). */
static void jvs_read_screen_position(const jvs_board *b, int player_num, s16 *x, s16 *y)
{
	if (b->kind != JVS_BOARD_837_13844_TOUCH)
	{
		*x = (s16)mo_x_abs[player_num];
		*y = (s16)mo_y_abs[player_num];
		return;
	}
	*x = (s16)jvs_fmin(jvs_fmax(mo_x_abs[player_num], 0.f), 639.f);
	*y = (s16)jvs_fmin(jvs_fmax(mo_y_abs[player_num], 0.f), 479.f);
	if (~kcode[player_num] & NAOMI_BTN0_KEY)
	{
		*x |= 0xf000;
		*y |= 0xf000;
	}
}

/* A player's switches as the game sees them, after the game's own buttons
 * have been put where it has them: some are wired to a drive board, a
 * shifter. 18 Wheeler: SHIFT L/H changes from low to high gear and back,
 * SHIFT R is reverse; the gear is on the second player's UP, DOWN and
 * LEFT. */
static u32 jvs_wired_inputs(jvs_board *b, int player_num, u32 keycode)
{
	u32 bits;
	if (!jvs_racing(b))
		return keycode;
	/* the drive board's answer: bits 4-7 on the first player's switches, 0-3 on the second's */
	if (player_num == 0 || player_num == 1)
	{
		bits = player_num == 0 ? b->drive_board >> 4 : b->drive_board & 0xf;
		keycode |= NAOMI_BTN2_KEY | NAOMI_BTN3_KEY | NAOMI_BTN4_KEY | NAOMI_BTN5_KEY;
		if (bits & 1)
			keycode &= ~NAOMI_BTN5_KEY;
		if (bits & 2)
			keycode &= ~NAOMI_BTN4_KEY;
		if (bits & 4)
			keycode &= ~NAOMI_BTN3_KEY;
		if (bits & 8)
			keycode &= ~NAOMI_BTN2_KEY;
	}
	if (b->kind != JVS_BOARD_837_13844_18WHEELER)
		return keycode;
	if (player_num == 0)
	{
		const u32 buttons = ~kcode[0];
		if (buttons & NAOMI_BTN2_KEY)
			b->gear = 0xff;
		else if (buttons & NAOMI_BTN1_KEY)
		{
			if (!b->shift_held)
			{
				b->gear = b->gear == 0 ? 1 : 0;
				b->shift_held = true;
			}
		}
		else
			b->shift_held = false;
	}
	else if (player_num == 1)
	{
		switch (b->gear)
		{
		case 0xff:
			keycode |= NAOMI_LEFT_KEY | NAOMI_DOWN_KEY;
			keycode &= ~NAOMI_UP_KEY;
			break;
		case 1:
			keycode &= ~(NAOMI_LEFT_KEY | NAOMI_DOWN_KEY);
			keycode |= NAOMI_UP_KEY;
			break;
		default:
			keycode |= NAOMI_DOWN_KEY;
			keycode &= ~(NAOMI_UP_KEY | NAOMI_LEFT_KEY);
			break;
		}
	}
	return keycode;
}

/* Before the rotary encoders are read, @chans of them: the mice's motion
 * (mo_x_delta, mo_y_delta) where the board's encoders take it */
static void jvs_turn_encoders(const jvs_boards *bs, const jvs_board *b, int chans)
{
	switch (b->kind)
	{
	case JVS_BOARD_837_13938_SHOOTOUT:
		/* Shootout Pool: the cue's tip is aimed with the trackball (the
		 * first two encoders) and the cue pushed with a roller, the third.
		 * The trackball is the roller while BUTTON 4 is held: its motion up
		 * and down turns the third encoder then, and the aim stays. */
		if (chans >= 3 && (~kcode[0] & NAOMI_BTN3_KEY))
		{
			mo_x_delta[1] -= mo_y_delta[0];
			mo_x_delta[0] = 0;
			mo_y_delta[0] = 0;
		}
		break;
	case JVS_BOARD_837_13938_KICK4CASH:
		/* Kick '4' Cash: the encoders are at 45 degrees to the trackball's
		 * axes (both are read, or the motion left for the other is turned
		 * again) */
		if (chans >= 2)
		{
			const f32 x = mo_x_delta[0];
			const f32 y = -mo_y_delta[0];
			mo_x_delta[0] = (x - y) * 0.7071f;
			mo_y_delta[0] = -(x + y) * 0.7071f;
		}
		break;
	case JVS_BOARD_837_13938_CRACKINDJ:
		/* Crackin'DJ: the turntables are the first and third encoders. A
		 * motor turns them, 10 a read, while the game has the second
		 * board's output 5 on (the records spinning) and the player is not
		 * turning them (an encoder is a third of the mouse's motion). */
		if (!(jvs_output0(bs, 2) & 0x10))
			break;
		if (chans >= 1 && mo_x_delta[0] == 0)
			mo_x_delta[0] = -30.f;
		if (chans >= 3 && mo_x_delta[1] == 0)
			mo_x_delta[1] = -30.f;
		break;
	default:
		break;
	}
}

/* (a byte the other way round) */
static u8 jvs_reverse(u8 v)
{
	v = (u8)((v & 0xF0) >> 4 | (v & 0x0F) << 4);
	v = (u8)((v & 0xCC) >> 2 | (v & 0x33) << 2);
	v = (u8)((v & 0xAA) >> 1 | (v & 0x55) << 1);
	return v;
}

/* The 837-13844 wired to a drive board with force feedback: 838-13843 of
 * F355 Challenge (ROM EPR-21867), 838-13992 of 18 Wheeler (EPR-23000),
 * with the 838-12912-01 servo board of the Model 3. The game sends the
 * drive board a byte on outputs 15 to 22, and reads its answer on the
 * players' BUTTON 3 to 6. What the motor does is given to the frontend as
 * rumble, where a rumble motor can show it.
 * https://github.com/njz3/vJoyIOFeederWithFFB/blob/master/DRIVEBOARD.md */
static u8 jvs_drive_command(jvs_board *b, u8 in)
{
	u8 out = 0;
	in = (u8)~in;
	switch (in)
	{
	case 0:
	case 1:
	case 2:
		/* 18 Wheeler: light (60%), normal (80%), heavy (100%) */
		if (!b->active)
			b->power = (u8)(60 + in * 20);
		break;
	case 0xf0:
		b->active = false;
		b->test_mode = true;
		break;
	case 0xfe:
		b->active = true;
		break;
	case 0xff:
		b->test_mode = false;
		b->active = false;
		naomi_ffb_rumble(NAOMI_FFB_STOP, 0.f, 0);
		break;
	case 0xf1:
		out = 0x10;	/* F355 and Tokyo Bus Guide wait for it */
		break;
	/* F355: 100%, 90%, 80%, 60% */
	case 0xfa:
		b->power = 100;
		break;
	case 0xfb:
		b->power = 90;
		break;
	case 0xfc:
		b->power = 80;
		break;
	case 0xfd:
		b->power = 60;
		break;
	default:
		if (!b->active)
			break;
		if (in >= 0x40 && in <= 0x7f)
		{
			/* The spring that brings the wheel back to the centre (bits
			 * 0-3 its strength). Tokyo Bus Guide sets the power with it:
			 * 0x44 light, 0x47 normal, 0x4a heavy. */
			if (b->tokyo_bus && in <= 0x4a)
				b->power = (u8)((in & 0xf) * 10);
		}
		else if (in >= 0x80 && in <= 0xbf)
			/* shaking, at 25 Hz (softened by half) */
			naomi_ffb_rumble(NAOMI_FFB_SHAKE, (in & 0x3f) / 63.f * b->power / 100.f / 2.f, 650);
		else if (in >= 0xe0 && in <= 0xef)
		{
			/* the test menu's roll to the left and right */
			b->torque = (s8)(in < 0xe8 ? 0xe0 - in : in - 0xe8);
			naomi_ffb_rumble(NAOMI_FFB_FORCE, abs(b->torque) / 7.f, 0);
		}
		else if ((in & 0xf0) == 0xd0)
			/* the high bits of the damper (friction), which the next 0xcN sets */
			b->damper_high = in & 0xf;
		break;
	}
	if (b->test_mode)
		out = in;
	return jvs_reverse(out);
}

/* The outputs the game sets. A drive board's byte is outputs 15 to 22, the
 * last 8 bits, the other way round. Wave Runner GP's made-up drive board
 * answers which way the handle is turned. */
static void jvs_write_digital_out(jvs_board *b, int count, const u8 *data)
{
	if (count != 3 || !jvs_drive_board(b))
		return;
	if (jvs_racing(b))
	{
		b->drive_board = jvs_drive_command(b, jvs_reverse((u8)((data[1] << 6) | (data[2] >> 2))));
		return;
	}
	{
		u16 out = (u16)((data[1] << 6) | (data[2] >> 2));
		out = (u16)((out & 0xF0) >> 4 | (out & 0x0F) << 4);
		out = (u16)((out & 0xCC) >> 2 | (out & 0x33) << 2);
		out = (u16)((out & 0xAA) >> 1 | (out & 0x55) << 1);
		if (out == 0xff)
			b->drive_board = 0xff;
		else if ((out & 0xf) == 0xf)
		{
			out >>= 4;
			if (out > 7)
				b->drive_board = (u8)(0xff & ~(1 << (14 - out)));
			else
				b->drive_board = (u8)(0xff & ~(1 << out));
		}
		else if ((out & 0xf0) == 0xf0)
		{
			out &= 0xf;
			if (out > 7)
				b->drive_board = (u8)(0xff & ~(1 << (out - 7)));
			else
				b->drive_board = (u8)(0xff & ~(1 << (7 - out)));
		}
	}
}

/* ---- The boards on the bus ---- */

bool jvs_dual_reversed_game(void)
{
	return settings.mapping.JammaSetup == JVS_WorldKicksPCB
			|| (settings.mapping.JammaSetup == JVS_DualIOBoards4P
				&& (!strcmp(naomi_game_id, "VIRTUA ATHLETE")
					|| !strcmp(naomi_game_id, "ROYAL RUMBLE")
					|| !strcmp(naomi_game_id, "BEACH SPIKERS JAPAN")
					|| !strcmp(naomi_game_id, "MJ JAPAN")));
}

static jvs_board *jvs_add(jvs_boards *bs, u8 kind, u8 node_id, u8 first_player)
{
	jvs_board *b = &bs->board[bs->count++];
	jvs_board_make(b, kind, node_id, first_player);
	return b;
}

void jvs_boards_create(jvs_boards *bs)
{
	if (bs->count != 0)
		return;
	switch (settings.mapping.JammaSetup)
	{
	case JVS_Default:
	default:
		if (!strncmp(naomi_game_id, "MKG TKOB", 8) || !strncmp(naomi_game_id, "MUSHIKING", 9)
				|| !strcmp(naomi_game_id, "MUSHIUSA '04 1ST VER0.900-"))
			jvs_add(bs, JVS_BOARD_837_13551_MUSHIKING, 1, 0);
		else
			jvs_add(bs, JVS_BOARD_837_13551, 1, 0);
		break;
	case JVS_FourPlayers:
		jvs_add(bs, JVS_BOARD_837_13551_4P, 1, 0);
		break;
	case JVS_RotaryEncoders:
		if (!strncmp(naomi_game_id, "SHOOTOUT POOL", 13))
			jvs_add(bs, JVS_BOARD_837_13938_SHOOTOUT, 1, 0);
		else if (!strcmp(naomi_game_id, "KICK '4' CASH"))
			jvs_add(bs, JVS_BOARD_837_13938_KICK4CASH, 1, 0);
		else if (!strncmp(naomi_game_id, "CRACKIN'DJ", 10))
			jvs_add(bs, JVS_BOARD_837_13938_CRACKINDJ, 1, 0);
		else
			jvs_add(bs, JVS_BOARD_837_13938, 1, 0);
		jvs_add(bs, JVS_BOARD_837_13551, 2, 0);
		break;
	case JVS_OutTrigger:
		jvs_add(bs, JVS_BOARD_837_13938, 1, 0);
		jvs_add(bs, JVS_BOARD_837_13551_NOANALOG, 2, 0);
		break;
	case JVS_SegaMarineFishing:
		jvs_add(bs, JVS_BOARD_837_13844, 1, 0);
		break;
	case JVS_DualIOBoards4P:
		jvs_add(bs, JVS_BOARD_837_13551, 1, 0);
		jvs_add(bs, JVS_BOARD_837_13551, 2, 2);
		bs->cabinet_order = jvs_dual_reversed_game();
		break;
	case JVS_LightGun:
		jvs_add(bs, JVS_BOARD_NAMCO_JYU, 1, 0);
		break;
	case JVS_LightGunAsAnalog:
		/* a regular board sending the light gun's position as axes 0 and 1 */
		jvs_add(bs, JVS_BOARD_837_13551, 1, 0)->lightgun_as_analog = true;
		break;
	case JVS_Mazan:
		jvs_add(bs, JVS_BOARD_NAMCO_FCB, 1, 0);
		jvs_add(bs, JVS_BOARD_NAMCO_FCB, 2, 0);
		break;
	case JVS_GunSurvivor:
		jvs_add(bs, JVS_BOARD_NAMCO_FCA, 1, 0);
		break;
	case JVS_DogWalking:
		jvs_add(bs, JVS_BOARD_837_13844_ENCODERS, 1, 0);
		break;
	case JVS_TouchDeUno:
		jvs_add(bs, JVS_BOARD_837_13844_TOUCH, 1, 0);
		break;
	case JVS_WorldKicks:
		jvs_add(bs, JVS_BOARD_NAMCO_V226, 1, 0);
		break;
	case JVS_WorldKicksPCB:
		jvs_add(bs, JVS_BOARD_NAMCO_V226_PCB, 1, 0);
		jvs_add(bs, JVS_BOARD_NAMCO_V226_PCB, 2, 2);
		bs->cabinet_order = true;
		break;
	case JVS_WaveRunnerGP:
		jvs_add(bs, JVS_BOARD_837_13844_WRUNGP, 1, 0);
		break;
	case JVS_F355:
		jvs_add(bs, JVS_BOARD_837_13844_RACING, 1, 0);
		break;
	case JVS_TokyoBus:
		jvs_add(bs, JVS_BOARD_837_13844_RACING, 1, 0)->tokyo_bus = true;
		break;
	case JVS_EighteenWheeler:
		jvs_add(bs, JVS_BOARD_837_13844_18WHEELER, 1, 0);
		break;
	case JVS_SambaDeAmigo:
		jvs_add(bs, JVS_BOARD_837_13844_SAMBA, 1, 0);
		break;
	}
}

void jvs_boards_order(jvs_boards *bs)
{
	const bool reversed = (EEPROM[8] & 0x30) >= 0x20;
	bs->board[0].first_player = reversed ? 2 : 0;
	bs->board[1].first_player = reversed ? 0 : 2;
}

/* Where the rotary encoders are: one for each of the two ways each of the
 * four mice moves. A game goes by how far one has turned since it last
 * looked, so they are in the save state, from V21. */
static f32 rotx[4];
static f32 roty[4];

static u16 jvs_read_rotary_encoder(f32 *encoder_value, f32 delta_value)
{
	*encoder_value = (f32)fmod(*encoder_value + delta_value, 65536.f);
	while (*encoder_value < 0)
		*encoder_value += 65536.f;
	return (u16)(*encoder_value + 0.5);
}

#define JVS_OUT(b) buffer_out[length++] = (u8)(b)
#define JVS_STATUS1() JVS_OUT(1)
/* A board's answer is 255 bytes at the most, its sum, the last of them,
 * included. How many inputs a request asks for is the game's to say:
 * JVS_FIT() is how many of @n, at two bytes each, there is still room for. */
#define JVS_OUT_MAX 254
#define JVS_FIT(n) ((u32)(n) * 2 > JVS_OUT_MAX - length ? (JVS_OUT_MAX - length) / 2 : (u32)(n))

/* Read digital input */
static u32 jvs_digital(jvs_board *b, const u8 *buffer_in, u32 cmdi, u8 *buffer_out, u32 length)
{
	u8 global_btns = 0;
	u32 next_keycode = 0;
	int players, player;

	JVS_STATUS1();	/* report byte */
	LOGJVS("btns \n");
	if (jvs_read_test_switch(b))		/* the global test button */
		global_btns |= 0x80;
	LOGJVS("glob %02x \n", global_btns);
	JVS_OUT(global_btns);		/* test, tilt1, tilt2, tilt3, unused, unused, unused, unused */
	players = (int)JVS_FIT(buffer_in[cmdi + 1]);
	for (player = 0; player < players; player++)
	{
		u32 keycode = jvs_read_digital_in(b, b->first_player + player);
		if (naomi_game_inputs != NULL)
		{
			u32 new_keycode = 0;
			u32 own_keycode = 0;	/* switches read as others of the player's own */
			int i;
			for (i = 0; naomi_game_inputs->buttons[i].mask != 0; i++)
			{
				const ButtonDescriptor *button = &naomi_game_inputs->buttons[i];
				if (button->target == 0xffffffff)
				{
					/* a switch that is always on */
					if (button->p2_mask == 0)
						keycode |= button->mask;
					else if (b->first_player + player == 0)
						new_keycode |= button->p2_mask;
				}
				else if ((button->mask & keycode) != 0 && button->p2_mask != 0)
				{
					new_keycode |= button->p2_mask;
					keycode &= ~button->mask;
				}
				else if ((button->mask & keycode) != 0 && button->target != 0)
				{
					own_keycode |= button->target;
					keycode &= ~button->mask;
				}
			}
			keycode |= own_keycode;
			keycode |= next_keycode;
			next_keycode = new_keycode;
		}
		keycode = jvs_wired_inputs(b, b->first_player + player, keycode);
		LOGJVS("P%d %02x \n", b->first_player + player + 1, (keycode >> 8) & 0xFF);
		JVS_OUT(keycode >> 8);
		if (buffer_in[cmdi + 2] > 1)
		{
			LOGJVS("%02x \n", keycode & 0xFF);
			JVS_OUT(keycode);
		}
	}
	return length;
}

/* Read coins */
static u32 jvs_coins(jvs_board *b, const u8 *buffer_in, u32 cmdi, u8 *buffer_out, u32 length)
{
	u32 slots, slot;
	JVS_STATUS1();	/* report byte */
	LOGJVS("coins \n");
	slots = JVS_FIT(buffer_in[cmdi + 1]);
	for (slot = 0; slot < slots; slot++)
	{
		if (slot < ARRAY_SIZE(b->coin_count) && b->first_player + slot < 4)
		{
			if (!(kcode[b->first_player + slot] & NAOMI_COIN_KEY))
			{
				if (!b->coin_chute[slot])
				{
					b->coin_chute[slot] = true;
					b->coin_count[slot]++;
				}
			}
			else
				b->coin_chute[slot] = false;
			LOGJVS("0:%d \n", b->coin_count[slot]);
			JVS_OUT((b->coin_count[slot] >> 8) & 0x3F);	/* status (2 highest bits, 0: normal), coin count MSB */
			JVS_OUT(b->coin_count[slot]);			/* coin count LSB */
		}
		else
		{
			LOGJVS("%d:0 \n", slot);
			JVS_OUT(0);
			JVS_OUT(0);
		}
	}
	return length;
}

/* Read analog inputs */
static u32 jvs_analog(const jvs_boards *bs, jvs_board *b, const u8 *buffer_in, u32 cmdi, u8 *buffer_out, u32 length)
{
	int axis = 0, axes;
	int axes_per_player = 4;

	JVS_STATUS1();	/* report byte */
	axes = (int)JVS_FIT(buffer_in[cmdi + 1]);
	LOGJVS("ana \n");
	if (b->lightgun_as_analog)
	{
		/* Death Crimson / Confidential Mission */
		while (axis < axes && b->first_player * 2 + axis < 8)
		{
			const int player_num = b->first_player + axis / 2;
			u16 x = 0, y = 0;
			if (!(mo_x_abs[player_num] < 0 || mo_x_abs[player_num] > 639
					|| mo_y_abs[player_num] < 0 || mo_y_abs[player_num] > 479))
			{
				x = (u16)(mo_x_abs[player_num] * 0xFFFF / 639 + 0.5f);
				y = (u16)(mo_y_abs[player_num] * 0xFFFF / 479 + 0.5f);
			}
			LOGJVS("P%d x,y:%4x,%4x \n", player_num + 1, x, y);
			JVS_OUT(x >> 8);		/* X, MSB */
			JVS_OUT(x);			/* X, LSB */
			axis++;
			if (axis < axes)
			{
				JVS_OUT(y >> 8);	/* Y, MSB */
				JVS_OUT(y);		/* Y, LSB */
				axis++;
			}
		}
	}
	if (naomi_game_inputs != NULL)
	{
		int i = 0;
		while (naomi_game_inputs->axes[i].name != NULL)
			i++;
		axes_per_player = i == 0 ? 2 : i;	/* (2 the default) */
	}
	if (settings.mapping.JammaSetup == JVS_DualIOBoards4P && !bs->cabinet_order)
		/* a hack for Ring Out 4x4 */
		axes_per_player = 2;
	for (; axis < axes; axis++)
	{
		const int player_num = b->first_player + axis / axes_per_player;
		int player_axis = axis % axes_per_player;
		u16 axis_value = 0x8000;
		if (player_num < 4)
		{
			bool inverted = false;
			bool half = false;
			bool unmapped_half = false;
			if (naomi_game_inputs != NULL)
			{
				const AxisDescriptor *desc = &naomi_game_inputs->axes[player_axis];
				inverted = desc->inverted;
				half = desc->name != NULL && desc->type == Half;
				/* a pedal or lever no stick or trigger is for is let go, not half way */
				unmapped_half = half && desc->axis > 5;
				player_axis = (int)desc->axis;
			}
			axis_value = unmapped_half ? 0 : jvs_read_analog_axis(b, player_num, player_axis, inverted);
			/* King of Route 66 takes a pedal at exactly half way for the race won */
			if (half && axis_value == 0x8000)
				axis_value = 0x8100;
		}
		LOGJVS("P%d.%d:%4x \n", player_num + 1, player_axis + 1, axis_value);
		JVS_OUT(axis_value >> 8);
		JVS_OUT(axis_value);
	}
	return length;
}

/* Read rotary encoders (there are four mice, an encoder for each of the
 * two ways one moves) */
static u32 jvs_encoders(const jvs_boards *bs, jvs_board *b, const u8 *buffer_in, u32 cmdi, u8 *buffer_out, u32 length)
{
	int chans, chan;
	JVS_STATUS1();	/* report byte */
	LOGJVS("rotenc \n");
	chans = (int)JVS_FIT(buffer_in[cmdi + 1]);
	if (chans > 8)
		chans = 8;
	jvs_turn_encoders(bs, b, chans);
	for (chan = 0; chan < chans; chan++)
	{
		u16 v;
		if (chan & 1)
		{
			v = jvs_read_rotary_encoder(&roty[chan / 2], -mo_y_delta[chan / 2] / 3.f);
			mo_y_delta[chan / 2] = 0;
		}
		else
		{
			v = jvs_read_rotary_encoder(&rotx[chan / 2], mo_x_delta[chan / 2] / 3.f);
			mo_x_delta[chan / 2] = 0;
		}
		LOGJVS("%d:%4x \n", chan, v);
		JVS_OUT(v >> 8);	/* MSB */
		JVS_OUT(v);		/* LSB */
	}
	return length;
}

/* Read screen pos inputs (guns are numbered from 1: number 0 is no player
 * either) */
static u32 jvs_screen(const jvs_board *b, const u8 *buffer_in, u32 cmdi, u8 *buffer_out, u32 length)
{
	const u32 channel = buffer_in[cmdi + 1] - 1u;
	const int player_num = (int)(b->first_player + channel);
	JVS_STATUS1();	/* report byte */
	if ((u32)player_num >= 4)
	{
		LOGJVS("P%d lightgun inv \n", player_num + 1);
		JVS_OUT(0xFF);
		JVS_OUT(0xFF);
		JVS_OUT(0xFF);
		JVS_OUT(0xFF);
	}
	else
	{
		s16 x, y;
		jvs_read_screen_position(b, player_num, &x, &y);
		LOGJVS("P%d lightgun %4x,%4x \n", player_num + 1, x, y);
		JVS_OUT(x >> 8);	/* X, MSB */
		JVS_OUT(x);		/* X, LSB */
		JVS_OUT(y >> 8);	/* Y, MSB */
		JVS_OUT(y);		/* Y, LSB */
	}
	return length;
}

/* The requests for inputs and outputs, one after another */
static u32 jvs_inputs(jvs_boards *bs, jvs_board *b, const struct mcfg_map *config,
		const u8 *buffer_in, u32 length_in, u8 *buffer_out, u32 length)
{
	PlainJoystickState pjs;
	u32 cmdi;

	LOGJVS("JVS Node %d: \n", b->node_id);
	pjs_init(&pjs);
	mcfg_get_input(config, &pjs);
	JVS_STATUS1();	/* status */
	for (cmdi = 0; cmdi < length_in; )
	{
		/* (what there is no room left to answer is not answered: five bytes
		 * is the most a request takes before its inputs) */
		if (length > JVS_OUT_MAX - 5)
			break;
		switch (buffer_in[cmdi])
		{
		case 0x20:
			length = jvs_digital(b, buffer_in, cmdi, buffer_out, length);
			cmdi += 3;
			break;
		case 0x21:
			length = jvs_coins(b, buffer_in, cmdi, buffer_out, length);
			cmdi += 2;
			break;
		case 0x22:
			length = jvs_analog(bs, b, buffer_in, cmdi, buffer_out, length);
			cmdi += 2;
			break;
		case 0x23:
			length = jvs_encoders(bs, b, buffer_in, cmdi, buffer_out, length);
			cmdi += 2;
			break;
		case 0x25:
			length = jvs_screen(b, buffer_in, cmdi, buffer_out, length);
			cmdi += 2;
			break;
		case 0x32:	/* switched outputs */
			LOGJVS("output(%d) %x\n", buffer_in[cmdi + 1], buffer_in[cmdi + 2]);
			if (buffer_in[cmdi + 1] > 0)
				b->output0 = buffer_in[cmdi + 2];
			jvs_write_digital_out(b, buffer_in[cmdi + 1], &buffer_in[cmdi + 2]);
			JVS_STATUS1();	/* report byte */
			cmdi += buffer_in[cmdi + 1] + 2;
			break;
		case 0x33:	/* analog outputs: not switches, and nothing here has any */
			LOGJVS("analog output(%d) %x\n", buffer_in[cmdi + 1], buffer_in[cmdi + 2]);
			JVS_STATUS1();	/* report byte */
			cmdi += buffer_in[cmdi + 1] + 2;
			break;
		case 0x30:	/* subtract coins */
			/* (the chute's number is the game's: one there is, or nothing is done) */
			if (buffer_in[cmdi + 1] >= 1 && buffer_in[cmdi + 1] <= ARRAY_SIZE(b->coin_count))
				b->coin_count[buffer_in[cmdi + 1] - 1] -= (buffer_in[cmdi + 2] << 8) + buffer_in[cmdi + 3];
			JVS_STATUS1();	/* report byte */
			cmdi += 4;
			break;
		case 0x74:	/* the board's serial port, which a printer is on (Touch de Uno) */
			if (printer_present())
			{
				/* (no more than the request has) */
				const u32 len = buffer_in[cmdi + 1] < length_in - cmdi - 1 ? buffer_in[cmdi + 1] : length_in - cmdi - 1;
				u32 i;
				for (i = 0; i < len; i++)
					printer_print((char)buffer_in[cmdi + 2 + i]);
				cmdi += len + 2;
				JVS_STATUS1();	/* report */
				/* 00 hardware error, 01 head up error, 02 Vp Volt error, 03
				 * auto cutter error, 04 head temp error, 3* paper end error */
				JVS_OUT(0xf);	/* printer ok */
				break;
			}
			/* fall through */
		default:
			DEBUG_LOG(MAPLE, "JVS: Unknown input type %x\n", buffer_in[cmdi]);
			JVS_OUT(2);		/* report byte: command error */
			cmdi = length_in;	/* the requests after it are not answered */
			break;
		}
	}
	return length;
}

u32 jvs_board_message(jvs_boards *bs, u32 index, const struct mcfg_map *config,
		const u8 *buffer_in, u32 length_in, u8 *buffer_out)
{
	jvs_board *b = &bs->board[index];
	const u8 jvs_cmd = buffer_in[0];
	u32 length = 0, jvs_length, i;
	u8 calc_crc = 0;
	const char *p;

	if (jvs_cmd == 0xF0)		/* JVS reset: nothing to do */
		return 0;
	if (jvs_cmd == 0xF1 && (length_in < 2 || buffer_in[1] != b->node_id))
		return 0;		/* not for this one */

	JVS_OUT(0xE0);	/* sync */
	JVS_OUT(0);	/* master node id */
	jvs_length = length++;
	switch (jvs_cmd)
	{
	case 0xF1:	/* set board address */
		JVS_STATUS1();	/* status */
		JVS_STATUS1();	/* report */
		JVS_OUT(5);
		LOGJVS("JVS Node %d address assigned\n", b->node_id);
		break;
	case 0x10:	/* read ID data */
		JVS_STATUS1();
		JVS_STATUS1();
		for (p = jvs_board_id(b); *p != 0; )
			JVS_OUT(*p++);
		JVS_OUT(0);
		break;
	case 0x11:	/* get command format version */
		JVS_STATUS1();
		JVS_STATUS1();
		JVS_OUT(0x11);	/* 1.1 */
		break;
	case 0x12:	/* get JAMMA VIDEO version */
		JVS_STATUS1();
		JVS_STATUS1();
		JVS_OUT(0x20);	/* 2.0 */
		break;
	case 0x13:	/* get communication version */
		JVS_STATUS1();
		JVS_STATUS1();
		JVS_OUT(0x10);	/* 1.0 */
		break;
	case 0x14:	/* get slave features */
		JVS_STATUS1();
		JVS_STATUS1();
		JVS_OUT(1);	/* digital inputs */
		JVS_OUT(b->player_count);
		JVS_OUT(b->digital_in_count);
		JVS_OUT(0);
		if (b->coin_input_count > 0)
		{
			JVS_OUT(2);	/* coin inputs */
			JVS_OUT(b->coin_input_count);
			JVS_OUT(0);
			JVS_OUT(0);
		}
		if (b->analog_count > 0)
		{
			JVS_OUT(3);	/* analog inputs */
			JVS_OUT(b->analog_count);	/* channel count */
			JVS_OUT(0x10);	/* 16 bits per channel, 0: unknown */
			JVS_OUT(0);
		}
		if (b->encoder_count > 0)
		{
			JVS_OUT(4);	/* rotary encoders */
			JVS_OUT(b->encoder_count);
			JVS_OUT(0);
			JVS_OUT(0);
		}
		if (b->light_gun_count > 0)
		{
			JVS_OUT(6);	/* light gun */
			JVS_OUT(16);	/* X bits */
			JVS_OUT(16);	/* Y bits */
			JVS_OUT(b->light_gun_count);
		}
		JVS_OUT(0x12);	/* general output driver */
		JVS_OUT(b->output_count);
		JVS_OUT(0);
		JVS_OUT(0);
		JVS_OUT(0);	/* end of list */
		break;
	case 0x15:	/* master board ID */
		JVS_STATUS1();
		JVS_STATUS1();
		break;
	case 0x70:
		LOGJVS("JVS 0x70: %02x %02x %02x %02x\n", buffer_in[1], buffer_in[2], buffer_in[3], buffer_in[4]);
		b->init_in_progress = true;
		JVS_STATUS1();
		JVS_STATUS1();
		if (buffer_in[2] == 3)
		{
			JVS_OUT(0x10);
			for (i = 0; i < 16; i++)
				JVS_OUT(0x7f);
			if (buffer_in[4] == 0x10 || buffer_in[4] == 0x11)
				b->init_in_progress = false;
		}
		else
		{
			JVS_OUT(2);
			JVS_OUT(3);
			JVS_OUT(1);
		}
		break;
	default:
		if ((jvs_cmd >= 0x20 && jvs_cmd <= 0x38) || (jvs_cmd == 0x74 && printer_present()))
			length = jvs_inputs(bs, b, config, buffer_in, length_in, buffer_out, length);
		else if (length_in == 0)
			/* a request with nothing in it has nothing wrong with it: the
			 * board says so, and has nothing to report */
			JVS_STATUS1();
		else
		{
			INFO_LOG(MAPLE, "JVS: Unknown JVS command %x\n", jvs_cmd);
			JVS_OUT(2);	/* unknown command */
		}
		break;
	}
	buffer_out[jvs_length] = (u8)(length - 2);
	for (i = 1; i < length; i++)
		calc_crc = (u8)((calc_crc + buffer_out[i]) & 0xFF);
	JVS_OUT(calc_crc);
	LOGJVS("CRC %x\n", calc_crc);
	return length;
}

bool jvs_board_serialize(const jvs_board *board, void **data, unsigned int *total_size)
{
	LIBRETRO_S(board->node_id);
	LIBRETRO_S(board->lightgun_as_analog);
	return true;
}

bool jvs_board_unserialize(jvs_board *board, void **data, unsigned int *total_size)
{
	u8 lightgun_as_analog = 0;
	LIBRETRO_US(board->node_id);
	/* (a byte of the state's, which is true unless 0) */
	LIBRETRO_USA(&lightgun_as_analog, 1);
	board->lightgun_as_analog = lightgun_as_analog != 0;
	return true;
}

/* ---- The boards' parts of a save state ---- */

/* What the I/O boards have had in a save state since V21, after everything
 * older. It is the same size whatever the cabinet has, and whether or not
 * its boards have been made yet: room for two boards, which is the most a
 * cabinet has, and the eight rotary encoders. A board is
 *   u32 coin_count[4]   the coins counted in each chute
 *   u8  coin_chute[4]   whether each coin switch was closed when last read
 *   u8  drive_board     what a drive board last answered
 *   u8  init_in_progress
 * None of it was in a state. A load made the boards anew, with nothing
 * counted, and the game, which goes by how far a count has moved since it
 * last looked, saw coins come or go: on every frame with run-ahead, which
 * loads a state each frame. The switches are kept with the counts: a coin
 * that is in the chute across a load is otherwise counted twice. */
#define JVS_STATE_BOARD_FLAGS 6
#define JVS_STATE_BOARD_SIZE (4 * sizeof(u32) + JVS_STATE_BOARD_FLAGS)

static void jvs_board_serialize_v21(const jvs_board *b, void **data, unsigned int *total_size)
{
	u8 flags[JVS_STATE_BOARD_FLAGS];
	int i;
	for (i = 0; i < 4; i++)
		flags[i] = b->coin_chute[i];
	flags[4] = jvs_drive_board(b) ? b->drive_board : 0;
	flags[5] = b->init_in_progress;
	LIBRETRO_SA(b->coin_count, 4);
	LIBRETRO_SA(flags, JVS_STATE_BOARD_FLAGS);
}

/* (a counter and the drive board's byte can be anything; a flag is 0 or 1
 * whatever byte the state has for it) */
static void jvs_board_state_v21(jvs_board *b, const u32 *coins, const u8 *flags)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		b->coin_count[i] = coins[i];
		b->coin_chute[i] = flags[i] != 0;
	}
	if (jvs_drive_board(b))
		b->drive_board = flags[4];
	b->init_in_progress = flags[5] != 0;
}

void maple_jvs_serialize_v21(void **data, unsigned int *total_size)
{
	static const u8 no_board[JVS_STATE_BOARD_SIZE] = { 0 };
	const jvs_boards *bs = maple_jvs_boards();
	u32 i;
	for (i = 0; i < JVS_BOARDS_MAX; i++)
	{
		if (bs != NULL && i < bs->count)
			jvs_board_serialize_v21(&bs->board[i], data, total_size);
		else
			LIBRETRO_SA(no_board, JVS_STATE_BOARD_SIZE);
	}
	LIBRETRO_SA(rotx, 4);
	LIBRETRO_SA(roty, 4);
}

void maple_jvs_unserialize_v21(void **data, unsigned int *total_size)
{
	jvs_boards *bs = maple_jvs_boards();
	f32 rot[8];
	u32 i;
	/* (the boards have been made by now: the older part of the state does that) */
	for (i = 0; i < JVS_BOARDS_MAX; i++)
	{
		u32 coins[4];
		u8 flags[JVS_STATE_BOARD_FLAGS];
		memset(coins, 0, sizeof(coins));
		memset(flags, 0, sizeof(flags));
		LIBRETRO_USA(coins, 4);
		LIBRETRO_USA(flags, JVS_STATE_BOARD_FLAGS);
		if (bs != NULL && i < bs->count)
			jvs_board_state_v21(&bs->board[i], coins, flags);
	}
	memset(rot, 0, sizeof(rot));
	LIBRETRO_USA(rot, 8);
	for (i = 0; i < 8; i++)
	{
		/* An encoder counts from 0 to 65535 and round again: anything else
		 * (not a number, too) is not where one can be */
		if (!(rot[i] >= 0.f && rot[i] < 65536.f))
			rot[i] = 0.f;
	}
	memcpy(rotx, &rot[0], sizeof(rotx));
	memcpy(roty, &rot[4], sizeof(roty));
}

/* A state from before V21 has none of this. Its boards are new ones, with
 * nothing counted, as they have been on every load so far. The drive
 * board's byte is the one it answers with when it has nothing to say,
 * which is what the game most likely last saw, and not the 0 of a board
 * that has not been spoken to yet. The encoders stay where they are. */
void maple_jvs_state_before_v21(void)
{
	static const u32 coins[4] = { 0 };
	static const u8 flags[JVS_STATE_BOARD_FLAGS] = { 0, 0, 0, 0, 0xff, 0 };
	jvs_boards *bs = maple_jvs_boards();
	u32 i;
	if (bs == NULL)
		return;
	for (i = 0; i < bs->count; i++)
		jvs_board_state_v21(&bs->board[i], coins, flags);
}

/* What the I/O boards have had in a save state since V25, after everything
 * older: a byte for each of the two boards a cabinet can have, which only
 * Mushiking's uses (whether its test switch was down at the last read), 0
 * for any other or no board. 2 bytes. */
void maple_jvs_serialize_v25(void **data, unsigned int *total_size)
{
	const jvs_boards *bs = maple_jvs_boards();
	u8 boards[JVS_BOARDS_MAX];
	u32 i;
	memset(boards, 0, sizeof(boards));
	for (i = 0; i < JVS_BOARDS_MAX; i++)
		if (bs != NULL && i < bs->count && bs->board[i].kind == JVS_BOARD_837_13551_MUSHIKING)
			boards[i] = bs->board[i].test_down;
	LIBRETRO_SA(boards, JVS_BOARDS_MAX);
}

bool maple_jvs_unserialize_v25(void **data, unsigned int *total_size)
{
	jvs_boards *bs = maple_jvs_boards();
	u8 boards[JVS_BOARDS_MAX];
	u32 i;
	memset(boards, 0, sizeof(boards));
	LIBRETRO_USA(boards, JVS_BOARDS_MAX);
	for (i = 0; i < JVS_BOARDS_MAX; i++)
		if (bs != NULL && i < bs->count && bs->board[i].kind == JVS_BOARD_837_13551_MUSHIKING)
			bs->board[i].test_down = boards[i] & 1;
	return true;
}

/* A state from before V25: the boards are new ones, as they were on every
 * load until now (the test switch not seen down). */
void maple_jvs_state_before_v25(void)
{
	jvs_boards *bs = maple_jvs_boards();
	u32 i;
	if (bs == NULL)
		return;
	for (i = 0; i < bs->count; i++)
		if (bs->board[i].kind == JVS_BOARD_837_13551_MUSHIKING)
			bs->board[i].test_down = 0;
}

/* What the I/O boards have had in a save state since V27, after everything
 * older: 6 bytes for each of the two boards a cabinet can have (zeros for
 * none), 12 bytes. Every board has the first byte of its outputs, which
 * Crackin'DJ's turntables go by; the boards wired to a drive board (F355
 * Challenge, Tokyo Bus Guide, 18 Wheeler) what the drive board is doing:
 *   [1] bit 0 test mode, bit 1 active (and 18 Wheeler's bit 2, SHIFT L/H held)
 *   [2] the damper's high bits
 *   [3] the motor's power, in percent
 *   [4] the force of the test menu's roll, -7 to 7
 *   [5] 18 Wheeler's gear (0 low, 1 high, 0xff reverse) */
#define JVS_STATE_V27_SIZE 6

static void jvs_board_get_v27(const jvs_board *b, u8 *v)
{
	v[0] = b->output0;
	if (!jvs_racing(b))
		return;
	v[1] = (u8)((b->test_mode ? 1 : 0) | (b->active ? 2 : 0));
	v[2] = b->damper_high;
	v[3] = b->power;
	v[4] = (u8)b->torque;
	if (b->kind != JVS_BOARD_837_13844_18WHEELER)
		return;
	v[1] |= b->shift_held ? 4 : 0;
	v[5] = b->gear;
}

/* (each board checks its own) */
static void jvs_board_set_v27(jvs_board *b, const u8 *v)
{
	b->output0 = v[0];
	if (!jvs_racing(b))
		return;
	b->test_mode = (v[1] & 1) != 0;
	b->active = (v[1] & 2) != 0;
	b->damper_high = v[2] & 0xf;
	b->power = v[3] <= 100 ? v[3] : 100;
	b->torque = (s8)v[4] >= -7 && (s8)v[4] <= 7 ? (s8)v[4] : 0;
	if (b->active && b->torque != 0)
		naomi_ffb_rumble(NAOMI_FFB_FORCE, abs(b->torque) / 7.f, 0);
	else
		naomi_ffb_rumble(NAOMI_FFB_STOP, 0.f, 0);
	if (b->kind != JVS_BOARD_837_13844_18WHEELER)
		return;
	b->shift_held = (v[1] & 4) != 0;
	b->gear = v[5] == 1 || v[5] == 0xff ? v[5] : 0;
}

void maple_jvs_serialize_v27(void **data, unsigned int *total_size)
{
	const jvs_boards *bs = maple_jvs_boards();
	u8 boards[JVS_BOARDS_MAX][JVS_STATE_V27_SIZE];
	u32 i;
	memset(boards, 0, sizeof(boards));
	for (i = 0; i < JVS_BOARDS_MAX; i++)
		if (bs != NULL && i < bs->count)
			jvs_board_get_v27(&bs->board[i], boards[i]);
	LIBRETRO_SA(&boards[0][0], JVS_BOARDS_MAX * JVS_STATE_V27_SIZE);
}

bool maple_jvs_unserialize_v27(void **data, unsigned int *total_size)
{
	jvs_boards *bs = maple_jvs_boards();
	u8 boards[JVS_BOARDS_MAX][JVS_STATE_V27_SIZE];
	u32 i;
	memset(boards, 0, sizeof(boards));
	LIBRETRO_USA(&boards[0][0], JVS_BOARDS_MAX * JVS_STATE_V27_SIZE);
	for (i = 0; i < JVS_BOARDS_MAX; i++)
		if (bs != NULL && i < bs->count)
			jvs_board_set_v27(&bs->board[i], boards[i]);
	return true;
}

/* A state from before V27: no outputs set, a drive board not yet told to
 * work, at full power, damper at its middle, in low gear. */
void maple_jvs_state_before_v27(void)
{
	static const u8 board[JVS_STATE_V27_SIZE] = { 0, 0, 8, 100, 0, 0 };
	jvs_boards *bs = maple_jvs_boards();
	u32 i;
	if (bs == NULL)
		return;
	for (i = 0; i < bs->count; i++)
		jvs_board_set_v27(&bs->board[i], board);
}
