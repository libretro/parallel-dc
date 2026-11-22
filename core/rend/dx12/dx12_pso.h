/*
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
/* The Direct3D 12 renderer's pipeline state objects.
 *
 * One pixel shader draws every polygon (dx12_shaders.h: the polygon's bits
 * are root constants; the per-pixel renderer's has one a pass), so a
 * pipeline is the fixed-function state a polygon is drawn with and nothing
 * more: its key (DX12PsoKey) is the blend, depth, stencil and cull state,
 * the interpolation, and which shaders. Every pipeline is made from its
 * key alone.
 *
 * When. At the start (dx12c_init()), every pipeline a frame can
 * need whatever it draws - the modifier volumes' in every mode and cull
 * mode, the depth prepass's, the quads', the texture kernels' - and every
 * key kept from earlier runs (the key log, below), so that a game's own
 * are there before it draws. Direct3D 12 has no dynamic blend or depth
 * state, so a polygon drawn with a key never seen before has no pipeline
 * that draws the same pixels to stand in for it, as Vulkan's general
 * pipelines do: that pipeline is made there and then, once, and its key is
 * logged (statMadeInFrame). The next run makes it at the start.
 *
 * The keys and the device's own compiled pipelines are kept on disk in the
 * system directory (dc/d3d12_pipelines.bin): a header, the keys, then an
 * ID3D12PipelineLibrary, which hands a pipeline back without compiling it
 * again. A library the driver no longer takes (a new driver) is started
 * again empty; the keys stay.
 *
 * SSAA (dx12_renderer.h): a pipeline also has the samples a pixel of what
 * it draws into, and for a polygon whether it is shaded at each sample or
 * once a pixel. When SSAA is taken up (the renderer starts, or changes
 * size), every pipeline of one sample there is gets made for the count
 * taken, at both rates (dx12_pso_make_for_samples()): a frame draws into the screen's
 * samples with what a frame without SSAA draws with, so it finds them made.
 *
 * The table is looked up by key for every draw: open addressing, made with
 * the context; nothing is allocated to find or add a pipeline. Only the
 * thread that renders touches it.
 *
 * Ahead (rend/d3d_warm.h): what the start and SSAA make goes to a thread
 * of its own, which hands each pipeline back as it is made; a frame that
 * wants one not handed back yet takes what is, then makes it itself if it
 * still is not there - nothing is drawn without its pipeline. The thread
 * alone uses the library while it builds. Those NVAPI makes (post-depth
 * coverage) are made where they always were, by the thread that renders. */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include <retro_inline.h>
#include "types.h"
#include "dx12_shader_flags.h"
#include "rend/d3d_warm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the stencil buffer is used for: as the Direct3D 11 renderer's
 * (dx11/dx11_renderstate.h, DX11Stencil) */
typedef enum
{
	DX12Stencil_None,
	DX12Stencil_Shadow,
	DX12Stencil_ShadowLaterWins,
	DX12Stencil_ShadowEarlierWins,
	DX12Stencil_Xor,
	DX12Stencil_Or,
	DX12Stencil_Inclusion,
	DX12Stencil_Exclusion,
	DX12Stencil_Final,
	DX12Stencil_Count
} DX12Stencil;

/* What the special blends are: the key's own factors; by the source's
 * alpha (the crosshairs); by the blend factor's (the VMUs' screens) */
enum { DX12_BLEND_KEY = 0, DX12_BLEND_ALPHA = 1, DX12_BLEND_FACTOR = 2 };
/* Which shaders: the per-triangle renderer's polygons, the modifier
 * volumes, the quads; the per-pixel renderer's polygons (in a pass, DX12_PASS_),
 * its lists put together, its translucent modifier volumes; SSAA's resolve;
 * the temporal upscaler's motion vector pass (dx12_renderer.h: into a
 * picture of two halves a pixel, over the depth the frame left; with pass
 * 1, the depth buffer copied into its picture of floats by the quad's
 * vertex shader) */
