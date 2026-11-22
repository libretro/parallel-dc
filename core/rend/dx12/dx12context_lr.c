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
#include "dx12context_lr.h"
#include <stdlib.h>
#include "rend/hdr/hdr_output.h"
#include "rend/dxgi_budget.h"
#include "rend/ssaa_pattern.h"

dx12_context dx12c;

typedef HRESULT (WINAPI *SerializeRootSignature_t)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *CreateDXGIFactory1_t)(REFIID, void **);

/* (the interfaces' ids, here: nothing linked for them) */
static const IID iid_descriptor_heap = { 0x8efb471d, 0x616c, 0x4f49, { 0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51 } };
static const IID iid_resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
static const IID iid_fence = { 0x0a753dcf, 0xc4d8, 0x4b91, { 0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76 } };
static const IID iid_command_allocator = { 0x6102dee4, 0xaf59, 0x4b09, { 0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24 } };
static const IID iid_graphics_command_list = { 0x5b160d0f, 0xac1b, 0x4185, { 0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55 } };
static const IID iid_root_signature = { 0xc54a6b66, 0x72df, 0x4ee8, { 0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14 } };
static const IID iid_command_signature = { 0xc36a797c, 0xec80, 0x4f0a, { 0x89, 0x85, 0xa7, 0xb2, 0x47, 0x50, 0x82, 0xd1 } };
static const IID iid_dxgi_factory4 = { 0x1bc6ea02, 0xef36, 0x464f, { 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a } };
static const IID iid_dxgi_adapter = { 0x2411e7e1, 0x12ac, 0x4ccf, { 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0 } };

static const DXGI_FORMAT textureFormats[5] = {
	DXGI_FORMAT_B5G6R5_UNORM,		/* TEX_TYPE_565 */
	DXGI_FORMAT_B5G5R5A1_UNORM,		/* TEX_TYPE_5551 */
	DXGI_FORMAT_B4G4R4A4_UNORM,		/* TEX_TYPE_4444 */
	DXGI_FORMAT_R8G8B8A8_UNORM,		/* TEX_TYPE_8888 */
	DXGI_FORMAT_R8_UNORM,			/* TEX_TYPE_8 */
};

#define MIN_U64(a, b) ((a) < (b) ? (a) : (b))
#define MAX_U64(a, b) ((a) > (b) ? (a) : (b))

/* ---- The struct returns, called as Microsoft's ABI has them: an out pointer ---- */

static D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu_start(ID3D12DescriptorHeap *heap)
{
	D3D12_CPU_DESCRIPTOR_HANDLE out;
	((void (STDMETHODCALLTYPE *)(ID3D12DescriptorHeap *, D3D12_CPU_DESCRIPTOR_HANDLE *))
		heap->lpVtbl->GetCPUDescriptorHandleForHeapStart)(heap, &out);
	return out;
}

static D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu_start(ID3D12DescriptorHeap *heap)
{
	D3D12_GPU_DESCRIPTOR_HANDLE out;
	((void (STDMETHODCALLTYPE *)(ID3D12DescriptorHeap *, D3D12_GPU_DESCRIPTOR_HANDLE *))
		heap->lpVtbl->GetGPUDescriptorHandleForHeapStart)(heap, &out);
	return out;
}

static LUID device_luid(ID3D12Device *device)
{
	LUID out;
	((void (STDMETHODCALLTYPE *)(ID3D12Device *, LUID *))device->lpVtbl->GetAdapterLuid)(device, &out);
	return out;
}

D3D12_RESOURCE_DESC dx12_resource_desc(ID3D12Resource *resource)
{
	D3D12_RESOURCE_DESC out;
	((void (STDMETHODCALLTYPE *)(ID3D12Resource *, D3D12_RESOURCE_DESC *))resource->lpVtbl->GetDesc)(resource, &out);
	return out;
}

/* ---- Descriptor heaps ---- */

bool dx12_heap_init(dx12_descriptor_heap *h, ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 count,
		bool shaderVisible, u32 reserved)
{
	D3D12_DESCRIPTOR_HEAP_DESC desc;
	dx12_heap_term(h);
	memset(&desc, 0, sizeof(desc));
	desc.Type = type;
	desc.NumDescriptors = count;
	desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &desc, &iid_descriptor_heap, (void **)&h->heap)))
	{
		h->heap = NULL;
		return false;
	}
	h->free_list = (u32 *)malloc(count * sizeof(u32));
	if (h->free_list == NULL)
	{
		dx12_heap_term(h);
		return false;
	}
	h->cpu_start = heap_cpu_start(h->heap);
	if (shaderVisible)
		h->gpu_start = heap_gpu_start(h->heap);
	h->increment = ID3D12Device_GetDescriptorHandleIncrementSize(device, type);
	h->capacity = count;
	dx12_heap_range(h, reserved, count);
	return true;
}

void dx12_heap_range(dx12_descriptor_heap *h, u32 first, u32 end)
{
	u32 i;
	h->free_count = 0;
	/* (the lowest first) */
	for (i = end; i > first; i--)
		h->free_list[h->free_count++] = i - 1;
}

void dx12_heap_term(dx12_descriptor_heap *h)
{
	free(h->free_list);
	h->free_list = NULL;
	h->free_count = h->capacity = 0;
	DX12_RELEASE(h->heap);
	h->cpu_start.ptr = 0;
	h->gpu_start.ptr = 0;
}

/* ---- The context ---- */

