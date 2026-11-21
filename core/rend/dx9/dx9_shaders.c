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
/* The Direct3D 11 renderer's shaders (dx11/dx11_shaders.cpp) in shader
 * model 3, which is what the Vulkan and OpenGL renderers do with a pixel,
 * with these differences of Direct3D 9's:
 *
 * - There is no interpolation without the perspective. What the pixel
 *   shaders take for the depth, 1/w, goes across the screen in a straight
 *   line: the vertex shader hands over its inverse, which the perspective
 *   interpolation brings to the inverse of the straight line's value.
 * - A pixel's centre is where its coordinates are whole numbers, and not
 *   half a pixel further on: the vertices are moved back by half a pixel.
 * - The depth is the position's, as in the other renderers
 *   (rend/depth_scale.h): the vertex goes out at z = S, a depth of S*z,
 *   the guest's 1/w up to 1 at fZ_max; no pixel shader writes a depth, and
 *   one function places the vertices of every shader. (Shader model 3 has
 *   no precise: the same text in every shader is what there is.)
 * - Vertex colours are read as D3DCOLOR, blue in the first byte: the
 *   vertices have red there, and the vertex shader puts it back.
 * - Flat shading is the device's (D3DRS_SHADEMODE), as is culling. */
#include <stdlib.h>
#include <string.h>
#include "dx9_shaders.h"
#include "rend/d3d_shader_cache.h"

/* (as dx9context_lr.h's DX9_RELEASE, which this does not include: the
 * tool that makes dx9_dxbc.inc builds this file alone) */
#define DX9_SHADER_RELEASE(p) do { if ((p) != NULL) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

static const char VertexTransform[] = "\n"
"float4 normalScale : register(c0);\n"
"float4 normalOffset : register(c1);\n"
"float4 halfPixel : register(c2);\n"
"// 1/fZ_max, and the inverse of what the matrix scales z by (rend/depth_scale.h)\n"
"float4 depthScale : register(c3);\n"
"\n"
"// A vertex's z is the guest's 1/w. It is made the clip space w's inverse,\n"
"// and x and y are multiplied by w, so that the division by it leaves them\n"
"// where they are and everything in between is interpolated with the\n"
"// perspective. The clip space z is S, so that the depth is S*z: the\n"
"// guest's 1/w, linear across the screen, as the chip has it.\n"
"float4 transformVertex(float3 pos, out float invW)\n"
"{\n"
"\tfloat4 vpos = float4(pos * normalScale.xyz + normalOffset.xyz, 1.0);\n"
"\tvpos.w = 1.0 / vpos.z;\n"
"\tinvW = vpos.w;\n"
"\tvpos.xy = (vpos.xy + halfPixel.xy) * vpos.w;\n"
"\t// (a z beyond fZ_max is at depth 1, not clipped)\n"
"\tvpos.z = min(depthScale.x * depthScale.y, vpos.w);\n"
"\treturn vpos;\n"
"}\n";

static const char VertexShaderSource[] = "\n"
"struct VertexIn\n"
"{\n"
"\tfloat3 pos : POSITION;\n"
"\tfloat4 col : COLOR0;\n"
"\tfloat4 spec : COLOR1;\n"
"\tfloat2 uv : TEXCOORD0;\n"
"};\n"
"\n"
"struct VertexOut\n"
"{\n"
"\tfloat4 pos : POSITION;\n"
"\tfloat4 col : COLOR0;\n"
"\tfloat4 spec : COLOR1;\n"
"\tfloat2 uv : TEXCOORD0;\n"
"\tfloat invW : TEXCOORD1;\n"
"};\n"
"\n"
"VertexOut main(in VertexIn vin)\n"
"{\n"
"\tVertexOut vo;\n"
"\tvo.pos = transformVertex(vin.pos, vo.invW);\n"
"\tvo.col = vin.col.bgra;\n"
"\tvo.spec = vin.spec.bgra;\n"
"\tvo.uv = vin.uv;\n"
"\treturn vo;\n"
"}\n";

