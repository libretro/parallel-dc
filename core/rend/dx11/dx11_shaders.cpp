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
/* The per-triangle renderer's shaders: the Vulkan renderer's GLSL
 * (vulkan/shaders.cpp) in HLSL, in the shape upstream's Direct3D 11
 * shaders have. What a pixel comes out as is what the Vulkan and OpenGL
 * renderers make of it, bit for bit where the two languages allow. */
#include "dx11_shaders.h"
#include <streams/file_stream.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

extern std::string get_writable_data_path(const std::string& filename);

/* The shaders the renderers' start makes, as bytecode (dx11_dxbc_index[]
 * sorted by hash, the bytes in dx11_dxbc_bytes[]) */
#include "dx11_dxbc.inc"

const char * const DX11VertexTransform = R"(
cbuffer VertexConstants : register(b0)
{
	// The normal matrix, which is never more than a scale and then a move:
	// the guest's coordinates to clip space.
	float4 normalScale;
	float4 normalOffset;
	// 1/fZ_max, and the inverse of what the matrix scales z by (rend/depth_scale.h)
	float4 depthScale;
};

// A vertex's z is the guest's 1/w. It is made the clip space w's inverse,
// and x and y are multiplied by w, so that the division by it leaves them
// where they are and everything in between is interpolated with the
// perspective. What the pixel shaders take for the depth (gl_FragCoord.w in
// the other renderers) is 1/w, which goes across the screen in a straight
// line. It is not handed down as it is, without the perspective: where a
// triangle is clipped (a vertex far off the screen, as a floor's or a
// train's near the camera has), what the rasterizer then makes of such a
// value is not always right: the polygon got next to no depth and was
// hidden behind the background.
// The clip space w is handed down instead, with the perspective, which
// clipping keeps right; one over it at a pixel is that 1/w.
// The depth is the position's (rend/depth_scale.h): z goes out as S, so
// that the depth the rasterizer interpolates is S*z, the guest's 1/w up to
// 1 at fZ_max, linear across the screen as the chip has it; no pixel
// shader writes a depth. This one function places the vertices of every
// shader, and the position is precise, so a pass that tests EQUAL against
// another pass's depth is exact.
float4 transformVertex(float3 pos, out float clipW)
{
	precise float4 vpos = float4(pos * normalScale.xyz + normalOffset.xyz, 1.0);
	vpos.w = 1.0 / vpos.z;
	vpos.xy *= vpos.w;
	// (a z beyond fZ_max is at depth 1, not clipped)
	vpos.z = min(depthScale.x * depthScale.y, vpos.w);
	clipW = vpos.w;
	return vpos;
}
)";

static const char VertexShaderSource[] = R"(
#if pp_Gouraud == 1
#define INTERPOLATION
#else
#define INTERPOLATION nointerpolation
#endif

struct VertexIn
{
	float3 pos : POSITION;
	float4 col : COLOR0;
	float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
};

struct VertexOut
{
	precise float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	float clipW : TEXCOORD1;
};

VertexOut main(in VertexIn vin)
{
	VertexOut vo;
	vo.pos = transformVertex(vin.pos, vo.clipW);
	vo.col = vin.col;
	vo.spec = vin.spec;
	vo.uv = vin.uv;
	return vo;
}
)";

const char * const DX11ModVolVertexShader = R"(
struct VertexOut
{
	precise float4 pos : SV_POSITION;
	float clipW : TEXCOORD1;
};

VertexOut main(in float3 pos : POSITION)
{
	VertexOut vo;
	// (behind the eye, or as good as at an infinite distance: nowhere)
	if (pos.z < 0.0 || pos.z > 3.4e37)
	{
		vo.pos = float4(0.0, 0.0, 1.0, 1.0 / pos.z);
		vo.clipW = vo.pos.w;
		return vo;
	}
	vo.pos = transformVertex(pos, vo.clipW);
	return vo;
}
)";

