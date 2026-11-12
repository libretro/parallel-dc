/*
	Mostly buggy, old, glue code that somehow still works
	Most of the work is now delegated on vtlb and only helpers are here
*/
#include "types.h"

#include "sh4_mem.h"
#include "sh4_rom.h"
#include "hw/holly/sb_mem.h"
#include "sh4_mmr.h"
#include "modules/modules.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/elan_host.h"
#include "hw/sh4/sh4_core.h"
#include "hw/mem/_vmem.h"
#include "modules/mmu.h"
#include "sh4_cache.h"

/* main system mem */
VArray2 mem_b;

#ifndef NO_MMU
/* Memory handlers */
ReadMem8Func ReadMem8;
ReadMem16Func ReadMem16;
ReadMem16Func IReadMem16;
ReadMem32Func ReadMem32;
ReadMem64Func ReadMem64;

WriteMem8Func WriteMem8;
WriteMem16Func WriteMem16;
WriteMem32Func WriteMem32;
WriteMem64Func WriteMem64;
#endif


/* AREA 1 */
static _vmem_handler area1_32b;

static void map_area1_init(void)
{
   area1_32b = _vmem_register_handler(pvr_read_area1_8, pvr_read_area1_16, pvr_read_area1_32,
			pvr_write_area1_8, pvr_write_area1_16, pvr_write_area1_32);
}

static void map_area1(u32 base)
{
	/* VRAM */
	
   /* Lower 32 mb map */
	/* 64b interface */
	_vmem_map_block(vram.data, 0x04 | base, 0x04 | base, VRAM_MASK);
	/* 32b interface */
	_vmem_map_handler(area1_32b, 0x05 | base, 0x05 | base);

	/* Upper 32 mb mirror */
	/* 0x0600 to 0x07FF */
	_vmem_mirror_mapping(0x06 | base, 0x04 | base, 0x02);
}

/* AREA 2 */
static void map_area2_init(void)
{
	/* the NAOMI 2's geometry processor */
	elan_host_map_init();
}

static void map_area2(u32 base)
{
	elan_host_map(base);
}


/* AREA 3 */
static void map_area3_init(void)
{
}

static void map_area3(u32 base)
{
	/* System RAM */
   _vmem_map_block_mirror(mem_b.data, 0x0C | base,0x0F | base, RAM_SIZE);
}

/* AREA 4 */
static _vmem_handler area4_handler_lower;
static _vmem_handler area4_handler_upper;

static void map_area4_init(void)
{
   area4_handler_lower = _vmem_register_handler(pvr_read_area4_lo_8, pvr_read_area4_lo_16, pvr_read_area4_lo_32,
									pvr_write_area4_lo_8, pvr_write_area4_lo_16, pvr_write_area4_lo_32);
	area4_handler_upper = _vmem_register_handler(pvr_read_area4_hi_8, pvr_read_area4_hi_16, pvr_read_area4_hi_32,
									pvr_write_area4_hi_8, pvr_write_area4_hi_16, pvr_write_area4_hi_32);
}

static void map_area4(u32 base)
{
   /* VRAM 64b/32b interface */
	_vmem_map_handler(area4_handler_lower, 0x11 | base, 0x11 | base);
	/* upper mirror */
	_vmem_map_handler(area4_handler_upper, 0x13 | base, 0x13 | base);
}


/* AREA 5	--	Ext. Device */
/* Read Ext.Device */
#define EXTDEV_SIZED(type, bytes) \
	static type DYNACALL ReadMem_extdev_##bytes(u32 addr) { return (type)libExtDevice_ReadMem_A5(addr, bytes); } \
	static void DYNACALL WriteMem_extdev_##bytes(u32 addr, type data) { libExtDevice_WriteMem_A5(addr, data, bytes); }
EXTDEV_SIZED(u8, 1)
EXTDEV_SIZED(u16, 2)
EXTDEV_SIZED(u32, 4)

