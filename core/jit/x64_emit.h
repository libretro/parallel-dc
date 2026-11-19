/* The rest of the x86-64 instructions the SH4's recompiler makes
 * (rec-x64/rec_x64.c), on top of x86_emit.h: 64-bit, 16-bit and 8-bit
 * forms, SSE, and the few AVX ones, each encoded as Xbyak encoded it (the
 * mandatory prefix, then REX, then the opcode; 2-byte VEX where it will
 * do). What takes no REX - the first eight registers, 32 bits and less -
 * is 32-bit x86's as well (rec-x86/rec_x86.c). C89. */
#ifndef X64_EMIT_H
#define X64_EMIT_H

#include "x86_emit.h"

/* An instruction: a prefix (66, F2, F3; 0 for none), REX.W, its opcode
 * bytes, @reg in ModRM's reg field, and a register (@rm) or a memory
 * operand (@m). @reg8 / @rm8: byte registers (spl..dil take a REX).
 * @imm_after: the bytes of immediate after it. */
static INLINE void x64_op(x86_emit *e, u32 pref, bool w, const u8 *ops, int nops, int reg, int rm,
      const x86_mem *m, int imm_after, bool reg8, bool rm8)
{
   int i;
   if (pref != 0)
      x86_db(e, pref);
   x86_rex8(e, w, reg, rm, m, reg8, rm8);
   for (i = 0; i < nops; i++)
      x86_db(e, ops[i]);
   if (m != NULL)
      x86_modrm_mi(e, reg, m, imm_after);
   else
      x86_modrm_r(e, reg, rm);
}

/* one opcode byte, or 0F and one (@op2 >= 0) */
static INLINE void x64_op1(x86_emit *e, u32 pref, bool w, u32 op1, int op2, int reg, int rm, const x86_mem *m,
      int imm_after, bool reg8, bool rm8)
{
   u8 ops[2];
   int n = 0;
   ops[n++] = (u8)op1;
   if (op2 >= 0)
      ops[n++] = (u8)op2;
   x64_op(e, pref, w, ops, n, reg, rm, m, imm_after, reg8, rm8);
}

/* ---- general registers ---- */

/* mov r64, r64 / r/m64 */
static INLINE void x64_mov_rr64(x86_emit *e, int dst, int src)
{
   x64_op1(e, 0, true, 0x89, -1, src, dst, NULL, 0, false, false);
}

static INLINE void x64_load64(x86_emit *e, int dst, x86_mem m)
{
   x64_op1(e, 0, true, 0x8B, -1, dst, 0, &m, 0, false, false);
}

static INLINE void x64_store64(x86_emit *e, x86_mem m, int src)
{
   x64_op1(e, 0, true, 0x89, -1, src, 0, &m, 0, false, false);
}

/* mov word [m], r16 / imm16; mov byte [m], imm8 */
static INLINE void x64_store16(x86_emit *e, x86_mem m, int src)
{
   x64_op1(e, 0x66, false, 0x89, -1, src, 0, &m, 0, false, false);
}

static INLINE void x64_store16_imm(x86_emit *e, x86_mem m, u32 imm)
{
   x64_op1(e, 0x66, false, 0xC7, -1, 0, 0, &m, 2, false, false);
   x86_db(e, imm);
   x86_db(e, imm >> 8);
}

static INLINE void x64_store8_imm(x86_emit *e, x86_mem m, u32 imm)
{
   x64_op1(e, 0, false, 0xC6, -1, 0, 0, &m, 1, false, false);
   x86_db(e, imm);
}

/* op r32/r64, r32/r64 (the reg-to-rm forms: @w for 64 bits) */
static INLINE void x64_alu_rr_w(x86_emit *e, bool w, int op, int dst, int src)
{
   x64_op1(e, 0, w, ((u32)op << 3) | 1, -1, src, dst, NULL, 0, false, false);
}

/* test r8, r8 */
static INLINE void x64_test_rr8(x86_emit *e, int a, int b)
{
   x64_op1(e, 0, false, 0x84, -1, b, a, NULL, 0, true, true);
}

/* test ah, imm8 (ah: no REX) */
static INLINE void x64_test_ah_imm(x86_emit *e, u32 imm)
{
   x86_db(e, 0xF6);
   x86_db(e, 0xC4);
   x86_db(e, imm);
}

