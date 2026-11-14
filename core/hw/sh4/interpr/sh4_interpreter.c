/*
	Highly inefficient and boring interpreter. Nothing special here
*/

#include "types.h"
#include "hw/sh4/sh4_cycles.h"
#include "hw/sh4/modules/mmu.h"

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "../sh4_sched.h"
#include "hw/holly/sb.h"
#include "../sh4_cache.h"

#define CPU_RATIO      (8)

/* The cycles of what is being run, for the accurate setting */
static Sh4Cycles timing = SH4_CYCLES_INIT;

static void ExecuteOpcode(u16 op)
{
	if (sr.FD == 1 && (OpDesc[op]->type & UsesFPU) != 0)
		RaiseFPUDisableException();
	OpPtr[op](op);
	if (settings.dynarec.AccurateTiming)
		Sh4cntx.cycle_counter -= sh4_cycles_count(&timing, op, mmu_enabled() ? 5 : 2);
	else
		Sh4cntx.cycle_counter -= CPU_RATIO;
}

static u16 ReadNexOp(void)
{
	u32 addr = next_pc;
	next_pc += 2;

	return IReadMem16(addr);
}

void Sh4_int_Run(void)
{
   sh4_int_bCpuRun = true;
	HostFloatModeSave();
	RestoreHostRoundingMode();

	Sh4cntx.cycle_counter = SH4_TIMESLICE;
	/* (nothing left of a run that ended in the middle of one) */
	sh4_catch_top = NULL;
	sh4_in_delay_slot = false;

   do
   {
#if !defined(NO_MMU)
      sh4_catch c;
      SH4_TRY(c)
      {
#endif
         do
         {
			u32 op = ReadNexOp();

            ExecuteOpcode(op);
         } while (Sh4cntx.cycle_counter > 0);
         Sh4cntx.cycle_counter += SH4_TIMESLICE;
         sh4_cycles_reset(&timing);
         UpdateSystem_INTC();
#if !defined(NO_MMU)
         sh4_catch_pop(&c);
      }
      else
      {
         Do_Exception(c.ex.epc, c.ex.expEvn, c.ex.callVect);
         Sh4cntx.cycle_counter -= CPU_RATIO * 5;	/* an exception requires the instruction pipeline to drain, so approx 5 cycles */
      }
#endif
   } while(sh4_int_bCpuRun);
   
   sh4_int_bCpuRun=false;
	HostFloatModeBack();
}

void Sh4_int_Stop(void)
{
	if (sh4_int_bCpuRun)
	{
		sh4_int_bCpuRun=false;
		/* and the time slice ends with the instruction being executed */
		Sh4cntx.cycle_counter = 0;
	}
}

void Sh4_int_Start(void)
{
	if (!sh4_int_bCpuRun)
		sh4_int_bCpuRun=true;
}


void Sh4_int_Step(void)
{
	if (sh4_int_bCpuRun)
	{
		WARN_LOG(INTERPRETER, "Sh4 Is running , can't step\n");
	}
	else
	{
		u32 op = ReadNexOp();
		ExecuteOpcode(op);
	}
}

void Sh4_int_Skip(void)
{
	if (!sh4_int_bCpuRun)
		next_pc+=2;
}

void Sh4_int_Reset(bool hard)
{
	if (sh4_int_bCpuRun)
		return;

	if (hard)
	{
		/* All but how long it is to the scheduler's next event: the
		 * scheduler's clock is counted from that, and what is queued -
		 * some of it by the devices, reset just before - is timed by the
		 * clock. Cleared, the clock jumped ahead by as much and all of it
		 * came early. */
		int sched_next = Sh4cntx.sh4_sched_next;

		memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));
		Sh4cntx.sh4_sched_next = sched_next;
		/* (none of the slice gone, for a recompiler that never writes it) */
		Sh4cntx.cycle_counter = SH4_TIMESLICE;
	}
	next_pc = 0xA0000000;

	memset(r,0,sizeof(r));
	memset(r_bank,0,sizeof(r_bank));

	gbr=ssr=spc=sgr=dbr=vbr=0;
	mac.full=pr=fpul=0;

	sh4_sr_SetFull(0x700000F0);
	old_sr.status=sr.status;
	UpdateSR();

	fpscr.full = 0x00040001;
	/* (Not the host's rounding mode with it, as UpdateFPSCR() would: this
	 * is whoever asked for the reset's thread, which with threaded
	 * rendering never runs the SH4 and kept the mode for good. Run() sets
	 * it each time it is entered.) */
	old_fpscr=fpscr;
	sh4_caches_reset(hard);

	/* Any more registers have default value ? */
	INFO_LOG(INTERPRETER, "Sh4 Reset\n");
}

