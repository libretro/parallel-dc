/* FMAC's sum, a * b + c rounded once, as fmaf() has it: in C89, which has
 * no fmaf(), and the same on every host.
 *
 * The product of two floats is a double exactly. Rounding towards zero,
 * the double nearest the sum the same way and then the float are the float
 * rounded once (truncating twice is truncating once). Rounding to nearest,
 * the double sum's error is had exactly (TwoSum), and an inexact sum is
 * made odd in its last bit towards it, so that rounding to the float does
 * not land on a tie the double rounding made (round to odd). @rz: the
 * host is rounding towards zero, as the SH4 is set to (FPSCR.RM). */
#pragma once
#include <string.h>
#include "types.h"

static TYPES_INLINE float sh4_fmac(float a, float b, float c, bool rz)
{
	double p = (double)a * (double)b;
	double s = p + (double)c;
	if (!rz && s - s == 0)
	{
		double bb = s - p;
		double e = (p - (s - bb)) + ((double)c - bb);
		if (e != 0)
		{
			u64 bits;
			memcpy(&bits, &s, sizeof(bits));
			if ((bits & 1) == 0)
			{
				/* (the magnitude up where the sum is further out than s) */
				if ((e > 0) == (s > 0))
					bits++;
				else
					bits--;
				memcpy(&s, &bits, sizeof(s));
			}
		}
	}
	return (float)s;
}
