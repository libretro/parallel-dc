/* The HDR output the frontend took (libretro.cpp, Color Format), and what
 * the renderers' encodes need of the display for it. Written and read on
 * the thread that renders - the libretro thread, threaded rendering or
 * not - once a frame, before the frame is drawn. C89. */
#ifndef HDR_OUTPUT_H__
#define HDR_OUTPUT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum hdr_output_format
{
   HDR_OUTPUT_NONE = 0, /* 24-bit: the picture as it always was */
   HDR_OUTPUT_HDR10,    /* RETRO_PIXEL_FORMAT_HDR10_2101010: PQ Rec.2020 */
   HDR_OUTPUT_SCRGB     /* RETRO_PIXEL_FORMAT_SCRGB_RGBA16F: linear Rec.709, 1.0 = 80 nits */
};

struct hdr_output
{
   int      format;           /* enum hdr_output_format */
   float    paper_white_nits;
   float    max_nits;         /* the peak the curves carry highlights to */
   float    panel_peak_nits;  /* the panel's own, 0 when unknown */
   float    black_pq;         /* PQ of the panel's black, 0 when unknown */
   int      expand_gamut;     /* the frontend's Colour Boost */
   int      shoulder;         /* the knees' roll-off: 0 Reinhard, 1 exponential */
   int      sdr_eotf;         /* the picture's transfer: 0 2.4, 1 2.2, 2 sRGB */
   float    to_panel[9];      /* linear Rec.2020 -> panel RGB, column-major; zero when unknown */
   uint16_t panel_xy[8];      /* the panel's primaries, CIE xy x 10000; zero when unknown */
};

extern struct hdr_output hdr_output;

/* The encodes' push constants / root constants: what the shaders'
 * parameter block holds (vulkan/shaders/hdr_encode.frag,
 * hdr/hdr_encode.hlsl), 96 bytes. */
struct hdr_encode_params
{
   float   to_panel[12];     /* three columns, each padded to four */
   float   paper_white;
   float   peak;
   float   black_pq;
   int32_t expand_gamut;
   int32_t shoulder;
   int32_t sdr_eotf;
   int32_t scrgb;
   int32_t deband;           /* the picture is 8 bits a channel */
   int32_t ssaa_log2;        /* Direct3D 12: SSAA's samples the picture was resolved from, log2 (0: none) */
   int32_t pad[3];           /* (a constant buffer is whole 16 bytes) */
};

void hdr_encode_params_fill(struct hdr_encode_params *p,
      const struct hdr_output *o, int deband);

/* ---- the post effects: bloom, halation, chromatic aberration ----
 *
 * With them on (hdr_post_enabled()) the encode is three steps, the same on
 * every backend: the picture into scene-linear light (1.0 = paper white)
 * in a half-float image of its size (hdr_encode.frag's LINEAR_OUT), the
 * bloom built from that by the passes of a struct hdr_post_plan, and the
 * composite (hdr_post.frag), which adds the bloom and encodes as the encode
 * would have. Off, none of it runs. */

/* What the encodes' channel sums are of (SSAA's resolve, and the X
 * scaler's pairs): whatever the samples a pixel that were resolved, N of
 * them, a sum is (2^HDR_SUMS_LOG2 / N) times the samples' */
#define HDR_SUMS_LOG2    4
#define HDR_SUMS_SAMPLES (1 << HDR_SUMS_LOG2)

/* The composite's, after struct hdr_encode_params in the same block: 32 bytes */
struct hdr_post_params
{
   float hal_bloom[4];       /* the halation tint, the bloom amount (0: none) */
   float band_w[2];          /* the tight band's weight, the wide one's */
   float ca;                 /* chromatic aberration, 0 off */
   float pad;
};

/* A bloom pass's (hdr_bloom.frag): 32 bytes */
struct hdr_bloom_params
{
   float p0[4];
   float p1[4];              /* xy the step in the source, zw the target's texel */
};

enum hdr_bloom_stage
{
   HDR_BLOOM_BRIGHT = 0,
   HDR_BLOOM_BLUR,
   HDR_BLOOM_DOWN,
   HDR_BLOOM_DOWN_KARIS,
   HDR_BLOOM_UP,             /* added into what the target holds */
   HDR_BLOOM_STAGES
};

/* The bloom's images: the two bands' pairs, or the pyramid's levels */
#define HDR_POST_IMAGES 6
#define HDR_POST_PASSES 11
/* (an image index for the scene image) */
#define HDR_POST_SCENE  (-1)

struct hdr_post_pass
{
   int      stage;           /* enum hdr_bloom_stage */
   int      src;             /* an image, or HDR_POST_SCENE */
   int      dst;
   struct hdr_bloom_params params;
};

/* A frame's post effects for a picture of @w x @h: what the bloom's images
 * are (0 x 0: not used), the passes in order, which images the composite
 * reads as its bands, and its parameters */
struct hdr_post_plan
{
   unsigned w[HDR_POST_IMAGES];
   unsigned h[HDR_POST_IMAGES];
   int      passes;
   struct hdr_post_pass pass[HDR_POST_PASSES];
   int      band_t;          /* an image, or HDR_POST_SCENE (no bloom: unread) */
   int      band_w;
   struct hdr_post_params composite;
};

void hdr_post_plan_make(struct hdr_post_plan *p, unsigned w, unsigned h);

#ifdef __cplusplus
}
#endif

#endif
