/* The SH4's portable recompiler, for a processor with no recompiler of its
 * own (HAVE_GENERIC_JIT=1, TARGET_PORTABLE_REC: FEAT_SHREC is DYNAREC_CPP).
 *
 * It makes no machine code. A block is compiled into a row of records in
 * the code cache, each the function that does an operation and where its
 * operands are - the registers in the SH4's context, or constants in the
 * record itself - and is run by calling them one after another: what the
 * decoder and the SSA optimizer have made of the SH4's instructions,
 * without the interpreter's decoding of each instruction every time it is
 * run. The code cache is plain memory, written and read: nothing in it is
 * run as code, so it needs no memory that can be.
 *
 * The common operations - moves, integer arithmetic and comparisons, the
 * four floating-point ones, memory accesses, what takes the interpreter's
 * own handler - are done by functions here; the rest are calls to their
 * canonical functions (dyna/shil_canonical.h), with the operands those are
 * to have. A block does not go on to the next: it comes back to the main
 * loop, which looks the next one up.
 *
 * tools/sh4/portable_run.sh runs it against the interpreter. */
#include "build.h"
#if FEAT_SHREC == DYNAREC_CPP
#include <setjmp.h>
#include <string.h>
#include "types.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/modules/ccn.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_interpreter.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/mem/_vmem.h"

typedef struct pop pop;
/* An operation's function: the record to go on with, or NULL at the end
 * of the block */
typedef const pop *(*pop_fn)(const pop *o);

/* A record: its function, a canonical function it calls, where its
 * operands are (a call's arguments, in order, then where what it returns
 * goes; a move's destination, then its sources), constants the operands
 * can be, and what else the function needs to know */
struct pop
{
	pop_fn fn;
	void (*call)(void);
	void *p[6];
	u32 imm[4];
	u32 n;
	u32 pc;
};

/* What is left of the time slice, in the cycles the blocks run take off it */
int cycle_counter;
/* Where an exception comes back to: the main loop */
static jmp_buf jmp_env;

/* The SH4's pc of an operation, odd in a delay slot */
static u32 op_pc(const RuntimeBlockInfo *block, const shil_opcode *op)
{
	return block->vaddr + op->guest_offs - (op->delay_slot ? 1 : 0);
}

/* What is left of the time slice, where the timers find it, before a call
 * that can reach an on-chip register: see cycle_counter in sh4_if.h */
#define SLICE_LEFT() (Sh4cntx.cycle_counter = cycle_counter)

void ngen_init(void)
{
}

void ngen_ResetBlocks(void)
{
}

/* (blocks are not linked to one another) */
u32 ngen_Relink(RuntimeBlockInfo *block)
{
	(void)block;
	return 0;
}

void ngen_GetFeatures(ngen_features *dst)
{
	dst->InterpreterFallback = false;
	dst->OnlyDynamicEnds = false;
}

void ngen_blockcheckfail(u32 pc)
{
	rdv_BlockCheckFail(pc);
}

/* (nothing is accessed in a way that faults) */
bool ngen_Rewrite(unat *host_pc)
{
	(void)host_pc;
	return false;
}

void ngen_HandleException(void)
{
	longjmp(jmp_env, 1);
}

#ifndef NO_MMU
/* An exception raised by an access with the MMU on (DoMMUException() has
 * taken it): back to the main loop */
static void mem_exception(u32 pc)
{
	/* (an odd pc: in a delay slot) */
	spc = (pc & 1) ? pc - 1 : pc;
	cycle_counter += CPU_RATIO * 2;
	longjmp(jmp_env, 1);
}
#endif

/* One the interpreter raised */
static void sh4_exception(SH4ThrownException *ex, u32 pc)
{
	if (pc & 1)
	{
		AdjustDelaySlotException(ex);
		pc--;
	}
	Do_Exception(pc, ex->expEvn, ex->callVect);
	cycle_counter += CPU_RATIO * 4;
	longjmp(jmp_env, 1);
}

