/* rend/tile/pvr_scaler.h as text, after pvr_resolve_text.inc, for the write-out pass: made by tools/rend/ssaa_rules_test.sh update from it. Not to be edited. */
R"PVR_TEXT(
/* The PowerVR2's X and Y scalers, the arithmetic of the chip's tile
 * write-out to video memory (SCALER_CTL, Y_COEFF; the Dreamcast System
 * Architecture manual, 3.4.11): one text for the reference software
 * renderer (tools/rend/refsw), the CPU's reference of the compute renderer
 * (tools/rend/tile_ref.c) and its kernels, read the three ways
 * pvr_lane.h says.
 *
 * The scalers work on the accumulation buffer's 8-bit ARGB, X first then
 * Y, then the framebuffer format's packing and dither (pvr_fb_write()).
 * What the manual leaves unsaid - the rounding - is taken here, in one
 * place each, and is not confirmed on the hardware.
 *
 * A colour is a word, red in its low byte, then green, blue and alpha. */
#ifndef PVR_SCALER_H
#define PVR_SCALER_H


/* The X scaler (SCALER_CTL bit 16): two adjacent pixels, each weighted
 * 0.5, made one. Rounded half up, as the resolve's box rounds: so that the
 * pair is one more step of the same box filter (the compute renderer
 * resolves a pair's samples together) */
PVR_FN pvr_u32 pvr_scaler_x(pvr_u32 a, pvr_u32 b)
{
   return pvr_resolve_join(pvr_resolve_rb(a) + pvr_resolve_rb(b), pvr_resolve_ga(a) + pvr_resolve_ga(b), 1u);
}

/* 1 when the Y scaler filters: it does only when the vertical scale
 * factor (SCALER_CTL 15:0, 6.10 fixed point, the picture scaled by its
 * inverse) reduces, as 0x401 does, the factor the manual gives for
 * filtering at the drawn resolution (its flicker-free type A) */
PVR_FN pvr_u32 pvr_scaler_yfilter(pvr_u32 scaler_ctl)
{
   return (scaler_ctl & 0xffffu) > 0x400u ? 1u : 0u;
}

/* The Y scaler's filter: a line from its three-line buffer, each channel
 * the line itself times coefficient 1 (Y_COEFF 15:8) plus the lines above
 * and below times coefficient 0/2 (Y_COEFF 7:0), each coefficient n/256
 * (the manual's example: 0x80 is 0.5, 0x40 0.25). Rounded to nearest, and
 * held at 255 where coefficients summing over 256 would go past it */
PVR_FN pvr_u32 pvr_scaler_y(pvr_u32 above, pvr_u32 mid, pvr_u32 below, pvr_u32 ycoeff)
{
   pvr_u32 c0 = ycoeff & 255u, c1 = (ycoeff >> 8) & 255u, y = 0u, s, v;
   for (s = 0u; s < 32u; s += 8u)
   {
      v = (c0 * (((above >> s) & 255u) + ((below >> s) & 255u)) + c1 * ((mid >> s) & 255u) + 128u) >> 8;
      y |= (v > 255u ? 255u : v) << s;
   }
   return y;
}

/* The write-out's packing to a framebuffer of 16 bits a pixel and its
 * dither (FB_W_CTRL), and the picture as the video output reads it back
 * (FB_R_CTRL's fb_concat in the low bits), in one word: pvr_fb_word() */
#define PVR_FB_555 1u
#define PVR_FB_565 2u
PVR_FN pvr_u32 pvr_fb_word(pvr_u32 packmode, pvr_u32 dither, pvr_u32 concat)
{
   const pvr_u32 f = packmode == 1u ? PVR_FB_565 : packmode == 0u || packmode == 3u ? PVR_FB_555 : 0u;
   return f == 0u ? 0u : f | (dither != 0u ? 4u : 0u) | ((concat & 7u) << 3);
}

PVR_FN pvr_u32 pvr_fb_channel(pvr_u32 v, pvr_u32 add, pvr_u32 bits, pvr_u32 low)
{
   v += add;
   if (v > 255u)
      v = 255u;
   return ((v >> (8u - bits)) << (8u - bits)) | low;
}

/* A pixel of the picture at the chip's pixel @x, @y through the packing
 * of @fbw: with the dither, a number from the same four by four pattern
 * as rend/rtt_watch.cpp's, from the frame's first pixel, added before the
 * low bits are dropped (a half of it where three are, a quarter where two
 * are); the colour stops at 255. Alpha is left as it is (the picture
 * shown is opaque) */
PVR_FN pvr_u32 pvr_fb_write(pvr_u32 c, pvr_u32 x, pvr_u32 y, pvr_u32 fbw)
{
   const pvr_u32 i = ((y & 3u) << 2) | (x & 3u);
   const pvr_u32 d = (fbw & 4u) != 0u ? ((i < 8u ? 0x3b19f7d5u : 0x082ac4e6u) >> ((i & 7u) * 4u)) & 15u : 0u;
   const pvr_u32 low = (fbw >> 3) & 7u;
   const pvr_u32 g6 = (fbw & 3u) == PVR_FB_565 ? 1u : 0u;
   if ((fbw & 3u) == 0u)
      return c;
   return pvr_fb_channel(c & 255u, d >> 1, 5u, low)
         | (pvr_fb_channel((c >> 8) & 255u, g6 != 0u ? d >> 2 : d >> 1, 5u + g6, g6 != 0u ? low & 3u : low) << 8)
         | (pvr_fb_channel((c >> 16) & 255u, d >> 1, 5u, low) << 16)
         | (c & 0xff000000u);
}

#endif
)PVR_TEXT"
