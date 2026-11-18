/* The AArch64 instructions the sound chip's recompilers make (dsp_arm64.c,
 * arm7_rec_arm64.c), and the macros they use as VIXL's MacroAssembler
 * expanded them: a constant moved in as few instructions as it takes, an
 * immediate an instruction cannot have put in a scratch register (w16)
 * first, shifted where that saves an instruction, and a literal pool at
 * the end with its marker. C89. */
#ifndef A64_EMIT_H
#define A64_EMIT_H

#include <string.h>
#include "types.h"

#define A64_ZR 31	/* the zero register (and sp, where an instruction takes it) */
#define A64_SP 31
#define A64_IP0 16	/* the scratch registers */
#define A64_IP1 17

/* Conditions */
enum { A64_EQ = 0, A64_NE = 1, A64_HS = 2, A64_LO = 3, A64_LT = 11, A64_GT = 12, A64_LE = 13 };

typedef struct a64_emit
{
   u32 *start;
   u32 *p;
   u32 *end;
   /* Something did not fit: what was made is not to be run */
   bool full;
   /* A 64-bit literal (one at most) and the loads of it, made at the end */
   u64 literal;
   bool has_literal;
   int literal_loads[160];
   int literal_load_count;
} a64_emit;

static INLINE void a64_init(a64_emit *e, void *code, size_t room)
{
   e->start = (u32 *)code;
   e->p     = e->start;
   e->end   = e->start + room / 4;
   e->full  = false;
   e->has_literal = false;
   e->literal_load_count = 0;
}

static INLINE void a64_dw(a64_emit *e, u32 w)
{
   if (e->p >= e->end)
   {
      e->full = true;
      return;
   }
   *e->p++ = w;
}

static INLINE int a64_clz64(u64 v)
{
   int n = 0;
   if (v == 0)
      return 64;
   while (!(v & ((u64)1 << 63)))
   {
      v <<= 1;
      n++;
   }
   return n;
}

static INLINE int a64_ctz64(u64 v, int width)
{
   int n = 0;
   if (v == 0)
      return width;
   while (!(v & 1))
   {
      v >>= 1;
      n++;
   }
   return n;
}

/* VIXL's: whether @value is a logical immediate of @width bits, and its
 * fields */
static INLINE bool a64_logical_imm(u64 value, unsigned width, unsigned *n, unsigned *imm_s, unsigned *imm_r)
{
   static const u64 multipliers[] = {
      0x0000000000000001ull,
      0x0000000100000001ull,
      0x0001000100010001ull,
      0x0101010101010101ull,
      0x1111111111111111ull,
      0x5555555555555555ull,
   };
   bool negate = false;
   u64 a, value_plus_a, b, value_plus_a_minus_b, c, mask, candidate;
   int d, clz_a, out_n, clz_b, s, r;

   if (value & 1)
   {
      negate = true;
      value = ~value;
   }
   if (width == 32)
   {
      value <<= 32;
      value |= value >> 32;
   }
   a = value & (0 - value);
   value_plus_a = value + a;
   b = value_plus_a & (0 - value_plus_a);
   value_plus_a_minus_b = value_plus_a - b;
   c = value_plus_a_minus_b & (0 - value_plus_a_minus_b);

   if (c != 0)
   {
      int clz_c;
      clz_a = a64_clz64(a);
      clz_c = a64_clz64(c);
      d = clz_a - clz_c;
      mask = (((u64)1 << d) - 1);
      out_n = 0;
   }
   else
   {
      if (a == 0)
         return false;
      clz_a = a64_clz64(a);
      d = 64;
      mask = ~(u64)0;
      out_n = 1;
   }
   if (d <= 0 || (d & (d - 1)) != 0)
      return false;
   if (((b - a) & ~mask) != 0)
      return false;
   candidate = (b - a) * multipliers[a64_clz64((u64)d) - 57];
   if (value != candidate)
      return false;
   clz_b = (b == 0) ? -1 : a64_clz64(b);
   s = clz_a - clz_b;
   if (negate)
   {
      s = d - s;
      r = (clz_b + 1) & (d - 1);
   }
   else
      r = (clz_a + 1) & (d - 1);
   *n = (unsigned)out_n;
   *imm_s = (unsigned)(((2 * -d) | (s - 1)) & 0x3f);
   *imm_r = (unsigned)r;
   return true;
}

