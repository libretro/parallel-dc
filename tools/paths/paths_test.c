/* Where the core writes its files (core/paths.c): the paths as they always
 * were, a content's name of any length (a memory card's name was once made
 * in 512 bytes, past which it ran over), and paths cut short at the room
 * they are given, with their whole length said (sh tools/paths/paths_run.sh). */
#include <stdio.h>
#include <string.h>
#include "paths.h"

char game_dir_no_slash[1024] = "/sys/dc";
char vmu_dir_no_slash[4096] = "/saves/dc";
char content_name[4096] = "Game";
unsigned per_content_vmus;

static int fails;

#ifdef _WIN32
#define S "\\"
#else
#define S "/"
#endif

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
	char out[8192], small[8];
	size_t n;
	unsigned i;

	n = writable_data_path(out, sizeof(out), "/texdump/");
	CHECK(strcmp(out, "/sys/dc" S "/texdump/") == 0 && n == strlen(out));
	n = writable_data_path(out, sizeof(out), "");
	CHECK(strcmp(out, "/sys/dc" S) == 0 && n == strlen(out));

	per_content_vmus = 0;
	writable_vmu_path(out, sizeof(out), "A1");
	CHECK(strcmp(out, "/sys/dc" S "vmu_save_A1.bin") == 0);
	per_content_vmus = 1;
	writable_vmu_path(out, sizeof(out), "A1");
	CHECK(strcmp(out, "/saves/dc" S "Game.A1.bin") == 0);
	writable_vmu_path(out, sizeof(out), "B1");
	CHECK(strcmp(out, "/sys/dc" S "vmu_save_B1.bin") == 0);
	per_content_vmus = 2;
	writable_vmu_path(out, sizeof(out), "C2");
	CHECK(strcmp(out, "/saves/dc" S "Game.C2.bin") == 0);

	/* a content's name longer than the 512 bytes it was once made in */
	for (i = 0; i < 3000; i++)
		content_name[i] = (char)('a' + i % 26);
	content_name[3000] = '\0';
	n = writable_vmu_path(out, sizeof(out), "A1");
	CHECK(n == strlen("/saves/dc" S) + 3000 + strlen(".A1.bin"));
	CHECK(strlen(out) == n);
	CHECK(strcmp(out + n - 7, ".A1.bin") == 0);

	/* cut short: as much as there is room for, and the whole length said */
	n = writable_vmu_path(small, sizeof(small), "A1");
	CHECK(n == strlen("/saves/dc" S) + 3000 + strlen(".A1.bin"));
	CHECK(strcmp(small, "/saves/") == 0);
	n = writable_data_path(small, 1, "x");
	CHECK(small[0] == '\0' && n == strlen("/sys/dc" S "x"));
	CHECK(writable_data_path(NULL, 0, "x") == n);

	if (fails)
	{
		printf("paths_test: %d failed\n", fails);
		return 1;
	}
	printf("paths_test: the paths as they should be\n");
	return 0;
}
