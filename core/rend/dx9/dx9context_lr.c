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
#include "dx9context_lr.h"
#include "libretro/lr_contexts.h"

dx9_context dx9c;

D3DFORMAT dx9c_texture_format(int type)
{
	switch (type)
	{
	case TEX_TYPE_565:
		return D3DFMT_R5G6B5;
	case TEX_TYPE_5551:
		return D3DFMT_A1R5G5B5;
	case TEX_TYPE_4444:
		return D3DFMT_A4R4G4B4;
	case TEX_TYPE_8888:
		/* (R G B A bytes, which the card reads as B G R A: the shaders swap them) */
		return D3DFMT_A8R8G8B8;
	case TEX_TYPE_8:
		return D3DFMT_A8;
	default:
		return D3DFMT_UNKNOWN;
	}
}

/* (IDirect3DDevice9Ex's, as d3d9.h has it: here, so that nothing more is
 * linked for it) */
static const IID dx9_iid_device_ex = { 0xb18b10ce, 0x2649, 0x405a, { 0x87, 0x0f, 0x95, 0xf7, 0x77, 0xd4, 0x31, 0x3a } };

static void dx9c_forget(void);
static void dx9c_set_base(bool record);

/* (D3DOK_NOAUTOGEN is a success that is not one) */
static bool check_format(IDirect3D9 *d3d, const D3DDEVICE_CREATION_PARAMETERS *params, const D3DDISPLAYMODE *mode,
		DWORD usage, D3DRESOURCETYPE type, D3DFORMAT format)
{
	return IDirect3D9_CheckDeviceFormat(d3d, params->AdapterOrdinal, params->DeviceType, mode->Format, usage, type, format) == D3D_OK;
}

