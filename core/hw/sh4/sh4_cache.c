/*
	Copyright 2020 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
/* The interpreter's instruction and operand caches */
#include <string.h>
#include <retro_miscellaneous.h>
#include "sh4_cache.h"
#include "sh4_mem.h"
#include "modules/mmu.h"
#include "modules/ccn.h"
#include "hw/sh4/sh4_core.h"
#include "hw/mem/_vmem.h"

/* (a line as the save states have it: the C++ caches' std::array of them) */
typedef struct
{
	bool valid;
	u32 address;
	u8 data[32];
} icache_line;

typedef struct
{
	bool valid;
	bool dirty;
	u32 address;
	u8 data[32];
} ocache_line;

typedef char icache_line_is_40_bytes[sizeof(icache_line) == 40 ? 1 : -1];
typedef char ocache_line_is_40_bytes[sizeof(ocache_line) == 40 ? 1 : -1];

static icache_line icache_lines[256];
static ocache_line ocache_lines[512];

static bool cached_area(u32 area)
{
	static const bool cached_areas[8] = {
			true, true, true, true,	/* P0/U0 */
			true,					/* P1 */
			false,					/* P2 */
			true,					/* P3 */
			false					/* P4 */
	};
	return cached_areas[area];
}

static bool translated_area(u32 area)
{
	static const bool translated_areas[8] = {
			true, true, true, true,	/* P0/U0 */
			false,					/* P1 */
			false,					/* P2 */
			true,					/* P3 */
			false					/* P4 */
	};
	return translated_areas[area];
}

/* A line's 32 bytes from memory at @line_addr into @data, or back */
static void line_read(u32 line_addr, u8 *data)
{
	u8 *mem_ptr = GetMemPtr(line_addr, 32);
	if (mem_ptr != NULL)
		memcpy(data, mem_ptr, 32);
	else
	{
		u32 *p = (u32 *)data;
		int i;
		for (i = 0; i < 32; i += 4)
			*p++ = _vmem_ReadMem32(line_addr + i);
	}
}

static void line_write(u32 line_addr, const u8 *data)
{
	u8 *mem_ptr = GetMemPtr(line_addr, 32);
	if (mem_ptr != NULL)
		memcpy(mem_ptr, data, 32);
	else
	{
		const u32 *p = (const u32 *)data;
		int i;
		for (i = 0; i < 32; i += 4)
			_vmem_WriteMem32(line_addr + i, *p++);
	}
}

/* Instruction cache */

static u32 icache_translate(u32 address, u32 *phys_addr, bool *cached)
{
	const u32 area = address >> 29;
	const bool user_mode = sr.MD == 0;

	/* Alignment errors */
	if (address & 1)
		return MMU_ERROR_BADADDR;

	/* (kernel memory is not protected in user mode: Windows CE fails then) */
	if (!user_mode)
	{
		/* P4 not executable */
		if (area == 7)
			return MMU_ERROR_BADADDR;
	}
	*cached = CCN_CCR.ICE == 1 && cached_area(area);

	if (CCN_MMUCR.AT == 0 || !translated_area(area)
			/* 7C000000 to 7FFFFFFF in P0/U0 not translated */
			|| (address & 0xFC000000) == 0x7C000000)
		*phys_addr = address;
	else
	{
		const TLB_Entry *entry;
		u32 err = mmu_instruction_lookup_c(address, &entry, phys_addr);

		if (err != MMU_ERROR_NONE)
			return err;

		/* Privileged mode only */
		if (user_mode && (entry->Data.PR >> 1) == 0)
			return MMU_ERROR_PROTECTED;
		*cached = *cached && entry->Data.C == 1;
	}
	return MMU_ERROR_NONE;
}

