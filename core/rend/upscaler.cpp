/*
	rend/upscaler.h

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
#include <math.h>
#include <unordered_map>
#include "upscaler.h"
#include "hw/pvr/ta_ctx.h"

static const struct retro_hw_render_upscaler_interface *upscaler;
static struct retro_hw_upscaler_request request;
static bool requested;
static int renderWidth, renderHeight;
static struct retro_hw_upscaler_inputs inputs;
static unsigned jitterIndex;
static bool resetPending = true;

/* A strip of the frame before: what it was sent under, and where its
 * vertices were (an index into the kept positions) */
struct KeptStrip
{
	u64 signature;
	u32 first;
	u32 count;
};
static std::vector<KeptStrip> keptStrips;
static std::vector<float> keptXy;
/* ...looked up by signature: the strips sent under it, in order */
static std::unordered_map<u64, std::vector<u32>> keptBySignature;
static std::vector<KeptStrip> nextStrips;
static std::vector<float> nextXy;

void upscaler_set_interface(const struct retro_hw_render_upscaler_interface *iface)
{
	upscaler = iface;
	requested = false;
	renderWidth = renderHeight = 0;
	upscaler_reset();
}

bool upscaler_poll(bool wanted)
{
	int width = 0, height = 0;
	if (wanted && upscaler != NULL && upscaler->get_request(upscaler->handle, &request))
	{
		width = (int)request.render_width;
		height = (int)request.render_height;
	}
	requested = width > 0 && height > 0;
	if (!requested)
		width = height = 0;
	if (width == renderWidth && height == renderHeight)
		return false;
	renderWidth = width;
	renderHeight = height;
	return true;
}

bool upscaler_active()
{
	return requested;
}

void upscaler_render_size(int *width, int *height)
{
	*width = renderWidth;
	*height = renderHeight;
}

/* Halton (2, 3), the sequence DLSS's own samples use, over the phase
 * the ratio of the sizes wants; at least 8 frames of it */
static float halton(unsigned index, unsigned base)
{
	float r = 0.f, f = 1.f;
	for (index++; index > 0; index /= base)
	{
		f /= base;
		r += f * (index % base);
	}
	return r;
}

static unsigned jitter_phase()
{
	if (!requested || renderWidth == 0)
		return 8;
	const float ratio = (float)request.output_width / (float)renderWidth;
	const unsigned phase = (unsigned)ceilf(8.f * ratio * ratio);
	return phase < 8 ? 8 : phase > 64 ? 64 : phase;
}

void upscaler_jitter(float *x, float *y)
{
	if (!requested)
	{
		*x = *y = 0.f;
		return;
	}
	const unsigned i = jitterIndex % jitter_phase();
	*x = halton(i, 2) - 0.5f;
	*y = halton(i, 3) - 0.5f;
}

static u64 strip_signature(const PolyParam& pp)
{
	u64 h = 0xcbf29ce484222325ull;
	const u32 words[] = { pp.isp.full, pp.tsp.full, pp.tcw.full, pp.pcw.full, pp.count,
			(u32)pp.texid, (u32)(pp.texid >> 32) };
	for (u32 w : words)
	{
		h ^= w;
		h *= 0x100000001b3ull;
	}
	return h;
}

/* Further than this, in the game's pixels, nothing on screen is the
 * same thing a frame later */
static const float FARTHEST = 160.f;

static void match_list(const rend_context& ctx, const List<PolyParam>& list, std::vector<float>& prev_xy,
		std::unordered_map<u64, u32>& seen)
{
	const Vertex *verts = ctx.verts.head();
	const u32 *idx = ctx.idx.head();
	for (int i = 0; i < list.used(); i++)
	{
		const PolyParam& pp = list.head()[i];
		if (pp.count < 3)
			continue;
		const u64 signature = strip_signature(pp);
		const u32 ordinal = seen[signature]++;
		const KeptStrip *kept = NULL;
		auto it = keptBySignature.find(signature);
		if (it != keptBySignature.end() && ordinal < it->second.size())
			kept = &keptStrips[it->second[ordinal]];
		/* (the vertices as they are now go to the next frame, matched or not) */
		KeptStrip next;
		next.signature = signature;
		next.first = (u32)nextXy.size() / 2;
		next.count = pp.count;
		bool still = kept == NULL;
		if (kept != NULL)
		{
			/* one vertex that went too far, and the strip stands still */
			for (u32 v = 0; v < pp.count && !still; v++)
			{
				const Vertex& vertex = verts[idx[pp.first + v]];
				const float px = keptXy[(kept->first + v) * 2], py = keptXy[(kept->first + v) * 2 + 1];
				if (fabsf(px - vertex.x) > FARTHEST || fabsf(py - vertex.y) > FARTHEST)
					still = true;
			}
		}
		for (u32 v = 0; v < pp.count; v++)
		{
			const u32 vi = idx[pp.first + v];
			const Vertex& vertex = verts[vi];
			nextXy.push_back(vertex.x);
			nextXy.push_back(vertex.y);
			if (!still)
			{
				prev_xy[vi * 2] = keptXy[(kept->first + v) * 2];
				prev_xy[vi * 2 + 1] = keptXy[(kept->first + v) * 2 + 1];
			}
		}
		nextStrips.push_back(next);
	}
}

void upscaler_match(const rend_context& ctx, std::vector<float>& prev_xy)
{
	const u32 count = (u32)ctx.verts.used();
	prev_xy.resize((size_t)count * 2);
	for (u32 i = 0; i < count; i++)
	{
		prev_xy[i * 2] = ctx.verts.head()[i].x;
		prev_xy[i * 2 + 1] = ctx.verts.head()[i].y;
	}
	nextStrips.clear();
	nextXy.clear();
	std::unordered_map<u64, u32> seen;
	if (resetPending)
	{
		keptBySignature.clear();
		keptStrips.clear();
		keptXy.clear();
	}
	match_list(ctx, ctx.global_param_op, prev_xy, seen);
	match_list(ctx, ctx.global_param_pt, prev_xy, seen);
	keptStrips.swap(nextStrips);
	keptXy.swap(nextXy);
	keptBySignature.clear();
	for (u32 i = 0; i < (u32)keptStrips.size(); i++)
		keptBySignature[keptStrips[i].signature].push_back(i);
}

void upscaler_submit(const struct retro_hw_upscaler_image& motion_vectors,
		const struct retro_hw_upscaler_image& depth, unsigned flags)
{
	if (upscaler == NULL || !requested)
		return;
	memset(&inputs, 0, sizeof(inputs));
	inputs.flags = flags | (resetPending ? RETRO_HW_UPSCALER_RESET : 0);
	inputs.motion_vectors = motion_vectors;
	inputs.depth = depth;
	upscaler_jitter(&inputs.jitter_x, &inputs.jitter_y);
	inputs.mv_scale_x = 1.f;
	inputs.mv_scale_y = 1.f;
	inputs.pre_exposure = 1.f;
	inputs.exposure_scale = 1.f;
	inputs.frame_time_ms = 1000.f / 60.f;
	upscaler->set_inputs(upscaler->handle, &inputs);
	resetPending = false;
	jitterIndex++;
}

void upscaler_reset()
{
	resetPending = true;
	jitterIndex = 0;
}
