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
#include "../dx11_shaders.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The per-pixel renderer's shaders: the Vulkan one's (vulkan/oit/oit_shaders.cpp)
 * in HLSL. They need shader model 5 (feature level 11.0). Made, kept and
 * compiled as dx11_shaders.h has it. */

typedef enum { DX11OIT_DEPTH, DX11OIT_COLOR, DX11OIT_OIT } dx11_oit_pass;
/* Their families (the file's numbers, kept) */
enum { DX11OITF_VS, DX11OITF_PS, DX11OITF_MODVOL_VS, DX11OITF_MODVOL_PS, DX11OITF_TR_MODVOL_PS,
	DX11OITF_FINAL_VS, DX11OITF_FINAL_PS, DX11OITF_GENERAL_PS };
/* Must match the order of the modifier volume modes in the shader */
typedef enum { DX11OIT_XOR, DX11OIT_OR, DX11OIT_INCLUSION, DX11OIT_EXCLUSION } dx11_oit_modvol_mode;

typedef struct dx11_oit_ps_params
{
	bool alphaTest;
	bool insideClipTest;
	bool useAlpha;
	bool texture;
	bool ignoreTexAlpha;
	int shaderInstr;
	bool offset;
	int fog;
	bool twoVolume;
	bool gouraud;
	bool bumpmap;
	bool clamping;
	bool palette;
	dx11_oit_pass pass;
} dx11_oit_ps_params;

u32 dx11_oit_shaders_key(const dx11_oit_ps_params *p);

void dx11_oit_shaders_init(ID3D11Device *device, pD3DCompile compile);
/* Everything a frame can want, made before the first: the vertex shaders,
 * the ones of a kind, the general pixel shaders */
bool dx11_oit_shaders_start(void);
void dx11_oit_shaders_term(void);
ID3D11VertexShader *dx11_oit_shaders_vs(bool gouraud);
/* A polygon's own, or the general one while its own is made (*general):
 * the polygon's key then in its constants */
ID3D11PixelShader *dx11_oit_shaders_ps(const dx11_oit_ps_params *params, bool *general);
ID3D11VertexShader *dx11_oit_shaders_modvol_vs(void);
ID3D11PixelShader *dx11_oit_shaders_modvol_ps(void);
ID3D11PixelShader *dx11_oit_shaders_tr_modvol_ps(dx11_oit_modvol_mode mode);
ID3D11VertexShader *dx11_oit_shaders_final_vs(void);
ID3D11PixelShader *dx11_oit_shaders_final_ps(void);
ID3DBlob *dx11_oit_shaders_vs_blob(void);
ID3DBlob *dx11_oit_shaders_modvol_vs_blob(void);
/* (for the tests) */
dx11_shader_compiler *dx11_oit_shaders_compiler(void);
const dx11_shader_families *dx11_oit_shaders_families(void);

#ifdef __cplusplus
}
#endif
