#pragma once
#include "aica.h"

#ifdef __cplusplus
extern "C" {
#endif

u32 libAICA_ReadReg(u32 addr,u32 size);
void libAICA_WriteReg(u32 addr,u32 data,u32 size);

void init_mem(void);
void term_mem(void);

extern u8 aica_reg[0x8000];

#ifdef __cplusplus
}
#endif
