#include <string.h>
#include "mmu.h"
#include "modules.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h"

TLB_Entry UTLB[64];
TLB_Entry ITLB[4];
u32 ITLB_LRU_USE[64];

/* SQ fast remap , mainly hackish , assumes 1MB pages */
/* max 64MB can be remapped on SQ */
/* Used when FullMMU is off */
u32 sq_remap[64];

#if defined(NO_MMU)

/* Sync memory mapping to MMU , suspend compiled blocks if needed.entry is a UTLB entry # , -1 is for full sync */
bool UTLB_Sync(u32 entry)
{	
	if ((UTLB[entry].Address.VPN & (0xFC000000 >> 10)) == (0xE0000000 >> 10))
	{
		u32 vpn_sq = ((UTLB[entry].Address.VPN & 0x7FFFF) >> 10) & 0x3F;/* upper bits are always known [0xE0/E1/E2/E3] */
		sq_remap[vpn_sq] = UTLB[entry].Data.PPN << 10;
		INFO_LOG(SH4, "SQ remap %d : 0x%X to 0x%X\n", entry, UTLB[entry].Address.VPN << 10, UTLB[entry].Data.PPN << 10);
	}
	else
	{
		INFO_LOG(SH4, "MEM remap %d : 0x%X to 0x%X\n", entry, UTLB[entry].Address.VPN << 10, UTLB[entry].Data.PPN << 10);
	}

	return true;
}
/* Sync memory mapping to MMU, suspend compiled blocks if needed.entry is a ITLB entry # , -1 is for full sync */
void ITLB_Sync(u32 entry)
{
	INFO_LOG(SH4, "ITLB MEM remap %d : 0x%X to 0x%X\n",entry,ITLB[entry].Address.VPN<<10,ITLB[entry].Data.PPN<<10);
}

void mmu_forget(u32 va)
{
}

void mmu_utlb_written(u32 entry)
{
	UTLB_Sync(entry);
}

void mmu_set_state(void)
{
}

void MMU_init(void)
{

}

void MMU_reset(void)
{
	memset(UTLB,0,sizeof(UTLB));
	memset(ITLB,0,sizeof(ITLB));
}

void MMU_term(void)
{
}
#else
/*
MMU support code
This is mostly hacked-on as the core was never meant to have mmu support

There are two modes, one with 'full' mmu emulation (for wince/bleem/wtfever)
and a fast-hack mode for 1mb sqremaps (for katana)

defining NO_MMU disables the full mmu emulation
*/
#include "mmu.h"
#include "modules.h"
#include "mmu_impl.h"
#include "hw/sh4/sh4_if.h"
#include "ccn.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"

#include "hw/mem/_vmem.h"

#define printf_mmu(...) DEBUG_LOG(SH4, __VA_ARGS__)

const u32 mmu_mask[4] =
{
	((0xFFFFFFFF) >> 10) << 10,	/* 1 kb page */
	((0xFFFFFFFF) >> 12) << 12,	/* 4 kb page */
	((0xFFFFFFFF) >> 16) << 16,	/* 64 kb page */
	((0xFFFFFFFF) >> 20) << 20	/* 1 MB page */
};

const u32 fast_reg_lut[8] =
{
	0, 0, 0, 0	/* P0-U0 */
	, 1		/* P1 */
	, 1		/* P2 */
	, 0		/* P3 */
	, 1		/* P4 */
};

static const u32 ITLB_LRU_OR[4] =
{
	0x00,/* 000xxx */
	0x20,/* 1xx00x */
	0x14,/* x1x1x0 */
	0x0B,/* xx1x11 */
};
static const u32 ITLB_LRU_AND[4] =
{
	0x07,/* 000xxx */
	0x39,/* 1xx00x */
	0x3E,/* x1x1x0 */
	0x3F,/* xx1x11 */
};

#ifndef FAST_MMU
/* sync mem mapping to mmu , suspend compiled blocks if needed.entry is a UTLB entry # , -1 is for full sync */
bool UTLB_Sync(u32 entry)
{
	printf_mmu("UTLB MEM remap %d : 0x%X to 0x%X : %d asid %d size %d\n", entry, UTLB[entry].Address.VPN << 10, UTLB[entry].Data.PPN << 10, UTLB[entry].Data.V,
			UTLB[entry].Address.ASID, UTLB[entry].Data.SZ0 + UTLB[entry].Data.SZ1 * 2);
	if (UTLB[entry].Data.V == 0)
		return true;

	if ((UTLB[entry].Address.VPN & (0xFC000000 >> 10)) == (0xE0000000 >> 10))
	{
		/* Used when FullMMU is off */
		u32 vpn_sq = ((UTLB[entry].Address.VPN & 0x7FFFF) >> 10) & 0x3F;/* upper bits are always known [0xE0/E1/E2/E3] */
		sq_remap[vpn_sq] = UTLB[entry].Data.PPN << 10;

		return true;
	}
	else
	{
		return false;
	}
}
void mmu_forget(u32 va)
{
	mmu_lut_forget(va & ~0xFFFFFu, 0x100000);
}

