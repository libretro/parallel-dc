/*
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
/* The Direct3D 11 renderers' shaders made ahead and kept on disk
 * (rend/dx11/dx11_shaders.cpp, rend/dx11/oit/dx11_oitshaders.cpp), on a
 * device, with d3dcompiler_47.dll: see dx11_shaders_ahead_test.sh.
 *
 * A first run, with no file: every shader is made, those ahead by the
 * thread, and a file written. A second: nothing is compiled - the vertex
 * shaders come from the file, the pixel shaders the file has are all made
 * ahead - and each one's bytecode is what compiling it makes. A file that
 * is not one (a byte changed) is not used, and is written again right; a
 * file whose shaders were compiled from another source has them compiled
 * again. A device for one thread only gets no thread, and still the file.
 * Each for both renderers. */
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

/* (what the renderer keeps to itself: the thread, to wait for it) */
#define private public
#define protected public
#include "rend/dx11/dx11_shaders.h"
#include "rend/dx11/oit/dx11_oitshaders.h"
#undef private
#undef protected

static int failures, skipped;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); } } while (0)

#include "d3d_core_stubs.h"

/* ---- the test ---- */

static pD3DCompile compileFn;

static std::vector<u8> readFile(const std::string& path)
{
	std::vector<u8> v;
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return v;
	int c;
	while ((c = fgetc(f)) != EOF)
		v.push_back((u8)c);
	fclose(f);
	return v;
}

static void writeFile(const std::string& path, const std::vector<u8>& v)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (f != nullptr)
	{
		fwrite(v.data(), 1, v.size(), f);
		fclose(f);
	}
}

/* Every entry's bytecode is what compiling what it says makes */
static void checkFile(DX11ShaderCompiler& s, const std::string& path, size_t expect)
{
	std::vector<u8> f = readFile(path);
	d3d_shcache_entry_t *e = nullptr;
	size_t count = 0, at = 0;
	CHECK(d3d_shcache_parse(f.data(), f.size(), &e, &count, &at));
	CHECK(count == expect);
	for (size_t i = 0; i < count; i++)
	{
		DX11ShaderDesc d;
		CHECK(s.Describe(e[i].family, e[i].variant, d));
		CHECK(DX11ShaderCompiler::HashOf(d) == e[i].hash);
		ComPtr<ID3DBlob> b = s.Compile(d.source.c_str(), "main", d.profile, d.macros);
		CHECK(b && b->GetBufferSize() == e[i].size
				&& memcmp(b->GetBufferPointer(), &f[at + e[i].offset], e[i].size) == 0);
	}
	free(e);
}

static ID3D11Device *newDevice(UINT flags)
{
	ID3D11Device *device = nullptr;
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &level, 1,
			D3D11_SDK_VERSION, &device, nullptr, nullptr)))
		return nullptr;
	return device;
}

/* What a game would want of the per-triangle renderer's */
static size_t useTriangle(DX11Shaders& s)
{
	size_t made = 0;
	made += s.GetVertexShader(false) != nullptr;
	made += s.GetVertexShader(true) != nullptr;
	made += s.GetModVolVertexShader() != nullptr;
	made += s.GetQuadVertexShader() != nullptr;
	made += s.GetModVolPixelShader() != nullptr;
	made += s.GetQuadPixelShader() != nullptr;
	for (u32 v = 0; v < 6; v++)
	{
		DX11FragmentShaderParams p = {};
		p.texture = (v & 1) != 0;
		p.useAlpha = (v & 2) != 0;
		p.shaderInstr = (int)(v % 4);
		p.fog = (int)(v % 3);
		p.gouraud = v >= 3;
		p.shadowed = v == 5;
		made += s.GetPixelShader(p) != nullptr;
	}
	CHECK(s.GetVertexShaderBlob() != nullptr && s.GetModVolVertexShaderBlob() != nullptr && s.GetQuadVertexShaderBlob() != nullptr);
	return made;
}
static const size_t TriangleShaders = 12;	// (VS 2, mod. vol. 2, quad 2, PS 6)

static size_t useOIT(DX11OITShaders& s)
{
	size_t made = 0;
	made += s.GetVertexShader(false) != nullptr;
	made += s.GetVertexShader(true) != nullptr;
	made += s.GetModVolVertexShader() != nullptr;
	made += s.GetFinalVertexShader() != nullptr;
	made += s.GetModVolPixelShader() != nullptr;
	made += s.GetFinalPixelShader() != nullptr;
	for (int m = 0; m < 4; m++)
		made += s.GetTrModVolPixelShader((DX11OITShaders::ModVolMode)m) != nullptr;
	for (u32 v = 0; v < 6; v++)
	{
		DX11OITShaders::FragmentShaderParams p = {};
		p.texture = (v & 1) != 0;
		p.twoVolume = (v & 2) != 0;
		p.fog = (int)(v % 3);
		p.pass = (DX11OITShaders::Pass)(v % 3);
		made += s.GetPixelShader(p) != nullptr;
	}
	return made;
}
static const size_t OITShaders = 16;	// (VS 2, mod. vol. 2, final 2, tr. mod. vol. 4, PS 6)

/* The checksum made right again for what was changed */
static void resign(std::vector<u8>& f)
{
	const u64 h = d3d_shcache_hash(D3D_SHCACHE_HASH_INIT, f.data() + 32, f.size() - 32);
	for (int i = 0; i < 8; i++)
		f[24 + i] = (u8)(h >> (8 * i));
}

