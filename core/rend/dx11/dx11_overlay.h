/*
	Copyright 2021 flyinghead

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
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d11.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What goes over the picture: the VMUs' screens and the light guns'
 * crosshairs, where the core options put them (as the Vulkan renderers'
 * vulkan/vk_osd.c does it). The renderer's, one at a time. */

/* Before the frame: the textures of what has changed */
void dx11_overlay_prepare(void);
/* Onto the target that is bound, @width by @height */
void dx11_overlay_draw(int width, int height);
/* Nothing to draw */
bool dx11_overlay_empty(void);
void dx11_overlay_term(void);

#ifdef __cplusplus
}
#endif
