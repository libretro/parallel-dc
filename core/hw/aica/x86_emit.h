/* The x86 and x86-64 instructions the sound chip's DSP recompilers make
 * (dsp_x64.c, dsp_x86.c), encoded as Xbyak encoded them: the short forms
 * where there is one (an 8-bit immediate, eax's own ALU and absolute
 * moves, no displacement off a base that needs none, a shift by 1). C89. */
#ifndef X86_EMIT_H
#define X86_EMIT_H

#include <string.h>
#include "types.h"

enum
{
   X86_EAX, X86_ECX, X86_EDX, X86_EBX, X86_ESP, X86_EBP, X86_ESI, X86_EDI,
   X86_R8, X86_R9, X86_R10, X86_R11, X86_R12, X86_R13, X86_R14, X86_R15
};

/* The ALU operations' numbers (the /digit of 81 and 83) */
enum { X86_ADD = 0, X86_OR = 1, X86_AND = 4, X86_SUB = 5, X86_XOR = 6, X86_CMP = 7 };

/* Conditions (cmovcc) */
enum { X86_CC_E = 0x4, X86_CC_GE = 0xD, X86_CC_G = 0xF };

typedef struct x86_emit
{
   u8 *start;
   u8 *p;
   u8 *end;
   /* Something did not fit: what was made is not to be run */
   bool full;
} x86_emit;

/* A memory operand: base and index registers (-1 for none), scale 1, 2, 4
 * or 8, and a displacement. With neither, the displacement is an address
 * (x86 only). */
typedef struct x86_mem
{
   int base;
   int index;
   int scale;
   s32 disp;
} x86_mem;

static INLINE x86_mem x86_m(int base, s32 disp)
{
   x86_mem m;
   m.base  = base;
   m.index = -1;
   m.scale = 1;
   m.disp  = disp;
   return m;
}

static INLINE x86_mem x86_mi(int base, int index, int scale, s32 disp)
{
   x86_mem m;
   m.base  = base;
   m.index = index;
   m.scale = scale;
   m.disp  = disp;
   return m;
}

static INLINE void x86_init(x86_emit *e, u8 *code, size_t room)
{
   e->start = code;
   e->p     = code;
   e->end   = code + room;
   e->full  = false;
}

static INLINE void x86_db(x86_emit *e, u32 b)
{
   if (e->p >= e->end)
   {
      e->full = true;
      return;
   }
   *e->p++ = (u8)b;
}

static INLINE void x86_dd(x86_emit *e, u32 v)
{
   x86_db(e, v);
   x86_db(e, v >> 8);
   x86_db(e, v >> 16);
   x86_db(e, v >> 24);
}

static INLINE bool x86_is8(s32 v)
{
   return v >= -128 && v <= 127;
}

/* REX for a register (reg field) and a register or memory operand (rm
 * field); none where nothing calls for one */
static INLINE void x86_rex(x86_emit *e, bool w, int reg, int rm_reg, const x86_mem *m)
{
   u32 rex = 0x40;
   if (w)
      rex |= 8;
   if (reg >= 8)
      rex |= 4;
   if (m != NULL)
   {
      if (m->index >= 8)
         rex |= 2;
      if (m->base >= 8)
         rex |= 1;
   }
   else if (rm_reg >= 8)
      rex |= 1;
   if (rex != 0x40)
      x86_db(e, rex);
}

/* ModRM (and SIB and displacement) of @reg against a memory operand */
static INLINE void x86_modrm_m(x86_emit *e, int reg, const x86_mem *m)
{
   u32 r = (u32)(reg & 7) << 3;
   u32 ss = m->scale == 8 ? 3 : m->scale == 4 ? 2 : m->scale == 2 ? 1 : 0;

   if (m->base < 0)
   {
      if (m->index < 0)
      {
         x86_db(e, 0x05 | r);
         x86_dd(e, (u32)m->disp);
      }
      else
      {
         x86_db(e, 0x04 | r);
         x86_db(e, (ss << 6) | ((u32)(m->index & 7) << 3) | 5);
         x86_dd(e, (u32)m->disp);
      }
      return;
   }
   {
      u32 mod;
      bool sib = m->index >= 0 || (m->base & 7) == X86_ESP;
      if (m->disp == 0 && (m->base & 7) != X86_EBP)
         mod = 0x00;
      else if (x86_is8(m->disp))
         mod = 0x40;
      else
         mod = 0x80;
      if (sib)
      {
         x86_db(e, mod | r | 4);
         x86_db(e, (ss << 6) | ((u32)(m->index >= 0 ? m->index & 7 : 4) << 3) | (u32)(m->base & 7));
      }
      else
         x86_db(e, mod | r | (u32)(m->base & 7));
      if (mod == 0x40)
         x86_db(e, (u32)m->disp);
      else if (mod == 0x80)
         x86_dd(e, (u32)m->disp);
   }
}

