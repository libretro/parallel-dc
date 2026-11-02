/* A polygon's blend in the Direct3D 12 renderer, where the factors that
 * read only the source are applied by its pixel shader (dx12_pso.h,
 * dx12_src_folds()): for every pair of the TSP's instructions, and colours
 * in and out of the frame's range, what the blender makes of the shader's
 * two colours with the pipeline's blend (dx12_blend_desc() of the key as
 * DX12PsoKey::Pack() folds it, the shader's factors as
 * dx12_shader_flags.h's DX12_SRC_FOLD_FACTOR and DX12_DST_FOLD_FACTOR
 * have them) against the PowerVR's blend of the colour and the frame's
 * pixel. And the keys: 15 blends for the 64 pairs, a folded key packed
 * again the same, and the key of a pair from an earlier layout (a file's)
 * folded to the one the renderer asks for. Built with mingw-w64 and run
 * under wine by dx12_blend_fold_test.sh: no device. */
#include "rend/dx12/dx12_pso.h"
#include <math.h>
#include <set>
#include <stdio.h>

struct Col
{
	float c[4];
};

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* The PowerVR's factor @i of the source (@source) or the destination, for
 * channel @ch (3: the alpha) of the source colour @s and the frame's
 * @d: the other colour is the destination's for the source, the
 * source's for the destination; of the alpha, the other's alpha */
static float pvr_factor(u32 i, bool source, const Col& s, const Col& d, int ch)
{
	const Col& other = source ? d : s;
	switch (i)
	{
	case 0: return 0.0f;
	case 1: return 1.0f;
	case 2: return other.c[ch];
	case 3: return 1.0f - other.c[ch];
	case 4: return s.c[3];
	case 5: return 1.0f - s.c[3];
	case 6: return d.c[3];
	default: return 1.0f - d.c[3];
	}
}

/* A Direct3D blend factor, for channel @ch, of the shader's colours @o0
 * and @o1 (clamped, as the blender takes them for a UNORM frame) and the
 * frame's @d */
static float d3d_factor(D3D12_BLEND f, const Col& o0, const Col& o1, const Col& d, int ch, bool *known)
{
	switch (f)
	{
	case D3D12_BLEND_ZERO: return 0.0f;
	case D3D12_BLEND_ONE: return 1.0f;
	case D3D12_BLEND_SRC_COLOR: return o0.c[ch];
	case D3D12_BLEND_INV_SRC_COLOR: return 1.0f - o0.c[ch];
	case D3D12_BLEND_SRC_ALPHA: return o0.c[3];
	case D3D12_BLEND_INV_SRC_ALPHA: return 1.0f - o0.c[3];
	case D3D12_BLEND_DEST_ALPHA: return d.c[3];
	case D3D12_BLEND_INV_DEST_ALPHA: return 1.0f - d.c[3];
	case D3D12_BLEND_DEST_COLOR: return d.c[ch];
	case D3D12_BLEND_INV_DEST_COLOR: return 1.0f - d.c[ch];
	case D3D12_BLEND_SRC1_COLOR: return o1.c[ch];
	case D3D12_BLEND_SRC1_ALPHA: return o1.c[3];
	default:
		*known = false;
		return 0.0f;
	}
}

