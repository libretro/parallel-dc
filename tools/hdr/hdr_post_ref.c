/* The HDR post effects on the CPU (vk_hdr --post): what the encode's
 * three steps - the picture into scene-linear light, the bloom of the
 * plan's passes, the composite - make of a 24-bit picture, with the
 * images rounded to their storage format after every pass and bilinear
 * taps where the shaders sample. Only the reference knee (the default
 * Display Transform) and scRGB, as the frontends of the tests take them:
 * paper white 200 nits, peak 1000, no panel primaries.
 *
 * The core's options are read the way the core reads them
 * (hdr_libretro_options()) and the passes are the core's plan
 * (hdr_post_plan_make()); the shaders' arithmetic is done here again.
 * The deband's grain (+-1 8-bit step of the picture before the transfer)
 * is taken either way in the scene, and a little slack is left for the
 * GPU's filtering and its float rounding in the bloom. C89. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

#include "../../core/rend/hdr/hdr_output.h"
#include "../../core/rend/hdr/hdr_transform.h"
#include "../../core/rend/hdr/hdr_libretro.h"
#include "hdr_post_ref.h"

static const char *const *ref_keys;
static const char *const *ref_vals;
static int                ref_n;

static bool ref_env(unsigned cmd, void *data)
{
   int i;
   if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE)
   {
      struct retro_variable *v = (struct retro_variable*)data;
      for (i = 0; i < ref_n; i++)
         if (!strcmp(v->key, ref_keys[i]))
         {
            v->value = ref_vals[i];
            return true;
         }
      return false;
   }
   return false;
}

typedef struct
{
   unsigned w, h;
   float   *px;   /* rgb */
} ref_image;

/* A positive float to @mbits of mantissa and a half's exponent range
 * (half: 10; B10G11R11: 6 for red and green, 5 for blue), nearest even */
static float round_to(double v, int mbits, int sign)
{
   int    e;
   double m, q;
   if (!(v == v))
      return 0.0f;
   if (v < 0.0 && !sign)
      return 0.0f;
   if (v == 0.0)
      return 0.0f;
   m = frexp(fabs(v), &e);       /* v = m 2^e, m in [0.5, 1) */
   if (e < -13)                  /* (subnormal: a fixed step of 2^-14 2^-mbits) */
   {
      q = ldexp(1.0, -14 - mbits);
      m = floor(fabs(v) / q + 0.5);
      return (float)((v < 0.0 ? -1.0 : 1.0) * m * q);
   }
   q = ldexp(1.0, mbits + 1);
   m = floor(m * q + 0.5);
   v = (v < 0.0 ? -1.0 : 1.0) * ldexp(m / q, e);
   if (fabs(v) > 65504.0)
      v = v < 0.0 ? -65504.0 : 65504.0;
   return (float)v;
}

static void store(ref_image *im, unsigned i, const double c[3], int compact)
{
   if (compact)
   {
      im->px[i * 3 + 0] = round_to(c[0], 6, 0);
      im->px[i * 3 + 1] = round_to(c[1], 6, 0);
      im->px[i * 3 + 2] = round_to(c[2], 5, 0);
   }
   else
   {
      im->px[i * 3 + 0] = round_to(c[0], 10, 1);
      im->px[i * 3 + 1] = round_to(c[1], 10, 1);
      im->px[i * 3 + 2] = round_to(c[2], 10, 1);
   }
}

/* A linear tap at @u, @v (0 to 1 over the image), clamped to the edge */
static void tap(const ref_image *im, double u, double v, double out[3])
{
   double   x = u * im->w - 0.5, y = v * im->h - 0.5;
   double   fx, fy;
   int      x0, y0, x1, y1, c;
   x0 = (int)floor(x);
   y0 = (int)floor(y);
   fx = x - x0;
   fy = y - y0;
   x1 = x0 + 1;
   y1 = y0 + 1;
#define CL(a, n) ((a) < 0 ? 0 : (a) >= (int)(n) ? (int)(n) - 1 : (a))
   x0 = CL(x0, im->w); x1 = CL(x1, im->w);
   y0 = CL(y0, im->h); y1 = CL(y1, im->h);
#undef CL
   for (c = 0; c < 3; c++)
   {
      const double a = im->px[((size_t)y0 * im->w + x0) * 3 + c];
      const double b = im->px[((size_t)y0 * im->w + x1) * 3 + c];
      const double d = im->px[((size_t)y1 * im->w + x0) * 3 + c];
      const double e = im->px[((size_t)y1 * im->w + x1) * 3 + c];
      out[c] = (a * (1.0 - fx) + b * fx) * (1.0 - fy) + (d * (1.0 - fx) + e * fx) * fy;
   }
}

