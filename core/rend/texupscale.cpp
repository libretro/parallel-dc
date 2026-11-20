/* Textures scaled up with xBRZ (deps/xbrz, which is C++): the texture
 * cache's (TexCache.h's texupscale_run()), its rows shared out to a band
 * pool's threads */
#include "TexCache.h"
#include "band_pool.h"
#include "deps/xbrz/xbrz.h"

static struct xbrz::ScalerCfg xbrz_cfg;

struct XbrzJob
{
	int factor;
	u32 *source;
	u32 *dest;
	int width;
	int height;
	bool has_alpha;
};

static void xbrz_rows(void *ctx, int first, int last)
{
	const XbrzJob *job = (const XbrzJob *)ctx;
	xbrz::scale(job->factor, job->source, job->dest, job->width, job->height,
			job->has_alpha ? xbrz::ColorFormat::ARGB : xbrz::ColorFormat::RGB, xbrz_cfg, first, last);
}

void texupscale_run(struct band_pool *pool, int factor, u32 *source, u32 *dest, int width, int height, bool has_alpha)
{
	XbrzJob job = { factor, source, dest, width, height, has_alpha };
	band_pool_run(pool, xbrz_rows, &job, 0, height);
}
