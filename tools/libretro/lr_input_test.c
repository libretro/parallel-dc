/* The frontend's input as the machine has it (core/libretro/lr_input.c),
 * driven at random: machines, ports set to every device, the frontend's
 * sticks, buttons, guns, pointers and mice, the vibration packs and the
 * NAOMI wheel on its rumble, the keyboard, the latch of the threaded
 * renderer, and the input's part of a state written and read back. Every
 * call to the frontend and all that comes of it goes into a hash, which
 * is printed each round. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libretro.h>
#include "types.h"
#include "serialize.h"
#include "libretro/lr_input.h"
#include "libretro/pad_state.h"
#include "libretro/input_latch.h"
#include "hw/maple/maple_devs.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/naomi_periph.h"
#include "hw/naomi/midiffb.h"
#include "oslib/oslib.h"
#include "oslib/audiostream.h"

/* What lr_input.c takes from the rest of the core */
u32 kcode[4];
u8 rt[4], lt[4];
s8 joyx[4], joyy[4], joyrx[4], joyry[4];
f32 mo_x_abs[4], mo_y_abs[4];
u32 mo_buttons[4];
f32 mo_x_delta[4], mo_y_delta[4], mo_wheel_delta[4];
u8 kb_shift, kb_led, kb_key[6];
lightgun_params_t lightgun_params[4];
MapleDeviceType maple_devices[MAPLE_PORTS];
InputDescriptors *naomi_game_inputs;
bool naomi_card_button;
settings_t settings;

static u32 hash = 2166136261u;
static void mix(u32 v)
{
   int i;
   for (i = 0; i < 4; i++)
   {
      hash ^= v & 0xff;
      hash *= 16777619u;
      v >>= 8;
   }
}
static void mixf(f32 f)
{
   u32 v;
   memcpy(&v, &f, 4);
   mix(v);
}

static u32 rng = 1;
static u32 rnd(void)
{
   rng ^= rng << 13;
   rng ^= rng >> 17;
   rng ^= rng << 5;
   return rng;
}

static double now;
double os_GetSeconds(void)
{
   mix(0x7100);
   return now;
}
void AudioHoldbackSerialize(void **data, unsigned int *total_size)
{
   (void)data;
   mix(0x7101);
   mix(*total_size);
}
void AudioHoldbackUnserialize(void **data, unsigned int *total_size)
{
   (void)data;
   mix(0x7102);
   mix(*total_size);
}
void ResetAudioBuffer(void)
{
   mix(0x7103);
}
bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
   if (*dest != NULL && src_size != 0)
   {
      memcpy(*dest, src, src_size);
      *dest = (unsigned char *)*dest + src_size;
   }
   *total_size += src_size;
   return true;
}
bool ra_unserialize(void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
   if (*dest != NULL && src_size != 0)
   {
      memcpy(src, *dest, src_size);
      *dest = (unsigned char *)*dest + src_size;
   }
   *total_size += src_size;
   return true;
}

/* The frontend: what a call is asked gives the same answer all frame */
static u32 frame_seed;
static u32 answer(unsigned port, unsigned device, unsigned index, unsigned id)
{
   u32 h = frame_seed ^ (port * 0x9e3779b9u) ^ (device * 0x85ebca6bu) ^ (index * 0xc2b2ae35u) ^ (id * 0x27d4eb2fu);
   h ^= h >> 15;
   h *= 0x2c1b3c6du;
   h ^= h >> 12;
   h *= 0x297a2d39u;
   h ^= h >> 15;
   return h;
}
static int16_t fake_input(unsigned port, unsigned device, unsigned index, unsigned id)
{
   u32 h = answer(port, device, index, id);
   int16_t v;
   mix(0x7200 | port);
   mix(device);
   mix(index);
   mix(id);
   switch (device & RETRO_DEVICE_MASK)
   {
   case RETRO_DEVICE_ANALOG:
      switch (h & 7)
      {
      case 0: v = 0; break;
      case 1: v = -32768; break;
      case 2: v = 32767; break;
      case 3: v = (int16_t)((h >> 16) & 0x3ff) - 512; break;
      default: v = (int16_t)(h >> 16); break;
      }
      break;
   case RETRO_DEVICE_POINTER:
      if (id == RETRO_DEVICE_ID_POINTER_COUNT)
         v = (int16_t)((h >> 8) % 4);
      else if (id == RETRO_DEVICE_ID_POINTER_PRESSED)
         v = (int16_t)((h >> 8) & 1);
      else
         v = (int16_t)(h >> 16);
      break;
   case RETRO_DEVICE_LIGHTGUN:
      if (id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X || id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y)
         v = (int16_t)(h >> 16);
      else
         v = (int16_t)((h >> 8) % 5 == 0);
      break;
   case RETRO_DEVICE_MOUSE:
      if (id == RETRO_DEVICE_ID_MOUSE_X || id == RETRO_DEVICE_ID_MOUSE_Y)
         v = (int16_t)((int)((h >> 16) & 0xff) - 128);
      else if (id == RETRO_DEVICE_ID_MOUSE_WHEELUP || id == RETRO_DEVICE_ID_MOUSE_WHEELDOWN)
         v = (int16_t)((h >> 8) % 7 == 0);
      else
         v = (int16_t)((h >> 8) & 1);
      break;
   default:
      if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         v = (int16_t)(h >> 16);
      else
         v = (int16_t)((h >> 8) % 3 == 0);
      break;
   }
   mix((u32)(u16)v);
   return v;
}

