/* The frontend's input as the machine has it: see lr_input.h */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <retro_atomic.h>
#include <retro_miscellaneous.h>
#include "lr_input.h"
#include "pad_state.h"
#include "input_latch.h"
#include "keyboard_map.h"
#include "types.h"
#include "serialize.h"
#include "oslib/oslib.h"
#include "oslib/audiostream.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_devs.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/naomi_periph.h"
#include "hw/naomi/midiffb.h"
#include "log/Log.h"

retro_input_state_t input_cb = NULL;
struct retro_rumble_interface lr_rumble;
int lr_device_type[4] = {-1,-1,-1,-1};
int astick_deadzone = 0;
int trigger_deadzone = 0;
bool digital_triggers = false;
bool allow_service_buttons = false;
bool libretro_supports_bitmasks = false;
bool naomi_ffb_enabled = false;
bool wide_by_cheat;
bool gl_ctx_resetting = false;
bool arcade_input_read;
u32 mouse_ports, mouse_ports_read;

enum DreamcastController
{
   DC_BTN_C       = 1,
   DC_BTN_B       = 1<<1,
   DC_BTN_A       = 1<<2,
   DC_BTN_START   = 1<<3,
   DC_DPAD_UP     = 1<<4,
   DC_DPAD_DOWN   = 1<<5,
   DC_DPAD_LEFT   = 1<<6,
   DC_DPAD_RIGHT  = 1<<7,
   DC_BTN_Z       = 1<<8,
   DC_BTN_Y       = 1<<9,
   DC_BTN_X       = 1<<10,
   DC_BTN_D       = 1<<11,
   DC_DPAD2_UP    = 1<<12,
   DC_DPAD2_DOWN  = 1<<13,
   DC_DPAD2_LEFT  = 1<<14,
   DC_DPAD2_RIGHT = 1<<15
};

static u32 vib_stop_time[4];
static double vib_strength[4];
static double vib_delta[4];

/* A NAOMI wheel's force feedback (midiffb.c, and the drive boards of
 * jvs_board.c), as rumble on the first port when "NAOMI Wheel Force
 * Feedback" is on: the force on the strong motor, a shake on the weak one
 * for as long as it lasts. The emulator says what it wants in one word,
 * which the frontend's side passes on when it changes:
 *   bits 0-7 the force, 8-15 the shake, 16-23 how long the shake lasts in
 *   10 ms, 24-30 counted up by each shake (one like the last one too) */
static int naomi_ffb_word;			/* the emulator's side */
static retro_atomic_int_t naomi_ffb_request;
static int naomi_ffb_applied;		/* the frontend's side */
static uint32_t naomi_ffb_shake_stop;
static uint32_t get_time_ms(void);

/* round()'s, half away from zero, for C89 */
static double lr_round(double x)
{
   double r;
   if (x < 0)
      return -lr_round(-x);
   r = floor(x);
   return x - r >= 0.5 ? r + 1 : r;
}

void naomi_ffb_rumble(int kind, float level, u32 duration_ms)
{
   const int v = level <= 0.f ? 0 : level >= 1.f ? 255 : (int)(level * 255.f + 0.5f);
   int word = naomi_ffb_word;

   switch (kind)
   {
   case NAOMI_FFB_FORCE:
      word = (word & ~0xff) | v;
      break;
   case NAOMI_FFB_SHAKE:
      {
         const int ticks = duration_ms / 10 < 255 ? duration_ms / 10 : 255;
         word = (word & 0xff) | (v << 8) | (ticks << 16) | ((((word >> 24) + 1) & 0x7f) << 24);
      }
      break;
   default:
      word = 0;
      break;
   }
   if (word != naomi_ffb_word)
   {
      naomi_ffb_word = word;
      retro_atomic_store_release_int(&naomi_ffb_request, word);
   }
}

void naomi_ffb_clear(void)
{
   naomi_ffb_word = 0;
   retro_atomic_store_release_int(&naomi_ffb_request, 0);
}

/* From the first port's input update: a load and a test a frame */
static void naomi_ffb_update(void)
{
   int word;
   if (!lr_rumble.set_rumble_state)
      return;
   word = naomi_ffb_enabled ? retro_atomic_load_acquire_int(&naomi_ffb_request) : 0;
   if (word != naomi_ffb_applied)
   {
      if ((word ^ naomi_ffb_applied) & 0xff)
         lr_rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, (word & 0xff) * 257);
      if ((word ^ naomi_ffb_applied) & ~0xff)
      {
         const int weak = (word >> 8) & 0xff;
         lr_rumble.set_rumble_state(0, RETRO_RUMBLE_WEAK, weak * 257);
         naomi_ffb_shake_stop = weak != 0 ? (get_time_ms() + ((word >> 16) & 0xff) * 10) | 1 : 0;
      }
      naomi_ffb_applied = word;
   }
   if (naomi_ffb_shake_stop != 0 && (int32_t)(get_time_ms() - naomi_ffb_shake_stop) >= 0)
   {
      naomi_ffb_shake_stop = 0;
      lr_rumble.set_rumble_state(0, RETRO_RUMBLE_WEAK, 0);
   }
}

/* Every motor stopped, and nothing waiting to run it down later: the
 * devices that ran them are gone, or the machine has been reset. */
void rumble_stop(void)
{
   int i;
   naomi_ffb_applied = 0;
   naomi_ffb_shake_stop = 0;
   if (!lr_rumble.set_rumble_state)
      return;
   for (i = 0; i < MAPLE_PORTS; i++)
   {
      vib_stop_time[i] = 0;
      lr_rumble.set_rumble_state(i, RETRO_RUMBLE_STRONG, 0);
      lr_rumble.set_rumble_state(i, RETRO_RUMBLE_WEAK,   0);
   }
}



void input_set_deadzone_stick(int percent)
{
   if ( percent >= 0 && percent <= 100 )
      astick_deadzone = (int)( percent * 0.01f * 0x8000);
}

void input_set_deadzone_trigger(int percent)
{
   if ( percent >= 0 && percent <= 100 )
      trigger_deadzone = (int)( percent * 0.01f * 0x8000);
}


