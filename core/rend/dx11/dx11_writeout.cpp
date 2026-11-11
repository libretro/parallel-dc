/* The chip's write-out for the Direct3D 11 renderers: see dx11_writeout.h */
#include "dx11_writeout.h"
#include "dx11_shaders.h"
#include "rend/tile/compute_frame.h"

static_assert(PVRC_FF_YON == 0x4000u, "the pass's flags, as pvr_compute.h has them");

void DX11WriteOut::Term()
{
	shader.reset();
	constants.reset();
	deviceContext = nullptr;
	tried = false;
}

/* Made the first time, as the renderer starts, whether the option is on or
 * not: turning it on while a game runs (a change of size) makes nothing on
 * the draw path (AGENTS.md). Whether it is used is the option's. */
bool DX11WriteOut::Init(ID3D11Device *device, ID3D11DeviceContext *ctx, DX11Shaders *shaders)
{
	if (!tried)
	{
		tried = true;
		Make(device, ctx, shaders);
	}
	return shader && !ComputeFrame::ScalersOff;
}

bool DX11WriteOut::Make(ID3D11Device *device, ID3D11DeviceContext *ctx, DX11Shaders *shaders)
{
	D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs{};
	fs.InFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0
			|| FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs, sizeof(fs)))
			|| (fs.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) == 0)
	{
		WARN_LOG(RENDERER, "DX11: no typed loads of the picture: the PowerVR2 write-out pass is left out");
		return false;
	}
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = 16;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (shaders->GetWriteOutShader(shader) == nullptr
			|| FAILED(device->CreateBuffer(&desc, nullptr, constants.put())))
	{
		WARN_LOG(RENDERER, "DX11: the PowerVR2 write-out pass could not be made: the picture goes without it");
		shader.reset();
		constants.reset();
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
