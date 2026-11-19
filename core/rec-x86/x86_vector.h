/* The SH4's two vector instructions as the 32-bit x86 recompiler emits
 * them: FIPR and FTRV, worked out in doubles exactly as the x86-64
 * recompiler does (rec-x64/x64_vector.h says why that gives the reference
 * implementations' results to the last bit), with what this processor has:
 * eax and ecx point at the operands, and only xmm0 to xmm3 are used - the
 * others are the block's. C89. */
#ifndef X86_VECTOR_H
#define X86_VECTOR_H
#include "jit/x64_emit.h"

/* eax, ecx: the two vectors. The product goes to @rd, as a float. */
static INLINE void x86_emit_fipr(x86_emit *e, int rd)
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

/* eax: the vector. ecx: the matrix, sixteen floats by columns. The four
 * results are left in xmm0. */
static INLINE void x86_emit_ftrv(x86_emit *e)
{
	int col;
	for (col = 0; col < 4; col++)
	{
		/* this column's element of the vector, twice */
		x64_cvtss2sd_m(e, 2, x86_m(X86_EAX, col * 4));
		x64_unpcklpd(e, 2, 2);
		/* times rows 0 and 1, then rows 2 and 3, into the two sums */
		x64_cvtps2pd_m(e, 3, x86_m(X86_ECX, col * 16));
		x64_mulpd(e, 3, 2);
		if (col == 0)
			x64_movapd(e, 0, 3);
		else
			x64_addpd(e, 0, 3);
		x64_cvtps2pd_m(e, 3, x86_m(X86_ECX, col * 16 + 8));
		x64_mulpd(e, 3, 2);
		if (col == 0)
			x64_movapd(e, 1, 3);
		else
			x64_addpd(e, 1, 3);
	}
	x64_cvtpd2ps(e, 0, 0);
	x64_cvtpd2ps(e, 1, 1);
	x64_movlhps(e, 0, 1);
}

#endif