uint32_t map_gamepad_button(unsigned device, unsigned id)
{
   static const uint32_t dc_joymap[] =
   {
      /* JOYPAD_B      */ DC_BTN_A,
      /* JOYPAD_Y      */ DC_BTN_X,
      /* JOYPAD_SELECT */ 0,
      /* JOYPAD_START  */ DC_BTN_START,
      /* JOYPAD_UP     */ DC_DPAD_UP,
      /* JOYPAD_DOWN   */ DC_DPAD_DOWN,
      /* JOYPAD_LEFT   */ DC_DPAD_LEFT,
      /* JOYPAD_RIGHT  */ DC_DPAD_RIGHT,
      /* JOYPAD_A      */ DC_BTN_B,
      /* JOYPAD_X      */ DC_BTN_Y,
   };

   static const uint32_t dc_lg_joymap[] =
   {
	  /* deprecated */ 			0,
	  /* deprecated */ 			0,
	  /* LIGHTGUN_TRIGGER */	DC_BTN_A,
	  /* LIGHTGUN_AUX_A */		DC_BTN_B,
	  /* LIGHTGUN_AUX_B */ 		0,
	  /* deprecated */ 			0,
	  /* LIGHTGUN_START */		DC_BTN_START,
	  /* LIGHTGUN_SELECT */ 	0,
	  /* LIGHTGUN_AUX_C */		0,
	  /* LIGHTGUN_UP   */ 		DC_DPAD_UP,
	  /* LIGHTGUN_DOWN   */ 	DC_DPAD_DOWN,
	  /* LIGHTGUN_LEFT   */ 	DC_DPAD_LEFT,
	  /* LIGHTGUN_RIGHT  */ 	DC_DPAD_RIGHT,
   };

   static const uint32_t aw_joymap[] =
   {
      /* JOYPAD_B      */ AWAVE_BTN0_KEY, /* BTN1 */
      /* JOYPAD_Y      */ AWAVE_BTN2_KEY, /* BTN3 */
      /* JOYPAD_SELECT */ AWAVE_COIN_KEY,
      /* JOYPAD_START  */ AWAVE_START_KEY,
      /* JOYPAD_UP     */ AWAVE_UP_KEY,
      /* JOYPAD_DOWN   */ AWAVE_DOWN_KEY,
      /* JOYPAD_LEFT   */ AWAVE_LEFT_KEY,
      /* JOYPAD_RIGHT  */ AWAVE_RIGHT_KEY,
      /* JOYPAD_A      */ AWAVE_BTN1_KEY, /* BTN2 */
      /* JOYPAD_X      */ AWAVE_BTN3_KEY, /* BTN4 */
      /* JOYPAD_L      */ 0,
      /* JOYPAD_R      */ AWAVE_BTN4_KEY, /* BTN5 */
      /* JOYPAD_L2     */ 0,
      /* JOYPAD_R2     */ 0,
      /* JOYPAD_L3     */ AWAVE_TEST_KEY,
      /* JOYPAD_R3     */ AWAVE_SERVICE_KEY,
   };

   static const uint32_t aw_lg_joymap[] =
   {
	   /* deprecated */ 			0,
	   /* deprecated */ 			0,
	   /* LIGHTGUN_TRIGGER */	AWAVE_TRIGGER_KEY,
	   /* LIGHTGUN_AUX_A */		AWAVE_BTN0_KEY,
	   /* LIGHTGUN_AUX_B */ 	AWAVE_BTN1_KEY,
	   /* deprecated */ 			0,
	   /* LIGHTGUN_START */		AWAVE_START_KEY,
	   /* LIGHTGUN_SELECT */ 	AWAVE_COIN_KEY,
	   /* LIGHTGUN_AUX_C */		AWAVE_BTN2_KEY,
	   /* LIGHTGUN_UP   */ 		AWAVE_UP_KEY,
	   /* LIGHTGUN_DOWN   */ 	AWAVE_DOWN_KEY,
	   /* LIGHTGUN_LEFT   */ 	AWAVE_LEFT_KEY,
	   /* LIGHTGUN_RIGHT  */ 	AWAVE_RIGHT_KEY,
   };

   static const uint32_t nao_joymap[] =
   {
      /* JOYPAD_B      */ NAOMI_BTN0_KEY, /* BTN1 */
      /* JOYPAD_Y      */ NAOMI_BTN2_KEY, /* BTN3 */
      /* JOYPAD_SELECT */ NAOMI_COIN_KEY,
      /* JOYPAD_START  */ NAOMI_START_KEY,
      /* JOYPAD_UP     */ NAOMI_UP_KEY,
      /* JOYPAD_DOWN   */ NAOMI_DOWN_KEY,
      /* JOYPAD_LEFT   */ NAOMI_LEFT_KEY,
      /* JOYPAD_RIGHT  */ NAOMI_RIGHT_KEY,
      /* JOYPAD_A      */ NAOMI_BTN1_KEY, /* BTN2 */
      /* JOYPAD_X      */ NAOMI_BTN3_KEY, /* BTN4 */
      /* JOYPAD_L      */ NAOMI_BTN5_KEY, /* BTN6 */
      /* JOYPAD_R      */ NAOMI_BTN4_KEY, /* BTN5 */
      /* JOYPAD_L2     */ NAOMI_BTN7_KEY, /* BTN8 */
      /* JOYPAD_R2     */ NAOMI_BTN6_KEY, /* BTN7 */
      /* JOYPAD_L3     */ NAOMI_TEST_KEY,
      /* JOYPAD_R3     */ NAOMI_SERVICE_KEY,
   };

   static const uint32_t nao_lg_joymap[] =
   {
	   /* deprecated */ 			0,
	   /* deprecated */ 			0,
	   /* LIGHTGUN_TRIGGER */	NAOMI_BTN0_KEY,
	   /* LIGHTGUN_AUX_A */		NAOMI_BTN1_KEY,
	   /* LIGHTGUN_AUX_B */ 	NAOMI_BTN2_KEY,
	   /* deprecated */ 			0,
	   /* LIGHTGUN_START */		NAOMI_START_KEY,
	   /* LIGHTGUN_SELECT */ 	NAOMI_COIN_KEY,
	   /* LIGHTGUN_AUX_C */		NAOMI_BTN3_KEY,
	   /* LIGHTGUN_UP   */ 		NAOMI_UP_KEY,
	   /* LIGHTGUN_DOWN   */ 	NAOMI_DOWN_KEY,
	   /* LIGHTGUN_LEFT   */ 	NAOMI_LEFT_KEY,
	   /* LIGHTGUN_RIGHT  */ 	NAOMI_RIGHT_KEY,
   };

   const uint32_t *joymap;
   size_t joymap_size;
   uint32_t mapped;

   switch (settings.System)
   {
   case DC_PLATFORM_DREAMCAST:
   case DC_PLATFORM_DEV_UNIT:
	  switch (device)
	  {
	  case RETRO_DEVICE_JOYPAD:
	  case RETRO_DEVICE_POINTER:
		 joymap = dc_joymap;
		 joymap_size = ARRAY_SIZE(dc_joymap);
		 break;
	  case RETRO_DEVICE_LIGHTGUN:
		 joymap = dc_lg_joymap;
		 joymap_size = ARRAY_SIZE(dc_lg_joymap);
		 break;
	  default:
		 return 0;
	  }
	  break;

   case DC_PLATFORM_NAOMI:
   case DC_PLATFORM_NAOMI2:
	  switch (device)
	  {
	  case RETRO_DEVICE_JOYPAD:
	  case RETRO_DEVICE_POINTER:
		 joymap = nao_joymap;
		 joymap_size = ARRAY_SIZE(nao_joymap);
		 break;
	  case RETRO_DEVICE_LIGHTGUN:
		 joymap = nao_lg_joymap;
		 joymap_size = ARRAY_SIZE(nao_lg_joymap);
		 break;
	  default:
		 return 0;
	  }
	  break;

   case DC_PLATFORM_ATOMISWAVE:
	  switch (device)
	  {
	  case RETRO_DEVICE_JOYPAD:
	  case RETRO_DEVICE_POINTER:
		 joymap = aw_joymap;
		 joymap_size = ARRAY_SIZE(aw_joymap);
		 break;
	  case RETRO_DEVICE_LIGHTGUN:
		 joymap = aw_lg_joymap;
		 joymap_size = ARRAY_SIZE(aw_lg_joymap);
		 break;
	  default:
		 return 0;
	  }
	  break;

   default:
	  return 0;
   }

   if (id >= joymap_size)
	  return 0;
   mapped = joymap[id];
   /* L puts a card in, in a game with a card reader */
   if (id == RETRO_DEVICE_ID_JOYPAD_L && device == RETRO_DEVICE_JOYPAD && SYSTEM_IS_NAOMI() && naomi_card_button)
	  mapped = NAOMI_INSERT_CARD_KEY;
   /* Hack to bind Button 9 instead of Service when not used */
   if (id == RETRO_DEVICE_ID_JOYPAD_R3 && device == RETRO_DEVICE_JOYPAD && SYSTEM_IS_NAOMI() && !allow_service_buttons)
	  mapped = NAOMI_BTN8_KEY;
   return mapped;
}

