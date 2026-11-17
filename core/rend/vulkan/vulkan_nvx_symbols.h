/* The VK_NVX_device_generated_commands entry points that vulkan.hpp's
 * static dispatcher names in the Vulkan headers this core carries.
 * Khronos withdrew the extension, so the symbol wrapper in
 * libretro-common, which goes by current headers, has no place for them:
 * they are declared here, and stay NULL - no driver offers them and the
 * core never calls them. */
#ifndef VULKAN_NVX_SYMBOLS_H
#define VULKAN_NVX_SYMBOLS_H

#include <vulkan/vulkan_symbol_wrapper.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(VK_NVX_device_generated_commands) && defined(VK_NVX_DEVICE_GENERATED_COMMANDS_SPEC_VERSION) \
   && (VK_NVX_DEVICE_GENERATED_COMMANDS_SPEC_VERSION < 4)
extern PFN_vkCmdProcessCommandsNVX parallel_dc_vkCmdProcessCommandsNVX;
#define vkCmdProcessCommandsNVX parallel_dc_vkCmdProcessCommandsNVX
extern PFN_vkCmdReserveSpaceForCommandsNVX parallel_dc_vkCmdReserveSpaceForCommandsNVX;
#define vkCmdReserveSpaceForCommandsNVX parallel_dc_vkCmdReserveSpaceForCommandsNVX
extern PFN_vkCreateIndirectCommandsLayoutNVX parallel_dc_vkCreateIndirectCommandsLayoutNVX;
#define vkCreateIndirectCommandsLayoutNVX parallel_dc_vkCreateIndirectCommandsLayoutNVX
extern PFN_vkDestroyIndirectCommandsLayoutNVX parallel_dc_vkDestroyIndirectCommandsLayoutNVX;
#define vkDestroyIndirectCommandsLayoutNVX parallel_dc_vkDestroyIndirectCommandsLayoutNVX
extern PFN_vkCreateObjectTableNVX parallel_dc_vkCreateObjectTableNVX;
#define vkCreateObjectTableNVX parallel_dc_vkCreateObjectTableNVX
extern PFN_vkDestroyObjectTableNVX parallel_dc_vkDestroyObjectTableNVX;
#define vkDestroyObjectTableNVX parallel_dc_vkDestroyObjectTableNVX
extern PFN_vkRegisterObjectsNVX parallel_dc_vkRegisterObjectsNVX;
#define vkRegisterObjectsNVX parallel_dc_vkRegisterObjectsNVX
extern PFN_vkUnregisterObjectsNVX parallel_dc_vkUnregisterObjectsNVX;
#define vkUnregisterObjectsNVX parallel_dc_vkUnregisterObjectsNVX
extern PFN_vkGetPhysicalDeviceGeneratedCommandsPropertiesNVX parallel_dc_vkGetPhysicalDeviceGeneratedCommandsPropertiesNVX;
#define vkGetPhysicalDeviceGeneratedCommandsPropertiesNVX parallel_dc_vkGetPhysicalDeviceGeneratedCommandsPropertiesNVX
#endif

#ifdef __cplusplus
}
#endif

#endif
