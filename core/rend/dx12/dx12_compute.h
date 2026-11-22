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
/* The compute renderer on Direct3D 12 (Rendering Method: Compute): a frame
 * drawn into the per-pixel renderer's picture of the screen by the kernels
 * of rend/tile/pvr_compute.h, tile by tile, as the Vulkan per-pixel
 * renderer has it (vulkan/compute_drawer.h), in Direct3D 12's terms:
 *
 * - Its input is the CPU's parse of the frame (ta_parse_vdrc()), packed by
 *   the same rules (rend/tile/compute_frame.h) into this frame's piece of
 *   a ring in write-combined memory of a custom heap, which the kernels
 *   read in place; or, for a frame the GPU decoded (TA processing: GPU,
 *   dx12_ta_decoder.h), the decode's own buffers, read in place: only the
 *   polygon parameters and the render passes go up, and PVRC_K_GPUSRC
 *   numbers the triangles on the GPU.
 * - The kernels are DXIL made when the source is (shader model 6.0, for
 *   their 64-bit integers: dx12_compute_dxil.inc, tools/rend/kernels_dx12.sh),
 *   the walk's for the screen's samples and for one; every pipeline, the
 *   ring, the primitives' setup and the bins are made in
 *   dx12_compute_init(), when the renderer starts or changes size: nothing
 *   is allocated or compiled while a game runs.
 * - The buffers are a table of unordered access views for each piece of
 *   the ring (so that a read past a buffer's end is 0, as Vulkan's robust
 *   buffers have it); the textures are the renderers' bindless table and
 *   samplers, a word for each of the frame's texture slots in its piece of
 *   the ring saying which; the picture is a view in that table.
 * - The dispatches whose sizes the GPU made (the clear's, and a decoded
 *   frame's setup, scatter and sort) take them from the bins' counts,
 *   copied to an argument buffer first; nothing is read back and nothing
 *   waits. A few counts for the log are copied to a readback buffer, read
 *   when the fence says that frame is done.
 * - A piece of the ring is taken again once the fence says the frame that
 *   had it is done; a frame whose piece the GPU still has is left to the
 *   per-pixel renderer (never waited for).
 * - A render to a texture is drawn the same way, at one sample a pixel as
 *   the per-pixel renderer draws it, into the very texture that renderer
 *   would have drawn it in (DX12Renderer::BeginRTT(), whose pool's
 *   pictures are made for unordered access then): the size of the frame
 *   from its first pixel to the pixel clip's last, in powers of two, times
 *   Render To Texture Upscaling, cleared outside the clip. Its bins are made
 *   for the largest such texture.
 * - A frame it cannot draw is left to the per-pixel renderer
 *   (dx12_compute_can_draw(), dx12_compute_prepare()): more textures than
 *   it has slots for, more render passes than the kernels take, a picture
 *   to start from that cannot be read where it is (the frontend's, or the
 *   one being drawn into). */
#pragma once
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include "types.h"
#include "rend/tile/compute_frame.h"
#include "dx12_ta_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Made for a picture of @width x @height and @samples a pixel (@depth:
 * and for the temporal upscaler, with the walks that store each pixel's
 * depth, at one sample); false when the device cannot have it (logged,
 * and not tried again until dx12_compute_reset()) */
bool dx12_compute_init(u32 width, u32 height, u32 samples, bool depth);
void dx12_compute_term(void);
/* ...and tried again on this device the next time */
void dx12_compute_reset(void);
/* Whether the device has what the kernels need (the reason it has not,
 * or null) */
const char *dx12_compute_device_lacks(void);
bool dx12_compute_ready(void);
/* whether a frame decoded on the GPU (pvrrc.gpu) is read from dx12_ta_get_draw() */
void dx12_compute_set_gpu_draw(bool on);
/* the screen's size it is made for */
u32 dx12_compute_width(void);
u32 dx12_compute_height(void);

/* whether the frame in pvrrc is one this draws (before anything is recorded) */
bool dx12_compute_can_draw(void);
/* The frame's words and textures, by @frame's rules, @last the place in
 * the bindless table of the picture to start from (~0u: none; @scaled,
 * one of another size); a render to a texture, into one of @rttWidth x
 * @rttHeight, at one sample a pixel. False: it does not fit, or its
 * piece of the ring is still the GPU's, and the per-pixel renderer draws
 * it; nothing is recorded either way. */
bool dx12_compute_prepare(const compute_frame_renderer *frame, u32 last, bool scaled, u32 rttWidth, u32 rttHeight);
/* ...then drawn into @picture (R8G8B8A8, made for unordered access),
 * which is in UNORDERED_ACCESS: all of it, the pixels outside the
 * frame's clip the clear colour; recorded into the context's list,
 * which the next dx12c_submit() executes. With @sums
 * (dx12_compute_sums_on(); an R16G16B16A16_UINT texture of at least the
 * screen's size, in UNORDERED_ACCESS), a frame of the screen stores each
 * pixel's channel sums into it as well, for the HDR output's encode
 * (dx12c_encode_hdr()). With @depth (dx12_compute_depth_on(); an
 * R32_FLOAT texture of the screen's size, in UNORDERED_ACCESS), a frame of
 * the screen stores each pixel's depth into it, for the temporal upscaler
 * (dx12_renderer.h) */
void dx12_compute_record(ID3D12Resource *picture, ID3D12Resource *sums, ID3D12Resource *depth);
/* Whether the walks that store the depth were made, and whether the
 * frame just recorded stored it */
bool dx12_compute_depth_on(void);
bool dx12_compute_depth_stored(void);
/* For the motion vector pass over the frame just recorded (the
 * renderer's): its vertices and indices where they are in the ring, as
 * a vertex and an index buffer, the ring transitioned to be read as
 * them (and back by the next dx12_compute_record()) */
void dx12_compute_ring_for_draw(D3D12_VERTEX_BUFFER_VIEW *vertices, D3D12_INDEX_BUFFER_VIEW *indices);
/* Whether the walks that store the channel sums were made: with the
 * HDR output when dx12_compute_init() was called, and none at 24-bit */
bool dx12_compute_sums_on(void);
/* Whether the frame just recorded stored them, and they are what the
 * picture was resolved from: SSAA's samples, or the X scaler's pairs
 * of pixels, and no Y filter over it */
bool dx12_compute_sums_stored(void);

/* The picture before copied into one of the drawer's own (made with
 * it, of the screen's size), for a frame to start from when it cannot be
 * read where it is - the picture the frame is drawn into, or the
 * frontend's (version 2): @from, of the screen's size, in COPY_SOURCE;
 * recorded after dx12_compute_prepare(), which is given its place in the
 * bindless table (dx12_compute_start_srv()) */
void dx12_compute_copy_start(ID3D12Resource *from);
u32 dx12_compute_start_srv(void);

/* a frame left to the per-pixel renderer, and why (CF_WHY_*) */
void dx12_compute_fallback(u32 why);

/* (for the raster renderers' write-out: dx12_writeout.c) */
const void *dx12_compute_kernel(u32 kernel, size_t *size);

#ifdef __cplusplus
}
#endif
