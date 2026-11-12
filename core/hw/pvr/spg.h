#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool spg_Init(void);
void spg_Term(void);
void spg_Reset(bool Manual);

/* @reschedule: the next line's event put where the new timing has it */
void CalculateSync(bool reschedule);
double spg_get_refresh_rate(void);
void read_lightgun_position(int x, int y);
/* The game cancelled the gun's occupation of the bus: no end owed at vblank out */
void spg_gun_occupy_cancel(void);
/* Before a read of SPG_STATUS: brings its scanline up to where the beam is */
void spg_sync(void);
/* The beam's state has been put in place from elsewhere (a save state) */
void spg_beam_forget(void);
/* A write to SPG_HBLANK_INT, SPG_VBLANK_INT or SPG_VBLANK */
void spg_write_timing(u32 addr, u32 data);
/* Where a run (a retro_run's frame) ends, spg.cpp: a run starts (dc_run())... */
void spg_frame_begin(void);
/* ...a render to the screen has been started... */
void spg_screen_render(void);
/* ...and a maple transfer: true if it waits for the start of the next run */
bool spg_maple_defer(void);
/* dc_run()'s backstop has stopped the run where it is */
void spg_frame_cut(void);
/* A transfer that asked a light gun where it points is under way */
bool spg_gun_pending(void);
/* A render started: its end comes after what drawing @size bytes of what the
 * game sent takes, or soon if -1 (nothing to draw) */
void SetREP(int size);

/* The beam: in the vertical blank, and the scanline it is on */
extern u32 in_vblank;
extern u32 clc_pvr_scanline;
extern u32 pvr_cur_scanline;
/* the events of the end of a render and of the next line (sh4_sched) */
extern int render_end_schid;
extern int vblank_schid;

/* The light gun in a save state: since V21, and since V28 */
void spg_gun_serialize(void **data, unsigned int *total_size);
void spg_gun_unserialize(void **data, unsigned int *total_size);
void spg_gun_before_v21(void);
void spg_gun_serialize_v28(void **data, unsigned int *total_size);
void spg_gun_unserialize_v28(void **data, unsigned int *total_size);
void spg_gun_before_v28(void);

#ifdef __cplusplus
}
#endif
