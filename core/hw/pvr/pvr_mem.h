#pragma once
#include "types.h"

f32 vrf(u32 addr);
u32 vri(u32 addr);

//vram 32-64b
extern VArray2 vram;

#define VRAM_BANK_BIT 0x400000

/* Where an address of the chip's 32-bit view of video memory is in the
 * memory as it is kept (the 64-bit view): the 64-bit bus is two banks
 * interleaved every 32 bits. Inline, for whatever reads a run of it. */
static inline u32 pvr_map32(u32 offset32)
{
	const u32 static_bits = VRAM_MASK - (VRAM_BANK_BIT * 2 - 1) + 3;
	const u32 offset_bits = (VRAM_BANK_BIT - 1) & ~3;
	u32 bank = (offset32 & VRAM_BANK_BIT) / VRAM_BANK_BIT;
	u32 rv = offset32 & static_bits;
	rv |= (offset32 & offset_bits) * 2;
	rv |= bank * 4;

	return rv;
}

//regs
void pvr_WriteReg(u32 paddr,u32 data);

void TAWrite(u32 address,u32* data,u32 count);
extern "C" void DYNACALL TAWriteSQ(u32 address,u8* sqb);

void YUV_init();
void YUV_reset();

template<typename T> T DYNACALL pvr_read_area1(u32 addr);
template<typename T> void DYNACALL pvr_write_area1(u32 addr, T data);
template<typename T, bool upper> T DYNACALL pvr_read_area4(u32 addr);
template<typename T, bool upper> void DYNACALL pvr_write_area4(u32 addr, T data);