#ifndef NO_MMU
static u32 translate(u32 addr, u32 pc, u32 size, u32 tt)
{
	u32 paddr = 0;
	const u32 rv = mmu_data_translation_c(addr, &paddr, tt, size);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(addr, rv, tt);
		mem_exception(pc);
	}
	return paddr;
}

static u32 read_mmu(u32 addr, u32 pc, u32 size, u32 *high)
{
	u32 paddr;
	if (size == 8 && mmu_in_two_pages(addr))
	{
		/* (two accesses, each translated: mmu.h) */
		const u32 low = _vmem_ReadMem32(translate(addr, pc, 4, MMU_TT_DREAD));
		*high = _vmem_ReadMem32(translate(addr + 4, pc, 4, MMU_TT_DREAD));
		return low;
	}
	paddr = translate(addr, pc, size, MMU_TT_DREAD);
	switch (size)
	{
	case 1:
		return (u32)(s32)(s8)_vmem_ReadMem8(paddr);
	case 2:
		return (u32)(s32)(s16)_vmem_ReadMem16(paddr);
	case 4:
		return _vmem_ReadMem32(paddr);
	default:
		{
			const u64 v = _vmem_ReadMem64(paddr);
			*high = (u32)(v >> 32);
			return (u32)v;
		}
	}
}

static void write_mmu(u32 addr, u32 pc, u32 size, u32 low, u32 high)
{
	u32 paddr;
	if (size == 8 && mmu_in_two_pages(addr))
	{
		_vmem_WriteMem32(translate(addr, pc, 4, MMU_TT_DWRITE), low);
		_vmem_WriteMem32(translate(addr + 4, pc, 4, MMU_TT_DWRITE), high);
		return;
	}
	paddr = translate(addr, pc, size, MMU_TT_DWRITE);
	switch (size)
	{
	case 1:
		_vmem_WriteMem8(paddr, (u8)low);
		break;
	case 2:
		_vmem_WriteMem16(paddr, (u16)low);
		break;
	case 4:
		_vmem_WriteMem32(paddr, low);
		break;
	default:
		_vmem_WriteMem64(paddr, ((u64)high << 32) | low);
		break;
	}
}
#endif

/* -- The operations ---------------------------------------------------- */

#define NEXT(o) ((o) + 1)
#define U32(x) (*(u32 *)(x))

/* p[0] = p[1] */
static const pop *op_mov32(const pop *o)
{
	U32(o->p[0]) = U32(o->p[1]);
	return NEXT(o);
}

static const pop *op_mov64(const pop *o)
{
	u32 *d = (u32 *)o->p[0];
	const u32 *s = (const u32 *)o->p[1];
	const u32 low = s[0], high = s[1];
	d[0] = low;
	d[1] = high;
	return NEXT(o);
}

/* p[0] = p[1] + p[2] (jdyn, jcond) */
static const pop *op_add3(const pop *o)
{
	U32(o->p[0]) = U32(o->p[1]) + U32(o->p[2]);
	return NEXT(o);
}

/* The address of an access: p[1] + p[2] */
#define MEM_ADDR(o) (U32((o)->p[1]) + U32((o)->p[2]))

/* p[0] = the n bytes at the address */
static const pop *op_read(const pop *o)
{
	const u32 addr = MEM_ADDR(o);
	u32 *d = (u32 *)o->p[0];
	SLICE_LEFT();
	switch (o->n)
	{
	case 1:
		d[0] = (u32)(s32)(s8)ReadMem8(addr);
		break;
	case 2:
		d[0] = (u32)(s32)(s16)ReadMem16(addr);
		break;
	case 4:
		d[0] = ReadMem32(addr);
		break;
	default:
		{
			const u64 v = ReadMem64(addr);
			d[0] = (u32)v;
			d[1] = (u32)(v >> 32);
		}
		break;
	}
	return NEXT(o);
}

/* the n bytes at the address = p[0] */
static const pop *op_write(const pop *o)
{
	const u32 addr = MEM_ADDR(o);
	const u32 *s = (const u32 *)o->p[0];
	SLICE_LEFT();
	switch (o->n)
	{
	case 1:
		WriteMem8(addr, (u8)s[0]);
		break;
	case 2:
		WriteMem16(addr, (u16)s[0]);
		break;
	case 4:
		WriteMem32(addr, s[0]);
		break;
	default:
		WriteMem64(addr, ((u64)s[1] << 32) | s[0]);
		break;
	}
	return NEXT(o);
}

