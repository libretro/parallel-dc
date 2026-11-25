/* The texture cache's textures, by key, in room made when the renderer
 * starts (tex_map_reserve()): nothing is allocated to find a texture, to
 * add one or to take one away while a game runs.
 *
 * The textures are in chunks of TEX_MAP_CHUNK, made at the start and never
 * moved - a texture's address is held by the renderer, the frames on the
 * GPU and the custom texture loader - and an index finds one by its key:
 * open addressing, linear probing, a gap closed by moving the run after it
 * up (no tombstones), at most half full. A texture taken away is unmade and
 * made anew in its place, free for the next. A cache that has no room left
 * has another chunk made, and the index made larger if it needs to be -
 * which means the room was too small for the game: it is said, once.
 *
 * An element is @elem bytes, made by @make (NULL: all zeros) and unmade by
 * @unmake (NULL: nothing). Render thread only. */
#ifndef REND_TEX_MAP_H
#define REND_TEX_MAP_H

#include <stddef.h>
#include <retro_inline.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEX_MAP_CHUNK 256
#define TEX_MAP_MAX_CHUNKS 1024

typedef struct tex_map
{
	u8 *chunks[TEX_MAP_MAX_CHUNKS];
	u32 chunk_count;
	size_t elem;
	void (*make)(void *e);
	void (*unmake)(void *e);
	/* by entry: its key, the next free one (+1, 0: none), whether it is
	 * one there is */
	u64 *entry_key;
	u32 *entry_next;
	u8 *entry_live;
	u32 free_entry;		/* +1, 0: none */
	u32 count;
	/* the index: a key, and its entry (+1, 0: an empty slot) */
	u64 *slot_key;
	u32 *slot_entry;
	u32 index_size;		/* a power of two */
	bool said;
	const char *name;	/* what it holds, for what it says when it runs out */
} tex_map_t;

/* Empty, for elements of @elem bytes */
void tex_map_init(tex_map_t *m, size_t elem, void (*make)(void *), void (*unmake)(void *));
/* Every element unmade and the memory given back */
void tex_map_free(tex_map_t *m);
/* Room for @count, made now */
void tex_map_reserve(tex_map_t *m, u32 count);
void *tex_map_find(const tex_map_t *m, u64 key);
/* A new element for @key, which has none: NULL if there is no room and
 * none could be made */
void *tex_map_add(tex_map_t *m, u64 key);
/* @key's element taken away: unmade, and made anew for the next */
void tex_map_remove(tex_map_t *m, u64 key);
/* Every element taken away, the room kept */
void tex_map_clear(tex_map_t *m);

/* The places there are, and what is at one: NULL if nothing (for going
 * through them all; nothing to be added or taken meanwhile) */
static INLINE u32 tex_map_places(const tex_map_t *m)
{
	return m->chunk_count * TEX_MAP_CHUNK;
}
static INLINE void *tex_map_at(const tex_map_t *m, u32 e, u64 *key)
{
	if (!m->entry_live[e])
		return NULL;
	*key = m->entry_key[e];
	return m->chunks[e / TEX_MAP_CHUNK] + (size_t)(e % TEX_MAP_CHUNK) * m->elem;
}

#ifdef __cplusplus
}
#endif

#endif
