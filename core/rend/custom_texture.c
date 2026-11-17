/*
	 Copyright 2018 flyinghead

	 This file is part of reicast.

	 reicast is free software: you can redistribute it and/or modify
	 it under the terms of the GNU General Public License as published by
	 the Free Software Foundation, either version 2 of the License, or
	 (at your option) any later version.

	 reicast is distributed in the hope that it will be useful,
	 but WITHOUT ANY WARRANTY; without even the implied warranty of
	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	 GNU General Public License for more details.

	 You should have received a copy of the GNU General Public License
	 along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
/* Custom textures (custom_texture.h) */
#include <stdlib.h>
#include <string.h>
#include "custom_texture.h"
#include "deps/coreio/coreio.h"
#include <retro_dirent.h>
#include <retro_miscellaneous.h>
#include <string/stdstring.h>
#include <compat/strl.h>
#include <file/file_path.h>
#if !defined(TARGET_NO_THREADS)
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#endif
#include "reios/reios.h"
#include "hw/naomi/naomi_cart.h"
#include "paths.h"

/* A picture in the directory: its hash, and its file's name in names */
typedef struct custom_entry
{
	u32 hash;
	u32 order;	/* (the directory's: of two of a hash, the later one is taken) */
	size_t name;
} custom_entry_t;

static retro_atomic_int_t initialized;
/* Set by custom_texture_available() for the directory being there, and by
 * the loader once it has listed it (to whether anything usable was in
 * it); read by the renderer. Cleared when the game goes. */
static retro_atomic_int_t custom_textures_available;
static char textures_path[PATH_MAX_LENGTH];	/* with a slash at the end */
/* The pictures, by hash: made by the loader before it takes any work,
 * looked up by it only */
static custom_entry_t *entries;
static size_t entry_count;
static char *names;
static size_t names_size;
/* Textures waiting for the loader, the last pushed first (a Treiber
 * stack). A texture is on it at most once: it is pushed when its count
 * leaves zero, and the loader only lets the count return to zero once it
 * is done with the texture. */
static retro_atomic_ptr_t work_queue;
#if !defined(TARGET_NO_THREADS)
static sthread_t *loader_thread;
static retro_eventcount_t wakeup_ec;
static retro_atomic_int_t wakeup;
static bool wakeup_ec_made;
#endif

void custom_request_init(custom_request_t *r)
{
	r->image_data = NULL;
	r->next = NULL;
	retro_atomic_store_release_int(&r->in_progress, 0);
	retro_atomic_int_init(&r->hash, 0);
	retro_atomic_int_init(&r->old_hash, 0);
	retro_atomic_int_init(&r->old_vqhash, 0);
}

static void work_push(custom_request_t *r)
{
	for (;;)
	{
		void *old = retro_atomic_load_relaxed_ptr(&work_queue);
		r->next = (custom_request_t *)old;
		if (retro_atomic_cas_ptr(&work_queue, old, r))
			return;
	}
}

/* Everything pushed so far, the first pushed first */
static custom_request_t *work_take_all(void)
{
	custom_request_t *list = (custom_request_t *)retro_atomic_exchange_ptr(&work_queue, NULL);
	custom_request_t *out = NULL;
	while (list != NULL)
	{
		custom_request_t *next = list->next;
		list->next = out;
		out = list;
		list = next;
	}
	return out;
}

/* The game's id: its product number (a NAOMI's game id), the spaces at
 * its end cut, the others made underscores. Empty if there is none. */
