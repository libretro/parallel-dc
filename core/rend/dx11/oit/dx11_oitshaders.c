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
/* The per-pixel renderer's shaders. What they do is what the Vulkan
 * per-pixel renderer's GLSL does (vulkan/oit/oit_shaders.cpp), written as
 * upstream's Direct3D 11 ones are: a list of the translucent pixels of
 * each screen pixel in a buffer, put together and blended in depth order
 * at the end. */
#include "dx11_oitshaders.h"
#include <string.h>
#include "../dx11_shaders_text.h"

#define MAX_PIXELS_PER_FRAGMENT "32"

#define TEXT_VS \
"\n" \
"#if pp_Gouraud == 1\n" \
"#define INTERPOLATION\n" \
"#else\n" \
"#define INTERPOLATION nointerpolation\n" \
"#endif\n" \
"\n" \
"struct VertexIn\n" \
"{\n" \
"\tfloat3 pos : POSITION;\n" \
"\tfloat4 col : COLOR0;\n" \
"\tfloat4 spec : COLOR1;\n" \
"\tfloat2 uv : TEXCOORD0;\n" \
"\tfloat4 col1 : COLOR2;\n" \
"\tfloat4 spec1 : COLOR3;\n" \
"\tfloat2 uv1 : TEXCOORD2;\n" \
"\t// the translucent polygon's number, for the lists: the draw's instance's\n" \
"\tuint polyNumber : POLYNUMBER;\n" \
"};\n" \
"\n" \
"struct VertexOut\n" \
"{\n" \
"\tprecise float4 pos : SV_POSITION;\n" \
"\tINTERPOLATION float4 col : COLOR0;\n" \
"\tINTERPOLATION float4 spec : COLOR1;\n" \
"\tfloat2 uv : TEXCOORD0;\n" \
"\tfloat clipW : TEXCOORD1;\n" \
"\tINTERPOLATION float4 col1 : COLOR2;\t\t// the second volume's, for opaque and punch-through polygons\n" \
"\tINTERPOLATION float4 spec1 : COLOR3;\n" \
"\tfloat2 uv1 : TEXCOORD2;\n" \
"\tnointerpolation uint polyNumber : POLYNUMBER;\n" \
"};\n" \
"\n" \
"VertexOut main(in VertexIn vin)\n" \
"{\n" \
"\tVertexOut vo;\n" \
"\tvo.pos = transformVertex(vin.pos, vo.clipW);\n" \
"\tvo.col = vin.col;\n" \
"\tvo.spec = vin.spec;\n" \
"\tvo.uv = vin.uv;\n" \
"\tvo.col1 = vin.col1;\n" \
"\tvo.spec1 = vin.spec1;\n" \
"\tvo.uv1 = vin.uv1;\n" \
"\tvo.polyNumber = vin.polyNumber;\n" \
"\treturn vo;\n" \
"}\n"

#define TEXT_HEADER \
"\n" \
"cbuffer FragmentConstants : register(b0)\n" \
"{\n" \
"\tfloat4 colorClampMin;\n" \
"\tfloat4 colorClampMax;\n" \
"\tfloat4 sp_FOG_COL_RAM;\n" \
"\tfloat4 sp_FOG_COL_VERT;\n" \
"\tfloat cp_AlphaTestValue;\n" \
"\tfloat sp_FOG_DENSITY;\n" \
"\tfloat shade_scale_factor;\n" \
"};\n" \
"\n" \
"Texture2D fog_table : register(t2);\n" \
"SamplerState pointSampler : register(s1);\n" \
"\n" \
"struct Pixel\n" \
"{\n" \
"\tuint color;\n" \
"\tfloat depth;\n" \
"\tuint seq_num;\n" \
"\tuint next;\n" \
"};\n" \
"#define EOL 0xFFFFFFFFu\n" \
"RWStructuredBuffer<Pixel> PixelBuffer : register(u1);\n" \
"RWTexture2D<uint> abufferPointers : register(u2);\n" \
"RWByteAddressBuffer PixelCounter : register(u3);\n" \
"\n" \
"cbuffer PixelBufferConstants : register(b2)\n" \
"{\n" \
"\tuint pixelBufferCount;\t// how many pixels the buffer has room for\n" \
"};\n" \
"\n" \
"#define ZERO\t\t\t\t0\n" \
"#define ONE\t\t\t\t\t1\n" \
"#define OTHER_COLOR\t\t\t2\n" \
"#define INVERSE_OTHER_COLOR\t3\n" \
"#define SRC_ALPHA\t\t\t4\n" \
"#define INVERSE_SRC_ALPHA\t5\n" \
"#define DST_ALPHA\t\t\t6\n" \
"#define INVERSE_DST_ALPHA\t7\n" \
"\n" \
"uint getNextPixelIndex()\n" \
"{\n" \
"\tuint index;\n" \
"\tPixelCounter.InterlockedAdd(0, 1, index);\n" \
"\tif (index >= pixelBufferCount)\n" \
"\t\t// Buffer overflow\n" \
"\t\tdiscard;\n" \
"\treturn index;\n" \
"}\n" \
"\n" \
"struct PolyParam\n" \
"{\n" \
"\tint tsp_isp_pcw;\n" \
"\tint tsp1;\n" \
"};\n" \
"/* (a pair of words a polygon, in a typed view: GPU-side TA processing's\n" \
" * kernels write them so, and the CPU the same way) */\n" \
"Buffer<uint2> TrPolyParamWords : register(t5);\n" \
"PolyParam getPolyParam(uint n)\n" \
"{\n" \
"\tconst uint2 w = TrPolyParamWords[n];\n" \
"\tPolyParam pp;\n" \
"\tpp.tsp_isp_pcw = int(w.x);\n" \
"\tpp.tsp1 = int(w.y);\n" \
"\treturn pp;\n" \
"}\n" \
"\n" \
"int getTsp(PolyParam pp, bool area1)\n" \
"{\n" \
"\treturn area1 \? pp.tsp1 : pp.tsp_isp_pcw;\n" \
"}\n" \
"\n" \
"int getSrcBlendFunc(PolyParam pp, bool area1)\n" \
"{\n" \
"\treturn (getTsp(pp, area1) >> 29) & 7;\n" \
"}\n" \
"\n" \
"int getDstBlendFunc(PolyParam pp, bool area1)\n" \
"{\n" \
"\treturn (getTsp(pp, area1) >> 26) & 7;\n" \
"}\n" \
"\n" \
"bool getSrcSelect(PolyParam pp, bool area1)\n" \
"{\n" \
"\treturn ((getTsp(pp, area1) >> 25) & 1) != 0;\n" \
"}\n" \
"\n" \
"bool getDstSelect(PolyParam pp, bool area1)\n" \
"{\n" \
"\treturn ((getTsp(pp, area1) >> 24) & 1) != 0;\n" \
"}\n" \
"\n" \
"bool getShadowEnable(PolyParam pp)\n" \
"{\n" \
"\treturn (pp.tsp_isp_pcw & 1) != 0;\n" \
"}\n" \
"\n" \
"uint getPolyNumber(Pixel pixel)\n" \
"{\n" \
"\treturn pixel.seq_num & 0x3FFFFFFFu;\n" \
"}\n" \
"\n" \
"#define SHADOW_STENCIL 0x40000000u\n" \
"#define SHADOW_ACC\t   0x80000000u\n" \
"\n" \
"bool isShadowed(Pixel pixel)\n" \
"{\n" \
"\treturn (pixel.seq_num & SHADOW_ACC) == SHADOW_ACC;\n" \
"}\n" \
"\n" \
"bool isTwoVolumes(PolyParam pp)\n" \
"{\n" \
"\treturn pp.tsp1 != -1;\n" \
"}\n" \
"\n" \
"uint packColors(float4 v)\n" \
"{\n" \
"\treturn (uint(round(v.r * 255.0)) << 24) | (uint(round(v.g * 255.0)) << 16) | (uint(round(v.b * 255.0)) << 8) | uint(round(v.a * 255.0));\n" \
"}\n" \
"\n" \
"float4 unpackColors(uint u)\n" \
"{\n" \
"\treturn float4(float((u >> 24) & 255u) / 255.0, float((u >> 16) & 255u) / 255.0, float((u >> 8) & 255u) / 255.0, float(u & 255u) / 255.0);\n" \
"}\n"

