/* posix_memalign(), which a C89 compile of the C library's headers leaves out */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <string.h>
#include <stdlib.h>
#include <retro_atomic.h>
#include <retro_miscellaneous.h>
#include "_vmem.h"
#include "hw/aica/aica_if.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/modules/mmu.h"

#if defined(HAVE_LIBNX)
#include <malloc.h>
#endif

#define HANDLER_MAX 0x1F
#define HANDLER_COUNT (HANDLER_MAX+1)

/* top registered handler */
static _vmem_handler       _vmem_lrp;

/* handler tables */
static _vmem_ReadMem8FP*   _vmem_RF8[HANDLER_COUNT];
static _vmem_WriteMem8FP*  _vmem_WF8[HANDLER_COUNT];

static _vmem_ReadMem16FP*  _vmem_RF16[HANDLER_COUNT];
static _vmem_WriteMem16FP* _vmem_WF16[HANDLER_COUNT];

static _vmem_ReadMem32FP*  _vmem_RF32[HANDLER_COUNT];
static _vmem_WriteMem32FP* _vmem_WF32[HANDLER_COUNT];

/* upper 8b of the address */
static void* _vmem_MemInfo_ptr[0x100];

/* A handler's function, as the recompilers take it: a pointer (ISO C has
 * no conversion of a function's to an object's, which every host this runs
 * on makes the same size) */
typedef union
{
	_vmem_ReadMem8FP *r8;
	_vmem_ReadMem16FP *r16;
	_vmem_ReadMem32FP *r32;
	_vmem_WriteMem8FP *w8;
	_vmem_WriteMem16FP *w16;
	_vmem_WriteMem32FP *w32;
	void *p;
} vmem_fn;

void *_vmem_read_ptr(u32 addr, bool *ismem, u32 sz)
{
	const unat iirf = (unat)_vmem_MemInfo_ptr[addr >> 24];
	void *ptr = (void *)(iirf & ~(unat)HANDLER_MAX);

	if (ptr == 0)
	{
		vmem_fn fn;
		*ismem = false;
		fn.p = NULL;
		if (sz == 1)
			fn.r8 = _vmem_RF8[iirf];
		else if (sz == 2)
			fn.r16 = _vmem_RF16[iirf];
		else if (sz == 4)
			fn.r32 = _vmem_RF32[iirf];
		else
			die("Invalid size");
		return fn.p;
	}
	*ismem = true;
	/* By the shift alone, the table entry's low bits. Shifting by the whole
	 * entry, pointer and all, left it to the processor to throw the rest
	 * away: x86 and ARM64 do, but it is not C, and GCC 13 for 32-bit ARM
	 * takes a count that cannot be under 32 as its leave to make the
	 * address 0 - every read and write then went to the first byte of its
	 * block, and nothing ran. */
	{
		const u32 shift = (u32)iirf & HANDLER_MAX;
		addr <<= shift;
		addr >>= shift;
	}
	return &((u8 *)ptr)[addr];
}

void *_vmem_write_ptr(u32 addr, bool *ismem, u32 sz)
{
	const unat iirf = (unat)_vmem_MemInfo_ptr[addr >> 24];
	void *ptr = (void *)(iirf & ~(unat)HANDLER_MAX);

	if (ptr == 0)
	{
		vmem_fn fn;
		*ismem = false;
		fn.p = NULL;
		if (sz == 1)
			fn.w8 = _vmem_WF8[iirf];
		else if (sz == 2)
			fn.w16 = _vmem_WF16[iirf];
		else if (sz == 4)
			fn.w32 = _vmem_WF32[iirf];
		else
			die("Invalid size");
		return fn.p;
	}
	*ismem = true;
	{
		const u32 shift = (u32)iirf & HANDLER_MAX;	/* not the whole entry: see the read's */
		addr <<= shift;
		addr >>= shift;
	}
	return &((u8 *)ptr)[addr];
}

/* The table's entry for @addr: the memory, or 0 and the handler's number */
#define VMEM_ENTRY(addr, iirf, ptr) \
	(iirf) = (unat)_vmem_MemInfo_ptr[(addr) >> 24]; \
	(ptr) = (void *)((iirf) & ~(unat)HANDLER_MAX)
/* @addr within the memory (by the shift alone: see _vmem_read_ptr()) */
#define VMEM_IN_BLOCK(addr, iirf) \
	(((addr) << ((u32)(iirf) & HANDLER_MAX)) >> ((u32)(iirf) & HANDLER_MAX))

