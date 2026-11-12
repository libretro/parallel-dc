#pragma once
#include "types.h"

#ifdef __cplusplus
#include "maple_devs.h"

extern "C" {
#else
struct maple_device;
typedef struct maple_device maple_device;
#endif

/* The devices on each port's bus: [bus][5] the main one, [bus][0..4] the
 * ones plugged into it (C sees them only as present or not) */
extern maple_device* MapleDevices[4][6];

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
