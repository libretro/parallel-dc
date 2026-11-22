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
/* The Direct3D 12 renderer's pipelines made ahead (rend/dx12/dx12_pso.c),
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
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>

#include "rend/dx12/dx12_pso.h"
#include "rend/dx12/dx12_shader_flags.h"
#include "rend/dx12/dx12_nvapi.h"
#include "d3d_core_stubs.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); } } while (0)

static const IID test_IID_ID3D12Device = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
static const IID test_IID_ID3D12RootSignature = { 0xc54a6b66, 0x72df, 0x4ee8, { 0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14 } };

/* (no NVAPI here) */
bool dx12_nvapi_post_z_available(void) { return false; }
bool dx12_nvapi_create_post_z_pipeline(ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
		ID3D12PipelineState **out)
{
	(void)device;
	(void)desc;
	(void)out;
	return false;
}

/* The renderer's root signature (dx12c_init()) */
static ID3D12RootSignature *raster_root(ID3D12Device *device)
{
	D3D12_DESCRIPTOR_RANGE srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, DX12_SRV_TABLE_SIZE, 0, 1, 0 };
	D3D12_DESCRIPTOR_RANGE samplerRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, DX12_SAMPLER_TABLE_SIZE, 0, 1, 0 };
	D3D12_DESCRIPTOR_RANGE oitRanges[3] = {
		{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 1, 0, 0 },
		{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 3 },
		{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 0, 4 }
	};
	D3D12_DESCRIPTOR_RANGE sumsRange = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4, 0, 0 };
	D3D12_ROOT_PARAMETER params[8];
	D3D12_STATIC_SAMPLER_DESC statics[2];
	D3D12_ROOT_SIGNATURE_DESC desc;
	ID3DBlob *blob = NULL, *errors = NULL;
	ID3D12RootSignature *root = NULL;
	int i;
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 1;
	params[0].Constants.Num32BitValues = 14;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[2].Descriptor.ShaderRegister = 2;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[3].DescriptorTable.NumDescriptorRanges = 1;
	params[3].DescriptorTable.pDescriptorRanges = &srvRange;
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[4].DescriptorTable.NumDescriptorRanges = 1;
	params[4].DescriptorTable.pDescriptorRanges = &samplerRange;
	params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[5].DescriptorTable.NumDescriptorRanges = 3;
	params[5].DescriptorTable.pDescriptorRanges = oitRanges;
	params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	params[6].Descriptor.ShaderRegister = 5;
	params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[7].DescriptorTable.NumDescriptorRanges = 1;
	params[7].DescriptorTable.pDescriptorRanges = &sumsRange;
	params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	memset(statics, 0, sizeof(statics));
	for (i = 0; i < 2; i++)
	{
		D3D12_STATIC_SAMPLER_DESC *s = &statics[i];
		s->Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		s->AddressU = s->AddressV = s->AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		s->MaxAnisotropy = 1;
		s->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		s->BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		s->MaxLOD = D3D12_FLOAT32_MAX;
		s->ShaderRegister = i;
		s->ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}
	memset(&desc, 0, sizeof(desc));
	desc.NumParameters = 8;
	desc.pParameters = params;
	desc.NumStaticSamplers = 2;
	desc.pStaticSamplers = statics;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	if (SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
		ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
				&test_IID_ID3D12RootSignature, (void **)&root);
	else
		printf("root signature: %s\n", errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
	if (blob)
		ID3D10Blob_Release(blob);
	if (errors)
		ID3D10Blob_Release(errors);
	return root;
}

/* Every key the table has, found made: nothing more made */
static void find_all(const u64 *keys, size_t count)
{
	const u32 made = dx12p.statMade;
	size_t found = 0, i;
	for (i = 0; i < count; i++)
	{
		const DX12PsoKey k = dx12_pso_key_unpack(keys[i]);
		found += dx12_pso_get(&k) != NULL;
	}
	CHECK(found == count);
	CHECK(dx12p.statMade == made);
}

/* One the start does not make: no colour, a stencil test only */
static DX12PsoKey unwanted(void)
{
	DX12PsoKey k;
	memset(&k, 0, sizeof(k));
	k.kind = DX12_PSO_POLY;
	k.gouraud = true;
	k.depthTest = true;
	k.depthWrite = true;
	k.depthFunc = 1;
	k.cull = 2;
	return k;
}

static u64 started[DX12_PSO_MAX_KEYS];
static size_t startedCount;

