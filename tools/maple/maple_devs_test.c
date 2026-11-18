/* The devices on the maple bus (core/hw/maple/maple_devs.c), through
 * maple_if.h's functions, on machines, buses and frames made at random:
 * every kind of device of a Dreamcast, an Atomiswave and a NAOMI made,
 * set up, asked every command (cut short and too long), its files read
 * and written through the save writer, its part of a state written and
 * read back changed, made again in its own memory and gone; the IC card
 * readers' cards. Every answer and everything asked of the rest of the
 * machine is said into a hash printed every so many machines; what it
 * prints must be what it always was (maple_devs_expected.txt;
 * maple_devs_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <streams/file_stream.h>
#include <retro_atomic.h>
#include "types.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_if.h"
#include "hw/maple/maple_helper.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_state.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/naomi_periph.h"
#include "hw/serial_port.h"
#include "log/Log.h"
#include "libretro/pad_state.h"
void maple_jvs_serialize_v27(void **data, unsigned int *total_size);
#ifdef MAPLE_DEVS_TEST_OLD
/* (the devices' own: the old tree's headers had them not) */
extern u8 kb_shift;
extern u8 kb_led;
extern u8 kb_key[6];
#endif

/* ---- The machine around the devices ---- */

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
vmu_screen_params_t vmu_screen_params[4];
u32 RealTimeClock;
maple_device *MapleDevices[4][6];

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

static void mixbytes(const void *p, size_t n)
{
	const u8 *b = (const u8 *)p;
	size_t i;
	for (i = 0; i < n; i++)
	{
		hash ^= b[i];
		hash *= 16777619u;
	}
}

static u32 seed = 0x85ebca6bu;

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

/* (the inflater's: none, the plain code) */
uint64_t cpu_features_get(void)
{
	return 0;
}

void os_DebugBreak(void)
{
	mix(0x2ff);
}

/* (the card readers' new cards are made from the time: a fixed one; the
 * old code read time() itself) */
static u32 now = 1000000;
#ifdef MAPLE_DEVS_TEST_OLD
time_t time(time_t *t)
{
	now += 7;
	if (t)
		*t = now;
	return now;
}
#else
u32 periph_seed(void)
{
	now += 7;
	return now;
}
#endif

/* ---- What the devices ask of the rest ---- */

static mcfg_map_t maps[4][6];

static u32 map_id(const mcfg_map_t *map)
{
	return map == NULL ? 0xffff : (u32)(map - &maps[0][0]);
}

void mcfg_get_input(const mcfg_map_t *map, PlainJoystickState *pjs)
{
	mix(0x400);
	mix(map_id(map));
	pjs->kcode = rnd() | rnd();
	pjs->joy[0] = (u8)rnd();
	pjs->joy[1] = (u8)rnd();
	pjs->joy[2] = (u8)rnd();
	pjs->joy[3] = (u8)rnd();
	pjs->trigger[0] = (u8)rnd();
	pjs->trigger[1] = (u8)rnd();
}

void mcfg_get_mouse(const mcfg_map_t *map, u32 *buttons, f32 *delta_x, f32 *delta_y, f32 *delta_wheel)
{
	mix(0x401);
	mix(map_id(map));
	*buttons = rnd();
	*delta_x = (float)((int)rn(7000) - 3500) / 4.f;
	*delta_y = (float)((int)rn(7000) - 3500) / 4.f;
	*delta_wheel = (float)((int)rn(41) - 20) / 2.f;
}

void mcfg_get_absolute_position(const mcfg_map_t *map, f32 *px, f32 *py)
{
	mix(0x402);
	mix(map_id(map));
	*px = (float)((int)rn(3000) - 500) / 4.f;
	*py = (float)((int)rn(3000) - 500) / 4.f;
}

void mcfg_set_image(const mcfg_map_t *map, void *img)
{
	mix(0x403);
	mix(map_id(map));
	mixbytes(img, 48 * 32);
}