enum { DX12_PSO_POLY = 0, DX12_PSO_MODVOL = 1, DX12_PSO_QUAD = 2, DX12_PSO_OIT = 3, DX12_PSO_OIT_FINAL = 4, DX12_PSO_OIT_MODVOL = 5,
	DX12_PSO_SSAA_RESOLVE = 6, DX12_PSO_MOTION = 7 };
/* SSAA's rates (DX12PsoKey::rate): none; a polygon shaded at each sample;
 * once a pixel. (The lists put together: at each sample into samples, or
 * resolved into a picture of one - and with the HDR output, storing the
 * samples' channel sums as well, DX12_RATE_PIXEL_SUMS; a translucent
 * volume: with SSAA.) */
enum { DX12_RATE_NONE = 0, DX12_RATE_SAMPLE = 1, DX12_RATE_PIXEL = 2, DX12_RATE_PIXEL_SUMS = 3 };

/* Of a polygon's blend, either renderer's, the TSP's factors that read
 * only the source, which its pixel shader applies (DX12_PF_SRC_FOLD, DX12_PF_DST_FOLD): the
 * source's zero, one, alpha and one less it, and the destination's those,
 * the source colour and one less it. Its pipeline then has one for the
 * first and the shader's second colour for the second (dual), so that
 * the 64 pairs of instructions are 15 pipelines: those that read the frame
 * - the other colour or the destination alpha as the source factor, the
 * destination alpha as the destination's - are the pipeline's still. */
static INLINE bool dx12_src_folds(u32 instr) { return instr == 0 || instr == 1 || instr == 4 || instr == 5; }
static INLINE bool dx12_dst_folds(u32 instr) { return instr <= 5; }

typedef struct DX12PsoKey
{
	u32 kind;			/* DX12_PSO_ */
	bool gouraud;
	bool blend;
	u32 src, dst;		/* the TSP's instructions, 0 to 7 */
	bool writeColor;
	bool depthTest;
	bool depthWrite;
	u32 depthFunc;		/* the ISP's depth mode, 0 never to 7 always */
	DX12Stencil stencil;
	u32 cull;			/* 0 none, 1 front faces, 2 back faces */
	u32 special;		/* DX12_BLEND_ */
	bool depthBound;	/* a depth and stencil is bound (its format is in the pipeline) */
	u32 pass;			/* DX12_PASS_, of a DX12_PSO_OIT pipeline */
	u32 samples;		/* SSAA: log2 of the samples a pixel of what it draws into (0: one) */
	u32 rate;			/* SSAA: DX12_RATE_ */
	bool dual;			/* a polygon's destination factor is its shader's second colour (made by dx12_pso_key_pack()) */
} DX12PsoKey;

/* (a polygon's blend as its pipeline has it: the factors its shader
 * applies are one and the second colour, whatever the instructions;
 * and no cull mode or shading, which its shaders apply:
 * DX12_PF_CULL_SHIFT, DX12_PF_GOURAUD) */
static INLINE u64 dx12_pso_key_pack(const DX12PsoKey *key)
{
	u32 s = key->src, d = key->dst;
	bool two = key->dual;
	const bool folds = key->kind == DX12_PSO_POLY || key->kind == DX12_PSO_OIT;
	const u32 c = folds ? 0 : key->cull;
	const bool g = folds ? false : key->gouraud;
	if (folds && key->blend)
	{
		if (dx12_src_folds(s))
			s = 1;
		if (dx12_dst_folds(d))
		{
			d = 0;
			two = true;
		}
	}
	return (u64)(key->kind & 7) | ((u64)g << 3) | ((u64)key->blend << 4) | ((u64)(s & 7) << 5) | ((u64)(d & 7) << 8)
			| ((u64)key->writeColor << 11) | ((u64)key->depthTest << 12) | ((u64)key->depthWrite << 13)
			| ((u64)(key->depthFunc & 7) << 14)
			| ((u64)((u32)key->stencil & 15) << 17) | ((u64)(c & 3) << 21) | ((u64)(key->special & 3) << 23)
			| ((u64)key->depthBound << 25) | ((u64)(key->pass & 3) << 26)
			/* (never 0: an empty slot's) */
			| ((u64)1 << 28)
			/* (after it: a key without SSAA is the one it was before SSAA) */
			| ((u64)(key->samples & 3) << 29) | ((u64)(key->rate & 3) << 31) | ((u64)two << 33);
}