void mmu_utlb_written(u32 entry)
{
	mmu_lut_flush();
	UTLB_Sync(entry);
}

/* sync mem mapping to mmu , suspend compiled blocks if needed.entry is a ITLB entry # , -1 is for full sync */
void ITLB_Sync(u32 entry)
{
	printf_mmu("ITLB MEM remap %d : 0x%X to 0x%X : %d\n", entry, ITLB[entry].Address.VPN << 10, ITLB[entry].Data.PPN << 10, ITLB[entry].Data.V);
}
#endif

static void raise_mmu_exception(u32 expEvnt, u32 callVect)
{
#if !defined(NO_MMU)
	sh4_throw(next_pc - 2, expEvnt, callVect);
#else
	msgboxf("Can't raise exceptions yet", MBX_ICONERROR);
#endif
}

/* A page that the TLB does not have, for a program with a TLB of its own:
 * the counter that says which entry LDTLB writes moves on, and starts
 * again at the limit the program gave it, if it gave one. On the machine
 * it moves at every look in the TLB, so a program cannot count on what it
 * is; one that leaves the choice of entry to it (KallistiOS does) must not
 * have every page it asks for put in the same entry, each taking the place
 * of the last, or an instruction in one page that reads another never has
 * both.
 *
 * It used to move at every search the emulator made, and how many it
 * makes is the emulator's business: fewer while it remembers the last
 * answers, more after a state has been loaded. A game loaded from a state
 * was then on another path from the same game left alone. A page not
 * being there is the program's own doing, the same in both. */
static void mmu_tlb_missed(void)
{
	u32 urc;
	if (!mmu_strict)
		return;

	urc = (CCN_MMUCR.URC + 1) & 63;

	CCN_MMUCR.URC = urc == CCN_MMUCR.URB ? 0 : urc;
}

void mmu_raise_exception(u32 mmu_error, u32 address, u32 am)
{
	printf_mmu("mmu_raise_exception -> pc = 0x%X : \n", next_pc);
	CCN_TEA = address;
	CCN_PTEH.VPN = address >> 10;

	switch (mmu_error)
	{
		/* No error */
	case MMU_ERROR_NONE:
		die("Error : mmu_raise_exception(MMU_ERROR_NONE)");
		break;

		/* TLB miss */
	case MMU_ERROR_TLB_MISS:
		printf_mmu("MMU_ERROR_UTLB_MISS 0x%X, handled\n", address);
		mmu_tlb_missed();
		if (am == MMU_TT_DWRITE)			/* WTLBMISS - Write Data TLB Miss Exception */
			raise_mmu_exception(0x60, 0x400);
		else if (am == MMU_TT_DREAD)		/* RTLBMISS - Read Data TLB Miss Exception */
			raise_mmu_exception(0x40, 0x400);
		else							/* ITLBMISS - Instruction TLB Miss Exception */
			raise_mmu_exception(0x40, 0x400);
		return;

		/* TLB Multihit */
	case MMU_ERROR_TLB_MHIT:
		/* Two entries of the TLB for one address. On the machine that is
		 * a reset; here it used to be the end of the emulator (die()),
		 * which a program could then bring about at will. An exception
		 * with its own code, as upstream has it: the program is no
		 * better off, the frontend is. */
		INFO_LOG(SH4, "MMU_ERROR_TLB_MHIT @ 0x%X\n", address);
		raise_mmu_exception(0x140, 0x100);
		return;

		/* Mem is read/write protected (depends on translation type) */
	case MMU_ERROR_PROTECTED:
		printf_mmu("MMU_ERROR_PROTECTED 0x%X, handled\n", address);
		if (am == MMU_TT_DWRITE)			/* WRITEPROT - Write Data TLB Protection Violation Exception */
			raise_mmu_exception(0xC0, 0x100);
		else if (am == MMU_TT_DREAD)		/* READPROT - Data TLB Protection Violation Exception */
			raise_mmu_exception(0xA0, 0x100);
		else								/* READPROT - Instr TLB Protection Violation Exception */
			raise_mmu_exception(0xA0, 0x100);
		return;

		/* Mem is write protected , firstwrite */
	case MMU_ERROR_FIRSTWRITE:
		printf_mmu("MMU_ERROR_FIRSTWRITE\n");
		verify(am == MMU_TT_DWRITE);
		/* FIRSTWRITE - Initial Page Write Exception */
		raise_mmu_exception(0x80, 0x100);
		return;

		/* data read/write missasligned */
	case MMU_ERROR_BADADDR:
		if (am == MMU_TT_DWRITE)			/* WADDERR - Write Data Address Error */
		{
			printf_mmu("MMU_ERROR_BADADDR(dw) 0x%X\n", address);
			raise_mmu_exception(0x100, 0x100);
		}
		else if (am == MMU_TT_DREAD)		/* RADDERR - Read Data Address Error */
		{
			printf_mmu("MMU_ERROR_BADADDR(dr) 0x%X\n", address);
			raise_mmu_exception(0xE0, 0x100);
		}
		else							/* IADDERR - Instruction Address Error */
		{
			printf_mmu("MMU_ERROR_BADADDR(i) 0x%X\n", address);
			raise_mmu_exception(0xE0, 0x100);
		}
		return;

		/* Can't Execute */
	case MMU_ERROR_EXECPROT:
		INFO_LOG(SH4, "MMU_ERROR_EXECPROT 0x%X\n", address);

		/* EXECPROT - Instruction TLB Protection Violation Exception */
		raise_mmu_exception(0xA0, 0x100);
		return;
	}

	die("Unknown mmu_error");
}


