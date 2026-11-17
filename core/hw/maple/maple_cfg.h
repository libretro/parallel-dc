#ifndef MAPLE_CFG_H
#define MAPLE_CFG_H
#include "types.h"
#include "maple_devs.h"
#include "maple_if.h"
#include "maple_state.h"
#include <retro_inline.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

enum PlainJoystickButtonId
{
	PJBI_B = 1,
	PJBI_A = 2,
	PJBI_START = 3,
	PJBI_DPAD_UP = 4,
	PJBI_DPAD_DOWN = 5,
	PJBI_DPAD_LEFT = 6,
	PJBI_DPAD_RIGHT = 7,
	PJBI_Y = 9,
	PJBI_X = 10,

	PJBI_Count=16
};

enum PlainJoystickAxisId
{
	PJAI_X1 = 0,
	PJAI_Y1 = 1,
	PJAI_X2 = 2,
	PJAI_Y2 = 3,

	PJAI_Count = 4
};

enum PlainJoystickTriggerId
{
	PJTI_L = 0,
	PJTI_R = 1,

	PJTI_Count = 2
};

typedef struct PlainJoystickState
{
	u32 kcode;

	u8 joy[PJAI_Count];
	u8 trigger[PJTI_Count];
} PlainJoystickState;

/* Nothing pressed, the sticks in the middle */
static INLINE void pjs_init(PlainJoystickState *pjs)
{
	pjs->kcode = 0xFFFFFFFF;
	pjs->joy[0] = pjs->joy[1] = pjs->joy[2] = pjs->joy[3] = 0x80;
	pjs->trigger[0] = pjs->trigger[1] = 0;
}

/* Where a device gets its inputs and puts its outputs: the player's, by
 * the bus it is on or the one it was made for (each slot of each bus has
 * one: maple_cfg.c) */
typedef struct mcfg_map
{
	maple_device *dev;
	int player_num;		/* -1: the bus's */
} mcfg_map_t;

void mcfg_set_vibration(const mcfg_map_t *map, u32 value, u32 max_duration);
void mcfg_get_input(const mcfg_map_t *map, PlainJoystickState *pjs);
void mcfg_get_absolute_position(const mcfg_map_t *map, f32 *px, f32 *py);
void mcfg_get_mouse(const mcfg_map_t *map, u32 *buttons, f32 *delta_x, f32 *delta_y, f32 *delta_wheel);
void mcfg_set_image(const mcfg_map_t *map, void *img);

/* Whether a device has expansion slots for others (a memory card, a
 * vibration pack). */
static INLINE bool maple_HasSlots(MapleDeviceType type)
{
	switch (type)
	{
	case MDT_SegaController:
	case MDT_LightGun:
	case MDT_TwinStick:
	case MDT_AsciiStick:
		return true;
	default:
		return false;
	}
}

/* @player_num -1: the bus's player's */
void mcfg_Create(MapleDeviceType type, u32 bus, u32 port, int player_num);
void mcfg_DestroyDevice(int i, int j);

RETRO_END_DECLS

#endif
