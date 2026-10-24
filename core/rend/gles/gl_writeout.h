/* The chip's write-out (rend/tile/pvr_scaler.h: the Y filter, then the
 * packing to a framebuffer of 16 bits a pixel and its dither) for both
 * OpenGL renderers, as the core option reicast_pvr2_scaler asks: a
 * compute shader's pass in place over the picture, OpenGL 4.3 and later
 * only. The picture is then a texture of the core's: the per-triangle
 * renderer's own framebuffer (gles.cpp's own_target_bind()), the per-pixel
 * renderer's this one's (gl_writeout_target()); either goes to the
 * frontend's framebuffer in one blit, after the pass. */
#pragma once
#include "types.h"

/* The pass's program made, or taken down, as the option and the context
 * have it; with @picture, the picture of @width x @height too (the
 * per-pixel renderer's). When the renderer starts and when it changes size
 * (a change of the option is one), never in a frame */
void gl_writeout_init(int width, int height, bool picture);
void gl_writeout_term();
// whether the pass is made: the screen's picture is to be a texture
bool gl_writeout_ready();
/* The pass in place over @texture, @width x @height, when the frame in the
 * registers has a write-out (ComputeFrame::WriteOut()) */
void gl_writeout_pass(u32 texture, int width, int height);
/* The per-pixel renderer's: the framebuffer to draw the screen in, its
 * picture (0: none made, the frontend's @frontend instead); the first
 * frame after it is made starts from what @frontend holds */
u32 gl_writeout_target(u32 frontend, int width, int height);
/* ...and once drawn: the pass over it (@pass: a frame the renderer drew,
 * not the framebuffer in video memory), and the picture to @frontend in
 * one blit, which is left bound */
void gl_writeout_present(u32 frontend, int width, int height, bool pass);
