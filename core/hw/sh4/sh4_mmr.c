/*
	Sh4 internal register routing (P4 & 'area 7')
*/
#include "types.h"
#include "sh4_mmr.h"

#include "hw/mem/_vmem.h"
#include "modules/mmu.h"
#include "modules/ccn.h"
#include "modules/modules.h"
#include "sh4_cache.h"

/* 64bytes of sq // now on context ~ */

DECL_ALIGN(32) u8 OnChipRAM[OnChipRAM_SIZE];

/* All registers are 4 byte aligned */

RegisterStruct CCN[SH4_CCN_REGS];
RegisterStruct UBC[SH4_UBC_REGS];
RegisterStruct BSC[SH4_BSC_REGS];
RegisterStruct DMAC[SH4_DMAC_REGS];
RegisterStruct CPG[SH4_CPG_REGS];
RegisterStruct RTC[SH4_RTC_REGS];
RegisterStruct INTC[SH4_INTC_REGS];
RegisterStruct TMU[SH4_TMU_REGS];
RegisterStruct SCI[SH4_SCI_REGS];
RegisterStruct SCIF[SH4_SCIF_REGS];

/* What a write keeps of a register that is plain data (RIO_DATA): the bits
 * the register has. The others are not there and read as 0. All of them
 * until sh4_rio_wmask() says which. As many as there are registers above,
 * and in the order of sh4_modules. (Beside the registers and not in their
 * flags: those go into save states.) */
static u32 CCN_wmask[SH4_CCN_REGS];
static u32 UBC_wmask[SH4_UBC_REGS];
static u32 BSC_wmask[SH4_BSC_REGS];
static u32 DMAC_wmask[SH4_DMAC_REGS];
static u32 CPG_wmask[SH4_CPG_REGS];
static u32 RTC_wmask[SH4_RTC_REGS];
static u32 INTC_wmask[SH4_INTC_REGS];
static u32 TMU_wmask[SH4_TMU_REGS];
static u32 SCI_wmask[SH4_SCI_REGS];
static u32 SCIF_wmask[SH4_SCIF_REGS];

struct sh4_module
{
	RegisterStruct *regs;
	u32 *wmask;
	u32 count;
};

static const struct sh4_module sh4_modules[] =
{
	{ CCN, CCN_wmask, SH4_CCN_REGS }, { UBC, UBC_wmask, SH4_UBC_REGS },
	{ BSC, BSC_wmask, SH4_BSC_REGS }, { DMAC, DMAC_wmask, SH4_DMAC_REGS },
	{ CPG, CPG_wmask, SH4_CPG_REGS }, { RTC, RTC_wmask, SH4_RTC_REGS },
	{ INTC, INTC_wmask, SH4_INTC_REGS }, { TMU, TMU_wmask, SH4_TMU_REGS },
	{ SCI, SCI_wmask, SH4_SCI_REGS }, { SCIF, SCIF_wmask, SH4_SCIF_REGS }
};

#define SH4_MODULES (sizeof(sh4_modules) / sizeof(sh4_modules[0]))

static const struct sh4_module *sh4_module_of(const RegisterStruct *arr)
{
	u32 i;
	for (i = 0; i < SH4_MODULES; i++)
		if (sh4_modules[i].regs == arr)
			return &sh4_modules[i];
	return NULL;
}

u32 sh4io_read_noacc(u32 addr) 
{ 
	INFO_LOG(SH4, "sh4io: Invalid read access @@ %08X\n", addr);
	return 0; 
} 

void sh4io_write_noacc(u32 addr, u32 data) 
{ 
	INFO_LOG(SH4, "sh4io: Invalid write access @@ %08X %08X\n", addr, data);
	/* verify(false); */
}

void sh4io_write_const(u32 addr, u32 data) 
{ 
	INFO_LOG(SH4, "sh4io: Const write ignored @@ %08X <- %08X\n", addr, data);
}

