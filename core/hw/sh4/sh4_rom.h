#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FSCA's table: the sine and the cosine of each of 65536 angles */
typedef struct f32_x2
{
	f32 u[2];
} f32_x2;
extern f32_x2 sin_table[0x10000];

/* Fills it in (mem_Init(), before anything runs) */
void sh4rom_init(void);

#ifdef __cplusplus
}
#endif