#ifdef STRICT_MODE
static u16 DYNACALL icache_read16(u32 address)
{
	bool cache_on = false;
	u32 phys_addr = 0;
	u32 index, tag;
	icache_line *line;
	u32 err = icache_translate(address, &phys_addr, &cache_on);
	if (err != MMU_ERROR_NONE)
		mmu_raise_exception(err, address, MMU_TT_IREAD);

	if (!cache_on)
		return _vmem_ReadMem16(phys_addr);

	index = CCN_CCR.IIX ?
			((address >> 5) & 0x7f) | ((address >> (25 - 7)) & 0x80)
			: (address >> 5) & 0xff;
	line = &icache_lines[index];
	tag = (phys_addr >> 10) & 0x7ffff;
	if (!line->valid || tag != line->address)
	{
		/* miss */
		line->valid = true;
		line->address = tag;
		line_read(phys_addr & ~0x1f, line->data);
	}

	return *(u16 *)&line->data[phys_addr & 0x1f];
}
#endif

void sh4_icache_invalidate(void)
{
	unsigned i;
	for (i = 0; i < ARRAY_SIZE(icache_lines); i++)
		icache_lines[i].valid = false;
}

u32 sh4_icache_read_address_array(u32 addr)
{
	const icache_line *line = &icache_lines[(addr >> 5) & 0xFF];
	return line->valid | (line->address << 10);
}

void sh4_icache_write_address_array(u32 addr, u32 data)
{
	icache_line *line = &icache_lines[(addr >> 5) & 0xFF];
	/* associative */
	if (addr & 8)
	{
		const u32 vaddr = data & ~0x3ff;
		bool cached = false;
		u32 phys_addr = 0;
		u32 err = icache_translate(vaddr, &phys_addr, &cached);
		if (err == MMU_ERROR_TLB_MISS)
			/* Ignore the write */
			return;
		if (err != MMU_ERROR_NONE)
			mmu_raise_exception(err, vaddr, MMU_TT_IREAD);

		if (!line->valid || ((phys_addr >> 10) & 0x7ffff) != line->address)
			/* Ignore the write */
			return;
		line->valid = data & 1;
	}
	else
	{
		line->valid = data & 1;
		line->address = (data >> 10) & 0x7ffff;
	}
}

u32 sh4_icache_read_data_array(u32 addr)
{
	return *(u32 *)&icache_lines[(addr >> 5) & 0xFF].data[addr & 0x1C];
}

void sh4_icache_write_data_array(u32 addr, u32 data)
{
	*(u32 *)&icache_lines[(addr >> 5) & 0xFF].data[addr & 0x1C] = data;
}

void sh4_icache_reset(bool hard)
{
	if (hard)
		memset(icache_lines, 0, sizeof(icache_lines));
}

void sh4_icache_serialize(void **data, unsigned int *total_size)
{
	LIBRETRO_S(icache_lines);
}

void sh4_icache_unserialize(void **data, unsigned int *total_size)
{
	LIBRETRO_US(icache_lines);
}

/* Operand cache */

static u32 ocache_line_index(u32 address)
{
	u32 index = CCN_CCR.OIX ?
			((address >> (25 - 8)) & 0x100) | ((address >> 5) & (CCN_CCR.ORA ? 0x7f : 0xff))
			: (address >> 5) & (CCN_CCR.ORA ? 0x17f : 0x1ff);
	if (CCN_CCR.ORA && (address >> 29) == 3)
		index |= 0x80;
	return index;
}

static void ocache_read_line(u32 address, ocache_line *line)
{
	line->valid = true;
	line->dirty = false;
	line_read(address & ~0x1f, line->data);
}

static void ocache_write_back_line(u32 index, const ocache_line *line)
{
	/* (the half that is RAM) */
	if (CCN_CCR.ORA && (index & 0x80))
		return;
	line_write((line->address << 10) | ((index & 0x1F) << 5), line->data);
}

/* @address's translation for an access of @size bytes, a read or a write
 * (@access); for a write, whether it is copied back (@copy_back) */
