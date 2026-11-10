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
#include <rthreads/retro_eventcount.h>

struct d3d_warm
{
	retro_spsc_t        jobs;      /* renderer -> worker */
	retro_spsc_t        results;   /* worker -> renderer */
	d3d_warm_build_t    build;
	d3d_warm_release_t  release;
	void               *ctx;
	sthread_t          *thread;    /* renderer's thread only */
	retro_eventcount_t  work;      /* the worker parks here */
	retro_eventcount_t  idle;      /* and d3d_warm_drain_wait(), here */
	/* Jobs handed over and not done with: added to by the renderer after
	 * it queues them, taken from by the worker once one is built and its
	 * result handed back. Once 0, the worker touches nothing of the
	 * renderer's until more are added. */
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
		int key;

		/* (stopped: whoever stopped it joins it and drops the rest) */
		if (retro_atomic_load_acquire_int(&w->stop))
			break;
		if (retro_spsc_read(&w->jobs, &job, sizeof(job)) == sizeof(job))
		{
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
				retro_eventcount_notify(&w->idle);
			continue;
		}
		/* none: parked until there are, or it is stopped */
		key = retro_eventcount_prepare_wait(&w->work);
		if (       retro_atomic_load_acquire_int(&w->stop)
				|| retro_spsc_read_avail(&w->jobs) >= sizeof(job))
		{
			retro_eventcount_cancel_wait(&w->work);
			continue;
		}
		retro_eventcount_commit_wait(&w->work, key);
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
		goto no_jobs;
	if (!retro_spsc_init(&w->results, max_jobs * sizeof(d3d_warm_result_t)))
		goto no_results;
	if (!retro_eventcount_init(&w->work))
		goto no_work;
	if (!retro_eventcount_init(&w->idle))
		goto no_idle;
	w->build   = build;
	w->release = release;
	w->ctx     = ctx;
	retro_atomic_int_init(&w->pending, 0);
	retro_atomic_int_init(&w->stop, 0);
	if (!(w->thread = sthread_create(d3d_warm_thread, w)))
		goto no_thread;
	return w;

no_thread:
	retro_eventcount_free(&w->idle);
no_idle:
	retro_eventcount_free(&w->work);
no_work:
	retro_spsc_free(&w->results);
no_results:
	retro_spsc_free(&w->jobs);
no_jobs:
	free(w);
	return NULL;
}

void d3d_warm_drain_wait(d3d_warm_t *w)
{
	if (!w)
		return;
	while (retro_atomic_load_acquire_int(&w->pending) != 0)
	{
		int key = retro_eventcount_prepare_wait(&w->idle);
		if (retro_atomic_load_acquire_int(&w->pending) == 0)
		{
			retro_eventcount_cancel_wait(&w->idle);
			break;
		}
		retro_eventcount_commit_wait(&w->idle, key);
	}
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
	retro_atomic_fetch_add_int(&w->pending, (int)put);
	retro_eventcount_notify(&w->work);
	return put;
}

void d3d_warm_stop(d3d_warm_t *w)
{
	if (!w)
		return;
	retro_atomic_store_release_int(&w->stop, 1);
	retro_eventcount_notify(&w->work);
	if (w->thread)
	{
		sthread_join(w->thread);
		w->thread = NULL;
	}
	/* (what was not started is not going to be) */
	retro_spsc_clear(&w->jobs);
	retro_atomic_store_release_int(&w->pending, 0);
	retro_eventcount_notify(&w->idle);
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
	retro_eventcount_free(&w->work);
	retro_eventcount_free(&w->idle);
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
