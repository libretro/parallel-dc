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
#include "grow_buf.h"
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

/* What is kept from one frame to the next, in memory that is only grown
 * (grow_buf.h): nothing is allocated a frame once it is as large as the
 * frames need.
 *
 * keptStrips/keptXy: the frame before's strips and their vertices' places.
 * keptTable: those strips by signature - a signature's place has where its
 * strips' indices start in keptIndex, in the order they were sent, and how
 * many there are; made by counting, not sorting.
 * seenTable: this frame's strips counted by signature, as they are matched.
 * nextStrips/nextXy: this frame's, which become the kept ones after. */
static grow_buf_t keptStrips, keptXy, nextStrips, nextXy;
static u32 keptCount, nextCount, nextXyCount;
struct SigSlot
{
	u64 signature;
	u32 start;          /* keptTable: the first of its strips in keptIndex */
	u32 count;          /* keptTable: its strips; seenTable: seen so far */
	u32 frame;          /* the frame it is for: older ones are empty */
};
static grow_buf_t keptTable, keptIndex, seenTable;
static u32 keptMask, seenMask, tableFrame;

static u32 sig_hash(u64 signature)
{
	return (u32)((signature ^ (signature >> 29)) * 0x9E3779B97F4A7C15ull >> 32);
}

/* @signature's place in @table (@mask + 1 places), made for this frame if
 * it has none */
static SigSlot *sig_slot(SigSlot *table, u32 mask, u64 signature)
{
	u32 i = sig_hash(signature) & mask;
	while (table[i].frame == tableFrame && table[i].signature != signature)
		i = (i + 1) & mask;
	SigSlot *slot = &table[i];
	if (slot->frame != tableFrame)
	{
		slot->frame = tableFrame;
		slot->signature = signature;
		slot->start = 0;
		slot->count = 0;
	}
	return slot;
}

/* @signature's place in @table, NULL if it has none this frame */
static const SigSlot *sig_find(const SigSlot *table, u32 mask, u64 signature)
{
	for (u32 i = sig_hash(signature) & mask; table[i].frame == tableFrame; i = (i + 1) & mask)
		if (table[i].signature == signature)
			return &table[i];
	return NULL;
}

/* A table of at least twice @entries places (a power of two), all empty */
static SigSlot *sig_table(grow_buf_t *buf, u32 *mask, u32 entries)
{
	u32 size = 64;
	while (size < entries * 2)
		size *= 2;
	if (buf->size < size * sizeof(SigSlot))
	{
		grow_buf_reserve(buf, size * sizeof(SigSlot));
		memset(buf->data, 0, buf->size);
	}
	/* (as large as it has ever been: emptier, and empty for this frame
	 * whatever it held, for its places are this frame's or not) */
	*mask = (u32)(buf->size / sizeof(SigSlot)) - 1;
	while ((*mask & (*mask + 1)) != 0)
		(*mask)--;
	return (SigSlot *)buf->data;
}

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

static void match_list(const rend_context& ctx, const List<PolyParam>& list, float *prev_xy)
{
	const Vertex *verts = ctx.verts.head();
	const u32 *idx = ctx.idx.head();
	const KeptStrip *kept_strips = (const KeptStrip *)keptStrips.data;
	const float *kept_xy = (const float *)keptXy.data;
	const u32 *kept_index = (const u32 *)keptIndex.data;
	const SigSlot *kept_table = (const SigSlot *)keptTable.data;
	SigSlot *seen_table = (SigSlot *)seenTable.data;
	KeptStrip *next_strips = (KeptStrip *)nextStrips.data;
	float *next_xy = (float *)nextXy.data;
	for (int i = 0; i < list.used(); i++)
	{
		const PolyParam& pp = list.head()[i];
		if (pp.count < 3)
			continue;
		const u64 signature = strip_signature(pp);
		const u32 ordinal = sig_slot(seen_table, seenMask, signature)->count++;
		const KeptStrip *kept = NULL;
		const SigSlot *slot = keptCount != 0 ? sig_find(kept_table, keptMask, signature) : NULL;
		if (slot != NULL && ordinal < slot->count)
			kept = &kept_strips[kept_index[slot->start + ordinal]];
		/* (the vertices as they are now go to the next frame, matched or not) */
		KeptStrip& next = next_strips[nextCount++];
		next.signature = signature;
		next.first = nextXyCount / 2;
		next.count = pp.count;
		bool still = kept == NULL;
		if (kept != NULL)
		{
			/* one vertex that went too far, and the strip stands still */
			for (u32 v = 0; v < pp.count && !still; v++)
			{
				const Vertex& vertex = verts[idx[pp.first + v]];
				const float px = kept_xy[(kept->first + v) * 2], py = kept_xy[(kept->first + v) * 2 + 1];
				if (fabsf(px - vertex.x) > FARTHEST || fabsf(py - vertex.y) > FARTHEST)
					still = true;
			}
		}
		for (u32 v = 0; v < pp.count; v++)
		{
			const u32 vi = idx[pp.first + v];
			const Vertex& vertex = verts[vi];
			next_xy[nextXyCount++] = vertex.x;
			next_xy[nextXyCount++] = vertex.y;
			if (!still)
			{
				prev_xy[vi * 2] = kept_xy[(kept->first + v) * 2];
				prev_xy[vi * 2 + 1] = kept_xy[(kept->first + v) * 2 + 1];
			}
		}
	}
}