static void tapo(const ref_image *im, double u, double v, const float st[2],
      double ox, double oy, double out[3])
{
   tap(im, u + st[0] * ox, v + st[1] * oy, out);
}

static double lum(const double c[3])
{
   return c[0] * 0.2126 + c[1] * 0.7152 + c[2] * 0.0722;
}

static void add(double acc[3], const double c[3], double w)
{
   acc[0] += c[0] * w;
   acc[1] += c[1] * w;
   acc[2] += c[2] * w;
}

/* One pass of hdr_bloom.frag at the target texel @x, @y */
static void stage(const struct hdr_post_pass *q, const ref_image *src, unsigned x, unsigned y,
      double out[3])
{
   const float *p0 = q->params.p0, *p1 = q->params.p1;
   const double u = (x + 0.5) * p1[2], v = (y + 0.5) * p1[3];
   double       t[3], acc[3] = { 0.0, 0.0, 0.0 };
   int          i;

   switch (q->stage)
   {
   case HDR_BLOOM_BRIGHT:
      {
         static const double o[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
         double l, sk, f, b[3], bl;
         for (i = 0; i < 4; i++)
         {
            tapo(src, u, v, p1, o[i][0], o[i][1], t);
            add(acc, t, 0.25);
         }
         for (i = 0; i < 3; i++)
            if (acc[i] < 0.0)
               acc[i] = 0.0;
         l  = lum(acc);
         sk = l - p0[0] + p0[1];
         sk = sk < 0.0 ? 0.0 : sk > 2.0 * p0[1] ? 2.0 * p0[1] : sk;
         sk = sk * sk / (4.0 * p0[1]);
         f  = (sk > l - p0[0] ? sk : l - p0[0]) / (l > 1e-5 ? l : 1e-5) / (1.0 - p0[0]);
         for (i = 0; i < 3; i++)
            b[i] = acc[i] * f;
         bl = lum(b);
         for (i = 0; i < 3; i++)
            out[i] = b[i] / (1.0 + bl * p0[2]);
      }
      return;
   case HDR_BLOOM_BLUR:
      {
         static const double o[7] = { 0.0, 1.36268, -1.36268, 3.21159, -3.21159, 5.11234, -5.11234 };
         static const double w[7] = { 0.24458, 0.31802, 0.31802, 0.05717, 0.05717, 0.00251, 0.00251 };
         for (i = 0; i < 7; i++)
         {
            tapo(src, u, v, p1, o[i], o[i], t);
            add(acc, t, w[i]);
         }
      }
      break;
   case HDR_BLOOM_DOWN:
   case HDR_BLOOM_DOWN_KARIS:
      {
         static const double o[13][2] = {
            { -2, 2 }, { 0, 2 }, { 2, 2 }, { -2, 0 }, { 0, 0 }, { 2, 0 }, { -2, -2 }, { 0, -2 }, { 2, -2 },
            { -1, 1 }, { 1, 1 }, { -1, -1 }, { 1, -1 } };
         static const double w[13] = { 0.03125, 0.0625, 0.03125, 0.0625, 0.125, 0.0625, 0.03125, 0.0625,
            0.03125, 0.125, 0.125, 0.125, 0.125 };
         double ws = 0.0;
         for (i = 0; i < 13; i++)
         {
            double k = w[i];
            tapo(src, u, v, p1, o[i][0], o[i][1], t);
            if (q->stage == HDR_BLOOM_DOWN_KARIS)
            {
               double m = t[0] > t[1] ? t[0] : t[1];
               m = m > t[2] ? m : t[2];
               k *= 1.0 / (1.0 + m);
            }
            add(acc, t, k);
            ws += k;
         }
         if (q->stage == HDR_BLOOM_DOWN_KARIS)
            for (i = 0; i < 3; i++)
               acc[i] /= ws;
      }
      break;
   case HDR_BLOOM_UP:
      {
         const double r = p0[0];
         static const double o[9][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 0, 0 }, { 1, 0 },
            { -1, 1 }, { 0, 1 }, { 1, 1 } };
         static const double w[9] = { 1, 2, 1, 2, 4, 2, 1, 2, 1 };
         for (i = 0; i < 9; i++)
         {
            tapo(src, u, v, p1, o[i][0] * r, o[i][1] * r, t);
            add(acc, t, w[i] * 0.0625);
         }
      }
      break;
   }
   out[0] = acc[0];
   out[1] = acc[1];
   out[2] = acc[2];
}