void DoMMUException(u32 address, u32 mmu_error, u32 access_type)
{
	printf_mmu("DoMMUException -> pc = 0x%X : %d \n", next_pc, access_type);
	CCN_TEA = address;
	CCN_PTEH.VPN = address >> 10;

	switch (mmu_error)
	{
		/* No error */
	case MMU_ERROR_NONE:
		die("Error : mmu_raise_exception(MMU_ERROR_NONE)");
		break;

		/* TLB miss */
	case MMU_ERROR_TLB_MISS:
		printf_mmu("MMU_ERROR_UTLB_MISS 0x%X, handled\n", address);
		mmu_tlb_missed();
		if (access_type == MMU_TT_DWRITE)			/* WTLBMISS - Write Data TLB Miss Exception */
			Do_Exception(next_pc, 0x60, 0x400);
		else if (access_type == MMU_TT_DREAD)		/* RTLBMISS - Read Data TLB Miss Exception */
			Do_Exception(next_pc, 0x40, 0x400);
		else							/* ITLBMISS - Instruction TLB Miss Exception */
			Do_Exception(next_pc, 0x40, 0x400);

		return;
		break;

		/* TLB Multihit */
	case MMU_ERROR_TLB_MHIT:
		/* (see mmu_raise_exception()) */
		INFO_LOG(SH4, "MMU_ERROR_TLB_MHIT @ 0x%X\n", address);
		Do_Exception(next_pc, 0x140, 0x100);
		return;

		/* Mem is read/write protected (depends on translation type) */
	case MMU_ERROR_PROTECTED:
		printf_mmu("MMU_ERROR_PROTECTED 0x%X, handled\n", address);
		if (access_type == MMU_TT_DWRITE)			/* WRITEPROT - Write Data TLB Protection Violation Exception */
			Do_Exception(next_pc, 0xC0, 0x100);
		else if (access_type == MMU_TT_DREAD)		/* READPROT - Data TLB Protection Violation Exception */
			Do_Exception(next_pc, 0xA0, 0x100);
		else
		{
			verify(false);
		}
		return;
		break;

		/* Mem is write protected , firstwrite */
	case MMU_ERROR_FIRSTWRITE:
		printf_mmu("MMU_ERROR_FIRSTWRITE\n");
		verify(access_type == MMU_TT_DWRITE);
		/* FIRSTWRITE - Initial Page Write Exception */
		Do_Exception(next_pc, 0x80, 0x100);

		return;
		break;

		/* data read/write missasligned */
	case MMU_ERROR_BADADDR:
		if (access_type == MMU_TT_DWRITE)			/* WADDERR - Write Data Address Error */
			Do_Exception(next_pc, 0x100, 0x100);
		else if (access_type == MMU_TT_DREAD)		/* RADDERR - Read Data Address Error */
			Do_Exception(next_pc, 0xE0, 0x100);
		else							/* IADDERR - Instruction Address Error */
		{
			printf_mmu("MMU_ERROR_BADADDR(i) 0x%X\n", address);
			Do_Exception(next_pc, 0xE0, 0x100);
			return;
		}
		printf_mmu("MMU_ERROR_BADADDR(d) 0x%X, handled\n", address);
		return;
		break;

		/* Can't Execute */
	case MMU_ERROR_EXECPROT:
		INFO_LOG(SH4, "MMU_ERROR_EXECPROT 0x%X\n", address);

		/* EXECPROT - Instruction TLB Protection Violation Exception */
		Do_Exception(next_pc, 0xA0, 0x100);
		return;
		break;
	}

	die("Unknown mmu_error");
}

