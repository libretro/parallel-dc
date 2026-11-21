/* The renderers' contexts as libretro.c has them (lr_contexts.cpp): the
 * frontend's Vulkan, Direct3D 9, 11 and 12 devices handed to the C++ that
 * draws with them. C89, for C and C++ alike. */
#ifndef LR_CONTEXTS_H
#define LR_CONTEXTS_H
#include <boolean.h>
#include <retro_common_api.h>
#include "types.h"

RETRO_BEGIN_DECLS

#ifdef HAVE_VULKAN
/* @render_if: the frontend's struct retro_hw_render_interface_vulkan */
bool lr_vk_init(void *render_if);
void lr_vk_term(void);
void lr_vk_set_window_size(u32 width, u32 height);
/* The size of the image handed to the frontend, 0 for the screen's */
u32 lr_vk_presented_width(void);
u32 lr_vk_presented_height(void);
void lr_vk_save_pipeline_cache(void);
/* The renderer's pipelines made, waited for (never in a frame) */
void lr_vk_wait_pipelines(void);
/* The device negotiation interface for a frontend that has @version of it */
const void *lr_vk_negotiation_interface(unsigned version);
#endif

#ifdef HAVE_D3D11
/* @iface: the frontend's struct retro_hw_render_interface_d3d11 */
bool lr_dx11_init(const void *iface);
void lr_dx11_term(void);
void lr_dx11_present(void);
#endif

#ifdef HAVE_D3D9
bool lr_dx9_init(const void *iface);
void lr_dx9_term(void);
/* false with no frame to hand over */
bool lr_dx9_present(void);
#endif

#ifdef HAVE_D3D12
bool lr_dx12_init(const void *iface);
void lr_dx12_term(void);
void lr_dx12_present(void);
void lr_dx12_wait_pipelines(void);
#endif

RETRO_END_DECLS

#endif