static uint32_t get_time_ms(void)
{
   return (uint32_t)(os_GetSeconds() * 1000.0);
}

static void get_analog_stick( retro_input_state_t input_state_cb,
                       int player_index,
                       int stick,
                       s8* p_analog_x,
                       s8* p_analog_y )
{
   int analog_x, analog_y;
   analog_x = input_state_cb( player_index, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_X );
   analog_y = input_state_cb( player_index, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_Y );

   /* Analog stick deadzone (borrowed code from parallel-n64 core) */
   if ( astick_deadzone > 0 )
   {
      static const int ASTICK_MAX = 0x8000;

      /* Convert cartesian coordinate analog stick to polar coordinates */
      /* (in double: two axes at -32768 overflow an int, and the stick
       * pushed fully into that corner read as centred) */
      double radius = sqrt((double)analog_x * analog_x + (double)analog_y * analog_y);
      double angle = atan2(analog_y, analog_x);

      if (radius > astick_deadzone)
      {
         /* Re-scale analog stick range to negate deadzone (makes slow movements possible) */
         radius = (radius - astick_deadzone)*((float)ASTICK_MAX/(ASTICK_MAX - astick_deadzone));

         /* Convert back to cartesian coordinates */
         analog_x = (int)lr_round(radius * cos(angle));
         analog_y = (int)lr_round(radius * sin(angle));

         /* Clamp to correct range */
         if (analog_x > +32767) analog_x = +32767;
         if (analog_x < -32767) analog_x = -32767;
         if (analog_y > +32767) analog_y = +32767;
         if (analog_y < -32767) analog_y = -32767;
      }
      else
      {
         analog_x = 0;
         analog_y = 0;
      }
   }

   /* output */
   *p_analog_x = (s8)(analog_x >> 8);
   *p_analog_y = (s8)(analog_y >> 8);
}

static uint16_t apply_trigger_deadzone( uint16_t input )
{
   if ( trigger_deadzone > 0 )
   {
      if ( input > trigger_deadzone )
      {
         /* Re-scale analog range */
         static const int TRIGGER_MAX = 0x8000;
         const float scale = ((float)TRIGGER_MAX/(float)(TRIGGER_MAX - trigger_deadzone));
         float scaled      = (input - trigger_deadzone)*scale;

         input = (int)lr_round(scaled);
         if (input > +32767)
            input = +32767;
      }
      else
         input = 0;
   }

   return input;
}

static uint16_t get_analog_trigger(
      int16_t ret,
      retro_input_state_t input_state_cb,
      int player_index,
      int id )
{
   /* NOTE: Analog triggers were added Nov 2017. Not all front-ends support this */
   /* feature (or pre-date it) so we need to handle this in a graceful way. */

   /* First, try and get an analog value using the new libretro API constant */
   uint16_t trigger = input_state_cb( player_index,
                       RETRO_DEVICE_ANALOG,
                       RETRO_DEVICE_INDEX_ANALOG_BUTTON,
                       id );

   if ( trigger == 0 )
   {
      /* If we got exactly zero, we're either not pressing the button, or the front-end */
      /* is not reporting analog values. We need to do a second check using the classic */
      /* digital API method, to at least get some response - better than nothing. */

      /* NOTE: If we're really just not holding the trigger, we're still going to get zero. */

      trigger = (ret & (1 << id)) ? 0x7FFF : 0;
   }
   else
   {
      /* We got something, which means the front-end can handle analog buttons. */
      /* So we apply a deadzone to the input and use it. */

      trigger = apply_trigger_deadzone( trigger );
   }

   return trigger;
}

#define setDeviceButtonState(port, deviceType, btnId) \
{ \
   uint32_t dc_key = map_gamepad_button(deviceType, btnId); \
   bool is_down = input_cb(port, deviceType, 0, btnId) != 0; \
   if (is_down) \
	  kcode[port] &= ~dc_key; \
   else \
	  kcode[port] |= dc_key; \
}

#define setDeviceButtonStateMacro(ret, port, deviceType, btnId) \
{ \
   uint32_t dc_key = map_gamepad_button(deviceType, btnId); \
   bool is_down    = (ret & (1 << btnId)) != 0; \
   if (is_down) \
	  kcode[port] &= ~dc_key; \
   else \
	  kcode[port] |= dc_key; \
}

