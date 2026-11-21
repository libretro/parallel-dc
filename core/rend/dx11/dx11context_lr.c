/*
	Copyright 2021 flyinghead

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
#include "dx11context_lr.h"
#include "dx11_shaders.h"
#include "rend/hdr/hdr_output.h"
#include <dxgi.h>
#include <dxgi1_4.h>
#include "rend/dxgi_budget.h"
#include "libretro/lr_contexts.h"

dx11_context dx11c;

/* (IDXGIDevice's, as dxgi.h has it: here, so that nothing more is linked for it) */
static const IID dx11_iid_dxgi_device = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };

static const DXGI_FORMAT textureFormats[5] = {
	DXGI_FORMAT_B5G6R5_UNORM,		/* TEX_TYPE_565 */
	DXGI_FORMAT_B5G5R5A1_UNORM,		/* TEX_TYPE_5551 */
	DXGI_FORMAT_B4G4R4A4_UNORM,		/* TEX_TYPE_4444 */
	DXGI_FORMAT_R8G8B8A8_UNORM,		/* TEX_TYPE_8888 */
	DXGI_FORMAT_R8_UNORM			/* TEX_TYPE_8 */
};

/* What ClearState() leaves */
static void clear_bound(void)
{
	int i;
	memset(&dx11c.bound, 0, sizeof(dx11c.bound));
	dx11c.bound.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	for (i = 0; i < 4; i++)
		dx11c.bound.blend_factor[i] = 1.f;
}

/* ---- The quad ---- */

typedef struct dx11_quad_vertex
{
	float x, y, u, v;
} dx11_quad_vertex;

static bool quad_init(void)
{
	ID3D11Device *device = dx11c.device;
	ID3DBlob *blob;
	D3D11_INPUT_ELEMENT_DESC layout[2];
	D3D11_RASTERIZER_DESC rast;
	D3D11_DEPTH_STENCIL_DESC ds;
	D3D11_BLEND_DESC blend;
	D3D11_BUFFER_DESC desc;

	dx11c.quad.vertex_shader = dx11_shaders_quad_vs();
	dx11c.quad.pixel_shader = dx11_shaders_quad_ps();
	blob = dx11_shaders_quad_vs_blob();
	if (dx11c.quad.vertex_shader == NULL || dx11c.quad.pixel_shader == NULL || blob == NULL)
		return false;

	memset(layout, 0, sizeof(layout));
	layout[0].SemanticName = "POSITION";
	layout[0].Format = DXGI_FORMAT_R32G32_FLOAT;
	layout[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
	layout[1].SemanticName = "TEXCOORD";
	layout[1].Format = DXGI_FORMAT_R32G32_FLOAT;
	layout[1].AlignedByteOffset = 2 * sizeof(float);
	layout[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
	if (FAILED(ID3D11Device_CreateInputLayout(device, layout, 2, ID3D10Blob_GetBufferPointer(blob),
			ID3D10Blob_GetBufferSize(blob), &dx11c.quad.input_layout)))
	{
		dx11c.quad.input_layout = NULL;
		return false;
	}

	memset(&rast, 0, sizeof(rast));
	rast.FillMode = D3D11_FILL_SOLID;
	rast.CullMode = D3D11_CULL_NONE;
	rast.DepthClipEnable = TRUE;
	if (FAILED(ID3D11Device_CreateRasterizerState(device, &rast, &dx11c.quad.rasterizer)))
	{
		dx11c.quad.rasterizer = NULL;
		return false;
	}

	memset(&ds, 0, sizeof(ds));
	ds.DepthEnable = FALSE;
	ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
	ds.StencilEnable = FALSE;
	ds.FrontFace.StencilFailOp = ds.FrontFace.StencilDepthFailOp = ds.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	ds.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	ds.BackFace = ds.FrontFace;
	if (FAILED(ID3D11Device_CreateDepthStencilState(device, &ds, &dx11c.quad.depth_stencil)))
	{
		dx11c.quad.depth_stencil = NULL;
		return false;
	}

	memset(&blend, 0, sizeof(blend));
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(ID3D11Device_CreateBlendState(device, &blend, &dx11c.quad.opaque)))
	{
		dx11c.quad.opaque = NULL;
		return false;
	}

	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = sizeof(dx11_quad_vertex) * 4 * DX11C_QUAD_COUNT;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	dx11c.quad.next = DX11C_QUAD_COUNT;
	if (FAILED(ID3D11Device_CreateBuffer(device, &desc, NULL, &dx11c.quad.vertex_buffer)))
	{
		dx11c.quad.vertex_buffer = NULL;
		return false;
	}
	return true;
}

static void quad_term(void)
{
	DX11_RELEASE(dx11c.quad.vertex_buffer);
	DX11_RELEASE(dx11c.quad.opaque);
	DX11_RELEASE(dx11c.quad.depth_stencil);
	DX11_RELEASE(dx11c.quad.rasterizer);
	DX11_RELEASE(dx11c.quad.input_layout);
	dx11c.quad.pixel_shader = NULL;
	dx11c.quad.vertex_shader = NULL;
}

void dx11c_draw_quad_at(ID3D11ShaderResourceView *view, ID3D11SamplerState *sampler,
		float x, float y, float w, float h, float u0, float v0, float u1, float v1,
		ID3D11BlendState *blend, const float *blend_factor)
{
	ID3D11DeviceContext *ctx = dx11c.device_context;
	dx11_quad_vertex vertices[4];
	D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (dx11c.quad.vertex_buffer == NULL)
		return;
	vertices[0].x = x;     vertices[0].y = y;     vertices[0].u = u0; vertices[0].v = v1;
	vertices[1].x = x;     vertices[1].y = y + h; vertices[1].u = u0; vertices[1].v = v0;
	vertices[2].x = x + w; vertices[2].y = y;     vertices[2].u = u1; vertices[2].v = v1;
	vertices[3].x = x + w; vertices[3].y = y + h; vertices[3].u = u1; vertices[3].v = v0;
	/* (a deferred context's commands are each a list of their own) */
	if (dx11c.quad.next >= DX11C_QUAD_COUNT || !dx11c.immediate)
	{
		mode = D3D11_MAP_WRITE_DISCARD;
		dx11c.quad.next = 0;
	}
	if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)dx11c.quad.vertex_buffer, 0, mode, 0, &mapped)))
		return;
	memcpy((dx11_quad_vertex *)mapped.pData + dx11c.quad.next * 4, vertices, sizeof(vertices));
	ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)dx11c.quad.vertex_buffer, 0);

	dx11c_set_input_layout(dx11c.quad.input_layout);
	dx11c_set_vertex_buffer(dx11c.quad.vertex_buffer, sizeof(dx11_quad_vertex), 0);
	dx11c_set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx11c_set_vs(dx11c.quad.vertex_shader);
	dx11c_set_ps(dx11c.quad.pixel_shader);
	dx11c_set_rasterizer(dx11c.quad.rasterizer);
	dx11c_set_depth_stencil(dx11c.quad.depth_stencil, 0);
	dx11c_set_blend(blend != NULL ? blend : dx11c.quad.opaque, blend_factor);
	dx11c_set_ps_resource(0, view);
	dx11c_set_ps_sampler(0, sampler);
	ID3D11DeviceContext_Draw(ctx, 4, dx11c.quad.next * 4);
	dx11c.quad.next++;
}

