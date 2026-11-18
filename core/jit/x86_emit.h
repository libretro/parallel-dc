/* The x86 and x86-64 instructions the recompilers make (the sound chip's
 * DSP, dsp_x64.c and dsp_x86.c, and its ARM7, arm7_rec_x64.c and
 * arm7_rec_x86.c), encoded as Xbyak encoded them: the short forms where
 * there is one (an 8-bit immediate, eax's own ALU, TEST and absolute
 * moves, no displacement off a base that needs none, a shift by 1, a
 * short jump to a label ahead or one near enough behind). C89. */
#ifndef X86_EMIT_H
#define X86_EMIT_H

#include <string.h>
#include "types.h"

enum
{
   X86_EAX, X86_ECX, X86_EDX, X86_EBX, X86_ESP, X86_EBP, X86_ESI, X86_EDI,
   X86_R8, X86_R9, X86_R10, X86_R11, X86_R12, X86_R13, X86_R14, X86_R15
};
/* (a memory operand's base: relative to the next instruction, x86-64 only) */
#define X86_RIP (-2)

/* The ALU operations' numbers (the /digit of 81 and 83, and op << 3 the
 * register forms) */
enum { X86_ADD = 0, X86_OR = 1, X86_ADC = 2, X86_SBB = 3, X86_AND = 4, X86_SUB = 5, X86_XOR = 6, X86_CMP = 7 };

/* Conditions (jcc, setcc, cmovcc) */
enum
{
   X86_CC_O = 0, X86_CC_NO = 1, X86_CC_B = 2, X86_CC_NB = 3, X86_CC_E = 4, X86_CC_NE = 5,
   X86_CC_BE = 6, X86_CC_A = 7, X86_CC_S = 8, X86_CC_NS = 9, X86_CC_L = 0xC, X86_CC_GE = 0xD,
   X86_CC_LE = 0xE, X86_CC_G = 0xF
};

typedef struct x86_emit
{
   u8 *start;
   u8 *p;
   u8 *end;
   /* Something did not fit (or a jump did not reach): what was made is not
    * to be run */
   bool full;
} x86_emit;

/* A memory operand: base and index registers (-1 for none), scale 1, 2, 4
 * or 8, and a displacement. With neither, the displacement is an address
 * (x86 only); with X86_RIP for the base, @target is the address. */
typedef struct x86_mem
{
   int base;
   int index;
   int scale;
   s32 disp;
   uintptr_t target;
} x86_mem;

static INLINE x86_mem x86_mi(int base, int index, int scale, s32 disp)
{
   x86_mem m;
   m.base   = base;
   m.index  = index;
   m.scale  = scale;
   m.disp   = disp;
   m.target = 0;
   return m;
}

static INLINE x86_mem x86_m(int base, s32 disp)
{
   return x86_mi(base, -1, 1, disp);
}

/* What is at @p: relative to the instruction (x86-64), at its address (x86) */
static INLINE x86_mem x86_rip(const void *p)
{
   x86_mem m = x86_m(X86_RIP, 0);
   m.target = (uintptr_t)p;
   return m;
}

static INLINE x86_mem x86_abs(const void *p)
{
   return x86_m(-1, (s32)(uintptr_t)p);
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
 * field); none where nothing calls for one. @reg8, @rm8: the registers are
 * byte registers, of which spl, bpl, sil and dil take a REX of their own. */
static INLINE void x86_rex8(x86_emit *e, bool w, int reg, int rm_reg, const x86_mem *m, bool reg8, bool rm8)
{
   u32 rex = 0x40;
   bool need = false;
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
   if (reg8 && reg >= 4 && reg <= 7)
      need = true;
   if (rm8 && m == NULL && rm_reg >= 4 && rm_reg <= 7)
      need = true;
   if (rex != 0x40 || need)
      x86_db(e, rex);
}

static INLINE void x86_rex(x86_emit *e, bool w, int reg, int rm_reg, const x86_mem *m)
{
   x86_rex8(e, w, reg, rm_reg, m, false, false);
}

