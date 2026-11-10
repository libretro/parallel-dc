/* The texture cache's textures, by key, in room made when the renderer
 * starts (Reserve()): nothing is allocated to find a texture, to add one or
 * to take one away while a game runs.
 *
 * The textures are in chunks of CHUNK, made at the start and never moved -
 * a texture's address is held by the renderer, the frames on the GPU and
 * the custom texture loader - and an index finds one by its key: open
 * addressing, linear probing, a gap closed by moving the run after it up
 * (no tombstones), at most half full. A texture taken away is destroyed and
 * made anew in its place, free for the next. A cache that has no room left
 * has another chunk made, and the index made larger if it needs to be -
 * which means the room was too small for the game: it is said, once.
 *
 * T is default constructible; a texture is value-initialized, as the
 * map's new values were - a member with no initializer starts at zero.
 * Render thread only. */
#ifndef REND_TEX_MAP_H
#define REND_TEX_MAP_H

#include "types.h"
#include "log/Log.h"
#include <new>
#include <stdlib.h>
#include <string.h>

template<typename T>
class TexMap
{
public:
	enum { CHUNK = 256, MAX_CHUNKS = 1024 };

	TexMap()
	{
		u32 i;
		for (i = 0; i < MAX_CHUNKS; i++)
			chunks[i] = NULL;
	}
	~TexMap() { Free(); }
	TexMap(const TexMap&) = delete;
	TexMap& operator=(const TexMap&) = delete;

	/* What it holds, for what it says when it runs out (the texture
	 * cache's textures unless said) */
	void Name(const char *what) { name = what; }
	/* Room for @textures, made now */
	void Reserve(u32 textures)
	{
		while (chunkCount * CHUNK < textures && chunkCount < MAX_CHUNKS)
			if (!AddChunk())
				break;
		IndexFor(chunkCount * CHUNK);
	}

	T *Find(u64 key) const
	{
		u32 i;
		if (indexSize == 0)
			return NULL;
		for (i = Home(key); slotEntry[i] != 0; i = (i + 1) & (indexSize - 1))
			if (slotKey[i] == key)
				return Entry(slotEntry[i] - 1);
		return NULL;
	}

	/* A new texture for @key, which has none: NULL if there is no room and
	 * none could be made */
	T *Add(u64 key)
	{
		u32 e, i;
		if (freeEntry == 0)
		{
			if (!said)
			{
				said = true;
				WARN_LOG(RENDERER, "%s: no room for more than %u, more made (too small for this game)",
						name, chunkCount * CHUNK);
			}
			if (chunkCount == MAX_CHUNKS || !AddChunk())
				return NULL;
		}
		if (!IndexFor(count + 1))
			return NULL;
		e = freeEntry - 1;
		freeEntry = entryNext[e];
		entryKey[e] = key;
		entryLive[e] = 1;
		count++;
		for (i = Home(key); slotEntry[i] != 0; i = (i + 1) & (indexSize - 1))
			;
		slotKey[i] = key;
		slotEntry[i] = e + 1;
		return Entry(e);
	}

	/* @key's texture taken away: destroyed, and made anew for the next */
	void Remove(u64 key)
	{
		u32 i, j, k, e;
		if (indexSize == 0)
			return;
		for (i = Home(key); slotEntry[i] != 0 && slotKey[i] != key; i = (i + 1) & (indexSize - 1))
			;
		if (slotEntry[i] == 0)
			return;
		e = slotEntry[i] - 1;
		slotEntry[i] = 0;
		/* (the run after the gap moved up into it, where it can be) */
		for (j = i, k = (i + 1) & (indexSize - 1); slotEntry[k] != 0; k = (k + 1) & (indexSize - 1))
		{
			const u32 home = Home(slotKey[k]);
			if (((k - home) & (indexSize - 1)) >= ((k - j) & (indexSize - 1)))
			{
				slotKey[j] = slotKey[k];
				slotEntry[j] = slotEntry[k];
				slotEntry[k] = 0;
				j = k;
			}
		}
		Recycle(e);
	}

	u32 Size() const { return count; }

