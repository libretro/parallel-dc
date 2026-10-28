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
/* Shaders and pipelines built ahead, on a thread of their own, for the
 * Direct3D renderers. C89.
 *
 * The renderer hands over jobs (a kind and a key of its own); a thread
 * builds each with the renderer's build function and hands the result
 * back. The thread exits once there is no job left, and the next jobs
 * handed over start another: it never waits for work. Both hand-overs are
 * lock-free single-producer single-consumer queues: the renderer's thread
 * gives jobs and takes results, the worker the other way round.
 *
 * The renderer takes results when it misses something it would otherwise
 * build: a result is never required, and one that could not be handed back
 * (the queue full) is released by the worker, and built by the renderer
 * when it is wanted, as without the worker.
 *
 * Without threads (or a compare-and-swap), d3d_warm_new() fails and the
 * renderer builds everything itself, as it did. */
#ifndef FLYCAST_D3D_WARM_H
#define FLYCAST_D3D_WARM_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct d3d_warm_job
{
	uint64_t key;
	uint32_t kind;
	uint32_t aux;
} d3d_warm_job_t;

typedef struct d3d_warm_result
{
	void    *obj;     /* the built object, or NULL */
	void    *extra;   /* anything else the build made */
	uint64_t key;
	uint32_t kind;
	uint32_t aux;
} d3d_warm_result_t;

/* Builds @job into @res (worker thread). Returns false when it could not. */
typedef bool (*d3d_warm_build_t)(void *ctx, const d3d_warm_job_t *job, d3d_warm_result_t *res);
/* Releases a result that is not handed back (worker thread, or teardown). */
typedef void (*d3d_warm_release_t)(void *ctx, d3d_warm_result_t *res);

typedef struct d3d_warm d3d_warm_t;

/* Room for @max_jobs queued jobs and as many results. NULL: no thread here. */
d3d_warm_t *d3d_warm_new(d3d_warm_build_t build, d3d_warm_release_t release, void *ctx, size_t max_jobs);
/* Stops the thread (the job it is building is finished), releases what
 * was not taken, frees. */
void d3d_warm_free(d3d_warm_t *w);
/* Hands over @n jobs, starting a thread if none is running. Returns how
 * many were taken: the rest are the caller's to build. */
size_t d3d_warm_submit(d3d_warm_t *w, const d3d_warm_job_t *jobs, size_t n);
/* One result, if there is one. */
bool d3d_warm_take(d3d_warm_t *w, d3d_warm_result_t *res);
/* Whether any job handed over is not done yet (renderer's thread). Once
 * false, the worker touches nothing of the renderer's until the next
 * submit. */
bool d3d_warm_busy(d3d_warm_t *w);
/* Waits for the thread to finish what it was given (tests). */
void d3d_warm_drain_wait(d3d_warm_t *w);
/* Stops the thread after the job it is building, and joins it: what it
 * finished is still there to take, and nothing more is built. */
void d3d_warm_stop(d3d_warm_t *w);

RETRO_END_DECLS

#endif
