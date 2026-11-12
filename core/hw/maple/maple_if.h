#pragma once
#include "types.h"

#ifdef __cplusplus
#include "maple_devs.h"

extern maple_device* MapleDevices[4][6];

extern "C" {
#endif

void maple_Init(void);
void maple_Reset(bool Manual);
void maple_Term(void);
void maple_ReconnectDevices(void);

void maple_vblank(void);
/* A transfer the last run left for the start of this one (hw/pvr/spg.c) */
void maple_start_deferred(void);
/* The transfer's event (sh4_sched) */
extern int maple_sched;

/* The answers of the transfer under way, kept until it is over: see maple_if.cpp */
#define MAPLE_OUT_WORDS 4096
extern u32 maple_out[MAPLE_OUT_WORDS];
extern u32 maple_out_used;
/* The transfer is over */
void maple_dma_done(void);
/* Flush pending VMU and EEPROM writes; the machine must be idle. */
void maple_FlushSaves(void);

#ifdef __cplusplus
}
#endif