void mcfg_set_vibration(const mcfg_map_t *map, u32 value, u32 max_duration)
{
	mix(0x404);
	mix(map_id(map));
	mix(value);
	mix(max_duration);
}

void vmu_lcd_publish(int screen, const u8 *pixels)
{
	mix(0x405);
	mix((u32)screen);
	mixbytes(pixels, 48 * 32);
}

void vmu_beep_set(u32 period, u32 on)
{
	mix(0x406);
	mix(period);
	mix(on);
}

void read_lightgun_position(int x, int y)
{
	mix(0x407);
	mix((u32)x);
	mix((u32)y);
}

/* The memory cards' files: there or not, and how the writer gets on */
static bool vmu_file_there;
static bool vmu_file_made;
static int vmu_path_gen;
static u32 put_fails;
/* (a stretch where every write fails, or none is taken) */
static bool put_always_fails;
static bool put_never_taken;
static u32 put_calls;

size_t writable_vmu_path(char *out, size_t size, const char *logical_port)
{
	char path[64];
	size_t n;
	sprintf(path, "vmu/%d/%s.bin", vmu_path_gen, logical_port);
	n = strlen(path);
	if (out != NULL && size > 0)
	{
		strncpy(out, path, size - 1);
		out[size - 1] = 0;
	}
	return n;
}

static u8 fake_file;

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	(void)hints;
	mix(0x410);
	mixbytes(path, strlen(path));
	mix(mode);
	if (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING)
		return vmu_file_there ? (RFILE *)&fake_file : NULL;
	return vmu_file_made ? (RFILE *)&fake_file : NULL;
}

int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
	int64_t i;
	(void)stream;
	mix(0x411);
	for (i = 0; i < len; i++)
		((u8 *)data)[i] = (u8)(rn(8) == 0 ? rnd() : i * 7);
	return len;
}

int64_t filestream_write(RFILE *stream, const void *data, int64_t len)
{
	(void)stream;
	mix(0x412);
	mixbytes(data, (size_t)len);
	return len;
}

int64_t filestream_seek(RFILE *stream, int64_t offset, int seek_position)
{
	(void)stream;
	mix(0x413);
	mix((u32)offset);
	mix((u32)seek_position);
	return 0;
}

int filestream_close(RFILE *stream)
{
	(void)stream;
	mix(0x414);
	return 0;
}

int save_writer_put(RFILE *file, int64_t offset, const void *data, size_t len, retro_atomic_int_t *failed)
{
	(void)file;
	put_calls++;
	mix(0x420);
	mix((u32)offset);
	mix((u32)len);
	mixbytes(data, len);
	if (put_never_taken)
		return 0;
	if (put_always_fails)
	{
		retro_atomic_store_release_int(failed, 1);
		return 1;
	}
	if (rn(5) == 0)
		return 0;	/* not taken */
	if (rn(4) == 0)
	{
		put_fails++;
		retro_atomic_store_release_int(failed, 1);
	}
	return 1;
}

int save_writer_put_file(const char *path, const void *data, size_t len, retro_atomic_int_t *failed)
{
	mix(0x421);
	mixbytes(path, strlen(path));
	mixbytes(data, len);
	if (rn(5) == 0)
		return 0;
	if (rn(4) == 0)
		retro_atomic_store_release_int(failed, 1);
	return 1;
}

void save_writer_drain(void)
{
	mix(0x422);
}

/* The card readers' files */
static bool card_files_there;

size_t periph_load_file(const char *suffix, void *buf, size_t size)
{
	size_t i;
	mix(0x430);
	mixbytes(suffix, strlen(suffix));
	if (!card_files_there)
		return 0;
	for (i = 0; i < size; i++)
		((u8 *)buf)[i] = (u8)rnd();
	return size;
}

void periph_save_file(const char *suffix, const void *buf, size_t size, retro_atomic_int_t *failed)
{
	(void)failed;
	mix(0x431);
	mixbytes(suffix, strlen(suffix));
	mixbytes(buf, size);
}

