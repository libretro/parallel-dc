#pragma once
#include <setjmp.h>
#include "types.h"
#include "sh4_if.h"

#ifdef __cplusplus
extern "C" {
#endif


#define r Sh4cntx.r
#define r_bank Sh4cntx.r_bank
#define gbr Sh4cntx.gbr
#define ssr Sh4cntx.ssr
#define spc Sh4cntx.spc
#define sgr Sh4cntx.sgr
#define dbr Sh4cntx.dbr
#define vbr Sh4cntx.vbr
#define mac Sh4cntx.mac
#define pr Sh4cntx.pr
#define fpul Sh4cntx.fpul
#define next_pc Sh4cntx.pc
#define curr_pc (next_pc-2)
#define sr Sh4cntx.sr
#define fpscr Sh4cntx.fpscr
#define old_sr Sh4cntx.old_sr
#define old_fpscr Sh4cntx.old_fpscr
#define fr (&Sh4cntx.xffr[16])
#define xf Sh4cntx.xffr
#define fr_hex ((u32*)fr)
#define xf_hex ((u32*)xf)
#define dr_hex ((u64*)fr)
#define xd_hex ((u64*)xf)
#define sh4_int_bCpuRun Sh4cntx.CpuRunning



void UpdateFPSCR(void);
bool UpdateSR(void);

typedef union DoubleReg
{
	double dbl;
	f32 sgl[2];
} DoubleReg;

static TYPES_INLINE double GetDR(u32 n)
{
	DoubleReg t;

	t.sgl[1]=fr[(n<<1) + 0];
	t.sgl[0]=fr[(n<<1) + 1];

	return t.dbl;
}

static TYPES_INLINE double GetXD(u32 n)
{
	DoubleReg t;

	t.sgl[1]=xf[(n<<1) + 0];
	t.sgl[0]=xf[(n<<1) + 1];

	return t.dbl;
}

static TYPES_INLINE void SetDR(u32 n, double val)
{
	DoubleReg t;
	t.dbl=val;


	fr[(n<<1) | 1]=t.sgl[0];
	fr[(n<<1) | 0]=t.sgl[1];
}

static TYPES_INLINE void SetXD(u32 n, double val)
{
	DoubleReg t;
	t.dbl=val;

	xf[(n<<1) | 1]=t.sgl[0];
	xf[(n<<1) | 0]=t.sgl[1];
}

#include "sh4_interrupts.h"	/* (Do_Exception) */

/* The MMU done the strict way (modules/mmu.h) */
#ifndef NO_MMU
extern bool mmu_strict;
#endif

/* The SH4's exceptions, raised in the middle of an instruction (an MMU
 * fault, an instruction it may not run): sh4_throw() goes back to the
 * innermost sh4_catch, by longjmp(), with what was raised in its ex.
 *
 *    sh4_catch c;
 *    SH4_TRY(c)
 *    {
 *       ...what may raise one...
 *       sh4_catch_pop(&c);		(on every way out but the exception)
 *    }
 *    else
 *       ...c.ex...
 *
 * A local the try changes and the else reads has to be volatile. Only the
 * SH4's thread raises and catches them. */
typedef struct SH4ThrownException SH4ThrownException;
struct SH4ThrownException
{
	u32 epc;
	u32 expEvn;
	u32 callVect;
};

#if defined(__APPLE__) || defined(__MACH__)
/* (not the signal mask, which setjmp() saves here, by a system call) */
#define SH4_SETJMP(env) _setjmp(env)
#define SH4_LONGJMP(env) _longjmp(env, 1)
#else
#define SH4_SETJMP(env) setjmp(env)
#define SH4_LONGJMP(env) longjmp(env, 1)
#endif

typedef struct sh4_catch sh4_catch;
struct sh4_catch
{
	jmp_buf env;
	sh4_catch *prev;
	bool in_delay_slot;		/* made by what the slot's instruction runs */
	SH4ThrownException ex;
};

extern sh4_catch *sh4_catch_top;
/* An instruction in a delay slot is being run (ExecuteDelayslot()): what it
 * raises, and is caught outside the slot, is the slot's exception
 * (AdjustDelaySlotException()) */
extern bool sh4_in_delay_slot;

static TYPES_INLINE void sh4_catch_push(sh4_catch *c)
{
	c->prev = sh4_catch_top;
	c->in_delay_slot = sh4_in_delay_slot;
	sh4_catch_top = c;
}

static TYPES_INLINE void sh4_catch_pop(sh4_catch *c)
{
	sh4_catch_top = c->prev;
}

#define SH4_TRY(c) sh4_catch_push(&(c)); if (SH4_SETJMP((c).env) == 0)

/* Raises the exception: does not come back */
void sh4_throw(u32 epc, u32 expEvn, u32 callVect);

static TYPES_INLINE void AdjustDelaySlotException(SH4ThrownException *ex)
{
	ex->epc -= 2;
	if (ex->expEvn == 0x800)	/* FPU disable exception */
		ex->expEvn = 0x820;	/* Slot FPU disable exception */
	else if (ex->expEvn == 0x180)	/* Illegal instruction exception */
		ex->expEvn = 0x1A0;			/* Slot illegal instruction exception */
}

static TYPES_INLINE void RaiseFPUDisableException(void)
{
#if !defined(NO_MMU)
	if (settings.dreamcast.FullMMU || mmu_strict)
		sh4_throw(next_pc - 2, 0x800, 0x100);
#else
	msgboxf("Full MMU support needed", MBX_ICONERROR);
#endif
}

/* The SH4 sets the signaling bit to 0 for qNaN (unlike all recent CPUs). Some games relies on this. */
/* Otherwise a NaN is the one NaN an ARM host gives, its default NaN, which
 * the SH4's thread asks for there (sh4_core_regs.cpp): the same on every
 * host, as what a game works out has to be. An x86 one gives a NaN made
 * from an operand's, or one with the sign set, and comes here to be given
 * that one; an ARM one has nothing to do. (The x86-64 recompiler does the
 * same in its code.) */
static TYPES_INLINE float fixNaN(f32 f)
{
#ifdef STRICT_MODE
	u32 *hex = (u32 *)&f;
	/* no fast-math */
	if (f != f)
		*hex = 0x7fbfffff;
/* 	// fast-math */
/* 	if ((hex & 0x7fffffff) > 0x7f800000) */
/* 		hex = 0x7fbfffff; */
#elif HOST_CPU == CPU_X86 || HOST_CPU == CPU_X64
	if (f != f)
	{
		const u32 nan = 0x7fc00000;
		memcpy(&f, &nan, sizeof(f));
	}
#endif
	return f;
}

static TYPES_INLINE f64 fixNaN64(f64 f)
{
#ifdef STRICT_MODE
	/* no fast-math */
	u64 *hex = (u64 *)&f;
	if (f != f)
		*hex = 0x7ff7ffffffffffffll;
	/* fast-math */
/* 	return (*(u64 *)&f & 0x7fffffffffffffffll) <= 0x7f80000000000000ll ? f : 0x7ff7ffffffffffffll; */
#elif HOST_CPU == CPU_X86 || HOST_CPU == CPU_X64
	if (f != f)
	{
		const u64 nan = 0x7ff8000000000000ull;
		memcpy(&f, &nan, sizeof(f));
	}
#endif
	return f;
 }

#ifdef __cplusplus
}
#endif
