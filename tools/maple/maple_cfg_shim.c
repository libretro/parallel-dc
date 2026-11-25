/* The devices maple_cfg_test.c plugs in: each says what is done to it into
 * the test's hash, and asks for its inputs as a device does (maple_if.h's
 * functions). */
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/maple/maple_if.h"
#include "hw/maple/maple_helper.h"
#include "hw/maple/maple_cfg.h"
#include "libretro/input_latch.h"

void mix(u32 v);
void mixf(f32 f);

static u32 made;

static void say_input(const PlainJoystickState *pjs)
{
	mix(0x300);
	mix(pjs->kcode);
	mix(pjs->joy[0] | pjs->joy[1] << 8 | pjs->joy[2] << 16 | (u32)pjs->joy[3] << 24);
	mix(pjs->trigger[0] | pjs->trigger[1] << 8);
}

typedef struct FakeDev
{
	MapleDeviceType type;
	u32 id;
	bool changed;
	u32 bus_id;
	mcfg_map_t *config;
} FakeDev;

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

void tlatch_init(void) { input_latch_init(&input_latch); }

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
	int p;
	for (p = 0; p < 4; p++)
	{
		input_latch_port *s = &next->port[p];
		s->kcode = kcodes[p];
		s->joyx = joy[p * 4]; s->joyy = joy[p * 4 + 1]; s->joyrx = joy[p * 4 + 2]; s->joyry = joy[p * 4 + 3];
		s->rt = trig[p * 2]; s->lt = trig[p * 2 + 1];
		s->mo_buttons = mouse[p * 4]; s->mo_x = mouse[p * 4 + 1]; s->mo_y = mouse[p * 4 + 2]; s->mo_wheel = mouse[p * 4 + 3];
	}
	input_latch_publish(&input_latch);
}