/* The I/O board bridge's world (jvs_bridge.c, jvs_board.c) */
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
	mix(0x440);
	mix((u32)kind);
	mixf(level);
	mix(duration_ms);
}

void naomi_periph_poll(void)
{
	mix(0x441);
}

bool naomi_cart_GetBootHeader(u8 *header, u32 size)
{
	(void)header;
	(void)size;
	return false;
}

/* The serializer: a run of bytes */
static u8 state[0x40000];

bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	memcpy(*dest, src, src_size);
	*dest = (u8 *)*dest + src_size;
	*total_size += src_size;
	return true;
}

bool ra_unserialize(void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	memcpy(src, *dest, src_size);
	*dest = (u8 *)*dest + src_size;
	*total_size += src_size;
	return true;
}

bool ra_unserialize_has(const void *data, size_t size)
{
	(void)data;
	(void)size;
	return true;
}

/* ---- What is made at random ---- */

static const u32 types_dc[] = {
	MDT_SegaController, MDT_SegaVMU, MDT_Microphone, MDT_PurupuruPack, MDT_AsciiStick,
	MDT_Keyboard, MDT_Mouse, MDT_LightGun, MDT_TwinStick, MDT_SegaVMU, MDT_PurupuruPack
};

static const char *const games[] = {
	"SOME GAME", "MKG TKOB 2 JPN VER2.001-", "MKG TKOB 4 JPN VER2.000-", "MKG TKOB", "VIRTUA FIGHTER 4",
	"MKG TKOX"
};

static InputDescriptors inputs;

static void make_inputs(void)
{
	int i;
	memset(&inputs, 0, sizeof(inputs));
	for (i = 0; i < 4; i++)
	{
		if (rn(3) == 0)
			continue;
		inputs.axes[i].name = "a";
		/* (an axis no stick nor trigger is for left the old one's answer to chance) */
		inputs.axes[i].axis = rn(6);
		inputs.axes[i].inverted = rn(2);
	}
	/* (names after a gap are read too) */
}