#define TEXT_PS \
"\n" \
"#define PI 3.1415926\n" \
"\n" \
"#define PASS_DEPTH 0\n" \
"#define PASS_COLOR 1\n" \
"#define PASS_OIT 2\n" \
"\n" \
"#if pp_Gouraud == 0\n" \
"#define INTERPOLATION nointerpolation\n" \
"#else\n" \
"#define INTERPOLATION\n" \
"#endif\n" \
"\n" \
"cbuffer PolyConstants : register(b1)\n" \
"{\n" \
"\tfloat4 clipTest;\n" \
"\tint4 blend_mode0;\n" \
"\tfloat trilinearAlpha;\n" \
"\tint pp_Number;\n" \
"\tfloat palette_index;\n" \
"\tuint polyKey;\t// the general shader's: FragmentShaderParams::hash()\n" \
"\n" \
"\t// two volume mode\n" \
"\tint4 blend_mode1;\n" \
"\tint shading_instr0;\n" \
"\tint shading_instr1;\n" \
"\tint fog_control0;\n" \
"\tint fog_control1;\n" \
"\tint use_alpha0;\n" \
"\tint use_alpha1;\n" \
"\tint ignore_tex_alpha0;\n" \
"\tint ignore_tex_alpha1;\n" \
"};\n" \
"\n" \
"// A polygon's own shader has its key's bits as numbers, and the branches\n" \
"// they decide are compiled away; the general shader (GENERAL) reads them\n" \
"// from the polygon's constants, and draws what its own shader would while\n" \
"// that is made: the same code, the same pixels. The pass, the two volumes\n" \
"// and the interpolation decide what the shader takes and gives: those are\n" \
"// the general shader's own.\n" \
"#ifdef GENERAL\n" \
"#define cp_AlphaTest (polyKey & 1u)\n" \
"#define pp_ClipInside ((polyKey >> 1) & 1u)\n" \
"#define pp_UseAlpha ((polyKey >> 2) & 1u)\n" \
"#define pp_Texture ((polyKey >> 3) & 1u)\n" \
"#define pp_IgnoreTexA ((polyKey >> 4) & 1u)\n" \
"#define pp_ShadInstr ((polyKey >> 5) & 3u)\n" \
"#define pp_Offset ((polyKey >> 7) & 1u)\n" \
"#define pp_FogCtrl ((polyKey >> 8) & 3u)\n" \
"#define pp_BumpMap ((polyKey >> 12) & 1u)\n" \
"#define ColorClamping ((polyKey >> 13) & 1u)\n" \
"#define pp_Palette ((polyKey >> 14) & 1u)\n" \
"#endif\n" \
"// (with two volumes, the area's own instructions decide)\n" \
"#if pp_TwoVolumes == 1\n" \
"#define SHAD(k) (cur_shading_instr == k)\n" \
"#define FOG(k) (cur_fog_control == k)\n" \
"#else\n" \
"#define SHAD(k) (pp_ShadInstr == k)\n" \
"#define FOG(k) (pp_FogCtrl == k)\n" \
"#endif\n" \
"\n" \
"Texture2D tex0 : register(t0);\n" \
"SamplerState sampler0 : register(s0);\n" \
"Texture2D palette : register(t1);\n" \
"Texture2D tex1 : register(t3);\n" \
"SamplerState sampler1 : register(s3);\n" \
"#if PASS == PASS_COLOR\n" \
"Texture2D<uint2> shadow_stencil : register(t4);\n" \
"#endif\n" \
"#if PASS == PASS_OIT\n" \
"Texture2D<float> DepthTex : register(t4);\n" \
"#endif\n" \
"\n" \
"float4 colorClamp(float4 col)\n" \
"{\n" \
"\t// TODO This can change in two-volume mode\n" \
"\tif (ColorClamping == 1)\n" \
"\t\treturn clamp(col, colorClampMin, colorClampMax);\n" \
"\treturn col;\n" \
"}\n" \
"\n" \
"float4 palettePixel(float2 coords)\n" \
"{\n" \
"\t// palette_index is where the middle of the palette's first colour is in the row of 1024\n" \
"\tfloat2 c = float2(tex0.Sample(sampler0, coords).r * (255.0 / 1024.0) + palette_index, 0.5);\n" \
"\treturn palette.SampleLevel(pointSampler, c, 0);\n" \
"}\n" \
"\n" \
"struct VertexOut\n" \
"{\n" \
"\tfloat4 pos : SV_POSITION;\n" \
"\tINTERPOLATION float4 col : COLOR0;\n" \
"\tINTERPOLATION float4 spec : COLOR1;\n" \
"\tfloat2 uv : TEXCOORD0;\n" \
"\tfloat clipW : TEXCOORD1;\n" \
"\tINTERPOLATION float4 col1 : COLOR2;\n" \
"\tINTERPOLATION float4 spec1 : COLOR3;\n" \
"\tfloat2 uv1 : TEXCOORD2;\n" \
"\tnointerpolation uint polyNumber : POLYNUMBER;\n" \
"};\n" \
"\n" \
"// (the depth is the position's, which the depth test took: no SV_Depth)\n" \
"struct PSO\n" \
"{\n" \
"\tfloat4 col : SV_Target;\n" \
"};\n" \
"\n" \
"PSO main(in VertexOut inpix)\n" \
"{\n" \
"\tconst float w = 1.0 / inpix.clipW;\n" \
"\tPSO pso;\n" \
"\n" \
"\t#if PASS == PASS_OIT\n" \
"\t\t// Manual depth testing\n" \
"\t\tfloat frontDepth = DepthTex[uint2(inpix.pos.xy)];\n" \
"\t\tif (inpix.pos.z < frontDepth)\n" \
"\t\t\tdiscard;\n" \
"\t#endif\n" \
"\n" \
"\t// Clip inside the box\n" \
"\tif (pp_ClipInside == 1)\n" \
"\t{\n" \
"\t\tif (inpix.pos.x >= clipTest.x && inpix.pos.x <= clipTest.z\n" \
"\t\t\t\t&& inpix.pos.y >= clipTest.y && inpix.pos.y <= clipTest.w)\n" \
"\t\t\tdiscard;\n" \
"\t}\n" \
"\n" \
"\tfloat4 color = inpix.col;\n" \
"\tfloat4 offset = inpix.spec;\n" \
"\tfloat2 uv = inpix.uv;\n" \
"\tbool area1 = false;\n" \
"\tint2 cur_blend_mode = blend_mode0.xy;\n" \
"\n" \
"\t#if pp_TwoVolumes == 1\n" \
"\t\tbool cur_use_alpha = use_alpha0 != 0;\n" \
"\t\tbool cur_ignore_tex_alpha = ignore_tex_alpha0 != 0;\n" \
"\t\tint cur_shading_instr = shading_instr0;\n" \
"\t\tint cur_fog_control = fog_control0;\n" \
"\t\t#if PASS == PASS_COLOR\n" \
"\t\t\tuint stencil = shadow_stencil[uint2(inpix.pos.xy)].g;\n" \
"\t\t\tif (stencil == 0x81u) {\n" \
"\t\t\t\tcolor = inpix.col1;\n" \
"\t\t\t\toffset = inpix.spec1;\n" \
"\t\t\t\tuv = inpix.uv1;\n" \
"\t\t\t\tarea1 = true;\n" \
"\t\t\t\tcur_blend_mode = blend_mode1.xy;\n" \
"\t\t\t\tcur_use_alpha = use_alpha1 != 0;\n" \
"\t\t\t\tcur_ignore_tex_alpha = ignore_tex_alpha1 != 0;\n" \
"\t\t\t\tcur_shading_instr = shading_instr1;\n" \
"\t\t\t\tcur_fog_control = fog_control1;\n" \
"\t\t\t}\n" \
"\t\t#endif\n" \
"\t#endif\n" \
"\n" \
"\t#if PASS == PASS_COLOR && pp_TwoVolumes == 0\n" \
"\t\t// Inside a modifier volume the PowerVR2 scales what the polygon is shaded\n" \
"\t\t// with, its base and offset colours, before the texture is combined with\n" \
"\t\t// them. Scaling the combined colour instead is the same thing for a\n" \
"\t\t// modulated texture, but also darkens a decal texture, which the\n" \
"\t\t// hardware leaves alone.\n" \
"\t\tuint stencil = shadow_stencil[uint2(inpix.pos.xy)].g;\n" \
"\t\tif (stencil == 0x81u) {\n" \
"\t\t\tcolor.rgb *= shade_scale_factor;\n" \
"\t\t\toffset.rgb *= shade_scale_factor;\n" \
"\t\t}\n" \
"\t#endif\n" \
"\t#if pp_TwoVolumes == 1\n" \
"\t\tif (!cur_use_alpha)\n" \
"\t\t\tcolor.a = 1.0;\n" \
"\t#else\n" \
"\t\tif (pp_UseAlpha == 0)\n" \
"\t\t\tcolor.a = 1.0;\n" \
"\t#endif\n" \
"\tif (pp_Texture == 1)\n" \
"\t{\n" \
"\t\tfloat4 texcol;\n" \
"\t\t#if pp_TwoVolumes == 1\n" \
"\t\t\t// only the first volume's texture is ever one of palette indices\n" \
"\t\t\tif (area1)\n" \
"\t\t\t\ttexcol = tex1.Sample(sampler1, uv);\n" \
"\t\t\telse\n" \
"\t\t#endif\n" \
"\t\tif (pp_Palette == 0)\n" \
"\t\t\ttexcol = tex0.Sample(sampler0, uv);\n" \
"\t\telse\n" \
"\t\t\ttexcol = palettePixel(uv);\n" \
"\t\tif (pp_BumpMap == 1)\n" \
"\t\t{\n" \
"\t\t\tfloat s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;\n" \
"\t\t\tfloat r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;\n" \
"\t\t\ttexcol.a = clamp(inpix.spec.a + inpix.spec.r * sin(s) + inpix.spec.g * cos(s) * cos(r - 2.0 * PI * inpix.spec.b), 0.0, 1.0);\n" \
"\t\t\ttexcol.rgb = float3(1.0, 1.0, 1.0);\n" \
"\t\t}\n" \
"\t\telse\n" \
"\t\t{\n" \
"\t\t#if pp_TwoVolumes == 1\n" \
"\t\t\tif (cur_ignore_tex_alpha)\n" \
"\t\t\t\ttexcol.a = 1.0;\n" \
"\t\t#else\n" \
"\t\t\tif (pp_IgnoreTexA == 1)\n" \
"\t\t\t\ttexcol.a = 1.0;\n" \
"\t\t#endif\n" \
"\t\t}\n" \
"\t\tif (SHAD(0)) // DECAL\n" \
"\t\t{\n" \
"\t\t\tcolor = texcol;\n" \
"\t\t}\n" \
"\t\tif (SHAD(1)) // MODULATE\n" \
"\t\t{\n" \
"\t\t\tcolor.rgb *= texcol.rgb;\n" \
"\t\t\tcolor.a = texcol.a;\n" \
"\t\t}\n" \
"\t\tif (SHAD(2)) // DECAL ALPHA\n" \
"\t\t{\n" \
"\t\t\tcolor.rgb = lerp(color.rgb, texcol.rgb, texcol.a);\n" \
"\t\t}\n" \
"\t\tif (SHAD(3)) // MODULATE ALPHA\n" \
"\t\t{\n" \
"\t\t\tcolor *= texcol;\n" \
"\t\t}\n" \
"\n" \
"\t\tif (pp_Offset == 1 && pp_BumpMap == 0)\n" \
"\t\t\tcolor.rgb += offset.rgb;\n" \
"\t}\n" \
"\n" \
"\tcolor = colorClamp(color);\n" \
"\n" \
"\t// Look-up table mode 2: the fog colour takes the place of the pixel's\n" \
"\t// colour and the fog coefficient the place of its alpha, after the\n" \
"\t// texture has been combined in.\n" \
"\tif (FOG(3)) // LUT Mode 2\n" \
"\t\tcolor = float4(sp_FOG_COL_RAM.rgb, fog_mode2(w) * (256.0 / 255.0));\n" \
"\tif (FOG(0)) // LUT\n" \
"\t{\n" \
"\t\tfloat fog = fog_mode2(w);\n" \
"\t\tcolor.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;\n" \
"\t}\n" \
"\tif (pp_Offset == 1 && pp_BumpMap == 0 && FOG(1))  // Per vertex\n" \
"\t{\n" \
"\t\tfloat fog = offset.a * (255.0 / 256.0);\n" \
"\t\tcolor.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;\n" \
"\t}\n" \
"\n" \
"\tcolor *= trilinearAlpha;\n" \
"\n" \
"\t// Punch-through polygons are tested on the alpha of the finished colour,\n" \
"\t// after the texture and the vertex colours are combined and clamped.\n" \
"\tif (cp_AlphaTest == 1)\n" \
"\t{\n" \
"\t\tif (cp_AlphaTestValue > color.a)\n" \
"\t\t\tdiscard;\n" \
"\t\tcolor.a = 1.0;\n" \
"\t}\n" \
"\n" \
"\t#if PASS == PASS_OIT\n" \
"\t\t// Discard as many pixels as possible\n" \
"\t\tswitch (cur_blend_mode.y) // DST\n" \
"\t\t{\n" \
"\t\tcase ONE:\n" \
"\t\t\tswitch (cur_blend_mode.x) // SRC\n" \
"\t\t\t{\n" \
"\t\t\t\tcase ZERO:\n" \
"\t\t\t\t\tdiscard;\n" \
"\t\t\t\t\tbreak;\n" \
"\t\t\t\tcase ONE:\n" \
"\t\t\t\tcase OTHER_COLOR:\n" \
"\t\t\t\tcase INVERSE_OTHER_COLOR:\n" \
"\t\t\t\t\tif (all(color == 0.0))\n" \
"\t\t\t\t\t\tdiscard;\n" \
"\t\t\t\t\tbreak;\n" \
"\t\t\t\tcase SRC_ALPHA:\n" \
"\t\t\t\t\tif (color.a == 0.0 || all(color.rgb == 0.0))\n" \
"\t\t\t\t\t\tdiscard;\n" \
"\t\t\t\t\tbreak;\n" \
"\t\t\t\tcase INVERSE_SRC_ALPHA:\n" \
"\t\t\t\t\tif (color.a == 1.0 || all(color.rgb == 0.0))\n" \
"\t\t\t\t\t\tdiscard;\n" \
"\t\t\t\t\tbreak;\n" \
"\t\t\t}\n" \
"\t\t\tbreak;\n" \
"\t\t// Destination times a white source is not left alone when the\n" \
"\t\t// fragment turns out to be in a shadow: its colour is scaled then.\n" \
"\t\tcase INVERSE_OTHER_COLOR:\n" \
"\t\t\tif (cur_blend_mode.x <= SRC_ALPHA && all(color == 0.0))\n" \
"\t\t\t\tdiscard;\n" \
"\t\t\tbreak;\n" \
"\t\tcase SRC_ALPHA:\n" \
"\t\t\tif ((cur_blend_mode.x == ZERO || cur_blend_mode.x == INVERSE_SRC_ALPHA) && color.a == 1.0)\n" \
"\t\t\t\tdiscard;\n" \
"\t\t\tbreak;\n" \
"\t\tcase INVERSE_SRC_ALPHA:\n" \
"\t\t\tswitch (cur_blend_mode.x) // SRC\n" \
"\t\t\t{\n" \
"\t\t\t\tcase ZERO:\n" \
"\t\t\t\tcase SRC_ALPHA:\n" \
"\t\t\t\t\tif (color.a == 0.0)\n" \
"\t\t\t\t\t\tdiscard;\n" \
"\t\t\t\t\tbreak;\n" \
"\t\t\t\tcase ONE:\n" \
"\t\t\t\tcase OTHER_COLOR:\n" \
"\t\t\t\tcase INVERSE_OTHER_COLOR:\n" \
"\t\t\t\t\tif (all(color == 0.0))\n" \
"\t\t\t\t\t\tdiscard;\n" \
"\t\t\t\t\tbreak;\n" \
"\t\t\t}\n" \
"\t\t\tbreak;\n" \
"\t\t}\n" \
"\n" \
"\t\tuint2 coords = uint2(inpix.pos.xy);\n" \
"\t\tuint idx = getNextPixelIndex();\n" \
"\n" \
"\t\tPixel pixel;\n" \
"\t\tpixel.color = packColors(clamp(color, 0.0, 1.0));\n" \
"\t\tpixel.depth = inpix.pos.z;\n" \
"\t\tpixel.seq_num = inpix.polyNumber;\n" \
"\t\tuint next;\n" \
"\t\tInterlockedExchange(abufferPointers[coords], idx, next);\n" \
"\t\tpixel.next = next;\n" \
"\t\tPixelBuffer[idx] = pixel;\n" \
"\t\tpso.col = 0.0;\n" \
"\t#else\n" \
"\t\tpso.col = color;\n" \
"\t#endif\n" \
"\treturn pso;\n" \
"}\n"

