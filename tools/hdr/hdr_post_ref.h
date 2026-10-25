/* See hdr_post_ref.c. C89. */
#ifndef HDR_POST_REF_H__
#define HDR_POST_REF_H__

#include <stdint.h>

/* The core's options @keys and @vals (@n of them) read as the core reads
 * them (hdr_libretro_options()): the post effects and the transform set */
void hdr_post_ref_configure(const char *const *keys, const char *const *vals, int n);

/* @sdr the 24-bit picture (RGBA8), @scrgb the scRGB one (RGBA16F) of the
 * same frame, @w x @h; @keys and @vals the core's options. 0 when every
 * channel is what the post effects make of the 24-bit picture, 1 when one
 * is not (each told on stderr, the first few), 2 when they are off. */
int hdr_post_ref_check(const unsigned char *sdr, unsigned w, unsigned h,
      const uint16_t *scrgb, double (*half_to_double)(uint16_t),
      const char *const *keys, const char *const *vals, int n);

#endif