bool dx12_make_texture(ID3D12Device *device, u32 width, u32 height, u32 levels, DXGI_FORMAT format,
		D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ID3D12Resource **out, const D3D12_CLEAR_VALUE *clear)
{
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC desc;
	DX12_RELEASE(*out);
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = (UINT16)levels;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = flags;
	if (FAILED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear,
			&iid_resource, (void **)out)))
	{
		WARN_LOG(RENDERER, "DX12: texture creation failed (%d x %d, %d levels, format %d)\n", width, height, levels, (int)format);
		*out = NULL;
		return false;
	}
	return true;
}

static bool format_support(DXGI_FORMAT format, D3D12_FEATURE_DATA_FORMAT_SUPPORT *support)
{
	memset(support, 0, sizeof(*support));
	support->Format = format;
	return dx12c.device != NULL
			&& SUCCEEDED(ID3D12Device_CheckFeatureSupport(dx12c.device, D3D12_FEATURE_FORMAT_SUPPORT, support, sizeof(*support)));
}

bool dx12c_can_render_to(DXGI_FORMAT format)
{
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support;
	return format_support(format, &support) && (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0;
}

bool dx12c_can_write_uav(DXGI_FORMAT format)
{
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support;
	return format_support(format, &support)
			&& (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0
			&& (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

u32 dx12c_ssaa_samples(u32 requested, u32 perSampleKib, u32 fixedKib, const char *what)
{
	static const DXGI_FORMAT formats[2] = { DX12FrameFormat, DX12DepthFormat };
	u32 counts = 1, n, deviceSamples, samples;
	int f;
	if (requested <= 1 || dx12c.device == NULL)
		return 1;
	/* the counts the frame's colour and depth can both be drawn with, in the standard pattern */
	for (n = 2; n <= SSAA_MAX_SAMPLES; n <<= 1)
	{
		bool ok = true;
		for (f = 0; f < 2; f++)
		{
			D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels;
			memset(&levels, 0, sizeof(levels));
			levels.Format = formats[f];
			levels.SampleCount = n;
			ok = ok && SUCCEEDED(ID3D12Device_CheckFeatureSupport(dx12c.device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
					&levels, sizeof(levels)))
					&& levels.NumQualityLevels > 0;
		}
		if (ok)
			counts |= n;
	}
	deviceSamples = ssaa_device_samples(requested, counts);
	samples = ssaa_clamp_samples(deviceSamples, vram_budget_available_kib(&dx12c.vram_budget), fixedKib, perSampleKib);
	NOTICE_LOG(RENDERER, "%s: SSAA %u samples a pixel (asked %u, the device can draw %u; %u MB a sample, %u MB of the budget left)\n",
			what, samples, requested, deviceSamples, perSampleKib >> 10, vram_budget_available_kib(&dx12c.vram_budget) >> 10);
	return samples;
}

static bool make_root_signature(SerializeRootSignature_t serialize, const D3D12_ROOT_SIGNATURE_DESC *desc, ID3D12RootSignature **out)
{
	ID3DBlob *blob = NULL, *errors = NULL;
	bool ok = SUCCEEDED(serialize(desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))
			&& SUCCEEDED(ID3D12Device_CreateRootSignature(dx12c.device, 0, ID3D10Blob_GetBufferPointer(blob),
					ID3D10Blob_GetBufferSize(blob), &iid_root_signature, (void **)out));
	if (!ok)
	{
		ERROR_LOG(RENDERER, "DX12: root signature: %s\n", errors != NULL ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "");
		*out = NULL;
	}
	DX12_RELEASE(blob);
	DX12_RELEASE(errors);
	return ok;
}

static void range(D3D12_DESCRIPTOR_RANGE *r, D3D12_DESCRIPTOR_RANGE_TYPE type, UINT count, UINT reg, UINT space, UINT offset)
{
	r->RangeType = type;
	r->NumDescriptors = count;
	r->BaseShaderRegister = reg;
	r->RegisterSpace = space;
	r->OffsetInDescriptorsFromTableStart = offset;
}

static bool make_root_signatures(void)
{
	HMODULE lib = GetModuleHandleA("d3d12.dll");
	SerializeRootSignature_t serialize;
	D3D12_DESCRIPTOR_RANGE srvRange, samplerRange, oitRanges[3], sumsRange, uavRange;
	D3D12_ROOT_PARAMETER params[8], kparams[3];
	D3D12_STATIC_SAMPLER_DESC statics[2];
	D3D12_ROOT_SIGNATURE_DESC desc, kdesc;
	D3D12_INDIRECT_ARGUMENT_DESC args[2];
	D3D12_COMMAND_SIGNATURE_DESC sig;
	int i;

	if (lib == NULL)
		lib = dx12c.d3d12_module = LoadLibraryA("d3d12.dll");
	serialize = lib != NULL ? (SerializeRootSignature_t)GetProcAddress(lib, "D3D12SerializeRootSignature") : NULL;
	if (serialize == NULL)
		return false;

	/* the polygons', the modifier volumes', the quads', and the per-pixel
	 * renderer's (see shaders/dx12_raster.hlsl): its lists, heads and
	 * counter (u1 to u3), the stencil (t4) and the depth (t6) are one table
	 * at DX12C_OIT_FIRST of the heap, its translucent polygons' parameters
	 * (t5) a root descriptor; and with the HDR output and SSAA, the channel
	 * sums its resolve stores (u4) a table of their own, at DX12C_OIT_SUMS_SLOT */
	range(&srvRange, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, DX12_SRV_TABLE_SIZE, 0, 1, 0);
	range(&samplerRange, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, DX12_SAMPLER_TABLE_SIZE, 0, 1, 0);
	range(&oitRanges[0], D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 1, 0, 0);
	range(&oitRanges[1], D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 3);
	range(&oitRanges[2], D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 0, 4);
	range(&sumsRange, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4, 0, 0);
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 1;
	params[0].Constants.RegisterSpace = 0;
	params[0].Constants.Num32BitValues = DX12C_POLY_CONSTANT_COUNT;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor.ShaderRegister = 0;
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
		s->RegisterSpace = 0;
		s->ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}
	memset(&desc, 0, sizeof(desc));
	desc.NumParameters = 8;
	desc.pParameters = params;
	desc.NumStaticSamplers = 2;
	desc.pStaticSamplers = statics;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	if (!make_root_signature(serialize, &desc, &dx12c.raster_root))
		return false;

	/* the texture kernels' (rend/texdecode.h: b0, t0 the ring, u0 the level) */
	range(&uavRange, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0);
	memset(kparams, 0, sizeof(kparams));
	kparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	kparams[0].Constants.ShaderRegister = 0;
	kparams[0].Constants.RegisterSpace = 0;
	kparams[0].Constants.Num32BitValues = 8;
	kparams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	kparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	kparams[1].Descriptor.ShaderRegister = 0;
	kparams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	kparams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	kparams[2].DescriptorTable.NumDescriptorRanges = 1;
	kparams[2].DescriptorTable.pDescriptorRanges = &uavRange;
	kparams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	memset(&kdesc, 0, sizeof(kdesc));
	kdesc.NumParameters = 3;
	kdesc.pParameters = kparams;
	if (!make_root_signature(serialize, &kdesc, &dx12c.decode_root))
		return false;

	/* Draws that set the polygon's root constants each, recorded as one
	 * ExecuteIndirect (the per-triangle renderer's batches). A device that
	 * cannot has its draws recorded one by one. */
	memset(args, 0, sizeof(args));
	args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
	args[0].Constant.RootParameterIndex = 0;
	args[0].Constant.DestOffsetIn32BitValues = 0;
	args[0].Constant.Num32BitValuesToSet = DX12C_POLY_CONSTANT_COUNT;
	args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
	memset(&sig, 0, sizeof(sig));
	sig.ByteStride = DX12C_POLY_CONSTANT_COUNT * 4 + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	sig.NumArgumentDescs = 2;
	sig.pArgumentDescs = args;
	if (FAILED(ID3D12Device_CreateCommandSignature(dx12c.device, &sig, dx12c.raster_root, &iid_command_signature,
			(void **)&dx12c.draw_signature)))
	{
		INFO_LOG(RENDERER, "DX12: no ExecuteIndirect of root constants on this device: draws recorded one by one\n");
		dx12c.draw_signature = NULL;
	}
	return true;
}

void dx12c_make_srv(ID3D12Resource *resource, DXGI_FORMAT format, u32 levels, u32 index)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Format = format;
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.Texture2D.MipLevels = levels;
	ID3D12Device_CreateShaderResourceView(dx12c.device, resource, &desc, dx12_heap_cpu(&dx12c.srv_heap, index));
}

static void reclaim_released(u64 done)
{
	while (dx12c.released_count != 0 && dx12c.released[dx12c.released_first].fence <= done)
	{
		dx12_released *r = &dx12c.released[dx12c.released_first];
		if (r->object != NULL)
			IUnknown_Release(r->object);
		else
			dx12_heap_free(&dx12c.srv_heap, r->srv);
		dx12c.released_first = (dx12c.released_first + 1) % DX12C_RELEASES;
		dx12c.released_count--;
	}
}

bool dx12c_init(const struct retro_hw_render_interface_d3d12 *iface)
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS options;
	UINT vendor = 0;
	bool ok;
	u32 i;
	int t;

	dx12c_term();
	if (iface == NULL || iface->device == NULL || iface->queue == NULL)
		return false;
	ID3D12Device_AddRef(iface->device);
	dx12c.device = iface->device;
	ID3D12CommandQueue_AddRef(iface->queue);
	dx12c.queue = iface->queue;
	dx12c.required_state = iface->required_state;
	dx12c.handle = iface->handle;
	dx12c.set_texture = iface->set_texture;
	dx12c.frame_format = DX12FrameFormat;
	if (iface->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2
			&& iface->get_sync_index != NULL && iface->get_sync_index_mask != NULL
			&& iface->wait_sync_index != NULL && iface->set_texture_fenced != NULL)
	{
		dx12c.v2 = true;
		dx12c.get_sync_index = iface->get_sync_index;
		dx12c.get_sync_index_mask = iface->get_sync_index_mask;
		dx12c.wait_sync_index = iface->wait_sync_index;
		dx12c.set_texture_fenced = iface->set_texture_fenced;
	}
	else if (dx12c.set_texture == NULL)
	{
		ERROR_LOG(RENDERER, "DX12: the frontend's interface has no set_texture\n");
		dx12c_term();
		return false;
	}

	memset(&options, 0, sizeof(options));
	if (FAILED(ID3D12Device_CheckFeatureSupport(dx12c.device, D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))
			|| options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
	{
		/* (tier 1 has no table of thousands of textures: the Direct3D 11
		 * renderer is for those devices) */
		ERROR_LOG(RENDERER, "DX12: resource binding tier 2 is needed (the device has %d)\n", (int)options.ResourceBindingTier);
		dx12c_term();
		return false;
	}
	for (t = 0; t < 5; t++)
	{
		D3D12_FEATURE_DATA_FORMAT_SUPPORT support;
		const UINT want = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
		dx12c.format_supported[t] = format_support(textureFormats[t], &support) && (support.Support1 & want) == want;
	}

	/* the adapter's budget (rend/dxgi_budget.h), found by its LUID */
	{
		HMODULE dxgi = LoadLibraryA("dxgi.dll");
		CreateDXGIFactory1_t createFactory = dxgi != NULL ? (CreateDXGIFactory1_t)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
		IDXGIFactory4 *factory = NULL;
		memset(&dx12c.vram_budget, 0, sizeof(dx12c.vram_budget));
		if (createFactory != NULL && SUCCEEDED(createFactory(&iid_dxgi_factory4, (void **)&factory)))
		{
			IDXGIAdapter *adapter = NULL;
			if (SUCCEEDED(IDXGIFactory4_EnumAdapterByLuid(factory, device_luid(dx12c.device), &iid_dxgi_adapter, (void **)&adapter)))
			{
				DXGI_ADAPTER_DESC desc;
				const bool haveDesc = SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc));
				if (haveDesc)
					vendor = desc.VendorId;
				dx12c.vram_budget = dxgi_vram_budget(adapter, haveDesc ? &desc : NULL);
				IDXGIAdapter_Release(adapter);
			}
			IDXGIFactory4_Release(factory);
		}
		if (dxgi != NULL)
			FreeLibrary(dxgi);
		if (dx12c.vram_budget.budget_kib == 0)
			dx12c.vram_budget.budget_kib = 512 * 1024;
		NOTICE_LOG(RENDERER, "DX12: vendor %04x, resource binding tier %d, interface version %d, required state %x\n",
				vendor, (int)options.ResourceBindingTier, dx12c.v2 ? 2 : 1, (unsigned)dx12c.required_state);
		vram_budget_say("DX12", &dx12c.vram_budget);
	}

	ok = SUCCEEDED(ID3D12Device_CreateFence(dx12c.device, 0, D3D12_FENCE_FLAG_NONE, &iid_fence, (void **)&dx12c.fence));
	if (!ok)
		dx12c.fence = NULL;
	dx12c.fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	ok = ok && dx12c.fence_event != NULL;
	dx12c.fence_value = dx12c.completed = 0;
	for (i = 0; ok && i < DX12C_ALLOCATORS; i++)
	{
		ok = SUCCEEDED(ID3D12Device_CreateCommandAllocator(dx12c.device, D3D12_COMMAND_LIST_TYPE_DIRECT, &iid_command_allocator,
				(void **)&dx12c.allocators[i]));
		if (!ok)
			dx12c.allocators[i] = NULL;
		dx12c.allocator_fence[i] = 0;
	}
	dx12c.allocator_next = 0;
	ok = ok && SUCCEEDED(ID3D12Device_CreateCommandList(dx12c.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, dx12c.allocators[0], NULL,
			&iid_graphics_command_list, (void **)&dx12c.cmd_list));
	if (ok)
		ID3D12GraphicsCommandList_Close(dx12c.cmd_list);
	dx12c.recording = false;

	/* The upload ring: what a frame's renders take to the GPU, vertices,
	 * indexes, constants, textures as they are in video memory, the
	 * textures the CPU converts - a share of the video memory there is,
	 * 32 to 96 MB, mapped once and for good */
	if (ok)
	{
		u64 bytes = (u64)vram_budget_available_kib(&dx12c.vram_budget) * 1024 / 32;
		D3D12_HEAP_PROPERTIES heap;
		D3D12_RESOURCE_DESC desc;
		D3D12_RANGE none;
		void *p = NULL;
		bytes = MAX_U64(bytes, (u64)32 << 20);
		bytes = MIN_U64(bytes, (u64)96 << 20);
		dx12c.ring_size = bytes & ~(u64)0xffff;
		memset(&heap, 0, sizeof(heap));
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		memset(&desc, 0, sizeof(desc));
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = dx12c.ring_size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ok = SUCCEEDED(ID3D12Device_CreateCommittedResource(dx12c.device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &iid_resource, (void **)&dx12c.ring));
		if (!ok)
			dx12c.ring = NULL;
		none.Begin = none.End = 0;
		ok = ok && SUCCEEDED(ID3D12Resource_Map(dx12c.ring, 0, &none, &p));
		dx12c.ring_cpu = (u8 *)p;
		dx12c.ring_gpu = ok ? ID3D12Resource_GetGPUVirtualAddress(dx12c.ring) : 0;
		dx12c.ring_head = dx12c.ring_tail = dx12c.ring_submit_head = 0;
		dx12c.ring_mark_first = dx12c.ring_mark_count = 0;
	}
	dx12c.released_first = dx12c.released_count = 0;

	/* the heaps */
	ok = ok && dx12_heap_init(&dx12c.srv_heap, dx12c.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DX12C_SRV_HEAP_SIZE, true,
			DX12_SRV_FIRST_FREE);
	if (ok)
		/* (the table's own; the UAVs' are handed out by the texture decoder from DX12C_UAV_FIRST on) */
		dx12_heap_range(&dx12c.srv_heap, DX12_SRV_FIRST_FREE, DX12C_UAV_FIRST);
	ok = ok && dx12_heap_init(&dx12c.sampler_heap, dx12c.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, DX12_SAMPLER_TABLE_SIZE, true,
			DX12_SAMPLER_TABLE_SIZE);
	ok = ok && dx12_heap_init(&dx12c.rtv_heap, dx12c.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, false, DX12C_TRANSIENT_RTVS);
	dx12c.transient_rtv = 0;
	/* (the depth's three views, and SSAA's depth's) */
	ok = ok && dx12_heap_init(&dx12c.dsv_heap, dx12c.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 8, false, 0);
	if (ok)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
		/* every place in the table something: a view of nothing */
		for (i = 0; i < DX12C_UAV_FIRST; i++)
			dx12c_make_srv(NULL, DXGI_FORMAT_R8G8B8A8_UNORM, 1, i);
		memset(&uav, 0, sizeof(uav));
		uav.Format = DXGI_FORMAT_R8G8B8A8_UINT;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		for (i = DX12C_UAV_FIRST; i < DX12C_SRV_HEAP_SIZE; i++)
			ID3D12Device_CreateUnorderedAccessView(dx12c.device, NULL, NULL, &uav, dx12_heap_cpu(&dx12c.srv_heap, i));
		uav.Format = DXGI_FORMAT_R16G16B16A16_UINT;
		ID3D12Device_CreateUnorderedAccessView(dx12c.device, NULL, NULL, &uav, dx12_heap_cpu(&dx12c.srv_heap, DX12C_OIT_SUMS_SLOT));
	}
	ok = ok && make_root_signatures();
	if (!ok)
	{
		ERROR_LOG(RENDERER, "DX12: initialization failed\n");
		dx12c_term();
		return false;
	}
	dx12_pso_init(dx12c.device, dx12c.raster_root, vendor);
	dx12_pso_make_at_start();
	DX12_RELEASE(dx12c.frame_resource);
	dx12c.handed_over = false;
	/* the HDR output, when the frontend took it (libretro.cpp) */
	if (hdr_output.format != HDR_OUTPUT_NONE)
	{
		dx12c.hdr_format = hdr_output.format == HDR_OUTPUT_SCRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
		dx12c.hdr = hdr_d3d12_new(dx12c.device, dx12c.hdr_format, dx12c.required_state, dx12c.fence);
		if (dx12c.hdr == NULL)
			ERROR_LOG(RENDERER, "DX12: the HDR output could not be set up; the picture goes out as 24-bit\n");
		else
			NOTICE_LOG(RENDERER, "DX12: HDR output, %s\n", hdr_output.format == HDR_OUTPUT_SCRGB ? "scRGB (RGBA16F)" : "HDR10 (R10G10B10A2)");
	}
	dx12c.stat_stalls = dx12c.stat_submits = 0;
	return true;
}

