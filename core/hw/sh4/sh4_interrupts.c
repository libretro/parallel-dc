/*
	Interrupt list caching and handling

	SH4 has a very flexible interrupt controller. In order to handle it efficiently, a sorted
	interrupt bitfield is build from the set interrupt priorities. Higher priorities get allocated
	into higher bits, and a simple mask is kept. In order to check for pending interrupts a simple
	!=0 test works, and to identify the pending interrupt bsr(pend) will give the sorted id. As
	this is a single cycle operation on most platforms, the interrupt checking/identification
	is very fast !
*/

#include "types.h"
#include "sh4_interrupts.h"
#include "sh4_core.h"
#include "sh4_mmr.h"
#include "oslib/oslib.h"

/*

*/

/* Where each source's priority is: the IRL lines' are fixed, the others'
 * are 4 bits of INTC's IPRA, IPRB or IPRC */
static const u16 IRLPriority = 0x0246;
enum { PRIO_IRL, PRIO_IPRA, PRIO_IPRB, PRIO_IPRC };

typedef struct InterptSourceList_Entry
{
	const u16* PrioReg;
	u32 Shift;
	u32 IntEvnCode;
} InterptSourceList_Entry;

static u32 GetPrLvl(const InterptSourceList_Entry *e)
{
	return ((*e->PrioReg)>>e->Shift)&0xF;
}

/* The sources, in the order of their ids (see sh4_interrupts.h): their
 * priority's register and the place in it, and their event code */
static const struct
{
	u8 reg;
	u8 shift;
	u16 code;
} InterruptSources[28] =
{
	/* IRL */
	{ PRIO_IRL, 0, 0x320 },	/* sh4_IRL_9 */
	{ PRIO_IRL, 4, 0x360 },	/* sh4_IRL_11 */
	{ PRIO_IRL, 8, 0x3A0 },	/* sh4_IRL_13 */
	/* HUDI */
	{ PRIO_IPRC, 0, 0x600 },	/* sh4_HUDI_HUDI: H-UDI underflow */
	/* GPIO (missing on dc ?) */
	{ PRIO_IPRC, 12, 0x620 },	/* sh4_GPIO_GPIOI */
	/* DMAC */
	{ PRIO_IPRC, 8, 0x640 },	/* sh4_DMAC_DMTE0 */
	{ PRIO_IPRC, 8, 0x660 },	/* sh4_DMAC_DMTE1 */
	{ PRIO_IPRC, 8, 0x680 },	/* sh4_DMAC_DMTE2 */
	{ PRIO_IPRC, 8, 0x6A0 },	/* sh4_DMAC_DMTE3 */
	{ PRIO_IPRC, 8, 0x6C0 },	/* sh4_DMAC_DMAE */
	/* TMU */
	{ PRIO_IPRA, 12, 0x400 },	/* sh4_TMU0_TUNI0: TMU0 underflow */
	{ PRIO_IPRA, 8, 0x420 },	/* sh4_TMU1_TUNI1: TMU1 underflow */
	{ PRIO_IPRA, 4, 0x440 },	/* sh4_TMU2_TUNI2: TMU2 underflow */
	{ PRIO_IPRA, 4, 0x460 },	/* sh4_TMU2_TICPI2 */
	/* RTC */
	{ PRIO_IPRA, 0, 0x480 },	/* sh4_RTC_ATI */
	{ PRIO_IPRA, 0, 0x4A0 },	/* sh4_RTC_PRI */
	{ PRIO_IPRA, 0, 0x4C0 },	/* sh4_RTC_CUI */
	/* SCI */
	{ PRIO_IPRB, 4, 0x4E0 },	/* sh4_SCI1_ERI */
	{ PRIO_IPRB, 4, 0x500 },	/* sh4_SCI1_RXI */
	{ PRIO_IPRB, 4, 0x520 },	/* sh4_SCI1_TXI */
	{ PRIO_IPRB, 4, 0x540 },	/* sh4_SCI1_TEI */
	/* SCIF */
	{ PRIO_IPRC, 4, 0x700 },	/* sh4_SCIF_ERI */
	{ PRIO_IPRC, 4, 0x720 },	/* sh4_SCIF_RXI */
	{ PRIO_IPRC, 4, 0x740 },	/* sh4_SCIF_BRI */
	{ PRIO_IPRC, 4, 0x760 },	/* sh4_SCIF_TXI */
	/* WDT */
	{ PRIO_IPRB, 12, 0x560 },	/* sh4_WDT_ITI */
	/* REF */
	{ PRIO_IPRB, 8, 0x580 },	/* sh4_REF_RCMI */
	{ PRIO_IPRB, 8, 0x5A0 }	/* sh4_REF_ROVI */
};

/* The sources with where their priorities are, made by interrupts_init() */
static InterptSourceList_Entry InterruptSourceList[28];

/* Maps siid -> EventID */
DECL_ALIGN(64) u16 InterruptEnvId[32] = { 0 };
/* Maps piid -> 1<<siid */
DECL_ALIGN(64) u32 InterruptBit[32] = { 0 };
/* Maps sh4 interrupt level to inclusive bitfield */
DECL_ALIGN(64) u32 InterruptLevelBit[16] = { 0 };

static bool Do_Interrupt(u32 intEvn);

u32 interrupt_vpend; /* Vector of pending interrupts */
u32 interrupt_vmask; /* Vector of masked interrupts             (-1 inhibits all interrupts) */
u32 decoded_srimask; /* Vector of interrupts allowed by SR.IMSK (-1 inhibits all interrupts) */