const char * const DX11FogFunction = R"(
// The fog coefficient for a depth, worked out the way the PowerVR2 does it.
// The depth times the fog density, between 1 and just under 256, is taken
// apart as a float: its exponent and the top four bits of its mantissa
// pick one of the 128 table entries, and the next eight bits of mantissa
// blend that entry with the following one. The blend is done in whole
// numbers out of 256 and so is the result, which is returned over 256.
float fog_mode2(float w)
{
	// (max() and min() hand back the other operand when one is not a
	// number - an infinite depth times a density of zero - and that comes
	// out as 1 like anything else below 1)
	float z = min(max(w * sp_FOG_DENSITY, 1.0), 255.9999);
	float e = 0.0;
	if (z >= 16.0) { z *= 0.0625; e = 4.0; }
	if (z >= 4.0) { z *= 0.25; e += 2.0; }
	if (z >= 2.0) { z *= 0.5; e += 1.0; }
	float t = (z - 1.0) * 16.0;
	float i = floor(t);
	float blend = floor((t - i) * 256.0);
	float u = (e * 16.0 + i + 0.5) / 128.0;
	// The table is two rows: the entry's own value under 0.75, the next entry's under 0.25
	float next = floor(fog_table.SampleLevel(pointSampler, float2(u, 0.25), 0).r * 255.0 + 0.5);
	float cur = floor(fog_table.SampleLevel(pointSampler, float2(u, 0.75), 0).r * 255.0 + 0.5);
	return floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;
}

)";

static const char PixelShaderHeader[] = R"(
cbuffer FragmentConstants : register(b0)
{
	float4 colorClampMin;
	float4 colorClampMax;
	float4 sp_FOG_COL_RAM;
	float4 sp_FOG_COL_VERT;
	float cp_AlphaTestValue;
	float sp_FOG_DENSITY;
	float shade_scale_factor;
};

Texture2D fog_table : register(t2);
// nearest, clamped: the fog table's and the palette's
SamplerState pointSampler : register(s1);
)";

static const char PixelShaderSource[] = R"(
#if pp_Gouraud == 1
#define INTERPOLATION
#else
#define INTERPOLATION nointerpolation
#endif
#define PI 3.1415926

cbuffer PolyConstants : register(b1)
{
	float4 clipTest;
	float trilinearAlpha;
	float palette_index;
	float prepassAlpha;
	uint polyKey;	// the general shader's: DX11FragmentShaderParams::hash()
};

// A polygon's own shader has its key's bits as numbers, and the branches
// they decide are compiled away; the general shader (GENERAL) reads them
// from the polygon's constants, and draws what its own shader would while
// that is made: the same code, the same pixels
#ifdef GENERAL
#define cp_AlphaTest (polyKey & 1u)
#define pp_ClipInside ((polyKey >> 1) & 1u)
#define pp_UseAlpha ((polyKey >> 2) & 1u)
#define pp_Texture ((polyKey >> 3) & 1u)
#define pp_IgnoreTexA ((polyKey >> 4) & 1u)
#define pp_ShadInstr ((polyKey >> 5) & 3u)
#define pp_Offset ((polyKey >> 7) & 1u)
#define pp_FogCtrl ((polyKey >> 8) & 3u)
#define pp_BumpMap ((polyKey >> 11) & 1u)
#define ColorClamping ((polyKey >> 12) & 1u)
#define pp_TriLinear ((polyKey >> 13) & 1u)
#define pp_Palette ((polyKey >> 14) & 1u)
#define pp_Shadowed ((polyKey >> 15) & 1u)
#endif

Texture2D tex : register(t0);
SamplerState texSampler : register(s0);
Texture2D palette : register(t1);

float4 colorClamp(float4 col)
{
	if (ColorClamping == 1)
		return clamp(col, colorClampMin, colorClampMax);
	return col;
}

float4 palettePixel(float2 coords)
{
	// palette_index is where the middle of the palette's first colour is in the row of 1024
	float2 c = float2(tex.Sample(texSampler, coords).r * (255.0 / 1024.0) + palette_index, 0.5);
	return palette.SampleLevel(pointSampler, c, 0);
}

struct Pixel
{
	float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	float clipW : TEXCOORD1;
};

// (the depth is the position's, which the depth test took: no SV_Depth)
struct PSO
{
	float4 col : SV_Target;
};

