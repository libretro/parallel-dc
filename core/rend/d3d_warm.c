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
/* See d3d_warm.h. C89. */
#include <stdlib.h>
#include <string.h>

#include "d3d_warm.h"

#if defined(HAVE_THREADS)
#include <retro_atomic.h>
#if defined(RETRO_ATOMIC_HAS_CAS)
#define D3D_WARM_THREADS 1
#endif
#endif

#ifdef D3D_WARM_THREADS
#include <retro_spsc.h>
#include <rthreads/rthreads.h>

struct d3d_warm
{
	retro_spsc_t        jobs;      /* renderer -> worker */
	retro_spsc_t        results;   /* worker -> renderer */
	d3d_warm_build_t    build;
	d3d_warm_release_t  release;
	void               *ctx;
	sthread_t          *thread;    /* renderer's thread only */
	/* Jobs handed over and not done with: added to by the renderer after
	 * it queues them, taken from by the worker once one is built and its
	 * result handed back. Changed by read-modify-writes only, one total
	 * order: the renderer that finds it 0 starts a worker; the worker that
	 * takes it to 0 has nothing left and leaves; while it is not 0 there is
	 * a worker, which has every job counted in it to read. */
	retro_atomic_int_t  pending;
	retro_atomic_int_t  stop;
};

static void d3d_warm_thread(void *data)
{
	d3d_warm_t       *w = (d3d_warm_t*)data;
	d3d_warm_job_t    job;
	d3d_warm_result_t res;

	for (;;)
	{
		/* (stopped: whoever stopped it joins it and drops the rest) */
		if (retro_atomic_load_acquire_int(&w->stop))
			break;
		/* (always there: counted after it was queued) */
		if (retro_spsc_read(&w->jobs, &job, sizeof(job)) != sizeof(job))
			break;
		memset(&res, 0, sizeof(res));
		res.key  = job.key;
		res.kind = job.kind;
		res.aux  = job.aux;
		if (w->build(w->ctx, &job, &res))
		{
			if (       retro_spsc_write_avail(&w->results) < sizeof(res)
					|| retro_spsc_write(&w->results, &res, sizeof(res)) != sizeof(res))
				w->release(w->ctx, &res);
		}
		/* the last one: nothing of the renderer's is touched after this */
		if (retro_atomic_fetch_sub_int(&w->pending, 1) == 1)
			break;
	}
}

d3d_warm_t *d3d_warm_new(d3d_warm_build_t build, d3d_warm_release_t release, void *ctx, size_t max_jobs)
{
	d3d_warm_t *w;

	if (!build || !release || !max_jobs)
		return NULL;
	if (!(w = (d3d_warm_t*)calloc(1, sizeof(*w))))
		return NULL;
	if (!retro_spsc_init(&w->jobs, max_jobs * sizeof(d3d_warm_job_t)))
	{
		free(w);
		return NULL;
	}
	if (!retro_spsc_init(&w->results, max_jobs * sizeof(d3d_warm_result_t)))
	{
		retro_spsc_free(&w->jobs);
		free(w);
		return NULL;
	}
	w->build   = build;
	w->release = release;
	w->ctx     = ctx;
	retro_atomic_int_init(&w->pending, 0);
	retro_atomic_int_init(&w->stop, 0);
	return w;
}

/* The thread joined, so the renderer is the only one at either queue. */
static void d3d_warm_join(d3d_warm_t *w)
{
	if (w->thread)
	{
		sthread_join(w->thread);
		w->thread = NULL;
	}
}

void d3d_warm_drain_wait(d3d_warm_t *w)
{
	if (w)
		d3d_warm_join(w);
}

size_t d3d_warm_submit(d3d_warm_t *w, const d3d_warm_job_t *jobs, size_t n)
{
	size_t room, put;

	if (!w || !n || retro_atomic_load_acquire_int(&w->stop))
		return 0;
	room = retro_spsc_write_avail(&w->jobs) / sizeof(*jobs);
	put  = n < room ? n : room;
	if (!put)
		return 0;
	retro_spsc_write(&w->jobs, jobs, put * sizeof(*jobs));
	/* None pending: no worker (the last one has left, or is leaving, and
	 * reads nothing more). Start one. */
	if (retro_atomic_fetch_add_int(&w->pending, (int)put) == 0)
	{
		d3d_warm_join(w);
		if (!(w->thread = sthread_create(d3d_warm_thread, w)))
		{
			/* Nothing will take them: they go back to the caller (none
			 * other is queued: none was pending). */
			retro_spsc_clear(&w->jobs);
			retro_atomic_store_release_int(&w->pending, 0);
			return 0;
		}
	}
	return put;
}

void d3d_warm_stop(d3d_warm_t *w)
{
	if (!w)
		return;
	retro_atomic_store_release_int(&w->stop, 1);
	d3d_warm_join(w);
	/* (what was not started is not going to be) */
	retro_spsc_clear(&w->jobs);
	retro_atomic_store_release_int(&w->pending, 0);
}

bool d3d_warm_take(d3d_warm_t *w, d3d_warm_result_t *res)
{
	return w && retro_spsc_read(&w->results, res, sizeof(*res)) == sizeof(*res);
}

bool d3d_warm_busy(d3d_warm_t *w)
{
	return w && retro_atomic_load_acquire_int(&w->pending) != 0;
}

void d3d_warm_free(d3d_warm_t *w)
{
	d3d_warm_result_t res;

	if (!w)
		return;
	d3d_warm_stop(w);
	while (retro_spsc_read(&w->results, &res, sizeof(res)) == sizeof(res))
		w->release(w->ctx, &res);
	retro_spsc_free(&w->jobs);
	retro_spsc_free(&w->results);
	free(w);
}

#else /* no threads: the renderer builds everything itself */

d3d_warm_t *d3d_warm_new(d3d_warm_build_t build, d3d_warm_release_t release, void *ctx, size_t max_jobs)
{
	(void)build; (void)release; (void)ctx; (void)max_jobs;
	return NULL;
}
void d3d_warm_free(d3d_warm_t *w) { (void)w; }
size_t d3d_warm_submit(d3d_warm_t *w, const d3d_warm_job_t *jobs, size_t n)
{
	(void)w; (void)jobs; (void)n;
	return 0;
}
bool d3d_warm_take(d3d_warm_t *w, d3d_warm_result_t *res) { (void)w; (void)res; return false; }
bool d3d_warm_busy(d3d_warm_t *w) { (void)w; return false; }
void d3d_warm_drain_wait(d3d_warm_t *w) { (void)w; }
void d3d_warm_stop(d3d_warm_t *w) { (void)w; }

#endif
