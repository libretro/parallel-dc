#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The three timers, as a state has them (tmu.c) */
extern u32 tmu_shift[3];
extern u32 tmu_mask[3];
extern u64 tmu_mask64[3];
extern u32 old_mode[3];
extern int tmu_sched[3];
extern u32 tmu_ch_base[3];
extern u64 tmu_ch_base64[3];

#ifdef __cplusplus
}
#endif