#ifndef NO_MMU
static const pop *op_read_mmu(const pop *o)
{
	const u32 addr = MEM_ADDR(o);
	u32 *d = (u32 *)o->p[0];
	u32 high = 0, low;
	SLICE_LEFT();
	low = read_mmu(addr, o->pc, o->n, &high);
	d[0] = low;
	if (o->n == 8)
		d[1] = high;
	return NEXT(o);
}

static const pop *op_write_mmu(const pop *o)
{
	const u32 *s = (const u32 *)o->p[0];
	SLICE_LEFT();
	write_mmu(MEM_ADDR(o), o->pc, o->n, s[0], o->n == 8 ? s[1] : 0);
	return NEXT(o);
}
#endif

/* The interpreter's handler of the instruction n, the pc first in the
 * context where imm[0] says so (imm[1]) */
static const pop *op_ifb(const pop *o)
{
	if (o->imm[0])
		next_pc = o->imm[1];
	SLICE_LEFT();
	OpDesc[o->n]->oph(o->n);
	return NEXT(o);
}

static const pop *op_ifb_mmu(const pop *o)
{
	sh4_catch c;
	if (o->imm[0])
		next_pc = o->imm[1];
	SLICE_LEFT();
	SH4_TRY(c)
	{
		OpDesc[o->n]->oph(o->n);
		sh4_catch_pop(&c);
	}
	else
		sh4_exception(&c.ex, o->pc);
	return NEXT(o);
}

/* A store queue written out (pref) where p[0] is an address of one */
static const pop *op_pref(const pop *o)
{
	const u32 addr = U32(o->p[0]);
	if ((addr >> 26) != 0x38)
		return NEXT(o);
	SLICE_LEFT();
#ifndef NO_MMU
	if (o->n == 2)
	{
		sh4_catch c;
		SH4_TRY(c)
		{
			do_sqw_mmu(addr);
			sh4_catch_pop(&c);
		}
		else
			sh4_exception(&c.ex, o->pc);
	}
	else
#endif
	if (o->n == 1)
		do_sqw_mmu(addr);
	else
		do_sqw_nommu(addr, sq_both);
	return NEXT(o);
}

/* A call to a canonical function: the kinds of its arguments, and what it
 * returns. The functions are called by the type they are declared with,
 * but for signed arguments, which are given as unsigned: the same register
 * either way. */
enum
{
	SIG_V, SIG_V_I, SIG_V_III, SIG_V_PI, SIG_V_PPP, SIG_V_PPPP,
	SIG_I_I, SIG_I_II, SIG_I_III, SIG_I_F, SIG_I_FF,
	SIG_F_I, SIG_F_F, SIG_F_FF, SIG_F_FFF, SIG_F_PP,
	SIG_L_I, SIG_L_II, SIG_L_III,
	SIG_COUNT
};
static const char *const sig_names[SIG_COUNT] =
{
	"v", "vi", "viii", "vpi", "vppp", "vpppp",
	"ii", "iii", "iiii", "if", "iff",
	"fi", "ff", "fff", "ffff", "fpp",
	"li", "lii", "liii"
};

#define AI(o, i) U32((o)->p[i])
#define AP(o, i) ((o)->p[i])

static f32 af(const pop *o, int i)
{
	f32 v;
	memcpy(&v, o->p[i], sizeof(v));
	return v;
}

/* (where what is returned goes: after the arguments, p[n] and p[n + 1]) */
static void ret_u32(const pop *o, u32 v)
{
	U32(o->p[4]) = v;
}

static void ret_f32(const pop *o, f32 v)
{
	memcpy(o->p[4], &v, sizeof(v));
}

static void ret_u64(const pop *o, u64 v)
{
	U32(o->p[4]) = (u32)v;
	U32(o->p[5]) = (u32)(v >> 32);
}

