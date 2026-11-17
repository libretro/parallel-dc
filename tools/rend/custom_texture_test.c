/* Custom textures (core/rend/custom_texture.c) by themselves, its loader
 * thread and all: the directory the game's id names, the pictures in it
 * by the hash their names are (as strtoll() reads them, two of a hash the
 * later one), a texture's picture loaded under its name or the ones it had
 * before, asked for again and again while the loader works, the loader
 * stopped with work waiting, the next game starting from nothing, and
 * textures dumped where they go. The file system, the files and the
 * pictures are this test's. See custom_texture_run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "rend/custom_texture.h"
#include "reios/reios.h"
#include "hw/naomi/naomi_cart.h"
#include "paths.h"
#include "deps/coreio/coreio.h"
#include <retro_dirent.h>
#include <file/file_path.h>
#include <rthreads/rthreads.h>
#include <retro_timers.h>

settings_t settings;
ip_meta_t ip_meta;
char naomi_game_id[33];
retro_log_printf_t log_cb;

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* --- the file system --- */

/* The directories there are, and what is in the textures' one (in the
 * order it lists them; a name ending in / is a directory) */
static const char *dirs_there[4];
static const char *const *listing;
static int listing_count;

typedef struct file_t
{
	const char *path;
	u32 w, h;		/* the picture in it; 0 x 0: not a picture */
	bool mapped;
} file_t;
static file_t files[32];
static int file_count;
static int opened;		/* files open */

const char *retro_get_system_directory(void)
{
	return "/sys";
}

size_t writable_data_path(char *out, size_t size, const char *filename)
{
	const size_t len = strlen("/data") + strlen(filename);
	if (size > 0)
	{
		strlcpy(out, "/data", size);
		strlcat(out, filename, size);
	}
	return len;
}

bool path_is_directory(const char *path)
{
	int i;
	for (i = 0; i < 4; i++)
		if (dirs_there[i] != NULL && strcmp(dirs_there[i], path) == 0)
			return true;
	return false;
}

static char made_dirs[8][256];
static int made_count;

bool path_is_valid(const char *path)
{
	int i;
	for (i = 0; i < made_count; i++)
		if (strcmp(made_dirs[i], path) == 0)
			return true;
	return false;
}

bool path_mkdir(const char *dir)
{
	if (made_count < 8)
		strlcpy(made_dirs[made_count++], dir, sizeof(made_dirs[0]));
	return true;
}

const char *path_get_extension(const char *path)
{
	const char *dot = strrchr(path, '.');
	return dot != NULL ? dot + 1 : "";
}

typedef struct test_dir
{
	int at;
} test_dir;
static test_dir the_dir;
static char dirent_name[128];

RDIR *retro_opendir(const char *name)
{
	if (!path_is_directory(name) || listing == NULL)
		return NULL;
	the_dir.at = -1;
	return (RDIR *)&the_dir;
}

int retro_readdir(RDIR *rdir)
{
	test_dir *d = (test_dir *)rdir;
	return ++d->at < listing_count;
}

const char *retro_dirent_get_name(RDIR *rdir)
{
	const test_dir *d = (const test_dir *)rdir;
	size_t len;
	strlcpy(dirent_name, listing[d->at], sizeof(dirent_name));
	len = strlen(dirent_name);
	if (len > 0 && dirent_name[len - 1] == '/')
		dirent_name[len - 1] = '\0';
	return dirent_name;
}

bool retro_dirent_is_dir(RDIR *rdir, const char *unused)
{
	const test_dir *d = (const test_dir *)rdir;
	const size_t len = strlen(listing[d->at]);
	(void)unused;
	return len > 0 && listing[d->at][len - 1] == '/';
}

void retro_closedir(RDIR *rdir)
{
	(void)rdir;
}

/* The file's bytes: "PIC" and its size (a picture), or not one */
static u8 file_bytes[16];

core_file *core_fopen(const char *filename)
{
	int i;
	for (i = 0; i < file_count; i++)
		if (strcmp(files[i].path, filename) == 0)
		{
			opened++;
			return (core_file *)&files[i];
		}
	return NULL;
}

size_t core_fsize(core_file *fc)
{
	(void)fc;
	return 11;
}