bool mmu_match(u32 va, CCN_PTEH_type Address, CCN_PTEL_type Data)
{
	u32 sz, mask;
	if (Data.V == 0)
		return false;

	sz = Data.SZ1 * 2 + Data.SZ0;
	mask = mmu_mask[sz];

	if ((((Address.VPN << 10)&mask) == (va&mask)))
	{
		bool asid_match = (Data.SH == 0) && ((sr.MD == 0) || (CCN_MMUCR.SV == 0));

		if ((asid_match == false) || (Address.ASID == CCN_PTEH.ASID))
		{
			return true;
		}
	}

	return false;
}

#ifndef FAST_MMU
/* Do a full lookup on the UTLB entry's */
u32 mmu_full_lookup_c(u32 va, const TLB_Entry** tlb_entry_ret, u32 *rv)
{
	u32 entry = (u32)-1;
	u32 nom = 0;
	u32 i;

	CCN_MMUCR.URC++;
	if (CCN_MMUCR.URB == CCN_MMUCR.URC)
		CCN_MMUCR.URC = 0;

	for (i = 0; i<64; i++)
	{
		/* verify(sz!=0); */
		TLB_Entry *tlb_entry = &UTLB[i];
		if (mmu_match(va, tlb_entry->Address, tlb_entry->Data))
		{
			u32 sz = tlb_entry->Data.SZ1 * 2 + tlb_entry->Data.SZ0;
			u32 mask = mmu_mask[sz];
			entry = i;
			nom++;
			/* VPN->PPN | low bits */
			*rv = ((tlb_entry->Data.PPN << 10) & mask) | (va & (~mask));
		}
	}

	if (nom != 1)
	{
		if (nom)
		{
			return MMU_ERROR_TLB_MHIT;
		}
		else
		{
			return MMU_ERROR_TLB_MISS;
		}
	}

	*tlb_entry_ret = &UTLB[entry];

	return MMU_ERROR_NONE;
}
#endif

/* Simple QACR translation for mmu (when AT is off) */
u32 mmu_QACR_SQ(u32 va)
{
	u32 QACR;

	/* default to sq 0 */
	QACR = CCN_QACR_TR[0];
	/* sq1 ? if so use QACR1 */
	if (va & 0x20)
		QACR = CCN_QACR_TR[1];
	va &= ~0x1f;
	return QACR + va;
}

#ifndef FAST_MMU
u32 mmu_full_SQ_c(u32 va, u32 *rv, u32 translation_type)
{

	if ((va & 3) || (CCN_MMUCR.SQMD == 1 && sr.MD == 0))
	{
		/* here, or after ? */
		return MMU_ERROR_BADADDR;
	}

	if (CCN_MMUCR.AT)
	{
		/* Address=Dest&0xFFFFFFE0; */

		const TLB_Entry *entry;
		u32 lookup = mmu_full_lookup_c(va, &entry, rv);
		u32 md;

		*rv &= ~31;/* lower 5 bits are forced to 0 */

		if (lookup != MMU_ERROR_NONE)
			return lookup;

		md = entry->Data.PR >> 1;

		/* Priv mode protection */
		if ((md == 0) && sr.MD == 0)
		{
			return MMU_ERROR_PROTECTED;
		}

		/* Write Protection (Lock or FW) */
		if (translation_type == MMU_TT_DWRITE)
		{
			if ((entry->Data.PR & 1) == 0)
				return MMU_ERROR_PROTECTED;
			else if (entry->Data.D == 0)
				return MMU_ERROR_FIRSTWRITE;
		}
	}
	else
	{
		*rv = mmu_QACR_SQ(va);
	}
	return MMU_ERROR_NONE;
}