_vmem_handler area5_handler;
static void map_area5_init(void)
{
	area5_handler = _vmem_register_handler(ReadMem_extdev_1, ReadMem_extdev_2, ReadMem_extdev_4,
			WriteMem_extdev_1, WriteMem_extdev_2, WriteMem_extdev_4);
}

static void map_area5(u32 base)
{
   /* map whole region to plugin handler */
	_vmem_map_handler(area5_handler,base|0x14,base|0x17);
}

/* AREA 6	--	Unassigned */
static void map_area6_init(void)
{
}
static void map_area6(u32 base)
{
	(void)base;
}


/* set vmem to default values */
void mem_map_default(void)
{
	int i;

	_vmem_init();

	/* U0/P0 */
	/* 0x0xxx xxxx	-> normal memmap */
	/* 0x2xxx xxxx	-> normal memmap */
	/* 0x4xxx xxxx	-> normal memmap */
	/* 0x6xxx xxxx	-> normal memmap */
	/* ----------- */
	/* P1 */
	/* 0x8xxx xxxx	-> normal memmap */
	/* ----------- */
	/* P2 */
	/* 0xAxxx xxxx	-> normal memmap */
	/* ----------- */
	/* P3 */
	/* 0xCxxx xxxx	-> normal memmap */
	/* ----------- */
	/* P4 */
	/* 0xExxx xxxx	-> internal area */

	/* Init Memmaps (register handlers) */
	map_area0_init();
	map_area1_init();
	map_area2_init();
	map_area3_init();
	map_area4_init();
	map_area5_init();
	map_area6_init();
	map_area7_init();

   /* 00-E0: eight times the normal memory map. The eighth is under the P4
    * region: the SH4 only keeps the store queues, the cache and TLB arrays
    * and its own registers there (see map_p4), and an address in the rest
    * of it (E4-EF, F8-FE) reaches the same memory as it does anywhere
    * else. NHL 2K2 gets at its textures that way. */
   for (i = 0; i < 8; i++)
	{
		map_area0(i << 5); /* Bios,Flahsrom,i/f regs,Ext. Device,Sound Ram */
		map_area1(i << 5); /* VRAM */
		map_area2(i << 5); /* Unassigned */
		map_area3(i << 5); /* RAM */
		map_area4(i << 5); /* TA */
		map_area5(i << 5); /* Ext. Device */
		map_area6(i << 5); /* Unassigned */
		map_area7(i << 5); /* Sh4 Regs */
	}

   /* E0: p4 region */
	map_p4();
}
void mem_Init(void)
{
	sh4rom_init();
	/* Allocate mem for memory/bios/flash */

	sh4_area0_Init();
	sh4_mmr_init();
	MMU_init();
}

/* Reset Sysmem/Regs -- Pvr is not changed , bios/flash are not zeroed out */
void mem_Reset(bool hard)
{
   /* mem is reset on hard restart (power on), not soft reset */
	if (hard)
	{
		/* fill mem w/ 0's */
		varray2_zero(&mem_b);
	}

	/* Reset registers */
	sh4_area0_Reset(hard);
	sh4_mmr_reset(hard);
	MMU_reset();
}

void mem_Term(void)
{
	MMU_term();
	sh4_mmr_term();
	sh4_area0_Term();
	
	/* mem_b.Term(); // handled by vmem */

	/* vmem */
	_vmem_term();
}

/* How many bytes of plain memory lie in a row from an address on, the byte
 * at that address being the first of them: as far as the end of the memory
 * it is in, or of its 16 MB of the address space if that comes first. A copy
 * no longer than that is one memcpy. A longer one goes on at the address
 * that follows, as the bus would: the start of the same memory again where
 * it is mirrored, its next 16 MB, or something that is not memory at all. */
static INLINE u32 mem_run_read(u32 addr, void *ptr)
{
	bool ismem;
	return (u32)((u8 *)_vmem_read_ptr(addr | 0x00FFFFFF, &ismem, 4) - (u8 *)ptr) + 1;
}