void sh4_rio_reg(RegisterStruct *arr, u32 addr, RegIO flags, u32 sz, RegReadAddrFP* rf, RegWriteAddrFP* wf)
{
	u32 idx=(addr&255)/4;
	const struct sh4_module *m = sh4_module_of(arr);

	(void)sz;
	verify(m != NULL && idx < m->count);

	arr[idx].flags = flags | REG_ACCESS_32;

   if (flags == RIO_NO_ACCESS)
   {
      arr[idx].readFunctionAddr=&sh4io_read_noacc;
      arr[idx].writeFunctionAddr=&sh4io_write_noacc;
   }
   else if (flags == RIO_CONST)
   {
      arr[idx].writeFunctionAddr=&sh4io_write_const;
   }
   else
   {
      arr[idx].data32=0;

      if (flags & REG_RF)
         arr[idx].readFunctionAddr=rf;

      if (flags & REG_WF)
         arr[idx].writeFunctionAddr=wf==0?&sh4io_write_noacc:wf;
   }
}

void sh4_rio_wmask(RegisterStruct *arr, u32 addr, u32 mask)
{
	u32 idx=(addr&255)/4;
	const struct sh4_module *m = sh4_module_of(arr);

	if (m != NULL && idx < m->count)
		m->wmask[idx] = mask;
}

static INLINE u32 sh4_rio_read(u32 sz, const RegisterStruct *sb_regs, u32 addr)
{	
   u32 offset = addr&255;
#ifdef TRACE
	if (offset & 3/*(size-1)*/) /* 4 is min align size */
	{
		INFO_LOG(SH4, "Unaligned System Bus register read\n");
	}
#endif

   offset>>=2;

#ifdef TRACE
	if (sb_regs[offset].flags & sz)
	{
#endif
      if (!(sb_regs[offset].flags & REG_RF) )
		{
			if (sz==4)
				return  sb_regs[offset].data32;
			else if (sz==2)
				return  sb_regs[offset].data16;
			else 
				return  sb_regs[offset].data8;
		}
		else
		{
			return sb_regs[offset].readFunctionAddr(addr);
		}
#ifdef TRACE
	}
	else
	{
		INFO_LOG(SH4, "ERROR [wrong size read on register]\n");
	}
#endif
	return 0;
}

static INLINE void sh4_rio_write(u32 sz, RegisterStruct *sb_regs, const u32* wmask, u32 addr, u32 data)
{
	u32 offset = addr&255;
#ifdef TRACE
	if (offset & 3/*(size-1)*/) /* 4 is min align size */
	{
		INFO_LOG(SH4, "Unaligned System bus register write\n");
	}
#endif
offset>>=2;
#ifdef TRACE
	if (sb_regs[offset].flags & sz)
	{
#endif
		if (!(sb_regs[offset].flags & REG_WF) )
		{
			data&=wmask[offset];
			if (sz==4)
				sb_regs[offset].data32=data;
			else if (sz==2)
				sb_regs[offset].data16=(u16)data;
			else
				sb_regs[offset].data8=(u8)data;
			return;
		}
		else
		{
			/* printf("RSW: %08X\n",addr); */
			sb_regs[offset].writeFunctionAddr(addr,data);
			return;
		}
#ifdef TRACE
	}
	else
	{
		INFO_LOG(SH4, "ERROR: Wrong size write on register - offset=%x, data=%x, size=%d\n",offset,data,sz);
	}
#endif
}

/* Region P4 */
/* Read P4 */
/* The read of @sz bytes; the functions by size follow (P4_HANDLERS) */
static INLINE u32 read_p4(u32 addr, u32 sz)
{
	switch((addr>>24)&0xFF)
	{
	case 0xE0:
	case 0xE1:
	case 0xE2:
	case 0xE3:
		INFO_LOG(SH4, "Unhandled p4 read [Store queue] 0x%x\n", addr);
		return 0;
	case 0xF0:
		DEBUG_LOG(SH4, "IC Address read %08x\n", addr);
		if (sz == 4)
			return sh4_icache_read_address_array(addr);
		else
			return 0;
	case 0xF1:
		DEBUG_LOG(SH4, "IC Data read %08x\n", addr);
		if (sz == 4)
			return sh4_icache_read_data_array(addr);
		else
			return 0;
	case 0xF2:
		{
			u32 entry=(addr>>8)&3;
			return ITLB[entry].Address.reg_data | (ITLB[entry].Data.V<<8);
		}

	case 0xF3:
		{
			u32 entry=(addr>>8)&3;
			return ITLB[entry].Data.reg_data;
		}

	case 0xF4:
		DEBUG_LOG(SH4, "OC Address read %08x\n", addr);
		if (sz == 4)
			return sh4_ocache_read_address_array(addr);
		else
			return 0;
	case 0xF5:
		DEBUG_LOG(SH4, "OC Data read %08x\n", addr);
		if (sz == 4)
			return sh4_ocache_read_data_array(addr);
		else
			return 0;
	case 0xF6:
		{
			u32 entry=(addr>>8)&63;
			u32 rv=UTLB[entry].Address.reg_data;
			rv|=UTLB[entry].Data.D<<9;
			rv|=UTLB[entry].Data.V<<8;
			return rv;
		}

	case 0xF7:
		{
			u32 entry=(addr>>8)&63;
			return UTLB[entry].Data.reg_data;
		}

	case 0xFF:
		INFO_LOG(SH4, "Unhandled p4 read [area7] 0x%x\n", addr);
		break;

	default:
		INFO_LOG(SH4, "Unhandled p4 read [Reserved] 0x%x\n", addr);
		break;
	}

	return 0;

}