static TYPES_INLINE u32 data_translation(u32 va, u32 *rv, u32 translation_type, u32 size)
{
	const TLB_Entry *entry;
	u32 lookup, md;

	/* A 64-bit access is two of 32 bits to the SH4 and has to sit on a
	 * 4-byte boundary, not an 8-byte one. This asked for 8, so that under
	 * the interpreter a double moved to or from an address 4 past a
	 * multiple of 8 was an address error. The recompilers translate a
	 * 64-bit access as a 32-bit one and never raised it, and upstream
	 * tests for 4. */
	if (va & ((size < 4 ? size : 4) - 1))
		return MMU_ERROR_BADADDR;

	if (translation_type == MMU_TT_DWRITE)
	{
		if ((va & 0xFC000000) == 0xE0000000)
		{
			lookup = mmu_full_SQ_c(va, rv, translation_type);
			if (lookup != MMU_ERROR_NONE)
				return lookup;

			*rv = va;	/* SQ writes are not translated, only write backs are. */
			return MMU_ERROR_NONE;
		}
	}

	if ((sr.MD == 0) && (va & 0x80000000) != 0)
	{
		/* if on kernel, and not SQ addr -> error */
		return MMU_ERROR_BADADDR;
	}

   if ((va & 0xFC000000) == 0x7C000000)
	{
      /* 7C000000 to 7FFFFFFF in P0/U0 not translated */
		*rv = va;
		return MMU_ERROR_NONE;
	}

	if (fast_reg_lut[va >> 29] != 0)
	{
		/* P1, P2 and P4 aren't translated */
		*rv = va;
		return MMU_ERROR_NONE;
	}

	lookup = mmu_full_lookup_c(va, &entry, rv);

	if (lookup != MMU_ERROR_NONE)
		return lookup;

	md = entry->Data.PR >> 1;

	/* 0X  & User mode-> protection violation */
	/* Priv mode protection */
	if ((md == 0) && sr.MD == 0)
	{
		return MMU_ERROR_PROTECTED;
	}

	/* X0 -> read olny */
	/* X1 -> read/write , can be FW */

	/* Write Protection (Lock or FW) */
	if (translation_type == MMU_TT_DWRITE)
	{
		if ((entry->Data.PR & 1) == 0)
			return MMU_ERROR_PROTECTED;
		else if (entry->Data.D == 0)
			return MMU_ERROR_FIRSTWRITE;
	}
   if ((*rv & 0x1C000000) == 0x1C000000)
		/* map 1C000000-1FFFFFFF to P4 memory-mapped registers */
		*rv |= 0xF0000000;
	return MMU_ERROR_NONE;
}
MMU_DATA_TRANSLATIONS

u32 mmu_instruction_translation_c(u32 va, u32 *rv)
{
	const TLB_Entry *entry;
	u32 lookup, md;

	if (va & 1)
	{
		return MMU_ERROR_BADADDR;
	}
	if ((sr.MD == 0) && (va & 0x80000000) != 0)
	{
		/* if SQ disabled , or if if SQ on but out of SQ mem then BAD ADDR ;) */
		if (va >= 0xE0000000)
			return MMU_ERROR_BADADDR;
	}

	if ((CCN_MMUCR.AT == 0) || (fast_reg_lut[va >> 29] != 0))
	{
		*rv = va;
		return MMU_ERROR_NONE;
	}

	lookup = mmu_instruction_lookup_c(va, &entry, rv);
	if (lookup != MMU_ERROR_NONE)
		return lookup;

	md = entry->Data.PR >> 1;

	/* 0X  & User mode-> protection violation */
	/* Priv mode protection */
	if (md == 0 && sr.MD == 0)
		return MMU_ERROR_PROTECTED;

	return MMU_ERROR_NONE;
}
#endif

