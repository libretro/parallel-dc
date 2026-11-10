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
/* A picture's rows worked on by a few threads at once (textures scaled up
 * with xBRZ). C89.
 *
 * The threads are started with the pool (band_pool_new()), not for each
 * job: with nothing to do they park on an eventcount, which the next job
 * wakes them from. A job is split into bands of rows; the caller works on
 * them too, and the threads take the rest, each claimed with a
 * compare-and-swap on one word that also names the job, so a thread late
 * from the last job never takes a band of the next. band_pool_run()
 * returns once every band is done. Nothing is allocated, and no lock is
 * taken, for a job.
 *
 * Without threads (or a compare-and-swap) band_pool_new() gives NULL, and
 * band_pool_run() on NULL does the whole job on the caller's thread, as
 * it does when the pool is in use on another thread. */
#ifndef FLYCAST_BAND_POOL_H
#define FLYCAST_BAND_POOL_H

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* the most bands a job is split into, the caller's thread among them */
#define BAND_POOL_MOST 16

typedef struct band_pool band_pool_t;

/* Works on rows @first to @last (not included) */
typedef void (*band_pool_fn_t)(void *ctx, int first, int last);

/* A pool of @threads threads besides the caller's; NULL for none (0, no
 * threads here, or they could not be started) */
band_pool_t *band_pool_new(unsigned threads);
void band_pool_free(band_pool_t *pool);

/* Rows @start to @end (not included), in as many bands as there are
 * threads, the caller's among them */
void band_pool_run(band_pool_t *pool, band_pool_fn_t fn, void *ctx, int start, int end);

/* How many threads to start besides the caller's for @cores processors:
 * a processor left for the rest, three bands at most */
unsigned band_pool_threads_for(unsigned cores);

RETRO_END_DECLS

#endif
