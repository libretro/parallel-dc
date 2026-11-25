#pragma once
/* Lock-free pieces the core's threads hand work across with. None of them
 * takes a lock or makes a system call; a thread that has to sleep until
 * the other side moves does it on a retro_eventcount. tools/threads/run.sh
 * exercises them under ThreadSanitizer. */
#include <stddef.h>
#include <boolean.h>
#include <retro_inline.h>
#include <retro_atomic.h>

/* Which of three buffers each side of a one-writer, one-reader pair owns.
 * The writer fills tribuf_back() and calls tribuf_publish(); the reader
 * calls tribuf_take() and reads tribuf_front(). Each side always holds one
 * buffer to itself and the third waits in between, so neither ever waits
 * for the other, and a publish the reader has not taken yet is simply
 * replaced by the next one. */
#define TRIBUF_FRESH 4
typedef struct tribuf
{
	retro_atomic_int_t mid;	/* the buffer neither side holds, | TRIBUF_FRESH */
	int back;
	int front;
} tribuf;

/* Only while neither side is running. */
static INLINE void tribuf_reset(tribuf *t)
{
	t->back  = 0;
	t->front = 1;
	retro_atomic_int_init(&t->mid, 2);
}

/* Writer only. */
static INLINE int tribuf_back(const tribuf *t)
{
	return t->back;
}

static INLINE void tribuf_publish(tribuf *t)
{
	t->back = retro_atomic_exchange_int(&t->mid, t->back | TRIBUF_FRESH) & 3;
}

/* Reader only. True when tribuf_front() now names a buffer published since
 * the last take. */
static INLINE int tribuf_front(const tribuf *t)
{
	return t->front;
}

static INLINE bool tribuf_take(tribuf *t)
{
	if (!(retro_atomic_load_acquire_int(&t->mid) & TRIBUF_FRESH))
		return false;
	t->front = retro_atomic_exchange_int(&t->mid, t->front) & 3;
	return true;
}