static bool fake_rumble(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
   mix(0x7300 | port);
   mix((u32)effect);
   mix(strength);
   return true;
}

static InputDescriptors inputs;

static const MapleDeviceType kinds[] = {
   MDT_SegaController, MDT_SegaVMU, MDT_Microphone, MDT_PurupuruPack, MDT_AsciiStick,
   MDT_Keyboard, MDT_Mouse, MDT_LightGun, MDT_TwinStick, MDT_NaomiJamma, MDT_None
};
static const unsigned devices[] = {
   RETRO_DEVICE_NONE, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_ANALOG, RETRO_DEVICE_LIGHTGUN,
   RETRO_DEVICE_POINTER, RETRO_DEVICE_MOUSE, RETRO_DEVICE_KEYBOARD, RETRO_DEVICE_TWINSTICK,
   RETRO_DEVICE_TWINSTICK_SATURN, RETRO_DEVICE_ASCIISTICK
};
static const int systems[] = {
   DC_PLATFORM_DREAMCAST, DC_PLATFORM_NAOMI, DC_PLATFORM_NAOMI2, DC_PLATFORM_ATOMISWAVE
};

static void mix_state(void)
{
   u32 p;
   int i;
   for (p = 0; p < 4; p++)
   {
      mix(kcode[p]);
      mix(rt[p] | lt[p] << 8 | (u8)joyx[p] << 16 | (u32)(u8)joyy[p] << 24);
      mix((u8)joyrx[p] | (u8)joyry[p] << 8);
      mixf(mo_x_abs[p]);
      mixf(mo_y_abs[p]);
      mix(mo_buttons[p]);
      mixf(mo_x_delta[p]);
      mixf(mo_y_delta[p]);
      mixf(mo_wheel_delta[p]);
      mix(lightgun_params[p].offscreen != 0);
      mixf(lightgun_params[p].x);
      mixf(lightgun_params[p].y);
   }
   mix(kb_shift);
   for (i = 0; i < 6; i++)
      mix(kb_key[i]);
   mix(mouse_ports | mouse_ports_read << 8 | (arcade_input_read != 0) << 16);
}

/* The emulation thread's side of the latch */
static void read_latch(void)
{
   u32 p;
   for (p = 0; p < 4; p++)
   {
      const input_latch_port *in = input_latch_read(&input_latch, p);
      u32 buttons;
      f32 dx, dy, dw;
      mix(in->kcode);
      mix((u8)in->joyx | (u8)in->joyy << 8 | (u8)in->joyrx << 16 | (u32)(u8)in->joyry << 24);
      mix(in->rt | in->lt << 8);
      if (rnd() & 1)
      {
         input_latch_read_mouse(&input_latch, p, &buttons, &dx, &dy, &dw);
         mix(buttons);
         mixf(dx);
         mixf(dy);
         mixf(dw);
      }
   }
}

static void settle(void)
{
   int i;
   settings.System = systems[rnd() % 4];
   settings.rend.ThreadedRendering = rnd() % 2 != 0;
   settings.rend.WideScreen = rnd() % 2 != 0;
   settings.mapping.JammaSetup = rnd() % 3 == 0 ? JVS_LightGun : rnd() % 2 ? JVS_LightGunAsAnalog : (enum JVS)(rnd() % 8);
   for (i = 0; i < 4; i++)
   {
      maple_devices[i] = kinds[rnd() % (sizeof(kinds) / sizeof(kinds[0]))];
      lr_device_type[i] = (int)devices[rnd() % (sizeof(devices) / sizeof(devices[0]))];
   }
   input_set_deadzone_stick((int)(rnd() % 120) - 10);
   input_set_deadzone_trigger((int)(rnd() % 120) - 10);
   if (rnd() % 3 == 0)
   {
      input_set_deadzone_stick(0);
      input_set_deadzone_trigger(0);
   }
   digital_triggers = rnd() % 2 != 0;
   allow_service_buttons = rnd() % 2 != 0;
   libretro_supports_bitmasks = rnd() % 2 != 0;
   naomi_ffb_enabled = rnd() % 2 != 0;
   wide_by_cheat = rnd() % 2 != 0;
   gl_ctx_resetting = rnd() % 16 == 0;
   naomi_card_button = rnd() % 2 != 0;
   lr_rumble.set_rumble_state = rnd() % 5 == 0 ? NULL : fake_rumble;
   if (rnd() % 2)
      naomi_game_inputs = NULL;
   else
   {
      int n = (int)(rnd() % 8);
      static const char *names[8] = { "A", "B", "C", "D", "E", "F", "G", "H" };
      memset(&inputs, 0, sizeof(inputs));
      for (i = 0; i < n; i++)
      {
         inputs.axes[i].name = names[i];
         inputs.axes[i].type = rnd() % 2 ? Half : Full;
         inputs.axes[i].axis = rnd() % 7;
         inputs.axes[i].inverted = rnd() % 2 != 0;
      }
      naomi_game_inputs = &inputs;
   }
}