static ref_image image_new(unsigned w, unsigned h)
{
   ref_image im;
   im.w  = w;
   im.h  = h;
   im.px = (float*)calloc((size_t)w * h * 3 + 3, sizeof(float));
   return im;
}

/* hdr.glsl's reference knee, in nits, then scRGB */
static double knee_scrgb(double lin, double omax, double paper, double peak)
{
   const double headroom = peak - paper;
   const double o        = omax > 0.0 ? omax / headroom : 0.0;
   const double k        = o > 0.0 ? 1.0 / (o + 1.0) : 0.0;
   const double over     = lin > paper ? lin - paper : 0.0;
   return ((lin < paper ? lin : paper) + over * k) / 80.0;
}

static void encode(const double c[3], double out[3])
{
   const double paper = 200.0, peak = 1000.0;
   double n[3], omax = 0.0;
   int    i;
   for (i = 0; i < 3; i++)
   {
      n[i] = (c[i] > 0.0 ? c[i] : 0.0) * paper;
      if (n[i] - paper > omax)
         omax = n[i] - paper;
   }
   for (i = 0; i < 3; i++)
      out[i] = knee_scrgb(n[i], omax, paper, peak);
}

void hdr_post_ref_configure(const char *const *keys, const char *const *vals, int n)
{
   ref_keys = keys;
   ref_vals = vals;
   ref_n    = n;
   hdr_libretro_options(ref_env);
}