/* ReadMem/WriteMem functions */
/* ReadMem */
u32 DYNACALL _vmem_ReadMem8SX32(u32 addr)
{
	unat iirf;
	void *ptr;
	VMEM_ENTRY(addr, iirf, ptr);
	if (likely(ptr != 0))
		return (u32)(s32)*(s8 *)&((u8 *)ptr)[VMEM_IN_BLOCK(addr, iirf)];
	return (u32)(s32)(s8)_vmem_RF8[iirf](addr);
}

u32 DYNACALL _vmem_ReadMem16SX32(u32 addr)
{
	unat iirf;
	void *ptr;
	VMEM_ENTRY(addr, iirf, ptr);
	if (likely(ptr != 0))
		return (u32)(s32)*(s16 *)&((u8 *)ptr)[VMEM_IN_BLOCK(addr, iirf)];
	return (u32)(s32)(s16)_vmem_RF16[iirf](addr);
}

u8 DYNACALL _vmem_ReadMem8(u32 addr)
{
	unat iirf;
	void *ptr;
	VMEM_ENTRY(addr, iirf, ptr);
	if (likely(ptr != 0))
		return *(u8 *)&((u8 *)ptr)[VMEM_IN_BLOCK(addr, iirf)];
	return _vmem_RF8[iirf](addr);
}

u16 DYNACALL _vmem_ReadMem16(u32 addr)
{
	unat iirf;
	void *ptr;
	VMEM_ENTRY(addr, iirf, ptr);
	if (likely(ptr != 0))
		return *(u16 *)&((u8 *)ptr)[VMEM_IN_BLOCK(addr, iirf)];
	return _vmem_RF16[iirf](addr);
}

u32 DYNACALL _vmem_ReadMem32(u32 addr)
{
	unat iirf;
	void *ptr;
	VMEM_ENTRY(addr, iirf, ptr);
	if (likely(ptr != 0))
		return *(u32 *)&((u8 *)ptr)[VMEM_IN_BLOCK(addr, iirf)];
	return _vmem_RF32[iirf](addr);
}

u64 DYNACALL _vmem_ReadMem64(u32 addr)
{
	unat iirf;
	void *ptr;
	VMEM_ENTRY(addr, iirf, ptr);
	if (likely(ptr != 0))
		return *(u64 *)&((u8 *)ptr)[VMEM_IN_BLOCK(addr, iirf)];
	{
		/* (the lower word read first) */
		u64 rv = _vmem_RF32[iirf](addr);
		rv |= (u64)_vmem_RF32[iirf](addr + 4) << 32;
		return rv;
	}
}

/* WriteMem */
void DYNACALL _vmem_WriteMem8(u32 addr, u8 data)
{
	unat iirf;
	void *ptr;
	VMEM_ENTRY(addr, iirf, ptr);
	if (likely(ptr != 0))
		*(u8 *)&((u8 *)ptr)[VMEM_IN_BLOCK(addr, iirf)] = data;
	else
		_vmem_WF8[iirf](addr, data);
}

void DYNACALL _vmem_WriteMem16(u32 addr, u16 data)
{
	unat iirf;
	void *ptr;
	VMEM_ENTRY(addr, iirf, ptr);
	if (likely(ptr != 0))
		*(u16 *)&((u8 *)ptr)[VMEM_IN_BLOCK(addr, iirf)] = data;
	else
		_vmem_WF16[iirf](addr, data);
}

void DYNACALL _vmem_WriteMem32(u32 addr, u32 data)
{
	unat iirf;
	void *ptr;
	VMEM_ENTRY(addr, iirf, ptr);
	if (likely(ptr != 0))
		*(u32 *)&((u8 *)ptr)[VMEM_IN_BLOCK(addr, iirf)] = data;
	else
		_vmem_WF32[iirf](addr, data);
}

void DYNACALL _vmem_WriteMem64(u32 addr, u64 data)
{
	unat iirf;
	void *ptr;
	VMEM_ENTRY(addr, iirf, ptr);
	if (likely(ptr != 0))
		*(u64 *)&((u8 *)ptr)[VMEM_IN_BLOCK(addr, iirf)] = data;
	else
	{
		_vmem_WF32[iirf](addr, (u32)data);
		_vmem_WF32[iirf](addr + 4, (u32)(data >> 32));
	}
}

#define MEM_ERROR_RETURN_VALUE 0