void dx12c_term(void)
{
	u32 i;
	if (dx12c.device != NULL)
	{
		/* everything done: what is released below is not used any more */
		if (dx12c.recording)
			dx12c_submit();
		dx12c_wait_for(dx12c.fence_value);
		reclaim_released(dx12c.fence_value);
	}
	hdr_d3d12_free(dx12c.hdr);
	dx12c.hdr = NULL;
	dx12c.hdr_frame = NULL;
	dx12_pso_term();
	DX12_RELEASE(dx12c.draw_signature);
	DX12_RELEASE(dx12c.decode_root);
	DX12_RELEASE(dx12c.raster_root);
	dx12_heap_term(&dx12c.dsv_heap);
	dx12_heap_term(&dx12c.rtv_heap);
	dx12_heap_term(&dx12c.sampler_heap);
	dx12_heap_term(&dx12c.srv_heap);
	if (dx12c.ring != NULL && dx12c.ring_cpu != NULL)
		ID3D12Resource_Unmap(dx12c.ring, 0, NULL);
	dx12c.ring_cpu = NULL;
	DX12_RELEASE(dx12c.ring);
	dx12c.ring_size = dx12c.ring_head = dx12c.ring_tail = 0;
	dx12c.ring_mark_first = dx12c.ring_mark_count = 0;
	DX12_RELEASE(dx12c.readback);
	dx12c.readback_size = 0;
	DX12_RELEASE(dx12c.cmd_list);
	for (i = 0; i < DX12C_ALLOCATORS; i++)
		DX12_RELEASE(dx12c.allocators[i]);
	dx12c.recording = false;
	DX12_RELEASE(dx12c.fence);
	if (dx12c.fence_event != NULL)
		CloseHandle(dx12c.fence_event);
	dx12c.fence_event = NULL;
	dx12c.fence_value = dx12c.completed = 0;
	DX12_RELEASE(dx12c.queue);
	DX12_RELEASE(dx12c.device);
	if (dx12c.d3d12_module != NULL)
		FreeLibrary(dx12c.d3d12_module);
	dx12c.d3d12_module = NULL;
	DX12_RELEASE(dx12c.frame_resource);
	dx12c.handed_over = false;
	dx12c.v2 = false;
	dx12c.handle = NULL;
	dx12c.set_texture = NULL;
	dx12c.get_sync_index = NULL;
	dx12c.get_sync_index_mask = NULL;
	dx12c.wait_sync_index = NULL;
	dx12c.set_texture_fenced = NULL;
	dx12c.before_submit = NULL;
	dx12c.before_submit_arg = NULL;
}