static bool dx9c_init_locked(const struct retro_hw_render_interface_d3d9 *iface)
{
	static const D3DVERTEXELEMENT9 quadElements[] = {
		{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		D3DDECL_END()
	};
	IDirect3DDevice9Ex *deviceEx = NULL;
	D3DCAPS9 caps;

	IDirect3DDevice9_AddRef(iface->device);
	dx9c.device = iface->device;
	dx9c.ex = SUCCEEDED(IDirect3DDevice9_QueryInterface(dx9c.device, &dx9_iid_device_ex, (void **)&deviceEx));
	if (deviceEx != NULL)
		IDirect3DDevice9Ex_Release(deviceEx);

	if (FAILED(IDirect3DDevice9_GetDeviceCaps(dx9c.device, &caps)))
	{
		ERROR_LOG(RENDERER, "DX9: no device capabilities\n");
		return false;
	}
	dx9c.extra_targets = caps.NumSimultaneousRTs > 1 ? (caps.NumSimultaneousRTs < 4 ? caps.NumSimultaneousRTs : 4) - 1 : 0;
	dx9c.dynamic_textures = (caps.Caps2 & D3DCAPS2_DYNAMICTEXTURES) != 0;
	dx9c.separate_alpha = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND) != 0;
	dx9c.max_anisotropy = caps.MaxAnisotropy > 1 ? caps.MaxAnisotropy : 1;
	if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0)
			|| caps.MaxVertexIndex <= 0xffff
			|| ((caps.TextureCaps & D3DPTEXTURECAPS_POW2) && !(caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL))
			|| (dx9c.ex && !dx9c.dynamic_textures))
	{
		ERROR_LOG(RENDERER, "DX9: shader model 3, 32-bit indexes and textures of any size needed (shaders %x/%x, max index %x)\n",
				(unsigned)caps.VertexShaderVersion, (unsigned)caps.PixelShaderVersion, (unsigned)caps.MaxVertexIndex);
		return false;
	}

	/* The formats, as the adapter has them for the mode it shows */
	{
		static const D3DFORMAT depths[] = { D3DFMT_D24FS8, D3DFMT_D24S8 };
		IDirect3D9 *d3d = NULL;
		D3DDEVICE_CREATION_PARAMETERS params;
		D3DDISPLAYMODE mode;
		int i;
		bool ok = SUCCEEDED(IDirect3DDevice9_GetDirect3D(dx9c.device, &d3d)) && d3d != NULL
				&& SUCCEEDED(IDirect3DDevice9_GetCreationParameters(dx9c.device, &params))
				&& SUCCEEDED(IDirect3D9_GetAdapterDisplayMode(d3d, params.AdapterOrdinal, &mode));
		if (ok)
		{
			for (i = 0; i < 5; i++)
			{
				const D3DFORMAT format = dx9c_texture_format(i);
				dx9c.format_supported[i] = check_format(d3d, &params, &mode, dx9c_write_usage(), D3DRTYPE_TEXTURE, format);
				dx9c.mips_supported[i] = dx9c.format_supported[i]
						&& check_format(d3d, &params, &mode, dx9c_write_usage() | D3DUSAGE_AUTOGENMIPMAP, D3DRTYPE_TEXTURE, format);
			}
			ok = check_format(d3d, &params, &mode, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
			/* (a float for the depth where there is one, as the other renderers have it) */
			dx9c.depth_format = D3DFMT_UNKNOWN;
			for (i = 0; i < (int)ARRAY_SIZE(depths); i++)
				if (check_format(d3d, &params, &mode, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, depths[i])
						&& SUCCEEDED(IDirect3D9_CheckDepthStencilMatch(d3d, params.AdapterOrdinal, params.DeviceType, mode.Format,
								D3DFMT_A8R8G8B8, depths[i])))
				{
					dx9c.depth_format = depths[i];
					break;
				}
		}
		if (d3d != NULL)
			IDirect3D9_Release(d3d);
		if (!ok || dx9c.depth_format == D3DFMT_UNKNOWN || !dx9c.format_supported[TEX_TYPE_8888]
				|| !dx9c.format_supported[TEX_TYPE_8])
		{
			ERROR_LOG(RENDERER, "DX9: the formats needed are not there\n");
			return false;
		}
	}

	memset(&dx9c.vram_budget, 0, sizeof(dx9c.vram_budget));
	dx9c.vram_budget.budget_kib = vram_kib(IDirect3DDevice9_GetAvailableTextureMem(dx9c.device));
	NOTICE_LOG(RENDERER, "DX9: %s device, interface version %u, %s textures, depth %s, 16-bit formats %d%d%d\n",
			dx9c.ex ? "Direct3D9Ex" : "Direct3D 9", iface->interface_version, dx9c.dynamic_textures ? "dynamic" : "managed",
			dx9c.depth_format == D3DFMT_D24FS8 ? "D24FS8" : "D24S8",
			dx9c.format_supported[0], dx9c.format_supported[1], dx9c.format_supported[2]);
	vram_budget_say("DX9", &dx9c.vram_budget);

	if (!dx9_shaders_init(dx9c.device, dx9c.compile))
	{
		ERROR_LOG(RENDERER, "DX9: no shaders (no compiler, or it failed)\n");
		return false;
	}

	if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dx9c.device, quadElements, &dx9c.quad_decl))
			|| FAILED(IDirect3DDevice9_CreateVertexBuffer(dx9c.device, DX9C_QUAD_COUNT * 4 * 4 * sizeof(float),
					D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &dx9c.quad_buffer, NULL)))
	{
		ERROR_LOG(RENDERER, "DX9: initialization failed\n");
		return false;
	}
	dx9c.quad_next = DX9C_QUAD_COUNT;

	/* The base state, recorded */
	if (SUCCEEDED(IDirect3DDevice9_BeginStateBlock(dx9c.device)))
	{
		IDirect3DStateBlock9 *block = NULL;
		dx9c_set_base(true);
		if (FAILED(IDirect3DDevice9_EndStateBlock(dx9c.device, &block)))
			block = NULL;
		DX9_RELEASE(dx9c.base_state);
		dx9c.base_state = block;
	}
	dx9c_forget();
	return true;
}

bool dx9c_init(const struct retro_hw_render_interface_d3d9 *iface)
{
	bool ok;
	dx9c_term();
	/* (any version from the first on: a later one only adds to it) */
	if (iface == NULL || iface->device == NULL || iface->interface_version < 1
			|| iface->lock_context == NULL || iface->unlock_context == NULL || iface->set_texture == NULL
			|| iface->get_sync_index == NULL || iface->get_sync_index_mask == NULL || iface->wait_sync_index == NULL)
		return false;

	dx9c.handle = iface->handle;
	dx9c.lock_context = iface->lock_context;
	dx9c.unlock_context = iface->unlock_context;
	dx9c.set_texture = iface->set_texture;
	dx9c.get_sync_index = iface->get_sync_index;
	dx9c.get_sync_index_mask = iface->get_sync_index_mask;
	dx9c.wait_sync_index = iface->wait_sync_index;

	dx9c.compile = (pD3DCompile)iface->D3DCompile;
	if (dx9c.compile == NULL)
	{
		/* (a frontend that found none: the system's) */
		static const char * const names[] = { "d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll" };
		int i;
		for (i = 0; i < (int)ARRAY_SIZE(names); i++)
		{
			dx9c.compiler_module = LoadLibraryA(names[i]);
			if (dx9c.compiler_module == NULL)
				continue;
			dx9c.compile = (pD3DCompile)GetProcAddress(dx9c.compiler_module, "D3DCompile");
			if (dx9c.compile != NULL)
				break;
			FreeLibrary(dx9c.compiler_module);
			dx9c.compiler_module = NULL;
		}
	}

	dx9c_lock();
	ok = dx9c_init_locked(iface);
	dx9c_unlock();
	if (!ok)
		dx9c_term();
	return ok;
}

