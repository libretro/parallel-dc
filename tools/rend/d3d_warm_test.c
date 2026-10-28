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
/* The Direct3D renderers' thread ahead (core/rend/d3d_warm.c) and their
 * file of compiled shaders (core/rend/d3d_shader_cache.c), with no
 * Direct3D: see d3d_warm_test.sh. C89.
 *
 * The thread: every job handed over comes back exactly once - taken, or
 * released when there is no room for it, or released when it is all
 * freed - whether the thread is running, has left or is started again;
 * one that cannot be built does not come back; a stopped one builds
 * nothing more; and once it is not busy, nothing of the renderer's is
 * being touched. Each result is a block of its own, so a result lost or
 * given back twice is a leak or a double free to the address sanitizer.
 *
 * The file: what is written is read back the same, and a file that fails
 * any one of the checks is not read - each broken on its own, the checksum
 * made right again where a later check is the one meant to catch it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d_warm.h"
#include "d3d_shader_cache.h"
#if defined(HAVE_THREADS)
#include <retro_atomic.h>
#endif

static int failures;

#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- the thread ---- */

#define MAX_KEY 100000

typedef struct
{
	unsigned fail_every;   /* keys divisible by it are not built (0: none) */
	unsigned built;        /* the worker's; read after a join */
	unsigned released;
	unsigned char seen[MAX_KEY];
#if defined(HAVE_THREADS) && defined(RETRO_ATOMIC_HAS_CAS)
	retro_atomic_int_t building;  /* in build(): never while not busy */
#endif
} test_ctx;

static bool test_build(void *ctx, const d3d_warm_job_t *job, d3d_warm_result_t *res)
{
	test_ctx *t = (test_ctx*)ctx;
	unsigned *obj;
	bool ok = false;
#if defined(HAVE_THREADS) && defined(RETRO_ATOMIC_HAS_CAS)
	retro_atomic_store_release_int(&t->building, 1);
#endif
	if (!(t->fail_every && job->key % t->fail_every == 0) && (obj = (unsigned*)malloc(sizeof(*obj))) != NULL)
	{
		*obj     = (unsigned)job->key;
		res->obj = obj;
		res->aux = job->aux + 1;
		t->built++;
		ok = true;
	}
#if defined(HAVE_THREADS) && defined(RETRO_ATOMIC_HAS_CAS)
	retro_atomic_store_release_int(&t->building, 0);
#endif
	return ok;
}

static void test_release(void *ctx, d3d_warm_result_t *res)
{
	test_ctx *t = (test_ctx*)ctx;
	free(res->obj);
	res->obj = NULL;
	t->released++;
}

static test_ctx *ctx_new(void)
{
	return (test_ctx*)calloc(1, sizeof(test_ctx));
}

/* Takes every result there is: each key seen once, its result its own */
static unsigned take_all(d3d_warm_t *w, test_ctx *t)
{
	d3d_warm_result_t r;
	unsigned n = 0;
	while (d3d_warm_take(w, &r))
	{
		unsigned key = (unsigned)r.key;
		CHECK(r.obj != NULL && *(unsigned*)r.obj == key);
		CHECK(r.aux == r.kind + 1);
		CHECK(key < MAX_KEY && !t->seen[key]);
		if (key < MAX_KEY)
			t->seen[key] = 1;
		free(r.obj);
		n++;
	}
	return n;
}

static size_t submit_range(d3d_warm_t *w, unsigned first, unsigned n)
{
	d3d_warm_job_t jobs[256];
	unsigned i;
	for (i = 0; i < n; i++)
	{
		jobs[i].key  = first + i;
		jobs[i].kind = (first + i) & 7;
		jobs[i].aux  = (first + i) & 7;
	}
	return d3d_warm_submit(w, jobs, n);
}

#if defined(HAVE_THREADS) && defined(RETRO_ATOMIC_HAS_CAS)