PSO main(in Pixel inpix)
{
	const float w = 1.0 / inpix.clipW;
	// Clip inside the box
	if (pp_ClipInside == 1)
	{
		if (inpix.pos.x >= clipTest.x && inpix.pos.x <= clipTest.z
				&& inpix.pos.y >= clipTest.y && inpix.pos.y <= clipTest.w)
			discard;
	}

	float4 color = inpix.col;
	if (pp_UseAlpha == 0)
		color.a = 1.0;
	float4 offset = inpix.spec;
	if (pp_Shadowed == 1)
	{
		// A pixel inside a modifier volume: what the polygon is shaded with,
		// its base and offset colours, is scaled before the texture is
		// combined with them. The texture itself is not.
		color.rgb *= shade_scale_factor;
		offset.rgb *= shade_scale_factor;
	}
	if (pp_Texture == 1)
	{
		float4 texcol;
		if (pp_Palette == 0)
			texcol = tex.Sample(texSampler, inpix.uv);
		else
			texcol = palettePixel(inpix.uv);
		if (pp_BumpMap == 1)
		{
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);
			texcol.rgb = float3(1.0, 1.0, 1.0);
		}
		else if (pp_IgnoreTexA == 1)
			texcol.a = 1.0;
		if (pp_ShadInstr == 0)
			color = texcol;
		if (pp_ShadInstr == 1)
		{
			color.rgb *= texcol.rgb;
			color.a = texcol.a;
		}
		if (pp_ShadInstr == 2)
			color.rgb = lerp(color.rgb, texcol.rgb, texcol.a);
		if (pp_ShadInstr == 3)
			color *= texcol;
		if (pp_Offset == 1 && pp_BumpMap == 0)
			color.rgb += offset.rgb;
	}

	color = colorClamp(color);

	// Look-up table mode 2: the fog colour takes the place of the pixel's
	// colour and the fog coefficient the place of its alpha, after the
	// texture has been combined in.
	if (pp_FogCtrl == 3)
		color = float4(sp_FOG_COL_RAM.rgb, fog_mode2(w) * (256.0 / 255.0));
	if (pp_FogCtrl == 0)
	{
		float fog = fog_mode2(w);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;
	}
	if (pp_FogCtrl == 1 && pp_Offset == 1 && pp_BumpMap == 0)
	{
		float fog = offset.a * (255.0 / 256.0);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;
	}

	if (pp_TriLinear == 1)
		color *= trilinearAlpha;

	// Punch-through polygons are tested on the alpha of the finished colour,
	// after the texture and the vertex colours are combined and clamped.
	if (cp_AlphaTest == 1)
	{
		if (cp_AlphaTestValue > color.a)
			discard;
		color.a = 1.0;
	}
	// The depth prepass of sorted translucent polygons (rend/sorter.h): the
	// pixels that hide what is behind them. (0 for the colours.)
	if (color.a < prepassAlpha)
		discard;

	PSO pso;
	pso.col = color;
	return pso;
}
)";

static const char ModVolPixelShaderSource[] = R"(
struct Pixel
{
	float4 pos : SV_POSITION;
	float clipW : TEXCOORD1;
};

// The colour is only ever written by the pass that darkens what is left
// in the volumes: by what the shadows take away (the depth is the position's)
float4 main(in Pixel inpix) : SV_Target
{
	return float4(0.0, 0.0, 0.0, 1.0 - shade_scale_factor);
}
)";

static const char QuadVertexShaderSource[] = R"(
struct VertexIn
{
	float2 pos : POSITION;
	float2 uv : TEXCOORD0;
};

struct VertexOut
{
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

VertexOut main(in VertexIn vin)
{
	VertexOut vo;
	vo.pos = float4(vin.pos, 0.0, 1.0);
	vo.uv = vin.uv;
	return vo;
}
)";

static const char QuadPixelShaderSource[] = R"(
struct VertexOut
{
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

Texture2D tex : register(t0);
SamplerState texSampler : register(s0);

float4 main(in VertexOut vin) : SV_Target
{
	return tex.Sample(texSampler, vin.uv);
}
)";

ComPtr<ID3DBlob> DX11ShaderCompiler::Compile(const char *source, const char *entry, const char *profile, const D3D_SHADER_MACRO *macros,
		ID3DInclude *include)
{
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> errors;

	if (compile == nullptr)
		return blob;
	HRESULT hr = compile(source, strlen(source), nullptr, macros, include, entry, profile, 0, 0, blob.put(), errors.put());
	if (FAILED(hr))
	{
		ERROR_LOG(RENDERER, "Shader compilation failed (%x): %s", (unsigned)hr,
				errors ? (const char *)errors->GetBufferPointer() : "");
		blob.reset();
	}
	return blob;
}

ComPtr<ID3D11VertexShader> DX11ShaderCompiler::CompileVS(const char *source, const char *entry, const char *profile,
		const D3D_SHADER_MACRO *macros, ComPtr<ID3DBlob> *blobOut, ID3DInclude *include)
{
	ComPtr<ID3D11VertexShader> shader;
	ComPtr<ID3DBlob> blob = Compile(source, entry, profile, macros, include);
	if (blob && FAILED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.put())))
		ERROR_LOG(RENDERER, "Vertex shader creation failed");
	if (blobOut != nullptr)
		*blobOut = blob;
	return shader;
}

ComPtr<ID3D11PixelShader> DX11ShaderCompiler::CompilePS(const char *source, const char *entry, const char *profile,
		const D3D_SHADER_MACRO *macros, ID3DInclude *include)
{
	ComPtr<ID3D11PixelShader> shader;
	ComPtr<ID3DBlob> blob = Compile(source, entry, profile, macros, include);
	if (blob && FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.put())))
		ERROR_LOG(RENDERER, "Pixel shader creation failed");
	return shader;
}