/* ---- Recording ---- */

static void reset_bound(void)
{
	memset(&dx12c.bound, 0, sizeof(dx12c.bound));
	dx12c.bound.topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	dx12c.bound.rtv = dx12c.bound.dsv = ~0u - 1;
	dx12c.bound.vertexBuffer.BufferLocation = ~(u64)0;
	dx12c.bound.indexBuffer.BufferLocation = ~(u64)0;
}

static void set_heaps(void)
{
	ID3D12DescriptorHeap *heaps[2];
	heaps[0] = dx12c.srv_heap.heap;
	heaps[1] = dx12c.sampler_heap.heap;
	ID3D12GraphicsCommandList_SetDescriptorHeaps(dx12c.cmd_list, 2, heaps);
}

ID3D12GraphicsCommandList *dx12c_cmd_open(void)
{
	const u32 slot = dx12c.allocator_next % DX12C_ALLOCATORS;
	if (dx12c.recording)
		return dx12c.cmd_list;
	if (dx12c.allocator_fence[slot] > dx12c_completed())
	{
		/* Every allocator holds a list the GPU has not done yet: the GPU is
		 * that many lists behind */
		dx12c.stat_stalls++;
		dx12c_wait_for(dx12c.allocator_fence[slot]);
	}
	ID3D12CommandAllocator_Reset(dx12c.allocators[slot]);
	ID3D12GraphicsCommandList_Reset(dx12c.cmd_list, dx12c.allocators[slot], NULL);
	set_heaps();
	reset_bound();
	dx12c.recording = true;
	return dx12c.cmd_list;
}