/* default read handlers */
static u8 DYNACALL _vmem_ReadMem8_not_mapped(u32 addresss)
{
	INFO_LOG(MEMORY, "[sh4]Read8 from 0x%X, not mapped [_vmem default handler]\n", addresss);
	return (u8)MEM_ERROR_RETURN_VALUE;
}
static u16 DYNACALL _vmem_ReadMem16_not_mapped(u32 addresss)
{
	INFO_LOG(MEMORY, "[sh4]Read16 from 0x%X, not mapped [_vmem default handler]\n", addresss);
	return (u16)MEM_ERROR_RETURN_VALUE;
}
static u32 DYNACALL _vmem_ReadMem32_not_mapped(u32 address)
{
	INFO_LOG(MEMORY, "[sh4]Read32 from 0x%X, not mapped [_vmem default handler]\n", address);
	return (u32)MEM_ERROR_RETURN_VALUE;
}
/* default write handers */
static void DYNACALL _vmem_WriteMem8_not_mapped(u32 addresss,u8 data)
{
	INFO_LOG(MEMORY, "[sh4]Write8 to 0x%X=0x%X, not mapped [_vmem default handler]\n", addresss, data);
}
static void DYNACALL _vmem_WriteMem16_not_mapped(u32 addresss,u16 data)
{
	INFO_LOG(MEMORY, "[sh4]Write16 to 0x%X=0x%X, not mapped [_vmem default handler]\n", addresss, data);
}
static void DYNACALL _vmem_WriteMem32_not_mapped(u32 addresss,u32 data)
{
	INFO_LOG(MEMORY, "[sh4]Write32 to 0x%X=0x%X, not mapped [_vmem default handler]\n", addresss, data);
}
/* code to register handlers */
/* 0 is considered error :) */
_vmem_handler _vmem_register_handler(
									 _vmem_ReadMem8FP* read8,
									 _vmem_ReadMem16FP* read16,
									 _vmem_ReadMem32FP* read32,

									 _vmem_WriteMem8FP* write8,
									 _vmem_WriteMem16FP* write16,
									 _vmem_WriteMem32FP* write32
									 )
{
	_vmem_handler rv=_vmem_lrp++;

	verify(rv<HANDLER_COUNT);

	_vmem_RF8[rv] =read8==0  ? _vmem_ReadMem8_not_mapped  : read8;
	_vmem_RF16[rv]=read16==0 ? _vmem_ReadMem16_not_mapped : read16;
	_vmem_RF32[rv]=read32==0 ? _vmem_ReadMem32_not_mapped : read32;

	_vmem_WF8[rv] =write8==0 ? _vmem_WriteMem8_not_mapped : write8;
	_vmem_WF16[rv]=write16==0? _vmem_WriteMem16_not_mapped: write16;
	_vmem_WF32[rv]=write32==0? _vmem_WriteMem32_not_mapped: write32;

	return rv;
}

static u32 FindMask(u32 msk)
{
	u32 s=-1;
	u32 rv=0;

	while(msk!=s>>rv)
		rv++;

	return rv;
}

/* map a registered handler to a mem region */
void _vmem_map_handler(_vmem_handler Handler,u32 start,u32 end)
{
	u32 i;
	verify(start<0x100);
	verify(end<0x100);
	verify(start<=end);
	for (i = start; i <= end; i++)
		_vmem_MemInfo_ptr[i] = (void *)(unat)Handler;
}

/* map a memory block to a mem region */
void _vmem_map_block(void* base,u32 start,u32 end,u32 mask)
{
	u32 i, j = 0;
	verify(start<0x100);
	verify(end<0x100);
	verify(start<=end);
	verify((0xFF & (size_t)base)==0);
	verify(base!=0);
	for (i = start; i <= end; i++)
	{
		_vmem_MemInfo_ptr[i]=&(((u8*)base)[j&mask]) + FindMask(mask) - (j & mask);
		j+=0x1000000;
	}
}

void _vmem_mirror_mapping(u32 new_region,u32 start,u32 size)
{
	u32 end=start+size-1;
	u32 i, j = new_region;
	verify(start<0x100);
	verify(end<0x100);
	verify(start<=end);
	verify(!((start>=new_region) && (end<=new_region)));

	for (i = start; i <= end; i++)
	{
		_vmem_MemInfo_ptr[j&0xFF]=_vmem_MemInfo_ptr[i&0xFF];
		j++;
	}
}

/* init/reset/term */
void _vmem_init(void)
{
	_vmem_reset();
}

