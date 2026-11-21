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
#pragma once
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string.h>
#include <retro_inline.h>
#include <retro_miscellaneous.h>
#include "types.h"
#include <libretro.h>
#include <libretro_d3d.h>
#include "dx11_renderstate.h"
#include "rend/vram_budget.h"
#include "rend/TexCache.h"
#include "rend/hdr/hdr_encode_d3d11.h"

/* (the calls these bind with, as C has them, for the C++ that includes
 * this: the renderers not yet C) */
#ifdef __cplusplus
#define ID3D11DeviceContext_IASetInputLayout(This, a0) (This)->IASetInputLayout(a0)
#define ID3D11DeviceContext_IASetIndexBuffer(This, a0,a1,a2) (This)->IASetIndexBuffer(a0,a1,a2)
#define ID3D11DeviceContext_IASetVertexBuffers(This, a0,a1,a2,a3,a4) (This)->IASetVertexBuffers(a0,a1,a2,a3,a4)
#define ID3D11DeviceContext_IASetPrimitiveTopology(This, a0) (This)->IASetPrimitiveTopology(a0)
#define ID3D11DeviceContext_VSSetShader(This, a0,a1,a2) (This)->VSSetShader(a0,a1,a2)
#define ID3D11DeviceContext_PSSetShader(This, a0,a1,a2) (This)->PSSetShader(a0,a1,a2)
#define ID3D11DeviceContext_VSSetConstantBuffers(This, a0,a1,a2) (This)->VSSetConstantBuffers(a0,a1,a2)
#define ID3D11DeviceContext_PSSetConstantBuffers(This, a0,a1,a2) (This)->PSSetConstantBuffers(a0,a1,a2)
#define ID3D11DeviceContext_PSSetShaderResources(This, a0,a1,a2) (This)->PSSetShaderResources(a0,a1,a2)
#define ID3D11DeviceContext_PSSetSamplers(This, a0,a1,a2) (This)->PSSetSamplers(a0,a1,a2)
#define ID3D11DeviceContext_RSSetState(This, a0) (This)->RSSetState(a0)
#define ID3D11DeviceContext_RSSetViewports(This, a0,a1) (This)->RSSetViewports(a0,a1)
#define ID3D11DeviceContext_RSSetScissorRects(This, a0,a1) (This)->RSSetScissorRects(a0,a1)
#define ID3D11DeviceContext_OMSetBlendState(This, a0,a1,a2) (This)->OMSetBlendState(a0,a1,a2)
#define ID3D11DeviceContext_OMSetDepthStencilState(This, a0,a1) (This)->OMSetDepthStencilState(a0,a1)
#define ID3D11DeviceContext_OMSetRenderTargets(This, a0,a1,a2) (This)->OMSetRenderTargets(a0,a1,a2)
#define ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(This, a0,a1,a2,a3,a4,a5,a6) (This)->OMSetRenderTargetsAndUnorderedAccessViews(a0,a1,a2,a3,a4,a5,a6)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A COM object of the renderer's given back, and the pointer to it NULL */
#define DX11_RELEASE(p) do { if ((p) != NULL) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)
/* @dst made @src, which it holds a reference to (the one it had given back) */
#define DX11_ASSIGN(dst, src) do { IUnknown *dx11_src_ = (IUnknown *)(src); \
	if (dx11_src_ != NULL) IUnknown_AddRef(dx11_src_); \
	DX11_RELEASE(dst); \
	*(void **)&(dst) = dx11_src_; } while (0)

/* The frontend's Direct3D 11 device, from RETRO_HW_CONTEXT_D3D11's
 * interface, and what the renderers share of it.
 *
 * With version 1 of the interface the frame is the shader resource view
 * left at the pixel shader's slot 0 when the frame is handed over. With
 * version 2 the context is the frontend's as well, taken in turns
 * (lock_context), the frame is named (set_texture), and the frontend may
 * still be reading a frame when the next one is drawn: a picture for each
 * of its sync indexes. */

#define DX11C_RESOURCE_SLOTS 6
/* The quad's rectangles go one after the other into its buffer, and it is
 * started again (discarded) only when it is full */
#define DX11C_QUAD_COUNT 64

