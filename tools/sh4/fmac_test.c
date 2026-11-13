/* FMAC's sum (core/hw/sh4/sh4_fmac.h) against the C library's fmaf(), both
 * ways the SH4 rounds (to nearest, towards zero), on random operands and on
 * ones made to land between two floats: a * b + c rounded once.
 *
 *   sh tools/sh4/fmac_test.sh
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fenv.h>

#include "types.h"
#include "hw/sh4/sh4_fmac.h"

static u64 state = 88172645463325252ull;
static u32 next(void)
{
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return (u32)state;
}

static float f_of(u32 u) { float f; memcpy(&f, &u, 4); return f; }
static u32 u_of(float f) { u32 u; memcpy(&u, &f, 4); return u; }

int main(void)
{
	long bad = 0, n = 0, i;
	int rz;

	for (rz = 0; rz < 2; rz++)
	{
		fesetround(rz ? FE_TOWARDZERO : FE_TONEAREST);
		for (i = 0; i < 20000000; i++)
		{
			u32 ua = next(), ub = next(), uc = next();
			float a, b, c, want, got;
			if (i % 3 == 0)
			{
				/* exponents that meet: cancellation, and sums near a tie */
				u32 ea = next() % 40 + 107, eb = next() % 40 + 107;
				ua = (ua & 0x807fffff) | (ea << 23);
				ub = (ub & 0x807fffff) | (eb << 23);
				uc = (uc & 0x807fffff) | ((ea + eb - 127 + next() % 30 - 15) << 23);
			}
			if (i % 7 == 0)
				ub &= 0xffff0000;	/* a short mantissa: products that end in a half */
			if (i % 11 == 0)
				uc = next() & 0x807fffff;	/* a denormal */
			a = f_of(ua);
			b = f_of(ub);
			c = f_of(uc);
			want = fmaf(a, b, c);
			got = sh4_fmac(a, b, c, rz != 0);
			n++;
			if (u_of(want) != u_of(got) && !(want != want && got != got))
			{
				if (bad++ < 5)
					fprintf(stderr, "FAIL (%s) %08x * %08x + %08x: fmaf %08x, sh4_fmac %08x\n",
							rz ? "towards zero" : "to nearest", ua, ub, uc, u_of(want), u_of(got));
			}
		}
	}
	fesetround(FE_TONEAREST);
	if (bad)
	{
		fprintf(stderr, "fmac_test: %ld of %ld differ\n", bad, n);
		return 1;
	}
	printf("fmac_test: %ld sums, each fmaf()'s\n", n);
	return 0;
}