/* ---- The file, the thread ---- */

/* Bytecode from the file, for an input layout: a blob of its own */
class DX11FileBlob : public ID3DBlob
{
public:
	DX11FileBlob(const void *code, size_t size) : size(size)
	{
		data = malloc(size);
		if (data != nullptr)
			memcpy(data, code, size);
	}
	virtual ~DX11FileBlob() { free(data); }
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob))
		{
			AddRef();
			*out = this;
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG n = --refs;
		if (n == 0)
			delete this;
		return n;
	}
	LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return data; }
	SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return size; }

private:
	void *data = nullptr;
	size_t size;
	ULONG refs = 1;
};

u64 DX11ShaderCompiler::HashOf(const DX11ShaderDesc& d)
{
	u64 h = d3d_shcache_hash(D3D_SHCACHE_HASH_INIT, d.source->c_str(), d.source->size() + 1);
	h = d3d_shcache_hash(h, d.profile, strlen(d.profile) + 1);
	for (const D3D_SHADER_MACRO *m = d.macros; m->Name != nullptr; m++)
	{
		h = d3d_shcache_hash(h, m->Name, strlen(m->Name) + 1);
		h = d3d_shcache_hash(h, m->Definition, strlen(m->Definition) + 1);
	}
	return h;
}

const u8 *DX11ShaderCompiler::BuiltIn(u64 hash, u32 *size)
{
	size_t lo = 0, hi = dx11_dxbc_count;
	while (lo < hi)
	{
		const size_t mid = (lo + hi) / 2;
		if (dx11_dxbc_index[mid].hash < hash)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == dx11_dxbc_count || dx11_dxbc_index[lo].hash != hash)
		return nullptr;
	*size = dx11_dxbc_index[lo].size;
	return &dx11_dxbc_bytes[dx11_dxbc_index[lo].offset];
}

const u8 *DX11ShaderCompiler::FromFile(u64 hash, u32 *size) const
{
	size_t lo = 0, hi = fileIndex.size();
	while (lo < hi)
	{
		const size_t mid = (lo + hi) / 2;
		if (fileIndex[mid].hash < hash)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == fileIndex.size() || fileIndex[lo].hash != hash)
		return nullptr;
	*size = fileIndex[lo].size;
	return &file[fileBlobs + fileIndex[lo].offset];
}

void DX11ShaderCompiler::Keep(u64 hash, u32 family, u32 variant, ID3DBlob *blob)
{
	if (blob == nullptr || blob->GetBufferSize() == 0 || kept.Find(hash) != nullptr)
		return;
	Kept *k = kept.Add(hash);
	k->family = family;
	k->variant = variant;
	blob->AddRef();
	k->blob.reset(blob);
	fileChanged = true;
}

bool DX11ShaderCompiler::Bytecode(u32 family, u32 variant, const DX11ShaderDesc& d, ComPtr<ID3DBlob>& hold,
		const void **code, size_t *size, bool *fromFile)
{
	const u64 hash = HashOf(d);
	u32 bytes = 0;
	const u8 *p = BuiltIn(hash, &bytes);
	*fromFile = false;
	if (p != nullptr)
	{
		statBuiltIn++;
		*code = p;
		*size = bytes;
		return true;
	}
	p = FromFile(hash, &bytes);
	*fromFile = p != nullptr;
	if (p != nullptr)
	{
		*code = p;
		*size = bytes;
		return true;
	}
	const Kept *k = kept.Find(hash);
	if (k != nullptr)
	{
		*code = k->blob->GetBufferPointer();
		*size = k->blob->GetBufferSize();
		return true;
	}
	hold = Compile(d.source->c_str(), "main", d.profile, d.macros);
	if (!hold)
		return false;
	statCompiled++;
	*code = hold->GetBufferPointer();
	*size = hold->GetBufferSize();
	Keep(hash, family, variant, hold);
	return true;
}

/* The thread's: the pixel shader of @job (kind the family, aux the
 * variant), from the file or compiled; key handed back the hash, extra
 * the bytecode compiled */
