/* The HDR output's side of the libretro API: the Color Format and
 * Display Transform options, the pixel format asked of the frontend, and
 * the frontend's metadata once a frame. C89. */
#ifndef HDR_LIBRETRO_H__
#define HDR_LIBRETRO_H__

#include <boolean.h>
#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The options read again: the Display Transform takes effect on the next
 * frame, Color Format at the next load. */
void hdr_libretro_options(retro_environment_t env);

/* The HDR options shown only with an HDR Color Format, the curves' own
 * only with their curve. True when any changed. */
bool hdr_libretro_update_display(retro_environment_t env);

/* Whether Color Format asks for HDR. */
bool hdr_libretro_wanted(void);

/* Back to the 24-bit output (hdr_output.format HDR_OUTPUT_NONE), the
 * options kept. */
void hdr_libretro_reset(void);

/* The pixel format Color Format asks for, asked of the frontend once a
 * renderer that has the HDR output is chosen; hdr_output says what it
 * took. False for the 24-bit output, the frontend's format XRGB8888. */
bool hdr_libretro_negotiate(retro_environment_t env);

/* The frontend's paper white, peak, black and panel, once a frame. */
void hdr_libretro_refresh(retro_environment_t env);

#ifdef __cplusplus
}
#endif

#endif
