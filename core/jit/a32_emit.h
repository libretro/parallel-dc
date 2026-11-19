/* The 32-bit ARM (ARMv7, with VFP) instructions the SH4's recompiler
 * (rec-ARM/rec_arm.c) and the sound chip's (hw/arm7/arm7_rec_arm32.c) make,
 * each encoded as the emitter they were written with encoded it. C89.
 *
 * The file that includes this says where an instruction goes: A32_EMIT(w)
 * puts one there, A32_HERE() is where the next one goes. */
#ifndef A32_EMIT_H
#define A32_EMIT_H

#include <stdio.h>
#include "types.h"

/* Registers */
enum
{
   A32_R0, A32_R1, A32_R2, A32_R3, A32_R4, A32_R5, A32_R6, A32_R7,
   A32_R8, A32_R9, A32_R10, A32_R11, A32_R12, A32_SP, A32_LR, A32_PC
};
#define A32_IP A32_R12

/* Conditions */
enum
{
   A32_EQ, A32_NE, A32_CS, A32_CC, A32_MI, A32_PL, A32_VS, A32_VC,
   A32_HI, A32_LS, A32_GE, A32_LT, A32_GT, A32_LE, A32_AL
};
#define A32_HS A32_CS
#define A32_LO A32_CC

/* Data processing, by their opcode */
enum
{
   A32_AND, A32_EOR, A32_SUB, A32_RSB, A32_ADD, A32_ADC, A32_SBC, A32_RSC,
   A32_TST, A32_TEQ, A32_CMP, A32_CMN, A32_ORR, A32_MOV, A32_BIC, A32_MVN
};

/* Shifts */
enum { A32_LSL, A32_LSR, A32_ASR, A32_ROR };

/* An address: the offset as it is, before the base is moved on by it, or
 * after */
enum { A32_OFFSET, A32_POST, A32_PRE };

/* VFP's 1.0, as vmov takes it */
#define A32_FPU_IMM_1 0x70

#define A32_COND(cc) ((u32)((cc) & 15) << 28)

/* An immediate a data-processing instruction can have: 8 bits rotated
 * right by an even amount, as the field it goes in (-1: none) */
static INLINE u32 a32_imm_enc(u32 imm32)
{
   int i;
   for (i = 0; i <= 30; i += 2)
   {
      u32 immv = i == 0 ? imm32 : (imm32 << i) | (imm32 >> (32 - i));
      if (immv < 256)
         return ((u32)(i / 2) << 8) | immv;
   }
   return (u32)-1;
}

static INLINE bool a32_is_imm(u32 imm32)
{
   return a32_imm_enc(imm32) != (u32)-1;
}

/* rd = rn op #imm (with the flags, @s: tst, teq, cmp and cmn always) */
static INLINE void a32_dp_imm(int op, bool s, int rd, int rn, u32 imm, int cc)
{
   u32 i = 0x02000000u | ((u32)op << 21) | A32_COND(cc) | ((u32)(rn & 15) << 16) | ((u32)(rd & 15) << 12);
   u32 enc = a32_imm_enc(imm);
   verify(enc != (u32)-1);
   i |= enc;
   if (s || (op >= A32_TST && op <= A32_CMN))
      i |= 1u << 20;
   A32_EMIT(i);
}

/* rd = rn op (rm shifted by #amount) */
static INLINE void a32_dp_reg(int op, bool s, int rd, int rn, int rm, int shift, u32 amount, int cc)
{
   u32 i = ((u32)op << 21) | A32_COND(cc) | ((u32)(rn & 15) << 16) | ((u32)(rd & 15) << 12) | (u32)(rm & 15)
         | ((u32)shift << 5) | ((amount & 31) << 7);
   if (s || (op >= A32_TST && op <= A32_CMN))
      i |= 1u << 20;
   A32_EMIT(i);
}

/* rd = rn op (rm shifted by rs) */
static INLINE void a32_dp_rsr(int op, int rd, int rn, int rm, int shift, int rs, int cc)
{
   A32_EMIT(((u32)op << 21) | A32_COND(cc) | ((u32)(rn & 15) << 16) | ((u32)(rd & 15) << 12) | (u32)(rm & 15)
         | ((u32)shift << 5) | ((u32)(rs & 15) << 8) | (1u << 4));
}