typedef void (*fn_v)(void);
typedef void (*fn_v_i)(u32);
typedef void (*fn_v_iii)(u32, u32, u32);
typedef void (*fn_v_pi)(void *, u32);
typedef void (*fn_v_ppp)(void *, void *, void *);
typedef void (*fn_v_pppp)(void *, void *, void *, void *);
typedef u32 (*fn_i_i)(u32);
typedef u32 (*fn_i_ii)(u32, u32);
typedef u32 (*fn_i_iii)(u32, u32, u32);
typedef u32 (*fn_i_f)(f32);
typedef u32 (*fn_i_ff)(f32, f32);
typedef f32 (*fn_f_i)(u32);
typedef f32 (*fn_f_f)(f32);
typedef f32 (*fn_f_ff)(f32, f32);
typedef f32 (*fn_f_fff)(f32, f32, f32);
typedef f32 (*fn_f_pp)(void *, void *);
typedef u64 (*fn_l_i)(u32);
typedef u64 (*fn_l_ii)(u32, u32);
typedef u64 (*fn_l_iii)(u32, u32, u32);

/* (a function for each kind, picked as the block is compiled) */
#define CALL_OP(name, body) static const pop *name(const pop *o) { body; return NEXT(o); }
CALL_OP(call_v, ((fn_v)o->call)())
CALL_OP(call_v_i, ((fn_v_i)o->call)(AI(o, 0)))
CALL_OP(call_v_iii, ((fn_v_iii)o->call)(AI(o, 0), AI(o, 1), AI(o, 2)))
CALL_OP(call_v_pi, ((fn_v_pi)o->call)(AP(o, 0), AI(o, 1)))
CALL_OP(call_v_ppp, ((fn_v_ppp)o->call)(AP(o, 0), AP(o, 1), AP(o, 2)))
CALL_OP(call_v_pppp, ((fn_v_pppp)o->call)(AP(o, 0), AP(o, 1), AP(o, 2), AP(o, 3)))
CALL_OP(call_i_i, ret_u32(o, ((fn_i_i)o->call)(AI(o, 0))))
CALL_OP(call_i_ii, ret_u32(o, ((fn_i_ii)o->call)(AI(o, 0), AI(o, 1))))
CALL_OP(call_i_iii, ret_u32(o, ((fn_i_iii)o->call)(AI(o, 0), AI(o, 1), AI(o, 2))))
CALL_OP(call_i_f, ret_u32(o, ((fn_i_f)o->call)(af(o, 0))))
CALL_OP(call_i_ff, ret_u32(o, ((fn_i_ff)o->call)(af(o, 0), af(o, 1))))
CALL_OP(call_f_i, ret_f32(o, ((fn_f_i)o->call)(AI(o, 0))))
CALL_OP(call_f_f, ret_f32(o, ((fn_f_f)o->call)(af(o, 0))))
CALL_OP(call_f_ff, ret_f32(o, ((fn_f_ff)o->call)(af(o, 0), af(o, 1))))
CALL_OP(call_f_fff, ret_f32(o, ((fn_f_fff)o->call)(af(o, 0), af(o, 1), af(o, 2))))
CALL_OP(call_f_pp, ret_f32(o, ((fn_f_pp)o->call)(AP(o, 0), AP(o, 1))))
CALL_OP(call_l_i, ret_u64(o, ((fn_l_i)o->call)(AI(o, 0))))
CALL_OP(call_l_ii, ret_u64(o, ((fn_l_ii)o->call)(AI(o, 0), AI(o, 1))))
CALL_OP(call_l_iii, ret_u64(o, ((fn_l_iii)o->call)(AI(o, 0), AI(o, 1), AI(o, 2))))

static const pop_fn call_ops[SIG_COUNT] =
{
	call_v, call_v_i, call_v_iii, call_v_pi, call_v_ppp, call_v_pppp,
	call_i_i, call_i_ii, call_i_iii, call_i_f, call_i_ff,
	call_f_i, call_f_f, call_f_ff, call_f_fff, call_f_pp,
	call_l_i, call_l_ii, call_l_iii
};

/* The operations done here rather than by a call, as their canonical
 * functions do them: p[0] = p[1] op p[2] */