static void test_basic(void)
{
	test_ctx *t = ctx_new();
	d3d_warm_t *w = d3d_warm_new(test_build, test_release, t, 64);
	unsigned i;
	CHECK(w != NULL);
	CHECK(submit_range(w, 1, 50) == 50);
	d3d_warm_drain_wait(w);
	CHECK(!d3d_warm_busy(w));
	CHECK(take_all(w, t) == 50);
	for (i = 1; i <= 50; i++)
		CHECK(t->seen[i]);
	/* the thread left: the next jobs start another */
	CHECK(submit_range(w, 51, 10) == 10);
	d3d_warm_drain_wait(w);
	CHECK(take_all(w, t) == 10);
	CHECK(t->built == 60 && t->released == 0);
	d3d_warm_free(w);
	CHECK(t->released == 0);
	free(t);
}

/* More than there is room for: the rest are the caller's */
static void test_room(void)
{
	test_ctx *t = ctx_new();
	d3d_warm_t *w = d3d_warm_new(test_build, test_release, t, 64);
	CHECK(submit_range(w, 1, 100) == 64);
	d3d_warm_drain_wait(w);
	CHECK(take_all(w, t) == 64);
	CHECK(!t->seen[65]);
	d3d_warm_free(w);
	free(t);
}

static void test_failed(void)
{
	test_ctx *t = ctx_new();
	d3d_warm_t *w = d3d_warm_new(test_build, test_release, t, 64);
	t->fail_every = 7;
	CHECK(submit_range(w, 1, 49) == 49);
	d3d_warm_drain_wait(w);
	CHECK(!d3d_warm_busy(w));
	CHECK(take_all(w, t) == 49 - 7);
	CHECK(!t->seen[7] && !t->seen[49] && t->seen[48]);
	d3d_warm_free(w);
	free(t);
}

/* No room for results not taken: released by the thread */
static void test_results_full(void)
{
	test_ctx *t = ctx_new();
	d3d_warm_t *w = d3d_warm_new(test_build, test_release, t, 16);
	CHECK(submit_range(w, 1, 16) == 16);
	d3d_warm_drain_wait(w);
	CHECK(submit_range(w, 17, 16) == 16);
	d3d_warm_drain_wait(w);
	CHECK(t->built == 32);
	CHECK(t->released == 16);
	CHECK(take_all(w, t) == 16);
	CHECK(t->seen[1] && t->seen[16] && !t->seen[17]);
	d3d_warm_free(w);
	CHECK(t->released == 16);
	free(t);
}

/* Freed with results not taken: released then */
static void test_free_untaken(void)
{
	test_ctx *t = ctx_new();
	d3d_warm_t *w = d3d_warm_new(test_build, test_release, t, 64);
	CHECK(submit_range(w, 1, 20) == 20);
	d3d_warm_drain_wait(w);
	d3d_warm_free(w);
	CHECK(t->built == 20 && t->released == 20);
	free(t);
}

/* Stopped: nothing more built or taken over, and what was is still there */
static void test_stop(void)
{
	test_ctx *t = ctx_new();
	d3d_warm_t *w = d3d_warm_new(test_build, test_release, t, 1024);
	unsigned taken;
	CHECK(submit_range(w, 1, 256) == 256);
	CHECK(submit_range(w, 257, 256) == 256);
	d3d_warm_stop(w);
	CHECK(!d3d_warm_busy(w));
	CHECK(submit_range(w, 600, 4) == 0);
	taken = take_all(w, t);
	CHECK(taken == t->built);
	CHECK(!t->seen[600]);
	d3d_warm_free(w);
	CHECK(t->released == 0);
	free(t);
}

/* Handed over a few at a time while the thread runs, leaves and is started
 * again: none lost (the thread leaving as more are handed over), and never
 * building while it is not busy. (Results it had no room for - the thread
 * through a full queue of jobs while this one is not taking - are
 * released: counted with those taken.) */
