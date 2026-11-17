/* The devices maple_cfg_test.c plugs in: each says what is done to it into
 * the test's hash, and asks for its inputs as a device does. Built with
 * MAPLE_CFG_TEST_OLD for the C++ maple_cfg.cpp there was (its devices a
 * maple_device's class), without for maple_cfg.c (its devices through
 * maple_if.h's functions). */
#include <stdlib.h>
#include <string.h>
#include <new>
#include "types.h"
#include "hw/maple/maple_if.h"
#include "hw/maple/maple_helper.h"
#include "hw/maple/maple_cfg.h"
#include "libretro/input_latch.h"

extern "C" {
void t_UpdateInputState(u32 port);
void t_UpdateInputStateMouse(u32 port);
void t_UpdateInputStateArcade(void);
void t_UpdateVibration(u32 port, u32 value, u32 max_duration);
void t_maple_naomi_eeprom_load(void);
void mix(u32 v);
void mixf(f32 f);
u32 rnd(void);
}

static u32 made;

static void say_input(const PlainJoystickState *pjs)
{
	mix(0x300);
	mix(pjs->kcode);
	mix(pjs->joy[0] | pjs->joy[1] << 8 | pjs->joy[2] << 16 | (u32)pjs->joy[3] << 24);
	mix(pjs->trigger[0] | pjs->trigger[1] << 8);
}

#ifdef MAPLE_CFG_TEST_OLD

struct FakeDev : maple_device
{
	MapleDeviceType type;
	u32 id;
	bool changed;
	FakeDev(MapleDeviceType t, bool in_place) : type(t), id(made++), changed(false) { config = NULL; mix(0x200); mix(t); mix(id); mix(in_place); mix(EEPROM_loaded); }
	/* (a NAOMI's board, going, forgets that the EEPROM was read) */
	~FakeDev() { mix(0x201); mix(id); if (type == MDT_NaomiJamma) EEPROM_loaded = false; }
	void OnSetup() override { mix(0x202); mix(id); }
	u32 RawDma(u32 *, u32, u32 *) override { return 0; }
	MapleDeviceType get_device_type() override { return type; }
	bool maple_serialize(void **data, unsigned int *total_size) override
	{
		u8 **p = (u8 **)data;
		mix(0x203); mix(id);
		if (*p != NULL)
		{
			(*p)[0] = (u8)type;
			(*p)[1] = (u8)id;
			*p += 2;
		}
		*total_size += 2;
		return true;
	}
	bool maple_unserialize(void **data, unsigned int *total_size) override
	{
		u8 *p = (u8 *)*data;
		mix(0x204); mix(id); mix(p[0] | p[1] << 8);
		*data = p + 2;
		*total_size += 2;
		return true;
	}
};

void maple_device::Setup(u32 prt)
{
	maple_port = prt;
	bus_port = maple_GetPort(prt);
	bus_id = maple_GetBusId(prt);
	mix(0x205); mix(prt);
}

maple_device::~maple_device()
{
}

maple_device *maple_CreateAt(MapleDeviceType type, void *at)
{
	return at != NULL ? new (at) FakeDev(type, true) : new FakeDev(type, false);
}

maple_device *maple_Create(MapleDeviceType type)
{
	return maple_CreateAt(type, NULL);
}

bool maple_vmu_file_changed(maple_device *dev)
{
	return ((FakeDev *)dev)->changed;
}

InputLatch input_latch;

void UpdateInputState(u32 port) { t_UpdateInputState(port); }
void UpdateInputStateMouse(u32 port) { t_UpdateInputStateMouse(port); }
void UpdateInputStateArcade(void) { t_UpdateInputStateArcade(); }
void UpdateVibration(u32 port, u32 value, u32 max_duration) { t_UpdateVibration(port, value, max_duration); }
void maple_naomi_eeprom_load(void) { t_maple_naomi_eeprom_load(); }

extern "C" {
MapleDeviceType maple_dev_type(maple_device *dev) { return dev->get_device_type(); }
void old_mcfg_Create(MapleDeviceType type, u32 bus, u32 port, int player_num) { mcfg_Create(type, bus, port, player_num); }
void old_mcfg_DestroyDevice(int i, int j) { mcfg_DestroyDevice(i, j); }

void tdev_input(maple_device *dev)
{
	PlainJoystickState pjs;
	dev->config->GetInput(&pjs);
	say_input(&pjs);
}

void tdev_mouse(maple_device *dev)
{
	u32 b;
	f32 x, y, w;
	dev->config->GetMouse(&b, &x, &y, &w);
	mix(0x301); mix(b); mixf(x); mixf(y); mixf(w);
}

void tdev_abs(maple_device *dev)
{
	f32 x, y;
	dev->config->GetAbsolutePosition(&x, &y);
	mix(0x302); mixf(x); mixf(y);
}

void tdev_image(maple_device *dev, void *img) { dev->config->SetImage(img); }
void tdev_vibration(maple_device *dev, u32 value, u32 duration) { dev->config->SetVibration(value, duration); }
void tdev_changed(maple_device *dev, bool changed) { ((FakeDev *)dev)->changed = changed; }

void tlatch_publish(const u32 *kcodes, const s8 *joy, const u8 *trig, const u32 *mouse)
{
	InputLatch::State& next = input_latch.Next();
	for (int p = 0; p < 4; p++)
	{
		InputLatch::Port& s = next.port[p];
		s.kcode = kcodes[p];
		s.joyx = joy[p * 4]; s.joyy = joy[p * 4 + 1]; s.joyrx = joy[p * 4 + 2]; s.joyry = joy[p * 4 + 3];
		s.rt = trig[p * 2]; s.lt = trig[p * 2 + 1];
		s.mo_buttons = mouse[p * 4]; s.mo_x = mouse[p * 4 + 1]; s.mo_y = mouse[p * 4 + 2]; s.mo_wheel = mouse[p * 4 + 3];
	}
	input_latch.Publish();
}
}