u32 mmu_instruction_lookup_c(u32 va, const TLB_Entry** tlb_entry_ret, u32 *rv)
{
	bool mmach = false;
	u32 entry, nom, i;
retry_ITLB_Match:
	entry = 4;
	nom = 0;
	for (i = 0; i<4; i++)
	{
		u32 sz, mask;
		if (ITLB[i].Data.V == 0)
			continue;
		sz = ITLB[i].Data.SZ1 * 2 + ITLB[i].Data.SZ0;
		mask = mmu_mask[sz];

		if ((((ITLB[i].Address.VPN << 10)&mask) == (va&mask)))
		{
			bool asid_match = (ITLB[i].Data.SH == 0) && ((sr.MD == 0) || (CCN_MMUCR.SV == 0));

			if ((asid_match == false) || (ITLB[i].Address.ASID == CCN_PTEH.ASID))
			{
				/* verify(sz!=0); */
				entry = i;
				nom++;
				/* VPN->PPN | low bits */
				*rv = ((ITLB[i].Data.PPN << 10)&mask) | (va&(~mask));
			}
		}
	}

	if (entry == 4)
	{
		const TLB_Entry *tlb_entry;
		u32 lookup, replace_index;
		verify(mmach == false);
		lookup = mmu_full_lookup_c(va, &tlb_entry, rv);

		if (lookup != MMU_ERROR_NONE)
			return lookup;

		replace_index = ITLB_LRU_USE[CCN_MMUCR.LRUI];
		verify(replace_index != 0xFFFFFFFF);
		ITLB[replace_index] = *tlb_entry;
		entry = replace_index;
		ITLB_Sync(entry);
		mmach = true;
		goto retry_ITLB_Match;
	}
	else if (nom != 1)
	{
		if (nom)
		{
			return MMU_ERROR_TLB_MHIT;
		}
		else
		{
			return MMU_ERROR_TLB_MISS;
		}
	}

	CCN_MMUCR.LRUI &= ITLB_LRU_AND[entry];
	CCN_MMUCR.LRUI |= ITLB_LRU_OR[entry];
	*tlb_entry_ret = &ITLB[entry];

	return MMU_ERROR_NONE;
}

#ifdef MMU_HOST_PAGE_LUT
uintptr_t mmu_read_lut[0x100000];
uintptr_t mmu_write_lut[0x100000];

/* Which 4K stretches of the tables have had an entry made in them since
 * they were last emptied: those are all that emptying has to clear. */
#define LUT_ENTRIES_PER_PART (4096 / sizeof(uintptr_t))
#define LUT_PARTS (0x100000 / LUT_ENTRIES_PER_PART)
static u16 lut_used_parts[LUT_PARTS];
static u32 lut_used_count;
static bool lut_part_used[LUT_PARTS];

void mmu_lut_flush(void)
{
	u32 i;
	for (i = 0; i < lut_used_count; i++)
	{
		const u32 part = lut_used_parts[i];
		memset(&mmu_read_lut[part * LUT_ENTRIES_PER_PART], 0, 4096);
		memset(&mmu_write_lut[part * LUT_ENTRIES_PER_PART], 0, 4096);
		lut_part_used[part] = false;
	}
	lut_used_count = 0;
}

void mmu_lut_forget(u32 va, u32 size)
{
	u32 page;
	if (lut_used_count == 0)
		return;
	if (size > 0x100000)
		size = 0x100000;
	va &= ~(size - 1);
	for (page = va >> 12; page <= (va + size - 1) >> 12; page++)
	{
		mmu_read_lut[page] = 0;
		mmu_write_lut[page] = 0;
	}
}

void mmu_lut_fill(u32 va, u32 pa, bool write)
{
	uintptr_t host;
	u32 part;

	/* main memory only */
	if ((pa & 0x1C000000) != 0x0C000000)
		return;
	if (va < 0x7C000000)
	{
		/* translated: a 1K page is finer than the tables */
		const TLB_Entry *entry;
		u32 rv;
		/* the strict way: nothing of a translation is kept that the next access could go by */
		if (mmu_strict)
			return;
		if (mmu_full_lookup_c(va, &entry, &rv) != MMU_ERROR_NONE
				|| (entry->Data.SZ1 == 0 && entry->Data.SZ0 == 0))
			return;
	}
	else if ((va >> 29) != 4 && (va >> 29) != 5)
		/* neither translated nor one of the two regions that are not */
		return;

	/* What to add to an address in the page to be at it on the host: the
	 * page on the host, less the page's address. (0 is "not known", and is
	 * what this is where the two are the same, which costs that page its
	 * place in the tables and nothing else.) */
	host = (uintptr_t)&mem_b.data[pa & RAM_MASK & ~0xFFFu] - (va & ~0xFFFu);
	mmu_read_lut[va >> 12] = host;
	if (write)
		mmu_write_lut[va >> 12] = host;

	part = (va >> 12) / LUT_ENTRIES_PER_PART;
	if (!lut_part_used[part])
	{
		lut_part_used[part] = true;
		lut_used_parts[lut_used_count++] = part;
	}
}
#endif

void mmu_set_state(void)
{
	mmu_lut_flush();
	mmu_detect_strict();
	mmu_strict_changed();
	if (CCN_MMUCR.AT == 1 && settings.dreamcast.FullMMU)
		NOTICE_LOG(SH4, "Enabling Full MMU support\n");

	SetMemoryHandlers();
}