/* don't call map_gamepad_button, we supply the DC bit directly. */
#define setDeviceButtonStateDirectMacro(ret, port, deviceType, btnId, dc_bit) \
{ \
   uint32_t dc_key = 1 << dc_bit; \
   bool is_down    = (ret & (1 << btnId)) != 0; \
   if (is_down) \
	  kcode[port] &= ~dc_key; \
   else \
	  kcode[port] |= dc_key; \
}

static void updateMouseState(u32 port)
{
   bool btn_state;
   /* Motion adds up until whoever reads it takes it: a frame in which the
    * game does not ask would lose its own otherwise. */
   mo_x_delta[port] += input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
   mo_y_delta[port] += input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);

   btn_state   = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
   if (btn_state)
	  mo_buttons[port] &= ~(1 << 2);
   else
	  mo_buttons[port] |= 1 << 2;
   btn_state = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
   if (btn_state)
	  mo_buttons[port] &= ~(1 << 1);
   else
	  mo_buttons[port] |= 1 << 1;
   /* (the middle button is bit 3, the wheel's; it was put in bit 0, which is no button of a mouse's) */
   btn_state = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE) != 0;
   if (btn_state)
	  mo_buttons[port] &= ~(1 << 3);
   else
	  mo_buttons[port] |= 1 << 3;
   if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN))
	  mo_wheel_delta[port] -= 10;
   else if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP))
	  mo_wheel_delta[port] += 10;
}

/* Where a light gun, or a pointer standing for one, points: the game's
 * 640x480, as mo_x_abs/mo_y_abs have it. With the widescreen hack the
 * picture is a third wider than the game's, which is in the middle of it:
 * what is pointed at is taken from the whole width the frontend shows. */
static void updateLightgunCoordinates(u32 port)
{
   int x, y;

   if (lr_device_type[port] == RETRO_DEVICE_POINTER)
   {
      x = input_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
      y = input_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
   }
   else
   {
      x = input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
      y = input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
   }
   if (settings.rend.WideScreen && !wide_by_cheat)
      mo_x_abs[port] = 640.f * ((x + 0x8000) * 4.f / 3.f / 0x10000 - (4.f / 3.f - 1.f) / 2.f);
   else
      mo_x_abs[port] = (x + 0x8000) * 640.f / 0x10000;
   mo_y_abs[port] = (y + 0x8000) * 480.f / 0x10000;

   lightgun_params[port].offscreen = false;
   lightgun_params[port].x = mo_x_abs[port];
   lightgun_params[port].y = mo_y_abs[port];
}

/* An arcade light gun game played with a gamepad: its left stick aims. */
static void updateLightgunCoordinatesFromAnalogStick(u32 port)
{
   int x = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
   int y = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);

   mo_x_abs[port] = 320 + x * 320 / 32767;
   mo_y_abs[port] = 240 + y * 240 / 32767;

   lightgun_params[port].offscreen = false;
   lightgun_params[port].x = mo_x_abs[port];
   lightgun_params[port].y = mo_y_abs[port];
}

/* A touchscreen standing for a light gun: a touch aims and fires, two
 * fingers point off the screen (reload). @trigger is the game's trigger. */
static void updatePointerAsLightgun(u32 port, u32 trigger, float offscreen)
{
   int pressed = input_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
   int count = input_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_COUNT);

   if (count > 1)
   {
      mo_x_abs[port] = offscreen;
      mo_y_abs[port] = offscreen;
      lightgun_params[port].offscreen = true;
      lightgun_params[port].x = mo_x_abs[port];
      lightgun_params[port].y = mo_y_abs[port];
   }
   else if (count == 1)
      updateLightgunCoordinates(port);
   if (pressed)
      kcode[port] &= ~trigger;
   else
      kcode[port] |= trigger;
}

