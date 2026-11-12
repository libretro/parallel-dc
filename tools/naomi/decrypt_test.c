/* The 315-5881 decryption chip (core/hw/naomi/decrypt.c) by itself, over
 * a cartridge of made-up ciphered data: for each of a set of keys, subkeys
 * and start addresses, a hash of the words it gives, one line each (see
 * decrypt_run.sh, which holds the lines against decrypt_expected.txt). The
 * data being random, the block headers it decrypts to have both plain and
 * compressed blocks.
 *
 * Also that the chip's part of a state, written in the middle of a stream
 * and read back, carries on the stream as it was.
 *
 * Built as C, or as C++ against a tree whose chip is C++. */
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "hw/naomi/decrypt.h"

#define CART_WORDS 0x40000
static u16 cart[CART_WORDS];

u16 naomi_m2_read_ciphered(u32 addr)
{
	return cart[addr % CART_WORDS];
}

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

static void start(u32 key, u32 addr, u16 subkey)
{
	cryptoReset();
	cyptoSetKey(key);
	cyptoSetLowAddr((u16)addr);
	cyptoSetHighAddr((u16)(addr >> 16));
	cyptoSetSubkey(subkey);
}

/* FNV-1a over the words */
static u32 hash_words(u32 key, u32 n)
{
	u32 h = 2166136261u;
	u32 i;
	for (i = 0; i < n; i++)
	{
		u16 w;
		cyptoSetKey(key);
		w = cryptoDecrypt();
		h = (h ^ (w & 0xff)) * 16777619u;
		h = (h ^ (w >> 8)) * 16777619u;
	}
	return h;
}

static u8 state[4096];

int main(void)
{
	static const u32 keys[] = { 0x00000000, 0x0008ad01, 0x00122501, 0x0a4a3f12, 0x0ffffffe };
	static const u32 addrs[] = { 0, 0x1234, 0x3fff0 };
	static const u16 subkeys[] = { 0x0000, 0x5aa5, 0xffff };
	u32 seed = 12345, i, k, a, s;
	int failures = 0;

	for (i = 0; i < CART_WORDS; i++)
	{
		seed = seed * 1103515245u + 12345u;
		cart[i] = (u16)(seed >> 8);
	}

	for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
		for (a = 0; a < sizeof(addrs) / sizeof(addrs[0]); a++)
			for (s = 0; s < sizeof(subkeys) / sizeof(subkeys[0]); s++)
			{
				u32 h, h1, h2;
				unsigned int total;
				void *p;

				start(keys[k], addrs[a], subkeys[s]);
				h = hash_words(keys[k], 20000);
				printf("%08x %05x %04x %08x\n", keys[k], addrs[a], subkeys[s], h);

				/* A state in the middle of the stream */
				start(keys[k], addrs[a], subkeys[s]);
				hash_words(keys[k], 7001);
				p = state; total = 0;
				cryptoSerialize(&p, &total);
				h1 = hash_words(keys[k], 3000);
				start(0x0badf00d, 0x2222, 0x1111);
				hash_words(0x0badf00d, 123);
				p = state; total = 0;
				cryptoUnserialize(&p, &total);
				h2 = hash_words(keys[k], 3000);
				if (h1 != h2)
				{
					fprintf(stderr, "FAIL: key %08x addr %05x subkey %04x: the stream after a state is not the same\n",
							keys[k], addrs[a], subkeys[s]);
					failures++;
				}
			}
	return failures != 0;
}