void dx12c_bind_raster(void)
{
	ID3D12GraphicsCommandList *cmd = dx12c_cmd();
	if (dx12c.bound.rasterBound)
		return;
	dx12c.bound.rasterBound = true;
	ID3D12GraphicsCommandList_SetGraphicsRootSignature(cmd, dx12c.raster_root);
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cmd, 3, dx12_heap_gpu(&dx12c.srv_heap, 0));
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cmd, 4, dx12_heap_gpu(&dx12c.sampler_heap, 0));
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cmd, 5,
			dx12_heap_gpu(&dx12c.srv_heap, dx12c.oit_table_ssaa ? DX12C_OIT_SSAA_FIRST : DX12C_OIT_FIRST));
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cmd, 7, dx12_heap_gpu(&dx12c.srv_heap, DX12C_OIT_SUMS_SLOT));
	dx12c.bound.oitSsaa = dx12c.oit_table_ssaa;
	dx12c.bound.rootConstantsSet = false;
	dx12c.bound.pso = NULL;
}

u64 dx12c_submit(void)
{
	ID3D12CommandList *lists[1];
	if (dx12c.before_submit != NULL)
		dx12c.before_submit(dx12c.before_submit_arg);
	if (!dx12c.recording)
		return dx12c.fence_value;
	dx12c.recording = false;
	if (FAILED(ID3D12GraphicsCommandList_Close(dx12c.cmd_list)))
		WARN_LOG(RENDERER, "DX12: the command list could not be closed\n");
	lists[0] = (ID3D12CommandList *)dx12c.cmd_list;
	ID3D12CommandQueue_ExecuteCommandLists(dx12c.queue, 1, lists);
	dx12c.fence_value++;
	ID3D12CommandQueue_Signal(dx12c.queue, dx12c.fence, dx12c.fence_value);
	dx12c.allocator_fence[dx12c.allocator_next % DX12C_ALLOCATORS] = dx12c.fence_value;
	dx12c.allocator_next++;
	/* What the ring had when the list before this one was submitted is
	 * the GPU's until this list is done: a piece written for this list
	 * may be read by the next one as well (a texture converted into the
	 * ring before its copy is recorded; the decoder's bytes, when a
	 * readback submits the list between the two), so a piece is let go of
	 * one list later than the one it was written for. */
	if (dx12c.ring_mark_count == DX12C_RING_MARKS)
	{
		/* (the oldest mark merged into the next: its place freed later, not sooner) */
		dx12c.ring_mark_first = (dx12c.ring_mark_first + 1) % DX12C_RING_MARKS;
		dx12c.ring_mark_count--;
	}
	{
		dx12_ring_mark *m = &dx12c.ring_marks[(dx12c.ring_mark_first + dx12c.ring_mark_count) % DX12C_RING_MARKS];
		m->fence = dx12c.fence_value;
		m->head = dx12c.ring_submit_head;
	}
	dx12c.ring_mark_count++;
	dx12c.ring_submit_head = dx12c.ring_head;
	dx12c.stat_submits++;
	dx12c_completed();
	return dx12c.fence_value;
}