typedef struct dx11_context
{
	ID3D11Device *device;
	ID3D11DeviceContext *device_context;
	D3D_FEATURE_LEVEL feature_level;
	/* what video memory there was to use when the context was set up */
	VramBudget vram_budget;
	/* the adapter's PCI vendor id (0x10de: NVIDIA's), 0 when it is not known */
	UINT vendor;
	/* A deferred context (version 1, a frontend that draws on a thread of
	 * its own) records commands, and cannot hand anything back */
	bool immediate;
	pD3DCompile compile;
	HMODULE compiler_module;
	bool format_supported[5];
	bool mips_supported[5];

	/* times the frontend has used the context, and how many of them the
	 * context has been cleared for */
	u32 state_changes;
	u32 state_cleared;

	/* What is bound */
	struct
	{
		ID3D11InputLayout *layout;
		ID3D11Buffer *vertex_buffer[3];
		UINT stride[3];
		ID3D11Buffer *index_buffer;
		D3D11_PRIMITIVE_TOPOLOGY topology;
		ID3D11VertexShader *vs;
		ID3D11PixelShader *ps;
		ID3D11Buffer *vs_constants;
		ID3D11Buffer *ps_constants[3];
		ID3D11ShaderResourceView *srv[DX11C_RESOURCE_SLOTS];
		ID3D11SamplerState *sampler[4];
		ID3D11RasterizerState *raster;
		float viewport[2];
		bool viewport_set;
		D3D11_RECT scissor;
		bool scissor_set;
		ID3D11BlendState *blend;
		float blend_factor[4];
		ID3D11DepthStencilState *depth;
		UINT stencil_ref;
		ID3D11RenderTargetView *target;
		ID3D11DepthStencilView *depth_target;
		ID3D11UnorderedAccessView *uavs[3];
		bool uavs_set;
	} bound;

	ID3D11Texture2D *frame_texture;
	ID3D11ShaderResourceView *frame_view;
	/* the HDR output (Color Format), when the frontend took it: what the frame is encoded through */
	hdr_d3d11_t *hdr;
	bool handed_over;

	/* the quad: a picture drawn onto what is bound, as a rectangle */
	struct
	{
		ID3D11VertexShader *vertex_shader;
		ID3D11PixelShader *pixel_shader;
		ID3D11InputLayout *input_layout;
		ID3D11Buffer *vertex_buffer;
		ID3D11RasterizerState *rasterizer;
		ID3D11DepthStencilState *depth_stencil;
		ID3D11BlendState *opaque;
		UINT next;
	} quad;

	/* version 2 */
	bool v2;
	void *handle;
	bool (*lock_context)(void *);
	void (*unlock_context)(void *);
	void (*set_texture)(void *, ID3D11Texture2D *);
	unsigned (*get_sync_index)(void *);
	unsigned (*get_sync_index_mask)(void *);
	void (*wait_sync_index)(void *);
	int lock_depth;
} dx11_context;

extern dx11_context dx11c;

/* The frontend has made its context (context_reset) */
bool dx11c_init(const struct retro_hw_render_interface_d3d11 *iface);
/* ...and is about to destroy it, or the game is going */
void dx11c_term(void);

/* Unordered access views in pixel shaders, and a context that can be read back from */
static INLINE bool dx11c_has_per_pixel(void)
{
	return dx11c.device != NULL && dx11c.feature_level >= D3D_FEATURE_LEVEL_11_0 && dx11c.immediate;
}
/* Whether the 16-bit formats can be sampled, and the smaller levels of
 * a texture made on the card (TEX_TYPE_*) */
static INLINE bool dx11c_format_supported(int type)
{
	return (unsigned)type < 5 && dx11c.format_supported[type];
}
static INLINE bool dx11c_can_generate_mips(int type)
{
	return (unsigned)type < 5 && dx11c.mips_supported[type];
}

/* The frontend draws with the context (version 1: in the video_refresh
 * the frame is handed over in): nothing bound is what this core bound.
 * The context is cleared when the core next takes it (dx11c_lock()). */
static INLINE void dx11c_state_changed(void)
{
	dx11c.state_changes++;
}

/* Version 2: the context, taken from the frontend and given back.
 * May be nested. With version 1 only what the frontend has done to the
 * context is looked at. */
void dx11c_lock(void);
void dx11c_unlock(void);

/* ---- What is bound ----
 *
 * Everything this core binds goes through these, and is set only when
 * it differs from what is bound already: from one render to the next
 * as well, for as long as the frontend has not used the context.
 * (dx11c_lock() clears the context when the frontend has, and this then
 * knows exactly what is bound: nothing.) The geometry shader is never
 * anything but none. */
