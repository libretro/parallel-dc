#ifndef INPUT_LATCH_H
#define INPUT_LATCH_H
/* The controller and mouse state the emulation thread reads.
 *
 * In threaded rendering the libretro thread samples input once a frame
 * and the emulation thread reads it when the game polls its controllers.
 * The libretro thread fills a snapshot of all four ports and publishes it
 * whole; the emulation thread reads the newest one. They go through a
 * triple buffer (tribuf), so neither thread waits for the other, nothing takes a
 * lock, and a read never sees half of one frame's state and half of the
 * next.
 *
 * Mouse motion is published as running totals, not per-frame amounts: the
 * reader subtracts what it has already been given, so motion from a frame
 * the game did not poll in is carried into the next poll instead of lost,
 * and polling twice in a frame does not count it twice.
 *
 * tools/threads/run.sh runs this under ThreadSanitizer. */
#include "types.h"
#include "lockfree.h"
#include <retro_inline.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct input_latch_port
{
	u32 kcode;		/* active low */
	s8  joyx, joyy, joyrx, joyry;
	u8  rt, lt;
	u32 mo_buttons;		/* active low */
	u32 mo_x, mo_y, mo_wheel;	/* running totals, wrap around */
} input_latch_port;

typedef struct input_latch_state
{
	input_latch_port port[4];
} input_latch_state;

typedef struct InputLatch
{
	input_latch_state buf[3];
	tribuf            tri;
	u32               seen_x[4], seen_y[4], seen_wheel[4];	/* emulation thread */
} InputLatch;

/* Nothing pressed, nothing moved, nothing published */
static INLINE void input_latch_init(InputLatch *l)
{
	int i, p;
	for (i = 0; i < 3; i++)
		for (p = 0; p < 4; p++)
		{
			input_latch_port *s = &l->buf[i].port[p];
			s->kcode = 0xFFFFFFFF;
			s->joyx = s->joyy = s->joyrx = s->joyry = 0;
			s->rt = s->lt = 0;
			s->mo_buttons = 0xFFFFFFFF;
			s->mo_x = s->mo_y = s->mo_wheel = 0;
		}
	for (p = 0; p < 4; p++)
		l->seen_x[p] = l->seen_y[p] = l->seen_wheel[p] = 0;
	tribuf_reset(&l->tri);
}

/* Libretro thread: fill the returned state, then input_latch_publish(). */
static INLINE input_latch_state *input_latch_next(InputLatch *l)
{
	return &l->buf[tribuf_back(&l->tri)];
}

static INLINE void input_latch_publish(InputLatch *l)
{
	tribuf_publish(&l->tri);
}

/* Emulation thread: the newest published state of @port. */
static INLINE const input_latch_port *input_latch_read(InputLatch *l, u32 port)
{
	tribuf_take(&l->tri);
	return &l->buf[tribuf_front(&l->tri)].port[port];
}

/* Emulation thread: mouse buttons, and the motion since the last call for
 * @port. */
static INLINE void input_latch_read_mouse(InputLatch *l, u32 port, u32 *buttons, f32 *dx, f32 *dy, f32 *dwheel)
{
	const input_latch_port *s = input_latch_read(l, port);

	*buttons = s->mo_buttons;
	*dx      = (f32)(s32)(s->mo_x - l->seen_x[port]);
	*dy      = (f32)(s32)(s->mo_y - l->seen_y[port]);
	*dwheel  = (f32)(s32)(s->mo_wheel - l->seen_wheel[port]);
	l->seen_x[port]     = s->mo_x;
	l->seen_y[port]     = s->mo_y;
	l->seen_wheel[port] = s->mo_wheel;
}

/* Libretro thread, while it holds the machine (the emulation thread is
 * asleep outside it): how much of @port's motion has been read, and
 * setting it - a loaded state says how much is still to be read. */
static INLINE void input_latch_get_seen(const InputLatch *l, u32 port, u32 *x, u32 *y, u32 *wheel)
{
	*x     = l->seen_x[port];
	*y     = l->seen_y[port];
	*wheel = l->seen_wheel[port];
}

static INLINE void input_latch_set_seen(InputLatch *l, u32 port, u32 x, u32 y, u32 wheel)
{
	l->seen_x[port]     = x;
	l->seen_y[port]     = y;
	l->seen_wheel[port] = wheel;
}

/* The core's (libretro.cpp) */
extern InputLatch input_latch;

RETRO_END_DECLS

#endif
