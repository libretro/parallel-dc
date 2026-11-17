/* Rendering Method (the core option): the screen's frames drawn by the
 * rasterizer, or by the compute renderer where the renderer has one
 * (Vulkan: rend/vulkan/compute_drawer.h). Set by the options, read when a
 * renderer is set up. */
#pragma once
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

extern bool rend_compute_wanted;

RETRO_END_DECLS
