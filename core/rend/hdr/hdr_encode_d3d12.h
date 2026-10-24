/* The HDR output with Direct3D 12: the picture handed to the frontend
 * goes through the Display Transform (hdr_encode.hlsl) into a texture of
 * the format the frontend took, recorded into the core's command list
 * after the frame and submitted with it, and that texture is handed over
 * in its place. The renderers draw as they always have; the 24-bit output
 * never comes here. C89. */
#ifndef HDR_ENCODE_D3D12_H__
#define HDR_ENCODE_D3D12_H__

#include <stdint.h>
#include <boolean.h>
#include <d3d12.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hdr_output;
typedef struct hdr_d3d12 hdr_d3d12_t;

/* @format: DXGI_FORMAT_R10G10B10A2_UNORM for HDR10,
 * DXGI_FORMAT_R16G16B16A16_FLOAT for scRGB; @handed: the state the
 * frontend wants what it is handed in. @fence: what the core's
 * submissions signal. NULL when it cannot be set up. */
hdr_d3d12_t *hdr_d3d12_new(ID3D12Device *device, DXGI_FORMAT format,
      D3D12_RESOURCE_STATES handed, ID3D12Fence *fence);

/* Frees everything; the GPU is done with it all (the core's context
 * waits for its queue before it goes). */
void hdr_d3d12_free(hdr_d3d12_t *e);

/* Records into @cmd the picture (@picture, in the pixel shader resource
 * state, @picture_format, @width x @height, 8 bits a channel when @unorm8)
 * encoded into the texture for the frontend's frame @index, which is left
 * in the state the frontend wants; @signal is the value the fence is
 * signalled with once @cmd is done. *@texture is that texture, the
 * encoder's to keep. What @cmd had bound - heaps included - is not put
 * back. False, nothing recorded, when it could not be. */
bool hdr_d3d12_encode(hdr_d3d12_t *e, ID3D12GraphicsCommandList *cmd,
      const struct hdr_output *out, ID3D12Resource *picture,
      DXGI_FORMAT picture_format, unsigned width, unsigned height,
      bool unorm8, unsigned index, uint64_t signal,
      ID3D12Resource **texture);

#ifdef __cplusplus
}
#endif

#endif