static INLINE void dx11c_set_input_layout(ID3D11InputLayout *layout)
{
	if (layout != dx11c.bound.layout)
	{
		ID3D11DeviceContext_IASetInputLayout(dx11c.device_context, layout);
		dx11c.bound.layout = layout;
	}
}
static INLINE void dx11c_set_index_buffer(ID3D11Buffer *index_buffer)
{
	if (index_buffer != dx11c.bound.index_buffer)
	{
		ID3D11DeviceContext_IASetIndexBuffer(dx11c.device_context, index_buffer, DXGI_FORMAT_R32_UINT, 0);
		dx11c.bound.index_buffer = index_buffer;
	}
}
/* A buffer at vertex slots 0 and 1 each, with those strides, and the index buffer (or none) */
static INLINE void dx11c_set_geometry2(ID3D11Buffer *buffer0, UINT stride0, ID3D11Buffer *buffer1, UINT stride1,
		ID3D11Buffer *index_buffer)
{
	if (buffer0 != dx11c.bound.vertex_buffer[0] || buffer1 != dx11c.bound.vertex_buffer[1]
			|| stride0 != dx11c.bound.stride[0] || stride1 != dx11c.bound.stride[1])
	{
		ID3D11Buffer *buffers[2];
		UINT strides[2];
		UINT offsets[2] = { 0, 0 };
		buffers[0] = buffer0;
		buffers[1] = buffer1;
		strides[0] = stride0;
		strides[1] = stride1;
		ID3D11DeviceContext_IASetVertexBuffers(dx11c.device_context, 0, 2, buffers, strides, offsets);
		dx11c.bound.vertex_buffer[0] = buffer0;
		dx11c.bound.vertex_buffer[1] = buffer1;
		dx11c.bound.stride[0] = stride0;
		dx11c.bound.stride[1] = stride1;
	}
	dx11c_set_index_buffer(index_buffer);
}
/* ...the same buffer at both */
static INLINE void dx11c_set_geometry(ID3D11Buffer *buffer, UINT stride0, UINT stride1, ID3D11Buffer *index_buffer)
{
	dx11c_set_geometry2(buffer, stride0, buffer, stride1, index_buffer);
}
static INLINE void dx11c_set_vertex_buffer(ID3D11Buffer *buffer, UINT stride, UINT slot)
{
	if (buffer != dx11c.bound.vertex_buffer[slot] || stride != dx11c.bound.stride[slot])
	{
		const UINT offset = 0;
		ID3D11DeviceContext_IASetVertexBuffers(dx11c.device_context, slot, 1, &buffer, &stride, &offset);
		dx11c.bound.vertex_buffer[slot] = buffer;
		dx11c.bound.stride[slot] = stride;
	}
}
static INLINE void dx11c_set_topology(D3D11_PRIMITIVE_TOPOLOGY topology)
{
	if (topology != dx11c.bound.topology)
	{
		ID3D11DeviceContext_IASetPrimitiveTopology(dx11c.device_context, topology);
		dx11c.bound.topology = topology;
	}
}
static INLINE void dx11c_set_vs(ID3D11VertexShader *vs)
{
	if (vs != dx11c.bound.vs)
	{
		ID3D11DeviceContext_VSSetShader(dx11c.device_context, vs, NULL, 0);
		dx11c.bound.vs = vs;
	}
}
static INLINE void dx11c_set_ps(ID3D11PixelShader *ps)
{
	if (ps != dx11c.bound.ps)
	{
		ID3D11DeviceContext_PSSetShader(dx11c.device_context, ps, NULL, 0);
		dx11c.bound.ps = ps;
	}
}
static INLINE void dx11c_set_vs_constants(ID3D11Buffer *buffer)
{
	if (buffer != dx11c.bound.vs_constants)
	{
		ID3D11DeviceContext_VSSetConstantBuffers(dx11c.device_context, 0, 1, &buffer);
		dx11c.bound.vs_constants = buffer;
	}
}
static INLINE void dx11c_set_ps_constants(UINT slot, ID3D11Buffer *buffer)
{
	if (buffer != dx11c.bound.ps_constants[slot])
	{
		ID3D11DeviceContext_PSSetConstantBuffers(dx11c.device_context, slot, 1, &buffer);
		dx11c.bound.ps_constants[slot] = buffer;
	}
}
static INLINE void dx11c_set_ps_resource(UINT slot, ID3D11ShaderResourceView *view)
{
	if (view != dx11c.bound.srv[slot])
	{
		ID3D11DeviceContext_PSSetShaderResources(dx11c.device_context, slot, 1, &view);
		dx11c.bound.srv[slot] = view;
	}
}
/* Several at once: in one call, for those of them that differ */
static INLINE void dx11c_set_ps_constants_n(UINT start, UINT count, ID3D11Buffer * const *buffers)
{
	UINT first = count, last = 0, i;
	for (i = 0; i < count; i++)
		if (buffers[i] != dx11c.bound.ps_constants[start + i])
		{
			if (i < first)
				first = i;
			last = i;
		}
	if (first == count)
		return;
	ID3D11DeviceContext_PSSetConstantBuffers(dx11c.device_context, start + first, last - first + 1, buffers + first);
	for (i = first; i <= last; i++)
		dx11c.bound.ps_constants[start + i] = buffers[i];
}
static INLINE void dx11c_set_ps_resources(UINT start, UINT count, ID3D11ShaderResourceView * const *views)
{
	UINT first = count, last = 0, i;
	for (i = 0; i < count; i++)
		if (views[i] != dx11c.bound.srv[start + i])
		{
			if (i < first)
				first = i;
			last = i;
		}
	if (first == count)
		return;
	ID3D11DeviceContext_PSSetShaderResources(dx11c.device_context, start + first, last - first + 1, views + first);
	for (i = first; i <= last; i++)
		dx11c.bound.srv[start + i] = views[i];
}
static INLINE void dx11c_set_ps_sampler(UINT slot, ID3D11SamplerState *sampler)
{
	if (sampler != dx11c.bound.sampler[slot])
	{
		ID3D11DeviceContext_PSSetSamplers(dx11c.device_context, slot, 1, &sampler);
		dx11c.bound.sampler[slot] = sampler;
	}
}
/* Pixel shader resources 0 to @count - 1 unbound */
static INLINE void dx11c_unbind_ps_resources(UINT count)
{
	ID3D11ShaderResourceView *none[DX11C_RESOURCE_SLOTS];
	UINT first = count, i;
	for (i = 0; i < count; i++)
		if (dx11c.bound.srv[i] != NULL)
		{
			first = i;
			break;
		}
	if (first == count)
		return;
	memset(none, 0, sizeof(none));
	ID3D11DeviceContext_PSSetShaderResources(dx11c.device_context, first, count - first, none);
	for (i = first; i < count; i++)
		dx11c.bound.srv[i] = NULL;
}
static INLINE void dx11c_set_rasterizer(ID3D11RasterizerState *state)
{
	if (state != dx11c.bound.raster)
	{
		ID3D11DeviceContext_RSSetState(dx11c.device_context, state);
		dx11c.bound.raster = state;
	}
}
static INLINE void dx11c_set_viewport(float width, float height)
{
	if (!dx11c.bound.viewport_set || width != dx11c.bound.viewport[0] || height != dx11c.bound.viewport[1])
	{
		D3D11_VIEWPORT vp;
		vp.TopLeftX = 0.f;
		vp.TopLeftY = 0.f;
		vp.Width = width;
		vp.Height = height;
		vp.MinDepth = 0.f;
		vp.MaxDepth = 1.f;
		ID3D11DeviceContext_RSSetViewports(dx11c.device_context, 1, &vp);
		dx11c.bound.viewport[0] = width;
		dx11c.bound.viewport[1] = height;
		dx11c.bound.viewport_set = true;
	}
}
static INLINE void dx11c_set_scissor(const D3D11_RECT *rect)
{
	if (!dx11c.bound.scissor_set || rect->left != dx11c.bound.scissor.left || rect->top != dx11c.bound.scissor.top
			|| rect->right != dx11c.bound.scissor.right || rect->bottom != dx11c.bound.scissor.bottom)
	{
		ID3D11DeviceContext_RSSetScissorRects(dx11c.device_context, 1, rect);
		dx11c.bound.scissor = *rect;
		dx11c.bound.scissor_set = true;
	}
}
/* (@factor NULL: what the state is given without one, all ones) */
static INLINE void dx11c_set_blend(ID3D11BlendState *state, const float *factor)
{
	static const float ones[4] = { 1.f, 1.f, 1.f, 1.f };
	if (factor == NULL)
		factor = ones;
	if (state != dx11c.bound.blend || memcmp(factor, dx11c.bound.blend_factor, sizeof(dx11c.bound.blend_factor)))
	{
		ID3D11DeviceContext_OMSetBlendState(dx11c.device_context, state, factor, 0xffffffff);
		dx11c.bound.blend = state;
		memcpy(dx11c.bound.blend_factor, factor, sizeof(dx11c.bound.blend_factor));
	}
}
static INLINE void dx11c_set_depth_stencil(ID3D11DepthStencilState *state, UINT ref)
{
	if (state != dx11c.bound.depth || ref != dx11c.bound.stencil_ref)
	{
		ID3D11DeviceContext_OMSetDepthStencilState(dx11c.device_context, state, ref);
		dx11c.bound.depth = state;
		dx11c.bound.stencil_ref = ref;
	}
}
/* A view of a texture bound to be drawn into is unbound by Direct3D
 * from wherever it is bound to be read: the views at the slots textures
 * and pictures go to are not known after that */
