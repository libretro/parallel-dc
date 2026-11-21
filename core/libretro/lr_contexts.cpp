/* The renderers' contexts as libretro.c has them: see lr_contexts.h */
#ifdef HAVE_D3D11
#include "rend/dx11/dx11context_lr.h"
#endif
#ifdef HAVE_D3D9
#include "rend/dx9/dx9context_lr.h"
#endif
#ifdef HAVE_D3D12
#include "rend/dx12/dx12context_lr.h"
#endif
#ifdef HAVE_VULKAN
#include "rend/vulkan/vulkan_context.h"
#include "rend/vulkan/pso.h"
#endif
#include "hw/pvr/Renderer_if.h"
#include "rend/gles/ta_decoder_gl.h"
#include "lr_contexts.h"

bool lr_rend_ready(void)
{
   return renderer == NULL || renderer->Ready();
}

bool lr_rend_there(void)
{
   return renderer != NULL;
}

void lr_rend_options_changed(void)
{
   if (renderer != NULL)
      renderer->OptionsChanged();
}

bool lr_gl_ta_context_can_do(void)
{
   return gltad_context_can_do();
}

#ifdef HAVE_VULKAN
static VulkanContext theVulkanContext;

bool lr_vk_init(void *render_if)
{
   return theVulkanContext.Init((retro_hw_render_interface_vulkan *)render_if);
}

void lr_vk_term(void)
{
   theVulkanContext.Term();
}

void lr_vk_set_window_size(u32 width, u32 height)
{
   theVulkanContext.SetWindowSize(width, height);
}

u32 lr_vk_presented_width(void)
{
   return theVulkanContext.PresentedWidth();
}

u32 lr_vk_presented_height(void)
{
   return theVulkanContext.PresentedHeight();
}

void lr_vk_save_pipeline_cache(void)
{
   theVulkanContext.SavePipelineCache();
}

void lr_vk_wait_pipelines(void)
{
   VkPsoWorker::Instance().WaitBatch();
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

#ifdef HAVE_D3D11
bool lr_dx11_init(const void *iface)
{
   return DX11Context::Instance()->Init((const struct retro_hw_render_interface_d3d11 *)iface);
}

void lr_dx11_term(void)
{
   DX11Context::Instance()->Term();
}

void lr_dx11_present(void)
{
   DX11Context::Instance()->Present();
}
#endif

#ifdef HAVE_D3D9
bool lr_dx9_init(const void *iface)
{
   return DX9Context::Instance()->Init((const struct retro_hw_render_interface_d3d9 *)iface);
}

void lr_dx9_term(void)
{
   DX9Context::Instance()->Term();
}

bool lr_dx9_present(void)
{
   return DX9Context::Instance()->Present();
}
#endif

#ifdef HAVE_D3D12
bool lr_dx12_init(const void *iface)
{
   return DX12Context::Instance()->Init((const struct retro_hw_render_interface_d3d12 *)iface);
}

void lr_dx12_term(void)
{
   DX12Context::Instance()->Term();
}

void lr_dx12_present(void)
{
   DX12Context::Instance()->Present();
}

void lr_dx12_wait_pipelines(void)
{
   DX12Context::Instance()->Pipelines().WaitMade();
}
#endif
