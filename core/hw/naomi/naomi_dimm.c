#include <stddef.h>

#include "naomi_dimm.h"

/* Firmware versions as the board leaves them: the version in the top 16
 * bits, then the board's state. */
#define VERSION_PLAIN       0x01020264u
#define VERSION_PLAIN_TEST  0x0102a264u
#define VERSION_NET         0x03170264u
#define VERSION_NET_TEST    0x0317a264u
#define VERSION_ANSWERED    0x03170100u

/* Requests: the BIOS's DIMM test, and the network board's (the game's
 * test mode asking for the address and the DNS server, the BIOS's
 * network settings and its network test). */
#define REQ_DIMM_TEST       0x00000001u
#define REQ_NET_ADDRESS     0x00000040u
#define REQ_NET_DNS         0x00000400u
#define REQ_NET_SETTINGS    0x00010000u
#define REQ_NET_TEST        0x00020000u

/* The network board's settings, in network byte order as the NAOMI
 * reads them: 192.168.1.1/24, gateway and first DNS 192.168.1.254,
 * second DNS 8.8.8.8. */
#define NET_ADDRESS         0x0101a8c0u
#define NET_MASK            0x00ffffffu
#define NET_GATEWAY         0xfe01a8c0u
#define NET_DNS2            0x08080808u

static void write_report(naomi_dimm_write8_t w8, const char *lines)
{
   uint32_t i;
   for (i = 0; i < 64; i++)
      w8(NAOMI_DIMM_REPORT + i, (uint8_t)lines[i]);
}

void naomi_dimm_startup(int net, uint32_t mem_size, const uint8_t *serial,
      naomi_dimm_write32_t w32)
{
   /* bits 16-17 the size of DIMM 0 (128, 256 or 512 MB), bit 28 the
    * network, bit 29 always, bit 30 a GD-ROM drive; the low bits in MB
    * where the game's memory ends (the network board keeps its last
    * 16 MB for its own buffers) */
   uint32_t size_bits = 1u << 16;
   uint32_t i;
   if (mem_size == 0x20000000u)
      size_bits = 3u << 16;
   else if (mem_size == 0x10000000u)
      size_bits = 2u << 16;

   if (net && mem_size > 0x01000000u)
   {
      w32(NAOMI_DIMM_CONFIG, size_bits | 0x70000000u | ((mem_size - 0x01000000u) >> 20));
      w32(NAOMI_DIMM_VERSION, VERSION_NET);
      w32(0x0c01fc10u, 0);
      w32(0x0c01fc14u, 1);
      w32(0x0c01fc18u, 0x00010002u);   /* LAN, DHCP off */
      w32(0x0c01fc20u, 0x00078000u);
      w32(0x0c01fc24u, 0x003e000au);
      w32(0x0c01fc28u, 0x0018077fu);
      w32(0x0c01fc2cu, 0x00010014u);
      w32(0x0c01fc60u, NET_ADDRESS);
      w32(0x0c01fc64u, NET_MASK);
      w32(0x0c01fc68u, NET_GATEWAY);
      w32(0x0c01fc6cu, NET_GATEWAY);
      w32(0x0c01fc70u, NET_DNS2);
      for (i = 0x0c01fc74u; i <= 0x0c01fc94u; i += 4)
         w32(i, 0);
   }
   else
   {
      w32(NAOMI_DIMM_CONFIG, size_bits | 0x60000000u | (mem_size >> 20));
      w32(NAOMI_DIMM_VERSION, VERSION_PLAIN);
   }

   for (i = 0; i < 16; i += 4)
      w32(NAOMI_DIMM_SERIAL + i, (uint32_t)serial[i]
            | ((uint32_t)serial[i + 1] << 8)
            | ((uint32_t)serial[i + 2] << 16)
            | ((uint32_t)serial[i + 3] << 24));
}

void naomi_dimm_answer(int net, uint32_t request,
      naomi_dimm_write32_t w32, naomi_dimm_write8_t w8)
{
   if (request == 0)
      return;
   if (request & REQ_DIMM_TEST)
   {
      w32(NAOMI_DIMM_REQUEST, request & ~REQ_DIMM_TEST);
      write_report(w8, "CHECKING DIMM BD" "DIMM0 - GOOD    " "DIMM1 - GOOD    " "--- COMPLETED---");
      w32(NAOMI_DIMM_VERSION, net ? VERSION_NET_TEST : VERSION_PLAIN_TEST);
   }
   else if (net && (request & REQ_NET_ADDRESS))
   {
      w32(NAOMI_DIMM_REQUEST, request & ~REQ_NET_ADDRESS);
      w32(0x0c01fc60u, NET_ADDRESS);
      w32(NAOMI_DIMM_VERSION, VERSION_NET);
   }
   else if (net && (request & REQ_NET_DNS))
   {
      w32(NAOMI_DIMM_REQUEST, request & ~REQ_NET_DNS);
      w32(0x0c01fc70u, NET_DNS2);
      w32(NAOMI_DIMM_VERSION, VERSION_NET);
   }
   else if (net && (request & REQ_NET_SETTINGS))
   {
      w32(NAOMI_DIMM_REQUEST, request & ~REQ_NET_SETTINGS);
      w32(NAOMI_DIMM_VERSION, VERSION_NET);
   }
   else if (net && (request & REQ_NET_TEST))
   {
      w32(NAOMI_DIMM_REQUEST, request & ~REQ_NET_TEST);
      write_report(w8, "CHECKING NETWORK" "NOT CONNECTED   " "--- COMPLETED---" "                ");
      w32(NAOMI_DIMM_VERSION, VERSION_NET);
   }
   else
   {
      w32(NAOMI_DIMM_REQUEST, 0);
      w32(NAOMI_DIMM_VERSION, VERSION_ANSWERED);
   }
}
