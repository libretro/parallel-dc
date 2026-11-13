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
/* A vblank transfer waits for its trigger to be reset (in the save state) */
extern bool maple_ddt_pending_reset;
/* The transfer's event (sh4_sched) */
extern int maple_sched;

/* The answers of the transfer under way, kept until it is over: see maple_if.c */
#define MAPLE_OUT_WORDS 4096
extern u32 maple_out[MAPLE_OUT_WORDS];
extern u32 maple_out_used;
/* The transfer is over */
void maple_dma_done(void);
/* Flush pending VMU and EEPROM writes; the machine must be idle. */
void maple_FlushSaves(void);

/* A word of a frame as the bus has it, the other way round */
#define SWAP32(a) ((((a) & 0xff) << 24)  | (((a) & 0xff00) << 8) | (((a) >> 8) & 0xff00) | (((a) >> 24) & 0xff))

/* The most a device answers a frame with, its header included */
#define MAPLE_FRAME_MAX_WORDS 256

/* A device, as C sees it (maple_devs.cpp). maple_dev_dma() answers a frame
 * of in_len bytes into out, MAPLE_FRAME_MAX_WORDS words at the most, and
 * says how many bytes it wrote. */
u32 maple_dev_dma(maple_device *dev, u32 *in, u32 in_len, u32 *out);
/* It has slots for others (a memory card, a vibration pack) */
bool maple_dev_has_slots(maple_device *dev);
/* It is a gun with the bus occupied for its position */
bool maple_dev_lightgun_pos(maple_device *dev);
void maple_dev_flush_save(maple_device *dev);

/* What is plugged in, as the settings say (maple_cfg.cpp) */
void mcfg_CreateDevices(void);
void mcfg_DestroyDevices(void);
void mcfg_ReconcileDevices(bool plug_in);
bool mcfg_DevicesAsSettingsSay(void);

/* What a NAOMI's I/O boards have had in a save state since V21, V25 and
 * V27 (maple_devs.cpp) */
void maple_jvs_serialize_v21(void **data, unsigned int *total_size);
void maple_jvs_unserialize_v21(void **data, unsigned int *total_size);
void maple_jvs_state_before_v21(void);
void maple_jvs_serialize_v25(void **data, unsigned int *total_size);
bool maple_jvs_unserialize_v25(void **data, unsigned int *total_size);
void maple_jvs_state_before_v25(void);
void maple_jvs_serialize_v27(void **data, unsigned int *total_size);
bool maple_jvs_unserialize_v27(void **data, unsigned int *total_size);
void maple_jvs_state_before_v27(void);
void maple_jvs_firmware_reset(void);
/* Maple's own part of a state since V21 */
void maple_serialize_v21(void **data, unsigned int *total_size);
bool maple_unserialize_v21(void **data, unsigned int *total_size);
void maple_state_before_v21(void);
void maple_SB_MDAPRO_Write(u32 addr, u32 data);

#ifdef __cplusplus
}
#endif