#define A1 U32(o->p[1])
#define A2 U32(o->p[2])
#define INT_OP(name, expr) static const pop *op_##name(const pop *o) { U32(o->p[0]) = (expr); return NEXT(o); }
INT_OP(add, A1 + A2)
INT_OP(sub, A1 - A2)
INT_OP(and, A1 & A2)
INT_OP(or, A1 | A2)
INT_OP(xor, A1 ^ A2)
INT_OP(not, ~A1)
INT_OP(neg, 0u - A1)
INT_OP(shl, A1 << A2)
INT_OP(shr, A1 >> A2)
INT_OP(sar, (u32)((s32)A1 >> A2))
INT_OP(test, (A1 & A2) == 0)
INT_OP(seteq, A1 == A2)
INT_OP(setge, (s32)A1 >= (s32)A2)
INT_OP(setgt, (s32)A1 > (s32)A2)
INT_OP(setae, A1 >= A2)
INT_OP(setab, A1 > A2)
INT_OP(mul_u16, (u32)(u16)A1 * (u16)A2)
INT_OP(mul_s16, (u32)((s32)(s16)A1 * (s16)A2))
INT_OP(mul_i32, A1 * A2)
INT_OP(ext_s8, (u32)(s32)(s8)A1)
INT_OP(ext_s16, (u32)(s32)(s16)A1)
INT_OP(swaplb, (A1 & 0xFFFF0000u) | ((A1 & 0xFF) << 8) | ((A1 >> 8) & 0xFF))

/* (floats: and a NaN made the SH4's) */
#define FLOAT_OP(name, expr) static const pop *op_##name(const pop *o) \
	{ f32 a, b, v; memcpy(&a, o->p[1], 4); memcpy(&b, o->p[2], 4); v = fixNaN(expr); memcpy(o->p[0], &v, 4); return NEXT(o); }
FLOAT_OP(fadd, a + b)
FLOAT_OP(fsub, a - b)
FLOAT_OP(fmul, a * b)
FLOAT_OP(fdiv, a / b)

/* -- The ends of blocks and their checks ------------------------------- */

/* The block's code as it was compiled (p[0]: where it is, p[1]: a copy of
 * it, n bytes), the block's physical address imm[0] and, with the MMU on,
 * the address it was compiled for imm[1] (n | 0x80000000) */
static const pop *op_check(const pop *o)
{
	const u32 size = o->n & 0x7fffffff;
	if ((o->n & 0x80000000) && next_pc != o->imm[1])
	{
		ngen_blockcheckfail(o->imm[0]);
		return NULL;
	}
	if (memcmp(o->p[0], o->p[1], size) != 0)
	{
		ngen_blockcheckfail(o->imm[0]);
		return NULL;
	}
	return NEXT(o);
}

/* With the MMU on: the block is the one for this address, imm[1] (its
 * physical one, imm[0], to fail with) */
static const pop *op_vaddr(const pop *o)
{
	if (next_pc != o->imm[1])
	{
		ngen_blockcheckfail(o->imm[0]);
		return NULL;
	}
	return NEXT(o);
}

/* With the MMU on, a block with floating-point instructions where they
 * are disabled: the exception, at imm[0] */
static const pop *op_fpu_disabled(const pop *o)
{
	if (sr.FD)
	{
		Do_Exception(o->imm[0], 0x800, 0x100);
		return NULL;
	}
	return NEXT(o);
}

/* The block's cycles (n) taken off where it starts (with the MMU on: it
 * can be left from the middle) */
static const pop *op_charge(const pop *o)
{
	cycle_counter -= (int)o->n;
	return NEXT(o);
}

/* The ends: n is the block's cycles to take off there (0 where op_charge
 * has), imm[0] and imm[1] the block's BranchBlock and NextBlock */
static const pop *end_static(const pop *o)
{
	cycle_counter -= (int)o->n;
	next_pc = o->imm[0];
	return NULL;
}

/* (imm[2]: the value of T, or of jdyn where p[0] is that, to branch on) */
static const pop *end_cond(const pop *o)
{
	cycle_counter -= (int)o->n;
	next_pc = U32(o->p[0]) == o->imm[2] ? o->imm[0] : o->imm[1];
	return NULL;
}

