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
#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The interpreter's instruction and operand caches emptied, as CCR's ICI
 * and OCI ask (sh4_interpreter.c) */
void sh4_icache_invalidate(void);
void sh4_ocache_invalidate(void);
/* Their address and data arrays, as P4 maps them (sh4_mmr.c) */
u32 sh4_icache_read_address_array(u32 addr);
void sh4_icache_write_address_array(u32 addr, u32 data);
u32 sh4_icache_read_data_array(u32 addr);
void sh4_icache_write_data_array(u32 addr, u32 data);
u32 sh4_ocache_read_address_array(u32 addr);
void sh4_ocache_write_address_array(u32 addr, u32 data);
u32 sh4_ocache_read_data_array(u32 addr);
void sh4_ocache_write_data_array(u32 addr, u32 data);
/* OCBI, OCBP, OCBWB and PREF on the operand cache (STRICT_MODE) */
void sh4_ocache_write_back(u32 addr, bool write_back, bool invalidate);
void sh4_ocache_prefetch(u32 addr);
/* Made as they are at power on (@hard) or at a reset */
void sh4_icache_reset(bool hard);
void sh4_ocache_reset(bool hard);
void sh4_caches_reset(bool hard);
/* Their lines, in a save state */
void sh4_icache_serialize(void **data, unsigned int *total_size);
void sh4_icache_unserialize(void **data, unsigned int *total_size);
void sh4_ocache_serialize(void **data, unsigned int *total_size);
void sh4_ocache_unserialize(void **data, unsigned int *total_size);
#ifdef STRICT_MODE
/* The SH4's memory functions the caches', for the interpreter
 * (SetMemoryHandlers()); and what they hold written back and dropped,
 * for the recompiler */
void sh4_caches_set_handlers(void);
void sh4_caches_write_back(void);
#endif

#ifdef __cplusplus
}
#endif