static const char ModVolVertexShaderSource[] = "\n"
"struct VertexOut\n"
"{\n"
"\tfloat4 pos : POSITION;\n"
"\tfloat invW : TEXCOORD1;\n"
"};\n"
"\n"
"VertexOut main(in float3 pos : POSITION)\n"
"{\n"
"\tVertexOut vo;\n"
"\t// (behind the eye, or as good as at an infinite distance: nowhere)\n"
"\tif (pos.z < 0.0 || pos.z > 3.4e37)\n"
"\t{\n"
"\t\tvo.pos = float4(0.0, 0.0, 0.0, 1.0 / pos.z);\n"
"\t\tvo.invW = vo.pos.w;\n"
"\t\treturn vo;\n"
"\t}\n"
"\tvo.pos = transformVertex(pos, vo.invW);\n"
"\treturn vo;\n"
"}\n";

static const char QuadVertexShaderSource[] = "\n"
"float4 halfPixel : register(c2);\n"
"\n"
"struct VertexIn\n"
"{\n"
"\tfloat2 pos : POSITION;\n"
"\tfloat2 uv : TEXCOORD0;\n"
"};\n"
"\n"
"struct VertexOut\n"
"{\n"
"\tfloat4 pos : POSITION;\n"
"\tfloat2 uv : TEXCOORD0;\n"
"};\n"
"\n"
"VertexOut main(in VertexIn vin)\n"
"{\n"
"\tVertexOut vo;\n"
"\tvo.pos = float4(vin.pos + halfPixel.xy, 0.0, 1.0);\n"
"\tvo.uv = vin.uv;\n"
"\treturn vo;\n"
"}\n";

static const char PixelShaderHeader[] = "\n"
"float4 colorClampMin : register(c0);\n"
"float4 colorClampMax : register(c1);\n"
"float4 sp_FOG_COL_RAM : register(c2);\n"
"float4 sp_FOG_COL_VERT : register(c3);\n"
"// the alpha test's value, the fog density, the shadows' scale\n"
"float4 frameParams : register(c4);\n"
"\n"
"sampler2D fog_table : register(s2);\n"
"\n"
"// The fog coefficient for a depth, worked out the way the PowerVR2 does it.\n"
"// The depth times the fog density, between 1 and just under 256, is taken\n"
"// apart as a float: its exponent and the top four bits of its mantissa\n"
"// pick one of the 128 table entries, and the next eight bits of mantissa\n"
"// blend that entry with the following one. The blend is done in whole\n"
"// numbers out of 256 and so is the result, which is returned over 256.\n"
"float fog_mode2(float w)\n"
"{\n"
"\tfloat z = min(max(w * frameParams.y, 1.0), 255.9999);\n"
"\tfloat e = 0.0;\n"
"\tif (z >= 16.0) { z *= 0.0625; e = 4.0; }\n"
"\tif (z >= 4.0) { z *= 0.25; e += 2.0; }\n"
"\tif (z >= 2.0) { z *= 0.5; e += 1.0; }\n"
"\tfloat t = (z - 1.0) * 16.0;\n"
"\tfloat i = floor(t);\n"
"\tfloat blend = floor((t - i) * 256.0);\n"
"\tfloat u = (e * 16.0 + i + 0.5) / 128.0;\n"
"\t// The table is two rows: the entry's own value under 0.75, the next entry's under 0.25\n"
"\tfloat next = floor(tex2Dlod(fog_table, float4(u, 0.25, 0.0, 0.0)).a * 255.0 + 0.5);\n"
"\tfloat cur = floor(tex2Dlod(fog_table, float4(u, 0.75, 0.0, 0.0)).a * 255.0 + 0.5);\n"
"\treturn floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;\n"
"}\n"
"\n"
"// (the depth is the position's, which the depth test took: none written here)\n"
"struct PSO\n"
"{\n"
"\tfloat4 col : COLOR0;\n"
"};\n";