/* What the renderer relies on and never changes */
static void dx9c_set_base(bool record)
{
	static const DWORD states[][2] = {
		{ D3DRS_FILLMODE, D3DFILL_SOLID },
		{ D3DRS_LIGHTING, FALSE },
		{ D3DRS_FOGENABLE, FALSE },
		{ D3DRS_SPECULARENABLE, FALSE },
		{ D3DRS_CLIPPING, TRUE },
		{ D3DRS_CLIPPLANEENABLE, 0 },
		{ D3DRS_SRGBWRITEENABLE, FALSE },
		{ D3DRS_MULTISAMPLEANTIALIAS, FALSE },
		{ D3DRS_ALPHATESTENABLE, FALSE },
		{ D3DRS_BLENDOP, D3DBLENDOP_ADD },
		{ D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD },
		{ D3DRS_TWOSIDEDSTENCILMODE, FALSE },
		{ D3DRS_SCISSORTESTENABLE, TRUE },
		{ D3DRS_DEPTHBIAS, 0 },
		{ D3DRS_SLOPESCALEDEPTHBIAS, 0 },
		{ D3DRS_WRAP0, 0 },
		{ D3DRS_DITHERENABLE, FALSE },
		{ D3DRS_POINTSPRITEENABLE, FALSE },
		{ D3DRS_ANTIALIASEDLINEENABLE, FALSE }
	};
	IDirect3DDevice9 *device = dx9c.device;
	DWORD stage;
	size_t i;
	for (i = 0; i < ARRAY_SIZE(states); i++)
	{
		if (record)
			IDirect3DDevice9_SetRenderState(device, (D3DRENDERSTATETYPE)states[i][0], states[i][1]);
		dx9c.render_state[states[i][0]] = states[i][1];
	}
	if (record)
	{
		IDirect3DDevice9_SetStreamSourceFreq(device, 0, 1);
		for (stage = 0; stage < 3; stage++)
		{
			IDirect3DDevice9_SetTexture(device, stage, NULL);
			IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MAXMIPLEVEL, 0);
			IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_SRGBTEXTURE, 0);
			IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_ELEMENTINDEX, 0);
			IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
			/* the palette's and the fog table's: nearest, clamped */
			if (stage != DX9S_Texture)
			{
				IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
				IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
				IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
				IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
				IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
				IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MIPMAPLODBIAS, 0);
				IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MAXANISOTROPY, 1);
			}
		}
	}
	for (stage = 0; stage < 3; stage++)
		dx9c.bound.texture[stage] = NULL;
	dx9c.bound.sampler[DX9S_Palette] = dx9c.bound.sampler[DX9S_Fog] = DX9C_POINT_SAMPLER;
}

/* Nothing is known: the frontend has had the device */
static void dx9c_forget(void)
{
	DWORD i;
	int stage;
	dx9c.epoch++;
	for (i = 0; i < DX9C_RENDER_STATES; i++)
		dx9c.render_state[i] = DX9C_UNKNOWN;
	memset(&dx9c.bound, 0, sizeof(dx9c.bound));
	/* (what is known not to be anything this sets) */
	dx9c.bound.vs = (IDirect3DVertexShader9 *)(uintptr_t)1;
	dx9c.bound.ps = (IDirect3DPixelShader9 *)(uintptr_t)1;
	dx9c.bound.decl = (IDirect3DVertexDeclaration9 *)(uintptr_t)1;
	dx9c.bound.stream = (IDirect3DVertexBuffer9 *)(uintptr_t)1;
	dx9c.bound.indices = (IDirect3DIndexBuffer9 *)(uintptr_t)1;
	dx9c.bound.target = (IDirect3DSurface9 *)(uintptr_t)1;
	dx9c.bound.depth = (IDirect3DSurface9 *)(uintptr_t)1;
	dx9c.bound.bools = ~0u;
	dx9c.bound.depth_key = ~0u;
	dx9c.bound.blend_key = ~0u;
	for (stage = 0; stage < 3; stage++)
		dx9c.bound.texture[stage] = (IDirect3DBaseTexture9 *)(uintptr_t)1;
	if (dx9c.device == NULL)
		return;
	if (dx9c.base_state != NULL)
	{
		IDirect3DStateBlock9_Apply(dx9c.base_state);
		dx9c_set_base(false);
	}
	else
		dx9c_set_base(true);
	/* (no more than the one target the renderer draws into) */
	for (i = 1; i <= dx9c.extra_targets; i++)
		IDirect3DDevice9_SetRenderTarget(dx9c.device, i, NULL);
}

