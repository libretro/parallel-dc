/*
	PowerVR interface to plugins
	Handles YUV conversion (slow and ugly -- but hey it works ...)

	Most of this was hacked together when i needed support for YUV-dma for thps2 ;)
*/

#include "pvr_mem.h"
#include "pvr_regs.h"
#include "ta.h"
#include "spg.h"
#include "Renderer_if.h"

#include "hw/holly/sb.h"
#include "hw/holly/holly_intc.h"


/* YUV converter code :) */
/* inits the YUV converter */
u32 YUV_tempdata[512/4];/* 512 bytes */

u32 YUV_dest=0;

u32 YUV_blockcount;

u32 YUV_x_curr;
u32 YUV_y_curr;

u32 YUV_x_size;
u32 YUV_y_size;

u32 YUV_index = 0;

void YUV_init(void)
{
   YUV_x_curr     = 0;
   YUV_y_curr     = 0;
   YUV_dest       = TA_YUV_TEX_BASE & VRAM_MASK;
   TA_YUV_TEX_CNT = 0;
   YUV_blockcount = (TA_YUV_TEX_CTRL.yuv_u_size + 1) * (TA_YUV_TEX_CTRL.yuv_v_size + 1);

   if (TA_YUV_TEX_CTRL.yuv_tex != 0)
   {
      /* A texture of 16x16 for each macroblock, not one of them all: 512
       * bytes each, one after another from the base address. Converted
       * as a picture one macroblock wide and high, a macroblock fills
       * exactly its 512 bytes and the next one starts where it ends. */
      YUV_x_size = 16;
      YUV_y_size = 16;
   }
   else
   {
      YUV_x_size = (TA_YUV_TEX_CTRL.yuv_u_size + 1) * 16;
      YUV_y_size = (TA_YUV_TEX_CTRL.yuv_v_size + 1) * 16;
   }
   YUV_index = 0;
}

/* Nothing set up and nothing half received: data is ignored until the game sets the converter up */
void YUV_reset(void)
{
   memset(YUV_tempdata, 0, sizeof(YUV_tempdata));
   YUV_dest       = 0;
   YUV_blockcount = 0;
   YUV_x_curr     = 0;
   YUV_y_curr     = 0;
   YUV_x_size     = 0;
   YUV_y_size     = 0;
   YUV_index      = 0;
}

#ifdef HAVE_LIBNX
#pragma GCC push_options
#pragma GCC optimize ("-O2")
#endif

/* (@x_size: the width in pixels of the picture @out is in) */
static void YUV_Block8x8(const u8* inuv, const u8* iny, u8* out, u32 x_size)
{
	u8* line_out_0=out+0;
	u8* line_out_1=out+x_size*2;
	int x, y;

	for (y=0;y<8;y+=2)
	{
		for (x=0;x<8;x+=2)
		{
			u8 u=inuv[0];
			u8 v=inuv[64];

			line_out_0[0]=u;
			line_out_0[1]=iny[0];
			line_out_0[2]=v;
			line_out_0[3]=iny[1];

			line_out_1[0]=u;
			line_out_1[1]=iny[8+0];
			line_out_1[2]=v;
			line_out_1[3]=iny[8+1];

			inuv+=1;
			iny+=2;

			line_out_0+=4;
			line_out_1+=4;
		}
		iny+=8;
		inuv+=4;

		line_out_0+=x_size*4-8*2;
		line_out_1+=x_size*4-8*2;
	}
}
#ifdef HAVE_LIBNX
#pragma GCC pop_options
#endif

static INLINE void YUV_Block384(u8* in, u8* out, u32 x_size)
{
	u8* inuv=in;
	u8* iny=in+128;
	u8* p_out=out;

	YUV_Block8x8(inuv+ 0,iny+  0,p_out,x_size);                 /* (0,0) */
	YUV_Block8x8(inuv+ 4,iny+64,p_out+8*2,x_size);              /* (8,0) */
	YUV_Block8x8(inuv+32,iny+128,p_out+x_size*8*2,x_size);      /* (0,8) */
	YUV_Block8x8(inuv+36,iny+192,p_out+x_size*8*2+8*2,x_size);  /* (8,8) */
}

