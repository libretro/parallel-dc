/* What is plugged into the maple bus (core/hw/maple/maple_cfg.c) by itself,
 * every function of it called at random: the devices a Dreamcast, a NAOMI
 * (with its card readers, or a System SP's none) and an Atomiswave (each of
 * its cabinets) are given, the settings changed under them and the devices
 * made to match, states saved and loaded - devices of each kind, of none,
 * of no kind there is, cut short, from V1 - and what the devices then ask
 * for: inputs with and without threaded rendering, the mouse, where a gun
 * points, the VMU's screen passed to the renderer, a vibration. The devices
 * are maple_cfg_shim's, saying what is done to them. All of it goes into a
 * hash, step by step; it must be what it always was
 * (maple_cfg_expected.txt). See maple_cfg_run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_if.h"
#include "hw/maple/maple_state.h"
#include "hw/naomi/naomi_periph.h"
#include "hw/naomi/naomi_cart.h"
#include "libretro/pad_state.h"

#define STEPS 5000

void mcfg_Create(MapleDeviceType type, u32 bus, u32 port, int player_num);
void mcfg_DestroyDevice(int i, int j);
MapleDeviceType maple_dev_type(maple_device *dev);

/* The shim's: what a device asks of where its inputs come from, and the
 * frontend's threaded input handed over */
void tdev_input(maple_device *dev);
void tdev_mouse(maple_device *dev);
void tdev_abs(maple_device *dev);
void tdev_image(maple_device *dev, void *img);
void tdev_vibration(maple_device *dev, u32 value, u32 duration);
void tdev_changed(maple_device *dev, bool changed);
void tlatch_init(void);
void tlatch_publish(const u32 *kcodes, const s8 *joy, const u8 *trig, const u32 *mouse);

settings_t settings;
retro_log_printf_t log_cb;
maple_device *MapleDevices[4][6];
bool EEPROM_loaded;
bool naomi_cart_systemsp;
bool enable_purupuru;
u32 kcode[4];
u8 rt[4], lt[4];
s8 joyx[4], joyy[4], joyrx[4], joyry[4];
f32 mo_x_abs[4], mo_y_abs[4];
u32 mo_buttons[4];
f32 mo_x_delta[4], mo_y_delta[4], mo_wheel_delta[4];
char naomi_game_id[33];

static u32 h = 2166136261u;
static u32 rng = 5;
static bool rfid_game;
static u8 state[4096];
static u8 *state_end = state + sizeof(state);	/* where a state read stops short */
static bool unserialize_failed;

void mix(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		h ^= v & 0xff;
		h *= 16777619u;
		v >>= 8;
	}
}

void mixf(f32 f)
{
	u32 v;
	memcpy(&v, &f, 4);
	mix(v);
}

u32 rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

/* --- what maple_cfg asks of the rest of the core --- */

void t_UpdateInputState(u32 port)
{
	mix(0x100 | port);
	kcode[port] = rnd();
	joyx[port] = (s8)rnd();
	joyy[port] = (s8)rnd();
	joyrx[port] = (s8)rnd();
	joyry[port] = (s8)rnd();
	rt[port] = (u8)rnd();
	lt[port] = (u8)rnd();
}

void t_UpdateInputStateMouse(u32 port)
{
	mix(0x110 | port);
	mo_buttons[port] = rnd();
	mo_x_delta[port] = (f32)(int)(rnd() % 200) - 100.f;
	mo_y_delta[port] = (f32)(int)(rnd() % 200) - 100.f;
	mo_wheel_delta[port] = (f32)(int)(rnd() % 10);
}

void t_UpdateInputStateArcade(void)
{
	u32 p;
	mix(0x120);
	for (p = 0; p < 4; p++)
	{
		t_UpdateInputState(p);
		t_UpdateInputStateMouse(p);
	}
}

void t_UpdateVibration(u32 port, u32 value, u32 max_duration)
{
	mix(0x130 | port);
	mix(value);
	mix(max_duration);
}


bool naomi_periph_rfid_game(void)
{
	return rfid_game;
}

void t_maple_naomi_eeprom_load(void)
{
	mix(0x140);
	EEPROM_loaded = true;
}

void UpdateInputState(u32 port) { t_UpdateInputState(port); }
void UpdateInputStateMouse(u32 port) { t_UpdateInputStateMouse(port); }
void UpdateInputStateArcade(void) { t_UpdateInputStateArcade(); }
void UpdateVibration(u32 port, u32 value, u32 max_duration) { t_UpdateVibration(port, value, max_duration); }
void maple_naomi_eeprom_load(void) { t_maple_naomi_eeprom_load(); }

bool ra_unserialize(void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	u8 *p = (u8 *)*dest;
	if (p + src_size > state_end)
	{
		/* (as serialize.c does it) */
		unserialize_failed = true;
		memset(src, 0, src_size);
		*total_size += src_size;
		return false;
	}
	memcpy(src, p, src_size);
	*dest = p + src_size;
	*total_size += src_size;
	return true;
}

void ra_unserialize_fail(void)
{
	mix(0x150);
	unserialize_failed = true;
}