void dx9c_term(void)
{
	u32 i;
	DX9_RELEASE(dx9c.frame_texture);
	dx9c.handed_over = false;
	if (dx9c.device != NULL)
	{
		IDirect3DDevice9 *device = dx9c.device;
		IDirect3DSurface9 *back = NULL;
		DWORD stage;
		dx9c_lock();
		/* Nothing of this core's left bound to the device, which keeps
		 * what is bound: a device is not reset while any of its default
		 * pool resources is there. (The frontend drops the frame it has
		 * of this core's.) */
		dx9c.set_texture(dx9c.handle, NULL);
		dx9c_end_scene();
		if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back != NULL)
		{
			IDirect3DDevice9_SetRenderTarget(device, 0, back);
			IDirect3DSurface9_Release(back);
		}
		IDirect3DDevice9_SetDepthStencilSurface(device, NULL);
		for (stage = 0; stage < 3; stage++)
			IDirect3DDevice9_SetTexture(device, stage, NULL);
		IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
		IDirect3DDevice9_SetIndices(device, NULL);
		IDirect3DDevice9_SetVertexShader(device, NULL);
		IDirect3DDevice9_SetPixelShader(device, NULL);
		IDirect3DDevice9_SetVertexDeclaration(device, NULL);
		dx9c_forget();
		dx9c_unlock();
	}
	for (i = 0; i < dx9c.spare_count; i++)
		DX9_RELEASE(dx9c.spares[i].texture);
	dx9c.spare_count = 0;
	dx9c.spare_bytes = 0;
	DX9_RELEASE(dx9c.quad_buffer);
	DX9_RELEASE(dx9c.quad_decl);
	DX9_RELEASE(dx9c.base_state);
	dx9_shaders_term();
	DX9_RELEASE(dx9c.device);
	if (dx9c.compiler_module != NULL)
		FreeLibrary(dx9c.compiler_module);
	dx9c.compiler_module = NULL;
	dx9c.compile = NULL;
	dx9c.ex = false;
	dx9c.handle = NULL;
	dx9c.lock_context = NULL;
	dx9c.unlock_context = NULL;
	dx9c.set_texture = NULL;
	dx9c.get_sync_index = NULL;
	dx9c.get_sync_index_mask = NULL;
	dx9c.wait_sync_index = NULL;
	dx9c.extra_targets = 0;
	dx9c.in_scene = false;
}

void dx9c_lock(void)
{
	if (dx9c.lock_depth++ != 0 || dx9c.lock_context == NULL)
		return;
	if (dx9c.lock_context(dx9c.handle))
		/* the frontend has drawn with it since: nothing is as it was left */
		dx9c_forget();
}

void dx9c_unlock(void)
{
	if (dx9c.lock_depth <= 0 || --dx9c.lock_depth != 0)
		return;
	dx9c_end_scene();
	if (dx9c.unlock_context != NULL)
		dx9c.unlock_context(dx9c.handle);
}

bool dx9c_begin_scene(void)
{
	if (dx9c.in_scene)
		return true;
	dx9c.in_scene = dx9c.device != NULL && SUCCEEDED(IDirect3DDevice9_BeginScene(dx9c.device));
	return dx9c.in_scene;
}

void dx9c_end_scene(void)
{
	if (!dx9c.in_scene)
		return;
	IDirect3DDevice9_EndScene(dx9c.device);
	dx9c.in_scene = false;
}

#define SAMPLER_FIELD(k, shift, mask) (((k) >> (shift)) & (mask))