/* Write P4 */
static INLINE void write_p4(u32 addr, u32 data, u32 sz)
{
	switch((addr>>24)&0xFF)
	{
	case 0xE0:
	case 0xE1:
	case 0xE2:
	case 0xE3:
		INFO_LOG(SH4, "Unhandled p4 Write [Store queue] 0x%x\n", addr);
		break;

	case 0xF0:
		DEBUG_LOG(SH4, "IC Address write %08x = %x\n", addr, data);
		if (sz == 4)
			sh4_icache_write_address_array(addr, data);
		return;
	case 0xF1:
		DEBUG_LOG(SH4, "IC Data write %08x = %x\n", addr, data);
		if (sz == 4)
			sh4_icache_write_data_array(addr, data);
		return;
	case 0xF2:
		{
			u32 entry=(addr>>8)&3;
			ITLB[entry].Address.reg_data=data & 0xFFFFFCFF;
			ITLB[entry].Data.V=(data>>8) & 1;
			ITLB_Sync(entry);
		}
		return;
	case 0xF3:
		{
			u32 entry=(addr>>8)&3;
			if (addr&0x800000)
			{
				ITLB[entry].Assistance.reg_data = data & 0xf;
			}
			else
			{
				ITLB[entry].Data.reg_data=data;
			}
			ITLB_Sync(entry);
			return;
		}

	case 0xF4:
		DEBUG_LOG(SH4, "OC Address write %08x = %x\n", addr, data);
		if (sz == 4)
			sh4_ocache_write_address_array(addr, data);
		return;
	case 0xF5:
		DEBUG_LOG(SH4, "OC Data write %08x = %x\n", addr, data);
		if (sz == 4)
			sh4_ocache_write_data_array(addr, data);
		return;
	case 0xF6:
		{
			if (addr&0x80)
			{
				CCN_PTEH_type t;
				u32 va;
#ifndef NO_MMU
				const u32 valid = ((u32)data >> 8) & 1;
				int i;
#else
				INFO_LOG(SH4, "Unhandled p4 Write [Unified TLB address array, Associative Write] 0x%x = %x\n", addr, data);
#endif
				t.reg_data=data;

				va=t.VPN<<10;

#ifndef NO_MMU
					for (i=0;i<64;i++)
					{
						if (mmu_match(va,UTLB[i].Address,UTLB[i].Data))
						{
							UTLB[i].Data.V=valid;
							UTLB[i].Data.D=((u32)data>>9)&1;
							if (valid)
								UTLB_Sync(i);
						}
					}

					for (i=0;i<4;i++)
					{
						if (mmu_match(va,ITLB[i].Address,ITLB[i].Data))
						{
							ITLB[i].Data.V=valid;
							ITLB[i].Data.D=((u32)data>>9)&1;
							ITLB_Sync(i);
						}
					}
					/* Not valid: the page is taken out of the TLB, which is
					 * how a program does that for one page. It has to be
					 * gone from what is kept beyond the 64 entries as well,
					 * and from everything that was made of it. */
					if (!valid)
						mmu_forget(va);
#endif
			}
			else
			{
				u32 entry=(addr>>8)&63;
				UTLB[entry].Address.reg_data=data & 0xFFFFFCFF;
				UTLB[entry].Data.D=(data>>9)&1;
				UTLB[entry].Data.V=(data>>8)&1;
				/* whatever page the entry was for before goes with it */
				mmu_utlb_written(entry);
			}
			return;
		}
		break;

	case 0xF7:
		{
			u32 entry=(addr>>8)&63;
			if (addr&0x800000)
			{
				UTLB[entry].Assistance.reg_data = data & 0xf;
			}
			else
			{
				UTLB[entry].Data.reg_data=data;
			}
			mmu_utlb_written(entry);

			return;
		}

	case 0xFF:
		INFO_LOG(SH4, "Unhandled p4 Write [area7] 0x%x = %x\n", addr, data);
		break;

	default:
		INFO_LOG(SH4, "Unhandled p4 Write [Reserved] 0x%x\n", addr);
		break;
	}
}


