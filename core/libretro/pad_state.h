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
 * 0-479 and outside it when it is off the picture (maple_devs.cpp) */
extern f32 mo_x_abs[4];
extern f32 mo_y_abs[4];

#ifdef __cplusplus
}
#endif