void _vmem_reset(void)
{
	/* clear read tables */
	memset(_vmem_RF8,0,sizeof(_vmem_RF8));
	memset(_vmem_RF16,0,sizeof(_vmem_RF16));
	memset(_vmem_RF32,0,sizeof(_vmem_RF32));
	
	/* clear write tables */
	memset(_vmem_WF8,0,sizeof(_vmem_WF8));
	memset(_vmem_WF16,0,sizeof(_vmem_WF16));
	memset(_vmem_WF32,0,sizeof(_vmem_WF32));
	
	/* clear meminfo table */
	memset(_vmem_MemInfo_ptr,0,sizeof(_vmem_MemInfo_ptr));

	/* reset registration index */
	_vmem_lrp=0;

	/* register default functions (0) for slot 0 */
	verify(_vmem_register_handler(0,0,0,0,0,0)==0);
}

void _vmem_term(void)
{

}

u8* virt_ram_base;
bool vmem_4gb_space;

static void* malloc_pages(size_t size) {
#if defined(_WIN32)
	return _aligned_malloc(size, PAGE_SIZE);
#elif defined(_ISOC11_SOURCE)
	return aligned_alloc(PAGE_SIZE, size);
#elif defined(HAVE_LIBNX)
   return memalign(PAGE_SIZE, size);
#else
	void *data;
	if (posix_memalign(&data, PAGE_SIZE, size) != 0)
		return NULL;
	else
		return data;
#endif
}

#if FEAT_SHREC != DYNAREC_NONE

/* Resets the FPCB table (by either clearing it to the default val */
/* or by flushing it and making it fault on access again. */
void _vmem_bm_reset(void)
{
	/* If we allocated it via vmem: */
	if (virt_ram_base)
	{
		if (settings.dynarec.PrecompileFpcb)
		{
			/* Commit and fill the whole block-dispatch table up front instead
			 * of faulting in one page at a time on first execution of each
			 * code address. That per-page fault (BM_LockedWrite) is a measured
			 * source of frame-time spikes whenever a game first reaches new
			 * code -- boot, level loads, newly hit branches. Trading it away
			 * costs resident memory: up to RAM_SIZE/2 * sizeof(void*) (64MB at
			 * 16MB RAM, 128MB at 32MB), which is why it is opt-in. */
			vmem_platform_ondemand_page(p_sh4rcb->fpcb, sizeof(p_sh4rcb->fpcb));
			bm_vmem_pagefill((void**)p_sh4rcb->fpcb, sizeof(p_sh4rcb->fpcb));
		}
		else
			vmem_platform_reset_mem(p_sh4rcb->fpcb, sizeof(p_sh4rcb->fpcb));
	}
	else
		/* We allocated it via a regular malloc/new/whatever on the heap */
		bm_vmem_pagefill((void**)p_sh4rcb->fpcb, sizeof(p_sh4rcb->fpcb));
}

/* This gets called whenever there is a pagefault, it is possible that it lands */
/* on the fpcb memory range, which is allocated on miss. Returning true tells the */
/* fault handler this was us, and that the page is resolved and can continue the execution. */
bool BM_LockedWrite(u8* address) {
	if (!virt_ram_base)
		return false;  /* No vmem, therefore not us who caused this. */

	{
	const uintptr_t ptrint = (uintptr_t)address;
	const uintptr_t start  = (uintptr_t)p_sh4rcb->fpcb;
	const uintptr_t end    = start + sizeof(p_sh4rcb->fpcb);

	if (ptrint >= start && ptrint < end) {
		/* Alloc the page then and initialize it to default values */
		void *aligned_addr = (void*)(ptrint & (~PAGE_MASK));
		vmem_platform_ondemand_page(aligned_addr, PAGE_SIZE);
		bm_vmem_pagefill((void**)aligned_addr, PAGE_SIZE);
		return true;
	}
	}
	return false;
}
#endif

u8 *_vmem_elan_ram;

/* What is at each range of the address space: the memories, in the
 * shared memory they are all in, at the start of each */
enum { MAP_NONE, MAP_RAM, MAP_VRAM, MAP_ARAM, MAP_ERAM };
typedef struct
{
	u32 start, end;	/* (end 0: the end of the 4 GB) */
	u8 what;
	u8 allow_writes;
} vmem_range;

/* (the recompiler's block of the SH4's context is whole pages) */
typedef char sh4rcb_is_whole_pages[(sizeof(Sh4RCB) % PAGE_SIZE) == 0 ? 1 : -1];

