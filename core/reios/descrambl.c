/*
	Based on work of Marcus Comstedt
	http://mc.pp.se, http://mc.pp.se/dc/sw.html, http://mc.pp.se/dc/files/scramble.c
	License: Gotta verify

	Adapted by Stefanos Kornilios Mitsis Poiitidis (skmp) for reicast
*/

#include "descrambl.h"
#include <stdlib.h>

#define MAXCHUNK (2048*1024)

static unsigned int seed;

static void my_srand(unsigned int n)
{
	seed = n & 0xffff;
}

static unsigned int my_rand(void)
{
	seed = (seed * 2109 + 9273) & 0x7fff;
	return (seed + 0xc000) & 0xffff;
}

static void load_chunk(u8 **srcp, unsigned char *ptr, unsigned long sz)
{
	static int idx[MAXCHUNK / 32];
	u8 *src = *srcp;
	u32 j;
	int i;

	verify(sz <= MAXCHUNK);

	/* Convert chunk size to number of slices */
	sz /= 32;

	/* Initialize index table with unity,
	so that each slice gets loaded exactly once */
	for (j = 0; j < sz; j++)
		idx[j] = (int)j;

	for (i = (int)sz - 1; i >= 0; --i)
	{
		/* Select a replacement index */
		int x = (int)((my_rand() * (unsigned)i) >> 16);
		int t;

		/* Swap */
		t = idx[i];
		idx[i] = idx[x];
		idx[x] = t;

		/* Load resulting slice */
		memcpy(ptr + 32 * idx[i], src, 32);
		src += 32;
	}
	*srcp = src;
}

static void descrambl_buffer(u8* src, unsigned char *dst, unsigned long filesz)
{
	unsigned long chunksz;

	my_srand(filesz);

	/* Descramble 2 meg blocks for as long as possible, then
	gradually reduce the window down to 32 bytes (1 slice) */
	for (chunksz = MAXCHUNK; chunksz >= 32; chunksz >>= 1)
		while (filesz >= chunksz)
		{
			load_chunk(&src, dst, chunksz);
			filesz -= chunksz;
			dst += chunksz;
		}

	/* Load final incomplete slice */
	if (filesz)
		memcpy(dst, src, filesz);
}

void descrambl_file(u32 FAD, u32 file_size, u8* dst)
{
	/* (zeroed: sectors the drive does not give are not whatever was in the block) */
	u8* temp_file = (u8 *)calloc(file_size + 2048, 1);
	if (temp_file == NULL)
		return;
	libGDR_ReadSector(temp_file, FAD, (file_size+2047) / 2048, 2048);

	descrambl_buffer(temp_file, dst, file_size);

	free(temp_file);
}