/* ---- The context ---- */

bool dx11c_init(const struct retro_hw_render_interface_d3d11 *iface)
{
	int i;
	dx11c_term();
	if (iface == NULL || iface->device == NULL || iface->context == NULL)
		return false;

	ID3D11Device_AddRef(iface->device);
	dx11c.device = iface->device;
	ID3D11DeviceContext_AddRef(iface->context);
	dx11c.device_context = iface->context;
	dx11c.feature_level = ID3D11Device_GetFeatureLevel(dx11c.device);
	dx11c.immediate = ID3D11DeviceContext_GetType(dx11c.device_context) == D3D11_DEVICE_CONTEXT_IMMEDIATE;

	dx11c.compile = iface->D3DCompile;
	if (dx11c.compile == NULL)
	{
		/* (a frontend that does not hand one over: the system's) */
		dx11c.compiler_module = LoadLibraryA("d3dcompiler_47.dll");
		if (dx11c.compiler_module != NULL)
			dx11c.compile = (pD3DCompile)GetProcAddress(dx11c.compiler_module, "D3DCompile");
		/* (the renderers start with the core's bytecode, and draw every
		 * polygon with the general shaders: dx11_shaders.h) */
		if (dx11c.compile == NULL)
			WARN_LOG(RENDERER, "DX11: no shader compiler (d3dcompiler_47.dll): polygons drawn with the general shaders\n");
	}

	if (iface->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2
			&& iface->lock_context != NULL && iface->unlock_context != NULL && iface->set_texture != NULL
			&& iface->get_sync_index != NULL && iface->get_sync_index_mask != NULL && iface->wait_sync_index != NULL)
	{
		dx11c.v2 = true;
		dx11c.handle = iface->handle;
		dx11c.lock_context = iface->lock_context;
		dx11c.unlock_context = iface->unlock_context;
		dx11c.set_texture = iface->set_texture;
		dx11c.get_sync_index = iface->get_sync_index;
		dx11c.get_sync_index_mask = iface->get_sync_index_mask;
		dx11c.wait_sync_index = iface->wait_sync_index;
	}

	{
		IDXGIDevice *dxgiDevice = NULL;
		dx11c.vendor = 0;
		memset(&dx11c.vram_budget, 0, sizeof(dx11c.vram_budget));
		if (SUCCEEDED(ID3D11Device_QueryInterface(dx11c.device, &dx11_iid_dxgi_device, (void **)&dxgiDevice)))
		{
			IDXGIAdapter *adapter = NULL;
			DXGI_ADAPTER_DESC desc;
			if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgiDevice, &adapter)))
			{
				const bool haveDesc = SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc));
				if (haveDesc)
					dx11c.vendor = desc.VendorId;
				dx11c.vram_budget = dxgi_vram_budget(adapter, haveDesc ? &desc : NULL);
				IDXGIAdapter_Release(adapter);
			}
			IDXGIDevice_Release(dxgiDevice);
		}
		NOTICE_LOG(RENDERER, "DX11: feature level %x, vendor %04x, %s context, interface version %d\n",
				(unsigned)dx11c.feature_level, dx11c.vendor, dx11c.immediate ? "immediate" : "deferred", dx11c.v2 ? 2 : 1);
		vram_budget_say("DX11", &dx11c.vram_budget);
	}
	if (!dx11c.immediate)
		WARN_LOG(RENDERER, "DX11: the frontend's context is a deferred one: renders to textures cannot reach video memory\n");

	for (i = 0; i < 5; i++)
	{
		UINT support = 0;
		const UINT sample = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
		const UINT mips = D3D11_FORMAT_SUPPORT_MIP_AUTOGEN | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
		if (FAILED(ID3D11Device_CheckFormatSupport(dx11c.device, textureFormats[i], &support)))
			support = 0;
		dx11c.format_supported[i] = (support & sample) == sample;
		dx11c.mips_supported[i] = dx11c.format_supported[i] && (support & mips) == mips;
	}
	/* what has to be, for the rest */
	if (!dx11c.format_supported[TEX_TYPE_8888] || !dx11c.format_supported[TEX_TYPE_8]
			|| dx11c.feature_level < D3D_FEATURE_LEVEL_10_0)
	{
		ERROR_LOG(RENDERER, "DX11: feature level 10.0 or later needed\n");
		dx11c_term();
		return false;
	}

	/* the HDR output, when the frontend took it (libretro.cpp) */
	if (hdr_output.format != HDR_OUTPUT_NONE)
	{
		dx11c.hdr = hdr_d3d11_new(dx11c.device, hdr_output.format == HDR_OUTPUT_SCRGB
				? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM);
		if (dx11c.hdr == NULL)
			ERROR_LOG(RENDERER, "DX11: the HDR output could not be set up (feature level 11.0 needed); the picture goes out as 24-bit\n");
		else
			NOTICE_LOG(RENDERER, "DX11: HDR output, %s\n", hdr_output.format == HDR_OUTPUT_SCRGB ? "scRGB (RGBA16F)" : "HDR10 (R10G10B10A2)");
	}

	/* (what is bound is not known until the context is first taken and cleared) */
	dx11c.state_changes = dx11c.state_cleared + 1;
	dx11_shaders_init(dx11c.device, dx11c.compile);
	if (!quad_init())
	{
		ERROR_LOG(RENDERER, "DX11: initialization failed\n");
		dx11c_term();
		return false;
	}
	return true;
}