void MMU_init(void)
{
	u32 e, i;
	memset(ITLB_LRU_USE, 0xFF, sizeof(ITLB_LRU_USE));
	for (e = 0; e<4; e++)
	{
		u32 match_key = ((~ITLB_LRU_AND[e]) & 0x3F);
		u32 match_mask = match_key | ITLB_LRU_OR[e];
		for (i = 0; i<64; i++)
		{
			if ((i & match_mask) == match_key)
			{
				verify(ITLB_LRU_USE[i] == 0xFFFFFFFF);
				ITLB_LRU_USE[i] = e;
			}
		}
	}
	mmu_set_state();
}


void MMU_reset(void)
{
	mmu_lut_flush();
	memset(UTLB, 0, sizeof(UTLB));
	memset(ITLB, 0, sizeof(ITLB));
	mmu_set_state();
	mmu_flush_table();
	/* (where the store queues go by the TLB, which is empty now) */
	memset(sq_remap, 0, sizeof(sq_remap));
}

void MMU_term(void)
{
}

#ifndef FAST_MMU
void mmu_flush_table(void)
{
	u32 i;
	/* printf("MMU tables flushed\n"); */

	ITLB[0].Data.V = 0;
	ITLB[1].Data.V = 0;
	ITLB[2].Data.V = 0;
	ITLB[3].Data.V = 0;

	for (i = 0; i < 64; i++)
		UTLB[i].Data.V = 0;
}
#endif

/* The SH4's accesses, with an exception where the translation fails. One of
 * 64 bits is the two of 32 that it is (mmu_in_two_pages()). */
static TYPES_INLINE u32 read_translation(u32 adr, u32 size)
{
	u32 addr;
	u32 rv = mmu_data_translation_c(adr, &addr, MMU_TT_DREAD, size);
	if (rv != MMU_ERROR_NONE)
		mmu_raise_exception(rv, adr, MMU_TT_DREAD);
	return addr;
}

static TYPES_INLINE u32 write_translation(u32 adr, u32 size)
{
	u32 addr;
	u32 rv = mmu_data_translation_c(adr, &addr, MMU_TT_DWRITE, size);
	if (rv != MMU_ERROR_NONE)
		mmu_raise_exception(rv, adr, MMU_TT_DWRITE);
	return addr;
}

u8 DYNACALL mmu_ReadMem8(u32 adr) { return _vmem_ReadMem8(read_translation(adr, 1)); }
u16 DYNACALL mmu_ReadMem16(u32 adr) { return _vmem_ReadMem16(read_translation(adr, 2)); }
u32 DYNACALL mmu_ReadMem32(u32 adr) { return _vmem_ReadMem32(read_translation(adr, 4)); }

u64 DYNACALL mmu_ReadMem64(u32 adr)
{
	if (mmu_in_two_pages(adr))
	{
		const u32 low = mmu_ReadMem32(adr);
		return ((u64)mmu_ReadMem32(adr + 4) << 32) | low;
	}
	return _vmem_ReadMem64(read_translation(adr, 8));
}

u16 DYNACALL mmu_IReadMem16(u32 vaddr)
{
	u32 addr;
	u32 rv = mmu_instruction_translation_c(vaddr, &addr);
	if (rv != MMU_ERROR_NONE)
		mmu_raise_exception(rv, vaddr, MMU_TT_IREAD);
	return _vmem_ReadMem16(addr);
}

void DYNACALL mmu_WriteMem8(u32 adr, u8 data) { _vmem_WriteMem8(write_translation(adr, 1), data); }
void DYNACALL mmu_WriteMem16(u32 adr, u16 data) { _vmem_WriteMem16(write_translation(adr, 2), data); }
void DYNACALL mmu_WriteMem32(u32 adr, u32 data) { _vmem_WriteMem32(write_translation(adr, 4), data); }

void DYNACALL mmu_WriteMem64(u32 adr, u64 data)
{
	if (mmu_in_two_pages(adr))
	{
		mmu_WriteMem32(adr, (u32)data);
		mmu_WriteMem32(adr + 4, (u32)(data >> 32));
		return;
	}
	_vmem_WriteMem64(write_translation(adr, 8), data);
}

