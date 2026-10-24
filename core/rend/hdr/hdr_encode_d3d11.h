/* The HDR output with Direct3D 11: the picture handed to the frontend
 * goes through the Display Transform (hdr_encode.hlsl) into a texture of
 * the format the frontend took, drawn on the core's context right after
 * the frame, and that texture is handed over in its place. The renderers
 * draw as they always have; the 24-bit output never comes here. Shader
 * model 5.0: feature level 11.0 or later. C89. */
#ifndef HDR_ENCODE_D3D11_H__
#define HDR_ENCODE_D3D11_H__

#include <boolean.h>
#include <d3d11.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hdr_output;
typedef struct hdr_d3d11 hdr_d3d11_t;

/* @format: DXGI_FORMAT_R10G10B10A2_UNORM for HDR10,
 * DXGI_FORMAT_R16G16B16A16_FLOAT for scRGB. NULL when the device cannot
 * draw it (below feature level 11.0). */
hdr_d3d11_t *hdr_d3d11_new(ID3D11Device *device, DXGI_FORMAT format);
void hdr_d3d11_free(hdr_d3d11_t *e);

/* The picture (@picture, @width x @height, 8 bits a channel when @unorm8)
 * encoded on @context into the texture for the frontend's frame @index;
 * @texture and @view are that texture's, the encoder's to keep. What the
 * context had bound is not put back. False when it could not be. */
bool hdr_d3d11_encode(hdr_d3d11_t *e, ID3D11DeviceContext *context,
      const struct hdr_output *out, ID3D11ShaderResourceView *picture,
      unsigned width, unsigned height, bool unorm8, unsigned index,
      ID3D11Texture2D **texture, ID3D11ShaderResourceView **view);

#ifdef __cplusplus
}
#endif

#endif
