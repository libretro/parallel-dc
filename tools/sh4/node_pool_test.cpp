/* The recompilers' pool (node_pool.h): pieces of every size taken and
 * given back at random, each filled with its own byte and checked before
 * it goes - none overlaps another, each is aligned to 16; a piece given
 * back is the next of its size taken; room that runs out has more made,
 * said once; a piece too large for a class is the heap's; and a standard
 * container on its allocator; with ASan, what is not handed out poisoned.
 * tools/sh4/node_pool_test.sh builds it, with
 * ASan and UBSan. */
#include <stdio.h>
#include <string.h>
#include <map>
#include <vector>

#include "node_pool.h"

static int said;
static void ran_out(size_t) { said++; }

#define N 4000
static unsigned char *piece[N];
static size_t piece_n[N];
static int failures;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s (line %d)\n", what, __LINE__); failures++; } } while (0)

static unsigned rnd_state = 12345;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }

static size_t size_of(void)
{
	switch (rnd() % 4)
	{
	case 0: return 1 + rnd() % 64;
	case 1: return 1 + rnd() % 1024;
	case 2: return 1 + rnd() % 70000;
	default: return 1500000 + rnd() % 100000;   /* (the heap's) */
	}
}

static void check_piece(int i)
{
	size_t k;
	for (k = 0; k < piece_n[i]; k++)
		if (piece[i][k] != (unsigned char)i)
		{
			CHECK(0, "a piece kept what was written in it");
			return;
		}
}

int main()
{
	int round, i;
	node_pool::The().ran_out = ran_out;
	node_pool::Reserve(1 << 20);
	CHECK(node_pool::The().made >= (1u << 20), "the room reserved");
	for (round = 0; round < 60000; round++)
	{
		i = (int)(rnd() % N);
		if (piece[i] != NULL)
		{
			check_piece(i);
			node_pool::Free(piece[i], piece_n[i]);
			piece[i] = NULL;
		}
		else
		{
			piece_n[i] = size_of();
			piece[i] = (unsigned char *)node_pool::Alloc(piece_n[i]);
			CHECK(piece[i] != NULL, "a piece");
			CHECK(((size_t)piece[i] & 15) == 0, "aligned to 16");
			memset(piece[i], (unsigned char)i, piece_n[i]);
		}
	}
	for (i = 0; i < N; i++)
		if (piece[i] != NULL)
		{
			check_piece(i);
			node_pool::Free(piece[i], piece_n[i]);
			piece[i] = NULL;
		}
	CHECK(said == 1, "room run out: more made, said once");
	{
		void *a = node_pool::Alloc(100), *b;
#ifdef NODE_POOL_ASAN
		CHECK(__asan_address_is_poisoned((char *)a + 100), "past what was asked for: poisoned");
#endif
		node_pool::Free(a, 100);
#ifdef NODE_POOL_ASAN
		CHECK(__asan_address_is_poisoned((char *)a + 40), "a piece given back: poisoned");
#endif
		b = node_pool::Alloc(112);
		CHECK(a == b, "a piece given back the next of its size");
		node_pool::Free(b, 112);
	}
	{
		std::map<int, std::vector<int, node_pool::Allocator<int> >, std::less<int>,
				node_pool::Allocator<std::pair<const int, std::vector<int, node_pool::Allocator<int> > > > > m;
		for (i = 0; i < 5000; i++)
			m[i % 777].push_back(i);
		CHECK(m.size() == 777 && m[5].size() == 7, "a map of vectors on the allocator");
	}
	if (failures)
	{
		printf("node_pool_test: %d failed\n", failures);
		return 1;
	}
	printf("node_pool_test: every piece its own, given back and taken again\n");
	return 0;
}
