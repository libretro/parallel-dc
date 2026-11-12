#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern u32 CCN_QACR_TR[2];

/* A write of @value to store queue @idx's QACR */
void CCN_QACR_write(u32 idx, u32 value);

#ifdef __cplusplus
}
#endif