/* *********** */
/* **Area  7** */
/* *********** */

#define OUT_OF_RANGE(reg) INFO_LOG(SH4, "Out of range on register %s index %x\n", reg, addr)
#define A7_REG_HASH(addr) (((addr) >> 16) & 0x1FFF)

/* Read P4 memory-mapped registers */
static INLINE u32 read_p4mmr(u32 addr, u32 sz)
{
	u32 map_base;
	/*
	if (likely(addr==0xffd80024))
	{
		return TMU_TCNT(2);
	}
	else if (likely(addr==0xFFD8000C))
	{
		return TMU_TCNT(0);
	}
	else */if (likely(addr==0xFF000028))
	{
		return CCN_INTEVT;
	}
	else if (likely(addr==0xFFA0002C))
	{
		return DMAC_CHCR(2).full;
	}

	addr&=0x1FFFFFFF;
	map_base=addr>>16;
   switch (expected(map_base, A7_REG_HASH(TMU_BASE_addr)))
	{
	case A7_REG_HASH(CCN_BASE_addr):
		if (addr<=0x1F000044)
		{
			return sh4_rio_read(sz, CCN,addr & 0xFF);
		}
		else
		{
			OUT_OF_RANGE("CCN");
			return 0;
		}
		break;

	case A7_REG_HASH(UBC_BASE_addr):
		if (addr<=0x1F200020)
		{
			return sh4_rio_read(sz, UBC,addr & 0xFF);
		}
		else
		{
			OUT_OF_RANGE("UBC");
			return 0;
		}
		break;

	case A7_REG_HASH(BSC_BASE_addr):
		if (addr<=0x1F800048)
		{
			return sh4_rio_read(sz, BSC,addr & 0xFF);
		}
		else
		{
			OUT_OF_RANGE("BSC");
			return 0;
		}
		break;
	case A7_REG_HASH(BSC_SDMR2_addr):
		/* dram settings 2 / write only */
		INFO_LOG(SH4, "Read from write-only registers [dram settings 2]\n");
		return 0;
	case A7_REG_HASH(BSC_SDMR3_addr):
		/* dram settings 3 / write only */
		INFO_LOG(SH4, "Read from write-only registers [dram settings 3]\n");
		return 0;



	case A7_REG_HASH(DMAC_BASE_addr):
		if (addr<=0x1FA00040)
		{
			return sh4_rio_read(sz, DMAC,addr & 0xFF);
		}
		else
		{
			OUT_OF_RANGE("DMAC");
			return 0;
		}
		break;

	case A7_REG_HASH(CPG_BASE_addr):
		if (addr<=0x1FC00010)
		{
			return sh4_rio_read(sz, CPG,addr & 0xFF);
		}
		else
		{
			OUT_OF_RANGE("CPG");
			return 0;
		}
		break;

	case A7_REG_HASH(RTC_BASE_addr):
		if (addr<=0x1FC8003C)
		{
			return sh4_rio_read(sz, RTC,addr & 0xFF);
		}
		else
		{
			OUT_OF_RANGE("RTC");
			return 0;
		}
		break;

	case A7_REG_HASH(INTC_BASE_addr):
		if (addr<=0x1FD00010)
		{
			return sh4_rio_read(sz, INTC,addr & 0xFF);
		}
		else
		{
			OUT_OF_RANGE("INTC");
			return 0;
		}
		break;

	case A7_REG_HASH(TMU_BASE_addr):
		if (addr<=0x1FD8002C)
		{
			return sh4_rio_read(sz, TMU,addr & 0xFF);
		}
		else
		{
			OUT_OF_RANGE("TMU");
			return 0;
		}
		break;

	case A7_REG_HASH(SCI_BASE_addr):
		if (addr<=0x1FE0001C)
		{
			return sh4_rio_read(sz, SCI,addr & 0xFF);
		}
		else
		{
			OUT_OF_RANGE("SCI");
			return 0;
		}
		break;

	case A7_REG_HASH(SCIF_BASE_addr):
		if (addr<=0x1FE80024)
		{
			return sh4_rio_read(sz, SCIF,addr & 0xFF);
		}
		else
		{
			OUT_OF_RANGE("SCIF");
			return 0;
		}
		break;

		/* Who really cares about ht-UDI? it's not existent on the Dreamcast IIRC */
	case A7_REG_HASH(UDI_BASE_addr):
		switch(addr)
		{
			/* UDI SDIR 0x1FF00000 0x1FF00000 16 0xFFFF Held Held Held Pclk */
		case UDI_SDIR_addr :
			break;


			/* UDI SDDR 0x1FF00008 0x1FF00008 32 Held Held Held Held Pclk */
		case UDI_SDDR_addr :
			break;
		}
		break;
	}

   INFO_LOG(SH4, "Unknown Read from P4 mmr - addr=%x\n", addr);
	return 0;
}