/* A frame for a device of @type: its command and what comes after */
static u32 make_frame(u32 *frame, u32 type, u32 addr)
{
	static const u8 common[] = { 0x01, 0x02, 0x03, 0x04, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x05, 0x77 };
	static const u8 rfid[] = { 0x90, 0x91, 0xA0, 0xA1, 0xD0, 0xD4, 0xC1, 0xD9, 0xDA, 0xB1, 0xB1, 0xD1, 0xD1, 0x82, 0x80, 0x80, 0x86, 0x84 };
	static const u32 funcs[] = { 0x02000000, 0x04000000, 0x08000000, 0x10000000, 0x00010000, 0, 0x12345678 };
	u8 *b = (u8 *)frame;
	u32 words = 0, cmd, i;
	memset(b, 0, 1100);
	if (type == MDT_RFIDReaderWriter && rn(4))
		cmd = rfid[rn(sizeof(rfid))];
	else if (type == MDT_NaomiJamma && rn(2))
	{
		/* the EEPROM written or read, through the bridge */
		b[0] = 0x86;
		b[4] = (u8)(rn(2) ? 0x0B : 0x03);
		b[5] = (u8)rn(0x80);
		b[6] = (u8)rn(0x40);
		for (i = 8; i < 8 + 0x40; i++)
			b[i] = (u8)rnd();
		return 4 + 4 + 0x40;
	}
	else
		cmd = common[rn(sizeof(common))];
	switch (cmd)
	{
	case 0x0A:	/* media info: a function, then a word */
	case 0x0B:	/* block read: a function, a block */
	case 0x0E:	/* set condition */
		frame[1] = funcs[rn(sizeof(funcs) / sizeof(funcs[0]))];
		frame[2] = rn(3) == 0 ? rnd() : rn(3) == 0 ? 0 : (rn(300) << 24) | (rn(4) << 8);
		words = 2;
		if (type == MDT_PurupuruPack)
			for (; words < 4; words++)
				frame[words + 1] = rnd();
		break;
	case 0x0C:	/* block write: a function, block and phase, data */
		frame[1] = funcs[rn(sizeof(funcs) / sizeof(funcs[0]))];
		frame[2] = (rn(260) << 24) | ((rn(8) == 0 ? rnd() & 0xff : rn(5)) << 8);
		words = 2 + (rn(3) == 0 ? rn(60) : rn(2) ? 32 : rn(2) ? 47 : 48);
		if (rn(10) == 0)
		{
			/* (a word past the card's end, or just to it) */
			frame[2] = (255u << 24) | (3 << 8);
			words = 2 + 32 + rn(2);
		}
		if (frame[1] == 0x08000000 && rn(2))
		{
			frame[2] = 0;
			words = 4;
		}
		for (i = 3; i <= words; i++)
			frame[i] = rnd();
		break;
	case 0x0F:	/* the microphone's control */
		frame[1] = rn(4) ? 0x10000000 : rnd();
		frame[2] = rn(5) == 0 ? rnd() : rn(5);
		if (rn(4) == 0)
			frame[2] = 0xfc;
		words = 2;
		break;
	case 0xB1:	/* a card written */
		b[4] = (u8)rn(40);
		b[5] = (u8)rn(40);
		for (i = 8; i < 4 + 4 * 40; i++)
			b[i] = (u8)rnd();
		words = 1 + rn(40);
		break;
	case 0xD1:
		b[4] = (u8)(rn(2) ? 0x03 << (2 * rn(4)) : rnd());
		words = rn(2);
		break;
	case 0x80:
		b[4] = (u8)rnd();
		b[5] = (u8)(rn(3) == 0 ? 0xff : rnd());
		for (i = 6; i < 4 + 0x20; i++)
			b[i] = (u8)rnd();
		words = 8;
		break;
	default:
		words = rn(4);
		for (i = 1; i <= words; i++)
			frame[i] = rnd();
		break;
	}
	/* cut short, now and then, or longer than its command */
	switch (rn(10))
	{
	case 0: words = rn(words + 1); break;
	case 1: words += rn(4); break;
	default: break;
	}
	frame[0] = cmd | (addr << 8) | ((rn(4) << 6 | 0x20) << 16) | (words << 24);
	if (rn(8) == 0)
		frame[0] = (frame[0] & ~0xff00u) | ((addr | 0x20) << 8);
	return 4 + words * 4;
}

/* A device's part of a state: written, mixed, read back now and then changed */
static void device_state(maple_device *dev)
{
	unsigned int total = 0, total2 = 0;
	void *data = state;
	u32 i, k;
	mix(maple_dev_serialize(dev, &data, &total));
	mix(total);
	mixbytes(state, total);
	if (rn(3) == 0)
		return;
	k = rn(6);
	for (i = 0; i < k && total > 0; i++)
	{
		/* (a block of a memory card's, or a byte) */
		if (total > 0x20000 && rn(2))
			memset(state + rn(256) * 512, (int)rnd(), 512);
		else
			state[rn(total)] = (u8)rnd();
	}
	if (maple_dev_type(dev) == MDT_NaomiJamma)
		state[0] &= 1;
	data = state;
	mix(maple_dev_unserialize(dev, &data, &total2));
	mix(total2);
}

static void rfid_states(void)
{
	unsigned int total = 0, total2 = 0;
	void *data = state;
	u32 i;
	rfid_serialize_v24(&data, &total);
	rfid_files_serialize_v30(&data, &total);
	mix(total);
	mixbytes(state, total);
	if (rn(2))
	{
		for (i = 0; i < 4 && total > 0; i++)
			state[rn(total)] = (u8)rnd();
		data = state;
		mix(rfid_unserialize_v24(&data, &total2));
		mix(rfid_files_unserialize_v30(&data, &total2));
		mix(total2);
	}
	else if (rn(4) == 0)
		rfid_state_before_v24();
}