u64 dx12c_completed(void)
{
	u64 done;
	if (dx12c.fence == NULL)
		return 0;
	done = ID3D12Fence_GetCompletedValue(dx12c.fence);
	/* (a device removed answers UINT64_MAX) */
	dx12c.completed = MIN_U64(done, dx12c.fence_value);
	while (dx12c.ring_mark_count != 0 && dx12c.ring_marks[dx12c.ring_mark_first].fence <= dx12c.completed)
	{
		dx12c.ring_tail = dx12c.ring_marks[dx12c.ring_mark_first].head;
		dx12c.ring_mark_first = (dx12c.ring_mark_first + 1) % DX12C_RING_MARKS;
		dx12c.ring_mark_count--;
	}
	/* (everything submitted done: the last list's pieces too) */
	if (dx12c.ring_mark_count == 0 && dx12c.completed == dx12c.fence_value)
		dx12c.ring_tail = dx12c.ring_submit_head;
	reclaim_released(dx12c.completed);
	return dx12c.completed;
}

void dx12c_wait_for(u64 value)
{
	if (dx12c.fence == NULL || value == 0 || ID3D12Fence_GetCompletedValue(dx12c.fence) >= value)
	{
		dx12c_completed();
		return;
	}
	if (SUCCEEDED(ID3D12Fence_SetEventOnCompletion(dx12c.fence, value, dx12c.fence_event)))
		WaitForSingleObject(dx12c.fence_event, 10000);
	dx12c_completed();
}