static u32 ocache_translate(u32 address, u32 size, u32 access, u32 *phys_addr,
		bool *cached, bool *copy_back)
{
	u32 area;
	bool user_mode;

	/* Alignment errors */
	if (address & (size - 1))
		return MMU_ERROR_BADADDR;
	if (access == MMU_TT_DWRITE && (address & 0xFC000000) == 0xE0000000)
	{
		/* Store queues */
		u32 rv;
		*phys_addr = address;
		return mmu_full_SQ_c(address, &rv, MMU_TT_DWRITE);
	}
	area = address >> 29;
	user_mode = sr.MD == 0;

	/* kernel mem protected in user mode */
	if (user_mode && (address & 0x80000000))
		return MMU_ERROR_BADADDR;

	*cached = CCN_CCR.OCE == 1 && cached_area(area);
	if (access == MMU_TT_DWRITE)
		/* Use CCR.CB if P1 otherwise use !CCR.WT */
		*copy_back = area == 4 ? CCN_CCR.CB : !CCN_CCR.WT;

	if (CCN_MMUCR.AT == 0 || !translated_area(area)
			/* 7C000000 to 7FFFFFFF in P0/U0 not translated */
			|| (address & 0xFC000000) == 0x7C000000)
		*phys_addr = address;
	else
	{
		const TLB_Entry *entry;
		u32 lookup = mmu_full_lookup_c(address, &entry, phys_addr);

		if (lookup != MMU_ERROR_NONE)
			return lookup;

		/* Privileged mode only */
		if (user_mode && (entry->Data.PR >> 1) == 0)
			return MMU_ERROR_PROTECTED;
		/* X0: read only, X1: read/write */
		if (access == MMU_TT_DWRITE)
		{
			if ((entry->Data.PR & 1) == 0)
				return MMU_ERROR_PROTECTED;
			if (entry->Data.D == 0)
				return MMU_ERROR_FIRSTWRITE;
			*copy_back = *copy_back && entry->Data.WT == 0;
		}
		*cached = *cached && entry->Data.C == 1;
		if ((*phys_addr & 0x1C000000) == 0x1C000000)
			/* map 1C000000-1FFFFFFF to P4 memory-mapped registers */
			*phys_addr |= 0xF0000000;
	}
	return MMU_ERROR_NONE;
}

#ifdef STRICT_MODE
/* A read of @size bytes: where in its line they are, NULL when it is not
 * cached (and is in @phys_addr) */
static u8 *ocache_read(u32 address, u32 size, u32 *phys_addr)
{
	bool cache_on = false;
	bool copy_back = false;
	u32 index, tag;
	ocache_line *line;
	u32 err = ocache_translate(address, size, MMU_TT_DREAD, phys_addr, &cache_on, &copy_back);
	if (err != MMU_ERROR_NONE)
		mmu_raise_exception(err, address, MMU_TT_DREAD);

	if (!cache_on)
		return NULL;

	index = ocache_line_index(address);
	line = &ocache_lines[index];
	tag = (*phys_addr >> 10) & 0x7ffff;
	if (!line->valid || tag != line->address)
	{
		/* miss */
		if (line->dirty && line->valid)
			/* write-back needed */
			ocache_write_back_line(index, line);
		line->address = tag;
		ocache_read_line(*phys_addr, line);
	}
	return &line->data[*phys_addr & 0x1f];
}

/* A write of @size bytes: where in its line they go (NULL: not there),
 * and whether memory is written too (@through, at @phys_addr) */