static const char PixelShaderSource[] = "\n"
"#define PI 3.1415926\n"
"\n"
"float4 clipTest : register(c5);\n"
"// the trilinear pass's alpha, where in the palette's row the first colour\n"
"// is, the depth prepass's least alpha (0 for the colour: rend/sorter.h)\n"
"float4 polyParams : register(c6);\n"
"\n"
"// What the other renderers make a shader for: flags, 0 or 1, in c8 to c11\n"
"// (as constants of their own: not every compiler places booleans where\n"
"// they are asked to go). Every pixel of a polygon branches the same way.\n"
"float4 flags[4] : register(c8);\n"
"#define pp_UseAlpha (flags[0].x > 0.5)\n"
"#define pp_IgnoreTexA (flags[0].y > 0.5)\n"
"#define pp_Offset (flags[0].z > 0.5)\n"
"#define pp_BumpMap (flags[0].w > 0.5)\n"
"#define pp_ColorClamp (flags[1].x > 0.5)\n"
"#define pp_TriLinear (flags[1].y > 0.5)\n"
"#define pp_ClipInside (flags[1].z > 0.5)\n"
"#define cp_AlphaTest (flags[1].w > 0.5)\n"
"#define pp_Shadowed (flags[2].x > 0.5)\n"
"#define pp_ShadInstr0 (flags[2].y > 0.5)\n"
"#define pp_ShadInstr1 (flags[2].z > 0.5)\n"
"#define pp_Fog0 (flags[2].w > 0.5)\n"
"#define pp_Fog1 (flags[3].x > 0.5)\n"
"#define pp_SwapRB (flags[3].y > 0.5)\n"
"\n"
"sampler2D tex : register(s0);\n"
"sampler2D palette : register(s1);\n"
"\n"
"#if pp_Palette == 1\n"
"float4 palettePixel(float2 coords)\n"
"{\n"
"\t// polyParams.y is where the middle of the palette's first colour is in the row of 1024\n"
"\tfloat2 c = float2(tex2D(tex, coords).a * (255.0 / 1024.0) + polyParams.y, 0.5);\n"
"\t// (B G R A in the texture, the palette's R G B A bytes)\n"
"\treturn tex2Dlod(palette, float4(c, 0.0, 0.0)).bgra;\n"
"}\n"
"#endif\n"
"\n"
"struct Pixel\n"
"{\n"
"\tfloat4 col : COLOR0;\n"
"\tfloat4 spec : COLOR1;\n"
"\tfloat2 uv : TEXCOORD0;\n"
"\tfloat invW : TEXCOORD1;\n"
"\tfloat2 vpos : VPOS;\n"
"};\n"
"\n"
"PSO main(in Pixel inpix)\n"
"{\n"
"\tfloat w = 1.0 / inpix.invW;\n"
"\n"
"\t// Clip inside the box (a pixel's centre, as the other renderers have it)\n"
"\tif (pp_ClipInside)\n"
"\t{\n"
"\t\tfloat2 p = inpix.vpos + 0.5;\n"
"\t\tif (p.x >= clipTest.x && p.x <= clipTest.z && p.y >= clipTest.y && p.y <= clipTest.w)\n"
"\t\t\tdiscard;\n"
"\t}\n"
"\n"
"\tfloat4 color = inpix.col;\n"
"\tif (!pp_UseAlpha)\n"
"\t\tcolor.a = 1.0;\n"
"\tfloat4 offset = inpix.spec;\n"
"\t// A pixel inside a modifier volume: what the polygon is shaded with,\n"
"\t// its base and offset colours, is scaled before the texture is\n"
"\t// combined with them. The texture itself is not.\n"
"\tif (pp_Shadowed)\n"
"\t{\n"
"\t\tcolor.rgb *= frameParams.z;\n"
"\t\toffset.rgb *= frameParams.z;\n"
"\t}\n"
"#if pp_Texture == 1\n"
"\t{\n"
"\t#if pp_Palette == 0\n"
"\t\tfloat4 texcol = tex2D(tex, inpix.uv);\n"
"\t\tif (pp_SwapRB)\n"
"\t\t\ttexcol = texcol.bgra;\n"
"\t#else\n"
"\t\tfloat4 texcol = palettePixel(inpix.uv);\n"
"\t#endif\n"
"\t\tif (pp_BumpMap)\n"
"\t\t{\n"
"\t\t\tfloat s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;\n"
"\t\t\tfloat r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;\n"
"\t\t\ttexcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);\n"
"\t\t\ttexcol.rgb = float3(1.0, 1.0, 1.0);\n"
"\t\t}\n"
"\t\telse\n"
"\t\t{\n"
"\t\t\tif (pp_IgnoreTexA)\n"
"\t\t\t\ttexcol.a = 1.0;\n"
"\t\t}\n"
"\t\tif (pp_ShadInstr1)\n"
"\t\t{\n"
"\t\t\tif (pp_ShadInstr0)\n"
"\t\t\t\tcolor *= texcol;\n"
"\t\t\telse\n"
"\t\t\t\tcolor.rgb = lerp(color.rgb, texcol.rgb, texcol.a);\n"
"\t\t}\n"
"\t\telse\n"
"\t\t{\n"
"\t\t\tif (pp_ShadInstr0)\n"
"\t\t\t{\n"
"\t\t\t\tcolor.rgb *= texcol.rgb;\n"
"\t\t\t\tcolor.a = texcol.a;\n"
"\t\t\t}\n"
"\t\t\telse\n"
"\t\t\t\tcolor = texcol;\n"
"\t\t}\n"
"\t\tif (pp_Offset)\n"
"\t\t{\n"
"\t\t\tif (!pp_BumpMap)\n"
"\t\t\t\tcolor.rgb += offset.rgb;\n"
"\t\t}\n"
"\t}\n"
"#endif\n"
"\n"
"\tif (pp_ColorClamp)\n"
"\t\tcolor = clamp(color, colorClampMin, colorClampMax);\n"
"\n"
"\tif (pp_Fog1)\n"
"\t{\n"
"\t\t// Look-up table mode 2: the fog colour takes the place of the\n"
"\t\t// pixel's colour and the fog coefficient the place of its alpha,\n"
"\t\t// after the texture has been combined in.\n"
"\t\tif (pp_Fog0)\n"
"\t\t\tcolor = float4(sp_FOG_COL_RAM.rgb, fog_mode2(w) * (256.0 / 255.0));\n"
"\t}\n"
"\telse\n"
"\t{\n"
"\t\tif (pp_Fog0)\n"
"\t\t{\n"
"\t\t\tif (pp_Offset)\n"
"\t\t\t{\n"
"\t\t\t\tif (!pp_BumpMap)\n"
"\t\t\t\t{\n"
"\t\t\t\t\tfloat fog = offset.a * (255.0 / 256.0);\n"
"\t\t\t\t\tcolor.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;\n"
"\t\t\t\t}\n"
"\t\t\t}\n"
"\t\t}\n"
"\t\telse\n"
"\t\t{\n"
"\t\t\tfloat fog = fog_mode2(w);\n"
"\t\t\tcolor.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;\n"
"\t\t}\n"
"\t}\n"
"\n"
"\tif (pp_TriLinear)\n"
"\t\tcolor *= polyParams.x;\n"
"\n"
"\t// Punch-through polygons are tested on the alpha of the finished colour,\n"
"\t// after the texture and the vertex colours are combined and clamped.\n"
"\tif (cp_AlphaTest)\n"
"\t{\n"
"\t\tif (frameParams.x > color.a)\n"
"\t\t\tdiscard;\n"
"\t\tcolor.a = 1.0;\n"
"\t}\n"
"\t// The depth prepass of sorted translucent polygons: the pixels that\n"
"\t// hide what is behind them\n"
"\tif (color.a < polyParams.z)\n"
"\t\tdiscard;\n"
"\n"
"\tPSO pso;\n"
"\tpso.col = color;\n"
"\treturn pso;\n"
"}\n";

