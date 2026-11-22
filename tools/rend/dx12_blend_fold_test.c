/* A polygon's blend in the Direct3D 12 renderer, where the factors that
 * read only the source are applied by its pixel shader (dx12_pso.h,
 * dx12_src_folds()): for every pair of the TSP's instructions, and colours
 * in and out of the frame's range, what the blender makes of the shader's
 * two colours with the pipeline's blend (dx12_blend_desc() of the key as
 * dx12_pso_key_pack() folds it, the shader's factors as
 * dx12_shader_flags.h's DX12_SRC_FOLD_FACTOR and DX12_DST_FOLD_FACTOR
 * have them) against the PowerVR's blend of the colour and the frame's
 * pixel. And the keys: 15 blends for the 64 pairs, a folded key packed
 * again the same, and the key of a pair from an earlier layout (a file's)
 * folded to the one the renderer asks for; the per-pixel renderer's
 * polygons' blends the same; a polygon's cull mode and shading, applied
 * by its shaders, folded out of its key too. Built with mingw-w64 and run
 * under wine by dx12_blend_fold_test.sh: no device. */
#include "rend/d3d_system.h"
#include "rend/dx12/dx12_pso.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct Col
{
	float c[4];
} Col;

/* @v added to the @count distinct of @set (room for 64) */
static void add_distinct(u64 *set, unsigned *count, u64 v)
{
	unsigned i;
	for (i = 0; i < *count; i++)
		if (set[i] == v)
			return;
	set[(*count)++] = v;
}

static DX12PsoKey zero_key(void)
{
	DX12PsoKey k;
	memset(&k, 0, sizeof(k));
	return k;
}

/* @old unpacked and packed again, as the renderer would pack @key */
static bool folds_to(u64 old, const DX12PsoKey *key)
{
	const DX12PsoKey made = dx12_pso_key_unpack(old);
	return dx12_pso_key_valid(old) && dx12_pso_key_pack(&made) == dx12_pso_key_pack(key);
}

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* The PowerVR's factor @i of the source (@source) or the destination, for
 * channel @ch (3: the alpha) of the source colour @s and the frame's
 * @d: the other colour is the destination's for the source, the
 * source's for the destination; of the alpha, the other's alpha */
static float pvr_factor(u32 i, bool source, const Col *s, const Col *d, int ch)
{
	const Col *other = source ? d : s;
	switch (i)
	{
	case 0: return 0.0f;
	case 1: return 1.0f;
	case 2: return other->c[ch];
	case 3: return 1.0f - other->c[ch];
	case 4: return s->c[3];
	case 5: return 1.0f - s->c[3];
	case 6: return d->c[3];
	default: return 1.0f - d->c[3];
	}
}

/* A Direct3D blend factor, for channel @ch, of the shader's colours @o0
 * and @o1 (clamped, as the blender takes them for a UNORM frame) and the
 * frame's @d */
static float d3d_factor(D3D12_BLEND f, const Col *o0, const Col *o1, const Col *d, int ch, bool *known)
{
	switch (f)
	{
	case D3D12_BLEND_ZERO: return 0.0f;
	case D3D12_BLEND_ONE: return 1.0f;
	case D3D12_BLEND_SRC_COLOR: return o0->c[ch];
	case D3D12_BLEND_INV_SRC_COLOR: return 1.0f - o0->c[ch];
	case D3D12_BLEND_SRC_ALPHA: return o0->c[3];
	case D3D12_BLEND_INV_SRC_ALPHA: return 1.0f - o0->c[3];
	case D3D12_BLEND_DEST_ALPHA: return d->c[3];
	case D3D12_BLEND_INV_DEST_ALPHA: return 1.0f - d->c[3];
	case D3D12_BLEND_DEST_COLOR: return d->c[ch];
	case D3D12_BLEND_INV_DEST_COLOR: return 1.0f - d->c[ch];
	case D3D12_BLEND_SRC1_COLOR: return o1->c[ch];
	case D3D12_BLEND_SRC1_ALPHA: return o1->c[3];
	default:
		*known = false;
		return 0.0f;
	}
}