static void test_race(void)
{
	test_ctx *t = ctx_new();
	d3d_warm_t *w = d3d_warm_new(test_build, test_release, t, 4096);
	unsigned key = 1, given = 0, taken = 0, round;
	for (round = 0; round < 20000; round++)
	{
		unsigned n = 1 + (round * 7) % 5;
		/* (the rest, when the queue is full, the caller's) */
		size_t put = submit_range(w, key, n);
		key   += n;
		given += (unsigned)put;
		taken += take_all(w, t);
		if (!d3d_warm_busy(w))
		{
			/* done with all of them: nothing being built */
			CHECK(retro_atomic_load_acquire_int(&t->building) == 0);
			taken += take_all(w, t);
			CHECK(taken + t->released == given);
		}
	}
	d3d_warm_drain_wait(w);
	CHECK(!d3d_warm_busy(w));
	taken += take_all(w, t);
	CHECK(taken + t->released == given);
	d3d_warm_free(w);
	free(t);
}

static void test_args(void)
{
	test_ctx *t = ctx_new();
	d3d_warm_result_t r;
	d3d_warm_job_t job;
	memset(&job, 0, sizeof(job));
	CHECK(d3d_warm_new(NULL, test_release, t, 4) == NULL);
	CHECK(d3d_warm_new(test_build, NULL, t, 4) == NULL);
	CHECK(d3d_warm_new(test_build, test_release, t, 0) == NULL);
	CHECK(d3d_warm_submit(NULL, &job, 1) == 0);
	CHECK(!d3d_warm_take(NULL, &r));
	CHECK(!d3d_warm_busy(NULL));
	d3d_warm_stop(NULL);
	d3d_warm_drain_wait(NULL);
	d3d_warm_free(NULL);
	free(t);
}

static void test_warm(void)
{
	test_basic();
	test_room();
	test_failed();
	test_results_full();
	test_free_untaken();
	test_stop();
	test_race();
	test_args();
}

#else

/* No thread: the renderer builds everything itself */
static void test_warm(void)
{
	test_ctx *t = ctx_new();
	d3d_warm_result_t r;
	CHECK(d3d_warm_new(test_build, test_release, t, 64) == NULL);
	CHECK(submit_range(NULL, 1, 4) == 0);
	CHECK(take_all(NULL, t) == 0);
	CHECK(!d3d_warm_take(NULL, &r));
	CHECK(!d3d_warm_busy(NULL));
	d3d_warm_free(NULL);
	CHECK(t->built == 0);
	free(t);
}

#endif

/* ---- the file ---- */

#define HDR 32
#define REC 24

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The checksum made right again for what was changed */
static void resign(uint8_t *f, size_t len)
{
	uint64_t h = d3d_shcache_hash(D3D_SHCACHE_HASH_INIT, f + HDR, len - HDR);
	put32(f + 24, (uint32_t)h);
	put32(f + 28, (uint32_t)(h >> 32));
}

static bool parses(const uint8_t *f, size_t len)
{
	d3d_shcache_entry_t *e = NULL;
	size_t count = 99, at = 99;
	bool ok = d3d_shcache_parse(f, len, &e, &count, &at);
	if (!ok)
		CHECK(e == NULL && count == 0 && at == 0);
	free(e);
	return ok;
}

/* A file of three shaders, in @*len bytes */
static uint8_t *three(size_t *len)
{
	static uint8_t a[8], b[60], c[4];
	const void *blobs[3];
	d3d_shcache_entry_t e[3];
	uint8_t *f;
	unsigned i;
	memcpy(a, "DXBC", 4);
	memcpy(b, "DXBC", 4);
	memcpy(c, "DXBC", 4);
	for (i = 4; i < sizeof(b); i++)
		b[i] = (uint8_t)(i * 13);
	a[5] = 0xaa;
	memset(e, 0, sizeof(e));
	e[0].hash = 0x1122334455667788ull; e[0].family = 1; e[0].variant = 0x8001; e[0].size = sizeof(a);
	e[1].hash = 0x0102030405060708ull; e[1].family = 3; e[1].variant = 0;      e[1].size = sizeof(b);
	e[2].hash = 0xffffffffffffffffull; e[2].family = 8; e[2].variant = 3;      e[2].size = sizeof(c);
	blobs[0] = a; blobs[1] = b; blobs[2] = c;
	*len = d3d_shcache_file_size(e, 3);
	f = (uint8_t*)malloc(*len);
	CHECK(f != NULL && d3d_shcache_write(f, *len, e, 3, blobs));
	CHECK(e[0].offset == 0 && e[1].offset == 8 && e[2].offset == 68);
	return f;
}

