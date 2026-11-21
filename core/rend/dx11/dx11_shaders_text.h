/* The HLSL both Direct3D 11 renderers' shaders share (dx11_shaders.c,
 * oit/dx11_oitshaders.c): the vertex transform, the modifier volumes'
 * vertex shader and the fog table, as string literals put together where
 * each family's source is (nothing put together while the core runs) */
#pragma once

#define DX11_TEXT_TRANSFORM \
"\n" \
"cbuffer VertexConstants : register(b0)\n" \
"{\n" \
"\t// The normal matrix, which is never more than a scale and then a move:\n" \
"\t// the guest's coordinates to clip space.\n" \
"\tfloat4 normalScale;\n" \
"\tfloat4 normalOffset;\n" \
"\t// 1/fZ_max, and the inverse of what the matrix scales z by (rend/depth_scale.h)\n" \
"\tfloat4 depthScale;\n" \
"};\n" \
"\n" \
"// A vertex's z is the guest's 1/w. It is made the clip space w's inverse,\n" \
"// and x and y are multiplied by w, so that the division by it leaves them\n" \
"// where they are and everything in between is interpolated with the\n" \
"// perspective. What the pixel shaders take for the depth (gl_FragCoord.w in\n" \
"// the other renderers) is 1/w, which goes across the screen in a straight\n" \
"// line. It is not handed down as it is, without the perspective: where a\n" \
"// triangle is clipped (a vertex far off the screen, as a floor's or a\n" \
"// train's near the camera has), what the rasterizer then makes of such a\n" \
"// value is not always right: the polygon got next to no depth and was\n" \
"// hidden behind the background.\n" \
"// The clip space w is handed down instead, with the perspective, which\n" \
"// clipping keeps right; one over it at a pixel is that 1/w.\n" \
"// The depth is the position's (rend/depth_scale.h): z goes out as S, so\n" \
"// that the depth the rasterizer interpolates is S*z, the guest's 1/w up to\n" \
"// 1 at fZ_max, linear across the screen as the chip has it; no pixel\n" \
"// shader writes a depth. This one function places the vertices of every\n" \
"// shader, and the position is precise, so a pass that tests EQUAL against\n" \
"// another pass's depth is exact.\n" \
"float4 transformVertex(float3 pos, out float clipW)\n" \
"{\n" \
"\tprecise float4 vpos = float4(pos * normalScale.xyz + normalOffset.xyz, 1.0);\n" \
"\tvpos.w = 1.0 / vpos.z;\n" \
"\tvpos.xy *= vpos.w;\n" \
"\t// (a z beyond fZ_max is at depth 1, not clipped)\n" \
"\tvpos.z = min(depthScale.x * depthScale.y, vpos.w);\n" \
"\tclipW = vpos.w;\n" \
"\treturn vpos;\n" \
"}\n"

#define DX11_TEXT_MODVOL_VS \
"\n" \
"struct VertexOut\n" \
"{\n" \
"\tprecise float4 pos : SV_POSITION;\n" \
"\tfloat clipW : TEXCOORD1;\n" \
"};\n" \
"\n" \
"VertexOut main(in float3 pos : POSITION)\n" \
"{\n" \
"\tVertexOut vo;\n" \
"\t// (behind the eye, or as good as at an infinite distance: nowhere)\n" \
"\tif (pos.z < 0.0 || pos.z > 3.4e37)\n" \
"\t{\n" \
"\t\tvo.pos = float4(0.0, 0.0, 1.0, 1.0 / pos.z);\n" \
"\t\tvo.clipW = vo.pos.w;\n" \
"\t\treturn vo;\n" \
"\t}\n" \
"\tvo.pos = transformVertex(pos, vo.clipW);\n" \
"\treturn vo;\n" \
"}\n"

#define DX11_TEXT_FOG \
"\n" \
"// The fog coefficient for a depth, worked out the way the PowerVR2 does it.\n" \
"// The depth times the fog density, between 1 and just under 256, is taken\n" \
"// apart as a float: its exponent and the top four bits of its mantissa\n" \
"// pick one of the 128 table entries, and the next eight bits of mantissa\n" \
"// blend that entry with the following one. The blend is done in whole\n" \
"// numbers out of 256 and so is the result, which is returned over 256.\n" \
"float fog_mode2(float w)\n" \
"{\n" \
"\t// (max() and min() hand back the other operand when one is not a\n" \
"\t// number - an infinite depth times a density of zero - and that comes\n" \
"\t// out as 1 like anything else below 1)\n" \
"\tfloat z = min(max(w * sp_FOG_DENSITY, 1.0), 255.9999);\n" \
"\tfloat e = 0.0;\n" \
"\tif (z >= 16.0) { z *= 0.0625; e = 4.0; }\n" \
"\tif (z >= 4.0) { z *= 0.25; e += 2.0; }\n" \
"\tif (z >= 2.0) { z *= 0.5; e += 1.0; }\n" \
"\tfloat t = (z - 1.0) * 16.0;\n" \
"\tfloat i = floor(t);\n" \
"\tfloat blend = floor((t - i) * 256.0);\n" \
"\tfloat u = (e * 16.0 + i + 0.5) / 128.0;\n" \
"\t// The table is two rows: the entry's own value under 0.75, the next entry's under 0.25\n" \
"\tfloat next = floor(fog_table.SampleLevel(pointSampler, float2(u, 0.25), 0).r * 255.0 + 0.5);\n" \
"\tfloat cur = floor(fog_table.SampleLevel(pointSampler, float2(u, 0.75), 0).r * 255.0 + 0.5);\n" \
"\treturn floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;\n" \
"}\n" \
"\n"