static INLINE void x86_modrm_r(x86_emit *e, int reg, int rm)
{
   x86_db(e, 0xC0 | ((u32)(reg & 7) << 3) | (u32)(rm & 7));
}

/* op r/m32, r32 (reg-to-rm form: 89 mov, 01 add, 31 xor...) */
static INLINE void x86_op_rr(x86_emit *e, bool w, u32 opcode, int dst, int src)
{
   x86_rex(e, w, src, dst, NULL);
   x86_db(e, opcode);
   x86_modrm_r(e, src, dst);
}

static INLINE void x86_mov_rr(x86_emit *e, int dst, int src)
{
   x86_op_rr(e, false, 0x89, dst, src);
}

static INLINE void x86_xor_rr(x86_emit *e, int dst, int src)
{
   x86_op_rr(e, false, 0x31, dst, src);
}

static INLINE void x86_add_rr(x86_emit *e, int dst, int src)
{
   x86_op_rr(e, false, 0x01, dst, src);
}

/* mov r32, [m] / mov [m], r32 */
static INLINE void x86_load(x86_emit *e, int dst, x86_mem m)
{
   if (dst == X86_EAX && m.base < 0 && m.index < 0)
   {
      /* (mov eax, [moffs32]) */
      x86_db(e, 0xA1);
      x86_dd(e, (u32)m.disp);
      return;
   }
   x86_rex(e, false, dst, -1, &m);
   x86_db(e, 0x8B);
   x86_modrm_m(e, dst, &m);
}

static INLINE void x86_store(x86_emit *e, x86_mem m, int src)
{
   if (src == X86_EAX && m.base < 0 && m.index < 0)
   {
      /* (mov [moffs32], eax) */
      x86_db(e, 0xA3);
      x86_dd(e, (u32)m.disp);
      return;
   }
   x86_rex(e, false, src, -1, &m);
   x86_db(e, 0x89);
   x86_modrm_m(e, src, &m);
}

/* mov word [m], r16 */
static INLINE void x86_store16(x86_emit *e, x86_mem m, int src)
{
   x86_db(e, 0x66);
   x86_rex(e, false, src, -1, &m);
   x86_db(e, 0x89);
   x86_modrm_m(e, src, &m);
}

/* mov dword [m], imm32 */
static INLINE void x86_store_imm(x86_emit *e, x86_mem m, u32 imm)
{
   x86_rex(e, false, 0, -1, &m);
   x86_db(e, 0xC7);
   x86_modrm_m(e, 0, &m);
   x86_dd(e, imm);
}

/* movzx / movsx r32, word [m] */
static INLINE void x86_movzx16(x86_emit *e, int dst, x86_mem m)
{
   x86_rex(e, false, dst, -1, &m);
   x86_db(e, 0x0F);
   x86_db(e, 0xB7);
   x86_modrm_m(e, dst, &m);
}

static INLINE void x86_movsx16(x86_emit *e, int dst, x86_mem m)
{
   x86_rex(e, false, dst, -1, &m);
   x86_db(e, 0x0F);
   x86_db(e, 0xBF);
   x86_modrm_m(e, dst, &m);
}

/* op r32, [m] (03 add) */
static INLINE void x86_add_rm(x86_emit *e, int dst, x86_mem m)
{
   x86_rex(e, false, dst, -1, &m);
   x86_db(e, 0x03);
   x86_modrm_m(e, dst, &m);
}

/* mov r32, imm32 */
static INLINE void x86_mov_ri(x86_emit *e, int dst, u32 imm)
{
   x86_rex(e, false, 0, dst, NULL);
   x86_db(e, 0xB8 | (u32)(dst & 7));
   x86_dd(e, imm);
}

/* mov r64, imm64: as a 32-bit move where it zero-extends to it, a
 * sign-extended 32-bit one where that does, the whole 64 bits otherwise */
static INLINE void x86_mov_ri64(x86_emit *e, int dst, u64 imm)
{
   if ((imm >> 32) == 0)
   {
      x86_mov_ri(e, dst, (u32)imm);
      return;
   }
   if ((u64)(s64)(s32)(u32)imm == imm)
   {
      x86_rex(e, true, 0, dst, NULL);
      x86_db(e, 0xC7);
      x86_modrm_r(e, 0, dst);
      x86_dd(e, (u32)imm);
      return;
   }
   x86_rex(e, true, 0, dst, NULL);
   x86_db(e, 0xB8 | (u32)(dst & 7));
   x86_dd(e, (u32)imm);
   x86_dd(e, (u32)(imm >> 32));
}

