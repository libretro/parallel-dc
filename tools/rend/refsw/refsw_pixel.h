#pragma once
/*
	This file is part of libswirl
*/
#include "license/bsd"

#include "refsw.h"

/* The pixel operations (libswirl's PixelPipeline): the depth test of
 * @render_mode with @depth_mode, and the shading of a pixel's tag */
void refsw_pixel_isp(refsw* backend, RenderMode render_mode, u32 depth_mode, float x, float y, float invW, u8 *pb, parameter_tag_t tag);
bool refsw_pixel_tsp(const FpuEntry *entry, float x, float y, float W, u8 *rb);
