/*
	aica interface
		Handles RTC, Display mode reg && arm reset reg !
	arm7 is handled on a separate arm plugin now
*/

#include "aica_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/holly/sb.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/dyna/blockmanager.h"

#include <time.h>

VArray2 aica_ram;
u32 VREG;
u32 ARMRST;
u32 rtc_EN;
int dma_sched_id;
/* SB_ADST. It is read through Read_SB_ADST(), and a register read through
 * a function keeps the function's address where the others keep their
 * value, so its value is kept here; in a save state from V29. */
u32 sb_adst;
u32 RealTimeClock;
int rtc_schid = -1;

u32 GetRTC_now(void)
{
	/* The Dreamcast Epoch time is 1/1/50 00:00 but without support for time zone or DST. */
	/* We compute the TZ/DST current time offset and add it to the result */
	/* as if we were in the UTC time zone (as well as the DC Epoch) */
	time_t rawtime = time(NULL);
	struct tm localtm, gmtm;
	time_t time_offset;
	localtm = *localtime(&rawtime);
	gmtm = *gmtime(&rawtime);
	gmtm.tm_isdst = -1;
	time_offset = mktime(&localtm) - mktime(&gmtm);
	/* 1/1/50 to 1/1/70 is 20 years and 5 leap days */
	return (20 * 365 + 5) * 24 * 60 * 60 + rawtime + time_offset;
}

u32 ReadMem_aica_rtc(u32 addr,u32 sz)
{
	switch( addr & 0xFF )
	{
	case 0:
		return RealTimeClock>>16;
	case 4:
		return RealTimeClock &0xFFFF;
	case 8:
		return 0;
	}

	WARN_LOG(AICA, "ReadMem_aica_rtc : invalid address %x sz %d\n", addr, sz);
	return 0;
}

void WriteMem_aica_rtc(u32 addr,u32 data,u32 sz)
{
	switch( addr & 0xFF )
	{
	case 0:
		if (rtc_EN)
		{
			RealTimeClock&=0xFFFF;
			RealTimeClock|=(data&0xFFFF)<<16;
			rtc_EN=0;
		}
		return;
	case 4:
		if (rtc_EN)
		{
			RealTimeClock&=0xFFFF0000;
			RealTimeClock|= data&0xFFFF;
			/* TODO: Clean the internal timer ? */
		}
		return;
	case 8:
		rtc_EN=data&1;
		return;
	}
}

u32 ReadMem_aica_reg(u32 addr,u32 sz)
{
	addr&=0x7FFF;
	if (sz==1)
	{
		if (addr==0x2C01)
		{
			return VREG;
		}
		else if (addr==0x2C00)
		{
			return ARMRST;
		}
		else
		{
			return libAICA_ReadReg(addr, sz);
		}
	}
	else
	{
		if (addr==0x2C00)
		{
			return (VREG<<8) | ARMRST;
		}
		else
		{
			return libAICA_ReadReg(addr, sz);
		}
	}
}

static void ArmSetRST(void)
{
	ARMRST &= 1;
   aicaarm_enable(ARMRST == 0);
}
void WriteMem_aica_reg(u32 addr,u32 data,u32 sz)
{
	addr&=0x7FFF;

	if (sz==1)
	{
		if (addr==0x2C01)
		{
			VREG=data;
			INFO_LOG(AICA_ARM, "VREG = %02X\n", VREG);
		}
		else if (addr==0x2C00)
		{
			ARMRST=data;
			INFO_LOG(AICA_ARM, "ARMRST = %02X\n", ARMRST);
			ArmSetRST();
		}
		else
		{
			libAICA_WriteReg(addr,data,sz);
		}
	}
	else
	{
		if (addr==0x2C00)
		{
			VREG=(data>>8)&0xFF;
			ARMRST=data&0xFF;
			INFO_LOG(AICA_ARM, "VREG = %02X ARMRST %02X\n", VREG, ARMRST);
			ArmSetRST();
		}
		else
		{
			libAICA_WriteReg(addr,data,sz);
		}
	}
}

static int DreamcastSecond(int tag, int c, int j)
{
	RealTimeClock++;

#if FEAT_SHREC != DYNAREC_NONE
	bm_Periodical_1s();
#endif

	return SH4_MAIN_CLOCK;
}

/* Init/res/term */
void aica_Init(void)
{
	RealTimeClock = GetRTC_now();
	/* (asked for by the reset that follows) */
	rtc_schid = sh4_sched_register(0, &DreamcastSecond);
}

void aica_Reset(bool hard)
{
	if (hard)
		RealTimeClock = GetRTC_now();
	VREG = 0;
	ARMRST = 0;
	/* The clock runs on its battery through a reset: a second that is on
	 * its way comes when it was going to. It is asked for here only when
	 * there is none, which is the first reset of a machine. */
	if (!sh4_sched_is_scheduled(rtc_schid))
		sh4_sched_request(rtc_schid, SH4_MAIN_CLOCK);
}

