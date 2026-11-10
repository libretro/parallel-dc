/* The pool a texture scaled up is shared out to (core/rend/band_pool.h):
 * every row of every job done once, by the caller's thread and the pool's,
 * with jobs of every size one after another; two callers at once (the
 * second does its job on its own thread); the pool let go with its
 * threads parked; and without a pool. C89.
 *
 *   sh tools/rend/band_pool_test.sh */
#include <stdio.h>
#include <string.h>

#include "band_pool.h"

#if defined(HAVE_THREADS)
#include <rthreads/rthreads.h>
#endif

#define ROWS 600
#define JOBS 3000

typedef struct job
{
	unsigned char done[ROWS];
	int bad;
} job_t;

static void rows(void *ctx, int first, int last)
{
	job_t *j = (job_t*)ctx;
	int r;
	if (first < 0 || last > ROWS || first >= last)
	{
		j->bad = 1;
		return;
	}
	for (r = first; r < last; r++)
		j->done[r]++;
}

/* @n jobs on @pool, each of rows @start to @end, every row done once */
static int jobs(band_pool_t *pool, int n, unsigned seed)
{
	static job_t j[2];
	job_t *jj = &j[seed & 1];
	int i;
	for (i = 0; i < n; i++)
	{
		int start, end, r;
		seed = seed * 1103515245u + 12345u;
		start = (int)((seed >> 8) % 40);
		end = start + 1 + (int)((seed >> 16) % (ROWS - 40));
		memset(jj, 0, sizeof(*jj));
		band_pool_run(pool, rows, jj, start, end);
		if (jj->bad)
		{
			printf("band_pool_test: a band outside rows %d to %d\n", start, end);
			return 0;
		}
		for (r = 0; r < ROWS; r++)
			if (jj->done[r] != (r >= start && r < end ? 1 : 0))
			{
				printf("band_pool_test: row %d of %d to %d done %d times\n", r, start, end, jj->done[r]);
				return 0;
			}
	}
	return 1;
}

#if defined(HAVE_THREADS)
typedef struct other
{
	band_pool_t *pool;
	int ok;
} other_t;

static void other_caller(void *data)
{
	other_t *o = (other_t*)data;
	o->ok = jobs(o->pool, JOBS / 4, 7u);
}
#endif

int main(void)
{
	unsigned threads;

	if (band_pool_threads_for(1) != 0 || band_pool_threads_for(2) != 0
			|| band_pool_threads_for(4) != 2 || band_pool_threads_for(64) != 2)
	{
		printf("band_pool_test: the threads for a count of processors are not as they should be\n");
		return 1;
	}
	if (band_pool_new(0) != NULL)
	{
		printf("band_pool_test: a pool of no threads\n");
		return 1;
	}
	/* without a pool: the caller's thread does it */
	if (!jobs(NULL, 50, 1u))
		return 1;

	for (threads = 1; threads <= 4; threads++)
	{
		band_pool_t *pool = band_pool_new(threads);
#if defined(HAVE_THREADS)
		other_t o;
		sthread_t *t;
		if (pool == NULL)
		{
			printf("band_pool_test: no pool of %u threads\n", threads);
			return 1;
		}
#endif
		if (!jobs(pool, JOBS, 2u + threads * 2u))
			return 1;
#if defined(HAVE_THREADS)
		/* two callers at once */
		o.pool = pool;
		o.ok = 0;
		t = sthread_create(other_caller, &o);
		if (!t)
			return 1;
		if (!jobs(pool, JOBS / 4, 4u))
			return 1;
		sthread_join(t);
		if (!o.ok)
			return 1;
#endif
		band_pool_free(pool);
	}
	printf("band_pool_test: every row of every job done once\n");
	return 0;
}