int hdr_post_ref_check(const unsigned char *sdr, unsigned w, unsigned h,
      const uint16_t *scrgb, double (*half_to_double)(uint16_t),
      const char *const *keys, const char *const *vals, int n)
{
   struct hdr_post_plan plan;
   ref_image            scene[3], img[HDR_POST_IMAGES];
   unsigned             x, y, i;
   int                  k, compact;
   unsigned long        bad = 0, bright = 0;
   double               worst = 0.0, bloom_max = 0.0;

   ref_keys = keys;
   ref_vals = vals;
   ref_n    = n;
   hdr_libretro_options(ref_env);
   if (!hdr_post_enabled())
   {
      fprintf(stderr, "hdr_post: the post effects are off\n");
      return 2;
   }
   compact = hdr_post_compact();
   hdr_post_plan_make(&plan, w, h);

   /* the scene: the picture, and with the grain either way */
   for (k = 0; k < 3; k++)
      scene[k] = image_new(w, h);
   for (i = 0; i < w * h; i++)
      for (k = 0; k < 3; k++)
      {
         double c[3];
         int    j;
         for (j = 0; j < 3; j++)
         {
            double g = (sdr[i * 4 + j] + (k - 1)) / 255.0;
            c[j] = g > 0.0 ? pow(g, 2.4) : 0.0;
         }
         store(&scene[k], i, c, compact);
      }
   for (k = 0; k < HDR_POST_IMAGES; k++)
      img[k] = image_new(plan.w[k] ? plan.w[k] : 1, plan.h[k] ? plan.h[k] : 1);
   /* the bloom, of the picture */
   for (k = 0; k < plan.passes; k++)
   {
      const struct hdr_post_pass *q = &plan.pass[k];
      const ref_image            *src = q->src == HDR_POST_SCENE ? &scene[1] : &img[q->src];
      ref_image                  *dst = &img[q->dst];
      for (y = 0; y < dst->h; y++)
         for (x = 0; x < dst->w; x++)
         {
            double c[3];
            stage(q, src, x, y, c);
            if (q->stage == HDR_BLOOM_UP)
            {
               const size_t o = ((size_t)y * dst->w + x) * 3;
               c[0] += dst->px[o];
               c[1] += dst->px[o + 1];
               c[2] += dst->px[o + 2];
            }
            store(dst, y * dst->w + x, c, compact);
         }
   }

   /* the composite: each output channel within what the scene with the
    * grain either way gives, the bloom of the picture added */
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         const double u = (x + 0.5) / w, v = (y + 0.5) / h;
         double       bl[3] = { 0.0, 0.0, 0.0 }, t[3], lo[3], hi[3];
         const struct hdr_post_params *cp = &plan.composite;
         for (k = 0; k < 3; k++)
         {
            lo[k] = 1e30;
            hi[k] = -1e30;
         }
         if (cp->hal_bloom[3] > 0.0f)
         {
            tap(plan.band_t == HDR_POST_SCENE ? &scene[1] : &img[plan.band_t], u, v, t);
            add(bl, t, cp->band_w[0]);
            if (cp->band_w[1] > 0.0f)
            {
               tap(plan.band_w == HDR_POST_SCENE ? &scene[1] : &img[plan.band_w], u, v, t);
               add(bl, t, cp->band_w[1]);
            }
            for (k = 0; k < 3; k++)
               bl[k] *= cp->hal_bloom[3] * cp->hal_bloom[k];
         }
         for (k = 0; k < 3; k++)
            if (bl[k] > bloom_max)
               bloom_max = bl[k];
         for (i = 0; i < 3; i += 2)
         {
            double lin[3], out[3];
            int    c;
            tap(&scene[i], u, v, lin);
            if (cp->ca != 0.0f)
            {
               const double dx = u - 0.5, dy = v - 0.5, r = (dx * dx + dy * dy) * cp->ca;
               tap(&scene[i], u - dx * r, v - dy * r, t);
               lin[0] = t[0];
               tap(&scene[i], u + dx * r, v + dy * r, t);
               lin[2] = t[2];
            }
            for (c = 0; c < 3; c++)
               lin[c] += bl[c];
            encode(lin, out);
            for (c = 0; c < 3; c++)
            {
               if (out[c] < lo[c])
                  lo[c] = out[c];
               if (out[c] > hi[c])
                  hi[c] = out[c];
            }
         }
         for (k = 0; k < 3; k++)
         {
            /* (the half float the output is; the bloom's taps and floats on the GPU) */
            const double slack = hi[k] * (compact ? 1.0 / 32.0 : 1.0 / 512.0)
                  + bl[k] * 200.0 / 80.0 * (compact ? 0.20 : 0.02) + 1e-4;
            const double got   = half_to_double(scrgb[((size_t)y * w + x) * 4 + k]);
            const double over  = got < lo[k] - slack ? lo[k] - slack - got : got > hi[k] + slack ? got - hi[k] - slack : 0.0;
            if (bl[k] * 200.0 / 80.0 > 0.01)
               bright++;
            if (over > 0.0)
            {
               if (bad < 4)
                  fprintf(stderr, "hdr_post: (%u,%u) channel %d is %g, not in [%g, %g] (bloom %g)\n",
                        x, y, k, got, lo[k] - slack, hi[k] + slack, bl[k] * 200.0 / 80.0);
               bad++;
               if (over > worst)
                  worst = over;
            }
         }
      }
   printf("hdr_post: %ux%u scRGB through the post effects (%d bloom passes, %s), %lu channels outside the CPU's, at most %g off; %lu channels with bloom, most %g\n",
         w, h, plan.passes, compact ? "11/11/10" : "half floats", bad, worst, bright, bloom_max * 200.0 / 80.0);
   for (k = 0; k < 3; k++)
      free(scene[k].px);
   for (k = 0; k < HDR_POST_IMAGES; k++)
      free(img[k].px);
   return bad ? 1 : 0;
}