/* movz / movn / movk (sf: 64-bit) */
static INLINE void a64_movw(a64_emit *e, bool sf, u32 opc, int rd, u32 imm16, int hw)
{
   a64_dw(e, (sf ? 0x80000000u : 0) | (opc << 29) | 0x12800000u | ((u32)hw << 21) | ((imm16 & 0xffff) << 5) | (u32)rd);
}

/* VIXL's movz / movn of a whole value: the halfword it is in */
static INLINE void a64_movwide(a64_emit *e, bool sf, u32 opc, int rd, u64 imm)
{
   int shift = 0;
   if ((imm & 0xffffffffffff0000ull) == 0)
      ;
   else if ((imm & 0xffffffff0000ffffull) == 0)
   {
      imm >>= 16;
      shift = 1;
   }
   else if ((imm & 0xffff0000ffffffffull) == 0)
   {
      imm >>= 32;
      shift = 2;
   }
   else if ((imm & 0x0000ffffffffffffull) == 0)
   {
      imm >>= 48;
      shift = 3;
   }
   a64_movw(e, sf, opc, rd, (u32)imm, shift);
}

static INLINE int a64_clear_halfwords(u64 imm, unsigned width)
{
   int count = 0;
   unsigned i;
   for (i = 0; i < width / 16; i++)
      if ((imm & ((u64)0xffff << (16 * i))) == 0)
         count++;
   return count;
}

/* Logical (immediate): and 0, orr 1, eor 2, ands 3 */
static INLINE void a64_logical_ri(a64_emit *e, bool sf, u32 opc, int rd, int rn, unsigned n, unsigned imm_s, unsigned imm_r)
{
   a64_dw(e, (sf ? 0x80000000u : 0) | (opc << 29) | 0x12000000u | (n << 22) | (imm_r << 16) | (imm_s << 10) | ((u32)rn << 5) | (u32)rd);
}

/* A constant in one instruction, if one does it */
static INLINE bool a64_mov_one(a64_emit *e, bool sf, int rd, u64 imm)
{
   unsigned width = sf ? 64 : 32;
   u64 m = sf ? ~(u64)0 : 0xffffffffull;
   unsigned n, imm_s, imm_r;

   if (a64_clear_halfwords(imm, width) >= (int)(width / 16 - 1))
   {
      if (e != NULL)
         a64_movwide(e, sf, 2, rd, imm);
      return true;
   }
   if (a64_clear_halfwords(~imm, width) >= (int)(width / 16 - 1))
   {
      if (e != NULL)
         a64_movwide(e, sf, 0, rd, ~imm & m);
      return true;
   }
   if (a64_logical_imm(imm, width, &n, &imm_s, &imm_r))
   {
      if (e != NULL)
         a64_logical_ri(e, sf, 1, rd, A64_ZR, n, imm_s, imm_r);
      return true;
   }
   return false;
}

/* Mov(rd, imm) */
static INLINE void a64_mov_imm(a64_emit *e, bool sf, int rd, u64 imm)
{
   unsigned width = sf ? 64 : 32;
   u64 ignored = 0;
   bool invert = false, first = true;
   unsigned i;

   if (!sf)
      imm &= 0xffffffffull;
   if (a64_mov_one(e, sf, rd, imm))
      return;
   if (a64_clear_halfwords(~imm, width) > a64_clear_halfwords(imm, width))
   {
      ignored = 0xffff;
      invert = true;
   }
   for (i = 0; i < width / 16; i++)
   {
      u64 imm16 = (imm >> (16 * i)) & 0xffff;
      if (imm16 != ignored)
      {
         if (first)
         {
            a64_movw(e, sf, invert ? 0 : 2, rd, invert ? (u32)(~imm16 & 0xffff) : (u32)imm16, (int)i);
            first = false;
         }
         else
            a64_movw(e, sf, 3, rd, (u32)imm16, (int)i);
      }
   }
}

