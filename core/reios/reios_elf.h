#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the ELF file into main memory. @entry gets the address it says it
 * starts at. */
bool reios_loadElf(const char *path, u32* entry);

#ifdef __cplusplus
}
#endif