/* bit 0 ~ 27 : interrupt source 27:0. 0 = lowest level, 27 = highest level. */
static void recalc_pending_itrs(void)
{
	Sh4cntx.interrupt_pend=interrupt_vpend&interrupt_vmask&decoded_srimask;
}

/* Rebuild sorted interrupt id table (priorities were updated) */
void SIIDRebuild(void)
{
	u32 cnt=0;
	u32 vpend=interrupt_vpend;
	u32 vmask=interrupt_vmask;
	u32 ilevel, isrc;
	interrupt_vpend=0;
	interrupt_vmask=0x00000000;
	/* rebuild interrupt table */
	for (ilevel=0;ilevel<16;ilevel++)
	{
	   /* Among sources of one level the first in the list of sources
	    * (in interrupts_init) comes first - IRL before the on-chip ones,
	    * a DMAC channel before the timers, timer 0 before timer 1: so it
	    * is given the highest bit of the level, and the list is gone
	    * through from its end. (It was gone through from its start, and
	    * the order within a level was the reverse of the processor's.) */
	   for (isrc=28;isrc-->0;)
       {
	      if (GetPrLvl(&InterruptSourceList[isrc])==ilevel)
	      {
             u32 p=InterruptBit[isrc]&vpend;
             u32 m=InterruptBit[isrc]&vmask;
             InterruptEnvId[cnt]=(u16)InterruptSourceList[isrc].IntEvnCode;
             InterruptBit[isrc]=1<<cnt;
             if (p)
                interrupt_vpend|=InterruptBit[isrc];
             if (m)
                interrupt_vmask|=InterruptBit[isrc];
             cnt++;
          }
      }
      InterruptLevelBit[ilevel]=(1<<cnt)-1;
	}

	SRdecode();
}

/* Decode SR.IMSK into a interrupt mask, update and return the interrupt state */
bool SRdecode(void)
{
   decoded_srimask = (sr.BL) ? ~0xFFFFFFFF : ~InterruptLevelBit[sr.IMASK];

	recalc_pending_itrs();
	return Sh4cntx.interrupt_pend;
}

int UpdateINTC(void)
{
	if (Sh4cntx.interrupt_pend)
      return Do_Interrupt(InterruptEnvId[bitscanrev(Sh4cntx.interrupt_pend)]);
   return 0;
}

void SetInterruptPend(InterruptID intr)
{
	u32 piid= intr & InterruptPIIDMask;
	interrupt_vpend|=InterruptBit[piid];
	recalc_pending_itrs();
}
void ResetInterruptPend(InterruptID intr)
{
	u32 piid= intr & InterruptPIIDMask;
	interrupt_vpend&=~InterruptBit[piid];
	recalc_pending_itrs();
}

void SetInterruptMask(InterruptID intr)
{
	u32 piid= intr & InterruptPIIDMask;
	interrupt_vmask|=InterruptBit[piid];
	recalc_pending_itrs();
}
void ResetInterruptMask(InterruptID intr)
{
	u32 piid= intr & InterruptPIIDMask;
	interrupt_vmask&=~InterruptBit[piid];
	recalc_pending_itrs();
}


static bool Do_Interrupt(u32 intEvn)
{
	CCN_INTEVT = intEvn;

	ssr = sh4_sr_GetFull();
	spc = next_pc;
	sgr = r[15];
	sr.BL = 1;
	sr.MD = 1;
	sr.RB = 1;
	UpdateSR();
	next_pc = vbr + 0x600;

	return true;
}

bool Do_Exception(u32 epc, u32 expEvn, u32 CallVect)
{
	/* The processor does not take an exception while they are blocked: it
	 * resets. That is not done here, and the exception is taken as if they
	 * were not, which is what this has always done; but it is said. (This
	 * was a check that either stopped the emulator or was compiled out.) */
	if (sr.BL != 0)
		ERROR_LOG(SH4, "Exception %03X at %08X with exceptions blocked (SR.BL set): the SH4 would reset\n", expEvn, epc);
	CCN_EXPEVT = expEvn;

	ssr = sh4_sr_GetFull();
	spc = epc;
	sgr = r[15];
	sr.BL = 1;
	sr.MD = 1;
	sr.RB = 1;
	UpdateSR();

	next_pc = vbr + CallVect;
	/* printf("RaiseException: from %08X , pc errh %08X, %08X vect\n", spc, epc, next_pc); */

	return true;
}

/* Init/Res/Term */
void interrupts_init(void)
{
	u32 i;

	for (i = 0; i < 28; i++)
	{
		const u16 *reg;

		switch (InterruptSources[i].reg)
		{
			case PRIO_IRL:  reg = &IRLPriority; break;
			case PRIO_IPRA: reg = &INTC_IPRA.reg_data; break;
			case PRIO_IPRB: reg = &INTC_IPRB.reg_data; break;
			default:        reg = &INTC_IPRC.reg_data; break;
		}
		InterruptSourceList[i].PrioReg = reg;
		InterruptSourceList[i].Shift = InterruptSources[i].shift;
		InterruptSourceList[i].IntEvnCode = InterruptSources[i].code;
	}
}

void interrupts_reset(void)
{
	/* reset interrupts cache */
	interrupt_vpend=0x00000000;
	interrupt_vmask=0xFFFFFFFF;
	decoded_srimask=0;

	{
		u32 i;
		for (i=0;i<28;i++)
			InterruptBit[i]=1<<i;
	}

	/* rebuild the interrupts table */
	SIIDRebuild();
}

void interrupts_term(void)
{

}
