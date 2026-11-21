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
#include <d3d9.h>
#include <d3dcompiler.h>
#include <string.h>
#include <retro_inline.h>
#include <retro_miscellaneous.h>
#include "types.h"
#include <libretro.h>
#include <libretro_d3d9.h>
#include "rend/TexCache.h"
#include "rend/vram_budget.h"
#include "dx9_shaders.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A COM object of the renderer's given back, and the pointer to it NULL */
#define DX9_RELEASE(p) do { if ((p) != NULL) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)
/* @dst made @src, which it holds a reference to (the one it had given back) */
#define DX9_ASSIGN(dst, src) do { IUnknown *dx9_src_ = (IUnknown *)(src); \
	if (dx9_src_ != NULL) IUnknown_AddRef(dx9_src_); \
	DX9_RELEASE(dst); \
	*(void **)&(dst) = dx9_src_; } while (0)

/* What the stencil buffer is used for (dx11/dx11_renderstate.h): the
 * modifier volumes' bits 0 and 1, bit 7 for a polygon that takes shadows,
 * bit 6 for a pixel drawn again for its shadow (rend/shadows.h). */
enum
{
	DX9Stencil_None,
	DX9Stencil_Shadow,
	DX9Stencil_ShadowLaterWins,
	DX9Stencil_ShadowEarlierWins,
	DX9Stencil_Xor,
	DX9Stencil_Or,
	DX9Stencil_Inclusion,
	DX9Stencil_Exclusion,
	DX9Stencil_Final
};

/* The frontend's Direct3D 9 device, from RETRO_HW_CONTEXT_D3D9's
 * interface (libretro_d3d9.h), and what the renderer shares of it.
 *
 * The device is the frontend's as well, taken in turns (dx9c_lock()), and
 * the frame is a picture of this core's handed over as it is
 * (dx9c_set_frame()): one for each of the frontend's sync indexes, each
 * drawn into again only once the frontend is done with it.
 *
 * Everything the renderer sets on the device goes through here and is
 * set only when it differs from what the device has: from one render to
 * the next as well, for as long as the frontend has not used the device.
 * When it has, nothing is known of what it left, and the renderer's base
 * state is put back in one call (a state block). */

/* (render states known: up to the last the renderer sets) */
#define DX9C_RENDER_STATES (D3DRS_BLENDOPALPHA + 1)
#define DX9C_UNKNOWN 0xfefefefe
/* Textures released, to be made into new ones of the same kind */
#define DX9C_MAX_SPARES 64
#define DX9C_MAX_SPARE_BYTES (32 * 1024 * 1024)
/* The quad's vertices, one rectangle after another, started again
 * (discarded) when full */
#define DX9C_QUAD_COUNT 64

typedef struct dx9_spare
{
	u32 width, height, levels;
	DWORD usage;
	D3DFORMAT format;
	u32 bytes;
	IDirect3DTexture9 *texture;
} dx9_spare;

typedef struct dx9_context
{
	IDirect3DDevice9 *device;
	bool ex;
	VramBudget vram_budget;
	pD3DCompile compile;
	HMODULE compiler_module;
	bool format_supported[5];
	bool mips_supported[5];
	bool dynamic_textures;
	bool separate_alpha;
	D3DFORMAT depth_format;
	DWORD max_anisotropy;
	DWORD extra_targets;		/* render targets 1 and on */

	void *handle;
	bool (*lock_context)(void *);
	void (*unlock_context)(void *);
	void (*set_texture)(void *, LPDIRECT3DTEXTURE9);
	unsigned (*get_sync_index)(void *);
	unsigned (*get_sync_index_mask)(void *);
	void (*wait_sync_index)(void *);
	int lock_depth;
	bool in_scene;
	u32 epoch;

	/* What the device has, as far as this knows */
	DWORD render_state[DX9C_RENDER_STATES];
	struct
	{
		IDirect3DBaseTexture9 *texture[3];
		u32 sampler[3];
		IDirect3DVertexShader9 *vs;
		IDirect3DPixelShader9 *ps;
		IDirect3DVertexDeclaration9 *decl;
		IDirect3DVertexBuffer9 *stream;
		UINT stride;
		IDirect3DIndexBuffer9 *indices;
		u32 bools;
		IDirect3DSurface9 *target;
		IDirect3DSurface9 *depth;
		u32 viewport[2];
		RECT scissor;
		bool scissor_set;
		u32 depth_key;
		u32 blend_key;
	} bound;
	/* What the renderer never changes, set again in one call */
	IDirect3DStateBlock9 *base_state;

	/* oldest first */
	dx9_spare spares[DX9C_MAX_SPARES];
	u32 spare_count;
	u32 spare_bytes;

	IDirect3DVertexBuffer9 *quad_buffer;
	IDirect3DVertexDeclaration9 *quad_decl;
	UINT quad_next;

	IDirect3DTexture9 *frame_texture;
	bool handed_over;
} dx9_context;