static const char ModVolPixelShaderSource[] = "\n"
"// The colour is only ever written by the pass that darkens what is left\n"
"// in the volumes: by what the shadows take away\n"
"PSO main(in float invW : TEXCOORD1)\n"
"{\n"
"\tPSO pso;\n"
"\tpso.col = float4(0.0, 0.0, 0.0, 1.0 - frameParams.z);\n"
"\treturn pso;\n"
"}\n";

static const char QuadPixelShaderSource[] = "\n"
"sampler2D tex : register(s0);\n"
"float4 quadColor : register(c7);\n"
"// (the polygons' flag pp_SwapRB)\n"
"float4 flags3 : register(c11);\n"
"\n"
"float4 main(in float2 uv : TEXCOORD0) : COLOR0\n"
"{\n"
"\tfloat4 c = tex2D(tex, uv);\n"
"\tif (flags3.y > 0.5)\n"
"\t\tc = c.bgra;\n"
"\treturn c * quadColor;\n"
"}\n";

/* What each is compiled from: the shared head, the body, the profile, the
 * macros, by the enums' order (vertex shaders, then pixel shaders) */
typedef struct DX9ShaderDesc
{
	const char *head;
	const char *body;
	const char *profile;
	const D3D_SHADER_MACRO *macros;
} DX9ShaderDesc;
static const D3D_SHADER_MACRO PlainMacros[] = { { "pp_Texture", "0" }, { "pp_Palette", "0" }, { NULL, NULL } };
static const D3D_SHADER_MACRO TextureMacros[] = { { "pp_Texture", "1" }, { "pp_Palette", "0" }, { NULL, NULL } };
static const D3D_SHADER_MACRO PaletteMacros[] = { { "pp_Texture", "1" }, { "pp_Palette", "1" }, { NULL, NULL } };
#define DX9_SHADERS (DX9VS_Count + DX9PS_Count)
static const DX9ShaderDesc ShaderDescs[DX9_SHADERS] = {
	{ VertexTransform, VertexShaderSource, "vs_3_0", NULL },
	{ VertexTransform, ModVolVertexShaderSource, "vs_3_0", NULL },
	{ "", QuadVertexShaderSource, "vs_3_0", NULL },
	{ PixelShaderHeader, PixelShaderSource, "ps_3_0", PlainMacros },
	{ PixelShaderHeader, PixelShaderSource, "ps_3_0", TextureMacros },
	{ PixelShaderHeader, PixelShaderSource, "ps_3_0", PaletteMacros },
	{ PixelShaderHeader, ModVolPixelShaderSource, "ps_3_0", NULL },
	{ "", QuadPixelShaderSource, "ps_3_0", NULL }
};
/* (ShaderDescs, by the enums) */
typedef char dx9_shader_descs_check[(DX9VS_Count == 3 && DX9PS_Count == 5) ? 1 : -1];