/* With the exception taken and said so instead */
u64 DYNACALL mmu_ReadMemNoEx_c(u32 adr, u32 size, u32 *exception_occurred)
{
	u32 addr, rv;

	if (size == 8 && mmu_in_two_pages(adr))
	{
		u32 low, high;
		low = (u32)mmu_ReadMemNoEx_c(adr, 4, exception_occurred);
		if (*exception_occurred)
			return 0;
		high = (u32)mmu_ReadMemNoEx_c(adr + 4, 4, exception_occurred);
		if (*exception_occurred)
			return 0;
		return ((u64)high << 32) | low;
	}

	rv = mmu_data_translation_c(adr, &addr, MMU_TT_DREAD, size);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(adr, rv, MMU_TT_DREAD);
		*exception_occurred = 1;
		return 0;
	}
	*exception_occurred = 0;
	/* the next read of the page need not come here */
	mmu_lut_fill(adr, addr, false);
	switch (size)
	{
	case 1: return _vmem_ReadMem8(addr);
	case 2: return _vmem_ReadMem16(addr);
	case 4: return _vmem_ReadMem32(addr);
	default: return _vmem_ReadMem64(addr);
	}
}

u32 DYNACALL mmu_WriteMemNoEx_c(u32 adr, u64 data, u32 size)
{
	u32 addr, rv;

	if (size == 8 && mmu_in_two_pages(adr))
	{
		if (mmu_WriteMemNoEx_c(adr, (u32)data, 4))
			return 1;
		return mmu_WriteMemNoEx_c(adr + 4, (u32)(data >> 32), 4);
	}

	rv = mmu_data_translation_c(adr, &addr, MMU_TT_DWRITE, size);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(adr, rv, MMU_TT_DWRITE);
		return 1;
	}
	mmu_lut_fill(adr, addr, true);
	switch (size)
	{
	case 1: _vmem_WriteMem8(addr, (u8)data); break;
	case 2: _vmem_WriteMem16(addr, (u16)data); break;
	case 4: _vmem_WriteMem32(addr, (u32)data); break;
	default: _vmem_WriteMem64(addr, data); break;
	}
	return 0;
}

bool mmu_TranslateSQW(u32 adr, u32* out)
{
	if (!mmu_enabled())
	{
		/* This will only work for 1 mb pages .. hopefully nothing else is used */
		/* *FIXME* to work for all page sizes ? */

		*out = sq_remap[(adr >> 20) & 0x3F] | (adr & 0xFFFE0);
	}
	else
	{
		u32 addr;
		u32 tv = mmu_full_SQ_c(adr, &addr, MMU_TT_DREAD);
		if (tv != MMU_ERROR_NONE)
		{
			mmu_raise_exception(tv, adr, MMU_TT_DREAD);
			return false;
		}

		*out = addr;
	}

	return true;
}
#endif

/* The SH4's memory functions, as the MMU, the interpreter's caches
 * (STRICT_MODE) and the recompiler want them (from sh4_mem.cpp, which is C
 * and has none of the templates) */
#ifdef STRICT_MODE
#include "hw/sh4/sh4_cache.h"
#endif

#ifdef STRICT_MODE
static bool interpreterRunning = false;
#endif

void SetMemoryHandlers(void)
{
#ifndef NO_MMU
#ifdef STRICT_MODE
	if (settings.dynarec.Enable && interpreterRunning)
		/* Flush caches when interp -> dynarec */
		sh4_caches_write_back();

	if (!settings.dynarec.Enable)
	{
		interpreterRunning = true;
		sh4_caches_set_handlers();
		return;
	}
	interpreterRunning = false;
#endif
	if (mmu_enabled())
	{
		IReadMem16 = &mmu_IReadMem16;
		ReadMem8 = &mmu_ReadMem8;
		ReadMem16 = &mmu_ReadMem16;
		ReadMem32 = &mmu_ReadMem32;
		ReadMem64 = &mmu_ReadMem64;

		WriteMem8 = &mmu_WriteMem8;
		WriteMem16 = &mmu_WriteMem16;
		WriteMem32 = &mmu_WriteMem32;
		WriteMem64 = &mmu_WriteMem64;
	}
	else
	{
		ReadMem8 = &_vmem_ReadMem8;
		ReadMem16 = &_vmem_ReadMem16;
		IReadMem16 = &_vmem_ReadMem16;
		ReadMem32 = &_vmem_ReadMem32;
		ReadMem64 = &_vmem_ReadMem64;

		WriteMem8 = &_vmem_WriteMem8;
		WriteMem16 = &_vmem_WriteMem16;
		WriteMem32 = &_vmem_WriteMem32;
		WriteMem64 = &_vmem_WriteMem64;
	}
#endif
}
