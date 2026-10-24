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
 * hdr/hdr_encode.hlsl), 80 bytes. */
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

#ifdef __cplusplus
}
#endif

#endif