bool dx12c_upload(u32 bytes, u32 align, dx12_upload *out)
{
	u64 pos, at;
	if (dx12c.ring_cpu == NULL || bytes > dx12c.ring_size)
		return false;
	pos = (dx12c.ring_head + align - 1) & ~(u64)(align - 1);
	/* (never across the end: from the start again) */
	if (pos % dx12c.ring_size + bytes > dx12c.ring_size)
		pos = (pos / dx12c.ring_size + 1) * dx12c.ring_size;
	while (pos + bytes - dx12c.ring_tail > dx12c.ring_size)
	{
		/* the GPU still reads what it would write over */
		if (dx12c.ring_mark_count == 0)
		{
			/* (what the list being recorded has already: more than the ring
			 * in one render) */
			WARN_LOG(RENDERER, "DX12: the upload ring is too small for this render (%u bytes more)\n", bytes);
			return false;
		}
		if (dx12c_completed() >= dx12c.ring_marks[dx12c.ring_mark_first].fence)
			continue;
		dx12c.stat_stalls++;
		dx12c_wait_for(dx12c.ring_marks[dx12c.ring_mark_first].fence);
	}
	dx12c.ring_head = pos + bytes;
	at = pos % dx12c.ring_size;
	out->cpu = dx12c.ring_cpu + at;
	out->gpu = dx12c.ring_gpu + at;
	out->buffer = dx12c.ring;
	out->offset = at;
	return true;
}

static void add_released(IUnknown *object, u32 srv, u64 last)
{
	dx12_released *r;
	if (dx12c.released_count == DX12C_RELEASES)
	{
		dx12c.stat_stalls++;
		dx12c_wait_for(dx12c.released[dx12c.released_first].fence);
		reclaim_released(dx12c.completed);
	}
	r = &dx12c.released[(dx12c.released_first + dx12c.released_count) % DX12C_RELEASES];
	r->object = object;
	r->srv = srv;
	r->fence = last;
	dx12c.released_count++;
}

void dx12c_release(IUnknown *object)
{
	u64 last;
	if (object == NULL)
		return;
	last = dx12c.recording ? dx12c.fence_value + 1 : dx12c.fence_value;
	if (last <= dx12c_completed())
	{
		IUnknown_Release(object);
		return;
	}
	add_released(object, ~0u, last);
}

void dx12c_release_srv(u32 index)
{
	u64 last;
	if (index < DX12_SRV_FIRST_FREE || index >= DX12C_UAV_FIRST)
		return;
	last = dx12c.recording ? dx12c.fence_value + 1 : dx12c.fence_value;
	if (last <= dx12c_completed())
	{
		dx12_heap_free(&dx12c.srv_heap, index);
		return;
	}
	add_released(NULL, index, last);
}

ID3D12Resource *dx12c_readback_buffer(u64 bytes)
{
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC desc;
	if (dx12c.readback != NULL && dx12c.readback_size >= bytes)
		return dx12c.readback;
	/* (the one there is may be being read into: a readback waits anyway) */
	if (dx12c.readback != NULL)
	{
		dx12c_submit();
		dx12c_wait_for(dx12c.fence_value);
		DX12_RELEASE(dx12c.readback);
	}
	dx12c.readback_size = 0;
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = (bytes + 0xffff) & ~(u64)0xffff;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(ID3D12Device_CreateCommittedResource(dx12c.device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
			NULL, &iid_resource, (void **)&dx12c.readback)))
	{
		WARN_LOG(RENDERER, "DX12: readback buffer creation failed (%u KB)\n", (u32)(bytes / 1024));
		dx12c.readback = NULL;
		return NULL;
	}
	dx12c.readback_size = desc.Width;
	return dx12c.readback;
}

void dx12c_uav_barrier(ID3D12Resource *resource)
{
	D3D12_RESOURCE_BARRIER barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = resource;
	ID3D12GraphicsCommandList_ResourceBarrier(dx12c_cmd(), 1, &barrier);
}

void dx12c_transition_resource(ID3D12Resource *resource, D3D12_RESOURCE_STATES *current, D3D12_RESOURCE_STATES state,
		UINT subresource)
{
	D3D12_RESOURCE_BARRIER barrier;
	if (*current == state || resource == NULL)
		return;
	memset(&barrier, 0, sizeof(barrier));
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = subresource;
	barrier.Transition.StateBefore = *current;
	barrier.Transition.StateAfter = state;
	ID3D12GraphicsCommandList_ResourceBarrier(dx12c_cmd(), 1, &barrier);
	*current = state;
}

