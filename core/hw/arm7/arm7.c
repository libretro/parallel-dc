#include <string.h>
#include "arm7.h"
#include "hw/mem/_vmem.h"

#include "arm7_interp.h"

DECL_ALIGN(8) reg_pair arm_Reg[RN_ARM_REG_SLOTS];

void CPUSwap(u32 *a, u32 *b)
{
	u32 c = *b;
	*b = *a;
	*a = c;
}

bool armIrqEnable;
bool armFiqEnable;
int armMode;

bool Arm7Enabled=false;

u8 cpuBitsSet[256];


#if FEAT_AREC == DYNAREC_NONE || defined(FEAT_AREC_SELECTABLE) || defined(HOST_APPLE_JIT)


/* ARM7 interpreter */

void arm_Run_(u32 CycleCount)
{
	s32 left;
	u32 clockTicks=0;
	if (!Arm7Enabled)
		return;

	/* The cycles the last instruction ran past the end of the last sample
	 * are this one's, as the recompiler counts them, in the same register
	 * (one from a state is a few cycles at most). */
	left = (s32)reg[CYCL_CNT].I;
	if (left > 0 || left < -(s32)CycleCount)
		left = 0;
	left += (s32)CycleCount;
	while ((s32)clockTicks < left)
	{
		if (reg[INTR_PEND].I)
		{
			CPUFiq();
		}

		reg[15].I = armNextPC + 8;
		#include "arm-new.h"
	}
	reg[CYCL_CNT].I = (u32)(left - (s32)clockTicks);
}
#endif

#if FEAT_AREC == DYNAREC_NONE
void aicaarm_run(u32 samples)
{
	u32 i;
	for (i = 0; i < samples; i++)
	{
		arm_Run_(ARM_CYCLES_PER_SAMPLE);
		libAICA_TimeStep();
	}
}
#endif

void aicaarm_init(void)
{
	int i, j;
#if FEAT_AREC != DYNAREC_NONE
	arm7_rec_init();
#endif
   aicaarm_reset();

	for (i = 0; i < 256; i++)
	{
		int count = 0;
		for (j = 0; j < 8; j++)
			if (i & (1 << j))
				count++;

		cpuBitsSet[i] = count;
	}
}