/* The kept strips by signature (keptTable, keptIndex), from keptStrips:
 * counted, the starts summed, the indices placed in the order sent */
static void index_kept(void)
{
	const KeptStrip *strips = (const KeptStrip *)keptStrips.data;
	SigSlot *table = sig_table(&keptTable, &keptMask, keptCount);
	u32 *index = (u32 *)grow_buf_reserve(&keptIndex, (size_t)(keptCount ? keptCount : 1) * sizeof(u32));
	u32 i, start = 0;

	for (i = 0; i < keptCount; i++)
		sig_slot(table, keptMask, strips[i].signature)->count++;
	for (i = 0; i <= keptMask; i++)
		if (table[i].frame == tableFrame)
		{
			table[i].start = start;
			start += table[i].count;
			table[i].count = 0;
		}
	for (i = 0; i < keptCount; i++)
	{
		SigSlot *slot = sig_slot(table, keptMask, strips[i].signature);
		index[slot->start + slot->count++] = i;
	}
}

void upscaler_match(const rend_context& ctx, float *prev_xy)
{
	const u32 count = (u32)ctx.verts.used();
	const Vertex *verts = ctx.verts.head();
	for (u32 i = 0; i < count; i++)
	{
		prev_xy[i * 2] = verts[i].x;
		prev_xy[i * 2 + 1] = verts[i].y;
	}
	if (resetPending)
		keptCount = 0;
	/* this frame's room: as many strips and vertices as its lists have */
	const List<PolyParam> *lists[2] = { &ctx.global_param_op, &ctx.global_param_pt };
	u32 strips = 0, vertices = 0;
	for (int l = 0; l < 2; l++)
	{
		strips += (u32)lists[l]->used();
		for (int i = 0; i < lists[l]->used(); i++)
			vertices += lists[l]->head()[i].count;
	}
	grow_buf_reserve(&nextStrips, (size_t)(strips ? strips : 1) * sizeof(KeptStrip));
	grow_buf_reserve(&nextXy, (size_t)(vertices ? vertices : 1) * 2 * sizeof(float));
	nextCount = nextXyCount = 0;
	/* (this frame's places: the kept table's, the frame before's, are
	 * looked up with the frame they were made for) */
	tableFrame++;
	if (tableFrame == 0)
	{
		/* (after four billion frames: every place emptied) */
		if (seenTable.data != NULL)
			memset(seenTable.data, 0, seenTable.size);
		if (keptTable.data != NULL)
			memset(keptTable.data, 0, keptTable.size);
		tableFrame = 1;
		keptCount = 0;
	}
	sig_table(&seenTable, &seenMask, strips);
	if (keptCount != 0)
	{
		/* the kept table was made last frame, for last frame: made again
		 * for this one (its places are found by the frame they are for) */
		index_kept();
	}
	match_list(ctx, ctx.global_param_op, prev_xy);
	match_list(ctx, ctx.global_param_pt, prev_xy);
	grow_buf_swap(&keptStrips, &nextStrips);
	grow_buf_swap(&keptXy, &nextXy);
	keptCount = nextCount;
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