static void fill(const file_t *f, u8 *out)
{
	memcpy(out, f->w != 0 ? "PIC" : "NOT", 3);
	memcpy(out + 3, &f->w, 4);
	memcpy(out + 7, &f->h, 4);
}

const uint8_t *core_fmap(core_file *fc, size_t *len)
{
	const file_t *f = (const file_t *)fc;
	(void)len;
	if (!f->mapped)
		return NULL;
	fill(f, file_bytes);
	return file_bytes;
}

size_t core_fread_at(core_file *fc, size_t offset, void *buf, size_t len)
{
	u8 b[11];
	fill((const file_t *)fc, b);
	if (offset != 0 || len != 11)
		return 0;
	memcpy(buf, b, 11);
	return 11;
}

int core_fclose(core_file *fc)
{
	(void)fc;
	opened--;
	return 0;
}

uint8_t *custom_image_decode(const uint8_t *data, size_t len,
		unsigned *width, unsigned *height)
{
	u32 w, h;
	u8 *image;
	if (len != 11 || memcmp(data, "PIC", 3) != 0)
		return NULL;
	memcpy(&w, data + 3, 4);
	memcpy(&h, data + 7, 4);
	image = (u8 *)malloc((size_t)w * h * 4);
	if (image != NULL)
	{
		memset(image, (int)(w ^ h), (size_t)w * h * 4);
		*width = w;
		*height = h;
	}
	return image;
}

static char dumped_path[256];
static enum custom_image_texels dumped_type;
static unsigned dumped_w, dumped_h;
static const void *dumped_src;

bool custom_image_dump(const char *path, const void *texels,
		unsigned width, unsigned height, enum custom_image_texels type)
{
	strlcpy(dumped_path, path, sizeof(dumped_path));
	dumped_src = texels;
	dumped_w = width;
	dumped_h = height;
	dumped_type = type;
	return true;
}

/* --- the textures --- */

static void add_file(const char *path, u32 w, u32 h, bool mapped)
{
	files[file_count].path = path;
	files[file_count].w = w;
	files[file_count].h = h;
	files[file_count].mapped = mapped;
	file_count++;
}

/* Until the loader has answered @r (a few seconds at most) */
static void wait_for(custom_request_t *r)
{
	int i;
	for (i = 0; i < 5000 && retro_atomic_load_acquire_int(&r->in_progress) != 0; i++)
		retro_sleep(1);
	CHECK(retro_atomic_load_acquire_int(&r->in_progress) == 0);
}

/* @r asked for and answered: its picture's width (0 for none) */
static u32 ask(custom_request_t *r, u32 hash, u32 old_vqhash, u32 old_hash)
{
	custom_texture_request(r, hash, old_vqhash, old_hash);
	wait_for(r);
	return r->image_data != NULL ? r->width : 0;
}

static void set_dc_id(const char *id)
{
	settings.System = DC_PLATFORM_DREAMCAST;
	memset(ip_meta.product_number, ' ', sizeof(ip_meta.product_number));
	memcpy(ip_meta.product_number, id, strlen(id));
}

/* Until the loader has listed the directory: available or not */
static bool listed(bool expect)
{
	int i;
	for (i = 0; i < 5000 && custom_texture_available() != expect; i++)
		retro_sleep(1);
	return custom_texture_available();
}

static const char *const game_one[] = {
	"1a2b.png",		/* 0x1a2b */
	"DEADBEEF.JPG",		/* the extension in any case */
	"zz.png",		/* not a hash */
	"12.gif",		/* not a picture */
	" 7.png",		/* as strtoll() reads it: 7 */
	"-2.png",		/* 0xfffffffe */
	"0x10.png",		/* 0x10 */
	"100000000.png",	/* 0 (the high word cut) */
	"8000000000000000.png",	/* past LLONG_MAX: 0xffffffff, the later one */
	"1a2b.jpeg",		/* 0x1a2b again: this one is taken */
	"abc.png/",		/* a directory */
	"abc.pngx",		/* not a picture */
	"55.jpg",		/* no file there (gone since): no picture */
	"66.png",		/* not a picture in it */
	"77.png",		/* read, not mapped */
	"0x.png"		/* "0" and then "x": not a hash */
};

