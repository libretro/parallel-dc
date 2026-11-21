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
#include <string.h>
#include "dx11_renderstate.h"
#include "dx11context_lr.h"
#include "rend/TexCache.h"

dx11_blend_states dx11_blends;
dx11_sampler_states dx11_samplers;

static const D3D11_COMPARISON_FUNC depthFuncs[] =
{
	D3D11_COMPARISON_NEVER,				/* 0	Never */
	D3D11_COMPARISON_LESS,				/* 1	Less */
	D3D11_COMPARISON_EQUAL,				/* 2	Equal */
	D3D11_COMPARISON_LESS_EQUAL,		/* 3	Less Or Equal */
	D3D11_COMPARISON_GREATER,			/* 4	Greater */
	D3D11_COMPARISON_NOT_EQUAL,			/* 5	Not Equal */
	D3D11_COMPARISON_GREATER_EQUAL,		/* 6	Greater Or Equal */
	D3D11_COMPARISON_ALWAYS				/* 7	Always */
};

ID3D11DepthStencilState *dx11_depth_states_make(dx11_depth_states *s, u32 key)
{
	const bool depthTest = (key & 1) != 0;
	const bool depthWrite = ((key >> 1) & 1) != 0;
	const int depthFunc = (key >> 2) & 7;
	const int stencil = (int)(key >> 5);
	D3D11_DEPTH_STENCIL_DESC desc;
	D3D11_DEPTH_STENCILOP_DESC *op = &desc.FrontFace;
	ID3D11Device *device = dx11c.device;

	memset(&desc, 0, sizeof(desc));
	desc.DepthEnable = depthTest;
	desc.DepthWriteMask = depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	desc.DepthFunc = depthFuncs[depthFunc & 7];
	desc.StencilEnable = stencil != DX11Stencil_None;
	op->StencilFailOp = op->StencilDepthFailOp = op->StencilPassOp = D3D11_STENCIL_OP_KEEP;
	op->StencilFunc = D3D11_COMPARISON_ALWAYS;
	/* (the reference is set with the state, OMSetDepthStencilState()) */
	switch (stencil)
	{
	case DX11Stencil_Shadow:
		op->StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		desc.StencilReadMask = 0;
		desc.StencilWriteMask = 0x80;
		break;
	case DX11Stencil_ShadowLaterWins:
		op->StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		op->StencilFunc = D3D11_COMPARISON_EQUAL;
		desc.StencilReadMask = 0x81;
		desc.StencilWriteMask = 0x40;
		break;
	case DX11Stencil_ShadowEarlierWins:
		op->StencilPassOp = D3D11_STENCIL_OP_INVERT;
		op->StencilFunc = D3D11_COMPARISON_EQUAL;
		desc.StencilReadMask = 0xC1;
		desc.StencilWriteMask = 0x40;
		break;
	case DX11Stencil_Xor:
		op->StencilPassOp = D3D11_STENCIL_OP_INVERT;
		desc.StencilReadMask = 0;
		desc.StencilWriteMask = 2;
		break;
	case DX11Stencil_Or:
		op->StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		desc.StencilReadMask = 2;
		desc.StencilWriteMask = 2;
		break;
	case DX11Stencil_Inclusion:
		op->StencilFailOp = op->StencilDepthFailOp = D3D11_STENCIL_OP_ZERO;
		op->StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		op->StencilFunc = D3D11_COMPARISON_LESS_EQUAL;
		desc.StencilReadMask = 3;
		desc.StencilWriteMask = 3;
		break;
	case DX11Stencil_Exclusion:
		op->StencilFailOp = op->StencilDepthFailOp = D3D11_STENCIL_OP_ZERO;
		op->StencilFunc = D3D11_COMPARISON_EQUAL;
		desc.StencilReadMask = 3;
		desc.StencilWriteMask = 3;
		break;
	case DX11Stencil_Final:
		/* takes shadows, in a volume, and not drawn again for it (bit 6);
		 * clears the volume bits and that mark */
		op->StencilFailOp = op->StencilDepthFailOp = op->StencilPassOp = D3D11_STENCIL_OP_ZERO;
		op->StencilFunc = D3D11_COMPARISON_EQUAL;
		desc.StencilReadMask = 0xC1;
		desc.StencilWriteMask = 0x43;
		break;
	default:
		break;
	}
	desc.BackFace = desc.FrontFace;

	if (s->started)
		s->draw_path++;
	DX11_RELEASE(s->states[key]);
	if (device == NULL || FAILED(ID3D11Device_CreateDepthStencilState(device, &desc, &s->states[key])))
	{
		s->states[key] = NULL;
		WARN_LOG(RENDERER, "Depth stencil state creation failed\n");
	}
	return s->states[key];
}