static void devices_mix(void)
{
	int b, p;
	for (b = 0; b < 4; b++)
	{
		for (p = 0; p < 6; p++)
			mix(MapleDevices[b][p] == NULL ? 0xff : (u32)maple_dev_type(MapleDevices[b][p]));
		mix(vmu_screen_params[b].vmu_lcd_screen != NULL);
	}
}

/* One of the devices there are, at random; NULL if none */
static maple_device *any_device(void)
{
	maple_device *there[24];
	int b, p, n = 0;
	for (b = 0; b < 4; b++)
		for (p = 0; p < 6; p++)
			if (MapleDevices[b][p] != NULL)
				there[n++] = MapleDevices[b][p];
	return n != 0 ? there[rnd() % n] : NULL;
}

static void settings_at_random(void)
{
	int b;
	static const MapleDeviceType kinds[] = { MDT_None, MDT_SegaController, MDT_Keyboard, MDT_Mouse,
		MDT_LightGun, MDT_TwinStick, MDT_AsciiStick, MDT_SegaVMU, MDT_PurupuruPack };
	for (b = 0; b < MAPLE_PORTS; b++)
		if (rnd() % 3 == 0)
			maple_devices[b] = kinds[rnd() % (sizeof(kinds) / sizeof(kinds[0]))];
	enable_purupuru = rnd() & 1;
}

static void step(void)
{
	int b, p;
	switch (rnd() % 29)
	{
	case 0:	/* another machine */
	{
		static const int systems[] = { DC_PLATFORM_DREAMCAST, DC_PLATFORM_NAOMI, DC_PLATFORM_NAOMI2, DC_PLATFORM_ATOMISWAVE };
		mcfg_DestroyDevices();
		settings.System = systems[rnd() % 4];
		naomi_cart_systemsp = rnd() % 4 == 0;
		rfid_game = rnd() % 4 == 0;
		settings.mapping.JammaSetup = (int)(rnd() % 8);
		settings_at_random();
		mcfg_CreateDevices();
		break;
	}
	case 1: case 2:
		settings_at_random();
		break;
	case 3:
		mcfg_ReconcileDevices(rnd() & 1);
		break;
	case 4:
		mix(mcfg_DevicesAsSettingsSay() ? 1 : 2);
		break;
	case 5:	/* a state saved, and loaded back, at times spoilt */
	{
		void *data = state;
		unsigned int size = 0;
		memset(state, 0, sizeof(state));
		mcfg_SerializeDevices(&data, &size);
		mix(size);
		for (b = 0; b < (int)size; b++)
			mix(state[b]);
		if (rnd() % 3 == 0)
			state[rnd() % (size + 1)] = (u8)(rnd() % 3 == 0 ? rnd() : rnd() % MDT_Count);
		data = state;
		size = 0;
		unserialize_failed = false;
		mcfg_UnserializeDevices(&data, &size);
		mix(size);
		mix(unserialize_failed);
		break;
	}
	case 6:	/* a state of any devices at all, at times stopping short */
	{
		void *data = state;
		unsigned int size = 0;
		for (b = 0; b < (int)sizeof(state); b++)
			state[b] = (u8)(rnd() % 4 == 0 ? rnd() % (MDT_Count + 2) : rnd() % 2 ? MDT_None : MDT_SegaVMU);
		if (rnd() % 3 == 0)
			state_end = state + rnd() % 40;
		unserialize_failed = false;
		mcfg_UnserializeDevices(&data, &size);
		state_end = state + sizeof(state);
		mix(size);
		mix(unserialize_failed);
		break;
	}
	case 19:	/* a card's file another, and the devices made as the settings say */
		for (b = 0; b < 4; b++)
			if (MapleDevices[b][0] != NULL && rnd() % 2 == 0)
				tdev_changed(MapleDevices[b][0], true);
		mcfg_ReconcileDevices(rnd() & 1);
		break;
	case 18:	/* an Atomiswave's trackball or two, from a state */
	{
		void *data = state;
		unsigned int size = 0;
		u8 *w = state;
		for (b = 0; b < 4; b++)
			for (p = 0; p < 6; p++)
				if (p == 5 && b >= 2)
				{
					*w++ = MDT_Mouse;
					*w++ = 0;
					*w++ = 0;
				}
				else
					*w++ = MDT_None;
		unserialize_failed = false;
		mcfg_UnserializeDevices(&data, &size);
		if (MapleDevices[2][5] != NULL)
			tdev_mouse(MapleDevices[2][5]);
		if (MapleDevices[3][5] != NULL)
			tdev_mouse(MapleDevices[3][5]);
		break;
	}
	case 7:	/* one of V1 */
	{
		void *data = state;
		unsigned int size = 0;
		mcfg_UnserializeDevicesV1(&data, &size);
		mix(size);
		break;
	}
	case 8:
		b = (int)(rnd() % 4);
		p = (int)(rnd() % 6);
		if (MapleDevices[b][p] != NULL)
			mcfg_DestroyDevice(b, p);
		break;
	case 9:
	{
		static const MapleDeviceType kinds[] = { MDT_SegaController, MDT_SegaVMU, MDT_Mouse,
			MDT_LightGun, MDT_Keyboard, MDT_PurupuruPack };
		b = (int)(rnd() % 4);
		p = (int)(rnd() % 6);
		if (MapleDevices[b][p] == NULL)
			mcfg_Create(kinds[rnd() % 6], (u32)b, (u32)p, (int)(rnd() % 3) - 1);
		break;
	}
	case 10:
		settings.rend.ThreadedRendering = rnd() & 1;
		break;
	case 11:	/* the frontend's input, handed over */
	{
		u32 kc[4], mouse[16];
		s8 joy[16];
		u8 trig[8];
		for (b = 0; b < 4; b++)
			kc[b] = rnd();
		for (b = 0; b < 16; b++)
		{
			joy[b] = (s8)rnd();
			mouse[b] = rnd() % 1000;
		}
		for (b = 0; b < 8; b++)
			trig[b] = (u8)rnd();
		tlatch_publish(kc, joy, trig, mouse);
		break;
	}
	case 12:
		for (b = 0; b < 4; b++)
		{
			mo_x_abs[b] = (f32)(int)(rnd() % 700);
			mo_y_abs[b] = (f32)(int)(rnd() % 500);
		}
		break;
	case 13:
		b = (int)(rnd() % 4);
		mix(mcfg_ArcadeKcode(b));
		{
			f32 x, y;
			mcfg_ArcadeAbsPos(b, &x, &y);
			mixf(x);
			mixf(y);
		}
		break;
	case 14:	/* a card's file now another */
	{
		maple_device *dev = any_device();
		if (dev != NULL)
			tdev_changed(dev, rnd() & 1);
		break;
	}
	case 15:
		if (rnd() & 1)
			vmu_lcd_reset();
		else	/* (a VMU's screen given to the renderer) */
			vmu_screen_params[rnd() % 4].vmu_lcd_screen = (u8 *)state;
		break;
	case 17:	/* the cabinet set another way, the devices as they are */
		settings.mapping.JammaSetup = (int)(rnd() % 8);
		break;
	case 16:
		b = (int)(rnd() % 5) - 1;
		mix(vmu_lcd_refresh(b) ? 3 : 4);
		{
			const u8 *px = vmu_lcd_pixels(b);
			int i;
			mix(px == NULL ? 0xdead : 0);
			if (px != NULL)
				for (i = 0; i < VMU_SCREEN_WIDTH * VMU_SCREEN_HEIGHT; i += 7)
					mix(px[i]);
		}
		break;
	default:	/* a device asks */
	{
		maple_device *dev = any_device();
		if (dev == NULL)
			break;
		switch (rnd() % 5)
		{
		case 0: tdev_input(dev); break;
		case 1: tdev_mouse(dev); break;
		case 2: tdev_abs(dev); break;
		case 3:
		{
			static u8 img[VMU_SCREEN_WIDTH * VMU_SCREEN_HEIGHT];
			int i;
			for (i = 0; i < (int)sizeof(img); i++)
				img[i] = (u8)(rnd() & 1);
			tdev_image(dev, img);
			break;
		}
		default: tdev_vibration(dev, rnd(), rnd() % 1000); break;
		}
		break;
	}
	}
	devices_mix();
	mix(EEPROM_loaded);
	for (b = 0; b < 4; b++)
	{
		mixf(mo_x_delta[b]);
		mixf(mo_y_delta[b]);
		mixf(mo_wheel_delta[b]);
	}
}