void CPUSwitchMode(int mode, bool saveState)
{
	u32 CPSR, SPSR;
	CPUUpdateCPSR();

	switch(armMode)
	{
	case 0x10:
	case 0x1F:
		reg[R13_USR].I = reg[13].I;
		reg[R14_USR].I = reg[14].I;
		reg[17].I = reg[16].I;
		break;
	case 0x11:
		CPUSwap(&reg[R8_FIQ].I, &reg[8].I);
		CPUSwap(&reg[R9_FIQ].I, &reg[9].I);
		CPUSwap(&reg[R10_FIQ].I, &reg[10].I);
		CPUSwap(&reg[R11_FIQ].I, &reg[11].I);
		CPUSwap(&reg[R12_FIQ].I, &reg[12].I);
		reg[R13_FIQ].I = reg[13].I;
		reg[R14_FIQ].I = reg[14].I;
		reg[SPSR_FIQ].I = reg[17].I;
		break;
	case 0x12:
		reg[R13_IRQ].I  = reg[13].I;
		reg[R14_IRQ].I  = reg[14].I;
		reg[SPSR_IRQ].I =  reg[17].I;
		break;
	case 0x13:
		reg[R13_SVC].I  = reg[13].I;
		reg[R14_SVC].I  = reg[14].I;
		reg[SPSR_SVC].I =  reg[17].I;
		break;
	case 0x17:
		reg[R13_ABT].I  = reg[13].I;
		reg[R14_ABT].I  = reg[14].I;
		reg[SPSR_ABT].I =  reg[17].I;
		break;
	case 0x1b:
		reg[R13_UND].I  = reg[13].I;
		reg[R14_UND].I  = reg[14].I;
		reg[SPSR_UND].I =  reg[17].I;
		break;
	}

	CPSR = reg[16].I;
	SPSR = reg[17].I;

	switch(mode)
	{
	case 0x10:
	case 0x1F:
		reg[13].I = reg[R13_USR].I;
		reg[14].I = reg[R14_USR].I;
		reg[16].I = SPSR;
		break;
	case 0x11:
		CPUSwap(&reg[8].I, &reg[R8_FIQ].I);
		CPUSwap(&reg[9].I, &reg[R9_FIQ].I);
		CPUSwap(&reg[10].I, &reg[R10_FIQ].I);
		CPUSwap(&reg[11].I, &reg[R11_FIQ].I);
		CPUSwap(&reg[12].I, &reg[R12_FIQ].I);
		reg[13].I = reg[R13_FIQ].I;
		reg[14].I = reg[R14_FIQ].I;
		if(saveState)
			reg[17].I = CPSR;
		else
			reg[17].I = reg[SPSR_FIQ].I;
		break;
	case 0x12:
		reg[13].I = reg[R13_IRQ].I;
		reg[14].I = reg[R14_IRQ].I;
		reg[16].I = SPSR;
		if(saveState)
			reg[17].I = CPSR;
		else
			reg[17].I = reg[SPSR_IRQ].I;
		break;
	case 0x13:
		reg[13].I = reg[R13_SVC].I;
		reg[14].I = reg[R14_SVC].I;
		reg[16].I = SPSR;
		if(saveState)
			reg[17].I = CPSR;
		else
			reg[17].I = reg[SPSR_SVC].I;
		break;
	case 0x17:
		reg[13].I = reg[R13_ABT].I;
		reg[14].I = reg[R14_ABT].I;
		reg[16].I = SPSR;
		if(saveState)
			reg[17].I = CPSR;
		else
			reg[17].I = reg[SPSR_ABT].I;
		break;
	case 0x1b:
		reg[13].I = reg[R13_UND].I;
		reg[14].I = reg[R14_UND].I;
		reg[16].I = SPSR;
		if(saveState)
			reg[17].I = CPSR;
		else
			reg[17].I = reg[SPSR_UND].I;
		break;
	default:
		/* A mode there is none of: the processor is in no state to go
		 * on from, and is left standing (upstream). It was the emulator
		 * that stopped, die(), for a value the program writes. */
		ERROR_LOG(AICA_ARM, "Unsupported ARM mode %02x\n", mode);
		Arm7Enabled = false;
		break;
	}
	armMode = mode;
	CPUUpdateFlags();
	CPUUpdateCPSR();
}

void CPUUpdateCPSR(void)
{
	reg_pair CPSR;

	CPSR.I = reg[RN_CPSR].I & 0x40;

	CPSR.PSR.NZCV=reg[RN_PSR_FLAGS].FLG.NZCV;


	if (!armFiqEnable)
		CPSR.I |= 0x40;
	if(!armIrqEnable)
		CPSR.I |= 0x80;

	CPSR.PSR.M=armMode;
	
	reg[16].I = CPSR.I;
}

void CPUUpdateFlags(void)
{
	u32 CPSR = reg[16].I;

	reg[RN_PSR_FLAGS].FLG.NZCV=reg[16].PSR.NZCV;

	armIrqEnable = (CPSR & 0x80) ? false : true;
	armFiqEnable = (CPSR & 0x40) ? false : true;
	update_armintc();
}

void CPUSoftwareInterrupt(int comment)
{
	u32 PC = reg[R15_ARM_NEXT].I+4;
	CPUSwitchMode(0x13, true);
	reg[14].I = PC;
	
	armIrqEnable = false;
	armNextPC = 0x08;
}

void CPUUndefinedException(void)
{
	u32 PC = reg[R15_ARM_NEXT].I+4;
	WARN_LOG(AICA_ARM, "arm7: CPUUndefinedException(). SOMETHING WENT WRONG\n");
	CPUSwitchMode(0x1b, true);
	reg[14].I = PC;
	armIrqEnable = false;
	armNextPC = 0x04;
}