/* Write P4 memory-mapped registers */
static INLINE void write_p4mmr(u32 addr, u32 data, u32 sz)
{
	u32 map_base;
	if (likely(addr==0xFF000038))
	{
		CCN_QACR_write(0, data);
		return;
	}
	else if (likely(addr==0xFF00003C))
	{
		CCN_QACR_write(1, data);
		return;
	}	

	addr&=0x1FFFFFFF;
	map_base=addr>>16;
	switch (map_base & 0x1FFF)
	{

	case A7_REG_HASH(CCN_BASE_addr):
		if (addr<=0x1F00003C)
		{
			sh4_rio_write(sz, CCN,CCN_wmask,addr & 0xFF,data);
		}
		else
		{
			OUT_OF_RANGE("CCN");
		}
		return;

	case A7_REG_HASH(UBC_BASE_addr):
		if (addr<=0x1F200020)
		{
			sh4_rio_write(sz, UBC,UBC_wmask,addr & 0xFF,data);
		}
		else
		{
			OUT_OF_RANGE("UBC");
		}
		return;

	case A7_REG_HASH(BSC_BASE_addr):
		if (addr<=0x1F800048)
		{
			sh4_rio_write(sz, BSC,BSC_wmask,addr & 0xFF,data);
		}
		else
		{
			OUT_OF_RANGE("BSC");
		}
		return;
	case A7_REG_HASH(BSC_SDMR2_addr):
		/* dram settings 2 / write only */
		return;

	case A7_REG_HASH(BSC_SDMR3_addr):
		/* dram settings 3 / write only */
		return;


	case A7_REG_HASH(DMAC_BASE_addr):
		if (addr<=0x1FA00040)
		{
			sh4_rio_write(sz, DMAC,DMAC_wmask,addr & 0xFF,data);
		}
		else
		{
			OUT_OF_RANGE("DMAC");
		}
		return;

	case A7_REG_HASH(CPG_BASE_addr):
		if (addr<=0x1FC00010)
		{
			sh4_rio_write(sz, CPG,CPG_wmask,addr & 0xFF,data);
		}
		else
		{
			OUT_OF_RANGE("CPG");
		}
		return;

	case A7_REG_HASH(RTC_BASE_addr):
		if (addr<=0x1FC8003C)
		{
			sh4_rio_write(sz, RTC,RTC_wmask,addr & 0xFF,data);
		}
		else
		{
			OUT_OF_RANGE("RTC");
		}
		return;

	case A7_REG_HASH(INTC_BASE_addr):
		if (addr<=0x1FD0000C)
		{
			sh4_rio_write(sz, INTC,INTC_wmask,addr & 0xFF,data);
		}
		else
		{
			OUT_OF_RANGE("INTC");
		}
		return;

	case A7_REG_HASH(TMU_BASE_addr):
		if (addr<=0x1FD8002C)
		{
			sh4_rio_write(sz, TMU,TMU_wmask,addr & 0xFF,data);
		}
		else
		{
			OUT_OF_RANGE("TMU");
		}
		return;

	case A7_REG_HASH(SCI_BASE_addr):
		if (addr<=0x1FE0001C)
		{
			sh4_rio_write(sz, SCI,SCI_wmask,addr & 0xFF,data);
		}
		else
		{
			OUT_OF_RANGE("SCI");
		}
		return;

	case A7_REG_HASH(SCIF_BASE_addr):
		if (addr<=0x1FE80024)
		{
			sh4_rio_write(sz, SCIF,SCIF_wmask,addr & 0xFF,data);
		}
		else
		{
			OUT_OF_RANGE("SCIF");
		}
		return;

		/* who really cares about ht-udi ? it's not existent on dc iirc .. */
	case A7_REG_HASH(UDI_BASE_addr):
		switch(addr)
		{
			/* UDI SDIR 0xFFF00000 0x1FF00000 16 0xFFFF Held Held Held Pclk */
		case UDI_SDIR_addr :
			break;


			/* UDI SDDR 0xFFF00008 0x1FF00008 32 Held Held Held Held Pclk */
		case UDI_SDDR_addr :
			break;
		}
		break;
	}

   INFO_LOG(SH4, "Write to P4 mmr not implemented, addr=%x, data=%x\n", addr, data);
}