/* What it is compiled from, hashed as d3d_shader_cache.h does */
static u64 descHash(const DX9ShaderDesc *d)
{
	const D3D_SHADER_MACRO *m;
	u64 h = d3d_shcache_hash(D3D_SHCACHE_HASH_INIT, d->head, strlen(d->head));
	h = d3d_shcache_hash(h, d->body, strlen(d->body) + 1);
	h = d3d_shcache_hash(h, d->profile, strlen(d->profile) + 1);
	for (m = d->macros; m != NULL && m->Name != NULL; m++)
	{
		h = d3d_shcache_hash(h, m->Name, strlen(m->Name) + 1);
		h = d3d_shcache_hash(h, m->Definition, strlen(m->Definition) + 1);
	}
	return h;
}

/* The bytecode, compiled or the core's, by the enums' order: made once
 * for the life of the core, before any game runs */
typedef struct dx9_code
{
	DWORD *words;
	size_t size;
} dx9_code;

/* (the code's room, @size bytes, in words) */
static bool code_take(dx9_code *code, const void *bytes, size_t size)
{
	DWORD *words = (DWORD *)malloc((size + 3) / 4 * 4);
	if (words == NULL)
		return false;
	memcpy(words, bytes, size);
	free(code->words);
	code->words = words;
	code->size = size;
	return true;
}

