/* The maple devices' part of a state, and what of the maple devices the rest
 * of the machine's state names, for C and C++ alike. */
#ifndef MAPLE_STATE_H
#define MAPLE_STATE_H
#include "types.h"
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* The NAOMI's (I/O board's) serial EEPROM, and whether it was read from its file */
extern char EEPROM[0x100];
extern bool EEPROM_loaded;

/* Every device on every port, its type and its own part */
void mcfg_SerializeDevices(void **data, unsigned int *total_size);
void mcfg_UnserializeDevices(void **data, unsigned int *total_size);
/* ...as a V1 state has them: each device there made anew, of the type it
 * is, and its part read */
void mcfg_UnserializeDevicesV1(void **data, unsigned int *total_size);

RETRO_END_DECLS

#endif
