#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

f32 vrf(u32 addr);
u32 vri(u32 addr);

/* vram 32-64b */
extern VArray2 vram;

#define VRAM_BANK_BIT 0x400000

/* Where an address of the chip's 32-bit view of video memory is in the
 * memory as it is kept (the 64-bit view): the 64-bit bus is two banks
 * interleaved every 32 bits. Inline, for whatever reads a run of it. */
static TYPES_INLINE u32 pvr_map32(u32 offset32)
{
	const u32 static_bits = VRAM_MASK - (VRAM_BANK_BIT * 2 - 1) + 3;
	const u32 offset_bits = (VRAM_BANK_BIT - 1) & ~3;
	u32 bank = (offset32 & VRAM_BANK_BIT) / VRAM_BANK_BIT;
	u32 rv = offset32 & static_bits;
	rv |= (offset32 & offset_bits) * 2;
	rv |= bank * 4;

	return rv;
}

/* regs */
void pvr_WriteReg(u32 paddr,u32 data);

void TAWrite(u32 address,u32* data,u32 count);
void DYNACALL TAWriteSQ(u32 address,u8* sqb);

void YUV_init(void);
void YUV_reset(void);
/* @count blocks of 32 bytes to the YUV converter */
void YUV_data(u32* data, u32 count);
/* The converter's state, for a save state */
extern u32 YUV_tempdata[512/4];
extern u32 YUV_dest;
extern u32 YUV_blockcount;
extern u32 YUV_x_curr;
extern u32 YUV_y_curr;
extern u32 YUV_x_size;
extern u32 YUV_y_size;
extern u32 YUV_index;

/* A store queue to video memory (the 32-bit ARM's TAWriteSQ has it too) */
void TAWriteSQ_to_vram(u32 address, u8* sq);

/* Video memory by the chip's 32-bit view (area 1) */
u8 DYNACALL pvr_read_area1_8(u32 addr);
u16 DYNACALL pvr_read_area1_16(u32 addr);
u32 DYNACALL pvr_read_area1_32(u32 addr);
void DYNACALL pvr_write_area1_8(u32 addr, u8 data);
void DYNACALL pvr_write_area1_16(u32 addr, u16 data);
void DYNACALL pvr_write_area1_32(u32 addr, u32 data);
/* Area 4, its lower half (lo) and its upper (hi): by the 32-bit view or the
 * 64-bit, as SB_LMMODE0 and SB_LMMODE1 have it */
u8 DYNACALL pvr_read_area4_lo_8(u32 addr);
u16 DYNACALL pvr_read_area4_lo_16(u32 addr);
u32 DYNACALL pvr_read_area4_lo_32(u32 addr);
void DYNACALL pvr_write_area4_lo_8(u32 addr, u8 data);
void DYNACALL pvr_write_area4_lo_16(u32 addr, u16 data);
void DYNACALL pvr_write_area4_lo_32(u32 addr, u32 data);
u8 DYNACALL pvr_read_area4_hi_8(u32 addr);
u16 DYNACALL pvr_read_area4_hi_16(u32 addr);
u32 DYNACALL pvr_read_area4_hi_32(u32 addr);
void DYNACALL pvr_write_area4_hi_8(u32 addr, u8 data);
void DYNACALL pvr_write_area4_hi_16(u32 addr, u16 data);
void DYNACALL pvr_write_area4_hi_32(u32 addr, u32 data);

#ifdef __cplusplus
}
#endif
