/* The SH4's serial port with a device on it (core/hw/sh4/modules/serial.c)
 * and the touch panel that is one (core/hw/naomi/touchscreen.c), by
 * themselves: the scheduler, the registers' table, the interrupt
 * controller and the players' guns are stand-ins here. A game that talks
 * to the panel through the port's registers - at random rates, FIFO
 * triggers and resets, reading as it likes - with the players moving and
 * shooting, and the two saved and loaded on the way. What the game reads,
 * the interrupts it is given and what the states hold are hashed, and must
 * be what they always were (scif_touch_expected.txt). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_interrupts.h"

retro_log_printf_t log_cb;

static u32 hash = 2166136261u;

static void mix(u32 v)
{
   int i;
   for (i = 0; i < 4; i++)
   {
      hash ^= (v >> (i * 8)) & 0xff;
      hash *= 16777619u;
   }
}

/* The scheduler: the events, when they are due */

sched_list sch_list[SH4_SCHED_SLOTS];
u32 sch_count;
static s64 due[SH4_SCHED_SLOTS];
static int asked[SH4_SCHED_SLOTS];
static u64 now;

int sh4_sched_register(int tag, sh4_sched_callback *ssc)
{
   u32 i;
   for (i = 0; i < sch_count; i++)
      if (sch_list[i].cb == ssc && sch_list[i].tag == tag)
      {
         due[i] = -1;
         return (int)i;
      }
   sch_list[sch_count].cb = ssc;
   sch_list[sch_count].tag = tag;
   sch_list[sch_count].start = sch_list[sch_count].end = -1;
   due[sch_count] = -1;
   return (int)sch_count++;
}

void sh4_sched_request(int id, int cycles)
{
   mix(0x51000000u | (u32)id);
   mix((u32)cycles);
   if (cycles < 0)
   {
      due[id] = -1;
      sch_list[id].start = sch_list[id].end = -1;
      return;
   }
   due[id] = (s64)(now + cycles);
   asked[id] = cycles;
   sch_list[id].start = (int)now;
   sch_list[id].end = (int)(now + cycles);
}

bool sh4_sched_is_scheduled(int id)
{
   return due[id] >= 0;
}

/* (a state's event: due when it said, or now if that has gone by) */
void periph_sched_restore(int id, s32 start, s32 end)
{
   mix(0x52000000u | (u32)id);
   mix((u32)start);
   mix((u32)end);
   if (id < 0)
      return;
   if (end < 0 || start < 0)
      sh4_sched_request(id, -1);
   else
      sh4_sched_request(id, end > (s32)now ? end - (s32)now : 0);
}

static void advance(u32 cycles)
{
   const u64 until = now + cycles;
   for (;;)
   {
      int next = -1;
      u32 i;
      for (i = 0; i < sch_count; i++)
         if (due[i] >= 0 && (u64)due[i] <= until && (next < 0 || due[i] < due[next]))
            next = (int)i;
      if (next < 0)
         break;
      now = (u64)due[next];
      due[next] = -1;
      {
         int r = sch_list[next].cb(sch_list[next].tag, asked[next], 0);
         mix(0x53000000u | (u32)next);
         mix((u32)r);
         if (r > 0)
            sh4_sched_request(next, r);
      }
   }
   now = until;
}

/* The registers' table */

RegisterStruct SCIF[SH4_SCIF_REGS];

void sh4_rio_reg(RegisterStruct *arr, u32 addr, RegIO flags, u32 sz, RegReadAddrFP *rf, RegWriteAddrFP *wf)
{
   RegisterStruct *r = &arr[(addr & 255) / 4];
   r->flags = flags;
   if (rf)
      r->readFunctionAddr = rf;
   r->writeFunctionAddr = wf;
}

static u32 reg_read(u32 addr)
{
   return SCIF[(addr & 255) / 4].readFunctionAddr(addr);
}

static void reg_write(u32 addr, u32 v)
{
   SCIF[(addr & 255) / 4].writeFunctionAddr(addr, v);
}

/* The interrupt controller: what it is told */

void SetInterruptPend(InterruptID intr) { mix(0x61000000u | (u32)intr); }
void ResetInterruptPend(InterruptID intr) { mix(0x62000000u | (u32)intr); }
void SetInterruptMask(InterruptID intr) { mix(0x63000000u | (u32)intr); }
void ResetInterruptMask(InterruptID intr) { mix(0x64000000u | (u32)intr); }

/* The players */

static u32 kcode[2];
static f32 pos_x[2], pos_y[2];

u32 mcfg_ArcadeKcode(int port) { return kcode[port]; }

void mcfg_ArcadeAbsPos(int port, f32 *x, f32 *y)
{
   *x = pos_x[port];
   *y = pos_y[port];
}

/* A save state's */
static u8 state[4096];

bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
   memcpy(*dest, src, src_size);
   *dest = (u8 *)*dest + src_size;
   *total_size += src_size;
   return true;
}

bool ra_unserialize(void *dest, unsigned int size, void **src, unsigned int *total_size)
{
   memcpy(dest, *src, size);
   *src = (u8 *)*src + size;
   *total_size += size;
   return true;
}

/* What the port and the panel are asked, either way they are made */
void glue_serial_init(void);
void glue_serial_reset(void);
void glue_touch_init(void);
void glue_touch_term(void);
void glue_serialize(void **data, unsigned int *total_size);
bool glue_unserialize(void **data, unsigned int *total_size);
void glue_state_before_v24(void);