/* The 512 MB space: the SH4's physical addresses */
static const vmem_range ranges_512mb[] = {
	{0x00000000, 0x00800000, MAP_NONE, 0},	/* Area 0 -> unused */
	{0x00800000, 0x01000000, MAP_ARAM, 0},	/* Aica */
	{0x01000000, 0x04000000, MAP_NONE, 0},	/* More unused */
	{0x04000000, 0x05000000, MAP_VRAM, 1},	/* Area 1 (vram, 16MB, wrapped on DC as 2x8MB) */
	{0x05000000, 0x06000000, MAP_NONE, 0},	/* 32 bit path (unused) */
	{0x06000000, 0x07000000, MAP_VRAM, 1},	/* VRAM mirror */
	{0x07000000, 0x08000000, MAP_NONE, 0},	/* 32 bit path (unused) mirror */
	{0x08000000, 0x0A000000, MAP_NONE, 0},	/* Area 2 */
	{0x0A000000, 0x0C000000, MAP_ERAM, 1},	/* a NAOMI 2's geometry chip (nothing elsewhere) */
	{0x0C000000, 0x10000000, MAP_RAM, 1},	/* Area 3 (main RAM + 3 mirrors) */
	{0x10000000, 0x20000000, MAP_NONE, 0},	/* Area 4-7 (unused) */
	/* This is outside of the 512MB addr space. We map 8MB in all cases to
	 * help some games read past the end of aica ram */
	{0x20000000, 0x20800000, MAP_ARAM, 1}	/* writable aica ram */
};

/* One 512 MB of the 4 GB space: P1, P2 and P3 are the same at their own
 * base, P0/U0 with nothing above its first 256 MB */
static const vmem_range ranges_4gb_area[] = {
	{0x00000000, 0x00800000, MAP_NONE, 0},	/* Area 0 -> unused */
	{0x00800000, 0x01000000, MAP_ARAM, 1},	/* Aica */
	{0x01000000, 0x02800000, MAP_NONE, 0},	/* unused */
	{0x02800000, 0x03000000, MAP_ARAM, 1},	/* Aica mirror */
	{0x03000000, 0x04000000, MAP_NONE, 0},	/* unused */
	{0x04000000, 0x05000000, MAP_VRAM, 1},	/* Area 1 (vram, 16MB, wrapped on DC as 2x8MB) */
	{0x05000000, 0x06000000, MAP_NONE, 0},	/* 32 bit path (unused) */
	{0x06000000, 0x07000000, MAP_VRAM, 1},	/* VRAM mirror */
	{0x07000000, 0x08000000, MAP_NONE, 0},	/* 32 bit path (unused) mirror */
	{0x08000000, 0x0A000000, MAP_NONE, 0},	/* Area 2 */
	{0x0A000000, 0x0C000000, MAP_ERAM, 1},	/* a NAOMI 2's geometry chip (nothing elsewhere) */
	{0x0C000000, 0x10000000, MAP_RAM, 1}	/* Area 3 (main RAM + 3 mirrors) */
};

static void vmem_range_set(vmem_mapping *m, u64 start, u64 end, int what, bool allow_writes)
{
	m->start_address = start;
	m->end_address = end;
	m->allow_writes = allow_writes;
	switch (what)
	{
	case MAP_RAM:  m->memoffset = MAP_RAM_START_OFFSET;  m->memsize = RAM_SIZE;  break;
	case MAP_VRAM: m->memoffset = MAP_VRAM_START_OFFSET; m->memsize = VRAM_SIZE; break;
	case MAP_ARAM: m->memoffset = MAP_ARAM_START_OFFSET; m->memsize = ARAM_SIZE; break;
	case MAP_ERAM: m->memoffset = MAP_ERAM_START_OFFSET; m->memsize = ERAM_SIZE; break;
	default:       m->memoffset = 0;                     m->memsize = 0;         break;
	}
}