static int dma_end_sched(int tag, int cycl, int jitt)
{
	u32 len=SB_ADLEN & 0x7FFFFFFF;
	if (SB_ADLEN & 0x80000000)
		SB_ADEN = 0;
	else
		SB_ADEN = 1;

	SB_ADSTAR+=len;
	SB_ADSTAG+=len;
	SB_ADST = 0;	/* dma done */
	SB_ADLEN = 0;

	/* indicate that dma is not happening, or has been paused */
	SB_ADSUSP |= 0x10;

	asic_RaiseInterrupt(holly_SPU_DMA);

	return 0;
}

static bool check_STAG(u32 addr)
{
#ifdef STRICT_MODE
	const u32 area = (addr >> 26) & 7;
	/* Aica and G2 Ext dev #1 and #2 on area 0 */
	/* G2 Ext dev #3 on area 5 */
	return area == 0 || area == 5;
#else
	return true;
#endif
}

static bool check_STAR(u32 addr)
{
#ifdef STRICT_MODE
	const u32 area = (addr >> 26) & 7;
	/* System RAM and VRAM */
	return area == 3 || area == 1;
#else
	return true;
#endif
}

static bool check_STAR_overrun(u32 addr)
{
#ifdef STRICT_MODE
	u32 bottom = (((SB_G2APRO >> 8) & 0x7f) << 20) | 0x08000000;
	u32 top = ((SB_G2APRO & 0x7f) << 20) | 0x080fffe0;

	return addr >= bottom && addr <= top;
#else
	return true;
#endif
}

/* The three G2 channels other than the AICA's (and the AICA's, for its
 * address registers): their registers and interrupts */
typedef struct g2_dma
{
	u32 en, st, star, stag, len, dir;
	HollyInterruptID done, illaddr, overrun;
	const char *tag;
} g2_dma;

static const g2_dma g2_dmas[] = {
	{ SB_ADEN_addr, SB_ADST_addr, SB_ADSTAR_addr, SB_ADSTAG_addr, SB_ADLEN_addr, SB_ADDIR_addr,
			holly_SPU_DMA, holly_AICA_ILLADDR, holly_AICA_OVERRUN, "G2-AICA DMA" },
	{ SB_E1EN_addr, SB_E1ST_addr, SB_E1STAR_addr, SB_E1STAG_addr, SB_E1LEN_addr, SB_E1DIR_addr,
			holly_EXT_DMA1, holly_EXT1_ILLADDR, holly_EXT1_OVERRUN, "G2-EXT1 DMA" },
	{ SB_E2EN_addr, SB_E2ST_addr, SB_E2STAR_addr, SB_E2STAG_addr, SB_E2LEN_addr, SB_E2DIR_addr,
			holly_EXT_DMA2, holly_EXT2_ILLADDR, holly_EXT2_OVERRUN, "G2-EXT2 DMA" },
	{ SB_DDEN_addr, SB_DDST_addr, SB_DDSTAR_addr, SB_DDSTAG_addr, SB_DDLEN_addr, SB_DDDIR_addr,
			holly_DEV_DMA, holly_DEV_ILLADDR, holly_DEV_OVERRUN, "G2-DDev DMA" },
};

/* The channel one of whose registers is at @addr */
static const g2_dma *g2_dma_of(u32 addr)
{
	unsigned i;
	addr &= ~3u;
	for (i = 1; i < sizeof(g2_dmas) / sizeof(g2_dmas[0]); i++)
		if (addr == g2_dmas[i].st || addr == g2_dmas[i].star || addr == g2_dmas[i].stag)
			return &g2_dmas[i];
	return &g2_dmas[0];
}

#define G2_REG(a) sb_regs[((a) - SB_BASE) / 4].data32