static void make_device(u32 bus, u32 port, u32 type)
{
	maple_device *dev = maple_dev_create((MapleDeviceType)type);
	MapleDevices[bus][port] = dev;
	if (dev == NULL)
		return;
	maple_dev_setup(dev, maple_GetAddress(bus, port), &maps[bus][port]);
	mix(maple_dev_type(dev));
	mix(maple_dev_bus_id(dev));
	mix(vmu_screen_params[bus].vmu_lcd_screen != NULL);
}

/* A block of the card at bus 0's first slot written: block @block, @words words */
static void vmu_write(u32 block, u32 words)
{
	static u32 in[1100 / 4], out[4096 / 4];
	u32 i, n;
	in[0] = 0x0C | (maple_GetAddress(0, 0) << 8) | (0x20 << 16) | ((2 + words) << 24);
	in[1] = 0x02000000;
	in[2] = block << 24;
	for (i = 0; i < words; i++)
		in[3 + i] = rnd();
	n = maple_dev_dma(MapleDevices[0][0], in, 4 + (2 + words) * 4, out);
	mix(n);
	mixbytes(out, n);
}

/* A card whose file goes on failing to be written, and then is not
 * taken: when it is tried again, and what is written then, with the
 * card's state loaded in between */
static void failing_card(void)
{
	u32 f;
	settings.System = DC_PLATFORM_DREAMCAST;
	vmu_file_there = true;
	memset(MapleDevices, 0, sizeof(MapleDevices));
	make_device(0, 5, MDT_SegaController);
	make_device(0, 0, MDT_SegaVMU);
	for (f = 0; f < 9000; f++)
	{
		put_always_fails = f < 7000;
		put_never_taken = f >= 7000 && f < 8800;
		if (f % 37 == 0)
		{
			vmu_write(rn(256), 32);
			vmu_write(rn(256), 32);
		}
		/* (two loads in a row, the second's change before or after the first's) */
		if (f >= 7700 && (f % 230 == 100 || f % 230 == 101))
			device_state(MapleDevices[0][0]);
		{
			const u32 calls = put_calls;
			maple_dev_flush_save(MapleDevices[0][0]);
			/* (gone with a failure not yet seen, now and then: written at once all the same) */
			if (f < 250 && put_calls != calls && f % 3 == 0)
			{
				void *at = maple_dev_destruct(MapleDevices[0][0]);
				MapleDevices[0][0] = maple_dev_create_at(MDT_SegaVMU, at);
				maple_dev_setup(MapleDevices[0][0], maple_GetAddress(0, 0), &maps[0][0]);
			}
		}
	}
	put_always_fails = false;
	put_never_taken = false;
	maple_dev_delete(MapleDevices[0][0]);
	maple_dev_delete(MapleDevices[0][5]);
	memset(MapleDevices, 0, sizeof(MapleDevices));
	printf("failing card %08x\n", hash);
}

