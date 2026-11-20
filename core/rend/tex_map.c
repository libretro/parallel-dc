/* The texture cache's map (tex_map.h) */
#include <stdlib.h>
#include <string.h>
#include "tex_map.h"
#include "log/Log.h"

static u32 tex_map_home(const tex_map_t *m, u64 key)
{
	return (u32)((key * 0x9E3779B97F4A7C15ull) >> 32) & (m->index_size - 1);
}

static void *tex_map_entry(const tex_map_t *m, u32 e)
{
	return m->chunks[e / TEX_MAP_CHUNK] + (size_t)(e % TEX_MAP_CHUNK) * m->elem;
}

static void tex_map_make(const tex_map_t *m, void *e)
{
	if (m->make != NULL)
		m->make(e);
	else
		memset(e, 0, m->elem);
}

void tex_map_init(tex_map_t *m, size_t elem, void (*make)(void *), void (*unmake)(void *))
{
	memset(m, 0, sizeof(*m));
	m->elem = elem;
	m->make = make;
	m->unmake = unmake;
	m->name = "Texture cache";
}

static void tex_map_recycle(tex_map_t *m, u32 e)
{
	void *t = tex_map_entry(m, e);
	if (m->unmake != NULL)
		m->unmake(t);
	tex_map_make(m, t);
	m->entry_live[e] = 0;
	m->entry_next[e] = m->free_entry;
	m->free_entry = e + 1;
	m->count--;
}

static bool tex_map_add_chunk(tex_map_t *m)
{
	const u32 entries = (m->chunk_count + 1) * TEX_MAP_CHUNK;
	u64 *keys = (u64 *)realloc(m->entry_key, entries * sizeof(u64));
	u32 *next, e;
	u8 *live, *chunk;
	if (keys == NULL)
		return false;
	m->entry_key = keys;
	next = (u32 *)realloc(m->entry_next, entries * sizeof(u32));
	if (next == NULL)
		return false;
	m->entry_next = next;
	live = (u8 *)realloc(m->entry_live, entries);
	if (live == NULL)
		return false;
	m->entry_live = live;
	chunk = (u8 *)malloc(m->elem * TEX_MAP_CHUNK);
	if (chunk == NULL)
		return false;
	for (e = 0; e < TEX_MAP_CHUNK; e++)
		tex_map_make(m, chunk + (size_t)e * m->elem);
	m->chunks[m->chunk_count] = chunk;
	/* (onto the free list, the first of them first) */
	for (e = entries; e-- > m->chunk_count * TEX_MAP_CHUNK; )
	{
		m->entry_live[e] = 0;
		m->entry_next[e] = m->free_entry;
		m->free_entry = e + 1;
	}
	m->chunk_count++;
	return true;
}

/* An index for @entries, at most half full: made larger (and filled
 * again) if it is not that large */
static bool tex_map_index_for(tex_map_t *m, u32 entries)
{
	u32 size = m->index_size != 0 ? m->index_size : 64, i, e;
	u64 *keys;
	u32 *where;
	while (size < entries * 2)
		size *= 2;
	if (size == m->index_size)
		return true;
	keys = (u64 *)malloc(size * sizeof(u64));
	where = (u32 *)calloc(size, sizeof(u32));
	if (keys == NULL || where == NULL)
	{
		free(keys);
		free(where);
		return false;
	}
	free(m->slot_key);
	free(m->slot_entry);
	m->slot_key = keys;
	m->slot_entry = where;
	m->index_size = size;
	for (e = 0; e < m->chunk_count * TEX_MAP_CHUNK; e++)
	{
		if (!m->entry_live[e])
			continue;
		for (i = tex_map_home(m, m->entry_key[e]); m->slot_entry[i] != 0; i = (i + 1) & (m->index_size - 1))
			;
		m->slot_key[i] = m->entry_key[e];
		m->slot_entry[i] = e + 1;
	}
	return true;
}