static void UpdateInputStateNaomi(u32 port)
{
   int id;
   int i;
   bool force_offscreen;
   int16_t ret;

   switch (maple_devices[port])
   {
   case MDT_LightGun:
	  if (lr_device_type[port] == RETRO_DEVICE_POINTER)
	  {
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_B);
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_START);
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_UP);
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_DOWN);
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_LEFT);
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_RIGHT);
		 updatePointerAsLightgun(port, SYSTEM_IS_NAOMI() ? (u32)NAOMI_BTN0_KEY : (u32)AWAVE_TRIGGER_KEY, 0);
	  }
	  else
	  {
		 /*  */
		 /* -- buttons */
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_AUX_A);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_AUX_B);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_AUX_C);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_START);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_SELECT);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT);

		 force_offscreen = false;

		 if (input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD))
		 {
			force_offscreen = true;
			if (SYSTEM_IS_NAOMI())
			   kcode[port] &= ~NAOMI_BTN0_KEY;
			else
			   kcode[port] &= ~AWAVE_TRIGGER_KEY;
		 }

		 if (force_offscreen || input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN))
		 {
			mo_x_abs[port] = 0;
			mo_y_abs[port] = 0;
			lightgun_params[port].offscreen = true;

			if (input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) || input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD))
			{
			 if (SYSTEM_IS_NAOMI())
			   kcode[port] &= ~NAOMI_BTN1_KEY;
			}
		 }
		 else
			updateLightgunCoordinates(port);
	  }
	  break;

   default:
	  ret = 0;
	  /*  */
	  /* -- buttons */
     {
        if (libretro_supports_bitmasks)
           ret = input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
        else
        {
           for (id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
              if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, id))
                 ret |= (1 << id);
        }

        for (id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
        {
           switch (id)
           {
              case RETRO_DEVICE_ID_JOYPAD_L3:
                 if (allow_service_buttons)
                    setDeviceButtonStateMacro(ret, port, RETRO_DEVICE_JOYPAD, id);
                 break;
              case RETRO_DEVICE_ID_JOYPAD_R3:
                 if (SYSTEM_IS_NAOMI() || allow_service_buttons)
                    setDeviceButtonStateMacro(ret, port, RETRO_DEVICE_JOYPAD, id);
                 break;
              default:
                 setDeviceButtonStateMacro(ret, port, RETRO_DEVICE_JOYPAD, id);
                 break;
           }
        }
     }
	  /*  */
	  /* -- analog stick */

	  get_analog_stick( input_cb, port, RETRO_DEVICE_INDEX_ANALOG_LEFT, &(joyx[port]), &(joyy[port]) );
	  get_analog_stick( input_cb, port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, &(joyrx[port]), &(joyry[port]));
	  lt[port] = get_analog_trigger(ret, input_cb, port, RETRO_DEVICE_ID_JOYPAD_L2) / 128;
	  rt[port] = get_analog_trigger(ret, input_cb, port, RETRO_DEVICE_ID_JOYPAD_R2) / 128;

	  if (naomi_game_inputs != NULL)
	  {
		 for (i = 0; naomi_game_inputs->axes[i].name != NULL; i++)
		 {
			AxisType axis_type = naomi_game_inputs->axes[i].type;
			/* Note:
			 * - Analog stick axes have a range of [-128, 127]
			 * - Analog triggers have a range of [0, 255] */
			switch (naomi_game_inputs->axes[i].axis)
			{
			case 0:
			   /* Left stick X: [-128, 127] */
			   if (axis_type == Half)
				  joyx[port] = MAX((int)joyx[port], 0) * 2;
			   else
				  joyx[port] += 128;
			   break;
			case 1:
			   /* Left stick Y: [-128, 127] */
			   if (axis_type == Half)
				  joyy[port] = MAX((int)joyy[port], 0) * 2;
			   else
				  joyy[port] += 128;
			   break;
			case 2:
			   /* Right stick X: [-128, 127] */
			   if (axis_type == Half)
				  joyrx[port] = MAX((int)joyrx[port], 0) * 2;
			   else
				  joyrx[port] += 128;
			   break;
			case 3:
			   /* Right stick Y: [-128, 127] */
			   if (axis_type == Half)
				  joyry[port] = MAX((int)joyry[port], 0) * 2;
			   else
				  joyry[port] += 128;
			   break;
			/* Case 4/5 correspond to right/left trigger.
			 * These inputs are always classified as 'Half',
			 * and already have the correct range - so no
			 * further action is required */
			}
		 }
	  }
	  else
	  {
		 /* Make all axes full by default */
		 joyx[port] += 128;
		 joyy[port] += 128;
		 joyrx[port] += 128;
		 joyry[port] += 128;
		 /* Left/right trigger are always classified as 'Half',
		  * so no ajustment required */
	  }

	  /* -- mouse, for rotary encoders */
	  updateMouseState(port);
	  /* -- a light gun game: the left stick aims */
	  if (settings.mapping.JammaSetup == JVS_LightGun || settings.mapping.JammaSetup == JVS_LightGunAsAnalog)
	  {
		 updateLightgunCoordinatesFromAnalogStick(port);
		 if (input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD))
		 {
			mo_x_abs[port] = 0;
			mo_y_abs[port] = 0;
			lightgun_params[port].offscreen = true;
			if (SYSTEM_IS_NAOMI())
			   kcode[port] &= ~NAOMI_BTN0_KEY;
			else
			   kcode[port] &= ~AWAVE_TRIGGER_KEY;
		 }
		 else if (!SYSTEM_IS_NAOMI())
		 {
			/* the gun's buttons from the pad's: button 1 fires, button 2 is the gun's first, and so on */
			u32 k = kcode[port] | (AWAVE_BTN0_KEY | AWAVE_BTN1_KEY | AWAVE_BTN2_KEY | AWAVE_BTN3_KEY | AWAVE_TRIGGER_KEY);
			if ((kcode[port] & AWAVE_BTN0_KEY) == 0)
			   k &= ~AWAVE_TRIGGER_KEY;
			if ((kcode[port] & AWAVE_BTN1_KEY) == 0)
			   k &= ~AWAVE_BTN0_KEY;
			if ((kcode[port] & AWAVE_BTN2_KEY) == 0)
			   k &= ~AWAVE_BTN1_KEY;
			if ((kcode[port] & AWAVE_BTN3_KEY) == 0)
			   k &= ~AWAVE_BTN2_KEY;
			kcode[port] = k;
		 }
	  }
	  break;
   }

   /* Avoid Left+Right or Up+Down buttons being pressed together as this crashes some games */
	if (SYSTEM_IS_NAOMI())
	{
	   if ((kcode[port] & (NAOMI_UP_KEY|NAOMI_DOWN_KEY)) == 0)
		  kcode[port] |= NAOMI_UP_KEY|NAOMI_DOWN_KEY;
	   if ((kcode[port] & (NAOMI_LEFT_KEY|NAOMI_RIGHT_KEY)) == 0)
		  kcode[port] |= NAOMI_LEFT_KEY|NAOMI_RIGHT_KEY;
	}
	else
	{
	   if ((kcode[port] & (AWAVE_UP_KEY|AWAVE_DOWN_KEY)) == 0)
		  kcode[port] |= AWAVE_UP_KEY|AWAVE_DOWN_KEY;
	   if ((kcode[port] & (AWAVE_LEFT_KEY|AWAVE_RIGHT_KEY)) == 0)
		  kcode[port] |= AWAVE_LEFT_KEY|AWAVE_RIGHT_KEY;
	}
}

/*
 * Input is sampled on the main (libretro) thread, once per frame, right after
 * poll_cb(). In threaded-rendering mode the emulation runs on a separate thread
 * that must not call the frontend's input_cb itself: libretro requires input to
 * be read on the retro_run thread after input_poll, and reading it at maple-DMA
 * time from the emu thread samples at a nondeterministic point relative to the
 * frame, which breaks runahead/netplay determinism. CaptureInput() samples all
 * ports here and publishes them through input_latch; the maple read path
 * (GetInput/GetMouse) reads the latch on the emu thread. Neither side takes a
 * lock (see input_latch.h).
 *
 * In non-threaded rendering the maple path samples live on the one thread
 * there is and reads the globals directly: a Dreamcast's port when its
 * device is asked, an arcade machine's four together once a frame
 * (UpdateInputStateArcade).
 */
InputLatch input_latch;

/* Once for the program, as it was made before any core was started: made
 * again, it would no longer match the motion counted below. */
void lr_input_init(void)
{
   static bool made;
   init_kb_map();
   if (made)
      return;
   made = true;
   input_latch_init(&input_latch);
}

/* Mouse motion handed to the latch so far. */
static u32 mo_x_total[MAPLE_PORTS], mo_y_total[MAPLE_PORTS], mo_wheel_total[MAPLE_PORTS];