static void game_id(char *out, size_t size)
{
	size_t len, i;
	if (settings.System == DC_PLATFORM_DREAMCAST)
	{
		len = sizeof(ip_meta.product_number) < size - 1 ? sizeof(ip_meta.product_number) : size - 1;
		memcpy(out, ip_meta.product_number, len);
		out[len] = '\0';
	}
	else
		strlcpy(out, naomi_game_id, size);
	len = strlen(out);
	while (len > 0 && out[len - 1] == ' ')
		len--;
	out[len] = '\0';
	for (i = 0; i < len; i++)
		if (out[i] == ' ')
			out[i] = '_';
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* @s as strtoll(s, &end, 16) reads it, cut to 32 bits: the characters it
 * took (0 if none) */
static size_t parse_hash(const char *s, u32 *hash)
{
	const u64 most = (u64)1 << 63;	/* (LLONG_MAX + 1) */
	size_t i = 0, start;
	bool neg = false, over = false;
	u64 acc = 0;

	while (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))
		i++;
	if (s[i] == '+' || s[i] == '-')
		neg = s[i++] == '-';
	if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X') && hex_digit(s[i + 2]) >= 0)
		i += 2;
	start = i;
	for (; hex_digit(s[i]) >= 0; i++)
	{
		if (acc > (most - (u64)hex_digit(s[i])) / 16)
			over = true;
		else
			acc = acc * 16 + (u64)hex_digit(s[i]);
	}
	if (i == start)
	{
		*hash = 0;
		return 0;
	}
	/* (beyond the range, the end of it: LLONG_MAX, or LLONG_MIN) */
	if (!neg && (over || acc == most))
		*hash = 0xffffffffu;
	else if (neg && over)
		*hash = 0;
	else
		*hash = neg ? (u32)(0u - (u32)acc) : (u32)acc;
	return i;
}

static int entry_cmp(const void *a, const void *b)
{
	const custom_entry_t *x = (const custom_entry_t *)a, *y = (const custom_entry_t *)b;
	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;
	return x->order < y->order ? -1 : x->order > y->order ? 1 : 0;
}

static void forget_map(void)
{
	free(entries);
	entries = NULL;
	entry_count = 0;
	free(names);
	names = NULL;
	names_size = 0;
}

/* Loader thread, once: the pictures in the directory, by hash */
static void load_map(void)
{
	size_t room = 0, names_room = 0, i, kept;
	RDIR *dir;

	forget_map();
	dir = retro_opendir(textures_path);
	if (dir == NULL)
		return;
	while (retro_readdir(dir))
	{
		const char *name, *extension_ro, *dot;
		char extension[5];
		size_t len, base_len;
		u32 hash;

		if (retro_dirent_is_dir(dir, NULL))
			continue;
		name = retro_dirent_get_name(dir);
		extension_ro = path_get_extension(name);
		strncpy(extension, extension_ro, 4);
		extension[4] = '\0';
		string_to_lower(extension);
		if (strcmp(extension, "jpg") && strcmp(extension, "jpeg") && strcmp(extension, "png"))
			continue;

		dot = strrchr(name, '.');
		base_len = dot != NULL ? (size_t)(dot - name) : strlen(name);
		{
			char basename[256];
			if (base_len >= sizeof(basename))
			{
				INFO_LOG(RENDERER, "Invalid hash %.*s\n", (int)base_len, name);
				continue;
			}
			memcpy(basename, name, base_len);
			basename[base_len] = '\0';
			if (parse_hash(basename, &hash) < base_len)
			{
				INFO_LOG(RENDERER, "Invalid hash %s\n", basename);
				continue;
			}
		}
		len = strlen(name) + 1;
		if (entry_count == room)
		{
			custom_entry_t *e;
			room = room ? room * 2 : 64;
			e = (custom_entry_t *)realloc(entries, room * sizeof(*e));
			if (e == NULL)
				break;
			entries = e;
		}
		if (names_size + len > names_room)
		{
			char *n;
			while (names_size + len > names_room)
				names_room = names_room ? names_room * 2 : 4096;
			n = (char *)realloc(names, names_room);
			if (n == NULL)
				break;
			names = n;
		}
		memcpy(names + names_size, name, len);
		entries[entry_count].hash = hash;
		entries[entry_count].order = (u32)entry_count;
		entries[entry_count].name = names_size;
		names_size += len;
		entry_count++;
	}
	retro_closedir(dir);
	if (entry_count != 0)
	{
		/* (of two of a hash, the one listed later) */
		qsort(entries, entry_count, sizeof(*entries), entry_cmp);
		for (i = 0, kept = 0; i < entry_count; i++)
			if (i + 1 == entry_count || entries[i + 1].hash != entries[i].hash)
				entries[kept++] = entries[i];
		entry_count = kept;
	}
	retro_atomic_store_release_int(&custom_textures_available, entry_count != 0);
}