#else

struct FakeDev
{
	MapleDeviceType type;
	u32 id;
	bool changed;
	u32 bus_id;
	mcfg_map_t *config;
};

static FakeDev *fake_make(MapleDeviceType type, void *at)
{
	FakeDev *d = (FakeDev *)(at != NULL ? at : malloc(sizeof(FakeDev)));
	d->type = type;
	d->id = made++;
	d->changed = false;
	d->config = NULL;
	mix(0x200); mix(type); mix(d->id); mix(at != NULL); mix(EEPROM_loaded);
	return d;
}

InputLatch input_latch;
static const bool input_latch_made = (input_latch_init(&input_latch), true);

extern "C" {
maple_device *maple_dev_create(MapleDeviceType type) { return (maple_device *)fake_make(type, NULL); }
maple_device *maple_dev_create_at(MapleDeviceType type, void *at) { return (maple_device *)fake_make(type, at); }

void *maple_dev_destruct(maple_device *dev)
{
	mix(0x201); mix(((FakeDev *)dev)->id);
	if (((FakeDev *)dev)->type == MDT_NaomiJamma)
		EEPROM_loaded = false;
	return dev;
}

void maple_dev_delete(maple_device *dev)
{
	if (dev == NULL)
		return;
	maple_dev_destruct(dev);
	free(dev);
}

void maple_dev_setup(maple_device *dev, u32 addr, struct mcfg_map *config)
{
	FakeDev *d = (FakeDev *)dev;
	d->bus_id = maple_GetBusId(addr);
	mix(0x205); mix(addr);
	d->config = config;
	mix(0x202); mix(d->id);
}

MapleDeviceType maple_dev_type(maple_device *dev) { return ((FakeDev *)dev)->type; }
u32 maple_dev_bus_id(const maple_device *dev) { return ((const FakeDev *)dev)->bus_id; }

bool maple_dev_serialize(maple_device *dev, void **data, unsigned int *total_size)
{
	FakeDev *d = (FakeDev *)dev;
	u8 **p = (u8 **)data;
	mix(0x203); mix(d->id);
	if (*p != NULL)
	{
		(*p)[0] = (u8)d->type;
		(*p)[1] = (u8)d->id;
		*p += 2;
	}
	*total_size += 2;
	return true;
}

bool maple_dev_unserialize(maple_device *dev, void **data, unsigned int *total_size)
{
	u8 *p = (u8 *)*data;
	mix(0x204); mix(((FakeDev *)dev)->id); mix(p[0] | p[1] << 8);
	*data = p + 2;
	*total_size += 2;
	return true;
}

bool maple_vmu_file_changed(maple_device *dev) { return ((FakeDev *)dev)->changed; }

void tdev_input(maple_device *dev)
{
	PlainJoystickState pjs;
	pjs_init(&pjs);
	mcfg_get_input(((FakeDev *)dev)->config, &pjs);
	say_input(&pjs);
}

void tdev_mouse(maple_device *dev)
{
	u32 b;
	f32 x, y, w;
	mcfg_get_mouse(((FakeDev *)dev)->config, &b, &x, &y, &w);
	mix(0x301); mix(b); mixf(x); mixf(y); mixf(w);
}

void tdev_abs(maple_device *dev)
{
	f32 x, y;
	mcfg_get_absolute_position(((FakeDev *)dev)->config, &x, &y);
	mix(0x302); mixf(x); mixf(y);
}

void tdev_image(maple_device *dev, void *img) { mcfg_set_image(((FakeDev *)dev)->config, img); }
void tdev_vibration(maple_device *dev, u32 value, u32 duration) { mcfg_set_vibration(((FakeDev *)dev)->config, value, duration); }
void tdev_changed(maple_device *dev, bool changed) { ((FakeDev *)dev)->changed = changed; }

void tlatch_publish(const u32 *kcodes, const s8 *joy, const u8 *trig, const u32 *mouse)
{
	input_latch_state *next = input_latch_next(&input_latch);
	for (int p = 0; p < 4; p++)
	{
		input_latch_port *s = &next->port[p];
		s->kcode = kcodes[p];
		s->joyx = joy[p * 4]; s->joyy = joy[p * 4 + 1]; s->joyrx = joy[p * 4 + 2]; s->joyry = joy[p * 4 + 3];
		s->rt = trig[p * 2]; s->lt = trig[p * 2 + 1];
		s->mo_buttons = mouse[p * 4]; s->mo_x = mouse[p * 4 + 1]; s->mo_y = mouse[p * 4 + 2]; s->mo_wheel = mouse[p * 4 + 3];
	}
	input_latch_publish(&input_latch);
}
}
#endif