static INLINE void dx11c_targets_changed(ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth)
{
	ID3D11ShaderResourceView * const unknown = (ID3D11ShaderResourceView *)(uintptr_t)1;
	dx11c.bound.target = target;
	dx11c.bound.depth_target = depth;
	dx11c.bound.srv[0] = dx11c.bound.srv[3] = dx11c.bound.srv[4] = unknown;
}
/* One render target (or none) and the depth and stencil (or none).
 * Unordered access views bound before are not looked at again. */
static INLINE void dx11c_set_targets(ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth)
{
	if (target != dx11c.bound.target || depth != dx11c.bound.depth_target || dx11c.bound.uavs_set)
	{
		ID3D11DeviceContext_OMSetRenderTargets(dx11c.device_context, target != NULL ? 1 : 0,
				target != NULL ? &target : NULL, depth);
		dx11c_targets_changed(target, depth);
		dx11c.bound.uavs_set = false;
	}
}
/* ...and with the unordered access views at slots 1 to 3, their counters kept */
static INLINE void dx11c_set_targets_and_uavs(ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth,
		ID3D11UnorderedAccessView * const uavs[3])
{
	if (target != dx11c.bound.target || depth != dx11c.bound.depth_target || !dx11c.bound.uavs_set
			|| memcmp(uavs, dx11c.bound.uavs, sizeof(dx11c.bound.uavs)))
	{
		const UINT counts[3] = { (UINT)-1, (UINT)-1, (UINT)-1 };
		ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(dx11c.device_context, 1, &target, depth, 1, 3,
				(ID3D11UnorderedAccessView **)uavs, counts);
		dx11c_targets_changed(target, depth);
		memcpy(dx11c.bound.uavs, uavs, sizeof(dx11c.bound.uavs));
		dx11c.bound.uavs_set = true;
	}
}