void dx11c_term(void)
{
	DX11_RELEASE(dx11c.frame_view);
	DX11_RELEASE(dx11c.frame_texture);
	dx11c.handed_over = false;
	hdr_d3d11_free(dx11c.hdr);
	dx11c.hdr = NULL;
	quad_term();
	dx11_blend_term();
	dx11_samplers_term();
	dx11_shaders_term();
	if (dx11c.device_context != NULL && dx11c.immediate)
	{
		dx11c_lock();
		ID3D11DeviceContext_ClearState(dx11c.device_context);
		ID3D11DeviceContext_Flush(dx11c.device_context);
		dx11c_unlock();
	}
	DX11_RELEASE(dx11c.device_context);
	DX11_RELEASE(dx11c.device);
	if (dx11c.compiler_module != NULL)
		FreeLibrary(dx11c.compiler_module);
	dx11c.compiler_module = NULL;
	dx11c.compile = NULL;
	dx11c.immediate = false;
	dx11c.vendor = 0;
	dx11c.v2 = false;
	dx11c.handle = NULL;
	dx11c.lock_context = NULL;
	dx11c.unlock_context = NULL;
	dx11c.set_texture = NULL;
	dx11c.get_sync_index = NULL;
	dx11c.get_sync_index_mask = NULL;
	dx11c.wait_sync_index = NULL;
	dx11c.lock_depth = 0;
}