void CaptureInput(void)
{
   input_latch_state *next = input_latch_next(&input_latch);
   u32 port;
   bool trackball;
   /* An Atomiswave's trackball is a mouse on the bus whatever device the
    * player's port is set to. (Its motion went to the latch only if that
    * was "Mouse": set to anything else, the trackball never moved.) */
   trackball = settings.System == DC_PLATFORM_ATOMISWAVE;

   for (port = 0; port < MAPLE_PORTS; port++)
   {
      input_latch_port *s = &next->port[port];

      UpdateInputState(port);

      /* The latch takes the motion a mouse or a trackball reads from it. A
       * NAOMI's rotary encoders take theirs from the globals themselves,
       * where it stays until they do. */
      if (maple_devices[port] == MDT_Mouse ? !SYSTEM_IS_NAOMI() : trackball)
      {
         mo_x_total[port]     += (u32)(s32)mo_x_delta[port];
         mo_y_total[port]     += (u32)(s32)mo_y_delta[port];
         mo_wheel_total[port] += (u32)(s32)mo_wheel_delta[port];
         mo_x_delta[port]      = 0;
         mo_y_delta[port]      = 0;
         mo_wheel_delta[port]  = 0;
      }

      s->kcode      = kcode[port];
      s->joyx       = joyx[port];
      s->joyy       = joyy[port];
      s->joyrx      = joyrx[port];
      s->joyry      = joyry[port];
      s->rt         = rt[port];
      s->lt         = lt[port];
      s->mo_buttons = mo_buttons[port];
      s->mo_x       = mo_x_total[port];
      s->mo_y       = mo_y_total[port];
      s->mo_wheel   = mo_wheel_total[port];
   }
   input_latch_publish(&input_latch);
}

/* Without threaded rendering, an arcade machine's ports are read from the
 * frontend, all four, unless that is done: when the machine first asks
 * for its inputs in a frame - no sooner, so with no more delay than when
 * each device read its own - or at the end of a frame in which it did not
 * (os_DoEvents). So each poll of the frontend's is read once, as
 * CaptureInput() reads it with threaded rendering, and mouse motion can add
 * up without being counted twice or left out. */
void UpdateInputStateArcade(void)
{
   u32 port;
   if (arcade_input_read)
      return;
   arcade_input_read = true;
   for (port = 0; port < MAPLE_PORTS; port++)
      UpdateInputState(port);
}

/* Without threaded rendering, a Dreamcast's mouse is read from the frontend
 * once for each poll of the frontend's, as an arcade machine's ports are:
 * when the game first asks for it in a frame, or at the end of a frame in
 * which it did not (os_DoEvents). (It was read each time the game asked: a
 * poll's motion was counted once for each time, and in a frame without one
 * not at all.) What the frontend says does not change between two polls, so
 * the buttons are the same whichever time they are read. */
void UpdateInputStateMouse(u32 port)
{
   const u32 bit = 1 << port;

   if (mouse_ports_read & bit)
      return;
   mouse_ports |= bit;
   mouse_ports_read |= bit;
   UpdateInputState(port);
}

/* The state's part of the libretro layer (V23): what has been read from the
 * frontend and the machine has not yet been given, and what has been made
 * and the frontend not yet sent.
 *
 * Mouse motion - a mouse's, a trackball's, a rotary encoder's - adds up
 * until the game reads it, and a game does not read it every frame. What
 * was still to be read stayed as it was through a load: under run-ahead,
 * whatever the frames run ahead had left of it, so a frame run from a
 * loaded state was given other motion than the same frame run without the
 * load. With it go the ports a game has asked a mouse on, which say whether
 * one is read at the end of a frame in which the game did not ask
 * (os_DoEvents). The motion is whole steps of the frontend's, the same in a
 * float as in the latch's counts. 49 bytes. */

void lr_serialize_v23(void **data, unsigned int *total_size)
{
   f32 pending[MAPLE_PORTS * 3];
   u8 asked = (u8)(mouse_ports & 0xF);
   u32 port;

   for (port = 0; port < MAPLE_PORTS; port++)
   {
      f32 x = mo_x_delta[port];
      f32 y = mo_y_delta[port];
      f32 wheel = mo_wheel_delta[port];
#if !defined(TARGET_NO_THREADS)
      if (settings.rend.ThreadedRendering)
      {
         u32 sx, sy, swheel;
         input_latch_get_seen(&input_latch, port, &sx, &sy, &swheel);
         x     += (f32)(s32)(mo_x_total[port] - sx);
         y     += (f32)(s32)(mo_y_total[port] - sy);
         wheel += (f32)(s32)(mo_wheel_total[port] - swheel);
      }
#endif
      pending[port * 3]     = x;
      pending[port * 3 + 1] = y;
      pending[port * 3 + 2] = wheel;
   }
   LIBRETRO_SA(pending, MAPLE_PORTS * 3);
   LIBRETRO_S(asked);
   AudioHoldbackSerialize(data, total_size);
}

bool lr_unserialize_v23(void **data, unsigned int *total_size)
{
   f32 pending[MAPLE_PORTS * 3];
   u8 asked = 0;
   u32 i, port;

   LIBRETRO_USA(pending, MAPLE_PORTS * 3);
   LIBRETRO_US(asked);
   for (i = 0; i < MAPLE_PORTS * 3; i++)
      /* (nothing that is not a number, nor more than a frontend's counts can come to) */
      if (!(pending[i] >= -1048576.f && pending[i] <= 1048576.f))
         pending[i] = 0.f;
   for (port = 0; port < MAPLE_PORTS; port++)
   {
      /* All of it is to be read from the globals, where a frame without
       * the latch takes it from, and a frame with it hands it to the latch
       * first (CaptureInput()). */
      mo_x_delta[port]     = pending[port * 3];
      mo_y_delta[port]     = pending[port * 3 + 1];
      mo_wheel_delta[port] = pending[port * 3 + 2];
      input_latch_set_seen(&input_latch, port, mo_x_total[port], mo_y_total[port], mo_wheel_total[port]);
   }
   mouse_ports      = asked & 0xF;
   mouse_ports_read = 0;
   AudioHoldbackUnserialize(data, total_size);
   return true;
}

/* A state from before V23: what is to be read stays as it is, and the
 * samples held back are dropped, as they were. */
void lr_state_before_v23(void)
{
   ResetAudioBuffer();
}

