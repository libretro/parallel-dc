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
/* The Direct3D 12 renderer's pipelines made ahead (rend/dx12/dx12_pso.cpp),
 * on a device: see dx12_pipelines_ahead_test.sh.
 *
 * What the start wants is made by the thread, and a frame finds it made
 * (nothing made by the thread that renders); one the start does not want
 * is made when a frame wants it; a frame that wants them before the
 * thread is done finds every one, each made once; SSAA taken up is made
 * ahead the same way; the keys kept are made ahead the next time (with the
 * device's pipeline library, where it has one); and ending while the
 * thread builds stops it, keeping what it made. Every pipeline is let go:
 * the device, released last, is gone. */
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* (what the table keeps to itself: the thread, to wait for it) */
#define private public
#include "rend/dx12/dx12_pso.h"
#undef private
#include "rend/dx12/dx12_shader_flags.h"
#include "d3d_core_stubs.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); } } while (0)

/* (no NVAPI here) */
bool dx12_nvapi_post_z_available() { return false; }
bool dx12_nvapi_create_post_z_pipeline(ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, ID3D12PipelineState **)
{
	return false;
}

/* The renderer's root signature (DX12Context::Init()) */
static ID3D12RootSignature *rasterRoot(ID3D12Device *device)
{
	D3D12_DESCRIPTOR_RANGE srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, DX12_SRV_TABLE_SIZE, 0, 1, 0 };
	D3D12_DESCRIPTOR_RANGE samplerRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, DX12_SAMPLER_TABLE_SIZE, 0, 1, 0 };
	D3D12_DESCRIPTOR_RANGE oitRanges[3] = {
		{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 1, 0, 0 },
		{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 3 },
		{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 0, 4 },
	};
	D3D12_DESCRIPTOR_RANGE sumsRange = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4, 0, 0 };
	D3D12_ROOT_PARAMETER params[8]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants = { 1, 0, 14 };
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor = { 0, 0 };
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[2].Descriptor = { 2, 0 };
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[3].DescriptorTable = { 1, &srvRange };
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[4].DescriptorTable = { 1, &samplerRange };
	params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[5].DescriptorTable = { 3, oitRanges };
	params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	params[6].Descriptor = { 5, 0 };
	params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[7].DescriptorTable = { 1, &sumsRange };
	params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC statics[2]{};
	for (int i = 0; i < 2; i++)
	{
		D3D12_STATIC_SAMPLER_DESC& s = statics[i];
		s.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		s.MaxAnisotropy = 1;
		s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		s.MaxLOD = D3D12_FLOAT32_MAX;
		s.ShaderRegister = i;
		s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}
	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = 8;
	desc.pParameters = params;
	desc.NumStaticSamplers = 2;
	desc.pStaticSamplers = statics;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob *blob = nullptr, *errors = nullptr;
	ID3D12RootSignature *root = nullptr;
	if (SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
		device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void **)&root);
	else
		printf("root signature: %s\n", errors ? (const char *)errors->GetBufferPointer() : "?");
	if (blob)
		blob->Release();
	if (errors)
		errors->Release();
	return root;
}

/* Every key the table has, found made: nothing more made */
static void findAll(DX12Pipelines& p, const std::vector<u64>& keys)
{
	const u32 made = p.statMade;
	size_t found = 0;
	for (u64 k : keys)
		found += p.Get(DX12PsoKey::Unpack(k)) != nullptr;
	CHECK(found == keys.size());
	CHECK(p.statMade == made);
}

/* One the start does not make: no colour, a stencil test only */
static DX12PsoKey unwanted()
{
	DX12PsoKey k{};
	k.kind = DX12_PSO_POLY;
	k.gouraud = true;
	k.depthTest = true;
	k.depthWrite = true;
	k.depthFunc = 1;
	k.cull = 2;
	return k;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("usage: dx12_pipelines_ahead_test DIR\\\n");
		return 2;
	}
	dataDir = argv[1];
	remove((dataDir + "d3d12_pipelines.bin").c_str());
	ID3D12Device *device = nullptr;
	if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&device)))
	{
		printf("dx12_pipelines_ahead_test: no Direct3D 12 device\n");
		return 2;
	}
	ID3D12RootSignature *root = rasterRoot(device);
	CHECK(root != nullptr);
	if (root == nullptr)
		return 1;
	std::vector<u64> started;

	printf("the start\n");
	{
		DX12Pipelines p;
		p.Init(device, root, 0);
		CHECK(p.warm != nullptr);
		p.MakeAtStart();
		const u32 queued = p.requestedUsed;
		d3d_warm_drain_wait(p.warm);
		CHECK(!d3d_warm_busy(p.warm));
		CHECK(p.statMade == 0);
		/* (taken at a frame's first miss) */
		CHECK(p.Get(unwanted()) != nullptr);
		printf("  %u wanted, %u made ahead, %u in all\n", queued, p.statAhead, p.statMade);
		CHECK(queued > 100);
		CHECK(p.statAhead == queued);
		CHECK(p.statMade == queued + 1 && p.statMadeInFrame == 1);
		started = p.keys;
		findAll(p, started);
		p.Term();
	}

	printf("wanted before the thread is done\n");
	{
		DX12Pipelines p;
		p.Init(device, root, 0);
		p.MakeAtStart();
		size_t found = 0;
		for (u64 k : started)
			found += p.Get(DX12PsoKey::Unpack(k)) != nullptr;
		CHECK(found == started.size());
		d3d_warm_drain_wait(p.warm);
		findAll(p, started);
		/* each made once: those made here as well were let go */
		CHECK(p.used == p.statMade);
		printf("  %u made ahead, %u here\n", p.statAhead, p.statMade - p.statAhead);
		p.Term();
	}

	printf("SSAA\n");
	{
		DX12Pipelines p;
		p.Init(device, root, 0);
		p.MakeAtStart();
		d3d_warm_drain_wait(p.warm);
		CHECK(p.Get(unwanted()) != nullptr);
		const u32 before = p.statMade, ahead = p.statAhead;
		p.MakeForSamples(2);
		d3d_warm_drain_wait(p.warm);
		DX12PsoKey k = DX12PsoKey::Unpack(started[0]);
		k.samples = 2;
		CHECK(p.Get(k) != nullptr);
		printf("  %u more made ahead\n", p.statAhead - ahead);
		CHECK(p.statAhead > ahead);
		CHECK(p.statMade - before == p.statAhead - ahead);
		p.Term();
	}

	printf("the next time\n");
	{
		DX12Pipelines p;
		p.Init(device, root, 0);
		p.MakeAtStart();
		d3d_warm_drain_wait(p.warm);
		CHECK(p.Get(unwanted()) != nullptr);
		/* (the one made in a frame last time, ahead now) */
		CHECK(p.statMadeInFrame == 0);
		printf("  %u made ahead, %u from the library%s\n", p.statAhead, p.statFromLibrary,
				p.library == nullptr ? " (the device has none)" : "");
		if (p.library != nullptr)
			CHECK(p.statFromLibrary > 0);
		p.Term();
	}

	printf("ended while building\n");
	{
		DX12Pipelines p;
		p.Init(device, root, 0);
		p.MakeAtStart();
		p.Term();
		CHECK(p.warm == nullptr && p.slots == nullptr);
	}

	root->Release();
	const ULONG left = device->Release();
	CHECK(left == 0);
	if (failures)
	{
		printf("dx12_pipelines_ahead_test: %d failed\n", failures);
		return 1;
	}
	printf("dx12_pipelines_ahead_test: PASS\n");
	return 0;
}