bool DX11ShaderCompiler::WarmBuild(void *ctx, const d3d_warm_job_t *job, d3d_warm_result_t *res)
{
	const DX11ShaderCompiler *self = (const DX11ShaderCompiler *)ctx;
	DX11ShaderDesc d;
	if (!self->Describe(job->kind, job->aux, d))
		return false;
	const u64 hash = HashOf(d);
	u32 size = 0;
	const void *code = BuiltIn(hash, &size);
	if (code == nullptr)
		code = self->FromFile(hash, &size);
	ID3DBlob *blob = nullptr;
	if (code == nullptr)
	{
		ID3DBlob *errors = nullptr;
		if (self->compile == nullptr
				|| FAILED(self->compile(d.source->c_str(), d.source->size(), nullptr, d.macros, nullptr, "main", d.profile, 0, 0, &blob, &errors)))
		{
			if (errors != nullptr)
				errors->Release();
			if (blob != nullptr)
				blob->Release();
			return false;
		}
		if (errors != nullptr)
			errors->Release();
		code = blob->GetBufferPointer();
		size = (u32)blob->GetBufferSize();
	}
	ID3D11PixelShader *shader = nullptr;
	if (FAILED(self->device->CreatePixelShader(code, size, nullptr, &shader)))
	{
		if (blob != nullptr)
			blob->Release();
		return false;
	}
	res->key = hash;
	res->obj = shader;
	res->extra = blob;
	return true;
}

void DX11ShaderCompiler::WarmRelease(void *ctx, d3d_warm_result_t *res)
{
	(void)ctx;
	if (res->obj != nullptr)
		((ID3D11PixelShader *)res->obj)->Release();
	if (res->extra != nullptr)
		((ID3DBlob *)res->extra)->Release();
	res->obj = res->extra = nullptr;
}

void DX11ShaderCompiler::TakeBuilt()
{
	d3d_warm_result_t r;
	while (d3d_warm_take(warm, &r))
	{
		if (r.extra != nullptr)
		{
			ID3DBlob *blob = (ID3DBlob *)r.extra;
			Keep(r.key, r.kind, r.aux, blob);
			blob->Release();
			statCompiledAhead++;
		}
		statAhead++;
		Adopt(r.kind, r.aux, (ID3D11PixelShader *)r.obj);
	}
}

bool DX11ShaderCompiler::Request(u32 family, u32 variant)
{
	d3d_warm_job_t j;
	/* (no warm thread: the general shader draws it for the run) */
	if (warm == nullptr)
		return true;
	j.key = 0;
	j.kind = family;
	j.aux = variant;
	return d3d_warm_submit(warm, &j, 1) == 1;
}

ID3D11PixelShader *DX11ShaderCompiler::MissedPS(u32 family, u32 variant, ComPtr<ID3D11PixelShader>& slot)
{
	TakeBuilt();
	if (slot)
		return slot;
	if (started)
		statDrawPath++;
	DX11ShaderDesc d;
	ComPtr<ID3DBlob> hold;
	const void *code;
	size_t size;
	bool fromFile;
	if (!Describe(family, variant, d) || !Bytecode(family, variant, d, hold, &code, &size, &fromFile))
		return nullptr;
	if (fromFile)
		statFromFile++;
	if (FAILED(device->CreatePixelShader(code, size, nullptr, slot.put())))
		ERROR_LOG(RENDERER, "Pixel shader creation failed");
	return slot;
}

ID3D11VertexShader *DX11ShaderCompiler::MakeVS(u32 family, u32 variant, ComPtr<ID3D11VertexShader>& slot, ComPtr<ID3DBlob> *blob)
{
	DX11ShaderDesc d;
	if (started)
		statDrawPath++;
	ComPtr<ID3DBlob> hold;
	const void *code;
	size_t size;
	bool fromFile;
	if (!Describe(family, variant, d) || !Bytecode(family, variant, d, hold, &code, &size, &fromFile))
		return nullptr;
	if (fromFile)
		statFromFile++;
	if (FAILED(device->CreateVertexShader(code, size, nullptr, slot.put())))
		ERROR_LOG(RENDERER, "Vertex shader creation failed");
	if (blob != nullptr)
	{
		if (!hold)
			hold.reset(new DX11FileBlob(code, size));
		*blob = hold;
	}
	return slot;
}

ID3D11ComputeShader *DX11ShaderCompiler::MakeCS(u32 family, u32 variant, ComPtr<ID3D11ComputeShader>& slot)
{
	DX11ShaderDesc d;
	ComPtr<ID3DBlob> hold;
	const void *code;
	size_t size;
	bool fromFile;
	if (!Describe(family, variant, d) || !Bytecode(family, variant, d, hold, &code, &size, &fromFile))
		return nullptr;
	if (fromFile)
		statFromFile++;
	if (FAILED(device->CreateComputeShader(code, size, nullptr, slot.put())))
		ERROR_LOG(RENDERER, "Compute shader creation failed");
	return slot;
}