int main(int argc, char **argv)
{
	char path[1100];
	ID3D12Device *device = NULL;
	ID3D12RootSignature *root;
	DX12PsoKey miss;
	ULONG left;
	if (argc < 2)
	{
		printf("usage: dx12_pipelines_ahead_test DIR\\\n");
		return 2;
	}
	snprintf(dataDir, sizeof(dataDir), "%s", argv[1]);
	snprintf(path, sizeof(path), "%sd3d12_pipelines.bin", dataDir);
	remove(path);
	if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &test_IID_ID3D12Device, (void **)&device)))
	{
		printf("dx12_pipelines_ahead_test: no Direct3D 12 device\n");
		return 2;
	}
	root = raster_root(device);
	CHECK(root != NULL);
	if (root == NULL)
		return 1;
	miss = unwanted();

	printf("the start\n");
	{
		u32 queued;
		dx12_pso_init(device, root, 0);
		CHECK(dx12p.warm != NULL);
		dx12_pso_make_at_start();
		queued = dx12p.requestedUsed;
		d3d_warm_drain_wait(dx12p.warm);
		CHECK(!d3d_warm_busy(dx12p.warm));
		CHECK(dx12p.statMade == 0);
		/* (taken at a frame's first miss) */
		CHECK(dx12_pso_get(&miss) != NULL);
		printf("  %u wanted, %u made ahead, %u in all\n", queued, dx12p.statAhead, dx12p.statMade);
		/* (the quads and the volumes: the renderers' PreparePipelines()
		 * want the polygons') */
		CHECK(queued >= 20);
		CHECK(dx12p.statAhead == queued);
		CHECK(dx12p.statMade == queued + 1 && dx12p.statMadeInFrame == 1);
		startedCount = dx12p.keyCount;
		memcpy(started, dx12p.keys, startedCount * sizeof(u64));
		find_all(started, startedCount);
		dx12_pso_term();
	}

	printf("wanted before the thread is done\n");
	{
		size_t found = 0, i;
		dx12_pso_init(device, root, 0);
		dx12_pso_make_at_start();
		for (i = 0; i < startedCount; i++)
		{
			const DX12PsoKey k = dx12_pso_key_unpack(started[i]);
			found += dx12_pso_get(&k) != NULL;
		}
		CHECK(found == startedCount);
		d3d_warm_drain_wait(dx12p.warm);
		find_all(started, startedCount);
		/* each made once: those made here as well were let go */
		CHECK(dx12p.used == dx12p.statMade);
		printf("  %u made ahead, %u here\n", dx12p.statAhead, dx12p.statMade - dx12p.statAhead);
		dx12_pso_term();
	}

	printf("SSAA\n");
	{
		u32 before, ahead;
		DX12PsoKey k;
		dx12_pso_init(device, root, 0);
		dx12_pso_make_at_start();
		d3d_warm_drain_wait(dx12p.warm);
		CHECK(dx12_pso_get(&miss) != NULL);
		before = dx12p.statMade;
		ahead = dx12p.statAhead;
		dx12_pso_make_for_samples(2, false);
		d3d_warm_drain_wait(dx12p.warm);
		k = dx12_pso_key_unpack(started[0]);
		k.samples = 2;
		CHECK(dx12_pso_get(&k) != NULL);
		printf("  %u more made ahead\n", dx12p.statAhead - ahead);
		CHECK(dx12p.statAhead > ahead);
		CHECK(dx12p.statMade - before == dx12p.statAhead - ahead);
		dx12_pso_term();
	}

	printf("the next time\n");
	{
		dx12_pso_init(device, root, 0);
		dx12_pso_make_at_start();
		d3d_warm_drain_wait(dx12p.warm);
		CHECK(dx12_pso_get(&miss) != NULL);
		/* (the one made in a frame last time, ahead now) */
		CHECK(dx12p.statMadeInFrame == 0);
		printf("  %u made ahead, %u from the library%s\n", dx12p.statAhead, dx12p.statFromLibrary,
				dx12p.library == NULL ? " (the device has none)" : "");
		if (dx12p.library != NULL)
			CHECK(dx12p.statFromLibrary > 0);
		dx12_pso_term();
	}

	printf("ended while building\n");
	{
		dx12_pso_init(device, root, 0);
		dx12_pso_make_at_start();
		dx12_pso_term();
		CHECK(dx12p.warm == NULL && dx12p.slots == NULL);
	}

	ID3D12RootSignature_Release(root);
	left = ID3D12Device_Release(device);
	CHECK(left == 0);
	if (failures)
	{
		printf("dx12_pipelines_ahead_test: %d failed\n", failures);
		return 1;
	}
	printf("dx12_pipelines_ahead_test: PASS\n");
	return 0;
}
