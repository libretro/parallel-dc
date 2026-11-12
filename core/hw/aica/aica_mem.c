/* The sound chip's registers as the SH4 and the ARM7 read and write them */
#include <string.h>

#include "aica.h"
#include "aica_if.h"
#include "aica_mem.h"
#include "dsp.h"
#include "sgc_if.h"

u8 aica_reg[0x8000];

/* 00000000~007FFFFF @DRAM_AREA* */
/* 00800000~008027FF @CHANNEL_DATA */
/* 00802800~00802FFF @COMMON_DATA */
/* 00803000~00807FFF @DSP_DATA */

/* The DSP's work registers, TEMP (128 of them, at 0x4000), MEMS (32, at
 * 0x4400) and MIXS (16, at 0x4500), are the DSP's own: each is seen as two
 * 16-bit registers 4 bytes apart, eight bytes to one. TEMP and MEMS have 24
 * bits, the low 8 in the first register and the high 16 in the second;
 * MIXS has 20, the low 4 and then the high 16. The registers with bit 1 of
 * their address set are not there. */
static s32 *DspWorkReg(u32 addr)
{
   if (addr < 0x4400)
      return &dsp.TEMP[(addr - 0x4000) / 8];
   if (addr < 0x4500)
      return &dsp.MEMS[(addr - 0x4400) / 8];
   return &dsp.MIXS[(addr - 0x4500) / 8];
}

/* (@sz bytes, 1 or 2, in all of these) */
static u32 ReadDspWorkReg(u32 addr, u32 sz)
{
   u32 low_bits, v;

   if (addr & 2)
      return 0;
   low_bits = addr < 0x4500 ? 8 : 4;
   v = (u32)*DspWorkReg(addr);
   if (addr & 4)
      v = (v >> low_bits) & 0xffff;
   else
      v &= (1 << low_bits) - 1;
   if (sz == 1)
      v = addr & 1 ? v >> 8 : v & 0xff;
   return v;
}

static void WriteDspWorkReg(u32 addr, u32 data, u32 sz)
{
   const u32 low_bits = addr < 0x4500 ? 8 : 4;
   s32 *v = DspWorkReg(addr);
   if (addr & 4)
   {
      /* the high part, sign-extended to 32 bits */
      if (sz == 1 && (addr & 1))
         *v = (*v & ((1 << (low_bits + 8)) - 1)) | (((s32)(data << 24)) >> (16 - low_bits));
      else if (sz == 1)
         *v = (*v & ~(0xff << low_bits)) | ((data & 0xff) << low_bits);
      else
         *v = (*v & ((1 << low_bits) - 1)) | (((s32)(data << 16)) >> (16 - low_bits));
   }
   else if (sz != 1 || (addr & 1) == 0)
      *v = (*v & ~((1 << low_bits) - 1)) | (data & ((1 << low_bits) - 1));
}

static u32 ReadReg(u32 addr, u32 sz)
{
   if (addr >= 0x2800 && addr < 0x2818)
   {
      if (sz == 1)
         ReadCommonReg(addr, true);
      else
         ReadCommonReg(addr, false);
   }
   else if (addr >= 0x4000 && addr < 0x4580)
      return ReadDspWorkReg(addr, sz);
   return mem_arr_read(aica_reg, addr, sz);
}

static void WriteReg(u32 addr,u32 data,u32 sz)
{
	if (addr < 0x2000)
	{
		/* Channel data */
		u32 chan = addr >> 7;
		u32 reg = addr & 0x7F;
		mem_arr_write(aica_reg, addr, data, sz);
		WriteChannelReg(chan, reg, sz);
		return;
	}

	if (addr<0x2800)
	{
		if (sz==1)
			mem_arr_write(aica_reg, addr, data, 1);
		else 
			mem_arr_write(aica_reg, addr, data, 2);
		return;
	}

	if (addr < 0x2818)
	{
		if (sz==1)
		{
			WriteCommonReg8(addr,data);
		}
		else
		{
			WriteCommonReg8(addr,data&0xFF);
			WriteCommonReg8(addr+1,data>>8);
		}
		return;
	}

	if (addr>=0x3000)
	{
		/* (no register has bit 1 of its address set here) */
		if (addr & 2)
			return;
		if (addr >= 0x4000 && addr < 0x4580)
		{
			WriteDspWorkReg(addr, data, sz);
			return;
		}
		if (sz==1)
		{
			mem_arr_write(aica_reg, addr, data, 1);
			dsp_writenmem(addr);
		}
		else
		{
			mem_arr_write(aica_reg, addr, data, 2);
			dsp_writenmem(addr);
			dsp_writenmem(addr+1);
		}
		return;
	}
	if (sz==1)
		WriteAicaReg(addr,data,1);
	else
		WriteAicaReg(addr,data,2);
}
/* Aica reads (both sh4&arm) */
/* The registers are 16 bits wide, each at an even address: anything wider
 * than a byte is for the register the address is in. An odd address used
 * to go through as it was, and one at 0x7FFF read or wrote a byte past
 * the end of aica_reg. */
u32 libAICA_ReadReg(u32 addr, u32 size)
{
   if (size == 1)
      return ReadReg(addr & 0x7FFF, 1);
   return ReadReg(addr & 0x7FFE, 2);
}

void libAICA_WriteReg(u32 addr,u32 data,u32 size)
{
   if (size==1)
		WriteReg(addr & 0x7FFF,data,1);
	else
		WriteReg(addr & 0x7FFE,data,2);
}

/* Map using _vmem .. yay */
void init_mem(void)
{
	memset(aica_reg,0,sizeof(aica_reg));
	aica_ram.data[ARAM_SIZE-1]=1;
   varray2_zero(&aica_ram);
}

/* kill mem map & free used mem ;) */
void term_mem(void)
{

}

