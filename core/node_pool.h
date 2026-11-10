/* Small pieces of memory for the recompilers - their blocks, the lists and
 * maps those are kept in, the assembler's labels - taken from room made
 * once and given back to it, never to the heap: what a block was given
 * goes to the next one compiled, and nothing is allocated while a game
 * runs once the room is large enough (node_pool::Reserve(), the
 * recompiler's start). Room that runs out has more made, in chunks, which
 * is said once.
 *
 * Pieces are in classes by size: 16 bytes apart up to 1 KB, powers of two
 * up to 1 MB, each class a list of the free ones linked through them;
 * larger ones are the heap's. A piece is given back with its size, as an
 * allocator and a sized operator delete have it.
 *
 * One thread at a time: the one that runs the machine, which compiles and
 * discards blocks (never its signal handler, which only puts blocks on a
 * list to be discarded). No lock.
 *
 * Built with AddressSanitizer, what is not handed out is poisoned - the
 * rest of a piece past what was asked for, and a piece given back but for
 * its link - so that a block used after it went, or written past, is
 * still caught. */
#ifndef CORE_NODE_POOL_H
#define CORE_NODE_POOL_H

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <new>

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

namespace node_pool
{

enum
{
	GRAIN = 16,
	SMALL = 1024,
	LARGE_MIN_LOG2 = 11,
	LARGE_MAX_LOG2 = 20,
	CLASSES = SMALL / GRAIN + (LARGE_MAX_LOG2 - LARGE_MIN_LOG2 + 1),
	CHUNK = 8 << 20
};

struct Pool
{
	void *free_list[CLASSES];
	char *cur, *end;
	void *chunks;		/* each chunk's first word the one before: all reachable */
	size_t made;		/* bytes of chunks */
	bool said;
	/* what says that room ran out (the recompiler's log; none, and it
	 * is not said), with the bytes there were */
	void (*ran_out)(size_t made);
};

/* (one for the program: an inline function's static) */
inline Pool& The()
{
	static Pool pool;
	return pool;
}

/* The class of a piece of @n bytes, and its size; -1 for the heap's */
inline int ClassOf(size_t n, size_t *size)
{
	int l;
	if (n == 0)
		n = 1;
	if (n <= SMALL)
	{
		*size = (n + GRAIN - 1) & ~(size_t)(GRAIN - 1);
		return (int)(*size / GRAIN) - 1;
	}
	for (l = LARGE_MIN_LOG2; l <= LARGE_MAX_LOG2; l++)
		if (n <= ((size_t)1 << l))
		{
			*size = (size_t)1 << l;
			return SMALL / GRAIN + (l - LARGE_MIN_LOG2);
		}
	return -1;
}

/* A chunk of at least @bytes more room */
inline bool Grow(size_t bytes)
{
	Pool& p = The();
	const size_t size = bytes + GRAIN * 2 > (size_t)CHUNK ? bytes + GRAIN * 2 : (size_t)CHUNK;
	char *chunk = (char *)malloc(size);
	uintptr_t at;
	if (chunk == NULL)
		return false;
	*(void **)chunk = p.chunks;
	p.chunks = chunk;
	at = ((uintptr_t)chunk + sizeof(void *) + GRAIN - 1) & ~(uintptr_t)(GRAIN - 1);
	p.cur = (char *)at;
	p.end = chunk + size;
	NODE_POOL_POISON(p.cur, (size_t)(p.end - p.cur));
	p.made += size;
	return true;
}

/* Room for @bytes made now, if there is not that much yet */
inline void Reserve(size_t bytes)
{
	Pool& p = The();
	if (p.made < bytes)
		Grow(bytes - p.made);
}


inline void *Alloc(size_t n)
{
	Pool& p = The();
	size_t size;
	const int c = ClassOf(n, &size);
	void *piece;
	if (c < 0)
		return malloc(n);
	piece = p.free_list[c];
	if (piece != NULL)
	{
		p.free_list[c] = *(void **)piece;
		NODE_POOL_POISON(piece, size);
		NODE_POOL_UNPOISON(piece, n);
		return piece;
	}
	if (p.cur == NULL || (size_t)(p.end - p.cur) < size)
	{
		if (p.made != 0 && !p.said)
		{
			p.said = true;
			if (p.ran_out != NULL)
				p.ran_out(p.made);
		}
		if (!Grow(size))
			return NULL;
	}
	piece = p.cur;
	p.cur += size;
	NODE_POOL_UNPOISON(piece, n);
	return piece;
}

inline void Free(void *piece, size_t n)
{
	Pool& p = The();
	size_t size;
	const int c = ClassOf(n, &size);
	if (piece == NULL)
		return;
	if (c < 0)
	{
		free(piece);
		return;
	}
	NODE_POOL_UNPOISON(piece, sizeof(void *));
	*(void **)piece = p.free_list[c];
	p.free_list[c] = piece;
	NODE_POOL_POISON((char *)piece + sizeof(void *), size - sizeof(void *));
}

/* For the standard containers */
template<class T>
struct Allocator
{
	typedef T value_type;
	typedef T *pointer;
	typedef const T *const_pointer;
	typedef T &reference;
	typedef const T &const_reference;
	typedef size_t size_type;
	typedef ptrdiff_t difference_type;
	template<class U> struct rebind { typedef Allocator<U> other; };

	Allocator() {}
	template<class U> Allocator(const Allocator<U>&) {}

	T *allocate(size_t n, const void * = 0)
	{
		T *p = (T *)Alloc(n * sizeof(T));
		if (p == NULL)
			throw std::bad_alloc();
		return p;
	}
	void deallocate(T *p, size_t n) { Free(p, n * sizeof(T)); }
	size_t max_size() const { return (size_t)-1 / sizeof(T); }
	template<class U> bool operator==(const Allocator<U>&) const { return true; }
	template<class U> bool operator!=(const Allocator<U>&) const { return false; }
};

}

#endif