/* The usual forms */
#define a32_mov(rd, rm, cc) a32_dp_reg(A32_MOV, false, rd, 0, rm, A32_LSL, 0, cc)
#define a32_mov_imm(rd, imm, cc) a32_dp_imm(A32_MOV, false, rd, 0, imm, cc)
#define a32_op(op, rd, rn, rm, cc) a32_dp_reg(op, false, rd, rn, rm, A32_LSL, 0, cc)
#define a32_op_imm(op, rd, rn, imm, cc) a32_dp_imm(op, false, rd, rn, imm, cc)
#define a32_cmp(rn, rm, cc) a32_dp_reg(A32_CMP, false, 0, rn, rm, A32_LSL, 0, cc)
#define a32_cmp_imm(rn, imm, cc) a32_dp_imm(A32_CMP, false, 0, rn, imm, cc)
#define a32_tst(rn, rm, cc) a32_dp_reg(A32_TST, false, 0, rn, rm, A32_LSL, 0, cc)
#define a32_tst_imm(rn, imm, cc) a32_dp_imm(A32_TST, false, 0, rn, imm, cc)

/* rd = rm shifted by #amount (lsl/lsr/asr/ror; ror #0 is rrx), or by rs */
static INLINE void a32_shift_imm(int shift, bool s, int rd, int rm, u32 amount, int cc)
{
   a32_dp_reg(A32_MOV, s, rd, 0, rm, shift, amount, cc);
}

static INLINE void a32_shift_reg(int shift, int rd, int rm, int rs, int cc)
{
   a32_dp_rsr(A32_MOV, rd, 0, rm, shift, rs, cc);
}

static INLINE void a32_movw(int rd, u32 imm16, int cc)
{
   A32_EMIT(0x03000000u | A32_COND(cc) | ((imm16 & 0xF000) << 4) | ((u32)(rd & 15) << 12) | (imm16 & 0x0FFF));
}

static INLINE void a32_movt(int rd, u32 imm16, int cc)
{
   A32_EMIT(0x03400000u | A32_COND(cc) | ((imm16 & 0xF000) << 4) | ((u32)(rd & 15) << 12) | (imm16 & 0x0FFF));
}

/* A constant: movw, and movt where it has a high half */
static INLINE void a32_mov32(int rd, u32 imm32, int cc)
{
   a32_movw(rd, imm32 & 0xFFFF, cc);
   if (imm32 >> 16)
      a32_movt(rd, (imm32 >> 16) & 0xFFFF, cc);
}

static INLINE void a32_ubfx(int rd, int rm, u32 lsb, u32 width, int cc)
{
   verify(lsb + width <= 32);
   A32_EMIT(0x07E00050u | A32_COND(cc) | ((u32)(rd & 15) << 12) | (u32)(rm & 15) | ((lsb & 31) << 7)
         | (((width - 1) & 31) << 16));
}

/* sxtb, sxth, uxtb, uxth */
#define A32_SXTB 0x06AF0070u
#define A32_SXTH 0x06BF0070u
#define A32_UXTB 0x06EF0070u
#define A32_UXTH 0x06FF0070u
static INLINE void a32_extend(u32 op, int rd, int rm, int cc)
{
   A32_EMIT(op | A32_COND(cc) | ((u32)(rd & 15) << 12) | (u32)(rm & 15));
}

static INLINE void a32_mul(int rd, int rs, int rm, int cc)
{
   A32_EMIT(0x00000090u | A32_COND(cc) | ((u32)(rd & 15) << 16) | ((u32)(rs & 15) << 8) | (u32)(rm & 15));
}

/* umull / smull (@sign) rdlo, rdhi, rs, rm */
static INLINE void a32_mull(bool sign, int rdhi, int rdlo, int rs, int rm, int cc)
{
   A32_EMIT((sign ? 0x00C00090u : 0x00800090u) | A32_COND(cc) | ((u32)(rdhi & 15) << 16) | ((u32)(rdlo & 15) << 12)
         | ((u32)(rs & 15) << 8) | (u32)(rm & 15));
}

/* mrs rd, cpsr/spsr (@spsr) and msr cpsr/spsr_<fields>, rm */
static INLINE void a32_mrs(int rd, u32 spsr, int cc)
{
   A32_EMIT(0x01000000u | A32_COND(cc) | ((spsr & 1) << 22) | (15u << 16) | ((u32)(rd & 15) << 12));
}

static INLINE void a32_msr(u32 spsr, u32 fmask, int rm, int cc)
{
   A32_EMIT(0x01200000u | A32_COND(cc) | ((spsr & 1) << 22) | ((fmask & 15) << 16) | (15u << 12) | (u32)(rm & 15));
}

static INLINE void a32_nop(void)
{
   A32_EMIT(0xE320F000u);
}

/* Branches */
static INLINE void a32_bx(int rm, int cc)
{
   A32_EMIT(0x012FFF10u | A32_COND(cc) | (u32)(rm & 15));
}

static INLINE void a32_blx(int rm, int cc)
{
   A32_EMIT(0x012FFF30u | A32_COND(cc) | (u32)(rm & 15));
}

