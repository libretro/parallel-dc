/* What a NAOMI GD-ROM system's DIMM board leaves in the NAOMI's memory:
 * at start-up its configuration, firmware version and serial number, and
 * once a second its answer to a request of the BIOS's or the game's.
 * Two boards: the plain one (firmware 1.02, no network) and the network
 * one (firmware 3.17), which some games ask for by its version and stop
 * without. The network itself is not there: the network board's
 * settings are a fixed address on a LAN with no one on it. C89. */
#ifndef NAOMI_DIMM_H
#define NAOMI_DIMM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The NAOMI's memory, as the board writes it. */
typedef void (*naomi_dimm_write32_t)(uint32_t addr, uint32_t value);
typedef void (*naomi_dimm_write8_t)(uint32_t addr, uint8_t value);

/* Where in the NAOMI's memory: the configuration, the requests, the
 * firmware version, the serial number, the lines of a test's report. */
#define NAOMI_DIMM_CONFIG   0x0c01fc04u
#define NAOMI_DIMM_REQUEST  0x0c01fc08u
#define NAOMI_DIMM_VERSION  0x0c01fc0cu
#define NAOMI_DIMM_SERIAL   0x0c01fc40u
#define NAOMI_DIMM_REPORT   0x0c01fd00u

/* At start-up: @mem_size bytes of game memory (a power of two), @serial
 * the 16 bytes of the ROM board's serial number. */
void naomi_dimm_startup(int net, uint32_t mem_size, const uint8_t *serial,
      naomi_dimm_write32_t w32);

/* The answer to @request, the word at NAOMI_DIMM_REQUEST, which is
 * written back with what was asked for cleared. Nothing for 0. */
void naomi_dimm_answer(int net, uint32_t request,
      naomi_dimm_write32_t w32, naomi_dimm_write8_t w8);

#ifdef __cplusplus
}
#endif

#endif
