/* The SH4's two vector instructions as the x86-64 recompiler emits them:
 * FIPR, the inner product of two vectors of four floats, and FTRV, a 4x4
 * matrix times such a vector.
 *
 * Both work in doubles: a float times a float is exact in a double, so the
 * only rounding is in the sums and in the final conversion back, and the
 * sums are taken in the order the reference implementations in
 * hw/sh4/dyna/shil_canonical.h take them. The result is then theirs to the
 * last bit, which tools/sh4/run.sh checks by running this very code against
 * them. These are here, and not in rec_x64.c, so that it can.
 *
 * In: rax and rcx point at the operands. Uses xmm0 to xmm4, which the
 * recompiler does not hand out. C89. */
#ifndef X64_VECTOR_H
#define X64_VECTOR_H
#include "jit/x64_emit.h"

/* rax, rcx: the two vectors. The product goes to @rd, as a float. */
static INLINE void x64_emit_fipr(x86_emit *e, int rd)
{
	int i;
	x64_cvtss2sd_m(e, 0, x86_m(X86_EAX, 0));
	x64_cvtss2sd_m(e, 1, x86_m(X86_ECX, 0));
	x64_mulsd(e, 0, 1);
	for (i = 1; i < 4; i++)
	{
		x64_cvtss2sd_m(e, 1, x86_m(X86_EAX, i * 4));
		x64_cvtss2sd_m(e, 2, x86_m(X86_ECX, i * 4));
		x64_mulsd(e, 1, 2);
		x64_addsd(e, 0, 1);
	}
	x64_cvtsd2ss(e, rd, 0);
}

/* rax: the vector. rcx: the matrix, sixteen floats by columns. The four
 * results are left in xmm0. */
static INLINE void x64_emit_ftrv(x86_emit *e)
{
	int col;
	for (col = 0; col < 4; col++)
	{
		/* this column times its element of the vector, two rows to a register */
		x64_cvtss2sd_m(e, 4, x86_m(X86_EAX, col * 4));
		x64_unpcklpd(e, 4, 4);
		x64_cvtps2pd_m(e, 2, x86_m(X86_ECX, col * 16));		/* rows 0 and 1 */
		x64_cvtps2pd_m(e, 3, x86_m(X86_ECX, col * 16 + 8));	/* rows 2 and 3 */
		x64_mulpd(e, 2, 4);
		x64_mulpd(e, 3, 4);
		if (col == 0)
		{
			x64_movapd(e, 0, 2);
			x64_movapd(e, 1, 3);
		}
		else
		{
			x64_addpd(e, 0, 2);
			x64_addpd(e, 1, 3);
		}
	}
	x64_cvtpd2ps(e, 0, 0);
	x64_cvtpd2ps(e, 1, 1);
	x64_movlhps(e, 0, 1);
}

#endif