void tex_map_reserve(tex_map_t *m, u32 count)
{
	while (m->chunk_count * TEX_MAP_CHUNK < count && m->chunk_count < TEX_MAP_MAX_CHUNKS)
		if (!tex_map_add_chunk(m))
			break;
	tex_map_index_for(m, m->chunk_count * TEX_MAP_CHUNK);
}

void *tex_map_find(const tex_map_t *m, u64 key)
{
	u32 i;
	if (m->index_size == 0)
		return NULL;
	for (i = tex_map_home(m, key); m->slot_entry[i] != 0; i = (i + 1) & (m->index_size - 1))
		if (m->slot_key[i] == key)
			return tex_map_entry(m, m->slot_entry[i] - 1);
	return NULL;
}

void *tex_map_add(tex_map_t *m, u64 key)
{
	u32 e, i;
	if (m->free_entry == 0)
	{
		if (!m->said)
		{
			m->said = true;
			WARN_LOG(RENDERER, "%s: no room for more than %u, more made (too small for this game)\n",
					m->name, m->chunk_count * TEX_MAP_CHUNK);
		}
		if (m->chunk_count == TEX_MAP_MAX_CHUNKS || !tex_map_add_chunk(m))
			return NULL;
	}
	if (!tex_map_index_for(m, m->count + 1))
		return NULL;
	e = m->free_entry - 1;
	m->free_entry = m->entry_next[e];
	m->entry_key[e] = key;
	m->entry_live[e] = 1;
	m->count++;
	for (i = tex_map_home(m, key); m->slot_entry[i] != 0; i = (i + 1) & (m->index_size - 1))
		;
	m->slot_key[i] = key;
	m->slot_entry[i] = e + 1;
	return tex_map_entry(m, e);
}

void tex_map_remove(tex_map_t *m, u64 key)
{
	u32 i, j, k, e;
	if (m->index_size == 0)
		return;
	for (i = tex_map_home(m, key); m->slot_entry[i] != 0 && m->slot_key[i] != key; i = (i + 1) & (m->index_size - 1))
		;
	if (m->slot_entry[i] == 0)
		return;
	e = m->slot_entry[i] - 1;
	m->slot_entry[i] = 0;
	/* (the run after the gap moved up into it, where it can be) */
	for (j = i, k = (i + 1) & (m->index_size - 1); m->slot_entry[k] != 0; k = (k + 1) & (m->index_size - 1))
	{
		const u32 home = tex_map_home(m, m->slot_key[k]);
		if (((k - home) & (m->index_size - 1)) >= ((k - j) & (m->index_size - 1)))
		{
			m->slot_key[j] = m->slot_key[k];
			m->slot_entry[j] = m->slot_entry[k];
			m->slot_entry[k] = 0;
			j = k;
		}
	}
	tex_map_recycle(m, e);
}

void tex_map_clear(tex_map_t *m)
{
	u32 e;
	for (e = 0; e < m->chunk_count * TEX_MAP_CHUNK; e++)
		if (m->entry_live[e])
			tex_map_recycle(m, e);
	if (m->index_size != 0)
		memset(m->slot_entry, 0, m->index_size * sizeof(u32));
}

void tex_map_free(tex_map_t *m)
{
	u32 c, e;
	for (c = 0; c < m->chunk_count; c++)
	{
		if (m->unmake != NULL)
			for (e = 0; e < TEX_MAP_CHUNK; e++)
				m->unmake(m->chunks[c] + (size_t)e * m->elem);
		free(m->chunks[c]);
		m->chunks[c] = NULL;
	}
	free(m->entry_key);
	free(m->entry_next);
	free(m->entry_live);
	free(m->slot_key);
	free(m->slot_entry);
	m->chunk_count = 0;
	m->entry_key = NULL;
	m->entry_next = NULL;
	m->entry_live = NULL;
	m->slot_key = NULL;
	m->slot_entry = NULL;
	m->index_size = 0;
	m->count = 0;
	m->free_entry = 0;
}
