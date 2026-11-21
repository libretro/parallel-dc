/* The chip's write-out (rend/tile/pvr_scaler.h: the Y filter, then the
 * packing to a framebuffer of 16 bits a pixel and its dither) for the
 * Direct3D 11 renderers' pictures, as the core option reicast_pvr2_scaler
 * asks: a compute shader's pass in place over the picture (pvr_compute.h's
 * PVRC_K_YFILTER, in Direct3D 11's terms), before what goes over it.
 * Compiled in Init() as the renderer starts, whether the option is on or
 * not: turning it on (a change of size) makes nothing; nothing is made in
 * a frame, and a frame without a write-out does nothing. Needs typed loads of R8G8B8A8_UNORM from an
 * unordered access view (Direct3D 11.3's TypedUAVLoadAdditionalFormats). */
#pragma once
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d11.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Made the first time (the renderer's start), whatever the option; true if
 * the option and the device have it used, and the pictures are to be made
 * for it (D3D11_BIND_UNORDERED_ACCESS) */
bool dx11_writeout_init(void);
void dx11_writeout_term(void);
/* The pass over the picture @uav, @width x @height, of the frame in the
 * registers now, when it has one; nothing bound is left bound */
void dx11_writeout_record(ID3D11UnorderedAccessView *uav, u32 width, u32 height);

#ifdef __cplusplus
}
#endif
