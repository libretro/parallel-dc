/* The cases the HDR output's encode is tried with, the same for every
 * way of drawing it (tools/vulkan/hdr_hlsl.c draws them with the GLSL and
 * the HLSL on Vulkan; tools/hdr/hdr_d3d.c with Direct3D 11 and 12 under
 * the encoders the core uses): the picture, and every Display Transform
 * and setting the encode reads, in one order. C89. */
#ifndef HDR_CASES_H__
#define HDR_CASES_H__

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../core/rend/hdr/hdr_output.h"
#include "../../core/rend/hdr/hdr_transform.h"
#include "../../core/rend/hdr/hdr_display.h"

#define HDR_CASES_W 256
#define HDR_CASES_H 64

/* The picture, RGBA8, HDR_CASES_W x HDR_CASES_H: every 8-bit level of
 * grey, each primary and secondary, and colours mixed from a fixed
 * sequence */
static void hdr_cases_picture(unsigned char *px)
{
   const unsigned W = HDR_CASES_W, H = HDR_CASES_H;
      uint32_t x, y, seed = 12345u;
      for (y = 0; y < H; y++)
         for (x = 0; x < W; x++)
         {
            unsigned char *p = px + ((size_t)y * W + x) * 4;
            unsigned row = y / 8, c;
            p[3] = 255;
            for (c = 0; c < 3; c++)
            {
               if (row == 0)
                  p[c] = (unsigned char)x;
               else if (row < 7)
                  p[c] = (row & (1u << c)) ? (unsigned char)x : 0;
               else
               {
                  seed = seed * 1664525u + 1013904223u;
                  p[c] = (unsigned char)(seed >> 24);
               }
            }
         }
}

/* @deband: the picture is 8 bits a channel; @cube: ACES 2.0 (Full) from
 * its cube (baked in place first), else solved per pixel */
typedef void (*hdr_case_fn)(const char *name, const struct hdr_dt_settings *dt,
      const struct hdr_output *o, int deband, int cube);

static void hdr_cases_each(hdr_case_fn run_case)
{
   static const char *const curves[HDR_DT_COUNT] = {
      "knee", "aces", "hejl", "gt", "hable", "lottes", "drago", "unreal", "filmicalu", "rplain",
      "rext", "expo", "hable2017", "aces2", "tumblin", "ward", "schlick", "devlin", "filmiclog",
      "jodie", "acesfit", "neutral", "agx", "aces2full" };
   /* a P3 panel */
   static const uint16_t p3[8] = { 6800, 3200, 2650, 6900, 1500, 600, 3127, 3290 };
   struct hdr_dt_settings dt;
   struct hdr_output o;
   char name[128];
   int c, v;


   memset(&o, 0, sizeof(o));
   o.paper_white_nits = 200.0f;
   o.max_nits = 1000.0f;
   memset(&dt, 0, sizeof(dt));
   dt.aces2_surround = 1;
   dt.aces2_gamut = 3;
   dt.gt_toe = 0.22f;
   dt.gt_linear = 0.40f;
   dt.filmic_contrast = 3;
   dt.filmic_max_ev = 4.026068811f;

   /* every curve, with each expansion and with hue preservation, to HDR10
    * and to scRGB */
   for (c = 0; c < HDR_DT_COUNT; c++)
      for (v = 0; v < 8; v++)
      {
         dt.curve = c;
         dt.expansion = v % 3;
         dt.hue_preserve = (v >> 2) & 1;
         o.format = (v & 1) ? HDR_OUTPUT_SCRGB : HDR_OUTPUT_HDR10;
         sprintf(name, "%s expansion %d hue %d %s", curves[c], dt.expansion, dt.hue_preserve,
               o.format == HDR_OUTPUT_SCRGB ? "scRGB" : "HDR10");
         run_case(name, &dt, &o, v & 1, 0);
      }
   dt.expansion = 0;
   dt.hue_preserve = 0;
   /* the knee's two shoulders, the three transfers, the four gamuts, a
    * panel's primaries and black, the deband */
   dt.curve = HDR_DT_KNEE;
   for (v = 0; v < 2 * 3 * 4; v++)
   {
      o.shoulder = v & 1;
      o.sdr_eotf = (v >> 1) % 3;
      o.expand_gamut = v / 6;
      o.format = (v & 2) ? HDR_OUTPUT_SCRGB : HDR_OUTPUT_HDR10;
      sprintf(name, "knee shoulder %d eotf %d gamut %d", o.shoulder, o.sdr_eotf, o.expand_gamut);
      run_case(name, &dt, &o, v & 1, 0);
   }
   memcpy(o.panel_xy, p3, sizeof(p3));
   hdr_display_panel_matrix(o.panel_xy, o.to_panel);
   o.black_pq = hdr_display_pq(0.05f);
   o.max_nits = 600.0f;
   o.panel_peak_nits = 650.0f;
   for (v = 0; v < 4; v++)
   {
      o.format = (v & 1) ? HDR_OUTPUT_SCRGB : HDR_OUTPUT_HDR10;
      dt.curve = (v & 2) ? HDR_DT_AGX : HDR_DT_KNEE;
      sprintf(name, "panel P3 black 0.05 %s %s", (v & 2) ? "agx" : "knee", (v & 1) ? "scRGB" : "HDR10");
      run_case(name, &dt, &o, 1, 0);
   }
   /* ACES 2.0 (Full): solved per pixel, and from its cube; each surround
    * and limiting gamut */
   dt.curve = HDR_DT_ACES2FULL;
   for (v = 0; v < 2 * 3 * 4; v++)
   {
      dt.aces2_surround = (v >> 1) % 3;
      dt.aces2_gamut = v / 6;
      o.format = HDR_OUTPUT_HDR10;
      sprintf(name, "aces2full %s surround %d gamut %d", (v & 1) ? "cube" : "solved",
            dt.aces2_surround, dt.aces2_gamut);
      run_case(name, &dt, &o, 0, v & 1);
   }
}

#endif