/* op r32 (or r64), imm: add, or, and, sub, xor, cmp */
static INLINE void x86_alu_ri(x86_emit *e, bool w, int op, int dst, u32 imm)
{
   if (x86_is8((s32)imm))
   {
      x86_rex(e, w, 0, dst, NULL);
      x86_db(e, 0x83);
      x86_modrm_r(e, op, dst);
      x86_db(e, imm);
   }
   else if (dst == X86_EAX)
   {
      x86_rex(e, w, 0, dst, NULL);
      x86_db(e, ((u32)op << 3) | 5);
      x86_dd(e, imm);
   }
   else
   {
      x86_rex(e, w, 0, dst, NULL);
      x86_db(e, 0x81);
      x86_modrm_r(e, op, dst);
      x86_dd(e, imm);
   }
}

/* shl / sar r32, imm (5 /4, 7 /7) */
enum { X86_SHL = 4, X86_SAR = 7 };
static INLINE void x86_shift_ri(x86_emit *e, int op, int dst, u32 count)
{
   x86_rex(e, false, 0, dst, NULL);
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

static INLINE void x86_neg(x86_emit *e, int dst)
{
   x86_rex(e, false, 0, dst, NULL);
   x86_db(e, 0xF7);
   x86_modrm_r(e, 3, dst);
}

/* cmovcc r32, r32 */
static INLINE void x86_cmov(x86_emit *e, int cc, int dst, int src)
{
   x86_rex(e, false, dst, src, NULL);
   x86_db(e, 0x0F);
   x86_db(e, 0x40 | (u32)cc);
   x86_modrm_r(e, dst, src);
}

/* movsxd r64, r32 */
static INLINE void x86_movsxd(x86_emit *e, int dst, int src)
{
   x86_rex(e, true, dst, src, NULL);
   x86_db(e, 0x63);
   x86_modrm_r(e, dst, src);
}

/* imul r64, r64 */
static INLINE void x86_imul_rr64(x86_emit *e, int dst, int src)
{
   x86_rex(e, true, dst, src, NULL);
   x86_db(e, 0x0F);
   x86_db(e, 0xAF);
   x86_modrm_r(e, dst, src);
}

/* sar r64, imm */
static INLINE void x86_sar_ri64(x86_emit *e, int dst, u32 count)
{
   x86_rex(e, true, 0, dst, NULL);
   x86_db(e, count == 1 ? 0xD1 : 0xC1);
   x86_modrm_r(e, X86_SAR, dst);
   if (count != 1)
      x86_db(e, count);
}

/* sub / add r64, imm (the stack) */
static INLINE void x86_alu_ri64(x86_emit *e, int op, int dst, u32 imm)
{
   x86_alu_ri(e, true, op, dst, imm);
}

/* imul r/m32: edx:eax = eax * r32 */
static INLINE void x86_imul1(x86_emit *e, int src)
{
   x86_rex(e, false, 0, src, NULL);
   x86_db(e, 0xF7);
   x86_modrm_r(e, 5, src);
}

/* shrd r32, r32, imm */
static INLINE void x86_shrd(x86_emit *e, int dst, int src, u32 count)
{
   x86_rex(e, false, src, dst, NULL);
   x86_db(e, 0x0F);
   x86_db(e, 0xAC);
   x86_modrm_r(e, src, dst);
   x86_db(e, count);
}

/* push / pop (r64 on x86-64, r32 on x86) */
static INLINE void x86_push(x86_emit *e, int r)
{
   x86_rex(e, false, 0, r, NULL);
   x86_db(e, 0x50 | (u32)(r & 7));
}

static INLINE void x86_pop(x86_emit *e, int r)
{
   x86_rex(e, false, 0, r, NULL);
   x86_db(e, 0x58 | (u32)(r & 7));
}

/* call rel32: false where @target is out of its reach */
static INLINE bool x86_call(x86_emit *e, uintptr_t target)
{
   s64 rel = (s64)((intptr_t)target - (intptr_t)(e->p + 5));
   if (rel != (s64)(s32)rel)
   {
      e->full = true;
      return false;
   }
   x86_db(e, 0xE8);
   x86_dd(e, (u32)(s32)rel);
   return true;
}

static INLINE void x86_ret(x86_emit *e)
{
   x86_db(e, 0xC3);
}

#endif