void UpdateInputState(u32 port)
{
   bool force_offscreen;
   if (gl_ctx_resetting)
	  return;

   if (SYSTEM_IS_NAOMI() || settings.System == DC_PLATFORM_ATOMISWAVE)
   {
      /* Update only this port. CaptureInput already iterates all ports; the
       * previous code updated all four on every call, so a 4-port loop read
       * each NAOMI/Atomiswave port -- and hit the frontend's input_cb for every
       * lightgun/analog axis -- 4 times per frame. */
      UpdateInputStateNaomi(port);
      if (port == 0)
         naomi_ffb_update();
      return;
   }
   if (lr_rumble.set_rumble_state != NULL && vib_stop_time[port] > 0)
   {
	  if (get_time_ms() >= vib_stop_time[port])
	  {
		 vib_stop_time[port] = 0;
		 lr_rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, 0);
	  }
	  else if (vib_delta[port] > 0.0)
	  {
		 u32 rem_time = vib_stop_time[port] - get_time_ms();
		 lr_rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, 65535 * vib_strength[port] * rem_time * vib_delta[port]);
	  }
   }

   lightgun_params[port].offscreen = true;

   switch (maple_devices[port])
   {
	  case MDT_SegaController:
	  {
		   int id;
         int16_t ret = 0;
		   /*  */
		   /* -- buttons */

         if (libretro_supports_bitmasks)
            ret = input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
         else
         {
            for (id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
               if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, id))
                  ret |= (1 << id);
         }

		   for (id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_X; ++id)
			  setDeviceButtonStateMacro(ret, port, RETRO_DEVICE_JOYPAD, id);

		   /*  */
		   /* -- analog stick */

		   get_analog_stick( input_cb, port, RETRO_DEVICE_INDEX_ANALOG_LEFT, &(joyx[port]), &(joyy[port]) );


		   /*  */
		   /* -- triggers */

		   if ( digital_triggers )
		   {
		      /* -- digital left trigger */
            if (ret & (1 << RETRO_DEVICE_ID_JOYPAD_L2))
		         lt[port]=0xFF;
		      else
		         lt[port]=0;
		      /* -- digital right trigger */
            if (ret & (1 << RETRO_DEVICE_ID_JOYPAD_R2))
	rt[port]=0xFF;
            else
		         rt[port]=0;
		   }
		   else
		   {
			   /* -- analog triggers */
			   lt[port] = get_analog_trigger(ret, input_cb, port, RETRO_DEVICE_ID_JOYPAD_L2 ) / 128;
			   rt[port] = get_analog_trigger(ret, input_cb, port, RETRO_DEVICE_ID_JOYPAD_R2 ) / 128;
		   }
	  }
	  break;

	case MDT_AsciiStick:
	{
      int16_t ret = 0;

      if (libretro_supports_bitmasks)
         ret = input_cb(port, RETRO_DEVICE_ASCIISTICK, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
      else
      {
         unsigned id;
         for (id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
            if (input_cb(port, RETRO_DEVICE_ASCIISTICK, 0, id))
               ret |= (1 << id);
      }

		kcode[port] = 0xFFFF; /* active-low */

		/* stick */
		setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_ASCIISTICK, RETRO_DEVICE_ID_JOYPAD_UP, 4 );
		setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_ASCIISTICK, RETRO_DEVICE_ID_JOYPAD_DOWN, 5 );
		setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_ASCIISTICK, RETRO_DEVICE_ID_JOYPAD_LEFT, 6 );
		setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_ASCIISTICK, RETRO_DEVICE_ID_JOYPAD_RIGHT, 7 );

		/* buttons */
		setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_ASCIISTICK, RETRO_DEVICE_ID_JOYPAD_B,  2 ); /* A */
		setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_ASCIISTICK, RETRO_DEVICE_ID_JOYPAD_A,  1 ); /* B */
		setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_ASCIISTICK, RETRO_DEVICE_ID_JOYPAD_Y, 10 ); /* X */
		setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_ASCIISTICK, RETRO_DEVICE_ID_JOYPAD_X,  9 ); /* Y */

		/* Z */
		{
		   uint32_t dc_key = 1 << 8; /* Z */
		   bool is_down = (ret & (1 << RETRO_DEVICE_ID_JOYPAD_L )) ||
						   (ret & (1 << RETRO_DEVICE_ID_JOYPAD_L2));
		   if (is_down)
			  kcode[port] &= ~dc_key;
		   else
			  kcode[port] |= dc_key;
		}

		/* C */
		{
		   uint32_t dc_key = 1 << 0; /* C */
		   bool is_down = (ret & (1 << RETRO_DEVICE_ID_JOYPAD_R)) ||
						   (ret & (1 << RETRO_DEVICE_ID_JOYPAD_R2));
		   if (is_down)
			  kcode[port] &= ~dc_key;
		   else
			  kcode[port] |= dc_key;
		}

		setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_ASCIISTICK, RETRO_DEVICE_ID_JOYPAD_START, 3 ); /* Start */

		/* unused inputs */
		lt[port]=0;
		rt[port]=0;
		joyx[port]=0;
		joyy[port]=0;
	}
	break;

	case MDT_TwinStick:
	{
      int16_t ret = 0;

		kcode[port] = 0xFFFF; /* active-low */


		if ( lr_device_type[port] == RETRO_DEVICE_TWINSTICK_SATURN )
		{
			/* NOTE: This is a remapping of the RetroPad layout in the block below to make using a real */
			/* Saturn Twin-Stick controller (via a USB adapter) less effort. */

			/* The Saturn Twin-Stick identifies as a regular Saturn controller internally but with its controls */
			/* wired to the two sticks without much rhyme or reason. The mapping below untangles that layout */
			/* into DC compatible inputs, without requiring a change for the Reicast and Beetle Saturn cores. */

			/* Hope that makes sense!! */

			/* NOTE: the dc_bits below are the same, only the retro id values have been rearranged. */

         if (libretro_supports_bitmasks)
            ret = input_cb(port, RETRO_DEVICE_TWINSTICK_SATURN, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
         else
         {
            unsigned id;
            for (id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
               if (input_cb(port, RETRO_DEVICE_TWINSTICK_SATURN, 0, id))
                  ret |= (1 << id);
         }

			/* left-stick */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_UP, 4 );
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_DOWN, 5 );
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_LEFT, 6 );
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_RIGHT, 7 );
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_L2, 10 ); /* left-trigger */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_R2, 9 ); /* left-turbo */

			/* right-stick */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_X, 12 ); /* up */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_L, 15 ); /* right */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_A, 13 ); /* down */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_Y, 14 ); /* left */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_B, 2 ); /* right-trigger */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_R, 1 ); /* right-turbo */

			/* misc control */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_START, 3 );
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ID_JOYPAD_SELECT, 11 ); /* D */
		}
		else
		{
			int analog;

			const int thresh = 11000; /* about 33%, allows for 8-way movement */

         if (libretro_supports_bitmasks)
            ret = input_cb(port, RETRO_DEVICE_TWINSTICK, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
         else
         {
            unsigned id;
            for (id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
               if (input_cb(port, RETRO_DEVICE_TWINSTICK, 0, id))
                  ret |= (1 << id);
         }

			/* LX */
			analog = input_cb( port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X );
			if ( analog < -thresh )
				kcode[port] &= ~( 1 << 6 ); /* L */
			else if ( analog > thresh )
				kcode[port] &= ~( 1 << 7 ); /* R */
			else
			{
				/* digital */
				setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_LEFT, 6 );
				setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_RIGHT, 7 );
			}

			/* LY */
			analog = input_cb( port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y );
			if ( analog < -thresh )
				kcode[port] &= ~( 1 << 4 ); /* U */
			else if ( analog > thresh )
				kcode[port] &= ~( 1 << 5 ); /* D */
			else
			{
				/* digital */
				setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_UP, 4 );
				setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_DOWN, 5 );
			}

			/* RX */
			analog = input_cb( port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X );
			if ( analog < -thresh )
				kcode[port] &= ~( 1 << 14 ); /* L */
			else if ( analog > thresh )
				kcode[port] &= ~( 1 << 15 ); /* R */
			else
			{
				/* digital */
				setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_Y, 14 ); /* left */
				setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_A, 15 ); /* right */
			}

			/* RY */
			analog = input_cb( port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y );
			if ( analog < -thresh )
				kcode[port] &= ~( 1 << 12 ); /* U */
			else if ( analog > thresh )
				kcode[port] &= ~( 1 << 13 ); /* D */
			else
			{
				/* digital */
				setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_X, 12 ); /* up */
				setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_B, 13 ); /* down */
			}

			/* left-stick buttons */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_L2, 10 ); /* left-trigger */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_L, 9 ); /* left-turbo */

			/* right-stick buttons */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_R2, 2 ); /* right-trigger */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_R, 1 ); /* right-turbo */

			/* misc control */
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_START, 3 );
			setDeviceButtonStateDirectMacro(ret, port, RETRO_DEVICE_TWINSTICK, RETRO_DEVICE_ID_JOYPAD_SELECT, 11 ); /* D */
		}

		/* unused inputs */
		lt[port]=0;
		rt[port]=0;
		joyx[port]=0;
		joyy[port]=0;
	}

	break;

	  case MDT_LightGun:
	  if (lr_device_type[port] == RETRO_DEVICE_POINTER)
	  {
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_B);
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_START);
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_UP);
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_DOWN);
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_LEFT);
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ID_JOYPAD_RIGHT);
		 updatePointerAsLightgun(port, DC_BTN_A, -1000);
	  }
	  else
	  {
		 /*  */
		 /* -- buttons */
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_AUX_A);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_START);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT);

		 force_offscreen = false;

		 if (input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD))
		 {
			force_offscreen = true;
			kcode[port] &= ~DC_BTN_A;
		 }

		 if (force_offscreen || input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN))
		 {
			mo_x_abs[port] = -1000;
			mo_y_abs[port] = -1000;
			lightgun_params[port].offscreen = true;

			lightgun_params[port].x = mo_x_abs[port];
			lightgun_params[port].y = mo_y_abs[port];
		 }
		 else
			updateLightgunCoordinates(port);
	  }
	  break;

	  case MDT_Mouse:
		 updateMouseState(port);
		 break;

	  default:
		 break;
   }
}

