#pragma once
#include "ta_list.h"

/* A frame's list of T: ta_list.h's, typed. Nothing here but what that is,
 * said in elements. */
template <class T>
struct List
{
	ta_list_t l;

	/* (the frame's flag is a bool, written as the byte it is) */
	static_assert(sizeof(bool) == 1, "a bool is a byte");

	T* head() const { return (T*)l.base; }
	/* where the next element goes, and that place set: what is before it
	 * is used, what is after it is left */
	T* cur() const { return (T*)l.cur; }
	void set_cur(T* p) { l.cur = (unsigned char *)p; }
	int used() const { return (int)((T*)l.cur - (T*)l.base); }
	int avail() const { return (int)((T*)l.end - (T*)l.cur); }
	int capacity() const { return (int)((T*)l.end - (T*)l.base); }
	int bytes() const { return (int)(l.cur - l.base); }
	/* the room made @n elements from the first (for a test of the edges) */
	void set_capacity(int n) { l.end = (unsigned char *)((T*)l.base + n); }

	__forceinline T* Append(int n = 1) { return (T*)ta_list_append(&l, (size_t)n * sizeof(T)); }
	__forceinline T* LastPtr(int n = 1) const { return (T*)l.cur - n; }
	T* PopLast()
	{
		l.cur -= sizeof(T);
		return (T*)l.cur;
	}
	/* Full: the frame is marked, and @n taken from the beginning */
	T* Full(int n) { return (T*)ta_list_full(&l, (size_t)n * sizeof(T)); }

	void InitBytes(int maxbytes, bool* ovrn, const char *name)
	{
		ta_list_init(&l, (size_t)maxbytes, sizeof(T), (unsigned char *)ovrn, name);
	}
	void Init(int maxsize, bool* ovrn, const char *name)
	{
		InitBytes(maxsize * (int)sizeof(T), ovrn, name);
	}
	bool Ready() const { return l.base != NULL; }
	void Clear() { ta_list_clear(&l); }
	void Free() { ta_list_free(&l); }

	T* begin() const { return head(); }
	T* end() const { return cur(); }
};