static u8 *ocache_write(u32 address, u32 size, u32 *phys_addr, bool *through)
{
	bool cache_on = false;
	bool copy_back = false;
	u32 index, tag;
	ocache_line *line;
	u32 err = ocache_translate(address, size, MMU_TT_DWRITE, phys_addr, &cache_on, &copy_back);
	if (err != MMU_ERROR_NONE)
		mmu_raise_exception(err, address, MMU_TT_DWRITE);

	*through = true;
	if (!cache_on)
		return NULL;

	index = ocache_line_index(address);
	line = &ocache_lines[index];
	tag = (*phys_addr >> 10) & 0x7ffff;
	if (copy_back)
	{
		/* a miss reads the line; then it is written and dirty */
		if (!line->valid || tag != line->address)
		{
			if (line->dirty && line->valid)
				/* write-back needed */
				ocache_write_back_line(index, line);
			line->address = tag;
			ocache_read_line(*phys_addr, line);
		}
		line->dirty = true;
		*through = false;
		return &line->data[*phys_addr & 0x1f];
	}
	/* write-through: a hit is updated, memory is written */
	if (line->valid && tag == line->address)
		return &line->data[*phys_addr & 0x1f];
	return NULL;
}

static u8 DYNACALL ocache_read8(u32 address)
{
	u32 phys_addr = 0;
	u8 *p = ocache_read(address, 1, &phys_addr);
	return p ? *p : _vmem_ReadMem8(phys_addr);
}

static u16 DYNACALL ocache_read16(u32 address)
{
	u32 phys_addr = 0;
	u8 *p = ocache_read(address, 2, &phys_addr);
	return p ? *(u16 *)p : _vmem_ReadMem16(phys_addr);
}

static u32 DYNACALL ocache_read32(u32 address)
{
	u32 phys_addr = 0;
	u8 *p = ocache_read(address, 4, &phys_addr);
	return p ? *(u32 *)p : _vmem_ReadMem32(phys_addr);
}

static u64 DYNACALL ocache_read64(u32 address)
{
	u32 phys_addr = 0;
	u8 *p = ocache_read(address, 8, &phys_addr);
	return p ? *(u64 *)p : _vmem_ReadMem64(phys_addr);
}

static void DYNACALL ocache_write8(u32 address, u8 data)
{
	u32 phys_addr = 0;
	bool through;
	u8 *p = ocache_write(address, 1, &phys_addr, &through);
	if (p)
		*p = data;
	if (through)
		_vmem_WriteMem8(phys_addr, data);
}

static void DYNACALL ocache_write16(u32 address, u16 data)
{
	u32 phys_addr = 0;
	bool through;
	u8 *p = ocache_write(address, 2, &phys_addr, &through);
	if (p)
		*(u16 *)p = data;
	if (through)
		_vmem_WriteMem16(phys_addr, data);
}

static void DYNACALL ocache_write32(u32 address, u32 data)
{
	u32 phys_addr = 0;
	bool through;
	u8 *p = ocache_write(address, 4, &phys_addr, &through);
	if (p)
		*(u32 *)p = data;
	if (through)
		_vmem_WriteMem32(phys_addr, data);
}

static void DYNACALL ocache_write64(u32 address, u64 data)
{
	u32 phys_addr = 0;
	bool through;
	u8 *p = ocache_write(address, 8, &phys_addr, &through);
	if (p)
		*(u64 *)p = data;
	if (through)
		_vmem_WriteMem64(phys_addr, data);
}
#endif

void sh4_ocache_invalidate(void)
{
	unsigned i;
	for (i = 0; i < ARRAY_SIZE(ocache_lines); i++)
	{
		ocache_lines[i].dirty = false;
		ocache_lines[i].valid = false;
	}
}

void sh4_ocache_write_back(u32 address, bool write_back, bool invalidate)
{
	u32 phys_addr = 0;
	bool cached = false;
	bool copy_back = false;
	u32 index;
	ocache_line *line;
	u32 err = ocache_translate(address, 1, MMU_TT_DWRITE, &phys_addr, &cached, &copy_back);
	if (err != MMU_ERROR_NONE)
		mmu_raise_exception(err, address, MMU_TT_DWRITE);

	if (!cached)
		return;

	index = ocache_line_index(address);
	line = &ocache_lines[index];
	if (!line->valid || ((phys_addr >> 10) & 0x7ffff) != line->address)
		return;
	if (write_back && line->dirty)
		ocache_write_back_line(index, line);
	line->valid = !invalidate;
	line->dirty = false;
}