bool _vmem_reserve(void)
{
	/* (the 4 GB layout's: P0/U0, P1, P2 and P3 of the area's, the rest) */
	vmem_mapping maps[4 * ARRAY_SIZE(ranges_4gb_area) + 8];
	unsigned n = 0, i;
	VMemType vmemstatus = MemTypeError;

	_vmem_elan_ram = NULL;
	/* (said again below if this load gets the 4 GB layout: the last one's does not count) */
	vmem_4gb_space = false;

	/* Use vmem only if settings mandate so, and if we have proper exception handlers. */
#ifndef TARGET_NO_EXCEPTIONS
#ifdef HAVE_LIBNX
	settings.dynarec.disable_nvmem = 1;
#endif /* HAVE_LIBNX */
	if (!settings.dynarec.disable_nvmem)
		vmemstatus = vmem_platform_init((void**)&virt_ram_base, (void**)&p_sh4rcb);
#endif
	
	/* Fallback to statically allocated buffers, this results in slow-ops being generated. */
	if (vmemstatus == MemTypeError) {
		WARN_LOG(VMEM, "Warning! nvmem is DISABLED (due to failure or not being built-in)\n");
		virt_ram_base = 0;

		/* Allocate it all and initialize it. */
		p_sh4rcb = (Sh4RCB*)malloc_pages(sizeof(Sh4RCB));
#if FEAT_SHREC != DYNAREC_NONE
		bm_vmem_pagefill((void**)p_sh4rcb->fpcb, sizeof(p_sh4rcb->fpcb));
#endif

		/* Each with a page more than it holds. An access is not checked for
		 * alignment, and a game can read or write 4 bytes from the last 2
		 * of a memory, or 8 from its last 4 (a double-precision move): past
		 * its end, which on the host was the heap. (In the memory mapped
		 * layouts what follows main RAM is its next mirror.) */
		mem_b.size = RAM_SIZE;
		mem_b.data = (u8*)malloc_pages(RAM_SIZE + PAGE_SIZE);

		vram.size = VRAM_SIZE;
		vram.data = (u8*)malloc_pages(VRAM_SIZE + PAGE_SIZE);

		aica_ram.size = ARAM_SIZE;
		aica_ram.data = (u8*)malloc_pages(ARAM_SIZE + PAGE_SIZE);

		if (mem_b.data)
			memset(mem_b.data + RAM_SIZE, 0, PAGE_SIZE);
		if (vram.data)
			memset(vram.data + VRAM_SIZE, 0, PAGE_SIZE);
		if (aica_ram.data)
			memset(aica_ram.data + ARAM_SIZE, 0, PAGE_SIZE);
	}
	else {
		NOTICE_LOG(VMEM, "Info: nvmem is enabled, with addr space of size %s\n", vmemstatus == MemType4GB ? "4GB" : "512MB");
		INFO_LOG(VMEM, "Info: p_sh4rcb: %p virt_ram_base: %p\n", (void *)p_sh4rcb, (void *)virt_ram_base);
		/* Map the different parts of the memory file into the new memory range we got. */
		if (vmemstatus == MemType512MB)
		{
			for (i = 0; i < ARRAY_SIZE(ranges_512mb); i++)
				vmem_range_set(&maps[n++], ranges_512mb[i].start, ranges_512mb[i].end,
						ranges_512mb[i].what, ranges_512mb[i].allow_writes);
			vmem_platform_create_mappings(maps, n);

			/* Point buffers to actual data pointers */
			aica_ram.data = &virt_ram_base[0x20000000];  /* Points to the writable AICA addrspace */
			vram.data = &virt_ram_base[0x04000000];   /* Points to first vram mirror (writable and lockable) */
			mem_b.data = &virt_ram_base[0x0C000000];   /* Main memory, first mirror */
			_vmem_elan_ram = ERAM_SIZE ? &virt_ram_base[0x0A000000] : NULL;
		}
		else
		{
			/* P0/U0, P1, P2 and P3, each with what is above the area's ranges unused */
			static const u32 bases[4] = { 0x00000000, 0x80000000, 0xA0000000, 0xC0000000 };
			static const u32 tops[4] = { 0x80000000, 0xA0000000, 0xC0000000, 0xE0000000 };
			unsigned b;
			for (b = 0; b < 4; b++)
			{
				for (i = 0; i < ARRAY_SIZE(ranges_4gb_area); i++)
					vmem_range_set(&maps[n++], (u64)bases[b] + ranges_4gb_area[i].start,
							(u64)bases[b] + ranges_4gb_area[i].end,
							ranges_4gb_area[i].what, ranges_4gb_area[i].allow_writes);
				vmem_range_set(&maps[n++], (u64)bases[b] + 0x10000000, tops[b], MAP_NONE, false);	/* Area 4-7 (unused) */
			}
			vmem_range_set(&maps[n++], 0xE0000000u, (u64)1 << 32, MAP_NONE, false);	/* P4 */
			vmem_platform_create_mappings(maps, n);

			/* Point buffers to actual data pointers */
			aica_ram.data = &virt_ram_base[0x80800000];  /* Points to the first AICA addrspace in P1 */
			vram.data = &virt_ram_base[0x84000000];   /* Points to first vram mirror (writable and lockable) in P1 */
			mem_b.data = &virt_ram_base[0x8C000000];   /* Main memory, first mirror in P1 */
			_vmem_elan_ram = ERAM_SIZE ? &virt_ram_base[0x8A000000] : NULL;

			vmem_4gb_space = true;
		}

		aica_ram.size = ARAM_SIZE;
		vram.size = VRAM_SIZE;
		mem_b.size = RAM_SIZE;
	}

	/* Clear out memory */
	varray2_zero(&aica_ram);
	varray2_zero(&vram);
	varray2_zero(&mem_b);

	return true;
}

