#pragma once
#include "types.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/mem/_vmem.h"

/* Translation Types */
/* Opcode read */
#define MMU_TT_IREAD 0
/* Data write */
#define MMU_TT_DWRITE 1
/* Data write */
#define MMU_TT_DREAD 2

/* Return Values */
/* Translation was successful */
#define MMU_ERROR_NONE	   0
/* TLB miss */
#define MMU_ERROR_TLB_MISS 1
/* TLB Multihit */
#define MMU_ERROR_TLB_MHIT 2
/* Mem is read/write protected (depends on translation type) */
#define MMU_ERROR_PROTECTED 3
/* Mem is write protected , firstwrite */
#define MMU_ERROR_FIRSTWRITE 4
/* data-Opcode read/write missasligned */
#define MMU_ERROR_BADADDR 5
/* Can't Execute */
#define MMU_ERROR_EXECPROT 6

#ifdef __cplusplus
extern "C" {
#endif

struct TLB_Entry
{
	CCN_PTEH_type Address;
	CCN_PTEL_type Data;
	CCN_PTEA_type Assistance;
};

typedef struct TLB_Entry TLB_Entry;

extern TLB_Entry UTLB[64];
extern TLB_Entry ITLB[4];
extern u32 sq_remap[64];
/* The instruction TLB's least recently used, by MMUCR.LRUI */
extern u32 ITLB_LRU_USE[64];
/* An address's page, by the page size (SZ1 * 2 + SZ0) */
extern const u32 mmu_mask[4];
extern const u32 fast_reg_lut[8];

/* These are working only for SQ remaps on ndce */
bool UTLB_Sync(u32 entry);
void ITLB_Sync(u32 entry);

bool mmu_match(u32 va, CCN_PTEH_type Address, CCN_PTEL_type Data);
/* the page @va is in has been taken out of the TLB */
void mmu_forget(u32 va);
/* the TLB's entry of that number has been written to */
void mmu_utlb_written(u32 entry);
void mmu_set_state(void);
#ifdef NO_MMU
static TYPES_INLINE void mmu_flush_table(void) {}
#else
void mmu_flush_table(void);
#endif
void mmu_raise_exception(u32 mmu_error, u32 address, u32 am);

/* A program that brings a TLB of its own making, and is not Windows CE:
 * it has the MMU, done the strict way, from the moment it turns address
 * translation on until the machine is reset. See fastmmu.cpp. */
extern bool mmu_strict;
/* AT has come on or gone off: is this such a program? */
void mmu_detect_strict(void);
/* what the strict way keeps of its lookups is to go */
void mmu_strict_changed(void);
/* The translations kept, in a state since V22 (fastmmu.c) */
void mmu_serialize_v22(void **data, unsigned int *total_size);
bool mmu_unserialize_v22(void **data, unsigned int *total_size);
void mmu_state_before_v22(void);

static TYPES_INLINE bool mmu_enabled(void)
{
#ifndef NO_MMU
	return mmu_strict || (settings.dreamcast.FullMMU && CCN_MMUCR.AT == 1);
#else
	return false;
#endif
}


/* Translations kept by, for the recompilers, which have to translate every
 * address themselves.
 *
 * (The 64-bit ones on Linux and Android used to have the host's MMU do
 * it: each page a Windows CE game touched was mapped on the host where
 * the game has it, and code read and wrote with no translating at all.
 * But such a mapping is for one address space, and all of it went
 * whenever the game changed to another - seventeen times a frame in Sega
 * Rally 2, which had 1.2 million pages mapped a minute that way, 87 in 100
 * of them a page it had mapped before for the same address space. Of the
 * 27 seconds of processor that minute took on x86-64 Linux, 10 were the
 * kernel's; with what follows it was under 16, and the kernel's a fifth of
 * one.)
 *
 * Two tables, one for reads and one for writes, each with an entry for
 * every 4K page of the SH4's address space: what to add to an address in
 * the page to be at it on the host, or 0. Recompiled code looks an address
 * up in the one for what it is doing, and on a hit goes straight to
 * memory; on a miss it calls the
 * translation, which fills the entry in if the page is one of main memory -
 * the only kind for which a host address is all an access needs.
 *
 * A write's entry is only filled by a write that was allowed, so a page
 * that may not be written, or has not been written yet and has its first
 * write to report, still gets its exception however often it has been read.
 * (Upstream's table, which this is after, has one entry for both.)
 *
 * What it does not know is who is asking: an entry made in privileged mode
 * is found in user mode too.
 *
 * It is emptied whenever what an address means can have changed: the TLB
 * flushed, another address space, the MMU or its single-space mode switched,
 * a TLB entry written through its memory-mapped array, a state loaded. A
 * page whose TLB entry is loaded or invalidated is forgotten on its own.
 * Emptying costs by how much of the tables has been used since the last
 * time, which is little: the parts that were are kept note of. */
#if !defined(NO_MMU) && (HOST_CPU == CPU_ARM || HOST_CPU == CPU_ARM64 || HOST_CPU == CPU_X64 \
		|| (HOST_CPU == CPU_X86 && FEAT_SHREC == DYNAREC_JIT))