int main(int argc, char **argv)
{
	const u32 machines = argc > 1 ? (u32)atoi(argv[1]) : 600;
	static u32 in[1100 / 4], out[4096 / 4];
	u32 c, f, bus, port, i;
	for (c = 0; c < machines; c++)
	{
		u32 frames;
		const u32 sys = rn(3);
		settings.System = sys == 0 ? DC_PLATFORM_DREAMCAST : sys == 1 ? DC_PLATFORM_ATOMISWAVE : DC_PLATFORM_NAOMI;
		settings.mapping.JammaSetup = JVS_Default;
		strcpy(naomi_game_id, games[rn(sizeof(games) / sizeof(games[0]))]);
		make_inputs();
		naomi_game_inputs = rn(3) == 0 ? NULL : &inputs;
		vmu_file_there = rn(3) != 0;
		vmu_file_made = rn(4) != 0;
		card_files_there = rn(2) != 0;
		RealTimeClock = rnd();
		kb_shift = (u8)rnd();
		kb_led = (u8)rnd();
		for (i = 0; i < 6; i++)
			kb_key[i] = (u8)rnd();

		/* the buses: a NAOMI's I/O board bridge and its card readers, or what plugs into a console */
		memset(MapleDevices, 0, sizeof(MapleDevices));
		for (bus = 0; bus < 4; bus++)
		{
			if (sys == 2)
			{
				if (bus == 0)
					make_device(bus, 5, MDT_NaomiJamma);
				else if (bus < 3 && rn(3))
					make_device(bus, 5, MDT_RFIDReaderWriter);
				else if (bus == 3 && rn(3) == 0)
					make_device(bus, 5, MDT_SegaController);
				continue;
			}
			if (rn(5) == 0)
				continue;
			make_device(bus, 5, types_dc[rn(sizeof(types_dc) / sizeof(types_dc[0]))]);
			for (port = 0; port < 2; port++)
				if (rn(2))
					make_device(bus, port, rn(2) ? MDT_SegaVMU : rn(2) ? MDT_PurupuruPack : MDT_Microphone);
		}
		rfid_init(sys == 2 && rn(4) != 0, rn(2) != 0);
		if (rn(20) == 0)
			mix(maple_dev_create((MapleDeviceType)MDT_None) == NULL);

		frames = rn(5) == 0 ? 300 + rn(300) : 30 + rn(60);
		for (f = 0; f < frames; f++)
		{
			maple_device *dev;
			u32 len, n;
			bus = rn(4);
			port = rn(3) == 0 ? rn(2) : 5;
			dev = MapleDevices[bus][port];
			if (dev == NULL)
				continue;
			switch (rn(24))
			{
			case 0:
				device_state(dev);
				break;
			case 1:
				maple_dev_flush_save(dev);
				mix(0x500);
				break;
			case 2:
				mix(maple_dev_lightgun_pos(dev));
				mix(maple_dev_has_slots(dev));
				break;
			case 3:
				/* gone and made again in its own memory, as a state load does */
				{
					const MapleDeviceType type = maple_dev_type(dev);
					void *at = maple_dev_destruct(dev);
					dev = maple_dev_create_at(type, at);
					MapleDevices[bus][port] = dev;
					maple_dev_setup(dev, maple_GetAddress(bus, port), &maps[bus][port]);
				}
				break;
			case 4:
				if (rn(3) == 0)
					vmu_path_gen++;
				mix(maple_vmu_file_changed(dev));
				break;
			case 5:
				insertRfidCard((int)rn(3) - (rn(5) == 0));
				{
					const u8 *card = getRfidCardData((int)rn(2));
					mix(card != NULL);
					if (card != NULL)
						mixbytes(card, 128);
				}
				break;
			case 6:
				rfid_states();
				break;
			default:
				len = make_frame(in, maple_dev_type(dev), maple_GetAddress(bus, port));
				memset(out, 0xcc, sizeof(out));
				n = maple_dev_dma(dev, in, len, out);
				mix(n);
				mixbytes(out, n);
				break;
			}
			/* (between frames, as the core flushes them: what was written goes out) */
			if (rn(2))
				for (bus = 0; bus < 4; bus++)
					for (port = 0; port < 6; port++)
						if (MapleDevices[bus][port] != NULL)
							maple_dev_flush_save(MapleDevices[bus][port]);
			if (rn(40) == 0)
			{
				unsigned int total = 0;
				void *data = state;
				EEPROM_loaded = rn(2) != 0;
				maple_naomi_eeprom_load();
				maple_jvs_serialize_v27(&data, &total);
				mix(total);
				mixbytes(state, total);
			}
		}
		/* gone: what is waiting written out */
		for (bus = 0; bus < 4; bus++)
			for (port = 0; port < 6; port++)
				if (MapleDevices[bus][port] != NULL)
				{
					maple_dev_flush_save(MapleDevices[bus][port]);
					maple_dev_delete(MapleDevices[bus][port]);
				}
		mix(put_fails);
		if (c % 50 == 49)
			printf("%u %08x\n", c + 1, hash);
	}
	failing_card();
	printf("maple_devs_test: %u machines %08x\n", machines, hash);
	return 0;
}