static const pop *end_dynamic(const pop *o)
{
	cycle_counter -= (int)o->n;
	next_pc = Sh4cntx.jdyn;
	return NULL;
}

static const pop *end_intr(const pop *o)
{
	cycle_counter -= (int)o->n;
	next_pc = o->imm[2] ? Sh4cntx.jdyn : o->imm[1];
	UpdateINTC();
	return NULL;
}

/* -- Compiling --------------------------------------------------------- */

/* The block being compiled: its records written from at on, up to end */
static pop *at, *end;
/* The call being put together (ngen_CC_*()), its arguments' kinds */
static pop call;
static int call_args, call_rets;
static char call_kinds[8];

static pop *new_op(pop_fn fn)
{
	pop *o = at;
	if (o >= end)
		die("portable recompiler: block too large");
	memset(o, 0, sizeof(*o));
	o->fn = fn;
	at++;
	return o;
}

/* Where an operand is: its register in the context, or the constant, in
 * the record's imm[i] (0 for none) */
static void *operand(pop *o, const shil_param *prm, int i)
{
	if (shil_is_reg(prm))
		return shil_reg_ptr(prm);
	o->imm[i] = shil_is_imm(prm) ? prm->_imm : 0;
	return &o->imm[i];
}

static void compile_op(shil_opcode *op, RuntimeBlockInfo *block)
{
	pop *o;
	switch (op->op)
	{
	case shop_mov32:
		o = new_op(op_mov32);
		o->p[0] = shil_reg_ptr(&op->rd);
		o->p[1] = operand(o, &op->rs1, 0);
		break;

	case shop_mov64:
		o = new_op(op_mov64);
		o->p[0] = shil_reg_ptr(&op->rd);
		o->p[1] = shil_reg_ptr(&op->rs1);
		break;

	case shop_jcond:
	case shop_jdyn:
		o = new_op(op_add3);
		o->p[0] = shil_reg_ptr(&op->rd);
		o->p[1] = operand(o, &op->rs1, 0);
		o->p[2] = operand(o, &op->rs2, 1);
		break;

	case shop_readm:
	case shop_writem:
		{
			const bool read = op->op == shop_readm;
#ifndef NO_MMU
			if (mmu_enabled())
				o = new_op(read ? op_read_mmu : op_write_mmu);
			else
#endif
				o = new_op(read ? op_read : op_write);
			o->n = op->flags & 0x7f;
			o->pc = op_pc(block, op);
			o->p[0] = read ? shil_reg_ptr(&op->rd) : operand(o, &op->rs2, 2);
			o->p[1] = operand(o, &op->rs1, 0);
			o->p[2] = operand(o, &op->rs3, 1);
		}
		break;

	case shop_ifb:
		o = new_op(mmu_enabled() ? op_ifb_mmu : op_ifb);
		o->n = op->rs3._imm;
		o->imm[0] = op->rs1._imm;
		o->imm[1] = op->rs2._imm;
		o->pc = op_pc(block, op);
		break;

	case shop_pref:
		o = new_op(op_pref);
		o->n = mmu_enabled() ? 2 : CCN_MMUCR.AT ? 1 : 0;
		o->pc = op_pc(block, op);
		o->p[0] = operand(o, &op->rs1, 0);
		break;

#define NATIVE(name, fn) case shop_##name: o = new_op(fn); o->p[0] = shil_reg_ptr(&op->rd); \
		o->p[1] = operand(o, &op->rs1, 0); o->p[2] = operand(o, &op->rs2, 1); break;
	NATIVE(add, op_add)
	NATIVE(sub, op_sub)
	NATIVE(and, op_and)
	NATIVE(or, op_or)
	NATIVE(xor, op_xor)
	NATIVE(not, op_not)
	NATIVE(neg, op_neg)
	NATIVE(shl, op_shl)
	NATIVE(shr, op_shr)
	NATIVE(sar, op_sar)
	NATIVE(test, op_test)
	NATIVE(seteq, op_seteq)
	NATIVE(setge, op_setge)
	NATIVE(setgt, op_setgt)
	NATIVE(setae, op_setae)
	NATIVE(setab, op_setab)
	NATIVE(mul_u16, op_mul_u16)
	NATIVE(mul_s16, op_mul_s16)
	NATIVE(mul_i32, op_mul_i32)
	NATIVE(ext_s8, op_ext_s8)
	NATIVE(ext_s16, op_ext_s16)
	NATIVE(swaplb, op_swaplb)
	NATIVE(fadd, op_fadd)
	NATIVE(fsub, op_fsub)
	NATIVE(fmul, op_fmul)
	NATIVE(fdiv, op_fdiv)
#undef NATIVE

	default:
		shil_chf[op->op](op);
		break;
	}
}

