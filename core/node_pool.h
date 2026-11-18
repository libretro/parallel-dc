/* Small pieces of memory for the recompilers - their blocks, the lists and
 * maps those are kept in, the assembler's labels - taken from room made
 * once and given back to it, never to the heap: what a block was given
 * goes to the next one compiled, and nothing is allocated while a game
 * runs once the room is large enough (node_pool_reserve(), the
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


#ifdef __cplusplus
extern "C" {
#endif

enum
{
	NODE_POOL_GRAIN = 16,
	NODE_POOL_SMALL = 1024,
	NODE_POOL_LARGE_MIN_LOG2 = 11,
	NODE_POOL_LARGE_MAX_LOG2 = 20,
	NODE_POOL_CLASSES = NODE_POOL_SMALL / NODE_POOL_GRAIN + (NODE_POOL_LARGE_MAX_LOG2 - NODE_POOL_LARGE_MIN_LOG2 + 1),
	NODE_POOL_CHUNK = 8 << 20
};

/* A piece of @n bytes (NULL: none to be had) */
void *node_pool_alloc(size_t n);
/* The piece back, with the size it was asked for */
void node_pool_free(void *piece, size_t n);
/* Room for @bytes made now, if there is not that much yet */
void node_pool_reserve(size_t bytes);
/* The bytes of room made */
size_t node_pool_made(void);
/* What says that room ran out (the recompiler's log; none, and it is not
 * said), with the bytes there were */
void node_pool_set_ran_out(void (*ran_out)(size_t made));

#ifdef __cplusplus
}

#include <new>

/* For the standard containers that are left */
namespace node_pool
{
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
		T *p = (T *)node_pool_alloc(n * sizeof(T));
		if (p == NULL)
			throw std::bad_alloc();
		return p;
	}
	void deallocate(T *p, size_t n) { node_pool_free(p, n * sizeof(T)); }
	size_t max_size() const { return (size_t)-1 / sizeof(T); }
	template<class U> bool operator==(const Allocator<U>&) const { return true; }
	template<class U> bool operator!=(const Allocator<U>&) const { return false; }
};
}
#endif

#endif