/* Add/sub (shifted register): add 0, sub 2 (op<<30 | S<<29) */
enum { A64_LSL = 0, A64_LSR = 1, A64_ASR = 2, A64_ROR = 3 };
static INLINE void a64_addsub_rr(a64_emit *e, bool sf, bool sub, bool s, int rd, int rn, int rm, int shift, unsigned amount)
{
   a64_dw(e, (sf ? 0x80000000u : 0) | (sub ? 0x40000000u : 0) | (s ? 0x20000000u : 0) | 0x0B000000u
         | ((u32)shift << 22) | ((u32)rm << 16) | (amount << 10) | ((u32)rn << 5) | (u32)rd);
}

static INLINE void a64_addsub_ri(a64_emit *e, bool sf, bool sub, bool s, int rd, int rn, u32 imm12, bool lsl12)
{
   a64_dw(e, (sf ? 0x80000000u : 0) | (sub ? 0x40000000u : 0) | (s ? 0x20000000u : 0) | 0x11000000u
         | (lsl12 ? 0x400000u : 0) | ((imm12 & 0xfff) << 10) | ((u32)rn << 5) | (u32)rd);
}

static INLINE bool a64_is_addsub_imm(u64 imm)
{
   return imm < 0x1000 || ((imm >> 12) < 0x1000 && (imm & 0xfff) == 0);
}

/* VIXL's MoveImmediateForShiftedOp(): the constant in @temp, and the shift
 * the operation is to take it with */
static INLINE void a64_imm_for_shifted_op(a64_emit *e, bool sf, int temp, u64 imm, int *shift, unsigned *amount)
{
   unsigned width = sf ? 64 : 32;
   *shift = A64_LSL;
   *amount = 0;
   if (!sf)
      imm &= 0xffffffffull;
   if (a64_mov_one(e, sf, temp, imm))
      return;
   {
      int shift_low = a64_ctz64(imm, (int)width);
      u64 imm_low = (u64)((s64)imm >> shift_low);
      int shift_high = a64_clz64(imm) - (64 - (int)width);
      u64 imm_high = (imm << shift_high) | (((u64)1 << shift_high) - 1);
      if (!sf)
      {
         imm_low &= 0xffffffffull;
         imm_high &= 0xffffffffull;
      }
      if (a64_mov_one(NULL, sf, temp, imm_low))
      {
         a64_mov_one(e, sf, temp, imm_low);
         *shift = A64_LSL;
         *amount = (unsigned)shift_low;
      }
      else if (a64_mov_one(NULL, sf, temp, imm_high))
      {
         a64_mov_one(e, sf, temp, imm_high);
         *shift = A64_LSR;
         *amount = (unsigned)shift_high;
      }
      else
         a64_mov_imm(e, sf, temp, imm);
   }
}

/* Add / Sub / Adds / Subs(rd, rn, imm) */
static INLINE void a64_addsub_imm(a64_emit *e, bool sf, bool sub, bool s, int rd, int rn, s64 imm)
{
   if (imm < 0 && a64_is_addsub_imm((u64)-imm))
   {
      sub = !sub;
      imm = -imm;
   }
   if (a64_is_addsub_imm((u64)imm))
   {
      if ((u64)imm < 0x1000)
         a64_addsub_ri(e, sf, sub, s, rd, rn, (u32)imm, false);
      else
         a64_addsub_ri(e, sf, sub, s, rd, rn, (u32)(imm >> 12), true);
   }
   else
   {
      /* (the lowest of rd, where it is not rn, and the scratch registers) */
      int temp = rd != rn && rd < A64_IP0 ? rd : rn != A64_IP0 ? A64_IP0 : A64_IP0 + 1;
      int shift;
      unsigned amount;
      a64_imm_for_shifted_op(e, sf, temp, (u64)imm, &shift, &amount);
      a64_addsub_rr(e, sf, sub, s, rd, rn, temp, shift, amount);
   }
}

