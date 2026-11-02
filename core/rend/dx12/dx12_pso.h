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
 * When. At the start (DX12Context::Init()), every pipeline a frame can
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
 * taken, at both rates (MakeForSamples()): a frame draws into the screen's
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
#include "types.h"
#include "dx12_shader_flags.h"
#include "rend/d3d_warm.h"
#include <vector>

/* (mingw-w64's d3d12.h has ID3D12Device1, which makes them, and not the
 * interface itself: as the Windows SDK has it) */
#ifndef __ID3D12PipelineLibrary_INTERFACE_DEFINED__
#define __ID3D12PipelineLibrary_INTERFACE_DEFINED__
struct ID3D12PipelineLibrary : public ID3D12DeviceChild
{
	virtual HRESULT STDMETHODCALLTYPE StorePipeline(LPCWSTR name, ID3D12PipelineState *pipeline) = 0;
	virtual HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
			REFIID riid, void **pipeline) = 0;
	virtual HRESULT STDMETHODCALLTYPE LoadComputePipeline(LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
			REFIID riid, void **pipeline) = 0;
	virtual SIZE_T STDMETHODCALLTYPE GetSerializedSize() = 0;
	virtual HRESULT STDMETHODCALLTYPE Serialize(void *data, SIZE_T size) = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D12PipelineLibrary, 0xc64226a8, 0x9201, 0x46af, 0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7, 0x41, 0x4f)
#endif
#endif

/* What the stencil buffer is used for: as the Direct3D 11 renderer's
 * (dx11/dx11_renderstate.h, DX11Stencil) */
enum class DX12Stencil
{
	None,
	Shadow,
	ShadowLaterWins,
	ShadowEarlierWins,
	Xor,
	Or,
	Inclusion,
	Exclusion,
	Final,
	Count
};

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

/* Of a polygon's blend, the TSP's factors that read only the source,
 * which its pixel shader applies (DX12_PF_SRC_FOLD, DX12_PF_DST_FOLD): the
 * source's zero, one, alpha and one less it, and the destination's those,
 * the source colour and one less it. Its pipeline then has one for the
 * first and the shader's second colour for the second (dual), so that
 * the 64 pairs of instructions are 15 pipelines: those that read the frame
 * - the other colour or the destination alpha as the source factor, the
 * destination alpha as the destination's - are the pipeline's still. */
static inline bool dx12_src_folds(u32 instr) { return instr == 0 || instr == 1 || instr == 4 || instr == 5; }
static inline bool dx12_dst_folds(u32 instr) { return instr <= 5; }

struct DX12PsoKey
{
	u32 kind;			// DX12_PSO_
	bool gouraud;
	bool blend;
	u32 src, dst;		// the TSP's instructions, 0 to 7
	bool writeColor;
	bool depthTest;
	bool depthWrite;
	u32 depthFunc;		// the ISP's depth mode, 0 never to 7 always
	DX12Stencil stencil;
	u32 cull;			// 0 none, 1 front faces, 2 back faces
	u32 special;		// DX12_BLEND_
	bool depthBound;	// a depth and stencil is bound (its format is in the pipeline)
	u32 pass;			// DX12_PASS_, of a DX12_PSO_OIT pipeline
	u32 samples;		// SSAA: log2 of the samples a pixel of what it draws into (0: one)
	u32 rate;			// SSAA: DX12_RATE_
	bool dual;			// a polygon's destination factor is its shader's second colour (made by Pack())

	/* (a polygon's blend as its pipeline has it: the factors its shader
	 * applies are one and the second colour, whatever the instructions) */
	u64 Pack() const
	{
		u32 s = src, d = dst;
		bool two = dual;
		if (kind == DX12_PSO_POLY && blend)
		{
			if (dx12_src_folds(s))
				s = 1;
			if (dx12_dst_folds(d))
			{
				d = 0;
				two = true;
			}
		}
		return (u64)(kind & 7) | ((u64)gouraud << 3) | ((u64)blend << 4) | ((u64)(s & 7) << 5) | ((u64)(d & 7) << 8)
				| ((u64)writeColor << 11) | ((u64)depthTest << 12) | ((u64)depthWrite << 13) | ((u64)(depthFunc & 7) << 14)
				| ((u64)((u32)stencil & 15) << 17) | ((u64)(cull & 3) << 21) | ((u64)(special & 3) << 23)
				| ((u64)depthBound << 25) | ((u64)(pass & 3) << 26)
				// (never 0: an empty slot's)
				| (1ull << 28)
				// (after it: a key without SSAA is the one it was before SSAA)
				| ((u64)(samples & 3) << 29) | ((u64)(rate & 3) << 31) | ((u64)two << 33);
	}
	static DX12PsoKey Unpack(u64 v)
	{
		DX12PsoKey k;
		k.kind = v & 7;
		k.gouraud = (v >> 3) & 1;
		k.blend = (v >> 4) & 1;
		k.src = (v >> 5) & 7;
		k.dst = (v >> 8) & 7;
		k.writeColor = (v >> 11) & 1;
		k.depthTest = (v >> 12) & 1;
		k.depthWrite = (v >> 13) & 1;
		k.depthFunc = (v >> 14) & 7;
		k.stencil = (DX12Stencil)((v >> 17) & 15);
		k.cull = (v >> 21) & 3;
		k.special = (v >> 23) & 3;
		k.depthBound = (v >> 25) & 1;
		k.pass = (v >> 26) & 3;
		k.samples = (v >> 29) & 3;
		k.rate = (v >> 31) & 3;
		k.dual = (v >> 33) & 1;
		return k;
	}
	/* Whether a key read from a file is one this core makes */
	static bool Valid(u64 v)
	{
		const DX12PsoKey k = Unpack(v);
		return (v >> 34) == 0 && ((v >> 28) & 1) == 1 && (!k.dual || (k.kind == DX12_PSO_POLY && k.blend)) && k.kind <= DX12_PSO_MOTION && (u32)k.stencil < (u32)DX12Stencil::Count
				&& k.cull <= 2 && k.special <= DX12_BLEND_FACTOR && k.pass <= DX12_PASS_OIT
				&& (k.rate <= DX12_RATE_PIXEL || (k.rate == DX12_RATE_PIXEL_SUMS && k.kind == DX12_PSO_OIT_FINAL));
	}
};

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
static inline void dx12_blend_desc(const DX12PsoKey& key, D3D12_RENDER_TARGET_BLEND_DESC& rt)
{
	rt.BlendEnable = key.blend;
	rt.SrcBlend = key.blend ? dx12_srcBlend[key.src & 7] : D3D12_BLEND_ONE;
	rt.DestBlend = key.blend ? dx12_dstBlend[key.dst & 7] : D3D12_BLEND_ZERO;
	rt.SrcBlendAlpha = key.blend ? dx12_srcBlendAlpha[key.src & 7] : D3D12_BLEND_ONE;
	rt.DestBlendAlpha = key.blend ? dx12_dstBlendAlpha[key.dst & 7] : D3D12_BLEND_ZERO;
	if (key.dual)
	{
		rt.DestBlend = D3D12_BLEND_SRC1_COLOR;
		rt.DestBlendAlpha = D3D12_BLEND_SRC1_ALPHA;
	}
}

