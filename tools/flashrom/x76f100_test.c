/* The X76F100 serial flash (core/hw/flashrom/x76f100.c) by itself, driven
 * through its four lines as the NAOMI's port drives them (chip select and
 * reset, then the clock, then the data, in each write):
 *
 * - the response to reset is the chip's first four bytes, LSB first;
 * - a read, after the command, a password and the acknowledge poll, gives
 *   the data from eight bytes into the block asked for on (the password's
 *   bytes counted), as far as the 112 bytes go;
 * - a write puts eight bytes in a block, and a read gives them back;
 * - the chip's state, written and read back, is written again the same;
 * - and lines driven at random, and the state at the end, as before: a
 *   hash of all the chip said, one line (see x76f100_run.sh, which holds
 *   it against x76f100_expected.txt).
 *
 * Built as C, or as C++ against a tree whose chip is a C++ class (with a
 * shim of the same names). */
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "hw/flashrom/x76f100.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	memcpy(*dest, src, src_size);
	*dest = (u8 *)*dest + src_size;
	*total_size += src_size;
	return true;
}
bool ra_unserialize(void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	memcpy(src, *dest, src_size);
	*dest = (u8 *)*dest + src_size;
	*total_size += src_size;
	return true;
}

static X76F100 chip;
static u32 said = 2166136261u;	/* FNV-1a of every SDA read */

static void port(int cs, int rst, int scl, int sda)
{
	x76f100_writeCS(&chip, cs != 0);
	x76f100_writeRST(&chip, rst != 0);
	x76f100_writeSCL(&chip, scl != 0);
	x76f100_writeSDA(&chip, sda != 0);
}

static int sda(void)
{
	int v = x76f100_readSDA(&chip) ? 1 : 0;
	said = (said ^ (u32)v) * 16777619u;
	return v;
}

static void start(void)
{
	port(0, 0, 1, 1);
	port(0, 0, 1, 0);
}

static void stop(void)
{
	port(0, 0, 0, 0);
	port(0, 0, 1, 0);
	port(0, 0, 1, 1);
}

/* A byte to the chip, MSB first, and the ninth clock */
static void send(u8 b)
{
	int i;
	for (i = 7; i >= 0; i--)
	{
		port(0, 0, 0, (b >> i) & 1);
		port(0, 0, 1, (b >> i) & 1);
	}
	port(0, 0, 0, 1);
	port(0, 0, 1, 1);
}

/* A byte from the chip, MSB first; @more: acknowledged, for the next one */
static u8 receive(int more)
{
	u8 b = 0;
	int i;
	for (i = 0; i < 8; i++)
	{
		port(0, 0, 0, 1);
		port(0, 0, 1, 1);
		b = (u8)((b << 1) | sda());
	}
	port(0, 0, 0, more ? 0 : 1);
	port(0, 0, 1, more ? 0 : 1);
	return b;
}

static void command(u8 cmd)
{
	int i;
	start();
	send(cmd);
	for (i = 0; i < 8; i++)
		send((u8)(0xa0 + i));
	send(0x55);
}

static u8 image[0x84];
static u8 state1[512], state2[512];

static unsigned save(u8 *to)
{
	void *p = to;
	unsigned int total = 0;
	x76f100_serialize(&chip, &p, &total);
	return total;
}

static void load(u8 *from)
{
	void *p = from;
	unsigned int total = 0;
	x76f100_unserialize(&chip, &p, &total);
}

int main(void)
{
	u8 got[112], blk[8];
	unsigned i, n, size;
	u32 seed = 1;

	for (i = 0; i < sizeof(image); i++)
		image[i] = (u8)(i * 37 + 11);
	x76f100_setData(&chip, image);
	x76f100_reset(&chip);
	CHECK(memcmp(x76f100_getProtectedData(&chip), image + 20, 112) == 0);

	/* The response to reset */
	port(1, 0, 1, 1);
	port(0, 0, 1, 1);
	port(0, 1, 1, 1);
	for (i = 0; i < 32; i++)
	{
		port(0, 1, 0, 1);
		CHECK(sda() == ((image[i / 8] >> (i % 8)) & 1));
		port(0, 1, 1, 1);
	}
	/* (deselected and selected again: stopped) */
	port(1, 0, 1, 1);
	port(0, 0, 1, 1);

	/* A read from block 0. The eight bytes of the password have moved the
	 * chip's place on, as it has always done here: the bytes come from
	 * eight on, and past the 112 bytes as zeroes. */
	command(0x81);
	for (i = 0; i < 112; i++)
		got[i] = receive(i != 111);
	stop();
	CHECK(memcmp(got, image + 20 + 8, 104) == 0);
	for (i = 104; i < 112; i++)
		CHECK(got[i] == 0);

	/* Block 5 written, then read from block 4 */
	command(0x80 | (5 << 1));
	for (i = 0; i < 8; i++)
	{
		blk[i] = (u8)(0xc0 ^ i);
		send(blk[i]);
	}
	stop();
	command(0x81 | (4 << 1));
	for (i = 0; i < 16; i++)
		got[i] = receive(i != 15);
	stop();
	CHECK(memcmp(got, blk, 8) == 0);
	CHECK(memcmp(got + 8, image + 20 + 48, 8) == 0);

	/* The state in the middle of a read */
	command(0x81 | (2 << 1));
	receive(1);
	size = save(state1);
	CHECK(size == 142);
	x76f100_reset(&chip);
	load(state1);
	CHECK(save(state2) == size);
	CHECK(memcmp(state1, state2, size) == 0);
	CHECK(receive(0) == image[20 + 16 + 8 + 1]);
	stop();

	/* The lines at random */
	for (n = 0; n < 200000; n++)
	{
		seed = seed * 1103515245u + 12345u;
		port((seed >> 16) & 1 && (seed >> 20) % 16 == 0, (seed >> 17) & 1 && (seed >> 24) % 8 == 0,
				(seed >> 18) & 1, (seed >> 19) & 1);
		sda();
		if (n % 50000 == 49999)
		{
			size = save(state1);
			load(state1);
			CHECK(save(state2) == size && memcmp(state1, state2, size) == 0);
		}
	}
	size = save(state1);
	for (i = 0; i < size; i++)
		said = (said ^ state1[i]) * 16777619u;
	printf("%08x\n", said);

	if (failures)
	{
		fprintf(stderr, "x76f100_test: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
