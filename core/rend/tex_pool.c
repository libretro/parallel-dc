/* The texture pool's book (tex_pool.h) */
#include "tex_pool.h"

u32 tex_pool_rect_share(u32 lw, u32 lh)
{
	/* by the shorter side, for one twice as long; half as many for each doubling more */
	static const u32 shorter[TEX_POOL_SIZES] = { 2, 4, 6, 6, 4, 2, 1, 0 };
	const u32 d = lw > lh ? lw - lh : lh - lw;
	return d == 0 ? 0 : shorter[lw < lh ? lw : lh] >> (d - 1);
}

u32 tex_pool_target_count(u32 lw, u32 lh)
{
	if (lw == lh)
		return lw == 3 ? 8 : lw >= 4 && lw <= 6 ? 2 : lw == 7 ? 1 : 0;
	if (lw == 7 && lh == 6)
		return 4;
	if (lw == 6 && lh == 5)
		return 2;
	if ((lw == lh + 2 || lh == lw + 2) && (lw > lh ? lw : lh) <= 5)
		return 2;
	return lw >= 3 && lh >= 3 && (lw == lh + 1 || lh == lw + 1) ? 1 : 0;
}

int tex_pool_drawn(u32 f, u32 lw, u32 lh, bool mips, u32 k, u32 up_max, const u32 *texel, u32 *bytes)
{
	int c;
	if (k != 0 && (8u << lw) * (8u << lh) <= up_max * up_max)
	{
		if (lw + k >= TEX_POOL_SIZES || lh + k >= TEX_POOL_SIZES || texel[TEX_POOL_FORMAT_8888] == 0)
			return -1;
		c = tex_pool_class(TEX_POOL_FORMAT_8888, 8u << (lw + k), 8u << (lh + k), mips);
		*bytes = tex_pool_bytes(c, texel[TEX_POOL_FORMAT_8888]);
		return c;
	}
	c = tex_pool_class(f, 8u << lw, 8u << lh, mips);
	*bytes = tex_pool_bytes(c, texel[f]);
	return c;
}

/* (2^k for a factor of @n, TEX_POOL_SIZES for one no power of two, 0 for none) */
static u32 tex_pool_log2_factor(u32 n)
{
	u32 k = 0;
	if (n <= 1)
		return 0;
	while ((2u << k) <= n)
		k++;
	return (1u << k) != n ? TEX_POOL_SIZES : k;
}

u64 tex_pool_plan(const VramBudget *budget, const u32 *texel, const u32 *use, const u8 *kind, u32 formats, u32 *counts,
		u32 upscale, u32 up_max, u32 rtt)
{
	/* For a format used as much as 565: square, mipmapped, then not;
	 * from 8 texels to 1024. */
	static const u32 mipped[TEX_POOL_SIZES] = { 1, 4, 6, 12, 16, 16, 4, 0 };
	static const u32 plain[TEX_POOL_SIZES]  = { 0, 0, 1,  2,  4,  4, 2, 0 };
	u64 unit = 0, rectUnit = 0, want, bytes = 0, squares, rects;
	u32 f, s, t, b;
	/* (2^k: a factor no power of two leaves those it scales out) */
	const u32 k = tex_pool_log2_factor(upscale);
	/* (renders to a texture @rtt times finer: 2^rk, none for a factor
	 * no power of two) */
	const u32 rk = tex_pool_log2_factor(rtt);
	int c;

	memset(counts, 0, sizeof(u32) * TEX_POOL_CLASSES);
	for (f = 0; f < formats && f < TEX_POOL_MAX_FORMATS; f++)
	{
		if (texel[f] == 0 || (kind != NULL && kind[f] != TEX_POOL_TEXTURES))
			continue;
		for (s = 0; s < TEX_POOL_SIZES; s++)
		{
			if (tex_pool_drawn(f, s, s, true, k, up_max, texel, &b) >= 0)
				unit += (u64)mipped[s] * use[f] * b;
			if (tex_pool_drawn(f, s, s, false, k, up_max, texel, &b) >= 0)
				unit += (u64)plain[s] * use[f] * b;
			for (t = 0; t < TEX_POOL_SIZES; t++)
				if (tex_pool_drawn(f, s, t, false, k, up_max, texel, &b) >= 0)
					rectUnit += (u64)tex_pool_rect_share(s, t) * use[f] * b;
		}
	}
	want = (u64)vram_budget_available_kib(budget) * 1024 / 16;
	if (want < (u64)16 << 20)
		want = (u64)16 << 20;
	if (want > (u64)128 << 20)
		want = (u64)128 << 20;
	/* the tables above times their part of want / unit, rounded up (the units are in eighths) */
	squares = want * 8 * 3 / 4;
	rects = want * 8 / 4;
	for (f = 0; f < formats && f < TEX_POOL_MAX_FORMATS; f++)
	{
		if (texel[f] == 0)
			continue;
		if (kind != NULL && kind[f] == TEX_POOL_TARGETS)
		{
			for (s = 0; s < TEX_POOL_SIZES; s++)
				for (t = 0; t < TEX_POOL_SIZES; t++)
				{
					/* (half as many of 256 a side and larger on a card
					 * with little memory: the smaller, which a game
					 * renders more of at once, take little) */
					const u32 n = want < (u64)64 << 20 && (s >= 5 || t >= 5)
							? (tex_pool_target_count(s, t) + 1) / 2 : tex_pool_target_count(s, t);
					c = tex_pool_class(f, 8u << s, 8u << t, false);
					counts[c] += n;
					bytes += (u64)n * tex_pool_bytes(c, texel[f]);
					/* ...and, rendered @rtt times finer, the same
					 * renders as large as they are drawn (those still
					 * no larger than 1024) */
					if (rk != 0 && rk < TEX_POOL_SIZES && s + rk < TEX_POOL_SIZES && t + rk < TEX_POOL_SIZES && n != 0)
					{
						c = tex_pool_class(f, 8u << (s + rk), 8u << (t + rk), false);
						counts[c] += n;
						bytes += (u64)n * tex_pool_bytes(c, texel[f]);
					}
				}
			continue;
		}
		for (s = 0; s < TEX_POOL_SIZES && unit != 0; s++)
		{
			u32 n;
			if ((c = tex_pool_drawn(f, s, s, true, k, up_max, texel, &b)) >= 0)
			{
				n = (u32)(((u64)mipped[s] * use[f] * squares + unit * 8 - 1) / (unit * 8));
				counts[c] += n;
				bytes += (u64)n * b;
			}
			if ((c = tex_pool_drawn(f, s, s, false, k, up_max, texel, &b)) >= 0)
			{
				n = (u32)(((u64)plain[s] * use[f] * squares + unit * 8 - 1) / (unit * 8));
				if (use[f] != 0 && n < tex_pool_least(s))
					n = tex_pool_least(s);
				counts[c] += n;
				bytes += (u64)n * b;
			}
			for (t = 0; t < TEX_POOL_SIZES && rectUnit != 0; t++)
			{
				if (t == s || (c = tex_pool_drawn(f, s, t, false, k, up_max, texel, &b)) < 0)
					continue;
				/* (to the nearest: there are many of them, and few of each are used) */
				n = (u32)(((u64)tex_pool_rect_share(s, t) * use[f] * rects + rectUnit * 4) / (rectUnit * 8));
				counts[c] += n;
				bytes += (u64)n * b;
			}
		}
	}
	return bytes;
}