int main()
{
	static const Col colours[] = {
		{ { 0.0f, 0.0f, 0.0f, 0.0f } }, { { 1.0f, 1.0f, 1.0f, 1.0f } },
		{ { 0.25f, 0.5f, 0.75f, 0.5f } }, { { 0.9f, 0.1f, 0.4f, 0.2f } },
		// (out of range: the shader's colour with an offset added, or a clamp below zero)
		{ { 1.3f, -0.2f, 0.6f, 1.4f } }, { { -0.5f, 2.0f, 0.3f, -0.1f } },
	};
	static const Col frames[] = {
		{ { 0.0f, 0.0f, 0.0f, 0.0f } }, { { 1.0f, 1.0f, 1.0f, 1.0f } },
		{ { 0.6f, 0.3f, 0.1f, 0.7f } }, { { 0.2f, 0.8f, 0.5f, 0.35f } },
	};
	unsigned failed = 0, checked = 0;
	std::set<u64> pipelines;

	for (u32 src = 0; src < 8; src++)
		for (u32 dst = 0; dst < 8; dst++)
		{
			DX12PsoKey key{};
			key.kind = DX12_PSO_POLY;
			key.blend = true;
			key.writeColor = true;
			key.src = src;
			key.dst = dst;
			const u64 packed = key.Pack();
			pipelines.insert(packed);
			const DX12PsoKey made = DX12PsoKey::Unpack(packed);
			if (made.Pack() != packed || !DX12PsoKey::Valid(packed))
			{
				printf("FAIL: %u %u: the folded key %016llx does not pack again the same, or is not valid\n",
						src, dst, (unsigned long long)packed);
				failed++;
			}
			D3D12_RENDER_TARGET_BLEND_DESC rt{};
			dx12_blend_desc(made, rt);
			if (!rt.BlendEnable)
			{
				printf("FAIL: %u %u: not blended\n", src, dst);
				failed++;
			}
			// (Direct3D takes no colour factor for the alpha)
			static const D3D12_BLEND colourFactors[] = { D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_DEST_COLOR,
					D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC1_COLOR, D3D12_BLEND_INV_SRC1_COLOR };
			for (D3D12_BLEND f : colourFactors)
				if (rt.SrcBlendAlpha == f || rt.DestBlendAlpha == f)
				{
					printf("FAIL: %u %u: a colour factor for the alpha\n", src, dst);
					failed++;
				}
			// the shader's flags, as DX12Renderer::DrawPoly() sets them
			const bool srcFold = dx12_src_folds(src), dstFold = dx12_dst_folds(dst);
			for (const Col& colour : colours)
				for (const Col& frame : frames)
				{
					// the shader's two colours
					Col s, o0, o1;
					for (int ch = 0; ch < 4; ch++)
						s.c[ch] = clamp01(colour.c[ch]);
					for (int ch = 0; ch < 4; ch++)
					{
						o0.c[ch] = srcFold ? s.c[ch] * DX12_SRC_FOLD_FACTOR(src, s.c[3]) : colour.c[ch];
						o1.c[ch] = dstFold ? DX12_DST_FOLD_FACTOR(dst, s.c[ch], s.c[3]) : 0.0f;
					}
					for (int ch = 0; ch < 4; ch++)
					{
						o0.c[ch] = clamp01(o0.c[ch]);
						o1.c[ch] = clamp01(o1.c[ch]);
					}
					for (int ch = 0; ch < 4; ch++)
					{
						const bool alpha = ch == 3;
						bool known = true;
						const float fs = d3d_factor(alpha ? rt.SrcBlendAlpha : rt.SrcBlend, o0, o1, frame, ch, &known);
						const float fd = d3d_factor(alpha ? rt.DestBlendAlpha : rt.DestBlend, o0, o1, frame, ch, &known);
						const float got = o0.c[ch] * fs + frame.c[ch] * fd;
						const float want = s.c[ch] * pvr_factor(src, true, s, frame, ch)
								+ frame.c[ch] * pvr_factor(dst, false, s, frame, ch);
						checked++;
						if (!known || fabsf(got - want) > 1e-6f)
						{
							if (failed < 20)
								printf("FAIL: src %u dst %u channel %d: %g, the PowerVR's %g\n", src, dst, ch, got, want);
							failed++;
						}
					}
				}
		}
	// the pipelines the 64 pairs need
	if (pipelines.size() != 15)
	{
		printf("FAIL: %u pipelines for the 64 blends, not 15\n", (unsigned)pipelines.size());
		failed++;
	}
	// a key from a file of version 2: its blend as the instructions were, not folded
	{
		const u64 old = (u64)DX12_PSO_POLY | (1ull << 4) | (4ull << 5) | (5ull << 8) | (1ull << 11) | (1ull << 28);
		DX12PsoKey key{};
		key.kind = DX12_PSO_POLY;
		key.blend = true;
		key.writeColor = true;
		key.src = 4;
		key.dst = 5;
		if (!DX12PsoKey::Valid(old) || DX12PsoKey::Unpack(old).Pack() != key.Pack())
		{
			printf("FAIL: a key of the earlier layout is not folded to the renderer's\n");
			failed++;
		}
	}
	// nothing but a blended polygon is folded
	{
		DX12PsoKey key{};
		key.kind = DX12_PSO_OIT;
		key.blend = true;
		key.src = 4;
		key.dst = 5;
		const DX12PsoKey made = DX12PsoKey::Unpack(key.Pack());
		if (made.dual || made.src != 4 || made.dst != 5)
		{
			printf("FAIL: a per-pixel renderer's key was folded\n");
			failed++;
		}
	}
	printf("dx12_blend_fold_test: %u channels of 64 blends checked, %u pipelines\n", checked, (unsigned)pipelines.size());
	printf("dx12_blend_fold_test: %s\n", failed == 0 ? "PASS" : "FAIL");
	return failed == 0 ? 0 : 1;
}