#define MMU_HOST_PAGE_LUT 1
extern uintptr_t mmu_read_lut[0x100000];
extern uintptr_t mmu_write_lut[0x100000];
void mmu_lut_flush(void);
void mmu_lut_forget(u32 va, u32 size);
/* @va has just been translated to @pa, for a write if @write */
void mmu_lut_fill(u32 va, u32 pa, bool write);
#else
static TYPES_INLINE void mmu_lut_flush(void) {}
static TYPES_INLINE void mmu_lut_forget(u32 va, u32 size) { (void)va; (void)size; }
/* (called below whichever host this is: without it a host with no table did not compile) */
static TYPES_INLINE void mmu_lut_fill(u32 va, u32 pa, bool write) { (void)va; (void)pa; (void)write; }
#endif



/* 64 bits are read and written as two times 32 by the SH4, each at its own
 * address: where the second is in another page than the first - at the
 * last four bytes of a page, be it one of 1K or more - it is somewhere
 * else altogether, and need not be there at all. Whoever translates one
 * address and takes eight bytes from where it is has to do such an access
 * as the two that it is. (None did, and such an access read and wrote four
 * bytes of whatever comes next in memory.) */
static TYPES_INLINE bool mmu_in_two_pages(u32 va)
{
	return (va & 0x3FF) == 0x3FC;
}
void DoMMUException(u32 addr, u32 mmu_error, u32 access_type);

#if defined(NO_MMU)
static TYPES_INLINE bool mmu_TranslateSQW(u32 addr, u32* mapped)
{
	*mapped = sq_remap[(addr>>20)&0x3F] | (addr & 0xFFFE0);
	return true;
}
#else
u16 DYNACALL mmu_IReadMem16(u32 addr);
bool mmu_TranslateSQW(u32 adr, u32 *out);
#endif

/* The translations, MMU_ERROR_NONE and the address in @rv, or the error.
 * Looked up in the TLB, the entry that says so in @entry: */
u32 mmu_full_lookup_c(u32 va, const TLB_Entry **entry, u32 *rv);
/* in the instruction TLB, which takes it from the other if it has to: */
u32 mmu_instruction_lookup_c(u32 va, const TLB_Entry **entry, u32 *rv);
/* a store queue's, for a read or a write (@translation_type): */
u32 mmu_full_SQ_c(u32 va, u32 *rv, u32 translation_type);
/* an access of @size bytes, for a read or a write (one each, below) */
u32 mmu_data_translation_r8(u32 va, u32 *rv);
u32 mmu_data_translation_r16(u32 va, u32 *rv);
u32 mmu_data_translation_r32(u32 va, u32 *rv);
u32 mmu_data_translation_r64(u32 va, u32 *rv);
u32 mmu_data_translation_w8(u32 va, u32 *rv);
u32 mmu_data_translation_w16(u32 va, u32 *rv);
u32 mmu_data_translation_w32(u32 va, u32 *rv);
u32 mmu_data_translation_w64(u32 va, u32 *rv);

static TYPES_INLINE u32 mmu_data_translation_c(u32 va, u32 *rv, u32 translation_type, u32 size)
{
	if (translation_type == MMU_TT_DWRITE)
		return size == 1 ? mmu_data_translation_w8(va, rv) : size == 2 ? mmu_data_translation_w16(va, rv)
				: size == 4 ? mmu_data_translation_w32(va, rv) : mmu_data_translation_w64(va, rv);
	return size == 1 ? mmu_data_translation_r8(va, rv) : size == 2 ? mmu_data_translation_r16(va, rv)
			: size == 4 ? mmu_data_translation_r32(va, rv) : mmu_data_translation_r64(va, rv);
}