#define TEXT_MODVOL_PS \
"\n" \
"struct ModVolPixel\n" \
"{\n" \
"\tfloat4 pos : SV_POSITION;\n" \
"\tfloat clipW : TEXCOORD1;\n" \
"};\n" \
"\n" \
"// (the depth is the position's; nothing is shaded)\n" \
"void main(in ModVolPixel inpix)\n" \
"{\n" \
"}\n"

#define TEXT_FINAL_PS \
"\n" \
"Texture2D<float4> opaqueTex : register(t0);\n" \
"\n" \
"static uint pixel_list[MAX_PIXELS_PER_FRAGMENT];\n" \
"\n" \
"int fillAndSortFragmentArray(uint2 coords)\n" \
"{\n" \
"\t// Load fragments into a local memory array for sorting\n" \
"\tuint idx = abufferPointers[coords];\n" \
"\tint count = 0;\n" \
"\tfor (; idx != EOL && count < MAX_PIXELS_PER_FRAGMENT; count++)\n" \
"\t{\n" \
"\t\tconst Pixel p = PixelBuffer[idx];\n" \
"\t\t// Insertion sort. The entry before the first one is not looked at.\n" \
"\t\tint j = count - 1;\n" \
"\t\twhile (j >= 0)\n" \
"\t\t{\n" \
"\t\t\tconst Pixel jp = PixelBuffer[pixel_list[j]];\n" \
"\t\t\tif (!(jp.depth > p.depth\n" \
"\t\t\t\t\t|| (jp.depth == p.depth && getPolyNumber(jp) > getPolyNumber(p))))\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tpixel_list[j + 1] = pixel_list[j];\n" \
"\t\t\tj--;\n" \
"\t\t}\n" \
"\t\tpixel_list[j + 1] = idx;\n" \
"\t\tidx = p.next;\n" \
"\t}\n" \
"\treturn count;\n" \
"}\n" \
"\n" \
"// Blend fragments back-to-front\n" \
"float4 resolveAlphaBlend(uint2 coords)\n" \
"{\n" \
"\t// Copy and sort fragments into a local array\n" \
"\tint num_frag = fillAndSortFragmentArray(coords);\n" \
"\n" \
"\tfloat4 finalColor = opaqueTex.Load(int3(coords, 0));\n" \
"\tfloat4 secondaryBuffer = 0.0; // Secondary accumulation buffer\n" \
"\n" \
"\tfor (int i = 0; i < num_frag; i++)\n" \
"\t{\n" \
"\t\tconst Pixel pixel = PixelBuffer[pixel_list[i]];\n" \
"\t\tconst PolyParam pp = getPolyParam(getPolyNumber(pixel));\n" \
"\t\tbool area1 = false;\n" \
"\t\tbool shadowed = false;\n" \
"\t\tif (isShadowed(pixel))\n" \
"\t\t{\n" \
"\t\t\tif (isTwoVolumes(pp))\n" \
"\t\t\t\tarea1 = true;\n" \
"\t\t\telse\n" \
"\t\t\t\tshadowed = true;\n" \
"\t\t}\n" \
"\t\tfloat4 srcColor;\n" \
"\t\tif (getSrcSelect(pp, area1))\n" \
"\t\t\tsrcColor = secondaryBuffer;\n" \
"\t\telse\n" \
"\t\t{\n" \
"\t\t\tsrcColor = unpackColors(pixel.color);\n" \
"\t\t\tif (shadowed)\n" \
"\t\t\t\tsrcColor.rgb *= shade_scale_factor;\n" \
"\t\t}\n" \
"\t\tfloat4 dstColor = getDstSelect(pp, area1) \? secondaryBuffer : finalColor;\n" \
"\t\tfloat4 srcCoef = 0.0;\n" \
"\t\tfloat4 dstCoef = 0.0;\n" \
"\n" \
"\t\tint srcBlend = getSrcBlendFunc(pp, area1);\n" \
"\t\tswitch (srcBlend)\n" \
"\t\t{\n" \
"\t\t\tcase ZERO:\n" \
"\t\t\t\tsrcCoef = 0.0;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase ONE:\n" \
"\t\t\t\tsrcCoef = 1.0;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase OTHER_COLOR:\n" \
"\t\t\t\t// the colour of what it is blended onto - which, when that is the second accumulation buffer, is that buffer's\n" \
"\t\t\t\tsrcCoef = dstColor;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase INVERSE_OTHER_COLOR:\n" \
"\t\t\t\tsrcCoef = 1.0 - dstColor;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase SRC_ALPHA:\n" \
"\t\t\t\tsrcCoef = srcColor.a;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase INVERSE_SRC_ALPHA:\n" \
"\t\t\t\tsrcCoef = 1.0 - srcColor.a;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase DST_ALPHA:\n" \
"\t\t\t\tsrcCoef = dstColor.a;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase INVERSE_DST_ALPHA:\n" \
"\t\t\t\tsrcCoef = 1.0 - dstColor.a;\n" \
"\t\t\t\tbreak;\n" \
"\t\t}\n" \
"\t\tint dstBlend = getDstBlendFunc(pp, area1);\n" \
"\t\tswitch (dstBlend)\n" \
"\t\t{\n" \
"\t\t\tcase ZERO:\n" \
"\t\t\t\tdstCoef = 0.0;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase ONE:\n" \
"\t\t\t\tdstCoef = 1.0;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase OTHER_COLOR:\n" \
"\t\t\t\tdstCoef = srcColor;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase INVERSE_OTHER_COLOR:\n" \
"\t\t\t\tdstCoef = 1.0 - srcColor;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase SRC_ALPHA:\n" \
"\t\t\t\tdstCoef = srcColor.a;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase INVERSE_SRC_ALPHA:\n" \
"\t\t\t\tdstCoef = 1.0 - srcColor.a;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase DST_ALPHA:\n" \
"\t\t\t\tdstCoef = dstColor.a;\n" \
"\t\t\t\tbreak;\n" \
"\t\t\tcase INVERSE_DST_ALPHA:\n" \
"\t\t\t\tdstCoef = 1.0 - dstColor.a;\n" \
"\t\t\t\tbreak;\n" \
"\t\t}\n" \
"\t\tconst float4 result = clamp(dstColor * dstCoef + srcColor * srcCoef, 0.0, 1.0);\n" \
"\t\tif (getDstSelect(pp, area1))\n" \
"\t\t\tsecondaryBuffer = result;\n" \
"\t\telse\n" \
"\t\t\tfinalColor = result;\n" \
"\t}\n" \
"\n" \
"\treturn finalColor;\n" \
"}\n" \
"\n" \
"float4 main(float4 pos : SV_Position) : SV_Target\n" \
"{\n" \
"\tuint2 coords = uint2(pos.xy);\n" \
"\t// Compute and output final color for the frame buffer\n" \
"\tfloat4 color = resolveAlphaBlend(coords);\n" \
"\t// The lists start empty for the next pass, or the next frame\n" \
"\tabufferPointers[coords] = EOL;\n" \
"\treturn color;\n" \
"}\n"