/* test dword [m], imm32 */
static INLINE void x64_test_mi(x86_emit *e, x86_mem m, u32 imm)
{
   x64_op1(e, 0, false, 0xF7, -1, 0, 0, &m, 4, false, false);
   x86_dd(e, imm);
}

/* test r64, r64 */
static INLINE void x64_test_rr64(x86_emit *e, int a, int b)
{
   x64_op1(e, 0, true, 0x85, -1, b, a, NULL, 0, false, false);
}

/* shift / rotate r32 or r64 by an immediate (by 1: D1) */
static INLINE void x64_shift_ri_w(x86_emit *e, bool w, int op, int dst, u32 count)
{
   x86_rex(e, w, 0, dst, NULL);
   if (count == 1)
   {
      x86_db(e, 0xD1);
      x86_modrm_r(e, op, dst);
   }
   else
   {
      x86_db(e, 0xC1);
      x86_modrm_r(e, op, dst);
      x86_db(e, count);
   }
}

/* rotate r16 by an immediate */
static INLINE void x64_shift_ri16(x86_emit *e, int op, int dst, u32 count)
{
   x86_db(e, 0x66);
   x64_shift_ri_w(e, false, op, dst, count);
}

/* neg / not / mul r32 or r64 (F7 /3, /2, /4) */
static INLINE void x64_f7_w(x86_emit *e, bool w, int digit, int dst)
{
   x64_op1(e, 0, w, 0xF7, -1, digit, dst, NULL, 0, false, false);
}

/* inc / dec dword [m] */
static INLINE void x64_inc_m(x86_emit *e, x86_mem m)
{
   x64_op1(e, 0, false, 0xFF, -1, 0, 0, &m, 0, false, false);
}

static INLINE void x64_dec_m(x86_emit *e, x86_mem m)
{
   x64_op1(e, 0, false, 0xFF, -1, 1, 0, &m, 0, false, false);
}

/* movsx / movzx r32 (r64 with @w), r8 or r16 */
static INLINE void x64_movx_rr(x86_emit *e, bool w, u32 op2, int dst, int src, bool src8)
{
   x64_op1(e, 0, w, 0x0F, (int)op2, dst, src, NULL, 0, false, src8);
}
#define x64_movsx8_rr(e, dst, src)  x64_movx_rr(e, false, 0xBE, dst, src, true)
#define x64_movsx16_rr(e, dst, src) x64_movx_rr(e, false, 0xBF, dst, src, false)
#define x64_movzx8_rr(e, dst, src)  x64_movx_rr(e, false, 0xB6, dst, src, true)
#define x64_movzx16_rr(e, dst, src) x64_movx_rr(e, false, 0xB7, dst, src, false)

/* movsx r32, byte [m] */
static INLINE void x64_movsx8_m(x86_emit *e, int dst, x86_mem m)
{
   x64_op1(e, 0, false, 0x0F, 0xBE, dst, 0, &m, 0, false, false);
}

/* lea r32 / r64, [m] */
static INLINE void x64_lea(x86_emit *e, bool w, int dst, x86_mem m)
{
   x64_op1(e, 0, w, 0x8D, -1, dst, 0, &m, 0, false, false);
}

static INLINE void x64_lahf(x86_emit *e)
{
   x86_db(e, 0x9F);
}

/* call / jmp r64 */
static INLINE void x64_call_r(x86_emit *e, int r)
{
   x64_op1(e, 0, false, 0xFF, -1, 2, r, NULL, 0, false, false);
}

static INLINE void x64_jmp_r(x86_emit *e, int r)
{
   x64_op1(e, 0, false, 0xFF, -1, 4, r, NULL, 0, false, false);
}

/* ---- SSE ---- */

/* @pref 0F @op, @reg in the reg field */
static INLINE void x64_sse_rr(x86_emit *e, u32 pref, bool w, u32 op, int reg, int rm)
{
   x64_op1(e, pref, w, 0x0F, (int)op, reg, rm, NULL, 0, false, false);
}

static INLINE void x64_sse_rm(x86_emit *e, u32 pref, bool w, u32 op, int reg, x86_mem m, int imm_after)
{
   x64_op1(e, pref, w, 0x0F, (int)op, reg, 0, &m, imm_after, false, false);
}

