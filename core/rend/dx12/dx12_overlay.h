/*
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
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include "types.h"
#include "dx12context_lr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What goes over the picture: the VMUs' screens and the light guns'
 * crosshairs, where the core options put them (as the Vulkan renderers'
 * vulkan/vmu.cpp does it). The eight pictures are made when the renderer
 * starts, and written through the upload ring when they change. */
bool dx12_overlay_init(void);
/* Before the frame: the pictures of what has changed */
void dx12_overlay_prepare(void);
/* Onto the target that is bound, @width by @height */
void dx12_overlay_draw(int width, int height);
/* Nothing to draw */
bool dx12_overlay_empty(void);
void dx12_overlay_term(void);

#ifdef __cplusplus
}
#endif
