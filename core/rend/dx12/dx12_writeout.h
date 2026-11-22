/* The chip's write-out (rend/tile/pvr_scaler.h: the Y filter, then the
 * packing to a framebuffer of 16 bits a pixel and its dither) for the
 * Direct3D 12 raster renderers' pictures, as the core option
 * reicast_pvr2_scaler asks: the compute renderer's own pass
 * (pvr_compute.h's PVRC_K_YFILTER, the same DXIL), run in place over the
 * picture before what goes over it (the VMUs, the crosshairs). Made in
 * dx12_writeout_init(), when the renderer starts; nothing is made in a frame, and a
 * frame without a write-out records nothing. */
#pragma once
#include "dx12context_lr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* whether the pictures are to be made for it (unordered access views of them) */
bool dx12_writeout_wanted(void);
void dx12_writeout_init(void);
void dx12_writeout_term(void);
/* the pass over @picture, which is left in D3D12_RESOURCE_STATE_UNORDERED_ACCESS if there was one */
void dx12_writeout_record(dx12_image *picture);

#ifdef __cplusplus
}
#endif