/* @total shaders used, @fixed pixel ones of a kind, @pixel of them all */
template<typename S>
static void run(const char *name, size_t (*use)(S&), size_t total, size_t fixed, size_t pixel)
{
	printf("%s\n", name);
	S probe;
	const std::string file = dataDir + probe.FileName();
	/* (wine's compiler does not compile the per-pixel renderer's) */
	{
		DX11ShaderDesc d;
		std::vector<d3d_warm_job_t> jobs;
		probe.compile = compileFn;
		probe.Fixed(jobs);
		bool all = true;
		for (const d3d_warm_job_t& j : jobs)
			all = all && probe.Describe(j.kind, j.aux, d) && probe.Compile(d.source.c_str(), "main", d.profile, d.macros);
		if (!all)
		{
			printf("  (this compiler does not compile its shaders: not tested)\n");
			skipped++;
			return;
		}
	}
	remove(file.c_str());
	ID3D11Device *device = newDevice(0);
	CHECK(device != nullptr);
	if (device == nullptr)
		return;
	probe.device = device;
	probe.compile = compileFn;

	/* no file: all compiled; those of a kind, ahead */
	{
		S s;
		s.Init(device, compileFn);
		CHECK(s.warm != nullptr);
		d3d_warm_drain_wait(s.warm);
		CHECK(use(s) == total);
		CHECK(s.statAhead == fixed && s.statCompiledAhead == fixed);
		CHECK(s.statCompiled == total - fixed && s.statFromFile == 0);
		s.Term();
	}
	checkFile(probe, file, total);
	const std::vector<u8> good = readFile(file);

	/* the file: nothing compiled, every pixel shader ahead, the file as it was */
	{
		S s;
		s.Init(device, compileFn);
		d3d_warm_drain_wait(s.warm);
		CHECK(use(s) == total);
		CHECK(s.statCompiled == 0 && s.statCompiledAhead == 0);
		CHECK(s.statAhead == pixel && s.statFromFile == total - pixel);
		s.Term();
	}
	CHECK(readFile(file) == good);

	/* wanted before the thread is done: the same shaders, still nothing compiled */
	{
		S s;
		s.Init(device, compileFn);
		CHECK(use(s) == total);
		CHECK(s.statCompiled == 0);
		CHECK(s.statAhead + s.statFromFile >= total - pixel);
		s.Term();
	}
	CHECK(readFile(file) == good);

	/* not a file (a byte changed): not used, and written again */
	{
		std::vector<u8> bad = good;
		bad[bad.size() - 7] ^= 0x40;
		writeFile(file, bad);
		S s;
		s.Init(device, compileFn);
		d3d_warm_drain_wait(s.warm);
		CHECK(use(s) == total);
		CHECK(s.statCompiled == total - fixed && s.statFromFile == 0);
		s.Term();
	}
	checkFile(probe, file, total);

	/* one compiled from another source: compiled again (ahead), and replaced */
	{
		std::vector<u8> stale = readFile(file);
		d3d_shcache_entry_t *e = nullptr;
		size_t count = 0, at = 0, which = 0;
		const bool parsed = d3d_shcache_parse(stale.data(), stale.size(), &e, &count, &at);
		CHECK(parsed);
		while (which < count && !probe.PixelFamily(e[which].family))
			which++;
		CHECK(which < count);
		free(e);
		if (!parsed || which == count)
			return;
		stale[32 + which * 24] ^= 1;		// (its hash)
		resign(stale);
		writeFile(file, stale);
		S s;
		s.Init(device, compileFn);
		d3d_warm_drain_wait(s.warm);
		CHECK(use(s) == total);
		CHECK(s.statCompiled == 0 && s.statCompiledAhead == 1 && s.statAhead == pixel);
		s.Term();
	}
	checkFile(probe, file, total);

	/* a device for one thread only: no thread, the file still (a device
	 * that does not say it is one - wine's - is not known to be) */
	{
		ID3D11Device *one = newDevice(D3D11_CREATE_DEVICE_SINGLETHREADED);
		CHECK(one != nullptr);
		if (one != nullptr && !(one->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED))
		{
			printf("  (the device does not say it is for one thread: not tested)\n");
			one->Release();
			one = nullptr;
		}
		if (one != nullptr)
		{
			S s;
			s.Init(one, compileFn);
			CHECK(s.warm == nullptr);
			CHECK(use(s) == total);
			CHECK(s.statCompiled == 0 && s.statAhead == 0 && s.statFromFile == total);
			s.Term();
			one->Release();
		}
	}
	probe.device = nullptr;
	device->Release();
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("usage: dx11_shaders_ahead_test DIR\\ [COMPILER.DLL]\n");
		return 2;
	}
	dataDir = argv[1];
	/* (another compiler than d3dcompiler_47.dll: one with D3DCompile) */
	HMODULE dll = LoadLibraryA(argc > 2 ? argv[2] : "d3dcompiler_47.dll");
	compileFn = dll != nullptr ? (pD3DCompile)(void *)GetProcAddress(dll, "D3DCompile") : nullptr;
	if (compileFn == nullptr)
	{
		printf("dx11_shaders_ahead_test: no d3dcompiler_47.dll\n");
		return 2;
	}
	ID3D11Device *device = newDevice(0);
	if (device == nullptr)
	{
		printf("dx11_shaders_ahead_test: no Direct3D 11 device of feature level 11.0\n");
		return 2;
	}
	device->Release();
	run<DX11Shaders>("per triangle", useTriangle, TriangleShaders, 2, 8);
	run<DX11OITShaders>("per pixel", useOIT, OITShaders, 6, 12);
	if (failures)
	{
		printf("dx11_shaders_ahead_test: %d failed\n", failures);
		return 1;
	}
	printf("dx11_shaders_ahead_test: PASS%s\n", skipped ? " (not all tested)" : "");
	return 0;
}