int main(void)
{
   int round;
   static u8 state[256];

   lr_input_init();
   lr_input_init();
   input_cb = fake_input;
   memset(kcode, 0xff, sizeof(kcode));
   memset(mo_buttons, 0xff, sizeof(mo_buttons));
   now = 1000.0;

   /* Every button of every device, on every machine, with and without a card reader */
   {
      static const int all[] = {
         DC_PLATFORM_DREAMCAST, DC_PLATFORM_DEV_UNIT, DC_PLATFORM_NAOMI, DC_PLATFORM_NAOMI2, DC_PLATFORM_ATOMISWAVE
      };
      unsigned s, d, id;
      for (s = 0; s < 5; s++)
         for (d = 0; d < 16; d++)
            for (id = 0; id < 24; id++)
            {
               settings.System = all[s];
               naomi_card_button = (id & 1) != 0;
               allow_service_buttons = (d & 1) != 0;
               mix(map_gamepad_button(d, id));
            }
      printf("buttons %08x\n", (unsigned)hash);
   }

   for (round = 0; round < 3000; round++)
   {
      int op;
      /* (the core started again: the latch goes on as it was) */
      if (round % 1000 == 500)
         lr_input_init();
      settle();
      for (op = 0; op < 40; op++)
      {
         u32 p = rnd() % 4;
         u32 r = rnd() % 100;
         frame_seed = rnd();
         if (rnd() % 4 == 0)
            now += (rnd() % 400) / 1000.0;
         if (r < 25)
            UpdateInputState(p);
         else if (r < 35)
         {
            CaptureInput();
            read_latch();
         }
         else if (r < 42)
         {
            if (rnd() % 2)
               arcade_input_read = false;
            UpdateInputStateArcade();
         }
         else if (r < 50)
         {
            if (rnd() % 2)
               mouse_ports_read = 0;
            UpdateInputStateMouse(p);
         }
         else if (r < 60)
         {
            u32 v = rnd();
            if (rnd() % 3 == 0)
               v &= ~0xff000000u;
            if (rnd() % 3 == 0)
               v &= ~0x00ff0000u;
            UpdateVibration(p, v, rnd() % 2000);
         }
         else if (r < 67)
         {
            static const int ffb[] = { NAOMI_FFB_FORCE, NAOMI_FFB_SHAKE, NAOMI_FFB_STOP };
            float level = (float)((int)(rnd() % 300) - 50) / 200.f;
            naomi_ffb_rumble(ffb[rnd() % 3], level, rnd() % 4000);
         }
         else if (r < 69)
            naomi_ffb_clear();
         else if (r < 71)
            rumble_stop();
         else if (r < 85)
         {
            unsigned key = rnd() % (RETROK_LAST + 8);
            if (rnd() % 4 == 0)
               key = rnd() % 2 ? RETROK_LSHIFT : RETROK_RCTRL;
            retro_keyboard_event(rnd() % 2 != 0, key, rnd(), (uint16_t)(rnd() & 0xffff));
         }
         else if (r < 93)
         {
            void *data = state;
            unsigned int total = 0;
            int i;
            lr_serialize_v23(&data, &total);
            mix(total);
            for (i = 0; i < (int)total; i++)
               mix(state[i]);
            if (rnd() % 3 == 0)
               state[rnd() % total] = (u8)rnd();
            if (rnd() % 5 == 0)
            {
               f32 big = (rnd() % 2) ? 1e30f : -2e6f;
               memcpy(state + (rnd() % 12) * 4, &big, 4);
            }
            data = state;
            total = 0;
            mix(lr_unserialize_v23(&data, &total));
            mix(total);
         }
         else if (r < 95)
            lr_state_before_v23();
         else
            settle();
         mix_state();
      }
      if (round % 100 == 99)
         printf("round %d %08x\n", round + 1, (unsigned)hash);
   }
   /* The wheel's shake run down to the millisecond, the longest one, and
    * the same shake asked for again and again before the frontend sees it */
   {
      int k, t;
      settings.System = DC_PLATFORM_NAOMI;
      naomi_ffb_enabled = true;
      gl_ctx_resetting = false;
      lr_rumble.set_rumble_state = fake_rumble;
      for (k = 0; k < 6; k++)
      {
         naomi_ffb_rumble(NAOMI_FFB_SHAKE, 0.5f, 2530 + k * 10);
         UpdateInputState(0);
         for (t = 0; t < 2600; t += 1)
         {
            now += 0.001;
            frame_seed = (u32)t;
            UpdateInputState(0);
         }
         mix_state();
      }
      for (k = 0; k < 200; k++)
      {
         int n;
         for (n = 0; n < k; n++)
            naomi_ffb_rumble(NAOMI_FFB_SHAKE, 0.25f, 100);
         UpdateInputState(0);
      }
      printf("wheel %08x\n", (unsigned)hash);
   }
   printf("done %08x\n", (unsigned)hash);
   return 0;
}