static void Write_DmaStart(u32 addr, u32 data)
{
	const g2_dma *c = g2_dma_of(addr);
	const u32 dirReg = G2_REG(c->dir);
	u32 src, dst, len;

	if (!(data & 1) || !(G2_REG(c->en) & 1))
		return;
	src = G2_REG(c->star);
	dst = G2_REG(c->stag);
	len = G2_REG(c->len) & 0x7FFFFFFF;

	/* STAR */
	if (!check_STAR(src))
	{
		INFO_LOG(AICA, "%s: Invalid src address %08x\n", c->tag, src);
		G2_REG(c->st) = 0;
		G2_REG(c->en) = 0;
		asic_RaiseInterrupt(c->illaddr);
		return;
	}
	/* STAG */
	if (!check_STAG(dst))
	{
		INFO_LOG(AICA, "%s: Invalid dst address %08x\n", c->tag, dst);
		G2_REG(c->st) = 0;
		G2_REG(c->en) = 0;
		asic_RaiseInterrupt(c->illaddr);
		return;
	}
	/* Overrun */
	if (!check_STAR_overrun(src) || !check_STAR_overrun(src + len - 1))
	{
		INFO_LOG(AICA, "%s: Overrun address %08x len %x\n", c->tag, src, len);
		G2_REG(c->st) = 0;
		G2_REG(c->en) = 0;
		asic_RaiseInterrupt(c->overrun);
		return;
	}

	if (dirReg == 1)
	{
		u32 t = src;
		src = dst;
		dst = t;
		DEBUG_LOG(AICA, "%s: DIR == 1 DMA Read to 0x%X from 0x%X %d bytes\n", c->tag, dst, src, len);
	}
	else
		DEBUG_LOG(AICA, "%s: DIR == 0 DMA Write to 0x%X from 0x%X %d bytes\n", c->tag, dst, src, len);

	WriteMemBlock_nommu_dma(dst, src, len);

	if (G2_REG(c->len) & 0x80000000)
		G2_REG(c->en) = 0;
	else
		G2_REG(c->en) = 1;

	G2_REG(c->star) += len;
	G2_REG(c->stag) += len;
	G2_REG(c->st) = 0; /* dma done */
	G2_REG(c->len) = 0;

	asic_RaiseInterrupt(c->done);
}

static void Write_SB_ADST(u32 addr, u32 data)
{
	/* 0x005F7800	SB_ADSTAG	RW	AICA:G2-DMA G2 start address */
	/* 0x005F7804	SB_ADSTAR	RW	AICA:G2-DMA system memory start address */
	/* 0x005F7808	SB_ADLEN	RW	AICA:G2-DMA length */
	/* 0x005F780C	SB_ADDIR	RW	AICA:G2-DMA direction */
	/* 0x005F7810	SB_ADTSEL	RW	AICA:G2-DMA trigger select */
	/* 0x005F7814	SB_ADEN		RW	AICA:G2-DMA enable */
	/* 0x005F7818	SB_ADST		RW	AICA:G2-DMA start */
	/* 0x005F781C	SB_ADSUSP	RW	AICA:G2-DMA suspend */
	
	/* a start while a transfer is under way is not taken: the one under way goes on to its end */
	if ((data&1) && !(SB_ADST&1))
	{
		if (SB_ADEN&1)
		{
			u32 src = SB_ADSTAR;
			u32 dst = SB_ADSTAG;
			u32 len = SB_ADLEN & 0x7FFFFFFF;
			int cycles;

			if (!check_STAR(src))
			{
				INFO_LOG(AICA, "AICA-DMA : Invalid src address %08x\n", src);
				SB_ADST = 0;
				SB_ADEN = 0;
				asic_RaiseInterrupt(holly_AICA_ILLADDR);
				return;
			}
			if (!check_STAG(dst))
			{
				INFO_LOG(AICA, "AICA-DMA : Invalid dst address %08x\n", dst);
				SB_ADST = 0;
				SB_ADEN = 0;
				asic_RaiseInterrupt(holly_AICA_ILLADDR);
				return;
			}
			/* Overrun */
			if (!check_STAR_overrun(src) || !check_STAR_overrun(src + len - 1))
			{
				INFO_LOG(AICA, "AICA-DMA : Overrun address %08x len %x\n", src, len);
				SB_ADST = 0;
				SB_ADEN = 0;
				asic_RaiseInterrupt(holly_AICA_OVERRUN);
				return;
			}

			if ((SB_ADDIR&1)==1)
			{
				/* swap direction */
				u32 tmp=src;
				src=dst;
				dst=tmp;
				DEBUG_LOG(AICA, "AICA-DMA : SB_ADDIR==1 DMA Read to 0x%X from 0x%X %x bytes\n", dst, src, SB_ADLEN);
			}
			else
				DEBUG_LOG(AICA, "AICA-DMA : SB_ADDIR==0:DMA Write to 0x%X from 0x%X %x bytes\n", dst, src, SB_ADLEN);

			WriteMemBlock_nommu_dma(dst,src,len);

			/* indicate that dma is in progress */
			SB_ADST = 1;
			SB_ADSUSP &= ~0x10;

			/* Schedule the end of DMA transfer interrupt */
			cycles = len * (SH4_MAIN_CLOCK / 2 / 25000000);       /* 16 bits @ 25 MHz */
			/* Only a very short transfer ends at once: Sports Jam's
			 * announcer and The Nomad Soul need longer ones to take the
			 * time they take. */
			if (cycles <= 512)
				dma_end_sched(0, 0, 0);
			else
				sh4_sched_request(dma_sched_id, cycles);
		}
	}
}

