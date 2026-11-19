/*
	Created on: Jun 2, 2019

	Copyright 2019 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef SH4_DYNA_SSA_H
#define SH4_DYNA_SSA_H
#include <stdio.h>
#include <math.h>
#include "types.h"
#include "decoder.h"
#include "blockmanager.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Each register a parameter names given the version it is at there, a
 * register written being a new version of it */
void ssa_add_versions(RuntimeBlockInfo *block);

/* The block's operations made fewer and simpler: constants carried through
 * and done here, what is never read gone, copies read from the register
 * copied, shifts in a row made one */
void AnalyseBlock(RuntimeBlockInfo *block);

#ifdef __cplusplus
}
#endif

#endif