/* ModRM (and SIB and displacement) of @reg against a memory operand;
 * @imm_after: the bytes of immediate that follow (for a displacement
 * relative to the instruction's end) */
static INLINE void x86_modrm_mi(x86_emit *e, int reg, const x86_mem *m, int imm_after)
{
   u32 r = (u32)(reg & 7) << 3;
   u32 ss = m->scale == 8 ? 3 : m->scale == 4 ? 2 : m->scale == 2 ? 1 : 0;

   if (m->base == X86_RIP)
   {
      s64 rel;
      x86_db(e, 0x05 | r);
      rel = (s64)((intptr_t)m->target - (intptr_t)(e->p + 4 + imm_after));
      if (rel != (s64)(s32)rel)
         e->full = true;
      x86_dd(e, (u32)(s32)rel);
      return;
   }
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

/* An instruction of one opcode byte, or 0x0F and a second (@op2 >= 0),
 * with @reg in ModRM's reg field and a register (@rm) or memory operand */
static INLINE void x86_op_r(x86_emit *e, bool w, u32 op1, int op2, int reg, int rm, bool reg8, bool rm8)
{
   x86_rex8(e, w, reg, rm, NULL, reg8, rm8);
   x86_db(e, op1);
   if (op2 >= 0)
      x86_db(e, (u32)op2);
   x86_modrm_r(e, reg, rm);
}

static INLINE void x86_op_m(x86_emit *e, bool w, u32 op1, int op2, int reg, x86_mem m, int imm_after, bool reg8)
{
   x86_rex8(e, w, reg, -1, &m, reg8, false);
   x86_db(e, op1);
   if (op2 >= 0)
      x86_db(e, (u32)op2);
   x86_modrm_mi(e, reg, &m, imm_after);
}

/* op r/m32, r32 (reg-to-rm form: 89 mov, 01 add, 31 xor...) */
static INLINE void x86_op_rr(x86_emit *e, bool w, u32 opcode, int dst, int src)
{
   x86_op_r(e, w, opcode, -1, src, dst, false, false);
}

static INLINE void x86_mov_rr(x86_emit *e, int dst, int src)
{
   x86_op_rr(e, false, 0x89, dst, src);
}

/* ALU r32, r32: add, or, adc, sbb, and, sub, xor, cmp */
static INLINE void x86_alu_rr(x86_emit *e, int op, int dst, int src)
{
   x86_op_rr(e, false, ((u32)op << 3) | 1, dst, src);
}

static INLINE void x86_xor_rr(x86_emit *e, int dst, int src)
{
   x86_alu_rr(e, X86_XOR, dst, src);
}

static INLINE void x86_add_rr(x86_emit *e, int dst, int src)
{
   x86_alu_rr(e, X86_ADD, dst, src);
}

/* ALU r32, [m] (03 add, 23 and...) */
static INLINE void x86_alu_rm(x86_emit *e, int op, int dst, x86_mem m)
{
   x86_op_m(e, false, ((u32)op << 3) | 3, -1, dst, m, 0, false);
}

static INLINE void x86_add_rm(x86_emit *e, int dst, x86_mem m)
{
   x86_alu_rm(e, X86_ADD, dst, m);
}

/* test r32, r32 */
static INLINE void x86_test_rr(x86_emit *e, int a, int b)
{
   x86_op_r(e, false, 0x85, -1, b, a, false, false);
}

/* test r32, imm32 */
static INLINE void x86_test_ri(x86_emit *e, int dst, u32 imm)
{
   if (dst == X86_EAX)
      x86_db(e, 0xA9);
   else
      x86_op_r(e, false, 0xF7, -1, 0, dst, false, false);
   x86_dd(e, imm);
}

/* mov r32 (r64 with @w), [m] / mov [m], r32 */
static INLINE void x86_load_w(x86_emit *e, bool w, int dst, x86_mem m)
{
   if (!w && dst == X86_EAX && m.base == -1 && m.index < 0)
   {
      /* (mov eax, [moffs32]) */
      x86_db(e, 0xA1);
      x86_dd(e, (u32)m.disp);
      return;
   }
   x86_op_m(e, w, 0x8B, -1, dst, m, 0, false);
}

static INLINE void x86_store_w(x86_emit *e, bool w, x86_mem m, int src)
{
   if (!w && src == X86_EAX && m.base == -1 && m.index < 0)
   {
      /* (mov [moffs32], eax) */
      x86_db(e, 0xA3);
      x86_dd(e, (u32)m.disp);
      return;
   }
   x86_op_m(e, w, 0x89, -1, src, m, 0, false);
}

static INLINE void x86_load(x86_emit *e, int dst, x86_mem m)
{
   x86_load_w(e, false, dst, m);
}

static INLINE void x86_store(x86_emit *e, x86_mem m, int src)
{
   x86_store_w(e, false, m, src);
}

/* mov word [m], r16 */
static INLINE void x86_store16(x86_emit *e, x86_mem m, int src)
{
   x86_db(e, 0x66);
   x86_op_m(e, false, 0x89, -1, src, m, 0, false);
}

/* mov byte [m], r8 */
static INLINE void x86_store8(x86_emit *e, x86_mem m, int src)
{
   x86_op_m(e, false, 0x88, -1, src, m, 0, true);
}

/* mov dword [m], imm32 */
static INLINE void x86_store_imm(x86_emit *e, x86_mem m, u32 imm)
{
   x86_op_m(e, false, 0xC7, -1, 0, m, 4, false);
   x86_dd(e, imm);
}

/* movzx / movsx r32, word [m]; movzx r32, byte [m]; movzx r32, r8 */
static INLINE void x86_movzx16(x86_emit *e, int dst, x86_mem m)
{
   x86_op_m(e, false, 0x0F, 0xB7, dst, m, 0, false);
}

static INLINE void x86_movsx16(x86_emit *e, int dst, x86_mem m)
{
   x86_op_m(e, false, 0x0F, 0xBF, dst, m, 0, false);
}

static INLINE void x86_movzx8m(x86_emit *e, int dst, x86_mem m)
{
   x86_op_m(e, false, 0x0F, 0xB6, dst, m, 0, false);
}

static INLINE void x86_movzx8r(x86_emit *e, int dst, int src)
{
   x86_op_r(e, false, 0x0F, 0xB6, dst, src, false, true);
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

/* op r32 (or r64), imm: add, or, adc, sbb, and, sub, xor, cmp */
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

/* op dword [m], imm */
static INLINE void x86_alu_mi(x86_emit *e, int op, x86_mem m, u32 imm)
{
   if (x86_is8((s32)imm))
   {
      x86_op_m(e, false, 0x83, -1, op, m, 1, false);
      x86_db(e, imm);
   }
   else
   {
      x86_op_m(e, false, 0x81, -1, op, m, 4, false);
      x86_dd(e, imm);
   }
}

/* shifts and rotates of r32, by an immediate and by cl */
enum { X86_ROL = 0, X86_ROR = 1, X86_RCL = 2, X86_RCR = 3, X86_SHL = 4, X86_SHR = 5, X86_SAR = 7 };
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

static INLINE void x86_shift_cl(x86_emit *e, int op, int dst)
{
   x86_op_r(e, false, 0xD3, -1, op, dst, false, false);
}

/* not / neg r32 */
static INLINE void x86_not(x86_emit *e, int dst)
{
   x86_op_r(e, false, 0xF7, -1, 2, dst, false, false);
}

static INLINE void x86_neg(x86_emit *e, int dst)
{
   x86_op_r(e, false, 0xF7, -1, 3, dst, false, false);
}

/* cmovcc r32, r32 / r32, [m] */
static INLINE void x86_cmov(x86_emit *e, int cc, int dst, int src)
{
   x86_op_r(e, false, 0x0F, 0x40 | cc, dst, src, false, false);
}

static INLINE void x86_cmov_m(x86_emit *e, int cc, int dst, x86_mem m)
{
   x86_op_m(e, false, 0x0F, 0x40 | cc, dst, m, 0, false);
}

/* setcc r8 */
static INLINE void x86_setcc(x86_emit *e, int cc, int dst)
{
   x86_op_r(e, false, 0x0F, 0x90 | cc, 0, dst, false, true);
}

/* bt dword [m], imm8 */
static INLINE void x86_bt_mi(x86_emit *e, x86_mem m, u32 bit)
{
   x86_op_m(e, false, 0x0F, 0xBA, 4, m, 1, false);
   x86_db(e, bit);
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

static INLINE void x86_pushf(x86_emit *e)
{
   x86_db(e, 0x9C);
}

static INLINE void x86_cmc(x86_emit *e)
{
   x86_db(e, 0xF5);
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

/* jmp / jcc rel32 to an address (@cc -1: jmp) */
static INLINE void x86_jmp_abs(x86_emit *e, int cc, uintptr_t target)
{
   int len = cc < 0 ? 5 : 6;
   s64 rel = (s64)((intptr_t)target - (intptr_t)(e->p + len));
   if (rel != (s64)(s32)rel)
      e->full = true;
   if (cc < 0)
      x86_db(e, 0xE9);
   else
   {
      x86_db(e, 0x0F);
      x86_db(e, 0x80 | (u32)cc);
   }
   x86_dd(e, (u32)(s32)rel);
}

/* jmp [m] */
static INLINE void x86_jmp_m(x86_emit *e, x86_mem m)
{
   x86_op_m(e, false, 0xFF, -1, 4, m, 0, false);
}

static INLINE void x86_ret(x86_emit *e)
{
   x86_db(e, 0xC3);
}

/* A label, and the jumps to it made before it was put down (as Xbyak's:
 * short, unless asked for near) */
typedef struct x86_label
{
   u8 *at;
   u8 *patch[16];
   u8 patch_size[16];
   int patches;
} x86_label;

static INLINE void x86_label_init(x86_label *l)
{
   l->at = NULL;
   l->patches = 0;
}

/* jmp / jcc to a label (@cc -1: jmp); @near_jump: rel32 whatever the distance */
static INLINE void x86_jmp_label(x86_emit *e, int cc, x86_label *l, bool near_jump)
{
   if (l->at != NULL)
   {
      s64 rel8 = (s64)(l->at - (e->p + 2));
      if (!near_jump && rel8 >= -128 && rel8 <= 127)
      {
         x86_db(e, cc < 0 ? 0xEB : 0x70 | (u32)cc);
         x86_db(e, (u32)(s32)rel8);
      }
      else
         x86_jmp_abs(e, cc, (uintptr_t)l->at);
      return;
   }
   if (l->patches == (int)(sizeof(l->patch) / sizeof(l->patch[0])))
   {
      e->full = true;
      return;
   }
   if (near_jump)
   {
      if (cc < 0)
         x86_db(e, 0xE9);
      else
      {
         x86_db(e, 0x0F);
         x86_db(e, 0x80 | (u32)cc);
      }
      l->patch[l->patches] = e->p;
      l->patch_size[l->patches++] = 4;
      x86_dd(e, 0);
   }
   else
   {
      x86_db(e, cc < 0 ? 0xEB : 0x70 | (u32)cc);
      l->patch[l->patches] = e->p;
      l->patch_size[l->patches++] = 1;
      x86_db(e, 0);
   }
}

/* The label put down here: the jumps to it made */
static INLINE void x86_label_bind(x86_emit *e, x86_label *l)
{
   int i;
   l->at = e->p;
   if (e->full)
      return;
   for (i = 0; i < l->patches; i++)
   {
      u8 *at = l->patch[i];
      s64 rel = (s64)(l->at - (at + l->patch_size[i]));
      if (l->patch_size[i] == 1)
      {
         if (rel < -128 || rel > 127)
            e->full = true;
         at[0] = (u8)rel;
      }
      else
      {
         u32 v = (u32)(s32)rel;
         at[0] = (u8)v;
         at[1] = (u8)(v >> 8);
         at[2] = (u8)(v >> 16);
         at[3] = (u8)(v >> 24);
      }
   }
   l->patches = 0;
}

#endif