void DX11ShaderCompiler::LoadFile()
{
	file.clear();
	fileIndex.clear();
	fileBlobs = 0;
	const std::string path = get_writable_data_path(FileName());
	void *data = nullptr;
	int64_t size = 0;
	if (filestream_read_file(path.c_str(), &data, &size) && data != nullptr && size > 0)
	{
		d3d_shcache_entry_t *entries = nullptr;
		size_t count = 0;
		if (d3d_shcache_parse((const u8 *)data, (size_t)size, &entries, &count, &fileBlobs))
		{
			file.assign((const u8 *)data, (const u8 *)data + size);
			fileIndex.assign(entries, entries + count);
			std::sort(fileIndex.begin(), fileIndex.end(),
					[](const d3d_shcache_entry_t& a, const d3d_shcache_entry_t& b) { return a.hash < b.hash; });
		}
		else
			WARN_LOG(RENDERER, "DX11: shader file %s is not one: ignored", path.c_str());
		free(entries);
	}
	free(data);
}

/* What the file had and what this run compiled, by family and variant:
 * this run's where both have one */
void DX11ShaderCompiler::SaveFile()
{
	if (!fileChanged)
		return;
	std::vector<d3d_shcache_entry_t> entries;
	std::vector<const void *> blobs;
	std::unordered_map<u64, size_t> byName;
	kept.Each([&](u64 hash, Kept& k) {
		const u64 name = ((u64)k.family << 32) | k.variant;
		if (byName.count(name) != 0 || entries.size() >= D3D_SHCACHE_MAX_ENTRIES)
			return true;
		byName[name] = entries.size();
		d3d_shcache_entry_t e{};
		e.hash = hash;
		e.family = k.family;
		e.variant = k.variant;
		e.size = (u32)k.blob->GetBufferSize();
		entries.push_back(e);
		blobs.push_back(k.blob->GetBufferPointer());
		return true;
	});
	for (const d3d_shcache_entry_t& f : fileIndex)
	{
		const u64 name = ((u64)f.family << 32) | f.variant;
		if (byName.count(name) != 0 || entries.size() >= D3D_SHCACHE_MAX_ENTRIES)
			continue;
		byName[name] = entries.size();
		entries.push_back(f);
		blobs.push_back(&file[fileBlobs + f.offset]);
	}
	if (entries.empty())
		return;
	std::vector<u8> out(d3d_shcache_file_size(entries.data(), entries.size()));
	if (!d3d_shcache_write(out.data(), out.size(), entries.data(), entries.size(), blobs.data()))
		return;
	// (written beside it, then put in its place: a write cut short leaves the one there was)
	const std::string path = get_writable_data_path(FileName());
	const std::string tmpPath = path + ".tmp";
	RFILE *f = filestream_open(tmpPath.c_str(), RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f == nullptr)
		return;
	bool written = filestream_write(f, out.data(), (int64_t)out.size()) == (int64_t)out.size();
	filestream_close(f);
	if (written)
	{
		filestream_delete(path.c_str());
		written = filestream_rename(tmpPath.c_str(), path.c_str()) == 0;
	}
	if (!written)
		filestream_delete(tmpPath.c_str());
	else
		NOTICE_LOG(RENDERER, "DX11: shader file saved: %u shaders", (u32)entries.size());
}