/* (what _aligned_malloc() gave has to go back to _aligned_free(): free() corrupts the heap) */
static void free_pages(void *p)
{
#ifdef _WIN32
	_aligned_free(p);
#else
	free(p);
#endif
}

#define freedefptr(x) \
	if (x) { free_pages(x); x = NULL; }

void _vmem_release(void) {
	_vmem_elan_ram = NULL;
	if (virt_ram_base)
		vmem_platform_destroy();
	else {
		_vmem_unprotect_vram(0, VRAM_SIZE);
		freedefptr(p_sh4rcb);
		freedefptr(vram.data);
		freedefptr(aica_ram.data);
		freedefptr(mem_b.data);
	}
}

/* Which pages of vram are write-protected, one bit a page. The render
 * thread protects pages and the emulation thread unprotects them, each
 * with one atomic operation on the word. */
static retro_atomic_int_t vram_page_protected[VRAM_SIZE_MAX / PAGE_SIZE / 32];

bool _vmem_vram_page_protected(u32 page)
{
	return (retro_atomic_load_acquire_int(&vram_page_protected[page >> 5]) >> (page & 31)) & 1;
}

static void vram_pages_set_protected(u32 addr, u32 size, bool on)
{
	u32 page;
	for (page = addr / PAGE_SIZE; page <= (addr + size - 1) / PAGE_SIZE; page++)
	{
		const int bit = (int)(1u << (page & 31));
		if (on)
			retro_atomic_fetch_or_int(&vram_page_protected[page >> 5], bit);
		else
			retro_atomic_fetch_and_int(&vram_page_protected[page >> 5], ~bit);
	}
}

/* Pages of video memory the game must not touch unnoticed at all, reads
 * included: what was rendered to a texture there is still on the graphics
 * card. See rend/rtt_watch.h. A watched page has no access whatever its
 * write protection says; that is kept, and is what the page goes back to. */
static retro_atomic_int_t vram_page_watched[VRAM_SIZE_MAX / PAGE_SIZE / 32];

bool _vmem_vram_page_watched(u32 page)
{
	return (retro_atomic_load_acquire_int(&vram_page_watched[page >> 5]) >> (page & 31)) & 1;
}

enum { VRAM_OPEN, VRAM_READ_ONLY, VRAM_NO_ACCESS };

/* Every mapping the game or the core reaches these pages through. */
static void vram_set_access(u32 addr, u32 size, int access)
{
	bool (* const set)(void *, size_t) = access == VRAM_OPEN ? mem_region_unlock
		: access == VRAM_READ_ONLY ? mem_region_lock : mem_region_noaccess;

	if (_nvmem_enabled())
	{
		/* The mirror 32 MB up is where a NAOMI 2 has its second PowerVR's
		 * memory, and its games write every texture there as well: the
		 * two are one memory here. */
		const bool mirror = settings.System == DC_PLATFORM_NAOMI2;

		if (!mmu_enabled() || !_nvmem_4gb_space())
		{
			set(virt_ram_base + 0x04000000 + addr, size);	/* P0 */
			if (mirror)
				set(virt_ram_base + 0x06000000 + addr, size);
			if (VRAM_SIZE == 0x800000)
				/* wraps when only 8MB VRAM */
				set(virt_ram_base + 0x04000000 + addr + VRAM_SIZE, size);
		}
		if (_nvmem_4gb_space())
		{
			set(virt_ram_base + 0x84000000 + addr, size);	/* P1 */
			set(virt_ram_base + 0xA4000000 + addr, size);	/* P2 */
			if (mirror)
			{
				set(virt_ram_base + 0x86000000 + addr, size);
				set(virt_ram_base + 0xA6000000 + addr, size);
			}
			/* We should also lock P3, and the other machines' mirrors, but they don't seem to be used... */
			if (VRAM_SIZE == 0x800000)
			{
				set(virt_ram_base + 0x84000000 + addr + VRAM_SIZE, size);
				set(virt_ram_base + 0xA4000000 + addr + VRAM_SIZE, size);
			}
		}
	}
	else
		set(&vram.data[addr], size);
}

