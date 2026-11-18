/* The recompilers' pool of small pieces of memory (node_pool.h). */
#include <stdlib.h>

#include "node_pool.h"

#if defined(__SANITIZE_ADDRESS__)
#define NODE_POOL_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NODE_POOL_ASAN 1
#endif
#endif
#ifdef NODE_POOL_ASAN
#include <sanitizer/asan_interface.h>
#define NODE_POOL_POISON(p, n) ASAN_POISON_MEMORY_REGION(p, n)
#define NODE_POOL_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION(p, n)
#else
#define NODE_POOL_POISON(p, n) ((void)(p), (void)(n))
#define NODE_POOL_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

static struct
{
	void *free_list[NODE_POOL_CLASSES];
	char *cur, *end;
	void *chunks;		/* each chunk's first word the one before: all reachable */
	size_t made;		/* bytes of chunks */
	int said;
	void (*ran_out)(size_t made);
} pool;

/* The class of a piece of @n bytes, and its size; -1 for the heap's */
static int class_of(size_t n, size_t *size)
{
	int l;
	if (n == 0)
		n = 1;
	if (n <= NODE_POOL_SMALL)
	{
		*size = (n + NODE_POOL_GRAIN - 1) & ~(size_t)(NODE_POOL_GRAIN - 1);
		return (int)(*size / NODE_POOL_GRAIN) - 1;
	}
	for (l = NODE_POOL_LARGE_MIN_LOG2; l <= NODE_POOL_LARGE_MAX_LOG2; l++)
		if (n <= ((size_t)1 << l))
		{
			*size = (size_t)1 << l;
			return NODE_POOL_SMALL / NODE_POOL_GRAIN + (l - NODE_POOL_LARGE_MIN_LOG2);
		}
	return -1;
}

/* A chunk of at least @bytes more room */
static int grow(size_t bytes)
{
	const size_t size = bytes + NODE_POOL_GRAIN * 2 > (size_t)NODE_POOL_CHUNK
			? bytes + NODE_POOL_GRAIN * 2 : (size_t)NODE_POOL_CHUNK;
	char *chunk = (char *)malloc(size);
	size_t at;
	if (chunk == NULL)
		return 0;
	*(void **)chunk = pool.chunks;
	pool.chunks = chunk;
	at = ((size_t)chunk + sizeof(void *) + NODE_POOL_GRAIN - 1) & ~(size_t)(NODE_POOL_GRAIN - 1);
	pool.cur = (char *)at;
	pool.end = chunk + size;
	NODE_POOL_POISON(pool.cur, (size_t)(pool.end - pool.cur));
	pool.made += size;
	return 1;
}

void node_pool_reserve(size_t bytes)
{
	if (pool.made < bytes)
		grow(bytes - pool.made);
}

size_t node_pool_made(void)
{
	return pool.made;
}

void node_pool_set_ran_out(void (*ran_out)(size_t made))
{
	pool.ran_out = ran_out;
}

void *node_pool_alloc(size_t n)
{
	size_t size;
	const int c = class_of(n, &size);
	void *piece;
	if (c < 0)
		return malloc(n);
	piece = pool.free_list[c];
	if (piece != NULL)
	{
		pool.free_list[c] = *(void **)piece;
		NODE_POOL_POISON(piece, size);
		NODE_POOL_UNPOISON(piece, n);
		return piece;
	}
	if (pool.cur == NULL || (size_t)(pool.end - pool.cur) < size)
	{
		if (pool.made != 0 && !pool.said)
		{
			pool.said = 1;
			if (pool.ran_out != NULL)
				pool.ran_out(pool.made);
		}
		if (!grow(size))
			return NULL;
	}
	piece = pool.cur;
	pool.cur += size;
	NODE_POOL_UNPOISON(piece, n);
	return piece;
}

void node_pool_free(void *piece, size_t n)
{
	size_t size;
	const int c = class_of(n, &size);
	if (piece == NULL)
		return;
	if (c < 0)
	{
		free(piece);
		return;
	}
	NODE_POOL_UNPOISON(piece, sizeof(void *));
	*(void **)piece = pool.free_list[c];
	pool.free_list[c] = piece;
	NODE_POOL_POISON((char *)piece + sizeof(void *), size - sizeof(void *));
}