/* And(rd, rn, imm) */
static INLINE void a64_and_imm(a64_emit *e, bool sf, int rd, int rn, u64 imm)
{
   unsigned n, imm_s, imm_r;
   if (!sf)
      imm &= 0xffffffffull;
   if (imm == 0)
   {
      a64_mov_imm(e, sf, rd, 0);
      return;
   }
   if (imm == (sf ? ~(u64)0 : 0xffffffffull))
   {
      if (rd != rn || !sf)
         a64_dw(e, (sf ? 0x80000000u : 0) | 0x2A000000u | ((u32)rn << 16) | (A64_ZR << 5) | (u32)rd);
      return;
   }
   if (a64_logical_imm(imm, sf ? 64 : 32, &n, &imm_s, &imm_r))
      a64_logical_ri(e, sf, 0, rd, rn, n, imm_s, imm_r);
   else
   {
      int shift;
      unsigned amount;
      a64_imm_for_shifted_op(e, sf, A64_IP0, imm, &shift, &amount);
      /* and (shifted register) */
      a64_dw(e, (sf ? 0x80000000u : 0) | 0x0A000000u | ((u32)shift << 22) | (A64_IP0 << 16)
            | (amount << 10) | ((u32)rn << 5) | (u32)rd);
   }
}

/* Mov(wd, wm): orr wd, wzr, wm */
static INLINE void a64_mov_rr(a64_emit *e, bool sf, int rd, int rm)
{
   a64_dw(e, (sf ? 0x80000000u : 0) | 0x2A000000u | ((u32)rm << 16) | (A64_ZR << 5) | (u32)rd);
}

/* Bitfield moves: sbfm 0, bfm 1, ubfm 2 */
static INLINE void a64_bfm(a64_emit *e, bool sf, u32 opc, int rd, int rn, unsigned immr, unsigned imms)
{
   a64_dw(e, (sf ? 0x80400000u : 0) | (opc << 29) | 0x13000000u | (immr << 16) | (imms << 10) | ((u32)rn << 5) | (u32)rd);
}

static INLINE void a64_lsl(a64_emit *e, int rd, int rn, unsigned shift)
{
   a64_bfm(e, false, 2, rd, rn, (32 - shift) & 31, 31 - shift);
}

static INLINE void a64_asr(a64_emit *e, bool sf, int rd, int rn, unsigned shift)
{
   a64_bfm(e, sf, 0, rd, rn, shift, sf ? 63 : 31);
}

static INLINE void a64_sbfx(a64_emit *e, int rd, int rn, unsigned lsb, unsigned width)
{
   a64_bfm(e, false, 0, rd, rn, lsb, lsb + width - 1);
}

static INLINE void a64_ubfx(a64_emit *e, int rd, int rn, unsigned lsb, unsigned width)
{
   a64_bfm(e, false, 2, rd, rn, lsb, lsb + width - 1);
}

static INLINE void a64_bfc(a64_emit *e, int rd, unsigned lsb, unsigned width)
{
   a64_bfm(e, false, 1, rd, A64_ZR, (32 - lsb) & 31, width - 1);
}

/* Sxtw(xd, wn) */
static INLINE void a64_sxtw(a64_emit *e, int rd, int rn)
{
   a64_bfm(e, true, 0, rd, rn, 0, 31);
}

/* Neg(wd, wm): sub wd, wzr, wm */
static INLINE void a64_neg(a64_emit *e, int rd, int rm)
{
   a64_addsub_rr(e, false, true, false, rd, A64_ZR, rm, A64_LSL, 0);
}