int main(void)
{
	int n;
	tlatch_init();
	settings.System = DC_PLATFORM_DREAMCAST;
	for (n = 0; n < MAPLE_PORTS; n++)
		maple_devices[n] = MDT_SegaController;
	enable_purupuru = true;
	mcfg_CreateDevices();
	for (n = 0; n < STEPS; n++)
	{
		step();
		if (n % 250 == 249)
			printf("%d: %08x\n", n, (unsigned)h);
	}
	/* Every place a Dreamcast's stick can be, kept in its circle */
	mcfg_DestroyDevices();
	settings.System = DC_PLATFORM_DREAMCAST;
	settings.rend.ThreadedRendering = 1;
	for (n = 0; n < MAPLE_PORTS; n++)
		maple_devices[n] = MDT_SegaController;
	mcfg_CreateDevices();
	for (n = 0; n < 65536; n++)
	{
		u32 kc[4] = { 0, 0, 0, 0 }, mouse[16];
		s8 joy[16];
		u8 trig[8];
		memset(joy, 0, sizeof(joy));
		memset(mouse, 0, sizeof(mouse));
		memset(trig, 0, sizeof(trig));
		joy[0] = (s8)(n & 0xff);
		joy[1] = (s8)(n >> 8);
		tlatch_publish(kc, joy, trig, mouse);
		tdev_input(MapleDevices[0][5]);
	}
	mcfg_DestroyDevices();
	printf("end: %08x\n", (unsigned)h);
	return 0;
}