int main(void)
{
	static const Col colours[] = {
		{ { 0.0f, 0.0f, 0.0f, 0.0f } }, { { 1.0f, 1.0f, 1.0f, 1.0f } },
		{ { 0.25f, 0.5f, 0.75f, 0.5f } }, { { 0.9f, 0.1f, 0.4f, 0.2f } },
		/* (out of range: the shader's colour with an offset added, or a clamp below zero) */
		{ { 1.3f, -0.2f, 0.6f, 1.4f } }, { { -0.5f, 2.0f, 0.3f, -0.1f } }
	};
	static const Col frames[] = {
		{ { 0.0f, 0.0f, 0.0f, 0.0f } }, { { 1.0f, 1.0f, 1.0f, 1.0f } },
		{ { 0.6f, 0.3f, 0.1f, 0.7f } }, { { 0.2f, 0.8f, 0.5f, 0.35f } }
	};
	/* (Direct3D takes no colour factor for the alpha) */
	static const D3D12_BLEND colourFactors[] = { D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_DEST_COLOR,
			D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC1_COLOR, D3D12_BLEND_INV_SRC1_COLOR };
	unsigned failed = 0, checked = 0, pipelineCount = 0;
	u64 pipelines[64];
	u32 src, dst, kind;
	DX12PsoKey key;

	for (src = 0; src < 8; src++)
		for (dst = 0; dst < 8; dst++)
		{
			u64 packed;
			DX12PsoKey made;
			D3D12_RENDER_TARGET_BLEND_DESC rt;
			bool srcFold, dstFold;
			size_t f, ci, fi;
			key = zero_key();
			key.kind = DX12_PSO_POLY;
			key.blend = true;
			key.writeColor = true;
			key.src = src;
			key.dst = dst;
			packed = dx12_pso_key_pack(&key);
			add_distinct(pipelines, &pipelineCount, packed);
			made = dx12_pso_key_unpack(packed);
			if (dx12_pso_key_pack(&made) != packed || !dx12_pso_key_valid(packed))
			{
				printf("FAIL: %u %u: the folded key %016llx does not pack again the same, or is not valid\n",
						src, dst, (unsigned long long)packed);
				failed++;
			}
			memset(&rt, 0, sizeof(rt));
			dx12_blend_desc(&made, &rt);
			if (!rt.BlendEnable)
			{
				printf("FAIL: %u %u: not blended\n", src, dst);
				failed++;
			}
			for (f = 0; f < sizeof(colourFactors) / sizeof(colourFactors[0]); f++)
				if (rt.SrcBlendAlpha == colourFactors[f] || rt.DestBlendAlpha == colourFactors[f])
				{
					printf("FAIL: %u %u: a colour factor for the alpha\n", src, dst);
					failed++;
				}
			/* the shader's flags, as the renderer's DrawPoly() sets them */
			srcFold = dx12_src_folds(src);
			dstFold = dx12_dst_folds(dst);
			for (ci = 0; ci < sizeof(colours) / sizeof(colours[0]); ci++)
				for (fi = 0; fi < sizeof(frames) / sizeof(frames[0]); fi++)
				{
					const Col *colour = &colours[ci], *frame = &frames[fi];
					/* the shader's two colours */
					Col s, o0, o1;
					int ch;
					for (ch = 0; ch < 4; ch++)
						s.c[ch] = clamp01(colour->c[ch]);
					for (ch = 0; ch < 4; ch++)
					{
						o0.c[ch] = srcFold ? s.c[ch] * DX12_SRC_FOLD_FACTOR(src, s.c[3]) : colour->c[ch];
						o1.c[ch] = dstFold ? DX12_DST_FOLD_FACTOR(dst, s.c[ch], s.c[3]) : 0.0f;
					}
					for (ch = 0; ch < 4; ch++)
					{
						o0.c[ch] = clamp01(o0.c[ch]);
						o1.c[ch] = clamp01(o1.c[ch]);
					}
					for (ch = 0; ch < 4; ch++)
					{
						const bool alpha = ch == 3;
						bool known = true;
						const float fs = d3d_factor(alpha ? rt.SrcBlendAlpha : rt.SrcBlend, &o0, &o1, frame, ch, &known);
						const float fd = d3d_factor(alpha ? rt.DestBlendAlpha : rt.DestBlend, &o0, &o1, frame, ch, &known);
						const float got = o0.c[ch] * fs + frame->c[ch] * fd;
						const float want = s.c[ch] * pvr_factor(src, true, &s, frame, ch)
								+ frame->c[ch] * pvr_factor(dst, false, &s, frame, ch);
						checked++;
						if (!known || fabs(got - want) > 1e-6)
						{
							if (failed < 20)
								printf("FAIL: src %u dst %u channel %d: %g, the PowerVR's %g\n", src, dst, ch, got, want);
							failed++;
						}
					}
				}
		}
	/* the pipelines the 64 pairs need */
	if (pipelineCount != 15)
	{
		printf("FAIL: %u pipelines for the 64 blends, not 15\n", pipelineCount);
		failed++;
	}
	/* a key from a file of version 2: its blend as the instructions were, not folded */
	key = zero_key();
	key.kind = DX12_PSO_POLY;
	key.blend = true;
	key.writeColor = true;
	key.src = 4;
	key.dst = 5;
	if (!folds_to((u64)DX12_PSO_POLY | ((u64)1 << 4) | ((u64)4 << 5) | ((u64)5 << 8) | ((u64)1 << 11) | ((u64)1 << 28), &key))
	{
		printf("FAIL: a key of the earlier layout is not folded to the renderer's\n");
		failed++;
	}
	/* either renderer's blended polygon folded, to the same 15; nothing else */
	for (kind = 0; kind < 8; kind++)
	{
		const bool folds = kind == DX12_PSO_POLY || kind == DX12_PSO_OIT;
		u64 blends[64];
		unsigned blendCount = 0;
		for (src = 0; src < 8; src++)
			for (dst = 0; dst < 8; dst++)
			{
				u64 packed;
				DX12PsoKey made;
				key = zero_key();
				key.kind = kind;
				key.blend = true;
				key.writeColor = true;
				key.src = src;
				key.dst = dst;
				packed = dx12_pso_key_pack(&key);
				add_distinct(blends, &blendCount, packed);
				made = dx12_pso_key_unpack(packed);
				if (dx12_pso_key_pack(&made) != packed || !dx12_pso_key_valid(packed) || (!folds && (made.dual || made.src != src || made.dst != dst)))
				{
					printf("FAIL: kind %u: blend %u %u: the key %016llx not kept\n", kind, src, dst, (unsigned long long)packed);
					failed++;
				}
			}
		if (blendCount != (folds ? 15u : 64u))
		{
			printf("FAIL: kind %u: %u pipelines for the 64 blends, not %u\n", kind, blendCount, folds ? 15u : 64u);
			failed++;
		}
	}
	/* a polygon's cull mode and shading, which its shaders apply
	 * (DX12_PF_CULL_SHIFT, DX12_PF_GOURAUD): one pipeline for each mode and
	 * shading, a key of an earlier file's (version 4: the shading in it)
	 * folded the same; another kind's kept */
	for (kind = 0; kind < 8; kind++)
	{
		const bool folds = kind == DX12_PSO_POLY || kind == DX12_PSO_OIT;
		u64 keys[64];
		unsigned keyCount = 0;
		u32 cull, g;
		for (cull = 0; cull < 3; cull++)
			for (g = 0; g < 2; g++)
			{
				u64 packed;
				DX12PsoKey made;
				key = zero_key();
				key.kind = kind;
				key.writeColor = true;
				key.cull = cull;
				key.gouraud = g != 0;
				packed = dx12_pso_key_pack(&key);
				add_distinct(keys, &keyCount, packed);
				made = dx12_pso_key_unpack(packed);
				if (dx12_pso_key_pack(&made) != packed || (!folds && (made.cull != cull || made.gouraud != (g != 0))))
				{
					printf("FAIL: kind %u cull %u gouraud %u: the key %016llx not kept\n", kind, cull, g, (unsigned long long)packed);
					failed++;
				}
			}
		if (keyCount != (folds ? 1u : 6u))
		{
			printf("FAIL: kind %u: %u pipelines for 3 cull modes and 2 shadings, not %u\n", kind, keyCount, folds ? 1u : 6u);
			failed++;
		}
	}
	key = zero_key();
	key.kind = DX12_PSO_POLY;
	key.writeColor = true;
	if (!folds_to((u64)DX12_PSO_POLY | ((u64)1 << 3) | ((u64)1 << 11) | ((u64)1 << 28), &key))
	{
		printf("FAIL: a smooth-shaded key of an earlier file is not folded to the renderer's\n");
		failed++;
	}
	printf("dx12_blend_fold_test: %u channels of 64 blends checked, %u pipelines\n", checked, pipelineCount);
	printf("dx12_blend_fold_test: %s\n", failed == 0 ? "PASS" : "FAIL");
	return failed == 0 ? 0 : 1;
}