void UpdateVibration(u32 port, u32 value, u32 max_duration)
{
   u8 POW_POS, POW_NEG, FREQ;
   s16 INC;
   bool CNT;
   double strength;
   if (!lr_rumble.set_rumble_state)
      return;

   POW_POS = (value >> 8) & 0x7;
   POW_NEG = (value >> 12) & 0x7;
   FREQ = (value >> 16) & 0xFF;
   INC = (value >> 24) & 0xFF;
   if (value & 0x8000)			/* INH */
	  INC = -INC;
   else if (!(value & 0x0800))	/* EXH */
	  INC = 0;
   CNT = (value & 1) != 0;

   strength           = MIN((POW_POS + POW_NEG) / 7.0, 1.0);
   vib_strength[port] = strength;

   lr_rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, (u16)(65535 * strength));

   if (FREQ > 0 && (!CNT || INC))
	  vib_stop_time[port] = get_time_ms() + MIN((int)(1000 * (INC ? abs(INC) * MAX(POW_POS, POW_NEG) : 1) / FREQ), (int)max_duration);
   else
	  vib_stop_time[port] = get_time_ms() + max_duration;
   if (INC == 0 || strength == 0)
	  vib_delta[port] = 0.0;
   else
	  vib_delta[port] = FREQ / (1000.0 * INC * MAX(POW_POS, POW_NEG));
}

static int kb_used;

static void release_key(unsigned dc_keycode)
{
   if (dc_keycode == 0)
	  return;

   if (kb_used > 0)
   {
	  int i, j;
	  for (i = 0; i < 6; i++)
	  {
		 if (kb_key[i] == dc_keycode)
		 {
			kb_used--;
			for (j = i; j < 5; j++)
			   kb_key[j] = kb_key[j + 1];
			kb_key[5] = 0;
		 }
	  }
   }
}

void retro_keyboard_event(bool down, unsigned keycode, uint32_t character, uint16_t key_modifiers)
{
   u8 dc_keycode;
   /* Dreamcast keyboard emulation */
   if (keycode == RETROK_LSHIFT || keycode == RETROK_RSHIFT)
   {
	  if (!down)
		 kb_shift &= ~(0x02 | 0x20);
	  else
		 kb_shift |= (0x02 | 0x20);
   }
   if (keycode == RETROK_LCTRL || keycode == RETROK_RCTRL)
   {
	  if (!down)
		 kb_shift &= ~(0x01 | 0x10);
	  else
		 kb_shift |= (0x01 | 0x10);
   }

   /* Make sure modifier keys are released */
   if ((key_modifiers & RETROKMOD_SHIFT) == 0)
   {
	  release_key(kb_map[RETROK_LSHIFT]);
	  release_key(kb_map[RETROK_LSHIFT]);
   }
   if ((key_modifiers & RETROKMOD_CTRL) == 0)
   {
	  release_key(kb_map[RETROK_LCTRL]);
	  release_key(kb_map[RETROK_RCTRL]);
   }

   /* (a key the table does not have is no key of the Dreamcast's) */
   dc_keycode = keycode < RETROK_LAST ? kb_map[keycode] : 0;
   if (dc_keycode != 0)
   {
	  if (down)
	  {
		 if (kb_used < 6)
		 {
			bool found = false;
			int i;
			for (i = 0; !found && i < 6; i++)
			{
			   if (kb_key[i] == dc_keycode)
				  found = true;
			}
			if (!found)
			{
			   kb_key[kb_used] = dc_keycode;
			   kb_used++;
			}
		 }
	  }
	  else
	  {
		 release_key(dc_keycode);
	  }
   }
}
