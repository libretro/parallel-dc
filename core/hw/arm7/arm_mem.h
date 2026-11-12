#pragma once
#include "types.h"
#include "hw/aica/aica_if.h"

#ifdef __cplusplus
extern "C" {
#endif

extern bool aica_interr;
extern u32 aica_reg_L;
extern bool e68k_out;
extern u32 e68k_reg_L;
extern u32 e68k_reg_M;

void update_armintc(void);

/* The sound chip's registers, as the ARM7 reads and writes them: @sz bytes */
u32 arm_ReadReg(u32 addr, u32 sz);
void arm_WriteReg(u32 addr, u32 data, u32 sz);

/* The ARM7's memory: the sound RAM below 8 MiB, the registers above */
static TYPES_INLINE u8 arm_ReadMem8(u32 addr)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
		return aica_ram.data[addr & ARAM_MASK];
	return (u8)arm_ReadReg(addr, 1);
}

static TYPES_INLINE u16 arm_ReadMem16(u32 addr)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
		return *(u16 *)&aica_ram.data[addr & (ARAM_MASK - 1)];
	return (u16)arm_ReadReg(addr, 2);
}

static TYPES_INLINE u32 arm_ReadMem32(u32 addr)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
	{
		u32 rv = *(u32 *)&aica_ram.data[addr & (ARAM_MASK - 3)];
		/* (a misaligned word is the aligned one rotated) */
		if (unlikely(addr & 3))
		{
			u32 sf = (addr & 3) * 8;
			return (rv >> sf) | (rv << (32 - sf));
		}
		return rv;
	}
	return arm_ReadReg(addr, 4);
}

static TYPES_INLINE void arm_WriteMem8(u32 addr, u8 data)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
		aica_ram.data[addr & ARAM_MASK] = data;
	else
		arm_WriteReg(addr, data, 1);
}

static TYPES_INLINE void arm_WriteMem16(u32 addr, u16 data)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
		*(u16 *)&aica_ram.data[addr & (ARAM_MASK - 1)] = data;
	else
		arm_WriteReg(addr, data, 2);
}

static TYPES_INLINE void arm_WriteMem32(u32 addr, u32 data)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
		*(u32 *)&aica_ram.data[addr & (ARAM_MASK - 3)] = data;
	else
		arm_WriteReg(addr, data, 4);
}

#ifdef __cplusplus
}
#endif
