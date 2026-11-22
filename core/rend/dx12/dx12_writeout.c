/* The chip's write-out for the Direct3D 12 raster renderers: see dx12_writeout.h */
#include "dx12_writeout.h"
#include <string.h>
#include "rend/tile/compute_frame.h"
#include "dx12_compute.h"

/* a slot of words: the frame's PVRC_F_WIDTH to PVRC_F_FLAGS */
#define SLOT_WORDS 64
typedef char dx12_writeout_slot_check[PVRC_F_FLAGS < SLOT_WORDS && PVRC_F_WIDTH < SLOT_WORDS && PVRC_F_HEIGHT < SLOT_WORDS ? 1 : -1];

static const IID iid_root_signature = { 0xc54a6b66, 0x72df, 0x4ee8, { 0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14 } };
static const IID iid_pipeline_state = { 0x765a30f3, 0xf624, 0x4c6f, { 0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45 } };
static const IID iid_resource = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };

static struct
{
	bool ready;
	ID3D12RootSignature *root;
	ID3D12PipelineState *pipeline;
	ID3D12Resource *words;	/* a slot of the frame's words for each picture the frontend can hold */
	u32 *mapped;
	u32 slot;
} wo;

bool dx12_writeout_wanted(void)
{
	return !compute_frame_scalers_off;
}

void dx12_writeout_term(void)
{
	if (wo.words != NULL && wo.mapped != NULL)
		ID3D12Resource_Unmap(wo.words, 0, NULL);
	wo.mapped = NULL;
	DX12_RELEASE(wo.words);
	DX12_RELEASE(wo.pipeline);
	DX12_RELEASE(wo.root);
	wo.ready = false;
	wo.slot = 0;
}

typedef HRESULT (WINAPI *serialize_fn)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);

void dx12_writeout_init(void)
{
	ID3D12Device *device = dx12c.device;
	D3D12_FEATURE_DATA_FORMAT_SUPPORT fs;
	u32 i;
	dx12_writeout_term();
	if (!dx12_writeout_wanted())
		return;
	/* (typed loads of the picture's R8G8B8A8_UNORM: every device of feature
	 * level 12_0 and up has them, an option of 11's) */
	memset(&fs, 0, sizeof(fs));
	fs.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (FAILED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))
			|| (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) == 0)
	{
		WARN_LOG(RENDERER, "DX12: no typed loads of the picture: the PowerVR2 write-out pass is left out\n");
		return;
	}
	/* the root signature: a table of the frame's words (u0) and the picture (u0 of space 2) */
	{
		D3D12_DESCRIPTOR_RANGE ranges[2];
		D3D12_ROOT_PARAMETER param;
		D3D12_ROOT_SIGNATURE_DESC desc;
		HMODULE lib = GetModuleHandleA("d3d12.dll");
		serialize_fn serialize = lib != NULL ? (serialize_fn)GetProcAddress(lib, "D3D12SerializeRootSignature") : NULL;
		ID3DBlob *blob = NULL, *errors = NULL;
		bool ok;
		memset(ranges, 0, sizeof(ranges));
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[0].NumDescriptors = 1;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 1;
		ranges[1].RegisterSpace = 2;
		ranges[1].OffsetInDescriptorsFromTableStart = 1;
		memset(&param, 0, sizeof(param));
		param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		param.DescriptorTable.NumDescriptorRanges = 2;
		param.DescriptorTable.pDescriptorRanges = ranges;
		memset(&desc, 0, sizeof(desc));
		desc.NumParameters = 1;
		desc.pParameters = &param;
		ok = serialize != NULL && SUCCEEDED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))
				&& SUCCEEDED(ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
						&iid_root_signature, (void **)&wo.root));
		DX12_RELEASE(blob);
		DX12_RELEASE(errors);
		if (!ok)
		{
			wo.root = NULL;
			WARN_LOG(RENDERER, "DX12: no root signature for the PowerVR2 write-out pass\n");
			dx12_writeout_term();
			return;
		}
	}
	{
		size_t size = 0;
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.pRootSignature = wo.root;
		desc.CS.pShaderBytecode = dx12_compute_kernel(PVRC_K_YFILTER, &size);
		desc.CS.BytecodeLength = size;
		if (FAILED(ID3D12Device_CreateComputePipelineState(device, &desc, &iid_pipeline_state, (void **)&wo.pipeline)))
		{
			wo.pipeline = NULL;
			WARN_LOG(RENDERER, "DX12: no pipeline for the PowerVR2 write-out pass\n");
			dx12_writeout_term();
			return;
		}
	}
	/* the words: memory of the host's the CPU writes and the views read in
	 * place, as the compute renderer's ring is */
	{
		D3D12_HEAP_PROPERTIES heap;
		D3D12_RESOURCE_DESC desc;
		D3D12_RANGE none;
		memset(&heap, 0, sizeof(heap));
		heap.Type = D3D12_HEAP_TYPE_CUSTOM;
		heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
		heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
		memset(&desc, 0, sizeof(desc));
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = (u64)SLOT_WORDS * 4 * DX12C_WRITEOUT_SLOTS;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		none.Begin = none.End = 0;
		if (FAILED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				NULL, &iid_resource, (void **)&wo.words)))
			wo.words = NULL;
		if (wo.words == NULL || FAILED(ID3D12Resource_Map(wo.words, 0, &none, (void **)&wo.mapped)))
		{
			WARN_LOG(RENDERER, "DX12: no words for the PowerVR2 write-out pass\n");
			wo.mapped = NULL;
			dx12_writeout_term();
			return;
		}
	}
	for (i = 0; i < DX12C_WRITEOUT_SLOTS; i++)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC d;
		memset(&d, 0, sizeof(d));
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		d.Buffer.FirstElement = (u64)i * SLOT_WORDS;
		d.Buffer.NumElements = SLOT_WORDS;
		d.Buffer.StructureByteStride = 4;
		ID3D12Device_CreateUnorderedAccessView(device, wo.words, NULL, &d, dx12_heap_cpu(&dx12c.srv_heap, DX12C_WRITEOUT_FIRST + 2 * i));
	}
	wo.ready = true;
}