static bool compileOne(pD3DCompile compile, const DX9ShaderDesc *d, dx9_code *code)
{
	size_t head, body;
	char *source;
	ID3DBlob *blob = NULL;
	ID3DBlob *errors = NULL;
	HRESULT hr;
	bool ok;
	if (compile == NULL)
		return false;
	head = strlen(d->head);
	body = strlen(d->body);
	source = (char *)malloc(head + body + 1);
	if (source == NULL)
		return false;
	memcpy(source, d->head, head);
	memcpy(source + head, d->body, body + 1);
	hr = compile(source, head + body, NULL, d->macros, NULL, "main", d->profile,
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
	free(source);
	if (FAILED(hr) || blob == NULL)
	{
		ERROR_LOG(RENDERER, "DX9: shader compilation failed (%x): %s\n", (unsigned)hr,
				errors != NULL ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "");
		DX9_SHADER_RELEASE(errors);
		DX9_SHADER_RELEASE(blob);
		return false;
	}
	ok = code_take(code, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob));
	DX9_SHADER_RELEASE(errors);
	DX9_SHADER_RELEASE(blob);
	return ok;
}

#ifndef DX9_DXBC_MAKE
/* The core's bytecode of them (dx9_dxbc.inc, tools/rend/kernels_dx9.sh) */
#include "dx9_dxbc.inc"

static const u8 *builtIn(u64 hash, u32 *size)
{
	size_t i;
	for (i = 0; i < dx9_dxbc_count; i++)
		if (dx9_dxbc_index[i].hash == hash)
		{
			*size = dx9_dxbc_index[i].size;
			return &dx9_dxbc_bytes[dx9_dxbc_index[i].offset];
		}
	return NULL;
}

static dx9_code codes[DX9_SHADERS];
static bool compiled;
static IDirect3DVertexShader9 *vs[DX9VS_Count];
static IDirect3DPixelShader9 *ps[DX9PS_Count];

static bool dx9_shaders_compile(pD3DCompile compile)
{
	u32 builtIns = 0;
	int i;
	if (compiled)
		return true;
	for (i = 0; i < DX9_SHADERS; i++)
	{
		u32 size = 0;
		const u8 *p = builtIn(descHash(&ShaderDescs[i]), &size);
		if (p != NULL ? code_take(&codes[i], p, size) : compileOne(compile, &ShaderDescs[i], &codes[i]))
		{
			if (p != NULL)
				builtIns++;
			continue;
		}
		for (i = 0; i < DX9_SHADERS; i++)
		{
			free(codes[i].words);
			codes[i].words = NULL;
			codes[i].size = 0;
		}
		return false;
	}
	NOTICE_LOG(RENDERER, "DX9: %u shaders the core's, %u compiled\n", builtIns, (u32)DX9_SHADERS - builtIns);
	compiled = true;
	return true;
}

bool dx9_shaders_init(IDirect3DDevice9 *device, pD3DCompile compile)
{
	int i;
	dx9_shaders_term();
	if (!dx9_shaders_compile(compile))
		return false;
	for (i = 0; i < DX9VS_Count; i++)
		if (FAILED(IDirect3DDevice9_CreateVertexShader(device, codes[i].words, &vs[i])))
		{
			vs[i] = NULL;
			ERROR_LOG(RENDERER, "DX9: vertex shader creation failed\n");
			dx9_shaders_term();
			return false;
		}
	for (i = 0; i < DX9PS_Count; i++)
		if (FAILED(IDirect3DDevice9_CreatePixelShader(device, codes[DX9VS_Count + i].words, &ps[i])))
		{
			ps[i] = NULL;
			ERROR_LOG(RENDERER, "DX9: pixel shader creation failed\n");
			dx9_shaders_term();
			return false;
		}
	return true;
}

void dx9_shaders_term(void)
{
	int i;
	for (i = 0; i < DX9VS_Count; i++)
		DX9_SHADER_RELEASE(vs[i]);
	for (i = 0; i < DX9PS_Count; i++)
		DX9_SHADER_RELEASE(ps[i]);
}

IDirect3DPixelShader9 *dx9_shaders_ps(int which)
{
	return ps[which];
}

IDirect3DVertexShader9 *dx9_shaders_vs(int which)
{
	return vs[which];
}
#endif
