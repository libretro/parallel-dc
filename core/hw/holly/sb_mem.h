#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void map_area0_init(void);
void map_area0(u32 base);
const void *area0_read_const(void *handler, u32 addr, u32 sz);

/* Init/Res/Term */
void sh4_area0_Init(void);
void sh4_area0_Reset(bool Manual);
void sh4_area0_Term(void);

#ifdef __cplusplus
}
#endif