void ngen_CC_Start(shil_opcode *op)
{
	(void)op;
	memset(&call, 0, sizeof(call));
	call_args = 0;
	call_rets = 0;
}

void ngen_CC_Param(shil_opcode *op, shil_param *prm, CanonicalParamType tp)
{
	(void)op;
	switch (tp)
	{
	case CPT_u32:
	case CPT_f32:
	case CPT_ptr:
		/* (in the order the function's arguments are, from the last) */
		if (call_args == 4)
			die("portable recompiler: a call of more than four arguments");
		call_kinds[call_args] = tp == CPT_u32 ? 'i' : tp == CPT_f32 ? 'f' : 'p';
		if (tp == CPT_ptr)
			call.p[call_args] = shil_reg_ptr(prm);
		else
			call.p[call_args] = operand(&call, prm, call_args);
		call_args++;
		break;

	case CPT_u32rv:
	case CPT_u64rvL:
	case CPT_f32rv:
		call.p[4] = shil_reg_ptr(prm);
		call_kinds[7] = tp == CPT_u32rv ? 'i' : tp == CPT_f32rv ? 'f' : 'l';
		call_rets = 1;
		break;

	case CPT_u64rvH:
		call.p[5] = shil_reg_ptr(prm);
		break;
	}
}

void ngen_CC_Call(shil_opcode *op, void *function)
{
	(void)op;
	/* (a data pointer to a function: what the canonical interface has) */
	memcpy(&call.call, &function, sizeof(call.call));
}

void ngen_CC_Finish(shil_opcode *op)
{
	char name[8];
	int i, sig;
	pop *o;

	/* the arguments the way round they go */
	name[0] = call_rets ? call_kinds[7] : 'v';
	for (i = 0; i < call_args; i++)
		name[1 + i] = call_kinds[call_args - 1 - i];
	name[1 + call_args] = 0;
	for (sig = 0; sig < SIG_COUNT; sig++)
		if (strcmp(name, sig_names[sig]) == 0)
			break;
	if (sig == SIG_COUNT)
	{
		ERROR_LOG(DYNAREC, "portable recompiler: shil op %s calls a function of the kind %s\n",
				shil_opcode_name(op->op), name);
		die("portable recompiler: a canonical function of an unknown kind");
	}

	o = new_op(call_ops[sig]);
	o->call = call.call;
	o->p[4] = call.p[4];
	o->p[5] = call.p[5];
	for (i = 0; i < call_args; i++)
	{
		const int from = call_args - 1 - i;
		/* (a constant is in the record: copied, and pointed to there) */
		if (call.p[from] == &call.imm[from])
		{
			o->imm[i] = call.imm[from];
			o->p[i] = &o->imm[i];
		}
		else
			o->p[i] = call.p[from];
	}
}