/* Cmp(wn, wm) */
static INLINE void a64_cmp_rr(a64_emit *e, int rn, int rm)
{
   a64_addsub_rr(e, false, true, true, A64_ZR, rn, rm, A64_LSL, 0);
}

static INLINE void a64_csel(a64_emit *e, int rd, int rn, int rm, u32 cond)
{
   a64_dw(e, 0x1A800000u | ((u32)rm << 16) | (cond << 12) | ((u32)rn << 5) | (u32)rd);
}

/* Mul(xd, xn, xm) */
static INLINE void a64_mul64(a64_emit *e, int rd, int rn, int rm)
{
   a64_dw(e, 0x9B000000u | ((u32)rm << 16) | (A64_ZR << 10) | ((u32)rn << 5) | (u32)rd);
}

/* ldr / str wt, [xn, #imm] (scaled unsigned offset) */
static INLINE void a64_ldst_imm(a64_emit *e, bool load, int rt, int rn, u32 offset)
{
   a64_dw(e, (load ? 0xB9400000u : 0xB9000000u) | ((offset / 4) << 10) | ((u32)rn << 5) | (u32)rt);
}

/* ldr / str wt, [xn, xm, lsl #2 (or not)]; ldrh / strh wt, [xn, xm] */
static INLINE void a64_ldst_reg(a64_emit *e, bool load, bool half, int rt, int rn, int rm, bool shifted)
{
   a64_dw(e, (half ? 0x78200800u : 0xB8200800u) | (load ? 0x400000u : 0) | ((u32)rm << 16) | (3u << 13)
         | (shifted ? 0x1000u : 0) | ((u32)rn << 5) | (u32)rt);
}

/* stp / ldp xt1, xt2, [sp, #imm] (pre-index, signed offset, post-index) */
enum { A64_POST = 1, A64_OFFSET = 2, A64_PRE = 3 };
static INLINE void a64_ldstp(a64_emit *e, bool load, int mode, int rt1, int rt2, int rn, int offset)
{
   a64_dw(e, 0xA8000000u | ((u32)mode << 23) | (load ? 0x400000u : 0) | (((u32)(offset / 8) & 0x7f) << 15)
         | ((u32)rt2 << 10) | ((u32)rn << 5) | (u32)rt1);
}

/* bl to an address: false where it is out of reach */
static INLINE bool a64_bl(a64_emit *e, uintptr_t target)
{
   s64 rel = (s64)((intptr_t)target - (intptr_t)e->p);
   if (rel < -128 * 1024 * 1024 || rel >= 128 * 1024 * 1024)
      return false;
   a64_dw(e, 0x94000000u | ((u32)(rel / 4) & 0x3ffffff));
   return true;
}

static INLINE void a64_blr(a64_emit *e, int rn)
{
   a64_dw(e, 0xD63F0000u | ((u32)rn << 5));
}

static INLINE void a64_ret(a64_emit *e)
{
   a64_dw(e, 0xD65F03C0u);
}

/* An operand of VIXL's: a constant, or a register shifted (by 0: plain) */
typedef struct a64_operand
{
   bool is_imm;
   u64 imm;
   int reg;
   int shift;
   unsigned amount;
} a64_operand;

static INLINE a64_operand a64_opnd_imm(u64 imm)
{
   a64_operand o;
   o.is_imm = true;
   o.imm    = imm;
   o.reg    = A64_ZR;
   o.shift  = A64_LSL;
   o.amount = 0;
   return o;
}

static INLINE a64_operand a64_opnd_reg(int reg, int shift, unsigned amount)
{
   a64_operand o;
   o.is_imm = false;
   o.imm    = 0;
   o.reg    = reg;
   o.shift  = shift;
   o.amount = amount;
   return o;
}

/* Logical (shifted register): and 0, orr 1, eor 2, ands 3; @n inverts rm
 * (bic, orn, eon, bics) */