void tex_pool_reset(tex_pool_book_t *b, u32 frames)
{
	memset(b->free, 0, sizeof(b->free));
	memset(b->retired, 0, sizeof(b->retired));
	memset(b->said, 0, sizeof(b->said));
	b->frames = frames == 0 ? 1 : frames > TEX_POOL_MAX_FRAMES ? TEX_POOL_MAX_FRAMES : frames;
	b->current = 0;
	b->made = b->madeLate = b->lateClasses = 0;
	b->bytesMade = b->bytesLate = 0;
}

void tex_pool_made(tex_pool_book_t *b, TexPoolEntry *e, int cls, u32 bytes, bool late, const char *format)
{
	e->next = NULL;
	e->cls = (u32)cls;
	e->bytes = bytes;
	b->made++;
	b->bytesMade += bytes;
	if (!late)
		return;
	b->madeLate++;
	b->bytesLate += bytes;
	if (!b->said[cls])
	{
		b->said[cls] = true;
		b->lateClasses++;
		WARN_LOG(RENDERER, "Texture pool: no %ux%u %s%s image left, one made (the pool is too small for this game)\n",
				tex_pool_width(cls), tex_pool_height(cls), format, tex_pool_mips(cls) ? " mipmapped" : "");
	}
}

void tex_pool_frame(tex_pool_book_t *b, u32 index)
{
	TexPoolEntry *e;
	b->current = index % b->frames;
	e = b->retired[b->current];
	b->retired[b->current] = NULL;
	while (e != NULL)
	{
		TexPoolEntry *next = e->next;
		tex_pool_give(b, e);
		e = next;
	}
}

TexPoolEntry *tex_pool_drain(tex_pool_book_t *b)
{
	u32 i, c;
	for (i = 0; i < TEX_POOL_MAX_FRAMES; i++)
		tex_pool_frame(b, i);
	for (c = 0; c < TEX_POOL_CLASSES; c++)
		if (b->free[c] != NULL)
		{
			TexPoolEntry *e = b->free[c];
			b->free[c] = e->next;
			return e;
		}
	return NULL;
}

void tex_pool_summary(const tex_pool_book_t *b, const char *api)
{
	NOTICE_LOG(RENDERER, "%s: texture pool, %u images made (%u MB), %u of them while the game ran (%u MB, %u shapes)\n",
			api, b->made, (u32)(b->bytesMade >> 20), b->madeLate, (u32)(b->bytesLate >> 20), b->lateClasses);
}