static const custom_entry_t *find(u32 hash)
{
	size_t lo = 0, hi = entry_count;
	while (lo < hi)
	{
		const size_t mid = lo + (hi - lo) / 2;
		if (entries[mid].hash == hash)
			return &entries[mid];
		if (entries[mid].hash < hash)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

/* Loader thread: the picture named @hash, decoded (malloc'd), or NULL */
static u8 *load_picture(u32 hash, u32 *width, u32 *height)
{
	static char path[PATH_MAX_LENGTH];
	const custom_entry_t *e = find(hash);
	core_file *f;
	const u8 *data;
	u8 *buf = NULL;
	u8 *image = NULL;
	size_t len;

	if (e == NULL)
		return NULL;
	strlcpy(path, textures_path, sizeof(path));
	strlcat(path, names + e->name, sizeof(path));
	f = core_fopen(path);
	if (f == NULL)
		return NULL;
	/* Decoded out of the mapping when there is one, else from one read. */
	len = core_fsize(f);
	data = core_fmap(f, NULL);
	if (data == NULL)
	{
		buf = (u8 *)malloc(len ? len : 1);
		if (buf != NULL && core_fread_at(f, 0, buf, len) == len)
			data = buf;
	}
	if (data != NULL)
	{
		unsigned w = 0, h = 0;
		if ((image = custom_image_decode(data, len, &w, &h)) != NULL)
		{
			*width = w;
			*height = h;
		}
	}
	free(buf);
	core_fclose(f);
	return image;
}

/* Loader thread: load the custom image for one queued texture. Requests
 * that arrive for it meanwhile are not queued again (the count is not
 * zero), so go round until the count this pass answered is all there is.
 *
 * Nothing of the texture is read here but what the request left for it,
 * and nothing written but the image and its size, which the renderer
 * only looks at once the count is back to zero. */
static void load(custom_request_t *r)
{
	for (;;)
	{
		/* The count, and only then the names: see custom_texture.h. */
		const int requests = retro_atomic_load_acquire_int(&r->in_progress);
		const u32 hash       = (u32)retro_atomic_load_acquire_int(&r->hash);
		const u32 old_vqhash = (u32)retro_atomic_load_acquire_int(&r->old_vqhash);
		const u32 old_hash   = (u32)retro_atomic_load_acquire_int(&r->old_hash);
		u32 width = 0, height = 0;
		u8 *image_data;

		if (r->image_data != NULL)
		{
			free(r->image_data);
			r->image_data = NULL;
		}
		image_data = load_picture(hash, &width, &height);
		/* under the names it has had before */
		if (image_data == NULL && old_vqhash != 0)
			image_data = load_picture(old_vqhash, &width, &height);
		if (image_data == NULL)
			image_data = load_picture(old_hash, &width, &height);
		if (image_data != NULL)
		{
			r->width = width;
			r->height = height;
			r->image_data = image_data;
		}
		if (retro_atomic_fetch_sub_int(&r->in_progress, requests) == requests)
			break;
	}
}

#if !defined(TARGET_NO_THREADS)
static bool wakeup_consume(void)
{
	return retro_atomic_load_acquire_int(&wakeup)
		&& retro_atomic_exchange_int(&wakeup, 0);
}

static void wakeup_wait(void)
{
	for (;;)
	{
		int key;
		if (wakeup_consume())
			return;
		key = retro_eventcount_prepare_wait(&wakeup_ec);
		if (wakeup_consume())
		{
			retro_eventcount_cancel_wait(&wakeup_ec);
			return;
		}
		retro_eventcount_commit_wait(&wakeup_ec, key);
	}
}

static void wakeup_set(void)
{
	retro_atomic_store_release_int(&wakeup, 1);
	retro_eventcount_notify(&wakeup_ec);
}

static void loader(void *param)
{
	(void)param;
	load_map();
	while (retro_atomic_load_acquire_int(&initialized))
	{
		custom_request_t *r = work_take_all();

		while (r != NULL)
		{
			/* Read the link first: once the count is back to zero the
			 * texture may be queued again, or deleted. */
			custom_request_t *next = r->next;

			if (retro_atomic_load_acquire_int(&initialized))
				load(r);
			else
				retro_atomic_store_release_int(&r->in_progress, 0);
			r = next;
		}

		wakeup_wait();
	}
}
#endif

bool custom_texture_available(void)
{
	if (!retro_atomic_load_relaxed_int(&initialized))
	{
		char id[64];

		retro_atomic_store_release_int(&initialized, 1);
		game_id(id, sizeof(id));
		if (id[0] != '\0')
		{
			strlcpy(textures_path, retro_get_system_directory(), sizeof(textures_path));
			strlcat(textures_path, "/dc/textures/", sizeof(textures_path));
			strlcat(textures_path, id, sizeof(textures_path));
			strlcat(textures_path, "/", sizeof(textures_path));

			if (path_is_directory(textures_path))
			{
				INFO_LOG(RENDERER, "Found custom textures directory: %s\n", textures_path);
				retro_atomic_store_release_int(&custom_textures_available, 1);
#if !defined(TARGET_NO_THREADS)
				if (!wakeup_ec_made)
					wakeup_ec_made = retro_eventcount_init(&wakeup_ec);
				retro_atomic_int_init(&wakeup, 0);
				if (wakeup_ec_made)
					loader_thread = sthread_create(loader, NULL);
				/* No loader, nothing to give work to: a texture queued for
				 * it would wait for good, and could not be freed. */
				if (loader_thread == NULL)
#endif
				{
					WARN_LOG(RENDERER, "The custom texture loader could not be started\n");
					retro_atomic_store_release_int(&custom_textures_available, 0);
				}
			}
		}
	}
	return retro_atomic_load_acquire_int(&custom_textures_available) != 0;
}

void custom_texture_terminate(void)
{
	if (retro_atomic_load_relaxed_int(&initialized))
	{
		custom_request_t *r;

		retro_atomic_store_release_int(&initialized, 0);
#if !defined(TARGET_NO_THREADS)
		if (loader_thread != NULL)
		{
			wakeup_set();
			sthread_join(loader_thread);
			loader_thread = NULL;
		}
#endif
		/* The loader is gone: drop what it did not get to. */
		r = work_take_all();
		while (r != NULL)
		{
			custom_request_t *next = r->next;
			retro_atomic_store_release_int(&r->in_progress, 0);
			r = next;
		}
		forget_map();
		/* The next game starts from nothing: left set, a game with no
		 * textures of its own inherited "available" from the one before
		 * and queued work for a loader that was never started. */
		retro_atomic_store_release_int(&custom_textures_available, 0);
		textures_path[0] = '\0';
	}
}

/* Renderer: ask for this texture's custom image. Its names were worked
 * out by the renderer - it is updating the texture, and the game is held
 * while it does - and are left for the loader, before the request is
 * counted. */
void custom_texture_request(custom_request_t *r, u32 hash, u32 old_vqhash, u32 old_hash)
{
	retro_atomic_store_release_int(&r->hash, (int)hash);
	retro_atomic_store_release_int(&r->old_vqhash, (int)old_vqhash);
	retro_atomic_store_release_int(&r->old_hash, (int)old_hash);

	if (retro_atomic_fetch_add_int(&r->in_progress, 1) == 0)
	{
		work_push(r);
#if !defined(TARGET_NO_THREADS)
		wakeup_set();
#endif
	}
}

void custom_texture_dump(u32 hash, int w, int h, enum custom_image_texels type, const void *src)
{
	static char dir[PATH_MAX_LENGTH], path[PATH_MAX_LENGTH];
	char id[64];

	writable_data_path(dir, sizeof(dir), "/texdump/");
	if (!path_is_valid(dir))
		path_mkdir(dir);
	game_id(id, sizeof(id));
	if (id[0] == '\0')
		return;
	strlcat(dir, id, sizeof(dir));
	strlcat(dir, "/", sizeof(dir));
	if (!path_is_valid(dir))
		path_mkdir(dir);
	{
		/* <hash in hex, small letters, no zeros before>.png */
		char name[16];
		int n = 0, b;
		for (b = 28; b >= 0; b -= 4)
		{
			const unsigned d = (hash >> b) & 15;
			if (d != 0 || n != 0 || b == 0)
				name[n++] = "0123456789abcdef"[d];
		}
		memcpy(name + n, ".png", 5);
		strlcpy(path, dir, sizeof(path));
		strlcat(path, name, sizeof(path));
	}
	if (!custom_image_dump(path, src, (unsigned)w, (unsigned)h, type))
		WARN_LOG(RENDERER, "dumpTexture: %s not written\n", path);
}