/* A picture onto what is bound: at x, y, w, h in clip space (y going
 * up), from (u0, v0) to (u1, v1) of the texture (v going down). Without a
 * blend state, it is put there as it is. */
void dx11c_draw_quad_at(ID3D11ShaderResourceView *view, ID3D11SamplerState *sampler,
		float x, float y, float w, float h, float u0, float v0, float u1, float v1,
		ID3D11BlendState *blend, const float *blend_factor);
/* ...all of it over all of the target */
#define dx11c_draw_quad(view, sampler) \
	dx11c_draw_quad_at(view, sampler, -1.f, -1.f, 2.f, 2.f, 0.f, 0.f, 1.f, 1.f, NULL, NULL)

/* The picture the next video_refresh is to show. With version 2 it is
 * handed over here, with the context taken: from then on it is the
 * frontend's (dx11c_is_handed_over()), until the frame has been shown. */
void dx11c_set_frame(ID3D11Texture2D *texture, ID3D11ShaderResourceView *view);
static INLINE bool dx11c_is_handed_over(const ID3D11Texture2D *texture)
{
	return dx11c.v2 && dx11c.handed_over && texture == dx11c.frame_texture;
}
/* Just before video_refresh(RETRO_HW_FRAME_BUFFER_VALID): it is handed over */
void dx11c_present(void);
/* How many pictures the screen is to be drawn in turn into, and which
 * of them the next frame is. Before drawing into it, the frontend is
 * waited for to be done with it - never with the context taken. */
unsigned dx11c_present_count(void);
unsigned dx11c_present_index(void);
void dx11c_wait_present_index(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* The context taken for as long as this lives (dx11c_lock()): the C++
 * renderers' */
struct DX11Lock
{
	DX11Lock() { dx11c_lock(); }
	~DX11Lock() { dx11c_unlock(); }
};
#endif