#define TEXT_FINAL_VS \
"\n" \
"float4 main(uint vertexId : SV_VertexID) : SV_Position\n" \
"{\n" \
"\t// the whole of the viewport, as a strip of two triangles\n" \
"\treturn float4((vertexId & 2) != 0 \? 1.0 : -1.0, (vertexId & 1) != 0 \? 1.0 : -1.0, 0.0, 1.0);\n" \
"}\n"

#define TEXT_TR_MODVOL_PS \
"\n" \
"// Must match ModVolMode\n" \
"#define MV_XOR\t\t 0\n" \
"#define MV_OR\t\t 1\n" \
"#define MV_INCLUSION 2\n" \
"#define MV_EXCLUSION 3\n" \
"\n" \
"struct ModVolPixel\n" \
"{\n" \
"\tfloat4 pos : SV_POSITION;\n" \
"\tfloat clipW : TEXCOORD1;\n" \
"};\n" \
"\n" \
"void main(in ModVolPixel inpix)\n" \
"{\n" \
"#if MV_MODE == MV_XOR || MV_MODE == MV_OR\n" \
"\tconst float depth = inpix.pos.z;\t// the position's, which the depth test took\n" \
"#endif\n" \
"\tuint2 coords = uint2(inpix.pos.xy);\n" \
"\n" \
"\tuint idx = abufferPointers[coords];\n" \
"\tint list_len = 0;\n" \
"\twhile (idx != EOL && list_len < MAX_PIXELS_PER_FRAGMENT)\n" \
"\t{\n" \
"\t\tconst Pixel pixel = PixelBuffer[idx];\n" \
"\t\tconst PolyParam pp = getPolyParam(getPolyNumber(pixel));\n" \
"\t\tif (getShadowEnable(pp))\n" \
"\t\t{\n" \
"\t\t\tuint prev_val;\n" \
"#if MV_MODE == MV_XOR\n" \
"\t\t\tif (depth >= pixel.depth)\n" \
"\t\t\t\tInterlockedXor(PixelBuffer[idx].seq_num, SHADOW_STENCIL, prev_val);\n" \
"#elif MV_MODE == MV_OR\n" \
"\t\t\tif (depth >= pixel.depth)\n" \
"\t\t\t\tInterlockedOr(PixelBuffer[idx].seq_num, SHADOW_STENCIL, prev_val);\n" \
"#elif MV_MODE == MV_INCLUSION\n" \
"\t\t\tInterlockedAnd(PixelBuffer[idx].seq_num, ~(SHADOW_STENCIL), prev_val);\n" \
"\t\t\t// (the word as it was read above, with the shadow bit set: as the other per-pixel renderers do)\n" \
"\t\t\tif ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_STENCIL)\n" \
"\t\t\t\tPixelBuffer[idx].seq_num = pixel.seq_num | SHADOW_ACC;\n" \
"#elif MV_MODE == MV_EXCLUSION\n" \
"\t\t\tInterlockedAnd(PixelBuffer[idx].seq_num, ~(SHADOW_STENCIL|SHADOW_ACC), prev_val);\n" \
"\t\t\tif ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_ACC)\n" \
"\t\t\t\tPixelBuffer[idx].seq_num = pixel.seq_num | SHADOW_ACC;\n" \
"#endif\n" \
"\t\t}\n" \
"\t\tidx = pixel.next;\n" \
"\t\tlist_len++;\n" \
"\t}\n" \
"}\n"