bool Sh4_int_IsCpuRunning(void)
{
	return sh4_int_bCpuRun;
}

/* TODO : Check for valid delayslot instruction */
void ExecuteDelayslot(void)
{
   u32 op;
#if !defined(NO_MMU)
   /* what it raises is the slot's exception (sh4_throw()) */
   sh4_in_delay_slot = true;
#endif
   op = ReadNexOp();

   ExecuteOpcode(op);
#if !defined(NO_MMU)
   sh4_in_delay_slot = false;
#endif
}

void ExecuteDelayslot_RTE(void)
{
	/* In an RTE delay slot, status register (SR) bits are referenced as follows. */
	/* In instruction access, the MD bit is used before modification, and in data access, */
	/* the MD bit is accessed after modification. */
	/* The other bits - S, T, M, Q, FD, BL, and RB - after modification are used for delay slot */
	/* instruction execution. The STC and STC.L SR instructions access all SR bits after modification. */
	volatile bool fetched = false;
#if !defined(NO_MMU)
   sh4_catch c;
   SH4_TRY(c)
   {
#endif
      u32 op = ReadNexOp();

      /* Fetched as RTE was, in privileged mode. Now SR is as it was saved,
       * and the registers are those of the bank it names: the instruction
       * in the slot is executed with them. (The banks used to be switched
       * after it, by the caller: it read and wrote R0 to R7 of the handler's
       * bank, and what it wrote was lost to the program returned to.) */
      fetched = true;
      sh4_sr_SetFull(ssr);
      UpdateSR();
      ExecuteOpcode(op);
#if !defined(NO_MMU)
      sh4_catch_pop(&c);
   }
   else
   {
      ERROR_LOG(INTERPRETER, "Exception in RTE delay slot\n");
      /* the return is made all the same: with SR as it was saved */
      if (!fetched)
      {
         sh4_sr_SetFull(ssr);
         UpdateSR();
      }
   }
#endif
}

/* every SH4_TIMESLICE cycles */
int UpdateSystem(void)
{
	Sh4cntx.sh4_sched_next -= SH4_TIMESLICE;
	if (Sh4cntx.sh4_sched_next<0)
		sh4_sched_tick(SH4_TIMESLICE);

	return Sh4cntx.interrupt_pend;
}

int UpdateSystem_INTC(void)
{
	if (UpdateSystem())
		return UpdateINTC();
	else
		return 0;
}

static void sh4_int_resetcache(void)
{
}

/* Get an interface to sh4 interpreter */
void Get_Sh4Interpreter(sh4_if *cpu)
{
	cpu->Run = Sh4_int_Run;
	cpu->Stop = Sh4_int_Stop;
	cpu->Start = Sh4_int_Start;
	cpu->Step = Sh4_int_Step;
	cpu->Skip = Sh4_int_Skip;
	cpu->Reset = Sh4_int_Reset;
	cpu->Init = Sh4_int_Init;
	cpu->Term = Sh4_int_Term;
	cpu->IsCpuRunning = Sh4_int_IsCpuRunning;

	cpu->ResetCache = sh4_int_resetcache;
}

void Sh4_int_Init(void)
{
	/* (the recompilers' offsets into it) */
	typedef char sh4cntx_is_448_bytes[sizeof(Sh4cntx) == 448 ? 1 : -1];

	memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));
}

void Sh4_int_Term(void)
{
	Sh4_int_Stop();
	INFO_LOG(INTERPRETER, "Sh4 Term\n");
}
