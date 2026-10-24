/* The chip's write-out (rend/tile/pvr_scaler.h: the Y filter, then the
 * packing to a framebuffer of 16 bits a pixel and its dither) for both
 * OpenGL renderers, as the core option reicast_pvr2_scaler asks: a
 * compute shader's pass over the picture, OpenGL 4.3 and later only. */
#pragma once
#include "types.h"

/* The pass's program and picture made, or taken down, as the option and
 * the context have it: when the renderer starts and when it changes size
 * (a change of the option is one), never in a frame */
void gl_writeout_init(int width, int height);
void gl_writeout_term();
/* The picture in @fbo, @width x @height, through the write-out of the frame
 * in the registers now, when it has one (ComputeFrame::WriteOut()); @fbo
 * is left bound */
void gl_writeout_run(u32 fbo, int width, int height);
