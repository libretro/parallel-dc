/* Stress test for the core's lock-free thread handoffs. See run.sh.
 *
 * Every test shares plain, non-atomic data between threads under the
 * protection of the piece it tests, so ThreadSanitizer reports a race the
 * moment that piece lets two threads in at once, and each test checks for
 * lost or duplicated work itself. A handoff that loses a wake-up hangs;
 * the watchdog turns that into a failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <retro_timers.h>

#include "types.h"
#include "lockfree.h"
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include "libretro/emu_baton.h"
#include "libretro/input_latch.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} } while (0)

static unsigned rng_next(unsigned *s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

/* ---- tribuf ---------------------------------------------------------- */

#define TRIPLE_ROUNDS 200000
#define TRIPLE_WORDS  64

static tribuf triple;
static unsigned triple_buf[3][TRIPLE_WORDS];
static retro_atomic_int_t triple_done;

static void triple_writer(void *arg)
{
	unsigned v;
	(void)arg;
	for (v = 1; v <= TRIPLE_ROUNDS; v++)
	{
		unsigned *b = triple_buf[tribuf_back(&triple)];
		int i;
		for (i = 0; i < TRIPLE_WORDS; i++)
			b[i] = v;
		tribuf_publish(&triple);
	}
	retro_atomic_store_release_int(&triple_done, 1);
}

static void test_triple_buffer(void)
{
	sthread_t *t;
	unsigned last = 0;
	long takes = 0;

	memset(triple_buf, 0, sizeof(triple_buf));
	tribuf_reset(&triple);
	retro_atomic_int_init(&triple_done, 0);
	CHECK(!tribuf_take(&triple));
	t = sthread_create(triple_writer, NULL);
	for (;;)
	{
		int done = retro_atomic_load_acquire_int(&triple_done);
		if (tribuf_take(&triple))
		{
			const unsigned *b = triple_buf[tribuf_front(&triple)];
			int i;
			/* A whole buffer from one publish, never older than the last. */
			for (i = 1; i < TRIPLE_WORDS; i++)
				CHECK(b[i] == b[0]);
			CHECK(b[0] > last);
			last = b[0];
			takes++;
		}
		else if (done)
			break;
	}
	sthread_join(t);
	CHECK(last == TRIPLE_ROUNDS);
	CHECK(takes > 0);
}

/* ---- emu_baton ------------------------------------------------------- */

#define BATON_FRAMES 30000

static emu_baton_t baton;
static long machine;		/* plain: whoever has the machine */
static long video_memory;	/* plain: the guest's, read by the renderer before it acks */
static long frames_made, renders_made, renders_seen;
static long drawing;		/* plain: the renderer's, between its ack and work_done */
/* The emulation thread is past its hand_over(): the renderer waits for
 * that before its work_done, so an ack that wakes nobody hangs here */
static retro_atomic_int_t emu_past;
static retro_eventcount_t emu_past_ec;

typedef struct fake_render { long snapshot; } fake_render;

/* Stands in for the emulation thread: each frame is some emulation, up to
 * three renders handed over, then more emulation. */
static void baton_emu_thread(void *arg)
{
	static fake_render render;
	unsigned seed = 7;
	(void)arg;

	while (emu_baton_wait_for_frame(&baton))
	{
		int renders = rng_next(&seed) % 4;
		int i;

		machine++;
		CHECK(emu_baton_frame_running(&baton));
		for (i = 0; i < renders; i++)
		{
			video_memory++;
			emu_baton_wait_work_done(&baton);
			/* the last render is drawn: two never overlap */
			CHECK(drawing == 0);
			render.snapshot = video_memory;
			renders_made++;
			emu_baton_hand_over(&baton, &render);
			retro_atomic_store_release_int(&emu_past, 1);
			retro_eventcount_notify(&emu_past_ec);
			/* The renderer has read video memory; writing it again is
			 * safe while it draws. */
			video_memory++;
			machine++;
		}
		frames_made++;
		emu_baton_end_frame(&baton);
	}
	machine++;	/* shutting down: still the thread's machine */
}

/* Stands in for a background save state. */
static retro_atomic_int_t baton_saver_stop;
static long baton_saves;

static void baton_saver(void *arg)
{
	(void)arg;
	while (!retro_atomic_load_acquire_int(&baton_saver_stop))
	{
		emu_baton_hold(&baton);
		machine++;
		baton_saves++;
		emu_baton_release(&baton);
	}
}