static INLINE DX12PsoKey dx12_pso_key_unpack(u64 v)
{
	DX12PsoKey k;
	k.kind = (u32)(v & 7);
	k.gouraud = ((v >> 3) & 1) != 0;
	k.blend = ((v >> 4) & 1) != 0;
	k.src = (u32)((v >> 5) & 7);
	k.dst = (u32)((v >> 8) & 7);
	k.writeColor = ((v >> 11) & 1) != 0;
	k.depthTest = ((v >> 12) & 1) != 0;
	k.depthWrite = ((v >> 13) & 1) != 0;
	k.depthFunc = (u32)((v >> 14) & 7);
	k.stencil = (DX12Stencil)((v >> 17) & 15);
	k.cull = (u32)((v >> 21) & 3);
	k.special = (u32)((v >> 23) & 3);
	k.depthBound = ((v >> 25) & 1) != 0;
	k.pass = (u32)((v >> 26) & 3);
	k.samples = (u32)((v >> 29) & 3);
	k.rate = (u32)((v >> 31) & 3);
	k.dual = ((v >> 33) & 1) != 0;
	return k;
}

/* Whether a key read from a file is one this core makes */
static INLINE bool dx12_pso_key_valid(u64 v)
{
	const DX12PsoKey k = dx12_pso_key_unpack(v);
	return (v >> 34) == 0 && ((v >> 28) & 1) == 1 && (!k.dual || ((k.kind == DX12_PSO_POLY || k.kind == DX12_PSO_OIT) && k.blend))
			&& k.kind <= DX12_PSO_MOTION && (u32)k.stencil < (u32)DX12Stencil_Count
			&& k.cull <= 2 && k.special <= DX12_BLEND_FACTOR && k.pass <= DX12_PASS_OIT
			&& (k.rate <= DX12_RATE_PIXEL || (k.rate == DX12_RATE_PIXEL_SUMS && k.kind == DX12_PSO_OIT_FINAL));
}

/* The TSP's instructions as Direct3D's factors. Those for the alpha
 * cannot be colours: other colour's alpha is the other's alpha. */
static const D3D12_BLEND dx12_srcBlend[] =
{
	D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR,
	D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
};
static const D3D12_BLEND dx12_dstBlend[] =
{
	D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR,
	D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
};
static const D3D12_BLEND dx12_srcBlendAlpha[] =
{
	D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
	D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
};
static const D3D12_BLEND dx12_dstBlendAlpha[] =
{
	D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA,
	D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
};

/* The blend of a pipeline made by its instructions (not DX12_BLEND_ALPHA
 * or DX12_BLEND_FACTOR): a polygon's destination factor from its shader's
 * second colour where the key is dual (see dx12_src_folds()) */
static INLINE void dx12_blend_desc(const DX12PsoKey *key, D3D12_RENDER_TARGET_BLEND_DESC *rt)
{
	rt->BlendEnable = key->blend;
	rt->SrcBlend = key->blend ? dx12_srcBlend[key->src & 7] : D3D12_BLEND_ONE;
	rt->DestBlend = key->blend ? dx12_dstBlend[key->dst & 7] : D3D12_BLEND_ZERO;
	rt->SrcBlendAlpha = key->blend ? dx12_srcBlendAlpha[key->src & 7] : D3D12_BLEND_ONE;
	rt->DestBlendAlpha = key->blend ? dx12_dstBlendAlpha[key->dst & 7] : D3D12_BLEND_ZERO;
	if (key->dual)
	{
		rt->DestBlend = D3D12_BLEND_SRC1_COLOR;
		rt->DestBlendAlpha = D3D12_BLEND_SRC1_ALPHA;
	}
}

/* The table's size (SSAA makes each pipeline of a polygon again at two rates) */
#define DX12_PSO_SLOTS 8192
/* The most keys kept in the file */
#define DX12_PSO_MAX_KEYS 8192

typedef struct dx12_pso_slot
{
	u64 key;
	ID3D12PipelineState *pso;
} dx12_pso_slot;

