#pragma once
#include "types.h"
#include "hw/arm7/arm7.h"

#ifdef __cplusplus
extern "C" {
#endif

extern u32 VREG;
/* The ARM's reset, the clock's write enable, and SB_ADST (aica_if.c) */
extern u32 ARMRST;
extern u32 rtc_EN;
extern u32 sb_adst;
/* the events of the end of an AICA DMA and of the clock's next second */
extern int dma_sched_id;
extern int rtc_schid;
/* The machine's clock, in seconds since 1950 (aica_if.cpp) */
extern u32 RealTimeClock;
extern VArray2 aica_ram;
u32 GetRTC_now(void);
u32 ReadMem_aica_rtc(u32 addr,u32 sz);
void WriteMem_aica_rtc(u32 addr,u32 data,u32 sz);
u32 ReadMem_aica_reg(u32 addr,u32 sz);
void WriteMem_aica_reg(u32 addr,u32 data,u32 sz);

void aica_Init(void);
void aica_Reset(bool hard);

void aica_sb_Init(void);
void aica_sb_Reset(bool hard);
void aica_sb_Term(void);
/* SB_ADST in a save state, from V29 */
void aica_sb_serialize_v29(void **data, unsigned int *total_size);
bool aica_sb_unserialize_v29(void **data, unsigned int *total_size);
void aica_sb_state_before_v29(void);

s32 libAICA_Init(void);
void libAICA_Reset(bool hard);
void libAICA_Term(void);

#ifdef __cplusplus
}
#endif