void dx12c_set_targets(u32 rtv, u32 dsv, u32 samples)
{
	/* (a list started here forgets what was bound: before the samples are noted) */
	ID3D12GraphicsCommandList *cmd = dx12c_cmd();
	D3D12_CPU_DESCRIPTOR_HANDLE r, d;
	dx12c.bound.samples = samples;
	if (rtv == dx12c.bound.rtv && dsv == dx12c.bound.dsv && rtv >= DX12C_TRANSIENT_RTVS)
		return;
	r = dx12_heap_cpu(&dx12c.rtv_heap, rtv);
	d = dx12_heap_cpu(&dx12c.dsv_heap, dsv);
	ID3D12GraphicsCommandList_OMSetRenderTargets(cmd, rtv != ~0u ? 1 : 0, rtv != ~0u ? &r : NULL, FALSE, dsv != ~0u ? &d : NULL);
	dx12c.bound.rtv = rtv;
	dx12c.bound.dsv = dsv;
}

void dx12c_draw_quad_at(u32 srv, bool linear, float x, float y, float w, float h,
		float u0, float v0, float u1, float v1, u32 special, const float *factor)
{
	DX12PsoKey key;
	ID3D12PipelineState *pso;
	float values[8];
	u32 constants[10];
	dx12c_bind_raster();
	memset(&key, 0, sizeof(key));
	key.kind = DX12_PSO_QUAD;
	key.writeColor = true;
	key.special = special;
	key.depthBound = dx12c_has_depth_bound();
	key.samples = dx12c_target_samples();
	pso = dx12_pso_get(&key);
	if (pso == NULL)
		return;
	dx12c_set_pipeline(pso);
	dx12c_set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	if (special == DX12_BLEND_FACTOR && factor != NULL)
		dx12c_set_blend_factor(factor);
	values[0] = x;
	values[1] = y;
	values[2] = w;
	values[3] = h;
	values[4] = u0;
	values[5] = v0;
	values[6] = u1;
	values[7] = v1;
	memcpy(constants, values, sizeof(values));
	constants[8] = srv;
	constants[9] = linear ? DX12_QF_LINEAR : 0;
	dx12c_set_root_constants(constants, 10);
	ID3D12GraphicsCommandList_DrawInstanced(dx12c_cmd(), 4, 1, 0, 0);
}

/* ---- The frame ---- */

bool dx12c_encode_hdr(dx12_image *image, u32 width, u32 height, dx12_image *samples, u32 samplesLog2, dx12_image *sums)
{
	dx12c.hdr_frame = NULL;
	if (dx12c.hdr == NULL || image->resource == NULL)
		return false;
	dx12c_transition(image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	if (samples != NULL)
		dx12c_transition(samples, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	if (sums != NULL)
		dx12c_transition(sums, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	/* (signalled by the dx12c_submit() that follows) */
	if (!hdr_d3d12_encode(dx12c.hdr, dx12c_cmd(), &hdr_output, image->resource, DX12FrameFormat, width, height, true,
			samples != NULL ? samples->resource : NULL, samples != NULL ? samplesLog2 : 0,
			sums != NULL ? sums->resource : NULL, dx12c_present_index(), dx12c.fence_value + 1, &dx12c.hdr_frame))
		return false;
	/* what the encode bound is not what this has noted */
	set_heaps();
	reset_bound();
	return true;
}

void dx12c_set_frame(const dx12_image *image)
{
	DX12_RELEASE(dx12c.frame_resource);
	dx12c.frame_format = DX12FrameFormat;
	if (image != NULL && dx12c.hdr_frame != NULL)
	{
		ID3D12Resource_AddRef(dx12c.hdr_frame);
		dx12c.frame_resource = dx12c.hdr_frame;
		dx12c.frame_format = dx12c.hdr_format;
	}
	else if (image != NULL && image->resource != NULL)
	{
		ID3D12Resource_AddRef(image->resource);
		dx12c.frame_resource = image->resource;
	}
	dx12c.hdr_frame = NULL;
	dx12c.frame_fence = dx12c.fence_value;
	dx12c.handed_over = dx12c.v2 && dx12c.frame_resource != NULL;
	if (dx12c.handed_over)
		dx12c.set_texture_fenced(dx12c.handle, dx12c.frame_resource, dx12c.frame_format, dx12c.fence, dx12c.frame_fence);
}

void dx12c_present(void)
{
	if (dx12c.device == NULL || dx12c.frame_resource == NULL)
		return;
	if (dx12c.v2)
	{
		/* (a frame with no render to the screen in it shows the last one again) */
		if (!dx12c.handed_over)
			dx12c.set_texture_fenced(dx12c.handle, dx12c.frame_resource, dx12c.frame_format, dx12c.fence, dx12c.frame_fence);
		dx12c.handed_over = false;
	}
	else
		/* the frontend copies it in video_refresh, on the same queue, after
		 * the render that drew it */
		dx12c.set_texture(dx12c.handle, dx12c.frame_resource, dx12c.frame_format);
}

unsigned dx12c_present_count(void)
{
	unsigned mask, count = 1;
	if (!dx12c.v2)
		return 1;
	mask = dx12c.get_sync_index_mask(dx12c.handle);
	while (count < 32 && (mask >> count) != 0)
		count++;
	return count;
}

unsigned dx12c_present_index(void)
{
	unsigned index;
	if (!dx12c.v2)
		return 0;
	index = dx12c.get_sync_index(dx12c.handle);
	return index < 32 ? index : 0;
}

void dx12c_wait_present_index(void)
{
	if (dx12c.v2)
		dx12c.wait_sync_index(dx12c.handle);
}