/* (the fog table and the depth, from the other renderer's) */
#define PIXEL(text) "#define MAX_PIXELS_PER_FRAGMENT " MAX_PIXELS_PER_FRAGMENT "\n" TEXT_HEADER DX11_TEXT_FOG text

static const char src_vs[] = DX11_TEXT_TRANSFORM TEXT_VS;
static const char src_ps[] = PIXEL(TEXT_PS);
static const char src_modvol_vs[] = DX11_TEXT_TRANSFORM DX11_TEXT_MODVOL_VS;
static const char src_modvol_ps[] = PIXEL(TEXT_MODVOL_PS);
static const char src_tr_modvol_ps[] = PIXEL(TEXT_TR_MODVOL_PS);
static const char src_final_vs[] = TEXT_FINAL_VS;
static const char src_final_ps[] = PIXEL(TEXT_FINAL_PS);

static const char * const MacroValues[] = { "0", "1", "2", "3" };

#define SOURCE(d, s) ((d)->source = (s), (d)->length = sizeof(s) - 1)
#define MACRO(m, n, v) ((m)->Name = (n), (m)->Definition = (v), (m)++)

static bool oit_describe(u32 family, u32 variant, dx11_shader_desc *d)
{
	D3D_SHADER_MACRO *m = d->macros;
	switch (family)
	{
	case DX11OITF_VS:
		SOURCE(d, src_vs);
		d->profile = "vs_5_0";
		MACRO(m, "pp_Gouraud", MacroValues[variant & 1]);
		break;
	case DX11OITF_PS:
		/* (the variant is dx11_oit_shaders_key()) */
		SOURCE(d, src_ps);
		d->profile = "ps_5_0";
		MACRO(m, "cp_AlphaTest", MacroValues[variant & 1]);
		MACRO(m, "pp_ClipInside", MacroValues[(variant >> 1) & 1]);
		MACRO(m, "pp_UseAlpha", MacroValues[(variant >> 2) & 1]);
		MACRO(m, "pp_Texture", MacroValues[(variant >> 3) & 1]);
		MACRO(m, "pp_IgnoreTexA", MacroValues[(variant >> 4) & 1]);
		MACRO(m, "pp_ShadInstr", MacroValues[(variant >> 5) & 3]);
		MACRO(m, "pp_Offset", MacroValues[(variant >> 7) & 1]);
		MACRO(m, "pp_FogCtrl", MacroValues[(variant >> 8) & 3]);
		MACRO(m, "pp_TwoVolumes", MacroValues[(variant >> 10) & 1]);
		MACRO(m, "pp_Gouraud", MacroValues[(variant >> 11) & 1]);
		MACRO(m, "pp_BumpMap", MacroValues[(variant >> 12) & 1]);
		MACRO(m, "ColorClamping", MacroValues[(variant >> 13) & 1]);
		MACRO(m, "pp_Palette", MacroValues[(variant >> 14) & 1]);
		MACRO(m, "PASS", MacroValues[(variant >> 15) & 3]);
		break;
	case DX11OITF_GENERAL_PS:
		/* (the variant: the pass, the interpolation and the two volumes) */
		SOURCE(d, src_ps);
		d->profile = "ps_5_0";
		MACRO(m, "GENERAL", "1");
		MACRO(m, "PASS", MacroValues[variant & 3]);
		MACRO(m, "pp_Gouraud", MacroValues[(variant >> 2) & 1]);
		MACRO(m, "pp_TwoVolumes", MacroValues[(variant >> 3) & 1]);
		break;
	case DX11OITF_MODVOL_VS:
		SOURCE(d, src_modvol_vs);
		d->profile = "vs_5_0";
		break;
	case DX11OITF_MODVOL_PS:
		SOURCE(d, src_modvol_ps);
		d->profile = "ps_5_0";
		break;
	case DX11OITF_TR_MODVOL_PS:
		SOURCE(d, src_tr_modvol_ps);
		d->profile = "ps_5_0";
		MACRO(m, "MV_MODE", MacroValues[variant & 3]);
		break;
	case DX11OITF_FINAL_VS:
		SOURCE(d, src_final_vs);
		d->profile = "vs_5_0";
		break;
	case DX11OITF_FINAL_PS:
		SOURCE(d, src_final_ps);
		d->profile = "ps_5_0";
		break;
	default:
		return false;
	}
	m->Name = NULL;
	m->Definition = NULL;
	return true;
}