void dx9c_set_sampler(DWORD stage, u32 key)
{
	IDirect3DDevice9 *device = dx9c.device;
	const u32 old = dx9c.bound.sampler[stage];
	bool known, linear;
	u32 anisotropy;
	if (key == old)
		return;
	known = (old & 0x10000) != 0;
	linear = (key & 1) != 0;
	anisotropy = SAMPLER_FIELD(key, 1, 31);
	if (anisotropy > dx9c.max_anisotropy)
		anisotropy = dx9c.max_anisotropy;
	if (anisotropy < 1)
		anisotropy = 1;
	if (!known || SAMPLER_FIELD(key, 0, 63) != SAMPLER_FIELD(old, 0, 63))
	{
		IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
		IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MINFILTER,
				!linear ? D3DTEXF_POINT : anisotropy > 1 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR);
		IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MAXANISOTROPY, anisotropy);
		if (!known)
			/* (the level nearest, as the other renderers: trilinear filtering is two passes) */
			IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
	}
	if (!known || SAMPLER_FIELD(key, 6, 7) != SAMPLER_FIELD(old, 6, 7))
		IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_ADDRESSU, SAMPLER_FIELD(key, 6, 7));
	if (!known || SAMPLER_FIELD(key, 9, 7) != SAMPLER_FIELD(old, 9, 7))
		IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_ADDRESSV, SAMPLER_FIELD(key, 9, 7));
	if (!known || SAMPLER_FIELD(key, 12, 15) != SAMPLER_FIELD(old, 12, 15))
	{
		const float bias = D_Adjust_LoD_Bias[SAMPLER_FIELD(key, 12, 15)];
		DWORD bits;
		memcpy(&bits, &bias, sizeof(bits));
		IDirect3DDevice9_SetSamplerState(device, stage, D3DSAMP_MIPMAPLODBIAS, bits);
	}
	dx9c.bound.sampler[stage] = key;
}

void dx9c_set_target(IDirect3DSurface9 *target, IDirect3DSurface9 *depth, u32 width, u32 height)
{
	IDirect3DDevice9 *device = dx9c.device;
	if (target != dx9c.bound.target)
	{
		IDirect3DDevice9_SetRenderTarget(device, 0, target);
		dx9c.bound.target = target;
		/* (which sets the viewport and the scissor rectangle to all of it) */
		dx9c.bound.viewport[0] = dx9c.bound.viewport[1] = 0;
		dx9c.bound.scissor_set = false;
	}
	if (depth != dx9c.bound.depth)
	{
		IDirect3DDevice9_SetDepthStencilSurface(device, depth);
		dx9c.bound.depth = depth;
	}
	if (width != dx9c.bound.viewport[0] || height != dx9c.bound.viewport[1])
	{
		D3DVIEWPORT9 viewport;
		float halfPixel[4];
		viewport.X = 0;
		viewport.Y = 0;
		viewport.Width = width;
		viewport.Height = height;
		viewport.MinZ = 0.f;
		viewport.MaxZ = 1.f;
		IDirect3DDevice9_SetViewport(device, &viewport);
		halfPixel[0] = -1.f / width;
		halfPixel[1] = 1.f / height;
		halfPixel[2] = 0.f;
		halfPixel[3] = 0.f;
		IDirect3DDevice9_SetVertexShaderConstantF(device, DX9VC_HalfPixel, halfPixel, 1);
		dx9c.bound.viewport[0] = width;
		dx9c.bound.viewport[1] = height;
	}
}

/* (the viewport's whole, within the scissor rectangle) */
static void scissor_all(void)
{
	RECT all;
	all.left = 0;
	all.top = 0;
	all.right = (LONG)dx9c.bound.viewport[0];
	all.bottom = (LONG)dx9c.bound.viewport[1];
	dx9c_set_scissor(&all);
}

void dx9c_clear(DWORD flags, D3DCOLOR color)
{
	scissor_all();
	IDirect3DDevice9_Clear(dx9c.device, 0, NULL, flags, color, 0.f, 0);
}

static const DWORD depthFuncs[] =
{
	D3DCMP_NEVER,				/* 0	Never */
	D3DCMP_LESS,				/* 1	Less */
	D3DCMP_EQUAL,				/* 2	Equal */
	D3DCMP_LESSEQUAL,			/* 3	Less Or Equal */
	D3DCMP_GREATER,				/* 4	Greater */
	D3DCMP_NOTEQUAL,			/* 5	Not Equal */
	D3DCMP_GREATEREQUAL,		/* 6	Greater Or Equal */
	D3DCMP_ALWAYS				/* 7	Always */
};