static void test_emu_baton(void)
{
	sthread_t *emu, *saver;
	void *w;
	int frame;

	CHECK(emu_baton_init(&baton));
	CHECK(retro_eventcount_init(&emu_past_ec));
	retro_atomic_int_init(&emu_past, 0);
	/* No frame being made: taking the machine never waits. */
	emu_baton_hold(&baton);
	machine++;
	emu_baton_release(&baton);

	emu = sthread_create(baton_emu_thread, NULL);
	retro_atomic_int_init(&baton_saver_stop, 0);
	saver = sthread_create(baton_saver, NULL);

	/* retro_run, over and over. */
	for (frame = 0; frame < BATON_FRAMES; frame++)
	{
		emu_baton_hold(&baton);
		machine++;
		emu_baton_start_frame(&baton);
		while ((w = emu_baton_wait_work(&baton)) != NULL)
		{
			fake_render *r = (fake_render *)w;
			/* What the emulation thread wrote before handing over is
			 * what is there until the ack. */
			CHECK(r->snapshot == video_memory);
			drawing = 1;
			emu_baton_ack_work(&baton);
			renders_seen++;
			for (;;)
			{
				int key;
				if (retro_atomic_load_acquire_int(&emu_past))
					break;
				key = retro_eventcount_prepare_wait(&emu_past_ec);
				if (retro_atomic_load_acquire_int(&emu_past))
					retro_eventcount_cancel_wait(&emu_past_ec);
				else
					retro_eventcount_commit_wait(&emu_past_ec, key);
			}
			retro_atomic_store_release_int(&emu_past, 0);
			drawing = 0;
			emu_baton_work_done(&baton);
		}
		/* The frame is over: the machine is this thread's again. */
		CHECK(!emu_baton_frame_running(&baton));
		machine++;
		CHECK(frames_made == frame + 1);
		emu_baton_release(&baton);
	}

	retro_atomic_store_release_int(&baton_saver_stop, 1);
	sthread_join(saver);

	/* Unload. */
	emu_baton_hold(&baton);
	emu_baton_tell_thread_to_exit(&baton);
	sthread_join(emu);
	emu_baton_thread_gone(&baton);

	emu_baton_hold(&baton);
	emu_baton_release(&baton);
	/* Every render handed over was drawn, none twice. */
	CHECK(renders_seen == renders_made);
	CHECK(baton_saves > 0);
	emu_baton_free(&baton);
	retro_eventcount_free(&emu_past_ec);
}

/* ---- InputLatch ------------------------------------------------------ */

#define LATCH_FRAMES 200000

static InputLatch latch;
static retro_atomic_int_t latch_done;

/* One frame's sample: every field of every port follows from @frame. */
static void latch_writer(void *arg)
{
	u32 frame;
	(void)arg;
	for (frame = 1; frame <= LATCH_FRAMES; frame++)
	{
		input_latch_state *next = input_latch_next(&latch);
		u32 p;
		for (p = 0; p < 4; p++)
		{
			input_latch_port *s = &next->port[p];
			s->kcode      = frame;
			s->joyx       = (s8)(frame + p);
			s->joyy       = (s8)(frame * 3);
			s->joyrx      = (s8)(frame * 5);
			s->joyry      = (s8)(frame * 7);
			s->rt         = (u8)(frame * 11);
			s->lt         = (u8)(frame * 13);
			s->mo_buttons = ~frame;
			s->mo_x       = frame * 2;	/* moved 2 right, 3 up a frame */
			s->mo_y       = (u32)0 - frame * 3;
			s->mo_wheel   = frame / 16;
		}
		input_latch_publish(&latch);
	}
	retro_atomic_store_release_int(&latch_done, 1);
}

static void test_input_latch(void)
{
	sthread_t *t;
	u32 last = 0, buttons;
	f32 dx, dy, dw;
	double x = 0, y = 0, w = 0;

	input_latch_init(&latch);
	/* Before anything is sampled: nothing held, nothing moved. */
	CHECK(input_latch_read(&latch, 2)->kcode == 0xFFFFFFFF);
	input_latch_read_mouse(&latch, 2, &buttons, &dx, &dy, &dw);
	CHECK(buttons == 0xFFFFFFFF && dx == 0 && dy == 0 && dw == 0);

	retro_atomic_int_init(&latch_done, 0);
	t = sthread_create(latch_writer, NULL);
	for (;;)
	{
		int done = retro_atomic_load_acquire_int(&latch_done);
		const input_latch_port *a = input_latch_read(&latch, 0);
		u32 frame = a->kcode;
		u32 p;

		/* One whole frame, on every port, never an older one. */
		CHECK(frame >= last);
		last = frame;
		if (frame != 0xFFFFFFFF)
			for (p = 0; p < 4; p++)
			{
				const input_latch_port *s = input_latch_read(&latch, p);
				if (s->kcode != frame)
				{
					/* A newer frame arrived between two ports; each
					 * port is still whole. */
					frame = s->kcode;
					last  = frame;
				}
				CHECK(s->joyx == (s8)(frame + p) && s->joyy == (s8)(frame * 3));
				CHECK(s->joyrx == (s8)(frame * 5) && s->joyry == (s8)(frame * 7));
				CHECK(s->rt == (u8)(frame * 11) && s->lt == (u8)(frame * 13));
				CHECK(s->mo_buttons == ~frame);
			}
		else
			last = 0;

		input_latch_read_mouse(&latch, 1, &buttons, &dx, &dy, &dw);
		x += dx;
		y += dy;
		w += dw;
		if (done)
			break;
	}
	sthread_join(t);
	input_latch_read_mouse(&latch, 1, &buttons, &dx, &dy, &dw);
	x += dx;
	y += dy;
	w += dw;
	/* However the polls fell against the frames, all of the motion
	 * arrived and none of it twice. */
	CHECK(x == 2.0 * LATCH_FRAMES);
	CHECK(y == -3.0 * LATCH_FRAMES);
	CHECK(w == LATCH_FRAMES / 16);
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	static const struct { const char *name; void (*fn)(void); } tests[] = {
		{ "tribuf",      test_triple_buffer },
		{ "emu_baton",   test_emu_baton },
		{ "InputLatch",  test_input_latch },
	};
	size_t i;

	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
	{
		int before = failures;
#ifndef _WIN32
		alarm(300);	/* a lost wake-up shows up as a hang */
#endif
		tests[i].fn();
		printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
	}
	return failures ? 1 : 0;
}