/* *********** */
/* On Chip Ram */
/* *********** */
/* The cache used as RAM is two areas of 4 KB. Which of the two an address
 * is in is said by its bit 13, or by bit 25 when CCR.OIX is set; bit 12 is
 * not looked at, so each area is seen twice over. */
static INLINE u32 ocr_offset(u32 addr)
{
	return ((addr >> (CCN_CCR.OIX == 1 ? 13 : 1)) & 0x1000) | (addr & 0xfff);
}

/* The handlers by size, of what is above by size: each a function of its
 * own, the size a constant in it */
#define SH4_MMR_SIZED(name, type, bytes) \
	static type DYNACALL ReadMem_##name##_##bytes(u32 addr) { return (type)read_##name(addr, bytes); } \
	static void DYNACALL WriteMem_##name##_##bytes(u32 addr, type data) { write_##name(addr, data, bytes); }
#define SH4_MMR_HANDLERS(name) \
	SH4_MMR_SIZED(name, u8, 1) \
	SH4_MMR_SIZED(name, u16, 2) \
	SH4_MMR_SIZED(name, u32, 4)
SH4_MMR_HANDLERS(p4)
SH4_MMR_HANDLERS(p4mmr)
#define SH4_MMR_REGISTER(name) _vmem_register_handler( \
	ReadMem_##name##_1, ReadMem_##name##_2, ReadMem_##name##_4, \
	WriteMem_##name##_1, WriteMem_##name##_2, WriteMem_##name##_4)

#define SH4_OCR(type, bytes) \
	static type DYNACALL ReadMem_area7_OCR_##bytes(u32 addr) \
	{ \
		if (CCN_CCR.ORA == 1) \
			return *(type *)&OnChipRAM[ocr_offset(addr)]; \
		INFO_LOG(SH4, "On Chip Ram Read, but OCR is disabled. addr %x\n", addr); \
		return 0; \
	} \
	static void DYNACALL WriteMem_area7_OCR_##bytes(u32 addr, type data) \
	{ \
		if (CCN_CCR.ORA == 1) \
			*(type *)&OnChipRAM[ocr_offset(addr)] = data; \
		else \
			INFO_LOG(SH4, "On Chip Ram Write, but OCR is disabled. addr %x\n", addr); \
	}
SH4_OCR(u8, 1)
SH4_OCR(u16, 2)
SH4_OCR(u32, 4)

