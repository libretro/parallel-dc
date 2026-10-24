/* The chip's write-out for the Direct3D 12 raster renderers: see dx12_writeout.h */
#include "dx12_writeout.h"
#include "rend/tile/compute_frame.h"

// (dx12_compute.cpp has the kernels' DXIL)
const void *dx12_compute_kernel(u32 kernel, size_t *size);

// a slot of words: the frame's PVRC_F_WIDTH to PVRC_F_FLAGS
static const u32 SlotWords = 64;
static_assert(PVRC_F_FLAGS < SlotWords && PVRC_F_WIDTH < SlotWords && PVRC_F_HEIGHT < SlotWords, "the pass's words in a slot");

bool DX12WriteOut::Wanted()
{
	return !ComputeFrame::ScalersOff;
}

void DX12WriteOut::Term()
{
	if (words && mapped != nullptr)
		words->Unmap(0, nullptr);
	mapped = nullptr;
	words.reset();
	pipeline.reset();
	root.reset();
	context = nullptr;
	slot = 0;
}

void DX12WriteOut::Init(DX12Context *ctx)
{
	Term();
	if (!Wanted())
		return;
	ID3D12Device *device = ctx->GetDevice();
	/* (typed loads of the picture's R8G8B8A8_UNORM: every device of feature
	 * level 12_0 and up has them, an option of 11's) */
	D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
	fs.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))
			|| (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) == 0)
	{
		WARN_LOG(RENDERER, "DX12: no typed loads of the picture: the PowerVR2 write-out pass is left out");
		return;
	}
	// the root signature: a table of the frame's words (u0) and the picture (u0 of space 2)
	{
		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 2, 1 };
		D3D12_ROOT_PARAMETER param{};
		param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		param.DescriptorTable.NumDescriptorRanges = 2;
		param.DescriptorTable.pDescriptorRanges = ranges;
		D3D12_ROOT_SIGNATURE_DESC desc{};
		desc.NumParameters = 1;
		desc.pParameters = &param;
		typedef HRESULT (WINAPI *serialize_fn)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
		HMODULE lib = GetModuleHandleA("d3d12.dll");
		serialize_fn serialize = lib != nullptr ? (serialize_fn)(void *)GetProcAddress(lib, "D3D12SerializeRootSignature") : nullptr;
		ComPtr<ID3DBlob> blob, errors;
		if (serialize == nullptr || FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()))
				|| FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void **)root.put())))
		{
			WARN_LOG(RENDERER, "DX12: no root signature for the PowerVR2 write-out pass");
			Term();
			return;
		}
	}
	{
		size_t size = 0;
		const void *code = dx12_compute_kernel(PVRC_K_YFILTER, &size);
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = root;
		desc.CS = { code, size };
		if (FAILED(device->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState), (void **)pipeline.put())))
		{
			WARN_LOG(RENDERER, "DX12: no pipeline for the PowerVR2 write-out pass");
			Term();
			return;
		}
	}
	/* the words: memory of the host's the CPU writes and the views read in
	 * place, as the compute renderer's ring is */
	{
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_CUSTOM;
		heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
		heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = (u64)SlotWords * 4 * DX12Context::WriteOutSlots;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		const D3D12_RANGE none{};
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
				__uuidof(ID3D12Resource), (void **)words.put())) || FAILED(words->Map(0, &none, (void **)&mapped)))
		{
			WARN_LOG(RENDERER, "DX12: no words for the PowerVR2 write-out pass");
			mapped = nullptr;
			Term();
			return;
		}
	}
	for (u32 i = 0; i < DX12Context::WriteOutSlots; i++)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		d.Buffer.FirstElement = (u64)i * SlotWords;
		d.Buffer.NumElements = SlotWords;
		d.Buffer.StructureByteStride = 4;
		device->CreateUnorderedAccessView(words, nullptr, &d, ctx->Srv().Cpu(DX12Context::WriteOutFirst + 2 * i));
	}
	context = ctx;
}

void DX12WriteOut::Record(DX12Image& picture)
{
	if (context == nullptr || !picture.resource)
		return;
	const u32 flags = ComputeFrame::WriteOut(picture.height);
	if (PVRC_FF_YLINES(flags) == 0)
		return;
	/* (a slot in turn: the frames that used it are done long since, as the
	 * frontend holds fewer pictures than there are slots) */
	slot = (slot + 1) % DX12Context::WriteOutSlots;
	u32 *w = mapped + slot * SlotWords;
	w[PVRC_F_WIDTH] = picture.width;
	w[PVRC_F_HEIGHT] = picture.height;
	w[PVRC_F_FLAGS] = flags;
	const u32 table = DX12Context::WriteOutFirst + 2 * slot;
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	context->GetDevice()->CreateUnorderedAccessView(picture.resource, nullptr, &desc, context->Srv().Cpu(table + 1));

	ID3D12GraphicsCommandList *cmd = context->Cmd();
	context->Transition(picture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ID3D12DescriptorHeap * const heaps[2] = { context->Srv().Heap(), context->Samplers().Heap() };
	cmd->SetDescriptorHeaps(2, heaps);
	cmd->SetComputeRootSignature(root);
	cmd->SetComputeRootDescriptorTable(0, context->Srv().Gpu(table));
	cmd->SetPipelineState(pipeline);
	cmd->Dispatch((picture.width + 63) / 64, PVRC_FF_YLINES(flags), 1);
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	b.UAV.pResource = picture.resource;
	cmd->ResourceBarrier(1, &b);
	// (another root signature was set for compute: the raster's are bound again when they are drawn with)
	context->ForgetRaster();
}
