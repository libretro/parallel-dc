/* The DIMM board of a NAOMI GD-ROM system (core/hw/naomi/naomi_dimm.c),
 * run by itself: what it leaves in the NAOMI's memory at start-up and in
 * answer to each request, for the plain board and the network one.
 *
 * The NAOMI's memory is the board's window of it, 0c01fc00-0c01fdff; a
 * write anywhere else is a failure. A game on the network board reads
 * the firmware version from 0c01fc0c and stops unless it is 3.17 or
 * later, at start-up and after every answer.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "naomi_dimm.h"

static int failures;

#define CHECK(what) do { if (!(what)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #what); failures++; } } while (0)

#define WINDOW_BASE 0x0c01fc00u
#define WINDOW_SIZE 0x200u

static uint8_t mem[WINDOW_SIZE];
static unsigned writes;
static unsigned strays;

static void w32(uint32_t addr, uint32_t value)
{
   writes++;
   if (addr < WINDOW_BASE || addr - WINDOW_BASE > WINDOW_SIZE - 4 || (addr & 3))
   {
      strays++;
      return;
   }
   addr -= WINDOW_BASE;
   mem[addr] = (uint8_t)value;
   mem[addr + 1] = (uint8_t)(value >> 8);
   mem[addr + 2] = (uint8_t)(value >> 16);
   mem[addr + 3] = (uint8_t)(value >> 24);
}

static void w8(uint32_t addr, uint8_t value)
{
   writes++;
   if (addr < WINDOW_BASE || addr - WINDOW_BASE >= WINDOW_SIZE)
   {
      strays++;
      return;
   }
   mem[addr - WINDOW_BASE] = value;
}

static uint32_t r32(uint32_t addr)
{
   addr -= WINDOW_BASE;
   return (uint32_t)mem[addr] | ((uint32_t)mem[addr + 1] << 8)
      | ((uint32_t)mem[addr + 2] << 16) | ((uint32_t)mem[addr + 3] << 24);
}

static void clear(void)
{
   memset(mem, 0xa5, sizeof(mem));
   writes = 0;
   strays = 0;
}

static const uint8_t serial[16] = {
   0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
   0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
};

static void check_serial(void)
{
   CHECK(memcmp(mem + (NAOMI_DIMM_SERIAL - WINDOW_BASE), serial, 16) == 0);
}

static void test_startup(void)
{
   static const struct { uint32_t size; uint32_t plain; uint32_t net; } sizes[] = {
      { 0x08000000u, 0x60010080u, 0x70010070u },
      { 0x10000000u, 0x60020100u, 0x700200f0u },
      { 0x20000000u, 0x60030200u, 0x700301f0u },
   };
   unsigned i;

   for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
   {
      clear();
      naomi_dimm_startup(0, sizes[i].size, serial, w32);
      CHECK(strays == 0);
      CHECK(r32(NAOMI_DIMM_CONFIG) == sizes[i].plain);
      CHECK(r32(NAOMI_DIMM_VERSION) == 0x01020264u);
      check_serial();
      /* the plain board has no network settings */
      CHECK(r32(0x0c01fc60u) == 0xa5a5a5a5u);

      clear();
      naomi_dimm_startup(1, sizes[i].size, serial, w32);
      CHECK(strays == 0);
      CHECK(r32(NAOMI_DIMM_CONFIG) == sizes[i].net);
      CHECK(r32(NAOMI_DIMM_CONFIG) & (1u << 28));
      CHECK(r32(NAOMI_DIMM_VERSION) >> 16 >= 0x0317u);
      check_serial();
      CHECK(r32(0x0c01fc14u) == 1);
      CHECK(r32(0x0c01fc18u) == 0x00010002u);
      /* 192.168.1.1/24 through 192.168.1.254, as the NAOMI reads them */
      CHECK(memcmp(mem + 0x60, "\xc0\xa8\x01\x01", 4) == 0);
      CHECK(memcmp(mem + 0x64, "\xff\xff\xff\x00", 4) == 0);
      CHECK(memcmp(mem + 0x68, "\xc0\xa8\x01\xfe", 4) == 0);
      CHECK(memcmp(mem + 0x70, "\x08\x08\x08\x08", 4) == 0);
      CHECK(r32(0x0c01fc94u) == 0);
   }
}

static void test_answers(void)
{
   uint32_t bit;
   int net;

   /* nothing asked: nothing written */
   for (net = 0; net < 2; net++)
   {
      clear();
      naomi_dimm_answer(net, 0, w32, w8);
      CHECK(writes == 0);
   }

   /* the BIOS's DIMM test: its report, and the request taken */
   for (net = 0; net < 2; net++)
   {
      clear();
      naomi_dimm_answer(net, 0x41u, w32, w8);
      CHECK(strays == 0);
      CHECK(r32(NAOMI_DIMM_REQUEST) == 0x40u);
      CHECK(memcmp(mem + 0x100, "CHECKING DIMM BD", 16) == 0);
      CHECK(memcmp(mem + 0x130, "--- COMPLETED---", 16) == 0);
      CHECK(r32(NAOMI_DIMM_VERSION) == (net ? 0x0317a264u : 0x0102a264u));
   }

   /* the network board's requests, each taken on its own */
   clear();
   naomi_dimm_answer(1, 0x40u, w32, w8);
   CHECK(r32(NAOMI_DIMM_REQUEST) == 0);
   CHECK(memcmp(mem + 0x60, "\xc0\xa8\x01\x01", 4) == 0);
   CHECK(r32(NAOMI_DIMM_VERSION) == 0x03170264u);

   clear();
   naomi_dimm_answer(1, 0x10400u, w32, w8);
   CHECK(r32(NAOMI_DIMM_REQUEST) == 0x10000u);
   CHECK(memcmp(mem + 0x70, "\x08\x08\x08\x08", 4) == 0);

   clear();
   naomi_dimm_answer(1, 0x20000u, w32, w8);
   CHECK(r32(NAOMI_DIMM_REQUEST) == 0);
   CHECK(memcmp(mem + 0x100, "CHECKING NETWORK", 16) == 0);
   CHECK(memcmp(mem + 0x120, "--- COMPLETED---", 16) == 0);

   /* the plain board takes the network's requests as any other: all of
    * them answered at once */
   clear();
   naomi_dimm_answer(0, 0x40u, w32, w8);
   CHECK(r32(NAOMI_DIMM_REQUEST) == 0);
   CHECK(r32(0x0c01fc60u) == 0xa5a5a5a5u);
   CHECK(r32(NAOMI_DIMM_VERSION) == 0x03170100u);

   /* whatever is asked, the network board answers as 3.17 or later, and
    * writes only in its window */
   for (bit = 1; bit != 0; bit <<= 1)
   {
      clear();
      naomi_dimm_answer(1, bit, w32, w8);
      CHECK(strays == 0);
      CHECK(r32(NAOMI_DIMM_VERSION) >> 16 >= 0x0317u);
      CHECK((r32(NAOMI_DIMM_REQUEST) & bit) == 0);
   }
}

int main(void)
{
   test_startup();
   test_answers();
   if (failures)
   {
      printf("dimm test: %d failed\n", failures);
      return 1;
   }
   printf("dimm test: ok\n");
   return 0;
}