int main(void)
{
	custom_request_t a, b, c;
	int i;
	u32 w;

	/* No id: nothing (whatever is in the directory no name would make) */
	set_dc_id("");
	dirs_there[2] = "/sys/dc/textures//";
	CHECK(!custom_texture_available());
	custom_texture_terminate();

	/* An id, and no directory for it */
	set_dc_id("T-1234N");
	CHECK(!custom_texture_available());
	custom_texture_terminate();

	/* A directory, with nothing usable in it: available until listed */
	{
		static const char *const junk[] = { "readme.txt", "zz.png", "dir.png/" };
		dirs_there[0] = "/sys/dc/textures/T-1234N/";
		listing = junk;
		listing_count = 3;
		CHECK(custom_texture_available());
		CHECK(!listed(false));
		custom_texture_terminate();
	}

	/* The game's: its id's spaces made underscores, the ones at the end cut */
	set_dc_id("MK 51 00");
	dirs_there[0] = "/sys/dc/textures/MK_51_00/";
	listing = game_one;
	listing_count = (int)(sizeof(game_one) / sizeof(game_one[0]));
	add_file("/sys/dc/textures/MK_51_00/1a2b.png", 1, 1, true);
	add_file("/sys/dc/textures/MK_51_00/1a2b.jpeg", 2, 2, true);
	add_file("/sys/dc/textures/MK_51_00/DEADBEEF.JPG", 3, 3, true);
	add_file("/sys/dc/textures/MK_51_00/ 7.png", 4, 4, true);
	add_file("/sys/dc/textures/MK_51_00/-2.png", 5, 5, true);
	add_file("/sys/dc/textures/MK_51_00/abc.png", 13, 13, true);
	add_file("/sys/dc/textures/MK_51_00/0x10.png", 6, 6, true);
	add_file("/sys/dc/textures/MK_51_00/100000000.png", 7, 7, true);
	add_file("/sys/dc/textures/MK_51_00/8000000000000000.png", 8, 8, true);
	add_file("/sys/dc/textures/MK_51_00/66.png", 0, 0, true);
	add_file("/sys/dc/textures/MK_51_00/77.png", 9, 9, false);
	add_file("/sys/dc/textures/MK_51_00/zz.png", 10, 10, true);
	add_file("/sys/dc/textures/MK_51_00/0x.png", 11, 11, true);
	custom_request_init(&a);
	custom_request_init(&b);
	custom_request_init(&c);
	CHECK(listed(true));

	CHECK(ask(&a, 0x1a2b, 0, 0) == 2);
	CHECK(a.height == 2);
	CHECK(ask(&a, 0xdeadbeef, 0, 0) == 3);
	CHECK(ask(&a, 7, 0, 0) == 4);
	CHECK(ask(&a, 0xffffffffu, 0, 0) == 8);
	CHECK(ask(&a, 0xfffffffeu, 0, 0) == 5);
	CHECK(ask(&a, 0xabc, 0, 0x4444) == 0);
	CHECK(ask(&a, 0x10, 0, 0) == 6);
	CHECK(ask(&a, 0, 0, 0) == 7);
	CHECK(ask(&a, 0x77, 0, 0) == 9);
	CHECK(ask(&a, 0x55, 0, 0x4444) == 0);
	CHECK(ask(&a, 0x66, 0, 0x4444) == 0);
	CHECK(ask(&a, 0x1234, 0, 0x4444) == 0);
	/* (an old name of 0 is one: only the compressed one's 0 is none) */
	CHECK(ask(&a, 0x1234, 0, 0) == 7);
	/* under the names it had before: the compressed one's, then the old */
	CHECK(ask(&a, 0x1234, 0x10, 7) == 6);
	CHECK(ask(&a, 0x1234, 0x4321, 7) == 4);
	CHECK(ask(&a, 0x1234, 0, 0x10) == 6);
	CHECK(ask(&a, 0x10, 7, 0) == 6);
	CHECK(ask(&a, 0x1234, 0x4321, 0x5678) == 0);
	CHECK(a.image_data == NULL);
	CHECK(opened == 0);

	/* Asked for again and again while the loader works: answered with
	 * the last names, the count back to zero */
	for (i = 0; i < 2000; i++)
	{
		custom_texture_request(&a, i & 1 ? 0x1a2b : 7, 0, 0);
		custom_texture_request(&b, i % 3 == 0 ? 0x10 : 0xdeadbeef, 0, 0);
		if (i % 7 == 0)
			custom_texture_request(&c, 0x9999, 0, i & 2 ? 0x4444 : 0x10);
	}
	custom_texture_request(&a, 0x1a2b, 0, 0);
	custom_texture_request(&b, 0xdeadbeef, 0, 0);
	custom_texture_request(&c, 0x9999, 0, 0x4444);
	wait_for(&a);
	wait_for(&b);
	wait_for(&c);
	CHECK(a.image_data != NULL && a.width == 2);
	CHECK(b.image_data != NULL && b.width == 3);
	CHECK(c.image_data == NULL);

	/* Asked for under other names while the loader answers: what it
	 * found under the names before is not the answer */
	for (i = 0; i < 300; i++)
	{
		int k;
		for (k = 0; k < 4; k++)
			custom_texture_request(&a, k & 1 ? 0x10 : 7, 0, 0);
		custom_texture_request(&a, i & 1 ? 0x1a2b : 0xdeadbeef, 0, 0);
		wait_for(&a);
		CHECK(a.image_data != NULL && a.width == (i & 1 ? 2u : 3u));
	}

	/* Stopped with work waiting: every count back to zero */
	for (i = 0; i < 64; i++)
	{
		custom_texture_request(&a, 7, 0, 0);
		custom_texture_request(&b, 0x10, 0, 0);
	}
	custom_texture_terminate();
	CHECK(retro_atomic_load_acquire_int(&a.in_progress) == 0);
	CHECK(retro_atomic_load_acquire_int(&b.in_progress) == 0);
	free(a.image_data);
	free(b.image_data);
	free(c.image_data);

	/* The next game, a NAOMI one: from nothing, its own directory */
	settings.System = DC_PLATFORM_NAOMI;
	strlcpy(naomi_game_id, "SOME GAME  ", sizeof(naomi_game_id));
	{
		static const char *const game_two[] = { "abc.png" };
		dirs_there[1] = "/sys/dc/textures/SOME_GAME/";
		listing = game_two;
		listing_count = 1;
		add_file("/sys/dc/textures/SOME_GAME/abc.png", 12, 3, true);
		custom_request_init(&a);
		CHECK(listed(true));
		CHECK(ask(&a, 0xabc, 0, 0) == 12);
		CHECK(ask(&a, 0x1a2b, 0, 0x4444) == 0);
		w = a.height;
		(void)w;
		custom_texture_terminate();
		free(a.image_data);
	}

	/* Dumped: <data>/texdump/<id>/<hash>.png, the directories made */
	custom_texture_dump(0xabc, 8, 4, CUSTOM_IMAGE_565, &a);
	CHECK(strcmp(dumped_path, "/data/texdump/SOME_GAME/abc.png") == 0);
	CHECK(dumped_w == 8 && dumped_h == 4 && dumped_type == CUSTOM_IMAGE_565 && dumped_src == &a);
	CHECK(made_count == 2 && strcmp(made_dirs[0], "/data/texdump/") == 0
			&& strcmp(made_dirs[1], "/data/texdump/SOME_GAME/") == 0);
	custom_texture_dump(0, 1, 1, CUSTOM_IMAGE_8888, &a);
	CHECK(strcmp(dumped_path, "/data/texdump/SOME_GAME/0.png") == 0);
	CHECK(made_count == 2);
	custom_texture_dump(0xf0000001u, 1, 1, CUSTOM_IMAGE_4444, &a);
	CHECK(strcmp(dumped_path, "/data/texdump/SOME_GAME/f0000001.png") == 0);
	/* no id: no file */
	naomi_game_id[0] = '\0';
	dumped_path[0] = '\0';
	custom_texture_dump(0x1, 1, 1, CUSTOM_IMAGE_8888, &a);
	CHECK(dumped_path[0] == '\0');

	printf("custom_texture_test: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