/* Init/Res/Term */
void sh4_mmr_init(void)
{
	u32 i, j;

	for (i = 0; i < SH4_MODULES; i++)
		for (j = 0; j < sh4_modules[i].count; j++)
			sh4_modules[i].wmask[j] = 0xFFFFFFFF;

	for (i=0;i<30;i++)
	{
		if (i<SH4_CCN_REGS)  sh4_rio_reg(CCN,CCN_BASE_addr+i*4,RIO_NO_ACCESS,32,0,0);   /* (16,true);    //CCN  : 14 registers */
		if (i<SH4_UBC_REGS)  sh4_rio_reg(UBC,UBC_BASE_addr+i*4,RIO_NO_ACCESS,32,0,0);   /* (9,true);     //UBC  : 9 registers */
		if (i<SH4_BSC_REGS)  sh4_rio_reg(BSC,BSC_BASE_addr+i*4,RIO_NO_ACCESS,32,0,0);   /* (19,true);    //BSC  : 18 registers */
		if (i<SH4_DMAC_REGS) sh4_rio_reg(DMAC,DMAC_BASE_addr+i*4,RIO_NO_ACCESS,32,0,0); /* (17,true);    //DMAC : 17 registers */
		if (i<SH4_CPG_REGS)  sh4_rio_reg(CPG,CPG_BASE_addr+i*4,RIO_NO_ACCESS,32,0,0);   /* (5,true);     //CPG  : 5 registers */
		if (i<SH4_RTC_REGS)  sh4_rio_reg(RTC,RTC_BASE_addr+i*4,RIO_NO_ACCESS,32,0,0);   /* (16,true);    //RTC  : 16 registers */
		if (i<SH4_INTC_REGS) sh4_rio_reg(INTC,INTC_BASE_addr+i*4,RIO_NO_ACCESS,32,0,0); /* (4,true);     //INTC : 4 registers */
		if (i<SH4_TMU_REGS)  sh4_rio_reg(TMU,TMU_BASE_addr+i*4,RIO_NO_ACCESS,32,0,0);   /* (12,true);    //TMU  : 12 registers */
		if (i<SH4_SCI_REGS)  sh4_rio_reg(SCI,SCI_BASE_addr+i*4,RIO_NO_ACCESS,32,0,0);   /* (8,true);     //SCI  : 8 registers */
		if (i<SH4_SCIF_REGS) sh4_rio_reg(SCIF,SCIF_BASE_addr+i*4,RIO_NO_ACCESS,32,0,0); /* (10,true);    //SCIF : 10 registers */
	}

	/* initialise Register structs */
	bsc_init();
	ccn_init();
	cpg_init();
	dmac_init();
	intc_init();
	rtc_init();
	serial_init();
	tmu_init();
	ubc_init();
}

void sh4_mmr_reset(bool hard)
{
	if (hard)
	{
		u32 i, j;
		for (i = 0; i < SH4_MODULES; i++)
			for (j = 0; j < sh4_modules[i].count; j++)
				register_reset(&sh4_modules[i].regs[j]);
	}
	memset(OnChipRAM, 0, sizeof(OnChipRAM));
	/* Reset register values */
	bsc_reset(hard);
	ccn_reset(hard);
	cpg_reset();
	dmac_reset();
	intc_reset();
	rtc_reset();
	serial_reset();
	tmu_reset(hard);
	ubc_reset();
}

void sh4_mmr_term(void)
{
	/* free any alloc'd resources [if any] */
	ubc_term();
	tmu_term();
	serial_term();
	rtc_term();
	intc_term();
	dmac_term();
	cpg_term();
	ccn_term();
	bsc_term();
}

/* AREA 7--Sh4 Regs */
static _vmem_handler p4mmr_handler;
static _vmem_handler area7_ocr_handler;

void map_area7_init(void)
{
   p4mmr_handler = SH4_MMR_REGISTER(p4mmr);
	area7_ocr_handler = SH4_MMR_REGISTER(area7_OCR);
}

void map_area7(u32 base)
{
   /* on-chip RAM: 7C000000-7FFFFFFF */
	if (base == 0x60)
		_vmem_map_handler(area7_ocr_handler, 0x7C, 0x7F);
}

/* P4 */
void map_p4(void)
{
	/* P4 Region : */
   _vmem_handler p4_handler = SH4_MMR_REGISTER(p4);

	/* the cache and TLB arrays. What P4 has nothing of its own in is left as */
	/* mem_map_default() made it: the same as everywhere else */
	_vmem_map_handler(p4_handler,0xF0,0xF7);

	/* Store Queues -- Write only 32bit */
	_vmem_map_block(sq_both,0xE0,0xE0,63);
	_vmem_map_block(sq_both,0xE1,0xE1,63);
	_vmem_map_block(sq_both,0xE2,0xE2,63);
	_vmem_map_block(sq_both,0xE3,0xE3,63);

   _vmem_map_handler(p4mmr_handler, 0xFF, 0xFF);
}
