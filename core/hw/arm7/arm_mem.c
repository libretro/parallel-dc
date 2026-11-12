#include "arm_mem.h"
#include "arm7.h"
#include "types.h"
#include "hw/aica/aica_if.h"

#define REG_L (0x2D00)
#define REG_M (0x2D04)

/* Set to true when aica interrupt is pending */
bool aica_interr=false;
u32 aica_reg_L=0;
/* Set to true when the out of the intc is 1 */
bool e68k_out = false;
u32 e68k_reg_L;
u32 e68k_reg_M=0; /* constant ? */

static void update_e68k(void)
{
	if (!e68k_out && aica_interr)
	{
		/* Set the pending signal */
		/* Is L register held here too ? */
		e68k_out=true;
		e68k_reg_L=aica_reg_L;

		update_armintc();
	}
}

void libARM_InterruptChange(u32 bits,u32 L)
{
	aica_interr=bits!=0;
	if (aica_interr)
		aica_reg_L=L;
	update_e68k();
}

static void e68k_AcceptInterrupt(void)
{
	e68k_out=false;
	update_e68k();
	update_armintc();
}

/* Reg reads from arm side .. */
u32 arm_ReadReg(u32 addr, u32 sz)
{
	addr&=0x7FFF;
	if (addr==REG_L)
		return e68k_reg_L;
	else if(addr==REG_M)
		return e68k_reg_M;	/* shouldn't really happen */
	else
		return libAICA_ReadReg(addr,sz);
}

void arm_WriteReg(u32 addr, u32 data, u32 sz)
{
	addr &= 0x7FFF;
	if (addr == REG_L)
	{
		return; /* Shouldn't really happen (read only) */
	}
	else if (addr == REG_M)
	{
		/* accept interrupts */
		if (data & 1)
			e68k_AcceptInterrupt();
	}
	else
	{
		libAICA_WriteReg(addr, data, sz);
	}
}

/* 00000000~007FFFFF @DRAM_AREA* */
/* 00800000~008027FF @CHANNEL_DATA */
/* 00802800~00802FFF @COMMON_DATA */
/* 00803000~00807FFF @DSP_DATA */
