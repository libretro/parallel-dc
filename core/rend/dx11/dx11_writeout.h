/* The chip's write-out (rend/tile/pvr_scaler.h: the Y filter, then the
 * packing to a framebuffer of 16 bits a pixel and its dither) for the
 * Direct3D 11 renderers' pictures, as the core option reicast_pvr2_scaler
 * asks: a compute shader's pass in place over the picture (pvr_compute.h's
 * PVRC_K_YFILTER, in Direct3D 11's terms), before what goes over it.
 * Compiled in Init(), when the renderer starts or changes size (a change
 * of the option is one); nothing is made in a frame, and a frame without a
 * write-out does nothing. Needs typed loads of R8G8B8A8_UNORM from an
 * unordered access view (Direct3D 11.3's TypedUAVLoadAdditionalFormats). */
#pragma once
#include <d3d11.h>
#include "types.h"
#include "comptr.h"

class DX11Shaders;

class DX11WriteOut
{
public:
	/* Made, or taken down, as the option and the device have it; true if
	 * the pictures are to be made for it (D3D11_BIND_UNORDERED_ACCESS) */
	bool Init(ID3D11Device *device, ID3D11DeviceContext *deviceContext, DX11Shaders *shaders);
	void Term();
	bool Ready() const { return (bool)shader; }
	/* The pass over the picture @uav, @width x @height, of the frame in
	 * the registers now, when it has one; nothing bound is left bound */
	void Record(ID3D11UnorderedAccessView *uav, u32 width, u32 height);

private:
	ID3D11DeviceContext *deviceContext = nullptr;
	ComPtr<ID3D11ComputeShader> shader;
	ComPtr<ID3D11Buffer> constants;
};