#ifdef FAST_MMU
static TYPES_INLINE u32 mmu_instruction_translation_c(u32 va, u32 *rv)
{
	const TLB_Entry *tlb_entry;
	if (va & 1)
		return MMU_ERROR_BADADDR;
	/* (a strict program keeps all this while it has AT off: nothing is translated then) */
	if (fast_reg_lut[va >> 29] != 0 || CCN_MMUCR.AT == 0)
	{
		*rv = va;
		return MMU_ERROR_NONE;
	}

	return mmu_full_lookup_c(va, &tlb_entry, rv);
}
#else
u32 mmu_instruction_translation_c(u32 va, u32 *rv);
#endif

/* Whether an access of @size bytes at @va is translated, for a read or a
 * write (@translation_type) */
static TYPES_INLINE bool mmu_is_translated_c(u32 va, u32 size, u32 translation_type)
{
	if (va & (size - 1))
		return true;

	if (translation_type == MMU_TT_DWRITE)
	{
		if ((va & 0xFC000000) == 0xE0000000)
			/* SQ writes are not translated, only write backs are. */
			return false;
	}
	if ((va & 0xFC000000) == 0x7C000000)
		/* On-chip RAM area isn't translated */
		return false;

	if (fast_reg_lut[va >> 29] != 0)
		return false;

	return true;
}

#if !defined(NO_MMU)
/* The SH4's accesses with the MMU on: an exception (sh4_throw()) where the
 * translation fails */
u8 DYNACALL mmu_ReadMem8(u32 adr);
u16 DYNACALL mmu_ReadMem16(u32 adr);
u32 DYNACALL mmu_ReadMem32(u32 adr);
u64 DYNACALL mmu_ReadMem64(u32 adr);
void DYNACALL mmu_WriteMem8(u32 adr, u8 data);
void DYNACALL mmu_WriteMem16(u32 adr, u16 data);
void DYNACALL mmu_WriteMem32(u32 adr, u32 data);
void DYNACALL mmu_WriteMem64(u32 adr, u64 data);

/* ...and with the exception taken (DoMMUException()) and @exception_occurred
 * set instead, for the recompilers: @size bytes */
u64 DYNACALL mmu_ReadMemNoEx_c(u32 adr, u32 size, u32 *exception_occurred);
u32 DYNACALL mmu_WriteMemNoEx_c(u32 adr, u64 data, u32 size);
#endif

#ifdef __cplusplus
}

/* (the same for C++, as it had them) */
template<bool internal = false>
static inline u32 mmu_full_lookup(u32 va, const TLB_Entry **entry, u32& rv)
{
	return mmu_full_lookup_c(va, entry, &rv);
}

static inline u32 mmu_instruction_lookup(u32 va, const TLB_Entry **entry, u32& rv)
{
	return mmu_instruction_lookup_c(va, entry, &rv);
}

template<u32 translation_type>
static inline u32 mmu_full_SQ(u32 va, u32& rv)
{
	return mmu_full_SQ_c(va, &rv, translation_type);
}

static inline u32 mmu_instruction_translation(u32 va, u32& rv)
{
	return mmu_instruction_translation_c(va, &rv);
}

template<u32 translation_type, typename T>
static inline u32 mmu_data_translation(u32 va, u32& rv)
{
	return mmu_data_translation_c(va, &rv, translation_type, sizeof(T));
}

template<u32 translation_type>
static inline bool mmu_is_translated(u32 va, u32 size)
{
	return mmu_is_translated_c(va, size, translation_type);
}

#if !defined(NO_MMU)
template<typename T>
static inline T DYNACALL mmu_ReadMemNoEx(u32 adr, u32 *exception_occurred)
{
	return (T)mmu_ReadMemNoEx_c(adr, sizeof(T), exception_occurred);
}

template<typename T>
static inline u32 DYNACALL mmu_WriteMemNoEx(u32 adr, T data)
{
	return mmu_WriteMemNoEx_c(adr, (u64)data, sizeof(T));
}
#endif
#endif
