/* The texture cache's map (rend/tex_map.h) against a plain list of what it
 * should hold: random keys added, found and taken away, in runs that
 * collide on purpose (keys a multiple of the index's size apart); every
 * texture's address the same for as long as it is there; each texture
 * made and unmade once (a count of the live ones), its fields at zero when
 * the map makes it (make NULL) or as its own make leaves them, the first
 * and every time after; every one there is seen once going through them
 * all; tex_map_clear(); and a map with no room left growing, which is
 * said. tools/rend/tex_map_test.sh builds it, with ASan and UBSan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (what rend/tex_map.h takes from the core) */
#include "types.h"
#include "rend/tex_map.h"

static int live;		/* textures made and not yet unmade */
int warned;

#define MAGIC 0x7E57

typedef struct Tex
{
	u32 magic;
	u64 key;
	u32 plain[16];
} Tex;

/* A make of the texture's own: everything at zero but its mark */
static void tex_make(void *e)
{
	Tex *t = (Tex *)e;
	memset(t, 0, sizeof(*t));
	t->magic = MAGIC;
	live++;
}
static void tex_unmake(void *e)
{
	((Tex *)e)->magic = 0xDEAD;
	live--;
}

#define MAXK 6000
static u64 keys[MAXK];
static Tex *where[MAXK];
static int nkeys;
static u32 magic;		/* what a new texture's mark is: MAGIC, or 0 made by the map */

static int failures;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s (line %d)\n", what, __LINE__); failures++; } } while (0)

static u64 rnd_state = 88172645463325252ull;
static u64 rnd(void)
{
	rnd_state ^= rnd_state << 13;
	rnd_state ^= rnd_state >> 7;
	rnd_state ^= rnd_state << 17;
	return rnd_state;
}

static int find_ref(u64 key)
{
	int i;
	for (i = 0; i < nkeys; i++)
		if (keys[i] == key)
			return i;
	return -1;
}

static void check_all(tex_map_t *map)
{
	int i, seen = 0;
	u32 e;
	u64 key;
	for (i = 0; i < nkeys; i++)
	{
		Tex *t = (Tex *)tex_map_find(map, keys[i]);
		CHECK(t == where[i], "a texture found where it was put");
		CHECK(t != NULL && t->key == keys[i] && t->magic == magic, "the texture its key's");
	}
	CHECK(map->count == (u32)nkeys, "the size");
	for (e = 0; e < tex_map_places(map); e++)
	{
		Tex *t = (Tex *)tex_map_at(map, e, &key);
		if (t != NULL)
		{
			const int r = find_ref(key);
			CHECK(r >= 0 && where[r] == t, "going through them: a texture there is, once");
			seen++;
		}
	}
	CHECK(seen == nkeys, "going through them: every texture");
}

static void run(tex_map_t *map, int rounds, u64 spread, u64 step)
{
	int round;
	for (round = 0; round < rounds; round++)
	{
		const u64 key = (rnd() % spread) * step;
		const int r = find_ref(key);
		if (r < 0 && nkeys < MAXK && (rnd() & 3) != 0)
		{
			Tex *t = (Tex *)tex_map_find(map, key);
			CHECK(t == NULL, "a key not added not found");
			t = (Tex *)tex_map_add(map, key);
			CHECK(t != NULL && t->magic == magic && t->key == 0, "a new texture, made anew");
			if (t != NULL)
			{
				u32 k, any = 0;
				for (k = 0; k < 16; k++)
					any |= t->plain[k];
				CHECK(any == 0, "a new texture's fields at zero");
				t->plain[(u32)key & 15] = 0xFFFFFFFF;	/* (for the next in its place) */
			}
			if (t == NULL)
				return;
			t->key = key;
			keys[nkeys] = key;
			where[nkeys] = t;
			nkeys++;
		}
		else if (r >= 0)
		{
			tex_map_remove(map, key);
			CHECK(tex_map_find(map, key) == NULL, "a key taken away not found");
			keys[r] = keys[nkeys - 1];
			where[r] = where[nkeys - 1];
			nkeys--;
		}
		if (round % 997 == 0)
			check_all(map);
	}
	check_all(map);
}

/* With the texture's own make and unmake, or the map's zeros */
static void test(bool own)
{
	tex_map_t map;
	int i;

	magic = own ? MAGIC : 0;
	nkeys = 0;
	warned = 0;
	tex_map_init(&map, sizeof(Tex), own ? tex_make : NULL, own ? tex_unmake : NULL);
	map.name = "Texture cache";
	tex_map_reserve(&map, 4096);
	if (own)
		CHECK(live == 4096, "the room made at the start, 4096 textures");
	/* (random keys; then keys the index's size apart, which all have one home) */
	run(&map, 200000, 3000, 1);
	run(&map, 100000, 3000, 1ull << 33);
	run(&map, 100000, 2000, 8192);
	if (own)
		CHECK(live == 4096, "every texture taken away made anew, none more");
	CHECK(warned == 0, "no room made while there was some");
	tex_map_clear(&map);
	nkeys = 0;
	check_all(&map);
	if (own)
		CHECK(live == 4096, "tex_map_clear(): the room kept");
	/* no room left: more made, and said */
	for (i = 0; i < 5000; i++)
	{
		Tex *t = (Tex *)tex_map_add(&map, (u64)i * 77 + 5);
		CHECK(t != NULL, "added past the room made");
		if (t == NULL)
			break;
		t->key = (u64)i * 77 + 5;
		keys[nkeys] = t->key;
		where[nkeys] = t;
		nkeys++;
	}
	CHECK(warned == 1, "no room left: said, once");
	check_all(&map);
	run(&map, 50000, 6000, 3);
	tex_map_free(&map);
	CHECK(live == 0, "every texture unmade with the map");
}

int main(void)
{
	test(true);
	test(false);
	if (failures)
	{
		printf("tex_map_test: %d failed\n", failures);
		return 1;
	}
	printf("tex_map_test: the map held what it was given, at the same addresses\n");
	return 0;
}
