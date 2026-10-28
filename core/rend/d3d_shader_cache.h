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
/* A file of compiled shaders (DXBC), for the Direct3D 11 renderer. C89.
 *
 * Bytecode does not depend on the driver: a shader once compiled from its
 * source is not compiled again, whatever driver the next run has. Each is
 * kept by a hash of what it was compiled from (the source, the entry
 * point, the profile and the macros), and by what the renderer calls it
 * (a family and a variant), so the renderer knows what was used and can
 * make it again ahead of a frame.
 *
 * The file: a header (magic, version, count, the bytes after the records,
 * a checksum of everything after the header), the records, the blobs. A
 * file that fails any check is not used. */
#ifndef FLYCAST_D3D_SHADER_CACHE_H
#define FLYCAST_D3D_SHADER_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define D3D_SHCACHE_MAX_ENTRIES 8192
#define D3D_SHCACHE_MAX_BYTES   (64u * 1024u * 1024u)

typedef struct d3d_shcache_entry
{
	uint64_t hash;      /* of what it was compiled from */
	uint32_t family;    /* the renderer's */
	uint32_t variant;
	uint32_t offset;    /* of its bytes, from the first blob */
	uint32_t size;
} d3d_shcache_entry_t;

/* FNV-1a, 64 bits, continued from @h (D3D_SHCACHE_HASH_INIT to start) */
#define D3D_SHCACHE_HASH_INIT 0xcbf29ce484222325ull
uint64_t d3d_shcache_hash(uint64_t h, const void *data, size_t len);

/* The records of the file at @data (@len bytes), malloc'd (free() them),
 * with their offsets checked against the blobs, which start at
 * @data + *blobs_at. False when the file is not one. */
bool d3d_shcache_parse(const uint8_t *data, size_t len,
		d3d_shcache_entry_t **entries, size_t *count, size_t *blobs_at);

/* The bytes a file of @count entries, whose blobs are @blob[i] of
 * entries[i].size, takes. */
size_t d3d_shcache_file_size(const d3d_shcache_entry_t *entries, size_t count);
/* Writes that file into @out (of d3d_shcache_file_size() bytes): offsets
 * are assigned in order. False on too many or too large. */
bool d3d_shcache_write(uint8_t *out, size_t out_size,
		d3d_shcache_entry_t *entries, size_t count, const void *const *blobs);

RETRO_END_DECLS

#endif
