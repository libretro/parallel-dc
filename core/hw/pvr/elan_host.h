/* The NAOMI 2's geometry processor in the machine. See elan_host.c. */
#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern u8 *elan_ram;       /* the chip's 32 MB, when the machine is a NAOMI 2 */
extern int elan_schid;     /* the end of a texture transfer */

void elan_host_init(void);
void elan_host_term(void);
void elan_host_reset(bool hard);
void elan_host_map_init(void);
void elan_host_map(u32 base);

/* the save state from V23 (elan_host.c) */
void pvr_serialize_v23(void **data, unsigned int *total_size);
bool pvr_unserialize_v23(void **data, unsigned int *total_size);
void pvr_state_before_v23(void);

#ifdef __cplusplus
}
#endif
