/* The chip's write-out for the Direct3D 11 renderers: see dx11_writeout.h */
#include "dx11_writeout.h"
#include "dx11_shaders.h"
#include "rend/tile/compute_frame.h"

/* The pass: a thread a column and a remainder of the lines mod m, which
 * walks down its lines in place, as pvr_compute.h's PVRC_K_YFILTER does */
static const char *const passSource =
"#define PVR_SHADER_HLSL\n#define PVR_LANE_NONE\n"
#include "rend/tile/pvr_resolve_text.inc"
#include "rend/tile/pvr_scaler_text.inc"
R"(
RWTexture2D<unorm float4> picture : register(u0);
cbuffer WriteOut : register(b0)
{
   uint4 words;	// the width, the height, the frame's PVRC_F_FLAGS
};

uint wo_load(int2 p)
{
   uint4 v = uint4(round(picture[p] * 255.0));
   return v.x | (v.y << 8) | (v.z << 16) | (v.w << 24);
}

[numthreads(64, 1, 1)]
void main(uint3 global : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
   uint w = words.x, h = words.y, flags = words.z;
   uint m = (flags >> 3) & 31u, ycoeff = flags >> 16, fbw = (flags >> 8) & 63u;
   int x = int(global.x);
   uint y = group.y;
   if (m == 0u || uint(x) >= w || y >= h)
      return;
   uint mid = wo_load(int2(x, int(y)));
   uint above = mid;
   for (; y < h; y += m)
   {
      uint below = y + m < h ? wo_load(int2(x, int(y + m))) : mid;
      uint c = pvr_fb_write((flags & 0x4000u) != 0u ? pvr_scaler_y(above, mid, below, ycoeff) : mid, uint(x) / m, y / m, fbw);
      picture[int2(x, int(y))] = (float4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float(c >> 24)) + 0.25) / 255.0;
      above = mid;
      mid = below;
   }
}
)";
static_assert(PVRC_FF_YON == 0x4000u, "the pass's flags, as pvr_compute.h has them");

void DX11WriteOut::Term()
{
	shader.reset();
	constants.reset();
	deviceContext = nullptr;
}

bool DX11WriteOut::Init(ID3D11Device *device, ID3D11DeviceContext *ctx, DX11Shaders *shaders)
{
	if (ComputeFrame::ScalersOff)
	{
		Term();
		return false;
	}
	if (shader)
		return true;
	D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs{};
	fs.InFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0
			|| FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs, sizeof(fs)))
			|| (fs.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) == 0)
	{
		WARN_LOG(RENDERER, "DX11: no typed loads of the picture: the PowerVR2 write-out pass is left out");
		return false;
	}
	ComPtr<ID3DBlob> blob = shaders->Compile(passSource, "main", "cs_5_0", nullptr);
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = 16;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (!blob || FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.put()))
			|| FAILED(device->CreateBuffer(&desc, nullptr, constants.put())))
	{
		WARN_LOG(RENDERER, "DX11: the PowerVR2 write-out pass could not be made: the picture goes without it");
		Term();
		return false;
	}
	deviceContext = ctx;
	return true;
}

void DX11WriteOut::Record(ID3D11UnorderedAccessView *uav, u32 width, u32 height)
{
	if (!shader || uav == nullptr)
		return;
	const u32 flags = ComputeFrame::WriteOut(height);
	if (PVRC_FF_YLINES(flags) == 0)
		return;
	const u32 words[4] = { width, height, flags, 0 };
	deviceContext->UpdateSubresource(constants, 0, nullptr, words, 0, 0);
	deviceContext->CSSetShader(shader, nullptr, 0);
	ID3D11Buffer *cb = constants.get();
	deviceContext->CSSetConstantBuffers(0, 1, &cb);
	deviceContext->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	deviceContext->Dispatch((width + 63) / 64, PVRC_FF_YLINES(flags), 1);
	ID3D11UnorderedAccessView *none = nullptr;
	deviceContext->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
	deviceContext->CSSetShader(nullptr, nullptr, 0);
}