void dx9c_set_depth_stencil(bool depth_test, bool depth_write, int depth_func, int stencil, DWORD ref)
{
	const u32 key = (u32)depth_test | ((u32)depth_write << 1) | ((u32)(depth_func & 7) << 2) | ((u32)stencil << 5);
	if (key != dx9c.bound.depth_key)
	{
		dx9c_set_render_state(D3DRS_ZENABLE, depth_test ? D3DZB_TRUE : D3DZB_FALSE);
		dx9c_set_render_state(D3DRS_ZWRITEENABLE, depth_write);
		dx9c_set_render_state(D3DRS_ZFUNC, depthFuncs[depth_func & 7]);
		dx9c_set_render_state(D3DRS_STENCILENABLE, stencil != DX9Stencil_None);
		if (stencil != DX9Stencil_None)
		{
			DWORD fail = D3DSTENCILOP_KEEP, zfail = D3DSTENCILOP_KEEP, pass = D3DSTENCILOP_KEEP;
			DWORD func = D3DCMP_ALWAYS, readMask = 0xff, writeMask = 0xff;
			switch (stencil)
			{
			case DX9Stencil_Shadow:
				pass = D3DSTENCILOP_REPLACE;
				readMask = 0;
				writeMask = 0x80;
				break;
			case DX9Stencil_ShadowLaterWins:
				pass = D3DSTENCILOP_REPLACE;
				func = D3DCMP_EQUAL;
				readMask = 0x81;
				writeMask = 0x40;
				break;
			case DX9Stencil_ShadowEarlierWins:
				pass = D3DSTENCILOP_INVERT;
				func = D3DCMP_EQUAL;
				readMask = 0xC1;
				writeMask = 0x40;
				break;
			case DX9Stencil_Xor:
				pass = D3DSTENCILOP_INVERT;
				readMask = 0;
				writeMask = 2;
				break;
			case DX9Stencil_Or:
				pass = D3DSTENCILOP_REPLACE;
				readMask = 2;
				writeMask = 2;
				break;
			case DX9Stencil_Inclusion:
				fail = zfail = D3DSTENCILOP_ZERO;
				pass = D3DSTENCILOP_REPLACE;
				func = D3DCMP_LESSEQUAL;
				readMask = writeMask = 3;
				break;
			case DX9Stencil_Exclusion:
				fail = zfail = D3DSTENCILOP_ZERO;
				func = D3DCMP_EQUAL;
				readMask = writeMask = 3;
				break;
			case DX9Stencil_Final:
				/* takes shadows, in a volume, and not drawn again for it (bit 6);
				 * clears the volume bits and that mark */
				fail = zfail = pass = D3DSTENCILOP_ZERO;
				func = D3DCMP_EQUAL;
				readMask = 0xC1;
				writeMask = 0x43;
				break;
			}
			dx9c_set_render_state(D3DRS_STENCILFAIL, fail);
			dx9c_set_render_state(D3DRS_STENCILZFAIL, zfail);
			dx9c_set_render_state(D3DRS_STENCILPASS, pass);
			dx9c_set_render_state(D3DRS_STENCILFUNC, func);
			dx9c_set_render_state(D3DRS_STENCILMASK, readMask);
			dx9c_set_render_state(D3DRS_STENCILWRITEMASK, writeMask);
		}
		dx9c.bound.depth_key = key;
	}
	if (stencil != DX9Stencil_None)
		dx9c_set_render_state(D3DRS_STENCILREF, ref);
}

/* The TSP's instructions as Direct3D's factors. Those for the alpha
 * cannot be colours: other colour's alpha is the other's alpha. */
static const DWORD srcBlend[] =
{
	D3DBLEND_ZERO,
	D3DBLEND_ONE,
	D3DBLEND_DESTCOLOR,
	D3DBLEND_INVDESTCOLOR,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA
};
static const DWORD dstBlend[] =
{
	D3DBLEND_ZERO,
	D3DBLEND_ONE,
	D3DBLEND_SRCCOLOR,
	D3DBLEND_INVSRCCOLOR,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA
};
static const DWORD srcBlendAlpha[] =
{
	D3DBLEND_ZERO,
	D3DBLEND_ONE,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA
};
static const DWORD dstBlendAlpha[] =
{
	D3DBLEND_ZERO,
	D3DBLEND_ONE,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA
};

void dx9c_set_blend(bool enable, u32 src, u32 dst, bool write_color)
{
	u32 key;
	src &= 7;
	dst &= 7;
	if (!enable)
		src = dst = 0;
	key = (u32)enable | (src << 1) | (dst << 4) | ((u32)write_color << 7);
	if (key == dx9c.bound.blend_key)
		return;
	dx9c_set_render_state(D3DRS_ALPHABLENDENABLE, enable);
	if (enable)
	{
		dx9c_set_render_state(D3DRS_SRCBLEND, srcBlend[src]);
		dx9c_set_render_state(D3DRS_DESTBLEND, dstBlend[dst]);
		dx9c_set_render_state(D3DRS_SEPARATEALPHABLENDENABLE, dx9c.separate_alpha);
		if (dx9c.separate_alpha)
		{
			dx9c_set_render_state(D3DRS_SRCBLENDALPHA, srcBlendAlpha[src]);
			dx9c_set_render_state(D3DRS_DESTBLENDALPHA, dstBlendAlpha[dst]);
		}
	}
	dx9c_set_render_state(D3DRS_COLORWRITEENABLE, write_color ? 0xf : 0);
	dx9c.bound.blend_key = key;
}