void dx11c_lock(void)
{
	if (dx11c.lock_depth++ != 0)
		return;
	if (dx11c.v2 && dx11c.lock_context(dx11c.handle))
		/* the frontend has drawn with it since: nothing is as it was left */
		dx11c.state_changes++;
	if (dx11c.state_changes != dx11c.state_cleared && dx11c.device_context != NULL)
	{
		ID3D11DeviceContext_ClearState(dx11c.device_context);
		clear_bound();
		dx11c.state_cleared = dx11c.state_changes;
	}
}

void dx11c_unlock(void)
{
	if (dx11c.lock_depth > 0 && --dx11c.lock_depth == 0 && dx11c.v2)
		dx11c.unlock_context(dx11c.handle);
}

void dx11c_set_frame(ID3D11Texture2D *texture, ID3D11ShaderResourceView *view)
{
	/* the HDR output: the frame encoded, with the context taken by the
	 * render that drew it, and that handed over in its place */
	if (dx11c.hdr != NULL && texture != NULL && view != NULL)
	{
		D3D11_TEXTURE2D_DESC desc;
		ID3D11Texture2D *encoded = NULL;
		ID3D11ShaderResourceView *encodedView = NULL;
		ID3D11Texture2D_GetDesc(texture, &desc);
		if (hdr_d3d11_encode(dx11c.hdr, dx11c.device_context, &hdr_output, view, desc.Width, desc.Height, true,
				dx11c.v2 ? dx11c_present_index() : 0, &encoded, &encodedView))
		{
			texture = encoded;
			view = encodedView;
		}
		/* what the encode bound is not what this has noted */
		ID3D11DeviceContext_ClearState(dx11c.device_context);
		clear_bound();
	}
	DX11_ASSIGN(dx11c.frame_texture, texture);
	DX11_ASSIGN(dx11c.frame_view, view);
	/* (with the context taken, by the render that drew it: no other lock
	 * of it is needed for this) */
	dx11c.handed_over = dx11c.v2 && texture != NULL;
	if (dx11c.handed_over)
		dx11c.set_texture(dx11c.handle, texture);
}

void dx11c_present(void)
{
	if (dx11c.device_context == NULL)
		return;
	if (dx11c.v2)
	{
		/* (a frame with no render to the screen in it shows the last one again) */
		if (!dx11c.handed_over)
		{
			dx11c_lock();
			dx11c.set_texture(dx11c.handle, dx11c.frame_texture);
			dx11c_unlock();
		}
		/* from here on the frontend has what it was handed: the next
		 * render waits for it to be done with it (dx11c_wait_present_index()) */
		dx11c.handed_over = false;
	}
	else
	{
		dx11c_lock();
		/* (a view of a picture that is also bound to be drawn into is not
		 * bound) */
		dx11c_set_targets(NULL, NULL);
		dx11c_set_ps_resource(0, dx11c.frame_view);
		/* the frontend draws with the context in video_refresh */
		dx11c_state_changed();
		dx11c_unlock();
	}
}

unsigned dx11c_present_count(void)
{
	unsigned mask, count = 1;
	if (!dx11c.v2)
		return 1;
	mask = dx11c.get_sync_index_mask(dx11c.handle);
	while (count < 32 && (mask >> count) != 0)
		count++;
	return count;
}

unsigned dx11c_present_index(void)
{
	unsigned index;
	if (!dx11c.v2)
		return 0;
	index = dx11c.get_sync_index(dx11c.handle);
	return index < 32 ? index : 0;
}

void dx11c_wait_present_index(void)
{
	if (dx11c.v2 && dx11c.lock_depth == 0)
		dx11c.wait_sync_index(dx11c.handle);
}

/* ---- The frontend's (libretro.c) ---- */

bool lr_dx11_init(const void *iface)
{
	return dx11c_init((const struct retro_hw_render_interface_d3d11 *)iface);
}

void lr_dx11_term(void)
{
	dx11c_term();
}

void lr_dx11_present(void)
{
	dx11c_present();
}
