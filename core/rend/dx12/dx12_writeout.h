/* The chip's write-out (rend/tile/pvr_scaler.h: the Y filter, then the
 * packing to a framebuffer of 16 bits a pixel and its dither) for the
 * Direct3D 12 raster renderers' pictures, as the core option
 * reicast_pvr2_scaler asks: the compute renderer's own pass
 * (pvr_compute.h's PVRC_K_YFILTER, the same DXIL), run in place over the
 * picture before what goes over it (the VMUs, the crosshairs). Made in
 * Init(), when the renderer starts; nothing is made in a frame, and a
 * frame without a write-out records nothing. */
#pragma once
#include "dx12context_lr.h"

class DX12WriteOut
{
public:
	// whether the pictures are to be made for it (unordered access views of them)
	static bool Wanted();
	void Init(DX12Context *context);
	void Term();
	// the pass over @picture, which is left in D3D12_RESOURCE_STATE_UNORDERED_ACCESS if there was one
	void Record(DX12Image& picture);

private:
	DX12Context *context = nullptr;
	ComPtr<ID3D12RootSignature> root;
	ComPtr<ID3D12PipelineState> pipeline;
	ComPtr<ID3D12Resource> words;	// a slot of the frame's words for each picture the frontend can hold
	u32 *mapped = nullptr;
	u32 slot = 0;
};