extern dx9_context dx9c;

/* The frontend has made its device (context_reset) */
bool dx9c_init(const struct retro_hw_render_interface_d3d9 *iface);
/* ...and is about to release it, or reset it, or the game is going */
void dx9c_term(void);

/* Whether a 16-bit format can be sampled; and whether the card makes
 * a texture's smaller levels (by TEX_TYPE_*) */
static INLINE bool dx9c_format_supported(int type)
{
	return (unsigned)type < 5 && dx9c.format_supported[type];
}
static INLINE bool dx9c_can_generate_mips(int type)
{
	return (unsigned)type < 5 && dx9c.mips_supported[type];
}
D3DFORMAT dx9c_texture_format(int type);
/* Textures written by the processor: dynamic ones, written where they
 * are; or, on a device without them, managed ones. What to lock one the
 * processor writes all of with (D3DLOCK_DISCARD where it can), and to
 * make it with. */
static INLINE DWORD dx9c_write_lock_flags(void)
{
	return dx9c.dynamic_textures ? D3DLOCK_DISCARD : 0;
}
static INLINE DWORD dx9c_write_usage(void)
{
	return dx9c.dynamic_textures ? D3DUSAGE_DYNAMIC : 0;
}

/* The device, taken from the frontend and given back. May be nested;
 * the scene begun inside is ended when it is given back. */
void dx9c_lock(void);
void dx9c_unlock(void);
/* Draws go between these. StretchRect(), GetRenderTargetData() and
 * the like outside: dx9c_end_scene() first. */
bool dx9c_begin_scene(void);
void dx9c_end_scene(void);

/* ---- What is set ---- */
static INLINE void dx9c_set_render_state(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((u32)state < DX9C_RENDER_STATES)
	{
		if (dx9c.render_state[state] == value)
			return;
		dx9c.render_state[state] = value;
	}
	IDirect3DDevice9_SetRenderState(dx9c.device, state, value);
}
static INLINE void dx9c_set_texture(DWORD stage, IDirect3DBaseTexture9 *texture)
{
	if (texture != dx9c.bound.texture[stage])
	{
		IDirect3DDevice9_SetTexture(dx9c.device, stage, texture);
		dx9c.bound.texture[stage] = texture;
	}
}
/* (a texture, as the base texture it is) */
#define DX9_BASE(t) ((IDirect3DBaseTexture9 *)(t))
/* A sampler's filter, addressing and level of detail bias, as one word:
 * see dx9c_sampler_key() */
void dx9c_set_sampler(DWORD stage, u32 key);
static INLINE u32 dx9c_sampler_key(bool linear, u32 anisotropy, D3DTEXTUREADDRESS u, D3DTEXTUREADDRESS v, u32 lod_bias)
{
	return (linear ? 1u : 0u) | ((anisotropy & 31) << 1) | ((u32)u << 6) | ((u32)v << 9) | ((lod_bias & 15) << 12) | 0x10000;
}
/* Clamped and nearest, or linear: pictures drawn whole */
#define DX9C_POINT_SAMPLER dx9c_sampler_key(false, 1, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, 0)
#define DX9C_LINEAR_SAMPLER dx9c_sampler_key(true, 1, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, 0)
static INLINE void dx9c_set_vs(IDirect3DVertexShader9 *vs)
{
	if (vs != dx9c.bound.vs)
	{
		IDirect3DDevice9_SetVertexShader(dx9c.device, vs);
		dx9c.bound.vs = vs;
	}
}
static INLINE void dx9c_set_ps(IDirect3DPixelShader9 *ps)
{
	if (ps != dx9c.bound.ps)
	{
		IDirect3DDevice9_SetPixelShader(dx9c.device, ps);
		dx9c.bound.ps = ps;
	}
}
static INLINE void dx9c_set_decl(IDirect3DVertexDeclaration9 *decl)
{
	if (decl != dx9c.bound.decl)
	{
		IDirect3DDevice9_SetVertexDeclaration(dx9c.device, decl);
		dx9c.bound.decl = decl;
	}
}
static INLINE void dx9c_set_stream(IDirect3DVertexBuffer9 *buffer, UINT stride)
{
	if (buffer != dx9c.bound.stream || stride != dx9c.bound.stride)
	{
		IDirect3DDevice9_SetStreamSource(dx9c.device, 0, buffer, 0, stride);
		dx9c.bound.stream = buffer;
		dx9c.bound.stride = stride;
	}
}
static INLINE void dx9c_set_indices(IDirect3DIndexBuffer9 *indices)
{
	if (indices != dx9c.bound.indices)
	{
		IDirect3DDevice9_SetIndices(dx9c.device, indices);
		dx9c.bound.indices = indices;
	}
}
/* The pixel shaders' flags, as bits (DX9B_*) */
static INLINE void dx9c_set_bools(u32 bits)
{
	if (bits != dx9c.bound.bools)
	{
		float values[16];
		int i;
		for (i = 0; i < 16; i++)
			values[i] = (float)((bits >> i) & 1);
		IDirect3DDevice9_SetPixelShaderConstantF(dx9c.device, DX9PC_Flags, values, 4);
		dx9c.bound.bools = bits;
	}
}
/* What is drawn into, @width by @height of it from its top left corner
 * (the viewport, and half a pixel of it for the vertex shaders) */
