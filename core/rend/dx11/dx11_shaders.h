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
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <unordered_map>
#include <string>
#include <vector>
#include "types.h"
#include "comptr.h"
#include "rend/d3d_warm.h"
#include "rend/d3d_shader_cache.h"

/* What a shader is compiled from: its source, profile and macros (a list
 * ending with a null one). One function says it for each shader, for the
 * thread that renders and the one that makes them ahead alike. */
struct DX11ShaderDesc
{
	std::string source;
	const char *profile;
	D3D_SHADER_MACRO macros[16];
};

/* The shaders, in HLSL, compiled the first time each one is wanted with the
 * compiler the frontend hands over (or d3dcompiler_47.dll's), and kept.
 *
 * Their bytecode is kept on disk too (rend/d3d_shader_cache.h), whatever
 * the driver: a shader compiled once is not compiled again. And the pixel
 * shaders the file has are made ahead, on a thread of their own
 * (rend/d3d_warm.h), when the device can be used from another thread: a
 * frame that wants one takes it from there, or makes it itself if it is
 * not there yet - nothing is drawn without its shader. The vertex shaders
 * are wanted as the renderer starts (for its input layouts): made there,
 * from the file. */
class DX11ShaderCompiler
{
public:
	virtual ~DX11ShaderCompiler() {}
	void Init(ID3D11Device *device, pD3DCompile compile);
	/* (an array of macros ending with a null one, or null) */
	ComPtr<ID3DBlob> Compile(const char *source, const char *entry, const char *profile, const D3D_SHADER_MACRO *macros,
			ID3DInclude *include = nullptr);
	ComPtr<ID3D11VertexShader> CompileVS(const char *source, const char *entry, const char *profile, const D3D_SHADER_MACRO *macros,
			ComPtr<ID3DBlob> *blob = nullptr, ID3DInclude *include = nullptr);
	ComPtr<ID3D11PixelShader> CompilePS(const char *source, const char *entry, const char *profile, const D3D_SHADER_MACRO *macros,
			ID3DInclude *include = nullptr);

	// what has been done so far, for measuring
	u32 statCompiled = 0;		// compiled by the thread that renders
	u32 statFromFile = 0;		// ...or found in the file
	u32 statAhead = 0;			// made by the thread, and taken
	u32 statCompiledAhead = 0;	// of them, compiled (not in the file)

protected:
	ID3D11Device *device = nullptr;
	pD3DCompile compile = nullptr;

	/* What @family's @variant is compiled from (both threads) */
	virtual bool Describe(u32 family, u32 variant, DX11ShaderDesc& d) const = 0;
	/* A pixel shader the thread made, kept if its place is empty */
	virtual void Adopt(u32 family, u32 variant, ID3D11PixelShader *shader) = 0;
	virtual const char *FileName() const = 0;
	/* The families of pixel shaders, made ahead; the shaders that are one
	 * of a kind, made ahead whether or not the file has them */
	virtual bool PixelFamily(u32 family) const = 0;
	virtual void Fixed(std::vector<d3d_warm_job_t>& jobs) const = 0;

	/* A pixel shader not there: the thread's, else made here */
	ID3D11PixelShader *MissedPS(u32 family, u32 variant, ComPtr<ID3D11PixelShader>& slot);
	/* A vertex shader, made here, and its bytecode for an input layout */
	ID3D11VertexShader *MakeVS(u32 family, u32 variant, ComPtr<ID3D11VertexShader>& slot, ComPtr<ID3DBlob> *blob);
	/* The thread stopped, what it made taken, the file saved */
	void TermCompiler();

	/* The pixel shaders last looked up, by their key: a polygon's is
	 * looked up for every polygon, and is most often one of a few */
	struct Memo
	{
		u32 key;
		ID3D11PixelShader *shader;
	};
	static const u32 MemoSize = 64;
	Memo memo[MemoSize] = {};
	Memo& MemoFor(u32 key) { return memo[(key ^ (key >> 6) ^ (key >> 12)) & (MemoSize - 1)]; }
	void Forget()
	{
		for (Memo& m : memo)
			m = Memo();
	}

private:
	static bool WarmBuild(void *ctx, const d3d_warm_job_t *job, d3d_warm_result_t *res);
	static void WarmRelease(void *ctx, d3d_warm_result_t *res);
	static u64 HashOf(const DX11ShaderDesc& d);
	/* The file's bytecode of @hash (both threads: the file is not changed
	 * while the thread runs) */
	const u8 *FromFile(u64 hash, u32 *size) const;
	/* The bytecode of @d: the file's, this run's, or compiled now (and kept) */
	bool Bytecode(u32 family, u32 variant, const DX11ShaderDesc& d, ComPtr<ID3DBlob>& hold, const void **code, size_t *size, bool *fromFile);
	void Keep(u64 hash, u32 family, u32 variant, const void *code, size_t size);
	void TakeBuilt();
	void LoadFile();
	void SaveFile();

