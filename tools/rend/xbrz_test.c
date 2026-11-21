/* xBRZ (core/deps/xbrz) by itself: pictures of a few colours near and far
 * apart, lines, blocks and noise, with and without alpha, scaled 2 to 6
 * times in slices of rows; what it makes goes into a hash for each
 * factor and colour format, which must be what it always was
 * (xbrz_expected.txt). See xbrz_run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "deps/xbrz/xbrz.h"

#define CASES 400
#define MAX_SIDE 48

static uint32_t rng = 7;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static uint32_t src[MAX_SIDE * MAX_SIDE];
static uint32_t trg[MAX_SIDE * MAX_SIDE * 36];

/* a picture of @w x @h: a palette, colours near each other or not, laid
 * out as blocks, lines or noise */
static void picture(int w, int h, int argb)
{
	uint32_t pal[8];
	int n = 2 + (int)(rnd() % 7), i, x, y;
	int kind = (int)(rnd() % 4);
	pal[0] = rnd();
	for (i = 1; i < n; i++)
	{
		if (rnd() % 2)
			pal[i] = rnd();
		else
		{
			/* (near the one before: the equal colour tests) */
			uint32_t d = rnd() & 0x0F0F0F0Fu;
			pal[i] = pal[i - 1] ^ d;
		}
	}
	for (i = 0; i < n; i++)
	{
		if (!argb)
			pal[i] |= 0xFF000000u;
		else
			switch (rnd() % 4)
			{
			case 0: pal[i] &= 0x00FFFFFFu; break;
			case 1: pal[i] |= 0xFF000000u; break;
			default: break;
			}
	}
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			int c;
			switch (kind)
			{
			case 0: c = (int)(rnd() % (unsigned)n); break;
			case 1: c = ((x / 3) + (y / 2)) % n; break;
			case 2: c = (x * 7 + y * 3) / 9 % n; break;
			default: c = (x == y || x + y == w || x == w / 2) ? 1 : 0; break;
			}
			src[y * w + x] = pal[c];
		}
}

int main(void)
{
	int factor, argb;
	for (argb = 0; argb < 2; argb++)
		for (factor = 2; factor <= 6; factor++)
		{
			uint32_t hash = 2166136261u;
			int c;
			for (c = 0; c < CASES; c++)
			{
				int w = 1 + (int)(rnd() % MAX_SIDE);
				int h = 1 + (int)(rnd() % MAX_SIDE);
				int y = 0, i;
				picture(w, h, argb);
				memset(trg, 0, sizeof(trg));
				/* (slices of rows, as the band pool shares them out) */
				while (y < h)
				{
					int rows = 1 + (int)(rnd() % (unsigned)h);
					xbrz_scale(factor, src, trg, w, h, argb != 0, y, y + rows);
					y += rows;
				}
				for (i = 0; i < w * h * factor * factor; i++)
				{
					hash ^= trg[i];
					hash *= 16777619u;
				}
			}
			printf("%s x%d %08x\n", argb ? "argb" : "rgb", factor, (unsigned)hash);
		}
	return 0;
}