/* Test Drive Le Mans and Looney Tunes Space Race sometimes send the same
 * transfer twice: they read SB_ADST as 0 and start it again before they
 * have taken the end-of-transfer interrupt. The first read with that
 * interrupt pending says the transfer is still under way; bit 1, which the
 * register does not show, marks that this was said, so that the next read
 * tells the truth. (The register is in the state with its bit 1.) */
static u32 Read_SB_ADST(u32 addr)
{
	if ((SB_ISTNRM & (1 << (u8)holly_SPU_DMA)) && !(SB_ADST & 2))
	{
		SB_ADST |= 2;
		return 1;
	}
	SB_ADST &= ~2;
	return SB_ADST;
}

static void Write_SB_STAG(u32 addr, u32 data)
{
	const g2_dma *c = g2_dma_of(addr);
	G2_REG(c->stag) = data & 0x1FFFFFE0;

	if (!check_STAG(data))
	{
		INFO_LOG(AICA, "%s Write_SB_STAG: Invalid address %08x\n", c->tag, data);
		asic_RaiseInterrupt(c->illaddr);
	}
}

static void Write_SB_STAR(u32 addr, u32 data)
{
	const g2_dma *c = g2_dma_of(addr);
	G2_REG(c->star) = data & 0x1FFFFFE0;

	if (!check_STAR(data))
	{
		INFO_LOG(AICA, "%s Write_SB_STAR: Invalid address %08x\n", c->tag, data);
		asic_RaiseInterrupt(c->illaddr);
	}
}

void Write_SB_G2APRO(u32 addr, u32 data)
{
	if ((data >> 16) == 0x4659)
		SB_G2APRO = data & 0x00007f7f;
}

void aica_sb_Init(void)
{
	/* G2-DMA registers */

	/* AICA */
	sb_rio_register(SB_ADST_addr, RIO_FUNC, &Read_SB_ADST, &Write_SB_ADST);
#ifdef STRICT_MODE
	sb_rio_register(SB_ADSTAR_addr, RIO_WF, NULL, &Write_SB_STAR);
	sb_rio_register(SB_ADSTAG_addr, RIO_WF, NULL, &Write_SB_STAG);
#endif

	/* G2 Ext device #1 */
	sb_rio_register(SB_E1ST_addr, RIO_WF, NULL, &Write_DmaStart);
	sb_rio_register(SB_E1STAR_addr, RIO_WF, NULL, &Write_SB_STAR);
	sb_rio_register(SB_E1STAG_addr, RIO_WF, NULL, &Write_SB_STAG);

	/* G2 Ext device #2 */
	sb_rio_register(SB_E2ST_addr, RIO_WF, NULL, &Write_DmaStart);
	sb_rio_register(SB_E2STAR_addr, RIO_WF, NULL, &Write_SB_STAR);
	sb_rio_register(SB_E2STAG_addr, RIO_WF, NULL, &Write_SB_STAG);

	/* G2 Ext device #3 */
	sb_rio_register(SB_DDST_addr, RIO_WF, NULL, &Write_DmaStart);
	sb_rio_register(SB_DDSTAR_addr, RIO_WF, NULL, &Write_SB_STAR);
	sb_rio_register(SB_DDSTAG_addr, RIO_WF, NULL, &Write_SB_STAG);

	sb_rio_register(SB_G2APRO_addr, RIO_WO_FUNC, NULL, &Write_SB_G2APRO);

	dma_sched_id = sh4_sched_register(0, &dma_end_sched);
}

void aica_sb_Reset(bool hard)
{
	/* A transfer that was under way: its end, with its interrupt, is not
	 * for the machine that starts now. (A hard reset has put the registers
	 * back, SB_ADST with them. A soft one leaves them as they are, with
	 * the transfer still marked as under way, and so the end of it is left
	 * to come and say it is over.) */
	if (hard && sh4_sched_is_scheduled(dma_sched_id))
		sh4_sched_request(dma_sched_id, -1);
	if (hard)
		sb_adst = 0;
}

void aica_sb_serialize_v29(void **data, unsigned int *total_size)
{
	LIBRETRO_S(sb_adst);
}

bool aica_sb_unserialize_v29(void **data, unsigned int *total_size)
{
	LIBRETRO_US(sb_adst);
	/* bit 0, a transfer under way, and bit 1, its end told once (see above) */
	sb_adst &= 3;
	return true;
}

/* An older state: what it had of SB_ADST went to a dummy, and the transfer
 * under way is known by its end being on its way, which the check after
 * the scheduler's events are back in place (dc_unserialize()) settles. */
void aica_sb_state_before_v29(void)
{
	sb_adst = 1;
}

void aica_sb_Term(void)
{
}