/* b / bl (@link) to @offset bytes from the instruction after the next */
static INLINE void a32_b(bool link, s32 offset, int cc)
{
   A32_EMIT((link ? 0x0B000000u : 0x0A000000u) | A32_COND(cc) | (((u32)offset >> 2) & 0xFFFFFF));
}

/* A call or a jump to @target: one instruction where it is in reach, the
 * address in ip where not. (A call to Thumb code is blx, which has no
 * condition but always.) */
static INLINE void a32_call(uintptr_t target, int cc)
{
   const bool thumb = (target & 1) != 0;
   s32 lit;
   target &= ~(uintptr_t)1;
   lit = (s32)((intptr_t)target - ((intptr_t)A32_HERE() + 8));
   if (lit == 0)
   {
      printf("Error, Compiler caught NULL literal, CALL(%08X)\n", (unsigned)target);
      verify(false);
      return;
   }
   if (lit < -33554432 || lit > 33554428)
   {
      printf("Warning, CALL(%08X) is out of range for literal(%08X)\n", (unsigned)target, (unsigned)lit);
      a32_mov32(A32_IP, (u32)target, cc);
      a32_blx(A32_IP, cc);
      return;
   }
   if (thumb)
   {
      verify(cc == A32_EQ);
      A32_EMIT(0xFA000000u | (1u << 24) | (((u32)lit >> 2) & 0xFFFFFF));
   }
   else
      a32_b(true, lit, cc);
}

static INLINE void a32_jump(uintptr_t target, int cc)
{
   s32 lit;
   verify((target & 1) == 0);
   target &= ~(uintptr_t)1;
   lit = (s32)((intptr_t)target - ((intptr_t)A32_HERE() + 8));
   if (lit < -33554432 || lit > 33554428)
   {
      printf("Warning, %X is out of range for imm jump! \n", (unsigned)target);
      a32_mov32(A32_IP, (u32)target, cc);
      a32_bx(A32_IP, cc);
      return;
   }
   a32_b(false, lit, cc);
}

/* Loads and stores of a word or a byte: [rn, #offset] (an offset of 0 is
 * written as #-0), or [rn, rm, <shift> #amount], added (@add) or taken off */
#define A32_STR  0x04000000u
#define A32_LDR  0x04100000u
#define A32_STRB 0x04400000u
#define A32_LDRB 0x04500000u
static INLINE u32 a32_mode(int mode)
{
   return mode == A32_OFFSET ? 1u << 24 : mode == A32_PRE ? (1u << 24) | (1u << 21) : 0;
}

static INLINE void a32_ls_imm(u32 op, int rt, int rn, s32 offset, int mode, int cc)
{
   u32 i = op | A32_COND(cc) | ((u32)(rt & 15) << 12) | ((u32)(rn & 15) << 16) | a32_mode(mode);
   if (offset != 0)
   {
      if (offset > 0)
         i |= 1u << 23;
      i |= (u32)(offset < 0 ? -offset : offset) & 0xFFF;
   }
   A32_EMIT(i);
}

static INLINE void a32_ls_reg(u32 op, int rt, int rn, int rm, int mode, bool add, int shift, u32 amount, int cc)
{
   u32 i = op | 0x02000000u | A32_COND(cc) | ((u32)(rt & 15) << 12) | ((u32)(rn & 15) << 16) | (u32)(rm & 15)
         | a32_mode(mode) | ((amount & 0x1F) << 7) | (((u32)shift & 3) << 5);
   if (add)
      i |= 1u << 23;
   A32_EMIT(i);
}

/* Halfwords and signed bytes: [rn, #offset] (8 bits; 0 written as #-0),
 * or [rn, rm] added (@add) or taken off */
#define A32_STRH  0x000000B0u
#define A32_LDRH  0x001000B0u
#define A32_LDRSB 0x001000D0u
#define A32_LDRSH 0x001000F0u
static INLINE void a32_lsh_imm(u32 op, int rt, int rn, s32 offset, int cc)
{
   u32 i = op | (1u << 22) | (1u << 24) | A32_COND(cc) | ((u32)(rt & 15) << 12) | ((u32)(rn & 15) << 16);
   if (offset != 0)
   {
      u32 a;
      verify(offset >= -128 && offset <= 127);
      if (offset > 0)
         i |= 1u << 23;
      a = (u32)(offset < 0 ? -offset : offset);
      i |= a & 0x0F;
      i |= (a & 0xF0) << 4;
   }
   A32_EMIT(i);
}

static INLINE void a32_lsh_reg(u32 op, int rt, int rn, int rm, bool add, int cc)
{
   u32 i = op | (1u << 24) | A32_COND(cc) | ((u32)(rt & 15) << 12) | ((u32)(rn & 15) << 16) | (u32)(rm & 15);
   if (add)
      i |= 1u << 23;
   A32_EMIT(i);
}

