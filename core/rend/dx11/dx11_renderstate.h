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
#include <retro_inline.h>
#include "types.h"
#include "hw/pvr/ta_words.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Direct3D 11 renderers' states, each made once before the first
 * frame and looked up by a key for every polygon (AGENTS.md: one made
 * after is made on the draw path, and counted in draw_path). */

/* What the stencil buffer is used for, as the Vulkan renderers' pipelines
 * use it (vulkan/vk_pipeline.c): the modifier volumes' bits 0 and 1, bit 7
 * for a polygon that takes shadows, bit 6 for a pixel drawn again for its
 * shadow (rend/shadows.h). */
typedef enum DX11Stencil
{
	DX11Stencil_None,
	DX11Stencil_Shadow,				/* a polygon that takes shadows, or not: bit 7 (the reference) written */
	DX11Stencil_ShadowLaterWins,	/* drawn again for its shadow: see PipelineManager::ShadowPass */
	DX11Stencil_ShadowEarlierWins,
	DX11Stencil_Xor,				/* a closed volume */
	DX11Stencil_Or,					/* an open one, or a quad */
	DX11Stencil_Inclusion,			/* the area summed */
	DX11Stencil_Exclusion,
	DX11Stencil_Final,				/* what is left in a volume, darkened */
	DX11Stencil_Count
} DX11Stencil;

/* ---- Depth and stencil: a renderer's own ---- */
#define DX11_DEPTH_KEYS (1 << 9)
typedef struct dx11_depth_states
{
	ID3D11DepthStencilState *states[DX11_DEPTH_KEYS];
	bool started;
	u32 draw_path;
} dx11_depth_states;

ID3D11DepthStencilState *dx11_depth_states_make(dx11_depth_states *s, u32 key);
/* @depth_func: the ISP's depth mode, 0 never to 7 always */
static INLINE ID3D11DepthStencilState *dx11_depth_states_get(dx11_depth_states *s,
		bool depth_test, bool depth_write, int depth_func, DX11Stencil stencil)
{
	const u32 key = ((u32)depth_test | ((u32)depth_write << 1) | ((u32)(depth_func & 7) << 2) | ((u32)stencil << 5))
			& (DX11_DEPTH_KEYS - 1);
	ID3D11DepthStencilState *state = s->states[key];
	return state != NULL ? state : dx11_depth_states_make(s, key);
}
/* Every one a frame can want */
bool dx11_depth_states_make_all(dx11_depth_states *s);
void dx11_depth_states_term(dx11_depth_states *s);

/* ---- Blending: the context's (the TSP's instructions, 0 to 7) ---- */
typedef struct dx11_blend_states
{
	ID3D11BlendState *states[256];
	ID3D11BlendState *alpha;
	ID3D11BlendState *factor_alpha;
	bool started;
	u32 draw_path;
} dx11_blend_states;
extern dx11_blend_states dx11_blends;

ID3D11BlendState *dx11_blend_make(u32 key);
static INLINE ID3D11BlendState *dx11_blend_get(bool enable, u32 src, u32 dst, bool write_color)
{
	u32 key;
	ID3D11BlendState *state;
	src &= 7;
	dst &= 7;
	if (!enable)
		src = dst = 0;
	key = (u32)enable | (src << 1) | (dst << 4) | ((u32)write_color << 7);
	state = dx11_blends.states[key];
	return state != NULL ? state : dx11_blend_make(key);
}
/* none */
#define dx11_blend_none() dx11_blend_get(false, 1, 0, true)
/* by the source's alpha: the crosshairs */
ID3D11BlendState *dx11_blend_alpha(void);
/* by the alpha of the blend factor: the VMUs' screens */
ID3D11BlendState *dx11_blend_factor_alpha(void);
/* Every one a frame can want */
bool dx11_blend_make_all(void);
void dx11_blend_term(void);

/* ---- Samplers: the context's ---- */
/* A polygon's: MipMapD, FilterMode, ClampU, ClampV, FlipU, FlipV */
#define DX11_TSP_SAMPLER_MASK 0x7ef00
typedef struct dx11_sampler_states
{
	/* (by the bits themselves) */
	ID3D11SamplerState *samplers[(DX11_TSP_SAMPLER_MASK >> 8) + 1];
	ID3D11SamplerState *linear;
	bool started;
	u32 draw_path;
} dx11_sampler_states;
extern dx11_sampler_states dx11_samplers;

ID3D11SamplerState *dx11_sampler_make(u32 tsp);
static INLINE ID3D11SamplerState *dx11_sampler_get(u32 tsp)
{
	ID3D11SamplerState *sampler = dx11_samplers.samplers[(tsp & DX11_TSP_SAMPLER_MASK) >> 8];
	return sampler != NULL ? sampler : dx11_sampler_make(tsp);
}
/* Clamped, nearest: the fog table's and the palette's */
ID3D11SamplerState *dx11_sampler_point(void);
/* Clamped, linear: pictures drawn whole onto another */
ID3D11SamplerState *dx11_sampler_linear(void);
/* Every one a frame can want */
bool dx11_samplers_make_all(void);
void dx11_samplers_term(void);

#ifdef __cplusplus
}
#endif