#ifndef SCIF_TEST_OLD
#include "hw/sh4/modules/modules.h"
#include "hw/naomi/touchscreen.h"
void glue_serial_init(void) { serial_init(); }
void glue_serial_reset(void) { serial_reset(); }
void glue_touch_init(void) { touchscreen_init(); }
void glue_touch_term(void) { touchscreen_term(); }
void glue_serialize(void **data, unsigned int *total_size)
{
   scif_serialize_v24(data, total_size);
   touchscreen_serialize_v24(data, total_size);
}
bool glue_unserialize(void **data, unsigned int *total_size)
{
   return scif_unserialize_v24(data, total_size) && touchscreen_unserialize_v24(data, total_size);
}
void glue_state_before_v24(void)
{
   scif_state_before_v24();
   touchscreen_state_before_v24();
}
#endif

/* The run */

static u32 seed = 4321;
/* bytes the game read from the panel */
static u32 received;

static u32 rnd(void)
{
   seed = seed * 1103515245u + 12345u;
   return (seed >> 8) & 0xffffff;
}

static void save_load(void)
{
   void *p = state;
   unsigned int size = 0, size2 = 0, i;
   glue_serialize(&p, &size);
   mix(size);
   for (i = 0; i < size; i++)
      mix(state[i]);
   /* (what a load finds is what was saved, whatever the panel was doing) */
   for (i = 0; i < (rnd() & 3); i++)
      advance(rnd() % 50000);
   p = state;
   mix(glue_unserialize(&p, &size2));
   mix(size2);
}

int main(void)
{
   static const u8 rates[] = { 0x0c, 0x19, 0x40, 0x81 };
   int i;

   glue_serial_init();
   glue_serial_reset();
   glue_touch_init();
   for (i = 0; i < 400000; i++)
   {
      const u32 op = rnd() % 32;
      mix(op);
      switch (op)
      {
      case 0:
         /* the line's speed and frame */
         SCIF_SCBRR2 = rates[rnd() % 4];
         SCIF_SCSMR2.full = (u16)(rnd() & 0x6b);
         break;
      case 1:
         reg_write(SCIF_SCSCR2_addr, (rnd() & 1) ? 0xf8 : rnd() & 0xff);
         break;
      case 2:
         /* FIFO triggers, now and then a FIFO reset */
         reg_write(SCIF_SCFCR2_addr, (rnd() & 0xf0) | ((rnd() & 7) == 0 ? (rnd() & 6) : 0));
         break;
      case 3: case 4:
         reg_write(SCIF_SCFTDR2_addr, (rnd() & 1) ? 0x39 : rnd() & 0xff);
         break;
      case 5: case 6: case 7:
         mix(reg_read(SCIF_SCFSR2_addr));
         break;
      case 8: case 9:
         /* the status bits it has read cleared */
         reg_write(SCIF_SCFSR2_addr, (rnd() & 1) ? 0 : rnd() & 0xff);
         break;
      case 10: case 11: case 12: case 13:
      {
         u32 n = reg_read(SCIF_SCFDR2_addr);
         mix(n);
         /* (not always all of it: the FIFO fills to its trigger, and over) */
         n &= 0x1f;
         if (n > (rnd() & 7))
            n = rnd() & 7;
         while (n-- > 0)
         {
            mix(reg_read(SCIF_SCFRDR2_addr));
            received++;
         }
         break;
      }
      case 14:
         mix(reg_read(SCIF_SCSCR2_addr));
         /* the overrun, which the game clears */
         mix(SCIF_SCLSR2.full);
         if (rnd() & 1)
            SCIF_SCLSR2.full = 0;
         break;
      case 15:
      {
         const int pl = (int)(rnd() & 1);
         kcode[pl] = rnd() | 0xfc00u;
         pos_x[pl] = (f32)((int)(rnd() % 1400) - 200);
         pos_y[pl] = (f32)((int)(rnd() % 1400) - 200);
         break;
      }
      case 16:
         if ((rnd() & 15) == 0)
            save_load();
         break;
      case 17:
         if ((rnd() & 63) == 0)
            glue_state_before_v24();
         break;
      case 18:
         if ((rnd() & 255) == 0)
         {
            glue_touch_term();
            glue_touch_init();
         }
         break;
      case 19:
         if ((rnd() & 255) == 0)
            glue_serial_reset();
         break;
      case 20: case 21:
      {
         /* a driver's receive interrupt: the FIFO emptied, then RDF and DR cleared */
         u32 st = reg_read(SCIF_SCFSR2_addr), n;
         mix(st);
         if (st & 3)
         {
            n = reg_read(SCIF_SCFDR2_addr) & 0x1f;
            mix(n);
            while (n-- > 0)
            {
               mix(reg_read(SCIF_SCFRDR2_addr));
               received++;
            }
            reg_write(SCIF_SCFSR2_addr, 0xfc);
            mix(reg_read(SCIF_SCFSR2_addr));
         }
         break;
      }
      default:
         advance(rnd() % 30000);
         break;
      }
      if (i % 40000 == 39999)
         printf("%6d: %08x\n", i + 1, (unsigned)hash);
   }
   printf("%u bytes received\n", (unsigned)received);
   glue_touch_term();
   return 0;
}