static INLINE void a64_logical_rr(a64_emit *e, bool sf, u32 opc, bool n, int rd, int rn, int rm, int shift, unsigned amount)
{
   a64_dw(e, (sf ? 0x80000000u : 0) | (opc << 29) | 0x0A000000u | ((u32)shift << 22) | (n ? 0x200000u : 0)
         | ((u32)rm << 16) | (amount << 10) | ((u32)rn << 5) | (u32)rd);
}

/* A shift by a constant (32-bit): lsl, lsr, asr (ubfm, sbfm), ror (extr) */
static INLINE void a64_shift_imm(a64_emit *e, int shift, int rd, int rn, unsigned amount)
{
   switch (shift)
   {
   case A64_LSL:
      a64_lsl(e, rd, rn, amount);
      break;
   case A64_LSR:
      a64_bfm(e, false, 2, rd, rn, amount, 31);
      break;
   case A64_ASR:
      a64_bfm(e, false, 0, rd, rn, amount, 31);
      break;
   default:
      a64_dw(e, 0x13800000u | ((u32)rn << 16) | (amount << 10) | ((u32)rn << 5) | (u32)rd);
      break;
   }
}

/* A shift by a register (32-bit): lslv, lsrv, asrv, rorv */
static INLINE void a64_shift_reg(a64_emit *e, int shift, int rd, int rn, int rm)
{
   a64_dw(e, 0x1AC02000u | ((u32)shift << 10) | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd);
}

/* Mov(rd, operand): a 32-bit register is moved even to itself */
static INLINE void a64_mov_op(a64_emit *e, bool sf, int rd, a64_operand op)
{
   if (op.is_imm)
      a64_mov_imm(e, sf, rd, op.imm);
   else if (op.amount != 0)
      a64_shift_imm(e, op.shift, rd, op.reg, op.amount);
   else if (rd != op.reg || !sf)
      a64_mov_rr(e, sf, rd, op.reg);
}

/* Mvn(rd, register operand): orn rd, zr, rm */
static INLINE void a64_mvn_op(a64_emit *e, bool sf, int rd, a64_operand op)
{
   if (op.is_imm)
      a64_mov_imm(e, sf, rd, ~op.imm);
   else
      a64_logical_rr(e, sf, 1, true, rd, A64_ZR, op.reg, op.shift, op.amount);
}

/* And / Orr / Eor / Ands (@opc) and their inverted forms (@n): VIXL's
 * LogicalMacro() */
static INLINE void a64_logical(a64_emit *e, bool sf, u32 opc, bool n, int rd, int rn, a64_operand op)
{
   u64 imm;
   unsigned ln, imm_s, imm_r;
   if (!op.is_imm)
   {
      a64_logical_rr(e, sf, opc, n, rd, rn, op.reg, op.shift, op.amount);
      return;
   }
   imm = n ? ~op.imm : op.imm;
   if (!sf)
      imm &= 0xffffffffull;
   if (opc != 3)
   {
      if (imm == 0)
      {
         if (opc == 0)
            a64_mov_imm(e, sf, rd, 0);
         else
            a64_mov_op(e, sf, rd, a64_opnd_reg(rn, A64_LSL, 0));
         return;
      }
      if (imm == (sf ? ~(u64)0 : 0xffffffffull))
      {
         if (opc == 0)
            a64_mov_op(e, sf, rd, a64_opnd_reg(rn, A64_LSL, 0));
         else if (opc == 1)
            a64_mov_imm(e, sf, rd, imm);
         else
            a64_mvn_op(e, sf, rd, a64_opnd_reg(rn, A64_LSL, 0));
         return;
      }
   }
   if (a64_logical_imm(imm, sf ? 64 : 32, &ln, &imm_s, &imm_r))
      a64_logical_ri(e, sf, opc, rd, rn, ln, imm_s, imm_r);
   else
   {
      int shift;
      unsigned amount;
      a64_imm_for_shifted_op(e, sf, A64_IP0, imm, &shift, &amount);
      a64_logical_rr(e, sf, opc, false, rd, rn, A64_IP0, shift, amount);
   }
}

