#pragma once
#include "types.h"
#include "ccn.h"
#include "mmu.h"

/* mmu_data_translation_r8() ... _w64() over the file's own
 * data_translation(va, rv, translation_type, size), which the compiler
 * makes one of for each */
#define MMU_DATA_TRANSLATIONS \
	u32 mmu_data_translation_r8(u32 va, u32 *rv) { return data_translation(va, rv, MMU_TT_DREAD, 1); } \
	u32 mmu_data_translation_r16(u32 va, u32 *rv) { return data_translation(va, rv, MMU_TT_DREAD, 2); } \
	u32 mmu_data_translation_r32(u32 va, u32 *rv) { return data_translation(va, rv, MMU_TT_DREAD, 4); } \
	u32 mmu_data_translation_r64(u32 va, u32 *rv) { return data_translation(va, rv, MMU_TT_DREAD, 8); } \
	u32 mmu_data_translation_w8(u32 va, u32 *rv) { return data_translation(va, rv, MMU_TT_DWRITE, 1); } \
	u32 mmu_data_translation_w16(u32 va, u32 *rv) { return data_translation(va, rv, MMU_TT_DWRITE, 2); } \
	u32 mmu_data_translation_w32(u32 va, u32 *rv) { return data_translation(va, rv, MMU_TT_DWRITE, 4); } \
	u32 mmu_data_translation_w64(u32 va, u32 *rv) { return data_translation(va, rv, MMU_TT_DWRITE, 8); }