/* The pipelines: one table, the context's (dx12context_lr.h) */
typedef struct dx12_pipelines
{
	/* what has been done so far, for measuring */
	u32 statMade;			/* all of them */
	u32 statMadeInFrame;	/* of them, while a game ran */
	u32 statFromLibrary;	/* handed back by the library, not compiled */
	u32 statPostZ;			/* SSAA's lists' made with post-depth coverage (NVAPI) */
	u32 statOwnTest;		/* ...made to test their samples themselves */
	u32 statAhead;			/* made by the thread ahead of a frame wanting them */

	ID3D12Device *device;
	ID3D12Device1 *device1;
	ID3D12RootSignature *root;
	dx12_pso_slot *slots;
	u32 used;
	struct ID3D12PipelineLibrary *library;
	/* what the library was made from: it reads it for as long as it lives */
	u8 *libraryData;
	size_t libraryBytes;
	u64 *keys;				/* the file's, then those made: DX12_PSO_MAX_KEYS of room */
	size_t keyCount;
	size_t fileKeys;		/* how many of them are the file's */
	u32 storedSinceLoad;
	bool started;
	d3d_warm_t *warm;
	u64 *requested;			/* every key made or queued, open addressing */
	u32 requestedUsed;
	d3d_warm_job_t *batch;	/* queued by the requests, handed over with the batch */
	size_t batchCount;
	/* SSAA's lists' pipelines made with NVIDIA's post-depth coverage
	 * (dx12_nvapi.h): on an NVIDIA GPU whose driver has NVAPI, until it
	 * would not make one */
	bool postZCoverage;
} dx12_pipelines;

extern dx12_pipelines dx12p;

/* The table, the shaders, the library from the file; @vendor the
 * adapter's PCI vendor id */
bool dx12_pso_init(ID3D12Device *device, ID3D12RootSignature *root, UINT vendor);
/* The library and the keys written to the file, if pipelines were made */
void dx12_pso_term(void);
/* Everything a frame can need, and the keys kept from earlier runs */
void dx12_pso_make_at_start(void);
/* One more a renderer wants made before a game draws */
void dx12_pso_prepare(const DX12PsoKey *key);
/* Many, made ahead as one batch, in their order (each once) */
void dx12_pso_prepare_many(const u64 *packed, size_t count);
/* Whether what the thread was given is made (and in the table) */
bool dx12_pso_made(void);
/* Waits for it: at the start, before the machine runs, never in a
 * frame (libretro.cpp) */
void dx12_pso_wait_made(void);
/* SSAA of 2^@log2n samples taken up: every pipeline of one sample made
 * for that many, at each rate it can be drawn at, and the keys of that
 * many kept from earlier runs (those made already are found made);
 * (@hdr: the HDR output's too, DX12_RATE_PIXEL_SUMS) */
void dx12_pso_make_for_samples(u32 log2n, bool hdr);
/* (the build itself, which changes nothing of the table's: the
 * thread's as well as the renderer's; @flags what it did, DX12_BUILT_) */
bool dx12_pso_build_desc(u64 key, bool postZ, bool useLibrary, ID3D12PipelineState **out, u32 *flags);
ID3D12PipelineState *dx12_pso_missed(u64 key);

static INLINE u32 dx12_pso_hash(u64 k)
{
	k ^= k >> 29;
	k *= (u64)0x9E3779B97F4A7C15ull;
	return (u32)(k >> 40) & (DX12_PSO_SLOTS - 1);
}

/* The pipeline of @key: the table's, or made now (and counted) */
static INLINE ID3D12PipelineState *dx12_pso_get(const DX12PsoKey *key)
{
	const u64 k = dx12_pso_key_pack(key);
	u32 i = dx12_pso_hash(k);
	for (;;)
	{
		const dx12_pso_slot *s = &dx12p.slots[i];
		if (s->key == k)
			return s->pso;
		if (s->key == 0)
			return dx12_pso_missed(k);
		i = (i + 1) & (DX12_PSO_SLOTS - 1);
	}
}

#ifdef __cplusplus
}
#endif
