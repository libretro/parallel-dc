#ifndef EMU_BATON_H
#define EMU_BATON_H
/* Who has the emulated machine, and how a frame gets made.
 *
 * In threaded rendering the machine is emulated on its own thread and its
 * frames are rendered on the libretro thread, but nothing runs free: every
 * retro_run() emulates exactly one frame, the same one non-threaded
 * rendering would, and returns when it is done.
 *
 *   libretro thread                emulation thread
 *   hold()                         (asleep in wait_for_frame())
 *   start_frame()          ---->   runs the machine up to vblank
 *   wait_work() gets a     <----   hand_over(render): asleep until...
 *     render, reads what it
 *     needs of the guest's
 *     memory, ack_work()   ---->   ...the guest may carry on
 *     draws it, work_done()
 *   wait_work() gets NULL  <----   end_frame()
 *   audio, video, release()
 *
 * (each an emu_baton_ function) Between ack_work() and the end of the
 * frame the two threads run side by side: one draws, the other emulates.
 *
 * Any other thread that needs the machine standing still (the frontend
 * saves states from a task thread) calls emu_baton_hold() as well and gets
 * it between two frames.
 *
 * There is no lock here. Every change of hands is one atomic operation and
 * a thread with nothing to do sleeps on the eventcount until something
 * changes: nobody polls and nothing times out.
 *
 * tools/threads/run.sh runs this under ThreadSanitizer. */
#include <boolean.h>
#include <retro_common_api.h>
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>

RETRO_BEGIN_DECLS

enum
{
	EMU_BATON_NONE = 0,	/* nobody has the machine */
	EMU_BATON_HELD,		/* a caller of emu_baton_hold() has it */
	EMU_BATON_FRAME,	/* the emulation thread is running a frame */
	EMU_BATON_EXIT		/* the emulation thread is to shut down and return */
};

typedef struct emu_baton
{
	retro_atomic_int_t state;
	retro_atomic_ptr_t work;	/* handed over, not taken yet */
	retro_atomic_int_t acked;	/* the holder has let the emulation thread go on */
	retro_atomic_int_t busy;	/* the holder is still on the last work */
	retro_eventcount_t ec;
} emu_baton_t;

/* Nobody has the machine. False: it cannot be used (and it is safe to
 * free). */
bool emu_baton_init(emu_baton_t *b);
/* No thread is in any other call on @b. */
void emu_baton_free(emu_baton_t *b);

/* Any thread: take the machine. Sleeps while another caller has it or a
 * frame is being made. */
void emu_baton_hold(emu_baton_t *b);
/* Holder: give the machine back. */
void emu_baton_release(emu_baton_t *b);
/* Holder: have the emulation thread make one frame. The machine is the
 * holder's again once emu_baton_wait_work() has returned NULL. */
void emu_baton_start_frame(emu_baton_t *b);
/* Holder, after emu_baton_start_frame(): the next thing the emulation
 * thread hands over, or NULL when the frame is finished. */
void *emu_baton_wait_work(emu_baton_t *b);
/* Holder: the emulation thread may go on past its emu_baton_hand_over().
 * Once for each piece of work. */
void emu_baton_ack_work(emu_baton_t *b);
/* Holder: finished with the work emu_baton_wait_work() returned. */
void emu_baton_work_done(emu_baton_t *b);
/* Holder: tell the emulation thread, asleep between frames, to shut down.
 * Join it, then call emu_baton_thread_gone(). */
void emu_baton_tell_thread_to_exit(emu_baton_t *b);
void emu_baton_thread_gone(emu_baton_t *b);

/* Emulation thread: sleep until there is a frame to make. False: shut
 * down and return instead. */
bool emu_baton_wait_for_frame(emu_baton_t *b);
/* Any thread: a frame is being made, and the holder is in
 * emu_baton_wait_work(). */
bool emu_baton_frame_running(emu_baton_t *b);
/* Emulation thread: the frame is made; the machine goes back to the
 * holder that asked for it. */
void emu_baton_end_frame(emu_baton_t *b);
/* Emulation thread: sleep until the holder has finished the work handed
 * over before, so that two never overlap. */
void emu_baton_wait_work_done(emu_baton_t *b);
/* Emulation thread: hand @w to the holder and sleep until it calls
 * emu_baton_ack_work(). */
void emu_baton_hand_over(emu_baton_t *b, void *w);

/* The core's: set up by retro_init(), gone with retro_deinit() */
extern emu_baton_t emu_baton;

RETRO_END_DECLS

#endif
