/* Dinosaur King's card codec (core/hw/naomi/dinokich_card.c): the game's own
 * DES, interleaving, card encoding, byte sums, MD5 and line scrambling, run
 * over fixed inputs and hashed against what they have always made
 * (sh tools/naomi/dinokich_card_run.sh). */
#include <stdio.h>
#include <string.h>
#include "hw/naomi/dinokich_card.h"

static unsigned h = 2166136261u;

/* FNV-1a */
static void mix(const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	while (n--)
	{
		h ^= *b++;
		h *= 16777619u;
	}
}

int main(void)
{
	static const size_t md5_lens[] = { 0, 1, 2, 55, 56, 57, 63, 64, 65, 119, 120, 200, 300 };
	u8 data[300], key[8], out[300], digest[16], card[64];
	dinokich_plain_t plain;
	dinokich_md5_ctx ctx;
	unsigned i, k;
	int sum;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (u8)(i * 37 + 11);
	/* keys with no odd digit among the bytes, and with many */
	for (k = 0; k < 4; k++)
	{
		for (i = 0; i < 8; i++)
			key[i] = (u8)(k * 61 + i * 13 + '0');
		dinokich_des_encrypt(data, 64, key, out);
		mix(out, 64);
		dinokich_interleave(data, key, card);
		mix(card, 64);
		memset(data + 64, '1' + 2 * (k & 1), 56);
		dinokich_interleave(data + 64, key, card);
		mix(card, 64);
		memcpy(&plain, data + k * 8, sizeof(plain));
		dinokich_encode_card(&plain, key, card);
		mix(card, 64);
	}
	sum = dinokich_sum_bytes(data, sizeof(data));
	mix(&sum, sizeof(sum));
	for (i = 0; i < sizeof(md5_lens) / sizeof(md5_lens[0]); i++)
	{
		dinokich_md5(data, md5_lens[i], digest);
		mix(digest, 16);
	}
	/* the same, in pieces */
	dinokich_md5_init(&ctx);
	dinokich_md5_update(&ctx, data, 7);
	dinokich_md5_update(&ctx, data + 7, 100);
	dinokich_md5_update(&ctx, data + 107, 93);
	dinokich_md5_final(&ctx, digest);
	mix(digest, 16);
	dinokich_md5_scramble(data, data + 2, 100, out);
	mix(out, 100);
	printf("dinokich_card: %08x\n", h);
	return 0;
}
