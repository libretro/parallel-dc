/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
/* See band_pool.h. C89. */
#include <stdlib.h>

#include "band_pool.h"

unsigned band_pool_threads_for(unsigned cores)
{
	unsigned total = cores > 1 ? cores - 1 : 1;
	if (total > 3)
		total = 3;
	return total - 1;
}

#if defined(HAVE_THREADS)
#include <retro_atomic.h>
#if defined(RETRO_ATOMIC_HAS_CAS)
#define BAND_POOL_THREADS 1
#endif
#endif

#ifdef BAND_POOL_THREADS
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>

/* The ticket: the job's number (15 bits), its bands and the next band to
 * be taken, a byte each */
#define TICKET(job, bands, next) ((int)(((unsigned)(job) << 16) | ((unsigned)(bands) << 8) | (unsigned)(next)))
#define TICKET_JOB(t) (((unsigned)(t) >> 16) & 0x7fffu)
#define TICKET_BANDS(t) (((unsigned)(t) >> 8) & 0xffu)
#define TICKET_NEXT(t) ((unsigned)(t) & 0xffu)

struct band_pool
{
	sthread_t          *threads[BAND_POOL_MOST];
	unsigned            count;
	retro_eventcount_t  work;      /* the threads park here */
	retro_eventcount_t  done;      /* and band_pool_run(), here */
	retro_atomic_int_t  ticket;
	retro_atomic_int_t  finished;  /* bands of this job done */
	retro_atomic_int_t  stop;
	retro_atomic_int_t  busy;      /* a job running (one at a time) */
	/* The job: written before its ticket is, read by a thread only once
	 * it has claimed one of its bands, and left as it is until every band
	 * is done */
	band_pool_fn_t      fn;
	void               *ctx;
	int                 start;
	int                 end;
	unsigned            job;       /* the caller's only */
};

/* Takes and does bands of job @job until there are none left */
static void band_pool_work(band_pool_t *p, unsigned job)
{
	for (;;)
	{
		const int t = retro_atomic_load_acquire_int(&p->ticket);
		unsigned bands, n;
		int rows, first, last;
		if (TICKET_JOB(t) != job)
			return;
		bands = TICKET_BANDS(t);
		n = TICKET_NEXT(t);
		if (n >= bands)
			return;
		if (!retro_atomic_cas_int(&p->ticket, t, t + 1))
			continue;
		rows  = p->end - p->start;
		first = p->start + (int)((long)rows * (long)n / (long)bands);
		last  = p->start + (int)((long)rows * (long)(n + 1) / (long)bands);
		if (last > first)
			p->fn(p->ctx, first, last);
		if (retro_atomic_fetch_add_int(&p->finished, 1) + 1 == (int)bands)
			retro_eventcount_notify(&p->done);
	}
}

static void band_pool_thread(void *data)
{
	band_pool_t *p = (band_pool_t*)data;
	unsigned seen  = 0;

	for (;;)
	{
		int t;
		for (;;)
		{
			int key;
			t = retro_atomic_load_acquire_int(&p->ticket);
			if (retro_atomic_load_acquire_int(&p->stop) || TICKET_JOB(t) != seen)
				break;
			key = retro_eventcount_prepare_wait(&p->work);
			t = retro_atomic_load_acquire_int(&p->ticket);
			if (retro_atomic_load_acquire_int(&p->stop) || TICKET_JOB(t) != seen)
			{
				retro_eventcount_cancel_wait(&p->work);
				break;
			}
			retro_eventcount_commit_wait(&p->work, key);
		}
		if (retro_atomic_load_acquire_int(&p->stop))
			return;
		seen = TICKET_JOB(t);
		band_pool_work(p, seen);
	}
}

band_pool_t *band_pool_new(unsigned threads)
{
	band_pool_t *p;
	unsigned i;

	if (threads == 0)
		return NULL;
	if (threads > BAND_POOL_MOST - 1)
		threads = BAND_POOL_MOST - 1;
	p = (band_pool_t*)calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	retro_atomic_int_init(&p->ticket, 0);
	retro_atomic_int_init(&p->finished, 0);
	retro_atomic_int_init(&p->stop, 0);
	retro_atomic_int_init(&p->busy, 0);
	if (!retro_eventcount_init(&p->work))
	{
		free(p);
		return NULL;
	}
	if (!retro_eventcount_init(&p->done))
	{
		retro_eventcount_free(&p->work);
		free(p);
		return NULL;
	}
	for (i = 0; i < threads; i++)
	{
		p->threads[i] = sthread_create(band_pool_thread, p);
		if (!p->threads[i])
			break;
		p->count++;
	}
	if (p->count == 0)
	{
		band_pool_free(p);
		return NULL;
	}
	return p;
}

void band_pool_free(band_pool_t *p)
{
	unsigned i;
	if (!p)
		return;
	retro_atomic_store_release_int(&p->stop, 1);
	retro_eventcount_notify(&p->work);
	for (i = 0; i < p->count; i++)
		sthread_join(p->threads[i]);
	retro_eventcount_free(&p->work);
	retro_eventcount_free(&p->done);
	free(p);
}

void band_pool_run(band_pool_t *p, band_pool_fn_t fn, void *ctx, int start, int end)
{
	unsigned bands;

	if (!p || end - start < 2 || !retro_atomic_cas_int(&p->busy, 0, 1))
	{
		fn(ctx, start, end);
		return;
	}
	bands = p->count + 1;
	if ((int)bands > end - start)
		bands = (unsigned)(end - start);
	p->fn    = fn;
	p->ctx   = ctx;
	p->start = start;
	p->end   = end;
	/* (0 is the number the threads start out having seen) */
	p->job   = (p->job + 1) & 0x7fffu;
	if (p->job == 0)
		p->job = 1;
	retro_atomic_store_release_int(&p->finished, 0);
	retro_atomic_store_release_int(&p->ticket, TICKET(p->job, bands, 0));
	retro_eventcount_notify(&p->work);
	band_pool_work(p, p->job);
	for (;;)
	{
		int key;
		if (retro_atomic_load_acquire_int(&p->finished) == (int)bands)
			break;
		key = retro_eventcount_prepare_wait(&p->done);
		if (retro_atomic_load_acquire_int(&p->finished) == (int)bands)
		{
			retro_eventcount_cancel_wait(&p->done);
			break;
		}
		retro_eventcount_commit_wait(&p->done, key);
	}
	retro_atomic_store_release_int(&p->busy, 0);
}

#else

band_pool_t *band_pool_new(unsigned threads)
{
	(void)threads;
	return NULL;
}

void band_pool_free(band_pool_t *p)
{
	(void)p;
}

void band_pool_run(band_pool_t *p, band_pool_fn_t fn, void *ctx, int start, int end)
{
	(void)p;
	fn(ctx, start, end);
}

#endif