void aicaarm_reset(void)
{
   DEBUG_LOG(AICA_ARM, "AICA ARM Reset\n");
#if FEAT_AREC != DYNAREC_NONE
	arm7_rec_flush();
#endif
	aica_interr = false;
	aica_reg_L = 0;
	e68k_out = false;
	e68k_reg_L = 0;
	e68k_reg_M = 0;

	Arm7Enabled = false;
	/* clean registers */
	memset(&arm_Reg[0], 0, sizeof(arm_Reg));

	armMode = 0x13;

	reg[13].I = 0x03007F00;
	reg[15].I = 0x0000000;
	reg[16].I = 0x00000000;
	reg[R13_IRQ].I = 0x03007FA0;
	reg[R13_SVC].I = 0x03007FE0;
	armIrqEnable = true;      
	armFiqEnable = false;
	update_armintc();

	C_FLAG = V_FLAG = N_FLAG = Z_FLAG = false;

	/* disable FIQ */
	reg[16].I |= 0x40;

	CPUUpdateCPSR();

	armNextPC = reg[15].I;
	reg[15].I += 4;
}

NOINLINE
void CPUFiq(void)
{
	u32 PC = reg[R15_ARM_NEXT].I+4;
	CPUSwitchMode(0x11, true);
	reg[14].I = PC;
	armIrqEnable = false;
	armFiqEnable = false;
	update_armintc();

	armNextPC = 0x1c;
}


/*
	--Seems like aica has 3 interrupt controllers actualy (damn lazy sega ..)
	The "normal" one (the one that exists on scsp) , one to emulate the 68k intc , and , 
	of course , the arm7 one

	The output of the sci* bits is input to the e68k , and the output of e68k is inputed into the FIQ
	pin on arm7
*/
#include "hw/sh4/sh4_core.h"


void aicaarm_enable(bool enabled)
{
	if(!Arm7Enabled && enabled)
      aicaarm_reset();
	
	Arm7Enabled=enabled;
}

void update_armintc(void)
{
	reg[INTR_PEND].I=e68k_out && armFiqEnable;
}

#if FEAT_AREC != DYNAREC_NONE && (HOST_CPU == CPU_X64 || HOST_CPU == CPU_X86 || HOST_CPU == CPU_ARM64)
/* For the recompiler (arm7_rec.cpp, with arm7_rec_x64.cpp, arm7_rec_x86.cpp
 * or arm7_rec_arm64.cpp) */

/* A single instruction, @opcode, interpreted */
void DYNACALL arm7_interpret(u32 opcode)
{
	u32 clockTicks = 0;

#define NO_OPCODE_READ
#include "arm-new.h"
#undef NO_OPCODE_READ

	reg[CYCL_CNT].I -= clockTicks;
}

/* MSR SPSR_<mask>, @v */
void DYNACALL arm7_msr_spsr(u32 v, u32 mask)
{
	if(armMode > 0x10 && armMode < 0x1f) /* !=0x10 ?*/
	{
		u32 newValue = reg[RN_SPSR].I;
		if (mask & 1)
			newValue = (newValue & 0xFFFFFF00) | (v & 0x000000FF);
		if (mask & 2)
			newValue = (newValue & 0xFFFF00FF) | (v & 0x0000FF00);
		if (mask & 4)
			newValue = (newValue & 0xFF00FFFF) | (v & 0x00FF0000);
		if (mask & 8)
			newValue = (newValue & 0x00FFFFFF) | (v & 0xFF000000);
		reg[RN_SPSR].I = newValue;
	}
}

/* MSR CPSR_<mask>, @v */
void DYNACALL arm7_msr_cpsr(u32 v, u32 mask)
{
	u32 newValue;
	CPUUpdateCPSR();

	newValue = reg[RN_CPSR].I;
	if(armMode > 0x10)
	{
		if (mask & 1)
			newValue = (newValue & 0xFFFFFF00) | (v & 0x000000FF);
		if (mask & 2)
			newValue = (newValue & 0xFFFF00FF) | (v & 0x0000FF00);
		if (mask & 4)
			newValue = (newValue & 0xFF00FFFF) | (v & 0x00FF0000);
	}
	if (mask & 8)
		newValue = (newValue & 0x00FFFFFF) | (v & 0xFF000000);
	newValue |= 0x10;
	if(armMode > 0x10)
	{
		CPUSwitchMode(newValue & 0x1f, false);
	}
	reg[RN_CPSR].I = newValue;
	CPUUpdateFlags();
}
#endif