/* A macroblock some of which would land past the end of video memory: it
 * is converted on its own and put there a byte at a time, each at its
 * address taken round to the start of video memory, as the chip's address
 * goes. No game does this; a wrong base address, or a save state, can. */
static NOINLINE void YUV_ConvertMacroBlockWrapped(u8* datap, u32 dest, u32 x_size)
{
	u8 block[16 * 32];
	u32 x, y;

	YUV_Block384(datap, block, 16);
	for (y = 0; y < 16; y++)
	{
		const u32 line = dest + y * x_size * 2;
		for (x = 0; x < 32; x++)
			vram.data[(line + x) & VRAM_MASK] = block[y * 32 + x];
	}
}

static INLINE void YUV_ConvertMacroBlock(u8* datap)
{
	/* A macroblock is 16 lines of 32 bytes, a picture's line apart: the last
	 * byte of it is 15 lines and 31 bytes from the first. */
	const u32 dest   = YUV_dest & VRAM_MASK;
	const u32 x_size = YUV_x_size;

	TA_YUV_TEX_CNT++;
	if ((u64)x_size * (15 * 2) + 32 + dest <= VRAM_SIZE)
		YUV_Block384((u8*)datap,vram.data + dest,x_size);
	else
		YUV_ConvertMacroBlockWrapped(datap, dest, x_size);

	YUV_dest+=32;

	YUV_x_curr+=16;
	if (YUV_x_curr==YUV_x_size)
	{
		YUV_dest+=15*YUV_x_size*2;
		YUV_x_curr=0;
		YUV_y_curr+=16;
		if (YUV_y_curr==YUV_y_size)
		{
			YUV_y_curr=0;
		}
	}

	/* TA_YUV_TEX_CNT stays at the count it reached: it starts again when more data comes */
	if (YUV_blockcount==TA_YUV_TEX_CNT)
		asic_RaiseInterrupt(holly_YUV_DMA);
}

void YUV_data(u32* data , u32 count)
{
	u32 block_size;

	if (YUV_blockcount==0)
	{
		/* not set up: nothing to convert to, and not a reason to stop */
		WARN_LOG(PVR, "YUV_data: YUV decoder not inited");
		return;
	}
	/* the picture before this one is complete: this is the start of the next */
	if (YUV_blockcount == TA_YUV_TEX_CNT)
		YUV_init();

	block_size = TA_YUV_TEX_CTRL.yuv_form == 0 ? 384 : 512;

	if (block_size != 384)
	{
		/* no support for 512 */
		WARN_LOG(PVR, "YUV_data: block size 512 not supported");
		return;
	}

	
	count*=32;

	while (count > 0)
	{
		if (YUV_index + count >= block_size)
		{
			/* more or exactly one block remaining */
			u32 dr = block_size - YUV_index;				/* remaining bytes til block end */
			if (YUV_index == 0)
			{
				/* Avoid copy */
				YUV_ConvertMacroBlock((u8 *)data);				/* convert block */
			}
			else
			{
				memcpy(&YUV_tempdata[YUV_index >> 2], data, dr);/* copy em */
				YUV_ConvertMacroBlock((u8 *)&YUV_tempdata[0]);	/* convert block */
				YUV_index = 0;
			}
			data += dr >> 2;									/* count em */
			count -= dr;
		}
		else
		{	/* less that a whole block remaining */
			memcpy(&YUV_tempdata[YUV_index >> 2], data, count);	/* append it */
			YUV_index += count;
			count = 0;
		}
	}
	verify(count==0);
}

/* vram 32-64b */

/* An access of the 32-bit view, aligned to its size: the last bytes of
 * video memory are not a way out of it */
#define PVR_AREA1(addr, size) (&vram.data[pvr_map32(addr) & ~(u32)((size) - 1)])

u8 DYNACALL pvr_read_area1_8(u32 addr) { return *PVR_AREA1(addr, 1); }
u16 DYNACALL pvr_read_area1_16(u32 addr) { return *(u16 *)PVR_AREA1(addr, 2); }
u32 DYNACALL pvr_read_area1_32(u32 addr) { return *(u32 *)PVR_AREA1(addr, 4); }

/* (whether the framebuffer the game drew itself has been written) */
static TYPES_INLINE void pvr_area1_written(u32 addr)
{
	u32 vaddr = addr & VRAM_MASK;
	if (vaddr >= fb_watch_addr_start && vaddr < fb_watch_addr_end)
		fb_dirty = true;
}