class DX12Pipelines
{
public:
	/* The table, the shaders, the library from the file; @vendor the
	 * adapter's PCI vendor id */
	bool Init(ID3D12Device *device, ID3D12RootSignature *root, UINT vendor);
	/* The library and the keys written to the file, if pipelines were made */
	void Term();
	/* Everything a frame can need, and the keys kept from earlier runs */
	void MakeAtStart();
	/* One more a renderer wants made before a game draws */
	void Prepare(const DX12PsoKey& key) { Want(key.Pack()); SubmitBatch(); }
	/* SSAA of 2^@log2n samples taken up: every pipeline of one sample made
	 * for that many, at each rate it can be drawn at, and the keys of that
	 * many kept from earlier runs (those made already are found made) */
	// (@hdr: the HDR output's too, DX12_RATE_PIXEL_SUMS)
	void MakeForSamples(u32 log2n, bool hdr = false);

	/* The pipeline of @key: the table's, or made now (and counted) */
	ID3D12PipelineState *Get(const DX12PsoKey& key)
	{
		const u64 k = key.Pack();
		u32 i = Hash(k);
		for (;;)
		{
			Slot& s = slots[i];
			if (s.key == k)
				return s.pso;
			if (s.key == 0)
				return Missed(k);
			i = (i + 1) & (SLOTS - 1);
		}
	}

	/* (the build itself, which changes nothing of the table's: the
	 * thread's as well as the renderer's; @flags what it did, DX12_BUILT_) */
	bool BuildDesc(u64 key, bool postZ, bool useLibrary, ID3D12PipelineState **out, u32 *flags) const;

	// what has been done so far, for measuring
	u32 statMade = 0;			// all of them
	u32 statMadeInFrame = 0;	// of them, while a game ran
	u32 statFromLibrary = 0;	// handed back by the library, not compiled
	u32 statPostZ = 0;			// SSAA's lists' made with post-depth coverage (NVAPI)
	u32 statOwnTest = 0;		// ...made to test their samples themselves
	u32 statAhead = 0;			// made by the thread ahead of a frame wanting them

private:
	enum { SLOTS = 8192 };	// (SSAA makes each pipeline of a polygon again at two rates)
	struct Slot
	{
		u64 key;
		ID3D12PipelineState *pso;
	};
	static u32 Hash(u64 k) { k ^= k >> 29; k *= 0x9E3779B97F4A7C15ull; return (u32)(k >> 40) & (SLOTS - 1); }
	ID3D12PipelineState *Make(u64 key, u32 slot, bool inFrame);
	ID3D12PipelineState *Missed(u64 key);
	void Add(u64 key);
	bool Build(u64 key, ID3D12PipelineState **out);
	/* Ahead: @key wanted (made, or queued to be); a batch handed over */
	bool Requested(u64 key);
	void Request(u64 key);
	void Want(u64 key);
	void SubmitBatch();
	bool WantsPostZ(const DX12PsoKey& key) const;
	/* What the thread handed back, into the table */
	void TakeBuilt();
	void Insert(u64 key, ID3D12PipelineState *pso, u32 flags);
	void Count(u32 flags);
	void LoadFile();
	void SaveFile();

	ID3D12Device *device = nullptr;
	ID3D12Device1 *device1 = nullptr;
	ID3D12RootSignature *root = nullptr;
	Slot *slots = nullptr;
	u32 used = 0;
	ID3D12PipelineLibrary *library = nullptr;
	std::vector<u8> libraryData;		// what the library was made from: it reads it for as long as it lives
	std::vector<u64> keys;				// the file's, then those made
	size_t fileKeys = 0;				// how many of them are the file's
	u32 storedSinceLoad = 0;
	bool started = false;
	d3d_warm_t *warm = nullptr;
	u64 *requested = nullptr;			// every key made or queued, open addressing
	u32 requestedUsed = 0;
	std::vector<d3d_warm_job_t> batch;	// queued by Request(), handed over by SubmitBatch()
	/* SSAA's lists' pipelines made with NVIDIA's post-depth coverage
	 * (dx12_nvapi.h): on an NVIDIA GPU whose driver has NVAPI, until it
	 * would not make one */
	bool postZCoverage = false;
};
