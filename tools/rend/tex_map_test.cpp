/* The texture cache's map (rend/tex_map.h) against a plain list of what it
 * should hold: random keys added, found and taken away, in runs that
 * collide on purpose (keys a multiple of the index's size apart); every
 * texture's address the same for as long as it is there; each texture
 * made and destroyed once (a count of the live ones), value-initialized -
 * its fields with no initializer at zero, as a map's new value's are, the
 * first and every time after; Each() seeing every
 * one once; Clear(); and a map with no room left growing, which is said.
 * tools/rend/tex_map_test.sh builds it, with ASan and UBSan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (what rend/tex_map.h takes from the core) */
#include "types.h"

static int live;		/* textures constructed and not yet destroyed */
static int warned;

struct Counted
{
	u32 magic;
	Counted() : magic(0x7E57) { live++; }
	~Counted() { magic = 0xDEAD; live--; }
};
/* (no constructor of its own: a texture's fields with no initializer, as
 * the renderers' have, start at zero only if it is value-initialized) */
struct Tex
{
	Counted c;
	u64 key;
	u32 plain[16];
};

#include "rend/tex_map.h"

#define MAXK 6000
static u64 keys[MAXK];
static Tex *where[MAXK];
static int nkeys;

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

static void check_all(TexMap<Tex>& map)
{
	int i, seen = 0;
	for (i = 0; i < nkeys; i++)
	{
		Tex *t = map.Find(keys[i]);
		CHECK(t == where[i], "a texture found where it was put");
		CHECK(t != NULL && t->key == keys[i] && t->c.magic == 0x7E57, "the texture its key's");
	}
	CHECK(map.Size() == (u32)nkeys, "the size");
	map.Each([&](u64 key, Tex& t) {
		const int r = find_ref(key);
		CHECK(r >= 0 && where[r] == &t, "Each(): a texture there is, once");
		seen++;
		return true;
	});
	CHECK(seen == nkeys, "Each(): every texture");
}

static void run(TexMap<Tex>& map, int rounds, u64 spread, u64 step)
{
	int round;
	for (round = 0; round < rounds; round++)
	{
		const u64 key = (rnd() % spread) * step;
		const int r = find_ref(key);
		if (r < 0 && nkeys < MAXK && (rnd() & 3) != 0)
		{
			Tex *t = map.Find(key);
			CHECK(t == NULL, "a key not added not found");
			t = map.Add(key);
			CHECK(t != NULL && t->c.magic == 0x7E57 && t->key == 0, "a new texture, made anew");
			if (t != NULL)
			{
				u32 k, any = 0;
				for (k = 0; k < 16; k++)
					any |= t->plain[k];
				CHECK(any == 0, "a new texture's fields with no initializer at zero");
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
			map.Remove(key);
			CHECK(map.Find(key) == NULL, "a key taken away not found");
			keys[r] = keys[nkeys - 1];
			where[r] = where[nkeys - 1];
			nkeys--;
		}
		if (round % 997 == 0)
			check_all(map);
	}
	check_all(map);
}

int main(void)
{
	{
		TexMap<Tex> map;
		map.Reserve(4096);
		CHECK(live == 4096, "the room made at the start, 4096 textures");
		/* (random keys; then keys the index's size apart, which all have one home) */
		run(map, 200000, 3000, 1);
		run(map, 100000, 3000, 1ull << 33);
		run(map, 100000, 2000, 8192);
		CHECK(live == 4096, "every texture taken away made anew, none more");
		CHECK(warned == 0, "no room made while there was some");
		map.Clear();
		nkeys = 0;
		check_all(map);
		CHECK(live == 4096, "Clear(): the room kept");
		/* no room left: more made, and said */
		{
			int i;
			for (i = 0; i < 5000; i++)
			{
				Tex *t = map.Add((u64)i * 77 + 5);
				CHECK(t != NULL, "added past the room made");
				if (t == NULL)
					break;
				t->key = (u64)i * 77 + 5;
				keys[nkeys] = t->key;
				where[nkeys] = t;
				nkeys++;
			}
			CHECK(warned == 1, "no room left: said, once");
			check_all(map);
			run(map, 50000, 6000, 3);
		}
	}
	CHECK(live == 0, "every texture destroyed with the map");
	if (failures)
	{
		printf("tex_map_test: %d failed\n", failures);
		return 1;
	}
	printf("tex_map_test: the map held what it was given, at the same addresses\n");
	return 0;
}
