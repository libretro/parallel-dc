/* What the Direct3D 11 context (dx11context_lr.c) asks of the renderers'
 * shaders (dx11_shaders.cpp): made with the device, and the quad's. */
#pragma once
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#ifdef __cplusplus
extern "C" {
#endif

void dx11_shaders_init(ID3D11Device *device, pD3DCompile compile);
void dx11_shaders_term(void);
ID3D11VertexShader *dx11_shaders_quad_vs(void);
ID3D11PixelShader *dx11_shaders_quad_ps(void);
/* (what the quad's input layout is made for) */
ID3DBlob *dx11_shaders_quad_vs_blob(void);

#ifdef __cplusplus
}
#endif
