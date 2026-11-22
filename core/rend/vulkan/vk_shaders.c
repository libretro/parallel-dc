/* The raster renderers' SPIR-V modules (raster_spv.inc) */
#include <string.h>
#include "vk_context.h"
#include "shaders.h"
#define RASTER_SPV_WORDS
#include "raster_spv.inc"

const u32 *RasterSpvWords(int id, size_t *size)
{
	*size = raster_spv_size[id];
	return raster_spv[id];
}

VkShaderModule vk_raster_module(int id)
{
	VkShaderModuleCreateInfo info;
	VkShaderModule module = VK_NULL_HANDLE;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = raster_spv_size[id];
	info.pCode = raster_spv[id];
	if (vkCreateShaderModule(vkc.device, &info, NULL, &module) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return module;
}
