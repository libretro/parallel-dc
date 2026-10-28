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
/* See d3d_shader_cache.h. C89. All fields little-endian, as the platforms
 * Direct3D runs on are. */
#include <stdlib.h>
#include <string.h>

#include "d3d_shader_cache.h"

static const char d3d_shcache_magic[8] = { 'F', 'C', 'D', '1', '1', 'S', 'H', 'C' };
#define D3D_SHCACHE_VERSION 1
/* magic, version, count, blob bytes, checksum (8 bytes) */
#define D3D_SHCACHE_HEADER  (8 + 4 + 4 + 4 + 4 + 8)
/* hash (8), family, variant, offset, size */
#define D3D_SHCACHE_RECORD  (8 + 4 + 4 + 4 + 4)

uint64_t d3d_shcache_hash(uint64_t h, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t*)data;
	size_t i;
	for (i = 0; i < len; i++)
	{
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v)
{
	wr32(p, (uint32_t)v);
	wr32(p + 4, (uint32_t)(v >> 32));
}

bool d3d_shcache_parse(const uint8_t *data, size_t len,
		d3d_shcache_entry_t **entries, size_t *count, size_t *blobs_at)
{
	uint32_t n, blob_bytes, reserved, i;
	size_t   records_end;
	d3d_shcache_entry_t *e;

	*entries  = NULL;
	*count    = 0;
	*blobs_at = 0;
	if (!data || len < D3D_SHCACHE_HEADER || memcmp(data, d3d_shcache_magic, 8)
			|| rd32(data + 8) != D3D_SHCACHE_VERSION)
		return false;
	n          = rd32(data + 12);
	blob_bytes = rd32(data + 16);
	reserved   = rd32(data + 20);
	if (reserved != 0 || n == 0 || n > D3D_SHCACHE_MAX_ENTRIES || blob_bytes > D3D_SHCACHE_MAX_BYTES)
		return false;
	records_end = D3D_SHCACHE_HEADER + (size_t)n * D3D_SHCACHE_RECORD;
	/* exactly the header, the records and the blobs */
	if (len != records_end + blob_bytes)
		return false;
	if (rd64(data + 24) != d3d_shcache_hash(D3D_SHCACHE_HASH_INIT, data + D3D_SHCACHE_HEADER, len - D3D_SHCACHE_HEADER))
		return false;
	if (!(e = (d3d_shcache_entry_t*)malloc((size_t)n * sizeof(*e))))
		return false;
	for (i = 0; i < n; i++)
	{
		const uint8_t *r = data + D3D_SHCACHE_HEADER + (size_t)i * D3D_SHCACHE_RECORD;
		e[i].hash    = rd64(r);
		e[i].family  = rd32(r + 8);
		e[i].variant = rd32(r + 12);
		e[i].offset  = rd32(r + 16);
		e[i].size    = rd32(r + 20);
		/* each blob inside the blobs, and bytecode at that */
		if (       e[i].size < 4 || e[i].offset > blob_bytes || e[i].size > blob_bytes - e[i].offset
				|| memcmp(data + records_end + e[i].offset, "DXBC", 4))
		{
			free(e);
			return false;
		}
	}
	*entries  = e;
	*count    = n;
	*blobs_at = records_end;
	return true;
}

size_t d3d_shcache_file_size(const d3d_shcache_entry_t *entries, size_t count)
{
	size_t i, bytes = D3D_SHCACHE_HEADER + count * D3D_SHCACHE_RECORD;
	for (i = 0; i < count; i++)
		bytes += entries[i].size;
	return bytes;
}

bool d3d_shcache_write(uint8_t *out, size_t out_size,
		d3d_shcache_entry_t *entries, size_t count, const void *const *blobs)
{
	size_t   i, records_end, at = 0;
	uint8_t *blob_out;

	if (!count || count > D3D_SHCACHE_MAX_ENTRIES || out_size != d3d_shcache_file_size(entries, count))
		return false;
	records_end = D3D_SHCACHE_HEADER + count * D3D_SHCACHE_RECORD;
	if (out_size - records_end > D3D_SHCACHE_MAX_BYTES)
		return false;
	blob_out = out + records_end;
	for (i = 0; i < count; i++)
	{
		uint8_t *r = out + D3D_SHCACHE_HEADER + i * D3D_SHCACHE_RECORD;
		entries[i].offset = (uint32_t)at;
		wr64(r, entries[i].hash);
		wr32(r + 8, entries[i].family);
		wr32(r + 12, entries[i].variant);
		wr32(r + 16, entries[i].offset);
		wr32(r + 20, entries[i].size);
		memcpy(blob_out + at, blobs[i], entries[i].size);
		at += entries[i].size;
	}
	memcpy(out, d3d_shcache_magic, 8);
	wr32(out + 8, D3D_SHCACHE_VERSION);
	wr32(out + 12, (uint32_t)count);
	wr32(out + 16, (uint32_t)at);
	wr32(out + 20, 0);
	wr64(out + 24, d3d_shcache_hash(D3D_SHCACHE_HASH_INIT, out + D3D_SHCACHE_HEADER, out_size - D3D_SHCACHE_HEADER));
	return true;
}