/* movss xmm, xmm / xmm, [m] / [m], xmm */
#define x64_movss_rr(e, dst, src)  x64_sse_rr(e, 0xF3, false, 0x10, dst, src)
#define x64_movss_load(e, dst, m)  x64_sse_rm(e, 0xF3, false, 0x10, dst, m, 0)
#define x64_movss_store(e, m, src) x64_sse_rm(e, 0xF3, false, 0x11, src, m, 0)
/* movd xmm, r32 / r32, xmm / xmm, [m] / [m], xmm */
#define x64_movd_xr(e, x, r)       x64_sse_rr(e, 0x66, false, 0x6E, x, r)
#define x64_movd_rx(e, r, x)       x64_sse_rr(e, 0x66, false, 0x7E, x, r)
#define x64_movd_xm(e, x, m)       x64_sse_rm(e, 0x66, false, 0x6E, x, m, 0)
#define x64_movd_mx(e, m, x)       x64_sse_rm(e, 0x66, false, 0x7E, x, m, 0)
/* movq xmm, r64 / r64, xmm */
#define x64_movq_xr(e, x, r)       x64_sse_rr(e, 0x66, true, 0x6E, x, r)
#define x64_movq_rx(e, r, x)       x64_sse_rr(e, 0x66, true, 0x7E, x, r)
/* movaps xmm, xmm / xmm, [m] / [m], xmm; movups [m], xmm */
#define x64_movaps_rr(e, dst, src) x64_sse_rr(e, 0, false, 0x28, dst, src)
#define x64_movaps_load(e, dst, m) x64_sse_rm(e, 0, false, 0x28, dst, m, 0)
#define x64_movaps_store(e, m, src) x64_sse_rm(e, 0, false, 0x29, src, m, 0)
#define x64_movups_store(e, m, src) x64_sse_rm(e, 0, false, 0x11, src, m, 0)
/* the single-float arithmetic: addss, mulss, subss, divss, sqrtss */
#define X64_ADDSS 0x58
#define X64_MULSS 0x59
#define X64_SUBSS 0x5C
#define X64_DIVSS 0x5E
#define X64_SQRTSS 0x51
#define x64_ss_rr(e, op, dst, src) x64_sse_rr(e, 0xF3, false, op, dst, src)
/* ucomiss xmm, xmm */
#define x64_ucomiss(e, a, b)       x64_sse_rr(e, 0, false, 0x2E, a, b)
/* pxor, andps, andnps, orps, unpcklps */
#define x64_pxor(e, dst, src)      x64_sse_rr(e, 0x66, false, 0xEF, dst, src)
#define x64_andps(e, dst, src)     x64_sse_rr(e, 0, false, 0x54, dst, src)
#define x64_andnps(e, dst, src)    x64_sse_rr(e, 0, false, 0x55, dst, src)
#define x64_orps(e, dst, src)      x64_sse_rr(e, 0, false, 0x56, dst, src)
#define x64_unpcklps(e, dst, src)  x64_sse_rr(e, 0, false, 0x14, dst, src)
/* movmskps r32, xmm */
#define x64_movmskps(e, r, x)      x64_sse_rr(e, 0, false, 0x50, r, x)
/* cvttss2si r32, xmm; cvtsi2ss xmm, r32 */
#define x64_cvttss2si(e, r, x)     x64_sse_rr(e, 0xF3, false, 0x2C, r, x)
#define x64_cvtsi2ss(e, x, r)      x64_sse_rr(e, 0xF3, false, 0x2A, x, r)
/* the doubles of FIPR and FTRV */
#define x64_cvtss2sd_m(e, x, m)    x64_sse_rm(e, 0xF3, false, 0x5A, x, m, 0)
#define x64_cvtsd2ss(e, dst, src)  x64_sse_rr(e, 0xF2, false, 0x5A, dst, src)
#define x64_mulsd(e, dst, src)     x64_sse_rr(e, 0xF2, false, 0x59, dst, src)
#define x64_addsd(e, dst, src)     x64_sse_rr(e, 0xF2, false, 0x58, dst, src)
#define x64_unpcklpd(e, dst, src)  x64_sse_rr(e, 0x66, false, 0x14, dst, src)
#define x64_cvtps2pd_m(e, x, m)    x64_sse_rm(e, 0, false, 0x5A, x, m, 0)
#define x64_mulpd(e, dst, src)     x64_sse_rr(e, 0x66, false, 0x59, dst, src)
#define x64_addpd(e, dst, src)     x64_sse_rr(e, 0x66, false, 0x58, dst, src)
#define x64_movapd(e, dst, src)    x64_sse_rr(e, 0x66, false, 0x28, dst, src)
#define x64_cvtpd2ps(e, dst, src)  x64_sse_rr(e, 0x66, false, 0x5A, dst, src)
#define x64_movlhps(e, dst, src)   x64_sse_rr(e, 0, false, 0x16, dst, src)