bool dx11_depth_states_make_all(dx11_depth_states *s)
{
	bool ok = true;
	u32 key;
	s->started = false;
	/* (every depth test, write and mode with every stencil: 288 of them) */
	for (key = 0; key < DX11_DEPTH_KEYS; key++)
		if ((key >> 5) < (u32)DX11Stencil_Count && s->states[key] == NULL)
			ok = dx11_depth_states_make(s, key) != NULL && ok;
	s->started = true;
	return ok;
}

void dx11_depth_states_term(dx11_depth_states *s)
{
	u32 key;
	for (key = 0; key < DX11_DEPTH_KEYS; key++)
		DX11_RELEASE(s->states[key]);
	s->started = false;
}

/* The TSP's instructions as Direct3D's factors. Those for the alpha
 * cannot be colours: other colour's alpha is the other's alpha. */
static const D3D11_BLEND srcBlend[] =
{
	D3D11_BLEND_ZERO,
	D3D11_BLEND_ONE,
	D3D11_BLEND_DEST_COLOR,
	D3D11_BLEND_INV_DEST_COLOR,
	D3D11_BLEND_SRC_ALPHA,
	D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_DEST_ALPHA,
	D3D11_BLEND_INV_DEST_ALPHA
};
static const D3D11_BLEND dstBlend[] =
{
	D3D11_BLEND_ZERO,
	D3D11_BLEND_ONE,
	D3D11_BLEND_SRC_COLOR,
	D3D11_BLEND_INV_SRC_COLOR,
	D3D11_BLEND_SRC_ALPHA,
	D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_DEST_ALPHA,
	D3D11_BLEND_INV_DEST_ALPHA
};
static const D3D11_BLEND srcBlendAlpha[] =
{
	D3D11_BLEND_ZERO,
	D3D11_BLEND_ONE,
	D3D11_BLEND_DEST_ALPHA,
	D3D11_BLEND_INV_DEST_ALPHA,
	D3D11_BLEND_SRC_ALPHA,
	D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_DEST_ALPHA,
	D3D11_BLEND_INV_DEST_ALPHA
};
static const D3D11_BLEND dstBlendAlpha[] =
{
	D3D11_BLEND_ZERO,
	D3D11_BLEND_ONE,
	D3D11_BLEND_SRC_ALPHA,
	D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_SRC_ALPHA,
	D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_DEST_ALPHA,
	D3D11_BLEND_INV_DEST_ALPHA
};

static ID3D11BlendState *blend_make_desc(const D3D11_BLEND_DESC *desc, ID3D11BlendState **state)
{
	ID3D11Device *device = dx11c.device;
	if (dx11_blends.started)
		dx11_blends.draw_path++;
	DX11_RELEASE(*state);
	if (device == NULL || FAILED(ID3D11Device_CreateBlendState(device, desc, state)))
	{
		*state = NULL;
		WARN_LOG(RENDERER, "Blend state creation failed\n");
	}
	return *state;
}

ID3D11BlendState *dx11_blend_make(u32 key)
{
	const bool enable = (key & 1) != 0;
	const u32 src = (key >> 1) & 7;
	const u32 dst = (key >> 4) & 7;
	const bool writeColor = ((key >> 7) & 1) != 0;
	D3D11_BLEND_DESC desc;
	D3D11_RENDER_TARGET_BLEND_DESC *rt = &desc.RenderTarget[0];

	memset(&desc, 0, sizeof(desc));
	rt->BlendEnable = enable;
	rt->SrcBlend = enable ? srcBlend[src] : D3D11_BLEND_ONE;
	rt->DestBlend = enable ? dstBlend[dst] : D3D11_BLEND_ZERO;
	rt->BlendOp = D3D11_BLEND_OP_ADD;
	rt->SrcBlendAlpha = enable ? srcBlendAlpha[src] : D3D11_BLEND_ONE;
	rt->DestBlendAlpha = enable ? dstBlendAlpha[dst] : D3D11_BLEND_ZERO;
	rt->BlendOpAlpha = D3D11_BLEND_OP_ADD;
	rt->RenderTargetWriteMask = writeColor ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
	return blend_make_desc(&desc, &dx11_blends.states[key]);
}

/* (a blend of @src and @dst of both the colour and the alpha) */
static ID3D11BlendState *blend_both(D3D11_BLEND src, D3D11_BLEND dst, ID3D11BlendState **state)
{
	D3D11_BLEND_DESC desc;
	D3D11_RENDER_TARGET_BLEND_DESC *rt = &desc.RenderTarget[0];
	memset(&desc, 0, sizeof(desc));
	rt->BlendEnable = TRUE;
	rt->SrcBlend = rt->SrcBlendAlpha = src;
	rt->DestBlend = rt->DestBlendAlpha = dst;
	rt->BlendOp = rt->BlendOpAlpha = D3D11_BLEND_OP_ADD;
	rt->RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	return blend_make_desc(&desc, state);
}

ID3D11BlendState *dx11_blend_alpha(void)
{
	if (dx11_blends.alpha == NULL)
		blend_both(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, &dx11_blends.alpha);
	return dx11_blends.alpha;
}

