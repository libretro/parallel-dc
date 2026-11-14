#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void map_area0_init(void);
void map_area0(u32 base);
const void *area0_read_const(void *handler, u32 addr, u32 sz);

/* F355's printer's status, in a save state since V27 */
void naomi_printer_serialize_v27(void **data, unsigned int *total_size);
bool naomi_printer_unserialize_v27(void **data, unsigned int *total_size);
void naomi_printer_state_before_v27(void);

/* Init/Res/Term */
void sh4_area0_Init(void);
void sh4_area0_Reset(bool Manual);
void sh4_area0_Term(void);

#ifdef __cplusplus
}
#endif