	d3d_warm_t *warm = nullptr;
	std::vector<u8> file;						// the file read, its blobs from fileBlobs
	std::vector<d3d_shcache_entry_t> fileIndex;	// sorted by hash
	size_t fileBlobs = 0;
	struct Kept
	{
		u64 hash;
		u32 family;
		u32 variant;
		std::vector<u8> code;
	};
	std::vector<Kept> kept;						// compiled this run
	std::unordered_map<u64, size_t> keptByHash;
	bool fileChanged = false;
};

/* What a polygon's pixel shader is made for: as the Vulkan renderer's */
struct DX11FragmentShaderParams
{
	bool alphaTest;
	bool insideClipTest;
	bool useAlpha;
	bool texture;
	bool ignoreTexAlpha;
	int shaderInstr;
	bool offset;
	int fog;
	bool gouraud;
	bool bumpmap;
	bool clamping;
	bool trilinear;
	bool palette;
	bool shadowed;	// drawn a second time where it is in a modifier volume: rend/shadows.h

	u32 hash() const
	{
		return (u32)alphaTest | ((u32)insideClipTest << 1) | ((u32)useAlpha << 2)
			| ((u32)texture << 3) | ((u32)ignoreTexAlpha << 4) | ((u32)shaderInstr << 5)
			| ((u32)offset << 7) | ((u32)fog << 8) | ((u32)gouraud << 10)
			| ((u32)bumpmap << 11) | ((u32)clamping << 12) | ((u32)trilinear << 13)
			| ((u32)palette << 14) | ((u32)shadowed << 15);
	}
};

class DX11Shaders : public DX11ShaderCompiler
{
public:
	/* Their families (DX11ShaderCompiler) */
	enum { VS, PS, ModVolVS, ModVolPS, QuadVS, QuadPS };
	void Term();

	ID3D11VertexShader *GetVertexShader(bool gouraud);
	ID3D11PixelShader *GetPixelShader(const DX11FragmentShaderParams& params);
	ID3D11VertexShader *GetModVolVertexShader();
	ID3D11PixelShader *GetModVolPixelShader();
	ID3D11VertexShader *GetQuadVertexShader();
	ID3D11PixelShader *GetQuadPixelShader();

	/* For the input layouts: what the vertex shaders take */
	ID3DBlob *GetVertexShaderBlob() { GetVertexShader(true); return vertexBlob; }
	ID3DBlob *GetModVolVertexShaderBlob() { GetModVolVertexShader(); return modVolVertexBlob; }
	ID3DBlob *GetQuadVertexShaderBlob() { GetQuadVertexShader(); return quadVertexBlob; }

private:
	ComPtr<ID3D11VertexShader> vertexShaders[2];
	ComPtr<ID3DBlob> vertexBlob;
	std::unordered_map<u32, ComPtr<ID3D11PixelShader>> pixelShaders;
	ComPtr<ID3D11VertexShader> modVolVertexShader;
	ComPtr<ID3DBlob> modVolVertexBlob;
	ComPtr<ID3D11PixelShader> modVolPixelShader;
	ComPtr<ID3D11VertexShader> quadVertexShader;
	ComPtr<ID3DBlob> quadVertexBlob;
	ComPtr<ID3D11PixelShader> quadPixelShader;

protected:
	bool Describe(u32 family, u32 variant, DX11ShaderDesc& d) const override;
	void Adopt(u32 family, u32 variant, ID3D11PixelShader *shader) override;
	const char *FileName() const override { return "d3d11_shaders.bin"; }
	bool PixelFamily(u32 family) const override { return family == PS || family == ModVolPS || family == QuadPS; }
	void Fixed(std::vector<d3d_warm_job_t>& jobs) const override;
};

/* The HLSL the per-pixel renderer shares with the other: the vertex
 * transform and the fog table */
extern const char * const DX11VertexTransform;
extern const char * const DX11FogFunction;
extern const char * const DX11ModVolVertexShader;