/* by dx11_oit_shaders_key(), all of them: nothing allocated in play */
#define OIT_PIXEL_KEYS (1u << 17)
/* by the pass, the interpolation (<< 2) and the two volumes (<< 3) */
#define OIT_GENERAL_COUNT 16

static struct
{
	dx11_shader_compiler c;
	ID3D11VertexShader *vs[2];
	ID3DBlob *vs_blob;
	ID3D11PixelShader *general[OIT_GENERAL_COUNT];
	ID3D11VertexShader *modvol_vs;
	ID3DBlob *modvol_vs_blob;
	ID3D11PixelShader *modvol_ps;
	ID3D11PixelShader *tr_modvol_ps[4];
	ID3D11VertexShader *final_vs;
	ID3D11PixelShader *final_ps;
} oit;

static void oit_adopt(u32 family, u32 variant, ID3D11PixelShader *shader)
{
	ID3D11PixelShader **slot = NULL;
	switch (family)
	{
	case DX11OITF_PS:
		if (variant < oit.c.pixel_keys)
			slot = &oit.c.pixel[variant];
		break;
	case DX11OITF_MODVOL_PS:
		slot = &oit.modvol_ps;
		break;
	case DX11OITF_TR_MODVOL_PS:
		slot = &oit.tr_modvol_ps[variant & 3];
		break;
	case DX11OITF_FINAL_PS:
		slot = &oit.final_ps;
		break;
	}
	if (slot != NULL && *slot == NULL)
		*slot = shader;
	else
		ID3D11PixelShader_Release(shader);
}

