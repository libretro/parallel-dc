/* The renderers' contexts as libretro.c has them: see lr_contexts.h */
#ifdef HAVE_D3D12
#include "rend/dx12/dx12context_lr.h"
#endif
#ifdef HAVE_VULKAN
#include "rend/vulkan/vk_context.h"
#endif
#include "lr_contexts.h"

#ifdef HAVE_VULKAN
bool lr_vk_init(void *render_if)
{
   return vkc_init((struct retro_hw_render_interface_vulkan *)render_if);
}

void lr_vk_term(void)
{
   vkc_term();
}

void lr_vk_set_window_size(u32 width, u32 height)
{
   vkc_set_window_size(width, height);
}

u32 lr_vk_presented_width(void)
{
   return vkc.presentedWidth;
}

u32 lr_vk_presented_height(void)
{
   return vkc.presentedHeight;
}

void lr_vk_save_pipeline_cache(void)
{
   vkc_save_pipeline_cache();
}

void lr_vk_wait_pipelines(void)
{
   vk_pso_worker_wait_batch();
}

/* Version 2 where the frontend has it: the device is then made through
 * the frontend, which can add what it needs (e.g. for its low-latency
 * modes). A frontend that predates v2 rejects any version but 1. */
const void *lr_vk_negotiation_interface(unsigned version)
{
   static const struct retro_hw_render_context_negotiation_interface_vulkan negotiation_interface_v1 = {
      RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
      1,
      VkGetApplicationInfo,
      VkCreateDevice,
      NULL,
   };
   static const struct retro_hw_render_context_negotiation_interface_vulkan negotiation_interface_v2 = {
      RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
      2,
      VkGetApplicationInfo,
      VkCreateDevice,
      NULL,
      NULL,
      VkCreateDevice2,
   };
   return version >= 2 ? &negotiation_interface_v2 : &negotiation_interface_v1;
}
#endif

#ifdef HAVE_D3D12
bool lr_dx12_init(const void *iface)
{
   return dx12c_init((const struct retro_hw_render_interface_d3d12 *)iface);
}

void lr_dx12_term(void)
{
   dx12c_term();
}

void lr_dx12_present(void)
{
   dx12c_present();
}

void lr_dx12_wait_pipelines(void)
{
   dx12_pso_wait_made();
}
#endif