static INLINE u32 mem_run_write(u32 addr, void *ptr)
{
	bool ismem;
	return (u32)((u8 *)_vmem_write_ptr(addr | 0x00FFFFFF, &ismem, 4) - (u8 *)ptr) + 1;
}

void WriteMemBlock_nommu_dma(u32 dst, u32 src, u32 size)
{
	bool src_ismem;
	void* src_ptr = _vmem_read_ptr(src, &src_ismem, 4);
	u32 i;

	while (src_ismem)
	{
		const u32 run = mem_run_read(src, src_ptr);

		if (likely(size <= run))
		{
			WriteMemBlock_nommu_ptr(dst, (u32*)src_ptr, size);
			return;
		}
		/* The source runs off the end of its memory: this much of it, then */
		/* the rest from wherever the address leads. */
		WriteMemBlock_nommu_ptr(dst, (u32*)src_ptr, run);
		dst += run;
		src += run;
		size -= run;
		src_ptr = _vmem_read_ptr(src, &src_ismem, 4);
	}

	verify(size % 4 == 0);
	for (i = 0; i < size; i += 4)
		WriteMem32_nommu(dst + i, ReadMem32_nommu(src + i));
}

void WriteMemBlock_nommu_ptr(u32 dst, u32* src, u32 size)
{
	bool dst_ismem;
	u32 i;

	void* dst_ptr = _vmem_write_ptr(dst, &dst_ismem, 4);

	while (dst_ismem)
	{
		const u32 run = mem_run_write(dst, dst_ptr);
		/* video memory, which every mapping of has at vram.data */
		const uintptr_t to_vram = (uintptr_t)dst_ptr - (uintptr_t)vram.data;

		if (to_vram < VRAM_SIZE)
			VramBlockWrite((u32)to_vram, size <= run ? size : run);
		if (likely(size <= run))
		{
			memcpy(dst_ptr, src, size);
			return;
		}
		/* The copy runs off the end of the memory it starts in: fill that, */
		/* and go on at the address that follows instead of past the end of */
		/* the host's block. */
		memcpy(dst_ptr, src, run);
		dst += run;
		src = (u32 *)((u8 *)src + run);
		size -= run;
		dst_ptr = _vmem_write_ptr(dst, &dst_ismem, 4);
	}

	for (i = 0; i < size;)
	{
		u32 left = size - i;
		if (left >= 4)
		{
			WriteMem32_nommu(dst + i, src[i >> 2]);
			i += 4;
		}
		else if (left >= 2)
		{
			WriteMem16_nommu(dst + i, ((u16 *)src)[i >> 1]);
			i += 2;
		}
		else
		{
			WriteMem8_nommu(dst + i, ((u8 *)src)[i]);
			i++;
		}
	}
}

void WriteMemBlock_nommu_sq(u32 dst, u32* src)
{
	bool dst_ismem;
	void* dst_ptr = _vmem_write_ptr(dst, &dst_ismem, 4);

	if (dst_ismem)
	{
		memcpy(dst_ptr, src, 32);
	}
	else
	{
		u32 i;
		for (i = 0; i < 32; i += 4)
			WriteMem32_nommu(dst + i, src[i >> 2]);
	}
}

/* Get pointer to ram area , 0 if error */
/* For debugger(gdb) - dynarec */
u8* GetMemPtr(u32 Addr,u32 size)
{
	verify((((Addr>>29) &0x7)!=7));
	switch ((Addr>>26)&0x7)
	{
		case 3:
		return &mem_b.data[Addr & RAM_MASK];
		
		case 0:
		case 1:
		case 2:
		case 4:
		case 5:
		case 6:
		case 7:
		default:
			/* printf("Get MemPtr unsupported area : addr=0x%X\n",Addr); */
			return 0;
	}
}