static bool oit_pixel_family(u32 family)
{
	return family == DX11OITF_PS || family == DX11OITF_MODVOL_PS || family == DX11OITF_TR_MODVOL_PS
			|| family == DX11OITF_FINAL_PS;
}

static const u32 oit_fixed[] = {
	DX11OITF_FINAL_PS << 16, DX11OITF_MODVOL_PS << 16,
	DX11OITF_TR_MODVOL_PS << 16, (DX11OITF_TR_MODVOL_PS << 16) | 1, (DX11OITF_TR_MODVOL_PS << 16) | 2,
	(DX11OITF_TR_MODVOL_PS << 16) | 3
};

static const dx11_shader_families oit_families = {
	"d3d11_oit_shaders.bin", oit_describe, oit_adopt, oit_pixel_family, oit_fixed, 6
};

u32 dx11_oit_shaders_key(const dx11_oit_ps_params *p)
{
	return (u32)p->alphaTest | ((u32)p->insideClipTest << 1) | ((u32)p->useAlpha << 2)
		| ((u32)p->texture << 3) | ((u32)p->ignoreTexAlpha << 4) | ((u32)p->shaderInstr << 5)
		| ((u32)p->offset << 7) | ((u32)p->fog << 8) | ((u32)p->twoVolume << 10) | ((u32)p->gouraud << 11)
		| ((u32)p->bumpmap << 12) | ((u32)p->clamping << 13) | ((u32)p->palette << 14) | ((u32)p->pass << 15);
}