ID3D11BlendState *dx11_blend_factor_alpha(void)
{
	if (dx11_blends.factor_alpha == NULL)
		blend_both(D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, &dx11_blends.factor_alpha);
	return dx11_blends.factor_alpha;
}

bool dx11_blend_make_all(void)
{
	bool ok = true;
	u32 key;
	dx11_blends.started = false;
	for (key = 0; key < 256; key++)
	{
		/* (blending off has its instructions at 0: dx11_blend_get()) */
		if ((key & 1) == 0 && (key & 0x7e) != 0)
			continue;
		if (dx11_blends.states[key] == NULL)
			ok = dx11_blend_make(key) != NULL && ok;
	}
	ok = dx11_blend_alpha() != NULL && dx11_blend_factor_alpha() != NULL && ok;
	dx11_blends.started = true;
	return ok;
}

void dx11_blend_term(void)
{
	u32 key;
	for (key = 0; key < 256; key++)
		DX11_RELEASE(dx11_blends.states[key]);
	DX11_RELEASE(dx11_blends.alpha);
	DX11_RELEASE(dx11_blends.factor_alpha);
	dx11_blends.started = false;
}

ID3D11SamplerState *dx11_sampler_make(u32 tsp)
{
	const u32 key = tsp & DX11_TSP_SAMPLER_MASK;
	const int anisotropy = settings.rend.AnisotropicFiltering;
	ID3D11Device *device = dx11c.device;
	ID3D11SamplerState **sampler = &dx11_samplers.samplers[key >> 8];
	D3D11_SAMPLER_DESC desc;

	memset(&desc, 0, sizeof(desc));
	if (TAW_GET(tsp, TSP_FilterMode) == 0)
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	else if (anisotropy > 1)
		desc.Filter = D3D11_FILTER_ANISOTROPIC;
	else
		/* (the mipmap level nearest, as the Vulkan renderer's: trilinear filtering is two passes) */
		desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	desc.AddressU = TAW_GET(tsp, TSP_ClampU) ? D3D11_TEXTURE_ADDRESS_CLAMP
			: TAW_GET(tsp, TSP_FlipU) ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
	desc.AddressV = TAW_GET(tsp, TSP_ClampV) ? D3D11_TEXTURE_ADDRESS_CLAMP
			: TAW_GET(tsp, TSP_FlipV) ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
	desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	desc.MipLODBias = D_Adjust_LoD_Bias[TAW_GET(tsp, TSP_MipMapD)];
	desc.MaxAnisotropy = anisotropy < 1 ? 1 : anisotropy > 16 ? 16 : anisotropy;
	desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	desc.MinLOD = 0.f;
	desc.MaxLOD = D3D11_FLOAT32_MAX;

	if (dx11_samplers.started)
		dx11_samplers.draw_path++;
	DX11_RELEASE(*sampler);
	if (device == NULL || FAILED(ID3D11Device_CreateSamplerState(device, &desc, sampler)))
	{
		*sampler = NULL;
		WARN_LOG(RENDERER, "Sampler creation failed\n");
	}
	return *sampler;
}

ID3D11SamplerState *dx11_sampler_point(void)
{
	return dx11_sampler_get(TAW_SET(TAW_SET(0, TSP_ClampU, 1), TSP_ClampV, 1));
}

ID3D11SamplerState *dx11_sampler_linear(void)
{
	if (dx11_samplers.linear == NULL)
	{
		ID3D11Device *device = dx11c.device;
		D3D11_SAMPLER_DESC desc;
		if (dx11_samplers.started)
			dx11_samplers.draw_path++;
		memset(&desc, 0, sizeof(desc));
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.MaxAnisotropy = 1;
		desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		if (device == NULL || FAILED(ID3D11Device_CreateSamplerState(device, &desc, &dx11_samplers.linear)))
		{
			dx11_samplers.linear = NULL;
			WARN_LOG(RENDERER, "Sampler creation failed\n");
		}
	}
	return dx11_samplers.linear;
}

bool dx11_samplers_make_all(void)
{
	bool ok = true;
	u32 bits;
	dx11_samplers.started = false;
	/* (every combination of the bits a sampler is made from: 1024) */
	for (bits = DX11_TSP_SAMPLER_MASK; ; bits = (bits - 1) & DX11_TSP_SAMPLER_MASK)
	{
		if (dx11_samplers.samplers[bits >> 8] == NULL)
			ok = dx11_sampler_make(bits) != NULL && ok;
		if (bits == 0)
			break;
	}
	ok = dx11_sampler_linear() != NULL && ok;
	dx11_samplers.started = true;
	return ok;
}

void dx11_samplers_term(void)
{
	u32 i;
	for (i = 0; i < ARRAY_SIZE(dx11_samplers.samplers); i++)
		DX11_RELEASE(dx11_samplers.samplers[i]);
	DX11_RELEASE(dx11_samplers.linear);
	dx11_samplers.started = false;
}
