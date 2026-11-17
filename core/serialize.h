/* The parts of a state the machine's devices write and read for
 * serialize.c, each in a block of its own: what each part has had since
 * the version that brought it. */
#ifndef CORE_SERIALIZE_H
#define CORE_SERIALIZE_H
#include "types.h"
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* What each part of the machine has had in a state since V21, in a block of
 * its own at the end: written by X_serialize_v21(), read by
 * X_unserialize_v21() - which answers false if the rest of the state cannot
 * be found after what it read - and, for a state from before V21, set to
 * what such a state implies by X_state_before_v21(). */
#define V21_PART(x) \
	void x##_serialize_v21(void **data, unsigned int *total_size); \
	bool x##_unserialize_v21(void **data, unsigned int *total_size); \
	void x##_state_before_v21(void)
V21_PART(aica);
V21_PART(gdrom);
V21_PART(maple);
V21_PART(modem);
V21_PART(reios);
V21_PART(gdhle);
V21_PART(nbios);
V21_PART(pvr);
#undef V21_PART
/* and since V22: which way the MMU is done, and the translations Windows
 * CE's way of it keeps (fastmmu.cpp), and whether the game has written to
 * the framebuffer since it was last shown (Renderer_if.c) */
void pvr_serialize_v22(void **data, unsigned int *total_size);
void pvr_unserialize_v22(void **data, unsigned int *total_size);
void pvr_state_before_v22(void);
/* and since V23: the NAOMI 2 geometry processor's clipping planes
 * (elan_host.c), the NAOMI's communication board and the I/O board's
 * program on its way (naomi.h), and the libretro layer's mouse motion
 * not yet read and sound not yet sent (libretro.cpp) */
void pvr_serialize_v23(void **data, unsigned int *total_size);
bool pvr_unserialize_v23(void **data, unsigned int *total_size);
void pvr_state_before_v23(void);
void lr_serialize_v23(void **data, unsigned int *total_size);
bool lr_unserialize_v23(void **data, unsigned int *total_size);
void lr_state_before_v23(void);
/* and since V24: the network device in use and what the modem or the
 * Broadband Adapter holds (netstate.cpp), the VMU's beep (vmu_beep.cpp),
 * the NAOMI's serial port and the card readers, touchscreen and hopper on
 * it (naomi_periph.h), and a System SP's own hardware (systemsp.c) */
void systemsp_serialize_v24(void **data, unsigned int *total_size);
bool systemsp_unserialize_v24(void **data, unsigned int *total_size);
void systemsp_state_before_v24(void);
/* and since V25: the dinokich IC card reader of a System SP (systemsp.c),
 * what a Mushiking I/O board holds back (maple_devs.cpp), and the DIMM
 * board of a NAOMI GD-ROM game (gdcartridge.h) */
void systemsp_serialize_v25(void **data, unsigned int *total_size);
bool systemsp_unserialize_v25(void **data, unsigned int *total_size);
void systemsp_state_before_v25(void);
/* and since V26: the NAOMI's two X76F100 serial flash chips, the main
 * board's and the ROM board's (naomi.c) */
void naomi_serial_serialize_v26(void **data, unsigned int *total_size);
bool naomi_serial_unserialize_v26(void **data, unsigned int *total_size);
void naomi_serial_state_before_v26(void);
/* and since V27: what the racing and other I/O boards keep (maple_devs.cpp),
 * the MIDI force feedback board of the driving games (midiffb.cpp: its
 * header), and F355's printer status (sb_mem.c) */
/* and since V28: a light gun's transfer owed its end at vblank-out
 * (spg.cpp) and how many renders ago each TA context was last used
 * (ta_ctx.c) */
void pvr_serialize_v28(void **data, unsigned int *total_size);
bool pvr_unserialize_v28(void **data, unsigned int *total_size);
void pvr_state_before_v28(void);
/* and since V29: SB_ADST, the AICA DMA's start register, which is read
 * through a function and so is not in the register block (aica_if.cpp) */
/* and since V30: the cards the NAOMI's card readers and IC card readers
 * keep out of the reader, as the game last wrote them (card_reader.h) */
/* and since V31: the network DIMM board of a NAOMI GD-ROM game
 * (gdcartridge.h) */

RETRO_END_DECLS

#endif