void dx11_oit_shaders_init(ID3D11Device *device, pD3DCompile compile)
{
	dx11_compiler_init(&oit.c, &oit_families, device, compile);
}

ID3D11VertexShader *dx11_oit_shaders_vs(bool gouraud)
{
	ID3D11VertexShader **shader = &oit.vs[gouraud ? 1 : 0];
	if (*shader == NULL)
	{
		ID3DBlob *blob = NULL;
		dx11_compiler_make_vs(&oit.c, DX11OITF_VS, gouraud ? 1 : 0, shader, &blob);
		if (oit.vs_blob == NULL)
			oit.vs_blob = blob;
		else if (blob != NULL)
			ID3D10Blob_Release(blob);
	}
	return *shader;
}

bool dx11_oit_shaders_start(void)
{
	u32 g, v, mode;
	bool ok = dx11_compiler_start(&oit.c, OIT_PIXEL_KEYS);
	for (g = 0; g < 2; g++)
		ok = dx11_oit_shaders_vs(g != 0) != NULL && ok;
	for (v = 0; v < OIT_GENERAL_COUNT; v++)
		if ((v & 3) <= DX11OIT_OIT)
			ok = dx11_compiler_missed_ps(&oit.c, DX11OITF_GENERAL_PS, v, &oit.general[v]) != NULL && ok;
	ok = dx11_oit_shaders_modvol_vs() != NULL && dx11_oit_shaders_modvol_ps() != NULL && ok;
	for (mode = 0; mode < 4; mode++)
		ok = dx11_oit_shaders_tr_modvol_ps((dx11_oit_modvol_mode)mode) != NULL && ok;
	ok = dx11_oit_shaders_final_vs() != NULL && dx11_oit_shaders_final_ps() != NULL && ok;
	oit.c.started = true;
	return ok;
}

ID3D11PixelShader *dx11_oit_shaders_ps(const dx11_oit_ps_params *params, bool *general)
{
	ID3D11PixelShader *own = dx11_compiler_polygon_ps(&oit.c, DX11OITF_PS, dx11_oit_shaders_key(params));
	*general = own == NULL;
	return own != NULL ? own
			: oit.general[(u32)params->pass | ((u32)params->gouraud << 2) | ((u32)params->twoVolume << 3)];
}

ID3D11VertexShader *dx11_oit_shaders_modvol_vs(void)
{
	if (oit.modvol_vs == NULL)
		dx11_compiler_make_vs(&oit.c, DX11OITF_MODVOL_VS, 0, &oit.modvol_vs, &oit.modvol_vs_blob);
	return oit.modvol_vs;
}

ID3D11PixelShader *dx11_oit_shaders_modvol_ps(void)
{
	if (oit.modvol_ps == NULL)
		dx11_compiler_missed_ps(&oit.c, DX11OITF_MODVOL_PS, 0, &oit.modvol_ps);
	return oit.modvol_ps;
}

ID3D11PixelShader *dx11_oit_shaders_tr_modvol_ps(dx11_oit_modvol_mode mode)
{
	ID3D11PixelShader **shader = &oit.tr_modvol_ps[mode & 3];
	if (*shader == NULL)
		dx11_compiler_missed_ps(&oit.c, DX11OITF_TR_MODVOL_PS, mode & 3, shader);
	return *shader;
}

ID3D11VertexShader *dx11_oit_shaders_final_vs(void)
{
	if (oit.final_vs == NULL)
		dx11_compiler_make_vs(&oit.c, DX11OITF_FINAL_VS, 0, &oit.final_vs, NULL);
	return oit.final_vs;
}

ID3D11PixelShader *dx11_oit_shaders_final_ps(void)
{
	if (oit.final_ps == NULL)
		dx11_compiler_missed_ps(&oit.c, DX11OITF_FINAL_PS, 0, &oit.final_ps);
	return oit.final_ps;
}

ID3DBlob *dx11_oit_shaders_vs_blob(void)
{
	dx11_oit_shaders_vs(true);
	return oit.vs_blob;
}

ID3DBlob *dx11_oit_shaders_modvol_vs_blob(void)
{
	dx11_oit_shaders_modvol_vs();
	return oit.modvol_vs_blob;
}

dx11_shader_compiler *dx11_oit_shaders_compiler(void)
{
	return &oit.c;
}

const dx11_shader_families *dx11_oit_shaders_families(void)
{
	return &oit_families;
}

#define RELEASE_TO_NULL(p, how) do { if ((p) != NULL) how(p); (p) = NULL; } while (0)

void dx11_oit_shaders_term(void)
{
	u32 i;
	dx11_compiler_term(&oit.c);
	for (i = 0; i < 2; i++)
		RELEASE_TO_NULL(oit.vs[i], ID3D11VertexShader_Release);
	for (i = 0; i < OIT_GENERAL_COUNT; i++)
		RELEASE_TO_NULL(oit.general[i], ID3D11PixelShader_Release);
	for (i = 0; i < 4; i++)
		RELEASE_TO_NULL(oit.tr_modvol_ps[i], ID3D11PixelShader_Release);
	RELEASE_TO_NULL(oit.vs_blob, ID3D10Blob_Release);
	RELEASE_TO_NULL(oit.modvol_vs, ID3D11VertexShader_Release);
	RELEASE_TO_NULL(oit.modvol_vs_blob, ID3D10Blob_Release);
	RELEASE_TO_NULL(oit.modvol_ps, ID3D11PixelShader_Release);
	RELEASE_TO_NULL(oit.final_vs, ID3D11VertexShader_Release);
	RELEASE_TO_NULL(oit.final_ps, ID3D11PixelShader_Release);
}