void dx12_writeout_record(dx12_image *picture)
{
	ID3D12GraphicsCommandList *cmd;
	ID3D12DescriptorHeap *heaps[2];
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
	D3D12_RESOURCE_BARRIER b;
	u32 flags, table, *w;
	if (!wo.ready || picture->resource == NULL)
		return;
	flags = compute_frame_write_out(picture->height);
	if (PVRC_FF_YLINES(flags) == 0)
		return;
	/* (a slot in turn: the frames that used it are done long since, as the
	 * frontend holds fewer pictures than there are slots) */
	wo.slot = (wo.slot + 1) % DX12C_WRITEOUT_SLOTS;
	w = wo.mapped + wo.slot * SLOT_WORDS;
	w[PVRC_F_WIDTH] = picture->width;
	w[PVRC_F_HEIGHT] = picture->height;
	w[PVRC_F_FLAGS] = flags;
	table = DX12C_WRITEOUT_FIRST + 2 * wo.slot;
	memset(&desc, 0, sizeof(desc));
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	ID3D12Device_CreateUnorderedAccessView(dx12c.device, picture->resource, NULL, &desc, dx12_heap_cpu(&dx12c.srv_heap, table + 1));

	cmd = dx12c_cmd();
	dx12c_transition(picture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	heaps[0] = dx12c.srv_heap.heap;
	heaps[1] = dx12c.sampler_heap.heap;
	ID3D12GraphicsCommandList_SetDescriptorHeaps(cmd, 2, heaps);
	ID3D12GraphicsCommandList_SetComputeRootSignature(cmd, wo.root);
	ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cmd, 0, dx12_heap_gpu(&dx12c.srv_heap, table));
	ID3D12GraphicsCommandList_SetPipelineState(cmd, wo.pipeline);
	ID3D12GraphicsCommandList_Dispatch(cmd, (picture->width + 63) / 64, PVRC_FF_YLINES(flags), 1);
	memset(&b, 0, sizeof(b));
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	b.UAV.pResource = picture->resource;
	ID3D12GraphicsCommandList_ResourceBarrier(cmd, 1, &b);
	/* (another root signature was set for compute: the raster's are bound again when they are drawn with) */
	dx12c_forget_raster();
}
