/* The frontend's input as the machine has it (lr_input.c): the pads,
 * sticks, triggers, guns, mice and keyboard read from the frontend for
 * each port, the vibration packs and the NAOMI wheel's force feedback run
 * on its rumble, and the input's part of a state. libretro.cpp says how
 * (the settings below) and when. C89, for C and C++ alike. */
#ifndef LR_INPUT_H
#define LR_INPUT_H
#include <libretro.h>
#include "types.h"
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define RETRO_DEVICE_TWINSTICK				RETRO_DEVICE_SUBCLASS( RETRO_DEVICE_JOYPAD, 1 )
#define RETRO_DEVICE_TWINSTICK_SATURN		RETRO_DEVICE_SUBCLASS( RETRO_DEVICE_JOYPAD, 2 )
#define RETRO_DEVICE_ASCIISTICK				RETRO_DEVICE_SUBCLASS( RETRO_DEVICE_JOYPAD, 3 )

/* The frontend's input, polled once a frame (libretro.cpp) */
extern retro_input_state_t input_cb;
/* Its rumble, where it has any */
extern struct retro_rumble_interface lr_rumble;
/* What each port is set to (RETRO_DEVICE_*) */
extern int lr_device_type[4];
/* The sticks' and the triggers' dead zones, out of 0x8000 */
extern int astick_deadzone;
extern int trigger_deadzone;
/* The triggers read as buttons */
extern bool digital_triggers;
/* An arcade machine's test and service buttons on L3 and R3 */
extern bool allow_service_buttons;
/* The frontend reads a pad's buttons all at once */
extern bool libretro_supports_bitmasks;
/* "NAOMI Wheel Force Feedback" */
extern bool naomi_ffb_enabled;
/* The picture is wide because a cheat makes it so, not the widescreen hack */
extern bool wide_by_cheat;
/* An OpenGL context is being made again: nothing is read */
extern bool gl_ctx_resetting;
/* Without threaded rendering: whether an arcade machine's ports have been
 * read from the frontend since the last frame ended */
extern bool arcade_input_read;
/* Without threaded rendering, a Dreamcast's mice, a bit for each port: the
 * ports the game has asked a mouse on, and those of them read from the
 * frontend since the last frame ended */
extern u32 mouse_ports, mouse_ports_read;

/* The keyboard's table, and the latch made once (retro_init()) */
void lr_input_init(void);
void input_set_deadzone_stick(int percent);
void input_set_deadzone_trigger(int percent);
/* A port's button @id of a frontend device as the machine's key bit, 0 for none */
uint32_t map_gamepad_button(unsigned device, unsigned id);
/* The wheel's force feedback asked for no longer (the machine is stopped) */
void naomi_ffb_clear(void);
/* Every motor stopped, and nothing waiting to run it down later */
void rumble_stop(void);
/* At each vblank, on the emulation thread: the machine's time moves on for
 * the rumble's durations */
void lr_input_vblank(void);
/* All four ports sampled for the emulation thread (threaded rendering) */
void CaptureInput(void);
void retro_keyboard_event(bool down, unsigned keycode, uint32_t character, uint16_t key_modifiers);

RETRO_END_DECLS

#endif