/* Add / Sub / Adds / Subs(rd, rn, operand); Cmp and Cmn are Subs and Adds to zr */
static INLINE void a64_addsub(a64_emit *e, bool sf, bool sub, bool s, int rd, int rn, a64_operand op)
{
   if (op.is_imm)
      a64_addsub_imm(e, sf, sub, s, rd, rn, (s64)op.imm);
   else if (op.shift == A64_ROR)
   {
      a64_mov_op(e, sf, A64_IP0, op);
      a64_addsub_rr(e, sf, sub, s, rd, rn, A64_IP0, A64_LSL, 0);
   }
   else
      a64_addsub_rr(e, sf, sub, s, rd, rn, op.reg, op.shift, op.amount);
}

/* Adc / Sbc / Adcs / Sbcs(rd, rn, operand) (32-bit) */
static INLINE void a64_addsub_carry(a64_emit *e, bool sub, bool s, int rd, int rn, a64_operand op)
{
   int rm = op.reg;
   if (op.is_imm || op.shift == A64_ROR || op.amount != 0)
   {
      rm = op.is_imm || op.reg != A64_IP0 ? A64_IP0 : A64_IP1;
      a64_mov_op(e, false, rm, op);
   }
   a64_dw(e, (sub ? 0x40000000u : 0) | (s ? 0x20000000u : 0) | 0x1A000000u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd);
}

/* Bfi(wd, wn, lsb, width) */
static INLINE void a64_bfi(a64_emit *e, int rd, int rn, unsigned lsb, unsigned width)
{
   a64_bfm(e, false, 1, rd, rn, (32 - lsb) & 31, width - 1);
}

/* Mrs(xt, NZCV), Msr(NZCV, xt) */
static INLINE void a64_mrs_nzcv(a64_emit *e, int rt)
{
   a64_dw(e, 0xD53B4200u | (u32)rt);
}

static INLINE void a64_msr_nzcv(a64_emit *e, int rt)
{
   a64_dw(e, 0xD51B4200u | (u32)rt);
}

/* ldr / str wt or xt (@x), [xn, #imm]: VIXL's LoadStoreMacro(), an offset
 * the instruction cannot have put in the scratch register first */
static INLINE void a64_ldst(a64_emit *e, bool load, bool x, int rt, int rn, s64 offset)
{
   unsigned size = x ? 8 : 4;
   u32 base = x ? 0xF9000000u : 0xB9000000u;
   if (offset >= 0 && (offset % size) == 0 && offset / size < 4096)
      a64_dw(e, base | (load ? 0x400000u : 0) | ((u32)(offset / size) << 10) | ((u32)rn << 5) | (u32)rt);
   else if (offset >= -256 && offset < 256)
      a64_dw(e, (base & ~0x01000000u) | (load ? 0x400000u : 0) | (((u32)offset & 0x1ff) << 12) | ((u32)rn << 5) | (u32)rt);
   else
   {
      a64_mov_imm(e, true, A64_IP0, (u64)offset);
      a64_dw(e, (x ? 0xF8206800u : 0xB8206800u) | (load ? 0x400000u : 0) | (A64_IP0 << 16) | ((u32)rn << 5) | (u32)rt);
   }
}

/* ldr / str / ldrb / strb (@size 1, 4 or 8) rt, [xn, xm (lsl #log2 size)] */
static INLINE void a64_ldst_rr(a64_emit *e, bool load, unsigned size, int rt, int rn, int rm, bool shifted)
{
   u32 base = size == 1 ? 0x38206800u : size == 4 ? 0xB8206800u : 0xF8206800u;
   a64_dw(e, base | (load ? 0x400000u : 0) | ((u32)rm << 16) | (shifted ? 0x1000u : 0) | ((u32)rn << 5) | (u32)rt);
}

