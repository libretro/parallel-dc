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
/* Textures made on the GPU from their bytes in video memory, for the
 * Direct3D 12 renderers: what dx11/dx11_texdecoder.h is for the Direct3D
 * 11 ones, with the same kernels - the Direct3D 11 renderers' DXBC
 * (rend/texdecode_dxbc.inc, shader model 5.0), which Direct3D 12 takes as
 * it is, so the texels are the CPU's converters' to the bit.
 *  - The texture's bytes - the texels, the codebook of a compressed
 *    texture, the part of the palette a paletted one uses - are copied as
 *    they are into the context's upload ring, which the kernels read as a
 *    raw buffer through a root descriptor (t0).
 *  - 32-bit texels and palette indices are written by a compute kernel
 *    through an unsigned integer view of the texture's level, made in a
 *    ring of places after the bindless table (u0, a descriptor table).
 *  - 16-bit texels are drawn into the level by a pixel shader, through a
 *    render target view made in the context's ring of those: few devices
 *    write the 16-bit formats through unordered access views.
 *  - A planar 16-bit texture needs no kernel: the PowerVR's texels are
 *    Direct3D's B5G6R5, B5G5R5A1 and B4G4R4A4 as they are, and its rows go
 *    from video memory to the texture through the ring, with a copy.
 * What a frame asks for is recorded at the end of Process()
 * (dx12_texdec_flush()), before the frame's draws, into the same command
 * list: nothing waits for the GPU, and nothing is allocated or compiled
 * while a game runs - the pipelines are made when the renderer starts. */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include "types.h"
#include "rend/texdecode.h"
#include "rend/TexCache.h"
#include "dx12context_lr.h"

#ifdef __cplusplus
extern "C" {
#endif

struct dx12_pooled_texture;

/* Everything made; nothing is done here when the device cannot */
void dx12_texdec_init(void);
void dx12_texdec_term(void);
bool dx12_texdec_ready(void);
/* Whether a texture of @type (TEX_TYPE_*) can be made by the kernels */
bool dx12_texdec_takes(int type);
/* A texture of @levels levels to be made from @bytes: where they go in
 * the ring (a multiple of 16 of them), and where that is in words.
 * Null if a texture that large never fits. */
u8 *dx12_texdec_begin(u32 bytes, u32 levels, u32 *base);
/* A level of @texture, made with the six words of rend/texdecode.h */
void dx12_texdec_add(struct dx12_pooled_texture *texture, u32 level, const u32 pc[6]);
/* Where the next level added goes, and the serial of what is not
 * recorded yet: for dx12_texdec_cancel() */
u32 dx12_texdec_next(void);
u32 dx12_texdec_serial(void);
/* The @count levels from @job on, added under @serial, are not made
 * after all: the texture is made another way, or goes */
void dx12_texdec_cancel(u32 job, u32 count, u32 serial);
/* What was asked for, recorded: the kernels run, the textures left to
 * be sampled */
void dx12_texdec_flush(void);

#ifdef __cplusplus
}
#endif