void dx9c_set_target(IDirect3DSurface9 *target, IDirect3DSurface9 *depth, u32 width, u32 height);
/* Within the viewport. False, and nothing set, if nothing of it is:
 * then nothing is to be drawn. */
static INLINE bool dx9c_set_scissor(const RECT *r)
{
	RECT rect;
	rect.left = r->left > 0 ? r->left : 0;
	rect.top = r->top > 0 ? r->top : 0;
	rect.right = r->right < (LONG)dx9c.bound.viewport[0] ? r->right : (LONG)dx9c.bound.viewport[0];
	rect.bottom = r->bottom < (LONG)dx9c.bound.viewport[1] ? r->bottom : (LONG)dx9c.bound.viewport[1];
	if (rect.right <= rect.left || rect.bottom <= rect.top)
		return false;
	if (!dx9c.bound.scissor_set || rect.left != dx9c.bound.scissor.left || rect.top != dx9c.bound.scissor.top
			|| rect.right != dx9c.bound.scissor.right || rect.bottom != dx9c.bound.scissor.bottom)
	{
		IDirect3DDevice9_SetScissorRect(dx9c.device, &rect);
		dx9c.bound.scissor = rect;
		dx9c.bound.scissor_set = true;
	}
	return true;
}
/* What is drawn into cleared, all of it */
void dx9c_clear(DWORD flags, D3DCOLOR color);

/* The depth and the stencil, as the Vulkan renderers' pipelines use
 * them: by key (DX9Stencil_*), set when the key changes */
void dx9c_set_depth_stencil(bool depth_test, bool depth_write, int depth_func, int stencil, DWORD ref);
/* Blending by the TSP's instructions (@src, @dst), or none */
void dx9c_set_blend(bool enable, u32 src, u32 dst, bool write_color);
#define dx9c_set_no_blend() dx9c_set_blend(false, 1, 0, true)
/* By the source's alpha: the crosshairs, the VMUs' screens, shadows */
void dx9c_set_alpha_blend(void);

/* ---- Pictures ---- */

/* A texture of the renderer's: one released before of the same kind
 * if there is one, or a new one; NULL if none can be made */
IDirect3DTexture9 *dx9c_make_texture(u32 width, u32 height, u32 levels, DWORD usage, D3DFORMAT format);
/* ...and given back, to be used again (*@texture NULL after) */
void dx9c_recycle(IDirect3DTexture9 **texture);

/* A picture onto what is bound: at x, y, w, h in clip space (y going
 * up), from (u0, v0) to (u1, v1) of the texture (v going down).
 * Blended by its alpha times @alpha, or put there as it is. */
void dx9c_draw_quad_at(IDirect3DTexture9 *texture, bool linear, bool swap_rb,
		float x, float y, float w, float h,
		float u0, float v0, float u1, float v1, bool blend, float alpha);
/* ...all of it over all of the target */
#define dx9c_draw_quad(texture, linear, swap_rb) \
	dx9c_draw_quad_at(texture, linear, swap_rb, -1.f, -1.f, 2.f, 2.f, 0.f, 0.f, 1.f, 1.f, false, 1.f)

/* ---- The frame ---- */

/* The picture the next video_refresh is to show, handed over here,
 * with the device taken: from then on it is the frontend's
 * (dx9c_is_handed_over()) until the frame has been shown */
void dx9c_set_frame(IDirect3DTexture9 *texture);
static INLINE bool dx9c_is_handed_over(const IDirect3DTexture9 *texture)
{
	return dx9c.handed_over && texture == dx9c.frame_texture;
}
/* Just before video_refresh: whether a frame was handed over for it */
bool dx9c_present(void);
/* How many pictures the screen is drawn in turn into, and which of
 * them the next frame is. Before drawing into it, the frontend is
 * waited for to be done with it - never with the device taken. */
unsigned dx9c_present_count(void);
unsigned dx9c_present_index(void);
void dx9c_wait_present_index(void);

#ifdef __cplusplus
}
#endif