void dx9c_set_alpha_blend(void)
{
	const u32 key = 0x100;
	if (key == dx9c.bound.blend_key)
		return;
	dx9c_set_render_state(D3DRS_ALPHABLENDENABLE, TRUE);
	dx9c_set_render_state(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	dx9c_set_render_state(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	dx9c_set_render_state(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	dx9c_set_render_state(D3DRS_COLORWRITEENABLE, 0xf);
	dx9c.bound.blend_key = key;
}

/* (spare @i gone from the list, the ones after it moved up) */
static void spare_remove(u32 i)
{
	memmove(&dx9c.spares[i], &dx9c.spares[i + 1], (dx9c.spare_count - i - 1) * sizeof(dx9c.spares[0]));
	dx9c.spare_count--;
}

IDirect3DTexture9 *dx9c_make_texture(u32 width, u32 height, u32 levels, DWORD usage, D3DFORMAT format)
{
	IDirect3DTexture9 *texture = NULL;
	D3DPOOL pool;
	HRESULT hr;
	u32 i;
	for (i = 0; i < dx9c.spare_count; i++)
	{
		dx9_spare *spare = &dx9c.spares[i];
		if (spare->width == width && spare->height == height && spare->levels == levels
				&& spare->usage == usage && spare->format == format)
		{
			texture = spare->texture;
			dx9c.spare_bytes -= spare->bytes;
			spare_remove(i);
			return texture;
		}
	}
	if (dx9c.device == NULL)
		return NULL;
	/* (the managed pool where the device has one and the processor writes
	 * the texture without dynamic ones) */
	pool = (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC)) != 0 || dx9c.ex ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;
	hr = IDirect3DDevice9_CreateTexture(dx9c.device, width, height, levels, usage, format, pool, &texture, NULL);
	if (FAILED(hr))
	{
		WARN_LOG(RENDERER, "DX9: texture creation failed (%x): %d x %d, %d levels, format %d\n", (unsigned)hr,
				width, height, levels, (int)format);
		texture = NULL;
	}
	return texture;
}

void dx9c_recycle(IDirect3DTexture9 **texture)
{
	D3DSURFACE_DESC desc;
	if (*texture == NULL)
		return;
	if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc(*texture, 0, &desc)) && (desc.Usage & D3DUSAGE_AUTOGENMIPMAP) == 0)
	{
		dx9_spare spare;
		u32 pixel;
		spare.width = desc.Width;
		spare.height = desc.Height;
		spare.levels = IDirect3DTexture9_GetLevelCount(*texture);
		spare.usage = desc.Usage;
		spare.format = desc.Format;
		pixel = desc.Format == D3DFMT_A8R8G8B8 ? 4 : desc.Format == D3DFMT_A8 ? 1 : 2;
		spare.bytes = desc.Width * desc.Height * pixel * (spare.levels > 1 ? 4 : 3) / 3;
		/* (the oldest go first, when there are too many to keep) */
		while (dx9c.spare_count != 0 && (dx9c.spare_count >= DX9C_MAX_SPARES
				|| dx9c.spare_bytes + spare.bytes > DX9C_MAX_SPARE_BYTES))
		{
			dx9c.spare_bytes -= dx9c.spares[0].bytes;
			DX9_RELEASE(dx9c.spares[0].texture);
			spare_remove(0);
		}
		if (spare.bytes <= DX9C_MAX_SPARE_BYTES)
		{
			spare.texture = *texture;
			*texture = NULL;
			dx9c.spare_bytes += spare.bytes;
			dx9c.spares[dx9c.spare_count++] = spare;
		}
	}
	DX9_RELEASE(*texture);
}