/* ldp / stp wt1, wt2, [xn, #imm] (signed offset) */
static INLINE void a64_ldstp_w(a64_emit *e, bool load, int rt1, int rt2, int rn, int offset)
{
   a64_dw(e, 0x29000000u | (load ? 0x400000u : 0) | (((u32)(offset / 4) & 0x7f) << 15)
         | ((u32)rt2 << 10) | ((u32)rn << 5) | (u32)rt1);
}

static INLINE void a64_br(a64_emit *e, int rn)
{
   a64_dw(e, 0xD61F0000u | ((u32)rn << 5));
}

/* b / bl by @rel bytes: false where it does not reach */
static INLINE bool a64_b_rel(a64_emit *e, bool link, s64 rel)
{
   if (rel < -128 * 1024 * 1024 || rel >= 128 * 1024 * 1024)
      return false;
   a64_dw(e, (link ? 0x94000000u : 0x14000000u) | ((u32)(rel / 4) & 0x3ffffff));
   return true;
}

/* A label, and the branches to it made before it was put down */
typedef struct a64_label
{
   u32 *at;
   u32 *patch[16];
   int patches;
} a64_label;

static INLINE void a64_label_init(a64_label *l)
{
   l->at = NULL;
   l->patches = 0;
}

/* The branch's offset to @to put in it (b: 26 bits, b.cond and cbz/cbnz: 19) */
static INLINE void a64_label_link(u32 *from, const u32 *to)
{
   s32 words = (s32)(to - from);
   if ((*from & 0x7C000000u) == 0x14000000u)
      *from = (*from & 0xFC000000u) | ((u32)words & 0x3ffffff);
   else
      *from = (*from & 0xFF00001Fu) | (((u32)words & 0x7ffff) << 5);
}

/* b (@cond -1), b.cond, cbnz wt (@cond -2, @rt) to a label */
static INLINE void a64_b_label(a64_emit *e, int cond, int rt, a64_label *l)
{
   u32 *from = e->p;
   if (cond == -1)
      a64_dw(e, 0x14000000u);
   else if (cond == -2)
      a64_dw(e, 0x35000000u | (u32)rt);
   else
      a64_dw(e, 0x54000000u | (u32)cond);
   if (e->full)
      return;
   if (l->at != NULL)
      a64_label_link(from, l->at);
   else if (l->patches == (int)(sizeof(l->patch) / sizeof(l->patch[0])))
      e->full = true;
   else
      l->patch[l->patches++] = from;
}

static INLINE void a64_label_bind(a64_emit *e, a64_label *l)
{
   int i;
   l->at = e->p;
   for (i = 0; i < l->patches; i++)
      a64_label_link(l->patch[i], l->at);
   l->patches = 0;
}

/* ldr xt, =literal (the one literal there is) */
static INLINE void a64_ldr_literal(a64_emit *e, int rt, u64 value)
{
   e->literal = value;
   e->has_literal = true;
   if (e->literal_load_count < (int)(sizeof(e->literal_loads) / sizeof(e->literal_loads[0]))
         && e->p < e->end)
      e->literal_loads[e->literal_load_count++] = (int)(e->p - e->start);
   else
      e->full = true;
   a64_dw(e, 0x58000000u | (u32)rt);
}

/* The end of the code (after its ret): the literal pool, with its marker */
static INLINE void a64_finish(a64_emit *e)
{
   int i, at;
   if (!e->has_literal)
      return;
   /* (after the ret: nothing to branch over) */
   a64_dw(e, 0x58000000u | (3u << 5) | A64_ZR);	/* ldr xzr, #12: the pool's size in words, its marker's with it */
   at = (int)(e->p - e->start);
   a64_dw(e, (u32)e->literal);
   a64_dw(e, (u32)(e->literal >> 32));
   if (e->full)
      return;
   for (i = 0; i < e->literal_load_count; i++)
   {
      int from = e->literal_loads[i];
      e->start[from] |= ((u32)(at - from) & 0x7ffff) << 5;
   }
}

#endif
