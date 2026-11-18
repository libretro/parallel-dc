#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The pads as the frontend last gave them, by port (libretro.cpp): the
 * buttons (a bit clear for each pressed), the triggers and the sticks */
extern u32 kcode[4];
extern u8 rt[4];
extern u8 lt[4];
extern s8 joyx[4], joyy[4];
extern s8 joyrx[4], joyry[4];
/* Where each port's light gun or mouse points on the screen, 0-639 by
 * 0-479 and outside it when it is off the picture (maple_devs.c) */
extern f32 mo_x_abs[4];
extern f32 mo_y_abs[4];
/* A mouse's buttons (a bit clear for each pressed) and its motion not yet
 * read by the game (maple_devs.c) */
extern u32 mo_buttons[4];
extern f32 mo_x_delta[4];
extern f32 mo_y_delta[4];
extern f32 mo_wheel_delta[4];
/* The keyboard's shift keys pressed (a bit each), its leds lit, and up to
 * six other keys pressed (maple_devs.c) */
extern u8 kb_shift;
extern u8 kb_led;
extern u8 kb_key[6];
/* Whether a controller has a vibration pack in its second slot (libretro.cpp) */
extern bool enable_purupuru;

/* Without threaded rendering, the frontend's input read for a port as the
 * game asks for it, for its mouse, and an arcade machine's ports all
 * together (libretro.cpp) */
void UpdateInputState(u32 port);
void UpdateInputStateMouse(u32 port);
void UpdateInputStateArcade(void);
/* A port's vibration pack set going */
void UpdateVibration(u32 port, u32 value, u32 max_duration);

#ifdef __cplusplus
}
#endif
