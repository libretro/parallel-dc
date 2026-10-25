/* See hdr_output.h. C89. */
#include <string.h>

#include "hdr_output.h"
#include "hdr_transform.h"

struct hdr_output hdr_output;

void hdr_encode_params_fill(struct hdr_encode_params *p,
      const struct hdr_output *o, int deband)
{
   unsigned c, r;
   memset(p, 0, sizeof(*p));
   for (c = 0; c < 3; c++)
      for (r = 0; r < 3; r++)
         p->to_panel[c * 4 + r] = o->to_panel[c * 3 + r];
   p->paper_white  = o->paper_white_nits;
   p->peak         = o->max_nits;
   p->black_pq     = o->black_pq;
   p->expand_gamut = o->expand_gamut;
   p->shoulder     = o->shoulder;
   p->sdr_eotf     = o->sdr_eotf;
   p->scrgb        = o->format == HDR_OUTPUT_SCRGB;
   p->deband       = deband;
   p->ssaa_log2    = 0;
   p->pad[0] = p->pad[1] = p->pad[2] = 0;
}

static unsigned post_dim(unsigned v, unsigned shift)
{
   v >>= shift;
   return v ? v : 1u;
}

static void post_pass(struct hdr_post_plan *p, int stage, int src, int dst,
      const float p0[4], float sx, float sy)
{
   struct hdr_post_pass *q = &p->pass[p->passes++];
   memset(q, 0, sizeof(*q));
   q->stage = stage;
   q->src   = src;
   q->dst   = dst;
   if (p0)
      memcpy(q->params.p0, p0, sizeof(q->params.p0));
   q->params.p1[0] = sx;
   q->params.p1[1] = sy;
   q->params.p1[2] = 1.0f / (float)p->w[dst];
   q->params.p1[3] = 1.0f / (float)p->h[dst];
}

/* beetle-psx's renderer_hdr_post_apply(), pass for pass */
void hdr_post_plan_make(struct hdr_post_plan *p, unsigned w, unsigned h)
{
   struct hdr_post_frame f;
   int i;

   memset(p, 0, sizeof(*p));
   hdr_post_frame_params(&f, w, h);
   p->band_t = p->band_w = HDR_POST_SCENE;
   memcpy(p->composite.hal_bloom, f.hal_bloom, sizeof(p->composite.hal_bloom));
   p->composite.band_w[0] = f.band_w[0];
   p->composite.band_w[1] = f.band_w[1];
   p->composite.ca        = f.ca;
   if (!f.bloom)
      return;
   {
      const unsigned qw = post_dim(w, 2), qh = post_dim(h, 2);
      if (f.levels)
      {
         static const float radius[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
         p->w[0] = qw;
         p->h[0] = qh;
         for (i = 1; i < f.levels; i++)
         {
            const unsigned pw = post_dim(qw, (unsigned)(i - 1)), ph = post_dim(qh, (unsigned)(i - 1));
            p->w[i] = pw > 1 ? pw / 2 : 1;
            p->h[i] = ph > 1 ? ph / 2 : 1;
         }
         /* the bright pass into the pyramid's base, down it, then back up
          * adding each level into the one above */
         post_pass(p, HDR_BLOOM_BRIGHT, HDR_POST_SCENE, 0, f.bright, 1.0f / (float)w, 1.0f / (float)h);
         for (i = 1; i < f.levels; i++)
            post_pass(p, i == 1 ? HDR_BLOOM_DOWN_KARIS : HDR_BLOOM_DOWN, i - 1, i, NULL,
                  1.0f / (float)post_dim(qw, (unsigned)(i - 1)), 1.0f / (float)post_dim(qh, (unsigned)(i - 1)));
         for (i = f.levels - 1; i > 0; i--)
            post_pass(p, HDR_BLOOM_UP, i, i - 1, radius,
                  1.0f / (float)post_dim(qw, (unsigned)i), 1.0f / (float)post_dim(qh, (unsigned)i));
         p->band_t = p->band_w = 0;
      }
      else
      {
         const unsigned sw = post_dim(w, 4), sh = post_dim(h, 4);
         p->w[0] = p->w[1] = qw;
         p->h[0] = p->h[1] = qh;
         p->w[2] = p->w[3] = sw;
         p->h[2] = p->h[3] = sh;
         /* the bright pass into the tight band, its separable blur, and the
          * wide band at a sixteenth seeded from it */
         post_pass(p, HDR_BLOOM_BRIGHT, HDR_POST_SCENE, 0, f.bright, 1.0f / (float)w, 1.0f / (float)h);
         post_pass(p, HDR_BLOOM_BLUR, 0, 1, NULL, 1.0f / (float)qw, 0.0f);
         post_pass(p, HDR_BLOOM_BLUR, 1, 0, NULL, 0.0f, 1.0f / (float)qh);
         post_pass(p, HDR_BLOOM_BLUR, 0, 3, NULL, 1.0f / (float)sw, 0.0f);
         post_pass(p, HDR_BLOOM_BLUR, 3, 2, NULL, 0.0f, 1.0f / (float)sh);
         p->band_t = 0;
         p->band_w = 2;
      }
   }
}