/* ---- VFP: sN is a single, dN a double ---- */

#define A32_SD(s) ((((u32)(s) & 0x1E) << 11) | (((u32)(s) & 1) << 22))
#define A32_SN(s) ((((u32)(s) & 0x1E) << 15) | (((u32)(s) & 1) << 7))
#define A32_SM(s) ((((u32)(s) & 0x1E) >> 1) | (((u32)(s) & 1) << 5))
#define A32_DD(d) ((((u32)(d) & 0x0F) << 12) | (((u32)(d) & 0x10) << 18))
#define A32_DM(d) ((((u32)(d) & 0x0F)) | (((u32)(d) & 0x10) << 1))

/* vldr / vstr sd or dd (@dbl), [rn, #words * 4] (#0 is #-0) */
static INLINE void a32_vls(bool load, bool dbl, int vd, int rn, s32 words, int cc)
{
   u32 i = (load ? 0x0D100A00u : 0x0D000A00u) | (dbl ? 0x100u : 0) | A32_COND(cc)
         | (dbl ? A32_DD(vd) : A32_SD(vd)) | ((u32)(rn & 15) << 16);
   if (words > 0)
      i |= 1u << 23;
   i |= (u32)(words < 0 ? -words : words) & 255;
   A32_EMIT(i);
}

/* vldm / vstm rn(!), {dd...} of @count doubles */
static INLINE void a32_vlsm(bool load, int dd, int rn, u32 count, bool wb, int cc)
{
   A32_EMIT((load ? 0x0C100B00u : 0x0C000B00u) | A32_COND(cc) | A32_DD(dd) | ((u32)(rn & 15) << 16)
         | ((count << 1) & 255) | (1u << 23) | (wb ? 1u << 21 : 0));
}

/* vmov sd, sm; sn, rt; rt, sn; dm, rt, rt2; rt, rt2, dm; sd, #imm */
static INLINE void a32_vmov_ss(int sd, int sm, int cc)
{
   A32_EMIT(0x0EB00A40u | A32_COND(cc) | A32_SD(sd) | A32_SM(sm));
}

static INLINE void a32_vmov_sr(int sn, int rt, int cc)
{
   A32_EMIT(0x0E000A10u | A32_COND(cc) | A32_SN(sn) | ((u32)(rt & 15) << 12));
}

static INLINE void a32_vmov_rs(int rt, int sn, int cc)
{
   A32_EMIT(0x0E100A10u | A32_COND(cc) | A32_SN(sn) | ((u32)(rt & 15) << 12));
}

static INLINE void a32_vmov_drr(int dm, int rt, int rt2, int cc)
{
   A32_EMIT(0x0C400B10u | A32_COND(cc) | A32_DM(dm) | ((u32)(rt & 15) << 12) | ((u32)(rt2 & 15) << 16));
}

static INLINE void a32_vmov_rrd(int rt, int rt2, int dm, int cc)
{
   A32_EMIT(0x0C500B10u | A32_COND(cc) | A32_DM(dm) | ((u32)(rt & 15) << 12) | ((u32)(rt2 & 15) << 16));
}

static INLINE void a32_vmov_imm(int sd, u32 imm8, int cc)
{
   A32_EMIT(0x0EB10A00u | A32_COND(cc) | (imm8 & 0x0F) | ((imm8 & 0xF0) << 12) | A32_SD(sd));
}

/* sd = sn op sm */
#define A32_VMLA 0x0E000A00u
#define A32_VMUL 0x0E200A00u
#define A32_VADD 0x0E300A00u
#define A32_VSUB 0x0E300A40u
#define A32_VDIV 0x0E800A00u
static INLINE void a32_vfp3(u32 op, int sd, int sn, int sm, int cc)
{
   A32_EMIT(op | A32_COND(cc) | A32_SD(sd) | A32_SN(sn) | A32_SM(sm));
}

/* sd = op sm */
#define A32_VABS   0x0EB00AC0u
#define A32_VNEG   0x0EB10A40u
#define A32_VSQRT  0x0EB10AC0u
#define A32_VCMP   0x0EB40A40u
#define A32_VCVT_S32_F32 0x0EBD0AC0u	/* to an integer (in a single) */
#define A32_VCVT_F32_S32 0x0EB80AC0u	/* from one */
static INLINE void a32_vfp2(u32 op, int sd, int sm, int cc)
{
   A32_EMIT(op | A32_COND(cc) | A32_SD(sd) | A32_SM(sm));
}

/* vmrs rt, fpscr (rt pc: the flags) */
static INLINE void a32_vmrs(int rt, int cc)
{
   A32_EMIT(0x0EF10A10u | A32_COND(cc) | ((u32)(rt & 15) << 12));
}

#endif