void DX11ShaderCompiler::Init(ID3D11Device *device, pD3DCompile compile)
{
	// (made again without being ended: the last thread's work is not kept)
	if (warm != nullptr)
	{
		d3d_warm_free(warm);
		warm = nullptr;
	}
	this->device = device;
	this->compile = compile;
	statCompiled = statFromFile = statBuiltIn = statAhead = statCompiledAhead = 0;
	/* (room for what a run compiles; more is made, and said) */
	kept.Name("DX11 shaders compiled");
	kept.Clear();
	kept.Reserve(1024);
	fileChanged = false;
	MakeSources();
	LoadFile();
	/* (a device made for one thread only is used from the one that renders
	 * only: the file still spares it compiling) */
	if (device == nullptr || (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0)
		return;
	statDrawPath = statGeneral = 0;
	started = false;
	std::vector<d3d_warm_job_t> jobs;
	Fixed(jobs);
	const size_t fixed = jobs.size();
	for (const d3d_shcache_entry_t& e : fileIndex)
		if (PixelFamily(e.family))
		{
			// (one of a kind, queued already; the file has each once)
			size_t i = 0;
			while (i < fixed && (jobs[i].kind != e.family || jobs[i].aux != e.variant))
				i++;
			if (i < fixed)
				continue;
			d3d_warm_job_t j;
			j.key = 0;
			j.kind = e.family;
			j.aux = e.variant;
			jobs.push_back(j);
		}
	// (room for the keys a game first wants while it runs, too)
	warm = d3d_warm_new(WarmBuild, WarmRelease, this, std::max<size_t>(jobs.size(), 1024));
	if (warm == nullptr || jobs.empty())
		return;
	const size_t taken = d3d_warm_submit(warm, jobs.data(), jobs.size());
	NOTICE_LOG(RENDERER, "DX11: %u shaders in %s, %u pixel shaders to be made ahead",
			(u32)fileIndex.size(), FileName(), (u32)taken);
}

void DX11ShaderCompiler::TermCompiler()
{
	if (warm != nullptr)
	{
		d3d_warm_stop(warm);
		TakeBuilt();
		d3d_warm_free(warm);
		warm = nullptr;
	}
	if (device != nullptr)
		NOTICE_LOG(RENDERER, "DX11: %s: %u compiled, %u the core's, %u from the file, %u made ahead (%u of them compiled), %u made on the draw path, %u draws with a general shader",
				FileName(), statCompiled, statBuiltIn, statFromFile, statAhead, statCompiledAhead, statDrawPath, statGeneral);
	SaveFile();
	file.clear();
	file.shrink_to_fit();
	fileIndex.clear();
	kept.Clear();
	fileChanged = false;
}

/* ---- The per-triangle renderer's ---- */

static const char * const MacroValues[] = { "0", "1", "2", "3" };

void DX11Shaders::MakeSources()
{
	sources[VS] = std::string(DX11VertexTransform) + VertexShaderSource;
	sources[PS] = std::string(PixelShaderHeader) + DX11FogFunction + PixelShaderSource;
	sources[ModVolVS] = std::string(DX11VertexTransform) + DX11ModVolVertexShader;
	sources[ModVolPS] = std::string(PixelShaderHeader) + DX11FogFunction + ModVolPixelShaderSource;
	sources[QuadVS] = QuadVertexShaderSource;
	sources[QuadPS] = QuadPixelShaderSource;
	sources[WriteOutCS] = DX11WriteOutSource;
}

bool DX11Shaders::Describe(u32 family, u32 variant, DX11ShaderDesc& d) const
{
	D3D_SHADER_MACRO *m = d.macros;
	switch (family)
	{
	case VS:
		d.source = &sources[VS];
		d.profile = "vs_4_0";
		*m++ = { "pp_Gouraud", MacroValues[variant & 1] };
		break;
	case PS:
		/* (the variant is DX11FragmentShaderParams::hash()) */
		d.source = &sources[PS];
		d.profile = "ps_4_0";
		*m++ = { "cp_AlphaTest", MacroValues[variant & 1] };
		*m++ = { "pp_ClipInside", MacroValues[(variant >> 1) & 1] };
		*m++ = { "pp_UseAlpha", MacroValues[(variant >> 2) & 1] };
		*m++ = { "pp_Texture", MacroValues[(variant >> 3) & 1] };
		*m++ = { "pp_IgnoreTexA", MacroValues[(variant >> 4) & 1] };
		*m++ = { "pp_ShadInstr", MacroValues[(variant >> 5) & 3] };
		*m++ = { "pp_Offset", MacroValues[(variant >> 7) & 1] };
		*m++ = { "pp_FogCtrl", MacroValues[(variant >> 8) & 3] };
		*m++ = { "pp_Gouraud", MacroValues[(variant >> 10) & 1] };
		*m++ = { "pp_BumpMap", MacroValues[(variant >> 11) & 1] };
		*m++ = { "ColorClamping", MacroValues[(variant >> 12) & 1] };
		*m++ = { "pp_TriLinear", MacroValues[(variant >> 13) & 1] };
		*m++ = { "pp_Palette", MacroValues[(variant >> 14) & 1] };
		*m++ = { "pp_Shadowed", MacroValues[(variant >> 15) & 1] };
		break;
	case GeneralPS:
		d.source = &sources[PS];
		d.profile = "ps_4_0";
		*m++ = { "GENERAL", "1" };
		*m++ = { "pp_Gouraud", MacroValues[variant & 1] };
		break;
	case ModVolVS:
		d.source = &sources[ModVolVS];
		d.profile = "vs_4_0";
		break;
	case ModVolPS:
		d.source = &sources[ModVolPS];
		d.profile = "ps_4_0";
		break;
	case QuadVS:
		d.source = &sources[QuadVS];
		d.profile = "vs_4_0";
		break;
	case QuadPS:
		d.source = &sources[QuadPS];
		d.profile = "ps_4_0";
		break;
	case WriteOutCS:
		d.source = &sources[WriteOutCS];
		d.profile = "cs_5_0";
		break;
	default:
		return false;
	}
	*m = { nullptr, nullptr };
	return true;
}

void DX11Shaders::Adopt(u32 family, u32 variant, ID3D11PixelShader *shader)
{
	ComPtr<ID3D11PixelShader> *slot = nullptr;
	switch (family)
	{
	case PS:
		if (variant < pixelShaders.size())
			slot = &pixelShaders[variant];
		break;
	case ModVolPS:
		slot = &modVolPixelShader;
		break;
	case QuadPS:
		slot = &quadPixelShader;
		break;
	}
	if (slot != nullptr && !*slot)
		slot->reset(shader);
	else
		shader->Release();
}

void DX11Shaders::Fixed(std::vector<d3d_warm_job_t>& jobs) const
{
	static const u32 fixed[] = { ModVolPS, QuadPS };
	for (u32 f : fixed)
	{
		d3d_warm_job_t j;
		j.key = 0;
		j.kind = f;
		j.aux = 0;
		jobs.push_back(j);
	}
}

ID3D11VertexShader *DX11Shaders::GetVertexShader(bool gouraud)
{
	ComPtr<ID3D11VertexShader>& shader = vertexShaders[gouraud];
	if (!shader)
	{
		ComPtr<ID3DBlob> blob;
		MakeVS(VS, gouraud, shader, &blob);
		if (!vertexBlob)
			vertexBlob = blob;
	}
	return shader;
}

bool DX11Shaders::Start()
{
	pixelShaders.assign(PixelKeys, ComPtr<ID3D11PixelShader>());
	requested.assign(PixelKeys / 32, 0);
	for (u32 g = 0; g < 2; g++)
	{
		GetVertexShader(g != 0);
		MissedPS(GeneralPS, g, generalShaders[g]);
	}
	GetModVolVertexShader();
	GetModVolPixelShader();
	GetQuadVertexShader();
	GetQuadPixelShader();
	Started();
	return vertexShaders[0] && vertexShaders[1] && generalShaders[0] && generalShaders[1]
			&& modVolVertexShader && modVolPixelShader && quadVertexShader && quadPixelShader;
}

ID3D11PixelShader *DX11Shaders::GetPixelShader(const DX11FragmentShaderParams& params, bool *general)
{
	const u32 key = params.hash();
	*general = false;
	Memo& m = MemoFor(key);
	if (m.shader != nullptr && m.key == key)
		return m.shader;
	ComPtr<ID3D11PixelShader>& shader = pixelShaders[key];
#ifndef DX11_SHADER_TEST_GENERAL
	if (!shader)
		TakeBuilt();
	if (shader)
	{
		m.key = key;
		m.shader = shader;
		return shader;
	}
	// (its own asked for once; drawn with the general one till it is there)
	u32& word = requested[key >> 5];
	if ((word & (1u << (key & 31))) == 0)
	{
		word |= 1u << (key & 31);
		/* (the queue full: asked again at its next draw) */
		if (!Request(PS, key))
			word &= ~(1u << (key & 31));
	}
#endif
	statGeneral++;
	*general = true;
	return generalShaders[params.gouraud];
}

ID3D11VertexShader *DX11Shaders::GetModVolVertexShader()
{
	if (!modVolVertexShader)
		MakeVS(ModVolVS, 0, modVolVertexShader, &modVolVertexBlob);
	return modVolVertexShader;
}

ID3D11PixelShader *DX11Shaders::GetModVolPixelShader()
{
	if (!modVolPixelShader)
		MissedPS(ModVolPS, 0, modVolPixelShader);
	return modVolPixelShader;
}

ID3D11VertexShader *DX11Shaders::GetQuadVertexShader()
{
	if (!quadVertexShader)
		MakeVS(QuadVS, 0, quadVertexShader, &quadVertexBlob);
	return quadVertexShader;
}

ID3D11PixelShader *DX11Shaders::GetQuadPixelShader()
{
	if (!quadPixelShader)
		MissedPS(QuadPS, 0, quadPixelShader);
	return quadPixelShader;
}

void DX11Shaders::Term()
{
	TermCompiler();
	for (auto& shader : vertexShaders)
		shader.reset();
	vertexBlob.reset();
	pixelShaders.clear();
	pixelShaders.shrink_to_fit();
	requested.clear();
	for (auto& shader : generalShaders)
		shader.reset();
	Forget();
	modVolVertexShader.reset();
	modVolVertexBlob.reset();
	modVolPixelShader.reset();
	quadVertexShader.reset();
	quadVertexBlob.reset();
	quadPixelShader.reset();
	device = nullptr;
	compile = nullptr;
}