	/* Each texture there is, @fn(key, texture), until it says false. Not
	 * to be added to or taken from meanwhile. */
	template<typename Fn>
	void Each(Fn fn)
	{
		u32 e;
		for (e = 0; e < chunkCount * CHUNK; e++)
			if (entryLive[e] && !fn(entryKey[e], *Entry(e)))
				return;
	}

	/* Every texture taken away (Remove()), the room kept */
	void Clear()
	{
		u32 e;
		for (e = 0; e < chunkCount * CHUNK; e++)
			if (entryLive[e])
				Recycle(e);
		if (indexSize != 0)
			memset(slotEntry, 0, indexSize * sizeof(u32));
	}

private:
	T *Entry(u32 e) const { return &chunks[e / CHUNK][e % CHUNK]; }
	u32 Home(u64 key) const
	{
		return (u32)((key * 0x9E3779B97F4A7C15ull) >> 32) & (indexSize - 1);
	}
	void Recycle(u32 e)
	{
		T *t = Entry(e);
		t->~T();
		new (t) T();
		entryLive[e] = 0;
		entryNext[e] = freeEntry;
		freeEntry = e + 1;
		count--;
	}
	bool AddChunk()
	{
		const u32 entries = (chunkCount + 1) * CHUNK;
		u64 *keys = (u64 *)realloc(entryKey, entries * sizeof(u64));
		u32 *next, e;
		u8 *live;
		T *chunk;
		if (keys == NULL)
			return false;
		entryKey = keys;
		next = (u32 *)realloc(entryNext, entries * sizeof(u32));
		if (next == NULL)
			return false;
		entryNext = next;
		live = (u8 *)realloc(entryLive, entries);
		if (live == NULL)
			return false;
		entryLive = live;
		/* (each one value-initialized, as a map's new value is: what has
		 * no initializer of its own starts at zero) */
		chunk = new (std::nothrow) T[CHUNK]();
		if (chunk == NULL)
			return false;
		chunks[chunkCount] = chunk;
		/* (onto the free list, the first of them first) */
		for (e = entries; e-- > chunkCount * CHUNK; )
		{
			entryLive[e] = 0;
			entryNext[e] = freeEntry;
			freeEntry = e + 1;
		}
		chunkCount++;
		return true;
	}
	/* An index for @entries, at most half full: made larger (and filled
	 * again) if it is not that large */
	bool IndexFor(u32 entries)
	{
		u32 size = indexSize != 0 ? indexSize : 64, i, e;
		u64 *keys;
		u32 *where;
		while (size < entries * 2)
			size *= 2;
		if (size == indexSize)
			return true;
		keys = (u64 *)malloc(size * sizeof(u64));
		where = (u32 *)calloc(size, sizeof(u32));
		if (keys == NULL || where == NULL)
		{
			free(keys);
			free(where);
			return false;
		}
		free(slotKey);
		free(slotEntry);
		slotKey = keys;
		slotEntry = where;
		indexSize = size;
		for (e = 0; e < chunkCount * CHUNK; e++)
		{
			if (!entryLive[e])
				continue;
			for (i = Home(entryKey[e]); slotEntry[i] != 0; i = (i + 1) & (indexSize - 1))
				;
			slotKey[i] = entryKey[e];
			slotEntry[i] = e + 1;
		}
		return true;
	}
	void Free()
	{
		u32 c;
		for (c = 0; c < chunkCount; c++)
			delete[] chunks[c];
		free(entryKey);
		free(entryNext);
		free(entryLive);
		free(slotKey);
		free(slotEntry);
	}

	T *chunks[MAX_CHUNKS];
	u32 chunkCount = 0;
	// by entry: its key, the next free one (+1, 0: none), whether it is a texture there is
	u64 *entryKey = NULL;
	u32 *entryNext = NULL;
	u8 *entryLive = NULL;
	u32 freeEntry = 0;		// +1, 0: none
	u32 count = 0;
	// the index: a key, and its entry (+1, 0: an empty slot)
	u64 *slotKey = NULL;
	u32 *slotEntry = NULL;
	u32 indexSize = 0;		// a power of two
	bool said = false;
	const char *name = "Texture cache";
};

#endif