void dx9c_draw_quad_at(IDirect3DTexture9 *texture, bool linear, bool swap_rb,
		float x, float y, float w, float h, float u0, float v0, float u1, float v1, bool blend, float alpha)
{
	IDirect3DDevice9 *device = dx9c.device;
	float vertices[4][4];
	float color[4];
	DWORD flags = D3DLOCK_NOOVERWRITE;
	void *data;
	u32 known;
	if (dx9c.quad_buffer == NULL || texture == NULL || !dx9c_begin_scene())
		return;
	vertices[0][0] = x;     vertices[0][1] = y;     vertices[0][2] = u0; vertices[0][3] = v1;
	vertices[1][0] = x;     vertices[1][1] = y + h; vertices[1][2] = u0; vertices[1][3] = v0;
	vertices[2][0] = x + w; vertices[2][1] = y;     vertices[2][2] = u1; vertices[2][3] = v1;
	vertices[3][0] = x + w; vertices[3][1] = y + h; vertices[3][2] = u1; vertices[3][3] = v0;
	if (dx9c.quad_next >= DX9C_QUAD_COUNT)
	{
		flags = D3DLOCK_DISCARD;
		dx9c.quad_next = 0;
	}
	if (FAILED(IDirect3DVertexBuffer9_Lock(dx9c.quad_buffer, dx9c.quad_next * sizeof(vertices), sizeof(vertices), &data, flags)))
		return;
	memcpy(data, vertices, sizeof(vertices));
	IDirect3DVertexBuffer9_Unlock(dx9c.quad_buffer);

	dx9c_set_decl(dx9c.quad_decl);
	dx9c_set_stream(dx9c.quad_buffer, 4 * sizeof(float));
	dx9c_set_vs(dx9_shaders_vs(DX9VS_Quad));
	dx9c_set_ps(dx9_shaders_ps(DX9PS_Quad));
	dx9c_set_depth_stencil(false, false, 7, DX9Stencil_None, 0);
	if (blend)
		dx9c_set_alpha_blend();
	else
		dx9c_set_no_blend();
	dx9c_set_render_state(D3DRS_CULLMODE, D3DCULL_NONE);
	scissor_all();
	dx9c_set_texture(0, DX9_BASE(texture));
	dx9c_set_sampler(0, linear ? DX9C_LINEAR_SAMPLER : DX9C_POINT_SAMPLER);
	/* From the flags that are set, not from dx9c_forget()'s "nothing known"
	 * (all ones): with the swap that is all ones still, and the flags
	 * were never set - the quad read whatever the frontend left there,
	 * and a frame the game wrote itself came out with red and blue
	 * swapped. */
	known = dx9c.bound.bools & ((1u << DX9B_Count) - 1);
	dx9c_set_bools(swap_rb ? (known | (1u << DX9B_SwapRB)) : (known & ~(1u << DX9B_SwapRB)));
	color[0] = color[1] = color[2] = 1.f;
	color[3] = alpha;
	IDirect3DDevice9_SetPixelShaderConstantF(device, DX9PC_Quad, color, 1);
	IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, dx9c.quad_next * 4, 2);
	dx9c.quad_next++;
}

void dx9c_set_frame(IDirect3DTexture9 *texture)
{
	DX9_ASSIGN(dx9c.frame_texture, texture);
	dx9c.handed_over = texture != NULL;
	/* (with the device taken, by the render that drew it) */
	if (dx9c.handed_over)
	{
		dx9c_end_scene();
		dx9c.set_texture(dx9c.handle, texture);
	}
}

bool dx9c_present(void)
{
	/* from here on the frontend has what it was handed: the next render
	 * waits for it to be done with it (dx9c_wait_present_index()) */
	const bool frame = dx9c.handed_over;
	dx9c.handed_over = false;
	return frame;
}

unsigned dx9c_present_count(void)
{
	unsigned mask, count = 1;
	if (dx9c.get_sync_index_mask == NULL)
		return 1;
	mask = dx9c.get_sync_index_mask(dx9c.handle);
	while (count < 32 && (mask >> count) != 0)
		count++;
	return count;
}

unsigned dx9c_present_index(void)
{
	unsigned index;
	if (dx9c.get_sync_index == NULL)
		return 0;
	index = dx9c.get_sync_index(dx9c.handle);
	return index < 32 ? index : 0;
}

void dx9c_wait_present_index(void)
{
	if (dx9c.wait_sync_index != NULL && dx9c.lock_depth == 0)
		dx9c.wait_sync_index(dx9c.handle);
}

/* ---- The frontend's (libretro.c) ---- */

bool lr_dx9_init(const void *iface)
{
	return dx9c_init((const struct retro_hw_render_interface_d3d9 *)iface);
}

void lr_dx9_term(void)
{
	dx9c_term();
}

bool lr_dx9_present(void)
{
	return dx9c_present();
}