void sh4_ocache_prefetch(u32 address)
{
	u32 phys_addr = 0;
	bool cached = false;
	bool copy_back = false;
	u32 index, tag;
	ocache_line *line;
	u32 err;

	address &= ~0x1F;
	err = ocache_translate(address, 1, MMU_TT_DREAD, &phys_addr, &cached, &copy_back);
	if (err != MMU_ERROR_NONE || !cached)
		/* ignore address translation errors */
		return;

	index = ocache_line_index(address);
	line = &ocache_lines[index];
	tag = (phys_addr >> 10) & 0x7ffff;
	if (line->valid && tag == line->address)
		return;
	if (line->valid && line->dirty)
		ocache_write_back_line(index, line);
	line->address = tag;
	ocache_read_line(phys_addr, line);
}

u32 sh4_ocache_read_address_array(u32 addr)
{
	const ocache_line *line = &ocache_lines[(addr >> 5) & 0x1FF];
	return line->valid | (line->dirty << 1) | (line->address << 10);
}

void sh4_ocache_write_address_array(u32 addr, u32 data)
{
	u32 index = (addr >> 5) & 0x1FF;
	ocache_line *line = &ocache_lines[index];
	/* associative */
	if (addr & 8)
	{
		u32 phys_addr = 0;
		bool cached = false;
		bool copy_back = false;
		u32 err = ocache_translate(data & ~0x3ff, 1, MMU_TT_DREAD, &phys_addr, &cached, &copy_back);
		if (err == MMU_ERROR_TLB_MISS)
			/* Ignore the write */
			return;
		if (err != MMU_ERROR_NONE)
			mmu_raise_exception(err, data & ~0x3ff, MMU_TT_DREAD);

		if (!line->valid || ((phys_addr >> 10) & 0x7ffff) != line->address)
			/* Ignore the write */
			return;
		if ((data & 3) != 0 && line->dirty)
			ocache_write_back_line(index, line);
	}
	else
	{
		if (line->valid && line->dirty)
			ocache_write_back_line(index, line);
		line->address = (data >> 10) & 0x7ffff;
	}
	line->valid = data & 1;
	line->dirty = (data >> 1) & 1;
}

u32 sh4_ocache_read_data_array(u32 addr)
{
	return *(u32 *)&ocache_lines[(addr >> 5) & 0x1FF].data[addr & 0x1C];
}

void sh4_ocache_write_data_array(u32 addr, u32 data)
{
	*(u32 *)&ocache_lines[(addr >> 5) & 0x1FF].data[addr & 0x1C] = data;
}

void sh4_ocache_reset(bool hard)
{
	if (hard)
		memset(ocache_lines, 0, sizeof(ocache_lines));
}

void sh4_ocache_serialize(void **data, unsigned int *total_size)
{
	LIBRETRO_S(ocache_lines);
}

void sh4_ocache_unserialize(void **data, unsigned int *total_size)
{
	LIBRETRO_US(ocache_lines);
}

#ifdef STRICT_MODE
void sh4_caches_set_handlers(void)
{
	IReadMem16 = &icache_read16;
	ReadMem8 = &ocache_read8;
	ReadMem16 = &ocache_read16;
	ReadMem32 = &ocache_read32;
	ReadMem64 = &ocache_read64;

	WriteMem8 = &ocache_write8;
	WriteMem16 = &ocache_write16;
	WriteMem32 = &ocache_write32;
	WriteMem64 = &ocache_write64;
}

void sh4_caches_write_back(void)
{
	unsigned i;
	for (i = 0; i < ARRAY_SIZE(ocache_lines); i++)
	{
		ocache_line *line = &ocache_lines[i];
		if (line->valid && line->dirty)
			ocache_write_back_line(i, line);
		line->valid = false;
		line->dirty = false;
	}
	sh4_icache_invalidate();
}
#endif

void sh4_caches_reset(bool hard)
{
	sh4_icache_reset(hard);
	sh4_ocache_reset(hard);
}