static void test_file(void)
{
	size_t len, count, at;
	uint8_t *f = three(&len), *g;
	d3d_shcache_entry_t *e = NULL;

	/* FNV-1a, 64 bits */
	CHECK(d3d_shcache_hash(D3D_SHCACHE_HASH_INIT, "a", 1) == 0xaf63dc4c8601ec8cull);
	CHECK(d3d_shcache_hash(D3D_SHCACHE_HASH_INIT, "", 0) == D3D_SHCACHE_HASH_INIT);

	/* read back as written */
	CHECK(len == HDR + 3 * REC + 72);
	CHECK(d3d_shcache_parse(f, len, &e, &count, &at));
	CHECK(count == 3 && at == HDR + 3 * REC);
	if (e != NULL && count == 3)
	{
		CHECK(e[0].hash == 0x1122334455667788ull && e[0].family == 1 && e[0].variant == 0x8001);
		CHECK(e[1].hash == 0x0102030405060708ull && e[1].family == 3 && e[1].size == 60);
		CHECK(e[2].hash == 0xffffffffffffffffull && e[2].offset == 68 && e[2].size == 4);
		CHECK(f[at + e[0].offset + 5] == 0xaa);
		CHECK(f[at + e[1].offset + 59] == (uint8_t)(59 * 13));
	}
	free(e);

	g = (uint8_t*)malloc(len + 1);
#define BROKEN(what) do { memcpy(g, f, len); what; } while (0)
	/* each check on its own */
	CHECK(!parses(NULL, len));
	CHECK(!parses(f, HDR - 1));
	BROKEN(g[0] = 'X');                                     CHECK(!parses(g, len));   /* magic */
	BROKEN(put32(g + 8, 2));                                CHECK(!parses(g, len));   /* version */
	BROKEN(put32(g + 20, 1));                               CHECK(!parses(g, len));   /* reserved */
	BROKEN(put32(g + 12, 0));                               CHECK(!parses(g, len));   /* no shaders */
	BROKEN(put32(g + 12, D3D_SHCACHE_MAX_ENTRIES + 1));     CHECK(!parses(g, len));   /* too many */
	BROKEN(put32(g + 16, D3D_SHCACHE_MAX_BYTES + 1));       CHECK(!parses(g, len));   /* too large */
	CHECK(!parses(f, len - 1));                                                       /* cut short */
	BROKEN(g[len] = 0);                                     CHECK(!parses(g, len + 1)); /* more after it */
	BROKEN(g[len] = 0; resign(g, len + 1));                 CHECK(!parses(g, len + 1)); /* (even counted) */
	BROKEN(put32(g + 12, 2));                               CHECK(!parses(g, len));   /* a count that does not add up */
	BROKEN(g[len - 1] ^= 1);                                CHECK(!parses(g, len));   /* checksum: a byte of a blob */
	BROKEN(g[HDR + 3] ^= 1);                                CHECK(!parses(g, len));   /* checksum: a byte of a record */
	BROKEN(g[24] ^= 1);                                     CHECK(!parses(g, len));   /* checksum: its own */
	/* (checksum right, a record wrong) */
	BROKEN(put32(g + HDR + 2 * REC + 20, 3); resign(g, len)); CHECK(!parses(g, len)); /* smaller than a magic */
	BROKEN(put32(g + HDR + 2 * REC + 16, 73); resign(g, len)); CHECK(!parses(g, len)); /* past the blobs */
	BROKEN(put32(g + HDR + 2 * REC + 16, 0xfffffffcu); resign(g, len)); CHECK(!parses(g, len)); /* (far past) */
	BROKEN(put32(g + HDR + 2 * REC + 16, 69); resign(g, len)); CHECK(!parses(g, len)); /* runs past them */
	BROKEN(put32(g + HDR + REC + 20, 0xfffffff0u); resign(g, len)); CHECK(!parses(g, len)); /* (and wraps) */
	BROKEN(g[HDR + 3 * REC + 8 + 1] = 'Y'; resign(g, len)); CHECK(!parses(g, len));  /* not bytecode */
	/* and the same made right again is read */
	BROKEN(resign(g, len));                                 CHECK(parses(g, len));
	CHECK(get32(g + 12) == 3);
#undef BROKEN
	free(g);

	/* none, every other check passing */
	{
		uint8_t empty[HDR];
		memcpy(empty, f, HDR);
		put32(empty + 12, 0);
		put32(empty + 16, 0);
		resign(empty, HDR);
		CHECK(!parses(empty, HDR));
	}
	/* more than allowed, every other check passing */
	{
		size_t n = D3D_SHCACHE_MAX_ENTRIES + 1, size = HDR + n * (REC + 4), i;
		uint8_t *m = (uint8_t*)calloc(1, size);
		CHECK(m != NULL);
		if (m != NULL)
		{
			memcpy(m, f, HDR);
			put32(m + 12, (uint32_t)n);
			put32(m + 16, (uint32_t)(n * 4));
			for (i = 0; i < n; i++)
			{
				put32(m + HDR + i * REC + 16, (uint32_t)(i * 4));
				put32(m + HDR + i * REC + 20, 4);
				memcpy(m + HDR + n * REC + i * 4, "DXBC", 4);
			}
			resign(m, size);
			CHECK(!parses(m, size));
			free(m);
		}
	}
	/* one larger than allowed, every other check passing */
	{
		size_t big = HDR + REC + D3D_SHCACHE_MAX_BYTES + 4;
		uint8_t *b = (uint8_t*)calloc(1, big);
		CHECK(b != NULL);
		if (b != NULL)
		{
			memcpy(b, f, HDR);
			put32(b + 12, 1);
			put32(b + 16, D3D_SHCACHE_MAX_BYTES + 4);
			put32(b + HDR + 20, D3D_SHCACHE_MAX_BYTES + 4);
			memcpy(b + HDR + REC, "DXBC", 4);
			resign(b, big);
			CHECK(!parses(b, big));
			/* (and at the limit, read) */
			put32(b + 16, D3D_SHCACHE_MAX_BYTES);
			put32(b + HDR + 20, D3D_SHCACHE_MAX_BYTES);
			resign(b, big - 4);
			CHECK(parses(b, big - 4));
			free(b);
		}
	}

	/* nothing written that would not be read */
	{
		d3d_shcache_entry_t one;
		const void *blob = "DXBC";
		uint8_t out[HDR + REC + 4];
		memset(&one, 0, sizeof(one));
		one.size = 4;
		CHECK(!d3d_shcache_write(out, sizeof(out), &one, 0, &blob));
		CHECK(!d3d_shcache_write(out, sizeof(out) - 1, &one, 1, &blob));
		CHECK(!d3d_shcache_write(out, sizeof(out), &one, D3D_SHCACHE_MAX_ENTRIES + 1, &blob));
		CHECK(d3d_shcache_write(out, sizeof(out), &one, 1, &blob));
		CHECK(parses(out, sizeof(out)));
	}
	free(f);
}

int main(void)
{
	test_warm();
	test_file();
	if (failures)
	{
		printf("d3d_warm_test: %d failed\n", failures);
		return 1;
	}
	printf("d3d_warm_test: PASS\n");
	return 0;
}
