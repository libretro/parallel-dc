/* The chip's write-out for the Direct3D 11 renderers: see dx11_writeout.h */
#include "dx11_writeout.h"
#include <string.h>
#include "dx11context_lr.h"
#include "dx11_shaders.h"
#include "rend/tile/compute_frame.h"

/* the pass's flags, as pvr_compute.h has them */
typedef char dx11_writeout_flags_check[PVRC_FF_YON == 0x4000u ? 1 : -1];

static struct
{
	bool tried;
	ID3D11ComputeShader *shader;
	ID3D11Buffer *constants;
} wo;

void dx11_writeout_term(void)
{
	DX11_RELEASE(wo.shader);
	DX11_RELEASE(wo.constants);
	wo.tried = false;
}

static bool make(void)
{
	ID3D11Device *device = dx11c.device;
	D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs;
	D3D11_BUFFER_DESC desc;
	memset(&fs, 0, sizeof(fs));
	fs.InFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (device == NULL || ID3D11Device_GetFeatureLevel(device) < D3D_FEATURE_LEVEL_11_0
			|| FAILED(ID3D11Device_CheckFeatureSupport(device, D3D11_FEATURE_FORMAT_SUPPORT2, &fs, sizeof(fs)))
			|| (fs.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) == 0)
	{
		WARN_LOG(RENDERER, "DX11: no typed loads of the picture: the PowerVR2 write-out pass is left out\n");
		return false;
	}
	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = 16;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (dx11_shaders_writeout_cs(&wo.shader) == NULL
			|| FAILED(ID3D11Device_CreateBuffer(device, &desc, NULL, &wo.constants)))
	{
		WARN_LOG(RENDERER, "DX11: the PowerVR2 write-out pass could not be made: the picture goes without it\n");
		wo.constants = NULL;
		DX11_RELEASE(wo.shader);
		return false;
	}
	return true;
}

/* Made the first time, as the renderer starts, whether the option is on or
 * not: turning it on while a game runs (a change of size) makes nothing on
 * the draw path (AGENTS.md). Whether it is used is the option's. */
bool dx11_writeout_init(void)
{
	if (!wo.tried)
	{
		wo.tried = true;
		make();
	}
	return wo.shader != NULL && !compute_frame_scalers_off;
}

void dx11_writeout_record(ID3D11UnorderedAccessView *uav, u32 width, u32 height)
{
	ID3D11DeviceContext *ctx = dx11c.device_context;
	ID3D11UnorderedAccessView *none = NULL;
	u32 flags, words[4];
	if (wo.shader == NULL || uav == NULL)
		return;
	flags = compute_frame_write_out(height);
	if (PVRC_FF_YLINES(flags) == 0)
		return;
	words[0] = width;
	words[1] = height;
	words[2] = flags;
	words[3] = 0;
	ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)wo.constants, 0, NULL, words, 0, 0);
	ID3D11DeviceContext_CSSetShader(ctx, wo.shader, NULL, 0);
	ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &wo.constants);
	ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav, NULL);
	ID3D11DeviceContext_Dispatch(ctx, (width + 63) / 64, PVRC_FF_YLINES(flags), 1);
	ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &none, NULL);
	ID3D11DeviceContext_CSSetShader(ctx, NULL, NULL, 0);
}