void ngen_Compile(RuntimeBlockInfo *block, bool force_checks, bool reset, bool staging, bool optimise)
{
	u8 *start = (u8 *)emit_GetCCPtr();
	u8 *base = (u8 *)(((uintptr_t)start + 15) & ~(uintptr_t)15);
	const bool mmu = mmu_enabled();
	pop *check = NULL;
	pop *o;
	u32 i;
	(void)reset;
	(void)staging;
	(void)optimise;

	verify(emit_FreeSpace() >= CODE_MARGIN);
	at = (pop *)base;
	end = (pop *)(start + emit_FreeSpace() - CODE_MARGIN / 2);

	if (force_checks)
	{
		check = new_op(op_check);
		check->imm[0] = block->addr;
		check->imm[1] = block->vaddr;
		check->n = block->sh4_code_size | (mmu ? 0x80000000 : 0);
		check->p[0] = GetMemPtr(block->addr, block->sh4_code_size);
		if (check->p[0] == NULL)
		{
			at--;
			check = NULL;
		}
	}
	else if (mmu)
	{
		o = new_op(op_vaddr);
		o->imm[0] = block->addr;
		o->imm[1] = block->vaddr;
	}
	if (mmu && block->has_fpu_op)
	{
		o = new_op(op_fpu_disabled);
		o->imm[0] = block->vaddr;
	}
	if (mmu)
	{
		o = new_op(op_charge);
		o->n = block->guest_cycles;
	}

	for (i = 0; i < block->op_count; i++)
		compile_op(&block->oplist[i], block);

	switch (block->BlockType)
	{
	case BET_StaticJump:
	case BET_StaticCall:
		o = new_op(end_static);
		break;
	case BET_Cond_0:
	case BET_Cond_1:
		o = new_op(end_cond);
		o->p[0] = block->has_jcond ? (void *)&Sh4cntx.jdyn : (void *)&sr.T;
		o->imm[2] = block->BlockType & 1;
		break;
	case BET_DynamicJump:
	case BET_DynamicCall:
	case BET_DynamicRet:
		o = new_op(end_dynamic);
		break;
	case BET_DynamicIntr:
	case BET_StaticIntr:
		o = new_op(end_intr);
		o->imm[2] = block->BlockType == BET_DynamicIntr;
		break;
	default:
		die("Invalid block end type");
		return;
	}
	o->n = mmu ? 0 : block->guest_cycles;
	o->imm[0] = block->BranchBlock;
	o->imm[1] = block->NextBlock;

	/* the copy of the code a check compares with, after the records */
	if (check != NULL)
	{
		u8 *copy = (u8 *)at;
		const u32 size = check->n & 0x7fffffff;
		if (copy + size > (u8 *)end)
			die("portable recompiler: block too large");
		memcpy(copy, check->p[0], size);
		check->p[1] = copy;
		at = (pop *)(((uintptr_t)copy + size + 15) & ~(uintptr_t)15);
	}

	block->code = (DynarecCodeEntryPtr)(uintptr_t)base;
	block->host_code_size = (u32)((u8 *)at - start);
	block->host_opcodes = (u32)(((u8 *)at - base) / sizeof(pop));
	emit_Skip(block->host_code_size);
}

/* -- The main loop ----------------------------------------------------- */

static void run_block(const pop *o)
{
	do
		o = o->fn(o);
	while (o != NULL);
}

void ngen_mainloop(void *cntx)
{
	(void)cntx;
	cycle_counter = SH4_TIMESLICE;
	/* (here at the start, and again after an exception) */
	setjmp(jmp_env);
	while (Sh4cntx.CpuRunning)
	{
		do
		{
			const DynarecCodeEntryPtr code = bm_GetCodeByVAddr(next_pc);
			if (code == ngen_FailedToFindBlock)
				rdv_FailedToFindBlock(next_pc);
			else
				run_block((const pop *)(uintptr_t)code);
		} while (cycle_counter > 0);

		cycle_counter += SH4_TIMESLICE;
		/* UpdateSystem_INTC(), only where an event is due or an interrupt
		 * waits: otherwise the slice is taken off the time to the next
		 * event, and that is all it would have done */
		if (Sh4cntx.sh4_sched_next - SH4_TIMESLICE >= 0 && !Sh4cntx.interrupt_pend)
			Sh4cntx.sh4_sched_next -= SH4_TIMESLICE;
		else
		{
			/* (the timers' time is the end of the slice here: see
			 * cycle_counter in sh4_if.h) */
			Sh4cntx.cycle_counter = SH4_TIMESLICE;
			UpdateSystem_INTC();
		}
	}
}
#else
/* (for the other processors: C has no empty file) */
typedef int rec_portable_not_here;
#endif