void DYNACALL pvr_write_area1_8(u32 addr, u8 data)
{
	(void)data;
	INFO_LOG(MEMORY, "%08x: 8-bit VRAM writes are not possible", addr);
}
void DYNACALL pvr_write_area1_16(u32 addr, u16 data)
{
	pvr_area1_written(addr);
	*(u16 *)PVR_AREA1(addr, 2) = data;
}
void DYNACALL pvr_write_area1_32(u32 addr, u32 data)
{
	pvr_area1_written(addr);
	*(u32 *)PVR_AREA1(addr, 4) = data;
}

void TAWrite(u32 address, u32* data, u32 count)
{
   if ((address & 0x800000) == 0)
      /* TA poly */
      ta_vtx_data(data, count);
   else
      /* YUV Converter */
      YUV_data(data, count);
}

/* A store queue written out to video memory: @sq is the queue, @address
 * where it goes. Used by WinCE. (On its own so that the 32-bit ARM build,
 * whose TAWriteSQ is in assembly, has it as well: ngen_arm.S.) */
void TAWriteSQ_to_vram(u32 address, u8* sq)
{
   u32 address_w = address & 0x01FFFFE0;
   bool path64b = (address & 0x02000000 ? SB_LMMODE1 : SB_LMMODE0) == 0;

   DEBUG_LOG(MEMORY, "Vram TAWriteSQ 0x%X SB_LMMODE0 %d", address, SB_LMMODE0);
   if (path64b)
   {
      /* 64b path */
      memcpy(&vram.data[address_w & VRAM_MASK], sq, 32);
   }
   else
   {
      /* 32b path */
      int i;
      for (i = 0; i < 8; i++, address_w += 4)
         pvr_write_area1_32(address_w, ((u32 *)sq)[i]);
   }
}

#if HOST_CPU!=CPU_ARM
void DYNACALL TAWriteSQ(u32 address,u8* sqb)
{
   u32 address_w = address & 0x01FFFFE0;
   u8* sq = &sqb[address & 0x20];

   if (likely(address_w < 0x800000))/* TA poly */
   {
      ta_vtx_data32(sq);
   }
   else if(likely(address_w < 0x1000000)) /* Yuv Converter */
   {
      YUV_data((u32*)sq, 1);
   }
   else /* Vram Writef */
      TAWriteSQ_to_vram(address, sq);
}
#endif

/* Misc interface */


/* A word of the chip's 32-bit view, as the chip reads it: the two low bits
 * of the address are not part of it. (Kept, they took a read from an odd
 * address the game gave - REGION_BASE, say - up to three bytes past the
 * end of video memory.) */
f32 vrf(u32 addr)
{
	return *(f32*)&vram.data[pvr_map32(addr) & ~3u];
}
u32 vri(u32 addr)
{
	return *(u32*)&vram.data[pvr_map32(addr) & ~3u];
}

/* Area 4, the lower half or the upper (@upper): by the 32-bit view or the
 * 64-bit, as SB_LMMODE0 or SB_LMMODE1 has it */
#define PVR_AREA4_32(upper) (((upper) ? SB_LMMODE1 : SB_LMMODE0) == 1)
#define PVR_AREA4(type, bits, half, upper) \
	type DYNACALL pvr_read_area4_##half##_##bits(u32 addr) \
	{ \
		if (PVR_AREA4_32(upper)) \
			return pvr_read_area1_##bits(addr); \
		return *(type *)&vram.data[addr & VRAM_MASK]; \
	} \
	void DYNACALL pvr_write_area4_##half##_##bits(u32 addr, type data) \
	{ \
		if (PVR_AREA4_32(upper)) \
			pvr_write_area1_##bits(addr, data); \
		else \
			*(type *)&vram.data[addr & VRAM_MASK] = data; \
	}
PVR_AREA4(u8, 8, lo, 0)
PVR_AREA4(u16, 16, lo, 0)
PVR_AREA4(u32, 32, lo, 0)
PVR_AREA4(u8, 8, hi, 1)
PVR_AREA4(u16, 16, hi, 1)
PVR_AREA4(u32, 32, hi, 1)