/* movq xmm, [m] / [m], xmm (the low 64 bits, the rest cleared on a load) */
#define x64_movq_load(e, x, m)     x64_sse_rm(e, 0xF3, false, 0x7E, x, m, 0)
#define x64_movq_store(e, m, x)    x64_sse_rm(e, 0x66, false, 0xD6, x, m, 0)
/* punpckldq xmm, xmm */
#define x64_punpckldq(e, dst, src) x64_sse_rr(e, 0x66, false, 0x62, dst, src)

/* fstp dword [m] (32-bit x86: where a C function leaves a float) */
static INLINE void x64_fstp_m32(x86_emit *e, x86_mem m)
{
   x64_op1(e, 0, false, 0xD9, -1, 3, 0, &m, 0, false, false);
}

/* No-operations to the next multiple of @align of the address, as Xbyak's
 * align() made them */
static INLINE void x64_align(x86_emit *e, u32 align)
{
   static const u8 nops[9][9] = {
      { 0x90 }, { 0x66, 0x90 }, { 0x0F, 0x1F, 0x00 }, { 0x0F, 0x1F, 0x40, 0x00 },
      { 0x0F, 0x1F, 0x44, 0x00, 0x00 }, { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 },
      { 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 }, { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
      { 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 }
   };
   u32 remain = (u32)((uintptr_t)e->p % align), size, i;
   if (remain == 0)
      return;
   size = align - remain;
   while (size > 0)
   {
      const u32 len = size > 9 ? 9 : size;
      for (i = 0; i < len; i++)
         x86_db(e, nops[len - 1][i]);
      size -= len;
   }
}

/* shufps / pshufd / cmpps xmm, xmm, imm8 */
static INLINE void x64_sse_rri(x86_emit *e, u32 pref, u32 op, int dst, int src, u32 imm)
{
   x64_sse_rr(e, pref, false, op, dst, src);
   x86_db(e, imm);
}
#define x64_shufps(e, dst, src, imm) x64_sse_rri(e, 0, 0xC6, dst, src, imm)
#define x64_pshufd(e, dst, src, imm) x64_sse_rri(e, 0x66, 0x70, dst, src, imm)
#define x64_cmpunordps(e, dst, src)  x64_sse_rri(e, 0, 0xC2, dst, src, 3)

/* ---- AVX ---- */

/* vfmadd231ss xmm, xmm, xmm (3-byte VEX: 66, 0F38, W0) */
static INLINE void x64_vfmadd231ss(x86_emit *e, int dst, int src1, int src2)
{
   x86_db(e, 0xC4);
   x86_db(e, (dst >= 8 ? 0 : 0x80) | 0x40 | (src2 >= 8 ? 0 : 0x20) | 0x02);
   x86_db(e, (((u32)~src1 & 15) << 3) | 0x01);
   x86_db(e, 0xB9);
   x86_modrm_r(e, dst, src2);
}

/* vmovaps ymm, [r64 + disp] / [r64 + disp], ymm (2-byte VEX), the
 * registers and bases below 8 */
static INLINE void x64_vmovaps_ymm(x86_emit *e, bool store, int ymm, x86_mem m)
{
   x86_db(e, 0xC5);
   x86_db(e, 0x80 | 0x78 | 0x04);
   x86_db(e, store ? 0x29 : 0x28);
   x86_modrm_mi(e, ymm, &m, 0);
}

/* vmovaps zmm, [r64] / [r64], zmm (EVEX), the registers and bases below 8,
 * no displacement */
static INLINE void x64_vmovaps_zmm(x86_emit *e, bool store, int zmm, int base)
{
   x86_db(e, 0x62);
   x86_db(e, 0xF1);
   x86_db(e, 0x7C);
   x86_db(e, 0x48);
   x86_db(e, store ? 0x29 : 0x28);
   x86_db(e, ((u32)(zmm & 7) << 3) | (u32)(base & 7));
}

static INLINE void x64_vzeroupper(x86_emit *e)
{
   x86_db(e, 0xC5);
   x86_db(e, 0xF8);
   x86_db(e, 0x77);
}

#endif