/* What each page of the range is to have, from the two sets of marks. */
static void vram_apply_access(u32 addr, u32 size)
{
	const u32 first = addr / PAGE_SIZE, last = (addr + size - 1) / PAGE_SIZE;
	u32 page;

	for (page = first; page <= last; )
	{
		const int access = _vmem_vram_page_watched(page) ? VRAM_NO_ACCESS
			: _vmem_vram_page_protected(page) ? VRAM_READ_ONLY : VRAM_OPEN;
		u32 end = page + 1;

		/* runs of pages that get the same, in one call each */
		while (end <= last && access == (_vmem_vram_page_watched(end) ? VRAM_NO_ACCESS
				: _vmem_vram_page_protected(end) ? VRAM_READ_ONLY : VRAM_OPEN))
			end++;
		vram_set_access(page * PAGE_SIZE, (end - page) * PAGE_SIZE, access);
		page = end;
	}
}

static bool vram_any_watched(u32 addr, u32 size)
{
	u32 page;
	for (page = addr / PAGE_SIZE; page <= (addr + size - 1) / PAGE_SIZE; page++)
		if (_vmem_vram_page_watched(page))
			return true;
	return false;
}

void _vmem_protect_vram(u32 addr, u32 size)
{
	addr &= VRAM_MASK;
	vram_pages_set_protected(addr, size, true);
	if (vram_any_watched(addr, size))
		vram_apply_access(addr, size);
	else
		vram_set_access(addr, size, VRAM_READ_ONLY);
}

void _vmem_unprotect_vram(u32 addr, u32 size)
{
	addr &= VRAM_MASK;
	vram_pages_set_protected(addr, size, false);
	if (vram_any_watched(addr, size))
		vram_apply_access(addr, size);
	else
		vram_set_access(addr, size, VRAM_OPEN);
}

void _vmem_watch_vram(u32 addr, u32 size)
{
	u32 page;
	addr &= VRAM_MASK;
	for (page = addr / PAGE_SIZE; page <= (addr + size - 1) / PAGE_SIZE; page++)
		retro_atomic_fetch_or_int(&vram_page_watched[page >> 5], (int)(1u << (page & 31)));
	vram_set_access(addr, size, VRAM_NO_ACCESS);
}

/* For whoever is about to fill watched pages in: writable, still marked. */
void _vmem_open_watched_vram(u32 addr, u32 size)
{
	vram_set_access(addr & VRAM_MASK, size, VRAM_OPEN);
}

void _vmem_unwatch_vram(u32 addr, u32 size)
{
	u32 page;
	addr &= VRAM_MASK;
	for (page = addr / PAGE_SIZE; page <= (addr + size - 1) / PAGE_SIZE; page++)
		retro_atomic_fetch_and_int(&vram_page_watched[page >> 5], ~(int)(1u << (page & 31)));
	vram_apply_access(addr, size);
}

u32 _vmem_get_vram_offset(void *addr)
{
	if (_nvmem_enabled())
	{
		ptrdiff_t offset = (u8*)addr - virt_ram_base;
		if (_nvmem_4gb_space())
		{
			if (mmu_enabled())
			{
				/* Only kernel mirrors */
				if (offset < 0x80000000 || offset >= 0xE0000000)
					return -1;
			}
			else
			{
				if (offset < 0 || offset >= 0xE0000000)
					return -1;
			}
			offset &= 0x1FFFFFFF;
		}
		else
		{
			if (offset < 0 || offset >= 0x20000000)
				return -1;
		}
		if ((offset >> 24) != 4 && ((offset >> 24) != 6 || settings.System != DC_PLATFORM_NAOMI2))
			return -1;
      if ((((u8*)addr - virt_ram_base) >> 29) != 0 && (((u8*)addr - virt_ram_base) >> 29) != 4  && (((u8*)addr - virt_ram_base) >> 29) != 5)
			/* other areas aren't mapped atm */
			return -1;

		return offset & VRAM_MASK;
	}
	else
	{
		/* On 64-bit ARM Android a heap pointer has a tag in its top byte,
		 * and the address of a fault, which this is given, has not: the
		 * two did not match, and a write to a protected page of video
		 * memory was not taken for one and stopped the emulator. */
#if defined(__aarch64__) && defined(__ANDROID__)
		const uintptr_t untag = 0x00ffffffffffffffull;
#else
		const uintptr_t untag = ~(uintptr_t)0;
#endif
		const uintptr_t offset = ((uintptr_t)addr & untag) - ((uintptr_t)vram.data & untag);
		if (offset >= VRAM_SIZE)
			return -1;

		return (u32)offset;
	}
}
