#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
/*
    Created on: Oct 2, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <stdlib.h>
#include <string.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include "vk_context.h"
#include "shaders.h"
#include "rend/hdr/hdr_output.h"
#include "rend/ssaa_pattern.h"
#include "rend/TexCache.h"
#include "paths.h"

extern int screen_width, screen_height;

vk_context vkc;

static const char *PipelineCacheFileName = "vulkan_pipeline.cache";

static const VkMemoryPropertyFlags HOST_MEMORY =
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

const VkApplicationInfo *VkGetApplicationInfo(void)
{
	/* apiVersion is the maximum vulkan version supported by the app */
	static const VkApplicationInfo applicationInfo = {
		VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "Flycast", 1, "Flycast", 1, VK_API_VERSION_1_1
	};
	return &applicationInfo;
}

static bool surface_support(VkPhysicalDevice gpu, u32 family, VkSurfaceKHR surface)
{
	VkBool32 supported = VK_FALSE;
	if (vkGetPhysicalDeviceSurfaceSupportKHR(gpu, family, surface, &supported) != VK_SUCCESS)
		return false;
	return supported != VK_FALSE;
}

/* the first discrete GPU, or the first there is */
static VkPhysicalDevice choose_gpu(VkInstance instance)
{
	VkPhysicalDevice chosen = VK_NULL_HANDLE;
	VkPhysicalDevice *devices;
	uint32_t i, count = 0;
	if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || count == 0)
		return VK_NULL_HANDLE;
	devices = (VkPhysicalDevice *)malloc(count * sizeof(*devices));
	if (devices == NULL)
		return VK_NULL_HANDLE;
	if (vkEnumeratePhysicalDevices(instance, &count, devices) >= 0 && count != 0)
	{
		for (i = 0; i < count; i++)
		{
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(devices[i], &props);
			if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			{
				chosen = devices[i];
				break;
			}
		}
		if (chosen == VK_NULL_HANDLE)
			chosen = devices[0];
	}
	free(devices);
	return chosen;
}

#define GRAPHICS_COMPUTE (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)

static bool create_device(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
		VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
		const char **required_device_extensions, unsigned num_required_device_extensions,
		const char **required_device_layers, unsigned num_required_device_layers,
		const VkPhysicalDeviceFeatures *required_features,
		retro_vulkan_create_device_wrapper_t create_device_wrapper, void *opaque)
{
	VkQueueFamilyProperties *families;
	VkExtensionProperties *properties = NULL;
	const char **deviceExtensions;
	uint32_t familyCount = 0, propertyCount = 0, i;
	unsigned extensionCount = 0;
	VkPhysicalDeviceFeatures supportedFeatures, features;
	VkPhysicalDeviceProperties deviceProperties;
	bool fragmentStoresAndAtomics;
	bool getMemReq2Supported = false, hasEds = false, hasEds3 = false, hasCacheControl = false;
	bool hasDescriptorIndexing = false;
	vk_pso_eds_features eds;
	vk_pso_eds3_features eds3;
	vk_pso_cache_control_features cacheControl;
	VkPhysicalDeviceDescriptorIndexingFeaturesEXT descriptorIndexing;
	void *featuresChain = NULL;
	float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queues[2];
	VkDeviceCreateInfo deviceCreateInfo;
	VkDevice device = VK_NULL_HANDLE;

	vulkan_symbol_wrapper_init(get_instance_proc_addr);
	vulkan_symbol_wrapper_load_global_symbols();
	vulkan_symbol_wrapper_load_core_symbols(instance);
	/* Vulkan 1.1 and the extensions the core uses: those the driver has,
	 * from the instance; the device's once it exists */
	vulkan_symbol_wrapper_load_extended_symbols(instance, VK_NULL_HANDLE);
	VULKAN_SYMBOL_WRAPPER_LOAD_INSTANCE_EXTENSION_SYMBOL(instance, vkGetPhysicalDeviceSurfaceSupportKHR);

	if (gpu == VK_NULL_HANDLE)
		gpu = choose_gpu(instance);
	if (gpu == VK_NULL_HANDLE)
	{
		ERROR_LOG(RENDERER, "No Vulkan device\n");
		return false;
	}
	context->gpu = gpu;

	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, NULL);
	families = (VkQueueFamilyProperties *)malloc((familyCount + 1) * sizeof(*families));
	if (families == NULL)
		return false;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families);

	/* the first family that supports graphics and compute */
	for (i = 0; i < familyCount; i++)
		if ((families[i].queueFlags & GRAPHICS_COMPUTE) == GRAPHICS_COMPUTE)
			break;
	context->queue_family_index = i;
	verify(context->queue_family_index < familyCount);

	if (surface != VK_NULL_HANDLE)
	{
		/* a queue family index that supports present: first whether the
		 * queue_family_index is good enough */
		context->presentation_queue_family_index = context->queue_family_index < familyCount
				&& surface_support(gpu, context->queue_family_index, surface) ? context->queue_family_index : familyCount;
		if (context->presentation_queue_family_index == familyCount)
		{
			/* it does not support present: another that supports graphics,
			 * compute and present */
			for (i = 0; i < familyCount; i++)
			{
				if ((families[i].queueFlags & GRAPHICS_COMPUTE) == GRAPHICS_COMPUTE && surface_support(gpu, i, surface))
				{
					context->queue_family_index = i;
					context->presentation_queue_family_index = i;
					break;
				}
			}
			if (context->presentation_queue_family_index == familyCount)
			{
				/* none supports all three: another that supports present */
				DEBUG_LOG(RENDERER, "Using separate Graphics and Present queue families\n");
				for (i = 0; i < familyCount; i++)
				{
					if (surface_support(gpu, i, surface))
					{
						context->presentation_queue_family_index = i;
						break;
					}
				}
			}
		}
		if (context->queue_family_index == familyCount || context->presentation_queue_family_index == familyCount)
		{
			ERROR_LOG(RENDERER, "Could not find a queue for graphics or present\n");
			free(families);
			return false;
		}
		if (context->queue_family_index == context->presentation_queue_family_index)
			DEBUG_LOG(RENDERER, "Using Graphics+Present queue family\n");
		else
			DEBUG_LOG(RENDERER, "Using distinct Graphics and Present queue families\n");
	}
	free(families);

	vkGetPhysicalDeviceFeatures(gpu, &supportedFeatures);
	fragmentStoresAndAtomics = supportedFeatures.fragmentStoresAndAtomics != VK_FALSE;
	vkc.samplerAnisotropy = supportedFeatures.samplerAnisotropy != VK_FALSE;
	vkc.storageImageExtendedFormats = supportedFeatures.shaderStorageImageExtendedFormats != VK_FALSE;
	vkc.multiDrawIndirect = supportedFeatures.multiDrawIndirect != VK_FALSE;
	vkc.drawIndirectFirstInstance = supportedFeatures.drawIndirectFirstInstance != VK_FALSE;
	vkc.drawIndirectCount = false;
	vkc.postDepthCoverage = false;
	/* (SSAA's shading per sample: see vkc_ssaa_samples()) */
	vkc.sampleRateShading = supportedFeatures.sampleRateShading != VK_FALSE;
	/* (the compute renderer's edge arithmetic, and its array of textures: vulkan/compute_drawer.h) */
	vkc.shaderInt64 = supportedFeatures.shaderInt64 != VK_FALSE;
	vkc.sampledImageArrayIndexing = supportedFeatures.shaderSampledImageArrayDynamicIndexing != VK_FALSE;

	/* VK_KHR_dedicated_allocation if available */
	vkc.dedicatedAllocationSupported = false;
	vkEnumerateDeviceExtensionProperties(gpu, NULL, &propertyCount, NULL);
	if (propertyCount != 0)
	{
		properties = (VkExtensionProperties *)malloc(propertyCount * sizeof(*properties));
		if (properties == NULL || vkEnumerateDeviceExtensionProperties(gpu, NULL, &propertyCount, properties) < 0)
			propertyCount = 0;
	}
	/* the swapchain, the frontend's, those found below (8 at most) */
	deviceExtensions = (const char **)malloc((1 + num_required_device_extensions + 8) * sizeof(*deviceExtensions));
	if (deviceExtensions == NULL)
	{
		free(properties);
		return false;
	}
	deviceExtensions[extensionCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	for (i = 0; i < num_required_device_extensions; i++)
		deviceExtensions[extensionCount++] = required_device_extensions[i];
	for (i = 0; i < propertyCount; i++)
	{
		const char *name = properties[i].extensionName;
		if (!strcmp(name, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
		{
			deviceExtensions[extensionCount++] = VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME;
			getMemReq2Supported = true;
		}
		else if (!strcmp(name, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME))
		{
			deviceExtensions[extensionCount++] = VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME;
			vkc.dedicatedAllocationSupported = true;
		}
		else if (!strcmp(name, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME))
		{
			/* (GPU-side TA processing's translucent draws: see TaDecoder) */
			deviceExtensions[extensionCount++] = VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME;
			vkc.drawIndirectCount = true;
		}
		else if (!strcmp(name, VK_EXT_POST_DEPTH_COVERAGE_EXTENSION_NAME))
		{
			/* (SSAA: the per-pixel renderer's lists take the samples the depth test passed) */
			deviceExtensions[extensionCount++] = VK_EXT_POST_DEPTH_COVERAGE_EXTENSION_NAME;
			vkc.postDepthCoverage = true;
		}
		else if (!strcmp(name, "VK_EXT_extended_dynamic_state"))
			hasEds = true;
		else if (!strcmp(name, "VK_EXT_extended_dynamic_state3"))
			hasEds3 = true;
		else if (!strcmp(name, "VK_EXT_pipeline_creation_cache_control"))
			hasCacheControl = true;
		else if (!strcmp(name, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
			hasDescriptorIndexing = true;
	}
	free(properties);
	vkc.dedicatedAllocationSupported = vkc.dedicatedAllocationSupported && getMemReq2Supported;

	/* What the general pipelines are drawn with (pso.h): the depth,
	 * stencil, cull and topology states, and the blend ones, set with each
	 * draw; and pipeline caches the core keeps to one thread each, which
	 * the driver then takes no lock for. Asked of the device's features
	 * first: an extension can be there with the feature off. */
	memset(&eds, 0, sizeof(eds));
	memset(&eds3, 0, sizeof(eds3));
	memset(&cacheControl, 0, sizeof(cacheControl));
	eds.sType = VK_PSO_STRUCTURE_TYPE_EDS_FEATURES;
	eds3.sType = VK_PSO_STRUCTURE_TYPE_EDS3_FEATURES;
	cacheControl.sType = VK_PSO_STRUCTURE_TYPE_CACHE_CONTROL_FEATURES;
	vkc.extendedDynamicState = false;
	vkc.extendedDynamicState3 = false;
	vkc.pipelineCacheControl = false;
	/* The compute renderer's array of textures indexed by a slot that
	 * differs between the lanes of a subgroup (vulkan/compute_drawer.h) */
	memset(&descriptorIndexing, 0, sizeof(descriptorIndexing));
	descriptorIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
	vkc.nonUniformTextureIndexing = false;
	vkGetPhysicalDeviceProperties(gpu, &deviceProperties);
	if ((hasEds || hasEds3 || hasCacheControl || hasDescriptorIndexing) && vkGetPhysicalDeviceFeatures2 != NULL
			&& VK_VERSION_MINOR(deviceProperties.apiVersion) >= 1)
	{
		VkPhysicalDeviceFeatures2 features2;
		void **link;
		bool useNonUniform, useEds, useEds3, useCacheControl;
		void *chain = NULL;
		eds.pNext = &eds3;
		eds3.pNext = &cacheControl;
		memset(&features2, 0, sizeof(features2));
		features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features2.pNext = &eds;
		/* (structures of extensions the device does not have are not to be in the chain) */
		link = &features2.pNext;
		if (!hasEds)
			*link = eds.pNext;
		else
			link = &eds.pNext;
		if (!hasEds3)
			*link = eds3.pNext;
		else
			link = &eds3.pNext;
		if (!hasCacheControl)
			*link = cacheControl.pNext;
		if (hasDescriptorIndexing)
		{
			descriptorIndexing.pNext = features2.pNext;
			features2.pNext = &descriptorIndexing;
		}
		vkGetPhysicalDeviceFeatures2(gpu, &features2);
		useNonUniform = hasDescriptorIndexing && descriptorIndexing.shaderSampledImageArrayNonUniformIndexing;
		useEds = hasEds && eds.extendedDynamicState;
		useEds3 = hasEds3 && eds3.colorBlendEnable && eds3.colorBlendEquation && eds3.colorWriteMask;
		useCacheControl = hasCacheControl && cacheControl.pipelineCreationCacheControl;
		/* the chain again, of what is enabled, with nothing else on */
		memset(&eds3, 0, sizeof(eds3));
		eds3.sType = VK_PSO_STRUCTURE_TYPE_EDS3_FEATURES;
		if (useNonUniform)
		{
			memset(&descriptorIndexing, 0, sizeof(descriptorIndexing));
			descriptorIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
			descriptorIndexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
			descriptorIndexing.pNext = chain;
			chain = &descriptorIndexing;
			deviceExtensions[extensionCount++] = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
		}
		if (useCacheControl)
		{
			cacheControl.pNext = chain;
			chain = &cacheControl;
			deviceExtensions[extensionCount++] = "VK_EXT_pipeline_creation_cache_control";
		}
		if (useEds3)
		{
			eds3.colorBlendEnable = eds3.colorBlendEquation = eds3.colorWriteMask = VK_TRUE;
			eds3.pNext = chain;
			chain = &eds3;
			deviceExtensions[extensionCount++] = "VK_EXT_extended_dynamic_state3";
		}
		if (useEds)
		{
			eds.pNext = chain;
			chain = &eds;
			deviceExtensions[extensionCount++] = "VK_EXT_extended_dynamic_state";
		}
		featuresChain = chain;
		vkc.extendedDynamicState = useEds;
		vkc.extendedDynamicState3 = useEds3;
		vkc.pipelineCacheControl = useCacheControl;
		vkc.nonUniformTextureIndexing = useNonUniform;
	}
	/* ...and where it cannot be, its lanes taking their textures in turns:
	 * Vulkan 1.1's subgroup ballot, in compute shaders */
	vkc.subgroupBallotCompute = false;
	if (VK_VERSION_MINOR(deviceProperties.apiVersion) >= 1 && vkGetPhysicalDeviceProperties2 != NULL)
	{
		static VkPhysicalDeviceProperties2 properties2;
		VkPhysicalDeviceSubgroupProperties subgroup;
		memset(&subgroup, 0, sizeof(subgroup));
		subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
		memset(&properties2, 0, sizeof(properties2));
		properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		properties2.pNext = &subgroup;
		vkGetPhysicalDeviceProperties2(gpu, &properties2);
		properties2.pNext = NULL;
		vkc.subgroupBallotCompute = (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0
				&& (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0;
	}

	/* the device */
	memset(queues, 0, sizeof(queues));
	queues[0].sType = queues[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queues[0].queueFamilyIndex = context->queue_family_index;
	queues[1].queueFamilyIndex = context->presentation_queue_family_index;
	queues[0].queueCount = queues[1].queueCount = 1;
	queues[0].pQueuePriorities = queues[1].pQueuePriorities = &queuePriority;
	if (required_features != NULL)
		features = *required_features;
	else
		memset(&features, 0, sizeof(features));
	if (fragmentStoresAndAtomics)
		features.fragmentStoresAndAtomics = VK_TRUE;
	if (vkc.samplerAnisotropy)
		features.samplerAnisotropy = VK_TRUE;
	if (vkc.storageImageExtendedFormats)
		features.shaderStorageImageExtendedFormats = VK_TRUE;
	if (vkc.multiDrawIndirect)
		features.multiDrawIndirect = VK_TRUE;
	if (vkc.drawIndirectFirstInstance)
		features.drawIndirectFirstInstance = VK_TRUE;
	if (vkc.sampleRateShading)
		features.sampleRateShading = VK_TRUE;
	if (vkc.shaderInt64)
		features.shaderInt64 = VK_TRUE;
	if (vkc.sampledImageArrayIndexing)
		features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
	memset(&deviceCreateInfo, 0, sizeof(deviceCreateInfo));
	deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceCreateInfo.pNext = featuresChain;
	deviceCreateInfo.queueCreateInfoCount = context->queue_family_index == context->presentation_queue_family_index ? 1 : 2;
	deviceCreateInfo.pQueueCreateInfos = queues;
	deviceCreateInfo.enabledLayerCount = num_required_device_layers;
	deviceCreateInfo.ppEnabledLayerNames = required_device_layers;
	deviceCreateInfo.enabledExtensionCount = extensionCount;
	deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;
	deviceCreateInfo.pEnabledFeatures = &features;
	if (create_device_wrapper != NULL)
	{
		/* the frontend's vkCreateDevice, which adds what it needs */
		device = create_device_wrapper(gpu, opaque, &deviceCreateInfo);
		if (device == VK_NULL_HANDLE)
			ERROR_LOG(RENDERER, "The frontend could not create the Vulkan device\n");
	}
	else if (vkCreateDevice(gpu, &deviceCreateInfo, NULL, &device) != VK_SUCCESS)
	{
		device = VK_NULL_HANDLE;
		ERROR_LOG(RENDERER, "The Vulkan device could not be created\n");
	}
	free((void *)deviceExtensions);
	if (device == VK_NULL_HANDLE)
		return false;
	context->device = device;
	vulkan_symbol_wrapper_load_core_device_symbols(context->device);
	vulkan_symbol_wrapper_load_extended_symbols(instance, context->device);

	/* the queues */
	vkGetDeviceQueue(device, context->queue_family_index, 0, &context->queue);
	vkGetDeviceQueue(device, context->presentation_queue_family_index, 0, &context->presentation_queue);
	return true;
}

bool VkCreateDevice(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
		VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
		const char **required_device_extensions, unsigned num_required_device_extensions,
		const char **required_device_layers, unsigned num_required_device_layers,
		const VkPhysicalDeviceFeatures *required_features)
{
	return create_device(context, instance, gpu, surface, get_instance_proc_addr,
			required_device_extensions, num_required_device_extensions,
			required_device_layers, num_required_device_layers, required_features, NULL, NULL);
}

bool VkCreateDevice2(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
		VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
		retro_vulkan_create_device_wrapper_t create_device_wrapper, void *opaque)
{
	return create_device(context, instance, gpu, surface, get_instance_proc_addr,
			NULL, 0, NULL, 0, NULL, create_device_wrapper, opaque);
}

/* The budget of the largest device-local heap, by VK_EXT_memory_budget
 * when the device has it (a physical device query: the extension does not
 * have to be enabled on the device the frontend made), and half that heap
 * when it has not */
static void query_vram_budget(void)
{
	static VkPhysicalDeviceMemoryProperties2 memory2;
	VkPhysicalDeviceMemoryBudgetPropertiesEXT budget;
	const VkPhysicalDeviceMemoryProperties *memory = &memory2.memoryProperties;
	bool haveBudget = false;
	u32 i, heap = VK_MAX_MEMORY_HEAPS;

	if (vkGetPhysicalDeviceMemoryProperties2 != NULL)
	{
		uint32_t count = 0;
		VkExtensionProperties *properties;
		vkEnumerateDeviceExtensionProperties(vkc.gpu, NULL, &count, NULL);
		properties = count != 0 ? (VkExtensionProperties *)malloc(count * sizeof(*properties)) : NULL;
		if (properties != NULL && vkEnumerateDeviceExtensionProperties(vkc.gpu, NULL, &count, properties) >= 0)
			for (i = 0; i < count; i++)
				if (!strcmp(properties[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
				{
					haveBudget = true;
					break;
				}
		free(properties);
	}

	memset(&budget, 0, sizeof(budget));
	memset(&memory2, 0, sizeof(memory2));
	if (haveBudget)
	{
		budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
		memory2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
		memory2.pNext = &budget;
		vkGetPhysicalDeviceMemoryProperties2(vkc.gpu, &memory2);
		memory2.pNext = NULL;
	}
	else
		vkGetPhysicalDeviceMemoryProperties(vkc.gpu, &memory2.memoryProperties);

	for (i = 0; i < memory->memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
		if ((memory->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
				&& (heap == VK_MAX_MEMORY_HEAPS || memory->memoryHeaps[i].size > memory->memoryHeaps[heap].size))
			heap = i;
	if (heap == VK_MAX_MEMORY_HEAPS)
		heap = 0;	/* (there is always a heap, and one at least is device-local) */

	memset(&vkc.vramBudget, 0, sizeof(vkc.vramBudget));
	if (haveBudget && budget.heapBudget[heap] != 0)
	{
		vkc.vramBudget.budget_kib = vram_kib(budget.heapBudget[heap]);
		vkc.vramBudget.usage_kib = vram_kib(budget.heapUsage[heap]);
		vkc.vramBudget.exact = true;
	}
	else
		vkc.vramBudget.budget_kib = vram_kib(memory->memoryHeaps[heap].size / 2);
	vram_budget_say("Vulkan", &vkc.vramBudget);
}

void vkc_log_vram_usage(void)
{
	query_vram_budget();
}

u32 vkc_ssaa_samples(u32 requested, u32 perSampleKib, u32 fixedKib, bool transient, const char *what)
{
	u32 device, samples, available;
	bool free_;
	if (requested <= 1)
		return 1;
	/* Shaded per sample, or it is not supersampling: a device the frontend
	 * made has no sample rate shading the core knows of */
	if (!vkc.sampleRateShading)
	{
		WARN_LOG(RENDERER, "%s: SSAA off: the device was made without sample rate shading\n", what);
		return 1;
	}
	device = ssaa_device_samples(requested, vkc.ssaaSampleCounts);
	/* Attachments that are never stored take no memory where the device
	 * has memory that is only backed when used (tilers) */
	free_ = transient && vkc.lazyMemory;
	available = vram_budget_available_kib(&vkc.vramBudget);
	samples = ssaa_clamp_samples(device, available, fixedKib, free_ ? 0 : perSampleKib);
	NOTICE_LOG(RENDERER, "%s: SSAA %u samples a pixel (asked %u, the device can draw %u; %u MB a sample%s, %u MB of the budget left)\n",
			what, samples, requested, device, perSampleKib >> 10, free_ ? " if backed" : "", available >> 10);
	return samples;
}

u32 vkc_depth_bytes(void)
{
	return vkc.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ? 8 : 4;
}

bool vkc_format_supported(int textureType)
{
	switch (textureType)
	{
	case TEX_TYPE_4444:
		return vkc.optimalTilingSupported4444;
	case TEX_TYPE_565:
		return vkc.optimalTilingSupported565;
	case TEX_TYPE_5551:
		return vkc.optimalTilingSupported1555;
	default:
		return true;
	}
}

const char *vkc_driver_name(void)
{
	static VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(vkc.gpu, &props);
	return props.deviceName;
}

static const char *depth_format_name(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return "D32SfloatS8Uint";
	case VK_FORMAT_D24_UNORM_S8_UINT:
		return "D24UnormS8Uint";
	default:
		return "D16UnormS8Uint";
	}
}

static bool find_depth_format(void)
{
	static const VkFormat depthFormats[] = { VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT };
	bool linear = false;
	unsigned i;
	vkc.depthFormat = VK_FORMAT_UNDEFINED;
	for (i = 0; i < ARRAY_SIZE(depthFormats); i++)
	{
		VkFormatProperties formatProperties;
		vkGetPhysicalDeviceFormatProperties(vkc.gpu, depthFormats[i], &formatProperties);
		if (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
		{
			vkc.depthFormat = depthFormats[i];
			break;
		}
	}
	if (vkc.depthFormat == VK_FORMAT_UNDEFINED)
	{
		/* a linear format then */
		for (i = 0; i < ARRAY_SIZE(depthFormats); i++)
		{
			VkFormatProperties formatProperties;
			vkGetPhysicalDeviceFormatProperties(vkc.gpu, depthFormats[i], &formatProperties);
			if (formatProperties.linearTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			{
				linear = true;
				vkc.depthFormat = depthFormats[i];
				break;
			}
		}
		if (vkc.depthFormat == VK_FORMAT_UNDEFINED)
		{
			ERROR_LOG(RENDERER, "No supported depth/stencil format found\n");
			return false;
		}
	}
	NOTICE_LOG(RENDERER, "Using depth format %s tiling %s\n", depth_format_name(vkc.depthFormat), linear ? "Linear" : "Optimal");
	return true;
}

static bool blit_format(VkFormat format, const char *name)
{
	VkFormatProperties formatProperties;
	const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT
			| VK_FORMAT_FEATURE_BLIT_SRC_BIT;
	vkGetPhysicalDeviceFormatProperties(vkc.gpu, format, &formatProperties);
	if ((formatProperties.optimalTilingFeatures & need) == need)
		return true;
	NOTICE_LOG(RENDERER, "%s not supported for optimal tiling\n", name);
	return false;
}

static bool heap_init(void)
{
	VkPhysicalDeviceMemoryProperties props;
	VkDeviceSize largest = 0, blockSize = 64u * 1024u * 1024u;
	vk_heap_fns_t fns;
	u32 i;

	verify(!vkc.heapReady);
	vkGetPhysicalDeviceMemoryProperties(vkc.gpu, &props);
	/* One block is 64 MB, or an eighth of the device's memory where that
	 * is less. */
	for (i = 0; i < props.memoryHeapCount; i++)
		if (props.memoryHeaps[i].size > largest)
			largest = props.memoryHeaps[i].size;
	if (largest / 8 < blockSize)
	{
		blockSize = (largest / 8) & ~(VkDeviceSize)0xFFFFF;
		if (blockSize < 4u * 1024u * 1024u)
			blockSize = 4u * 1024u * 1024u;
	}

	fns.allocate_memory = vkAllocateMemory;
	fns.free_memory = vkFreeMemory;
	fns.map_memory = vkMapMemory;
	fns.unmap_memory = vkUnmapMemory;
	if (!vk_heap_init(&vkc.heap, vkc.device, &props, &fns, blockSize, 0))
	{
		ERROR_LOG(RENDERER, "Vulkan heap: could not be made\n");
		return false;
	}
	vkc.heapReady = true;
	vkc.trimIn = 0;

	/* The first block for textures and render targets and the first one
	 * for the buffers the CPU fills are taken now, so that a frame gets
	 * its memory as an offset and not from the driver. */
	vk_heap_reserve(&vkc.heap, ~0u, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 1);
	vk_heap_reserve(&vkc.heap, ~0u, HOST_MEMORY, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, 1);

	NOTICE_LOG(RENDERER, "Vulkan heap: %u MB blocks, %u MB reserved\n", (u32)(blockSize >> 20), (u32)(vkc.heap.bytes_reserved >> 20));
	return true;
}

static void heap_term(void)
{
	if (vkc.heapReady)
	{
		vk_heap_shutdown(&vkc.heap);
		vkc.heapReady = false;
	}
}

/* After the renderer has changed size: blocks that what it had before
 * left empty go back to the driver, all but the last of each kind. */
static void heap_trim_spare(void)
{
	VkDeviceSize before;
	if (!vkc.heapReady)
		return;
	before = vkc.heap.bytes_reserved;
	if (vk_heap_trim_spare(&vkc.heap) != 0)
		NOTICE_LOG(RENDERER, "Vulkan heap: %u MB given back, %u MB reserved\n", (u32)((before - vkc.heap.bytes_reserved) >> 20),
				(u32)(vkc.heap.bytes_reserved >> 20));
}

/* Why the heap had nothing to give: what the driver said - no memory of
 * the kind, or memory it would not map - where there is that to go by. */
VkResult vkc_heap_error(void)
{
	return vkc.heap.last_error != VK_SUCCESS ? vkc.heap.last_error : VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

bool vkc_heap_alloc_image(VkImage image, bool linear, bool hostVisible, bool transient, vk_heap_alloc_t *alloc)
{
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(vkc.device, image, &requirements);
	if (hostVisible)
	{
		if (!vk_heap_alloc(&vkc.heap, &requirements, HOST_MEMORY, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, linear, alloc))
			return false;
	}
	/* A transient attachment takes memory the device only backs if it has
	 * to, where there is such a thing. */
	else if (!transient
			|| !vk_heap_alloc(&vkc.heap, &requirements,
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, 0, linear, alloc))
	{
		if (!vk_heap_alloc(&vkc.heap, &requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, linear, alloc))
			return false;
	}
	vkBindImageMemory(vkc.device, image, alloc->memory, alloc->offset);
	return true;
}

bool vkc_heap_alloc_buffer(VkBuffer buffer, bool hostVisible, bool readBack, vk_heap_alloc_t *alloc)
{
	VkMemoryRequirements requirements;
	VkMemoryPropertyFlags required = 0;
	VkMemoryPropertyFlags preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	vkGetBufferMemoryRequirements(vkc.device, buffer, &requirements);
	if (hostVisible)
	{
		required = HOST_MEMORY;
		/* What the CPU reads back wants the host's cache between it and
		 * the memory; device memory seen through the bus is write-combined
		 * and reading it is slow. What the CPU only writes goes as close
		 * to the device as there is. */
		if (readBack)
			preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	}
	if (!vk_heap_alloc(&vkc.heap, &requirements, required, preferred, true, alloc))
		return false;
	vkBindBufferMemory(vkc.device, buffer, alloc->memory, alloc->offset);
	return true;
}

/* The path of @name in the writable data directory (made at the start and
 * the end only) */
static char *data_path(const char *name, const char *suffix)
{
	const size_t n = writable_data_path(NULL, 0, name), s = strlen(suffix);
	char *path = (char *)malloc(n + s + 1);
	if (path == NULL)
		return NULL;
	writable_data_path(path, n + 1, name);
	memcpy(path + n, suffix, s + 1);
	return path;
}

/* The cache file: this header, the keys of the pipelines made
 * (vk_pso_keys), then the device's pipeline cache data. A file without the
 * header is a cache of an earlier core's, the device's data alone. */
typedef struct PsoCacheFileHeader
{
	char magic[8];
	u32 version;	/* of the keys' layout (vk_pipeline.h, oit/vk_oit_pipeline.h) */
	u32 keyCount;
	u64 dataSize;
} PsoCacheFileHeader;
static const char PsoCacheMagic[8] = { 'F', 'C', 'V', 'K', 'P', 'S', 'O', 0 };
#define PSO_CACHE_VERSION 1

static void load_pipeline_cache(void)
{
	char *cachePath = data_path(PipelineCacheFileName, "");
	void *file = NULL;
	int64_t fileSize = 0;
	const u8 *data = NULL;
	size_t dataSize = 0;
	u32 keyCount = 0;
	VkPipelineCacheCreateInfo info;
	VkPipelineCache cache = VK_NULL_HANDLE;

	vk_pso_keys_init(&vkc.psoKeys);
	if (cachePath != NULL && filestream_read_file(cachePath, &file, &fileSize) && file != NULL && fileSize > 0)
	{
		PsoCacheFileHeader header;
		memset(&header, 0, sizeof(header));
		if ((size_t)fileSize >= sizeof(header))
			memcpy(&header, file, sizeof(header));
		if ((size_t)fileSize >= sizeof(header) && !memcmp(header.magic, PsoCacheMagic, sizeof(PsoCacheMagic)))
		{
			const size_t keyBytes = (size_t)header.keyCount * sizeof(u64);
			if (keyBytes / sizeof(u64) == header.keyCount && sizeof(header) + keyBytes <= (size_t)fileSize
					&& header.dataSize <= (u64)fileSize - sizeof(header) - keyBytes)
			{
				if (header.version == PSO_CACHE_VERSION)
					vk_pso_keys_load(&vkc.psoKeys, (const u8 *)file + sizeof(header), header.keyCount);
				data = (const u8 *)file + sizeof(header) + keyBytes;
				dataSize = (size_t)header.dataSize;
			}
		}
		else
		{
			data = (const u8 *)file;
			dataSize = (size_t)fileSize;
		}
	}
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	info.flags = vkc.pipelineCacheControl ? VK_PSO_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED : 0;
	info.initialDataSize = dataSize;
	info.pInitialData = data;
	if (vkCreatePipelineCache(vkc.device, &info, NULL, &cache) != VK_SUCCESS)
	{
		info.initialDataSize = 0;
		info.pInitialData = NULL;
		if (vkCreatePipelineCache(vkc.device, &info, NULL, &cache) != VK_SUCCESS)
			cache = VK_NULL_HANDLE;
	}
	vkc.pipelineCache = cache;
	/* the worker's, from the same data; the fixed pipelines a draw makes itself, in this one */
	vk_pso_worker_set_draw_cache(vkc.pipelineCache);
	vk_pso_worker_start(vkc.device, data, dataSize, vkc.pipelineCacheControl);
	vkc.savedBuilt = 0;
	vk_pso_keys_sorted(&vkc.psoKeys, &keyCount);
	NOTICE_LOG(RENDERER, "Vulkan pipeline cache loaded from %s: %u bytes, %u pipeline keys\n",
			cachePath != NULL ? cachePath : "", (unsigned)dataSize, keyCount);
	free(file);
	free(cachePath);
}

void vkc_save_pipeline_cache(void)
{
	size_t dataSize = 0, fileBytes;
	u32 keyCount = 0, made;
	const u64 *keys;
	PsoCacheFileHeader header;
	u8 *file;
	char *cachePath, *tmpPath;
	RFILE *f;
	bool written;

	if (vkc.pipelineCache == VK_NULL_HANDLE)
		return;
	/* nothing made since the last save: as it is */
	made = vk_pso_worker_built() + vk_pso_worker_sync_built();
	if (made == vkc.savedBuilt && vkc.savedBuilt != 0)
		return;
	vk_pso_worker_wait_idle();
	vk_pso_worker_merge_into(vkc.pipelineCache);
	if (vkGetPipelineCacheData(vkc.device, vkc.pipelineCache, &dataSize, NULL) != VK_SUCCESS)
		return;
	keys = vk_pso_keys_sorted(&vkc.psoKeys, &keyCount);
	memcpy(header.magic, PsoCacheMagic, sizeof(PsoCacheMagic));
	header.version = PSO_CACHE_VERSION;
	header.keyCount = keyCount;
	file = (u8 *)malloc(sizeof(header) + keyCount * sizeof(u64) + dataSize);
	if (file == NULL)
		return;
	if (vkGetPipelineCacheData(vkc.device, vkc.pipelineCache, &dataSize, file + sizeof(header) + keyCount * sizeof(u64)) != VK_SUCCESS)
	{
		free(file);
		return;
	}
	header.dataSize = dataSize;
	memcpy(file, &header, sizeof(header));
	if (keyCount != 0)
		memcpy(file + sizeof(header), keys, keyCount * sizeof(u64));
	fileBytes = sizeof(header) + keyCount * sizeof(u64) + dataSize;
	/* (written beside it, then put in its place: a write cut short leaves the one there was) */
	cachePath = data_path(PipelineCacheFileName, "");
	tmpPath = data_path(PipelineCacheFileName, ".tmp");
	f = cachePath != NULL && tmpPath != NULL
			? filestream_open(tmpPath, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE) : NULL;
	if (f != NULL)
	{
		written = filestream_write(f, file, (int64_t)fileBytes) == (int64_t)fileBytes;
		filestream_close(f);
		if (written)
		{
			filestream_delete(cachePath);
			written = filestream_rename(tmpPath, cachePath) == 0;
		}
		if (!written)
			filestream_delete(tmpPath);
		else
			NOTICE_LOG(RENDERER, "Vulkan pipeline cache saved to %s: %u bytes, %u pipeline keys\n", cachePath, (unsigned)dataSize, keyCount);
		vkc.savedBuilt = made;
	}
	free(cachePath);
	free(tmpPath);
	free(file);
}

VkDescriptorSet vkc_alloc_desc_set(VkDescriptorSetLayout layout)
{
	VkDescriptorSetAllocateInfo info;
	VkDescriptorSet set = VK_NULL_HANDLE;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	info.descriptorPool = vkc.descriptorPool;
	info.descriptorSetCount = 1;
	info.pSetLayouts = &layout;
	if (vkAllocateDescriptorSets(vkc.device, &info, &set) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return set;
}

void vkc_free_desc_set(VkDescriptorSet set)
{
	if (set != VK_NULL_HANDLE)
		vkFreeDescriptorSets(vkc.device, vkc.descriptorPool, 1, &set);
}

static PFN_vkVoidFunction device_proc(const char *name)
{
	return vkGetDeviceProcAddr(vkc.device, name);
}

bool vkc_init(struct retro_hw_render_interface_vulkan *iface)
{
	static VkPhysicalDeviceProperties props;
	static VkPhysicalDeviceProperties2 properties2;
	const VkPhysicalDeviceProperties *properties;
	VkPhysicalDeviceMemoryProperties memory;
	VkDescriptorPoolSize pool_sizes[11];
	VkDescriptorPoolCreateInfo poolInfo;
	u32 i, m;

	if (iface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN
			|| iface->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
		return false;
	vkc.iface = iface;
	vkc.swapChainSize = 1;
	for (m = iface->get_sync_index_mask(iface->handle); m >>= 1; )
		vkc.swapChainSize++;
	vkc.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
	vkc.maxDrawIndirectCount = 1;

	vkc.instance = iface->instance;
	vkc.gpu = iface->gpu;
	vkc.device = iface->device;
	vkc.queue = iface->queue;

	vkGetPhysicalDeviceProperties(vkc.gpu, &props);
	NOTICE_LOG(RENDERER, "GPU Supports Vulkan API: %u.%u.%u\n",
			VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	if (VK_VERSION_MINOR(props.apiVersion) >= 1 && vkGetPhysicalDeviceFormatProperties2 != NULL)
	{
		VkPhysicalDeviceMaintenance3Properties properties3;
		NOTICE_LOG(RENDERER, "GPU Supports vkGetPhysicalDeviceProperties2\n");
		memset(&properties3, 0, sizeof(properties3));
		properties3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
		memset(&properties2, 0, sizeof(properties2));
		properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		properties2.pNext = &properties3;
		vkGetPhysicalDeviceProperties2(vkc.gpu, &properties2);
		properties2.pNext = NULL;
		properties = &properties2.properties;
		vkc.maxMemoryAllocationSize = properties3.maxMemoryAllocationSize;
	}
	else
	{
		properties = &props;
		vkc.maxMemoryAllocationSize = 0xFFFFFFFFu;
	}
	vkc.uniformBufferAlignment = properties->limits.minUniformBufferOffsetAlignment;
	vkc.storageBufferAlignment = properties->limits.minStorageBufferOffsetAlignment;
	vkc.maxStorageBufferRange = properties->limits.maxStorageBufferRange;
	vkc.maxSamplerAnisotropy = properties->limits.maxSamplerAnisotropy;
	vkc.vendorID = properties->vendorID;
	vkc.apiVersion = props.apiVersion;
	vkc.maxDrawIndirectCount = properties->limits.maxDrawIndirectCount;
	vkc.cmdDrawIndexedIndirectCount = vkc.drawIndirectCount
			? (PFN_vkCmdDrawIndexedIndirectCountKHR)device_proc("vkCmdDrawIndexedIndirectCountKHR") : NULL;
	/* the general pipelines' dynamic states, as the device was made with them (pso.h) */
	memset(&vkc.dynamicState, 0, sizeof(vkc.dynamicState));
	if (vkc.extendedDynamicState)
	{
		vk_pso_dyn *f = &vkc.dynamicState;
		f->CmdSetCullMode = (void (VKAPI_PTR *)(VkCommandBuffer, VkCullModeFlags))device_proc("vkCmdSetCullModeEXT");
		f->CmdSetPrimitiveTopology = (void (VKAPI_PTR *)(VkCommandBuffer, VkPrimitiveTopology))device_proc("vkCmdSetPrimitiveTopologyEXT");
		f->CmdSetDepthTestEnable = (void (VKAPI_PTR *)(VkCommandBuffer, VkBool32))device_proc("vkCmdSetDepthTestEnableEXT");
		f->CmdSetDepthWriteEnable = (void (VKAPI_PTR *)(VkCommandBuffer, VkBool32))device_proc("vkCmdSetDepthWriteEnableEXT");
		f->CmdSetDepthCompareOp = (void (VKAPI_PTR *)(VkCommandBuffer, VkCompareOp))device_proc("vkCmdSetDepthCompareOpEXT");
		f->CmdSetStencilTestEnable = (void (VKAPI_PTR *)(VkCommandBuffer, VkBool32))device_proc("vkCmdSetStencilTestEnableEXT");
		f->CmdSetStencilOp = (void (VKAPI_PTR *)(VkCommandBuffer, VkStencilFaceFlags, VkStencilOp, VkStencilOp, VkStencilOp, VkCompareOp))
				device_proc("vkCmdSetStencilOpEXT");
		f->depthStencil = f->CmdSetCullMode && f->CmdSetPrimitiveTopology && f->CmdSetDepthTestEnable && f->CmdSetDepthWriteEnable
				&& f->CmdSetDepthCompareOp && f->CmdSetStencilTestEnable && f->CmdSetStencilOp;
	}
	if (vkc.extendedDynamicState3)
	{
		vk_pso_dyn *f = &vkc.dynamicState;
		f->CmdSetColorBlendEnable = (void (VKAPI_PTR *)(VkCommandBuffer, uint32_t, uint32_t, const VkBool32 *))
				device_proc("vkCmdSetColorBlendEnableEXT");
		f->CmdSetColorBlendEquation = (void (VKAPI_PTR *)(VkCommandBuffer, uint32_t, uint32_t, const vk_pso_blend_equation *))
				device_proc("vkCmdSetColorBlendEquationEXT");
		f->CmdSetColorWriteMask = (void (VKAPI_PTR *)(VkCommandBuffer, uint32_t, uint32_t, const VkColorComponentFlags *))
				device_proc("vkCmdSetColorWriteMaskEXT");
		f->blend = f->CmdSetColorBlendEnable && f->CmdSetColorBlendEquation && f->CmdSetColorWriteMask;
	}
#ifdef VK_PSO_TEST_NO_EDS
	vkc.dynamicState.depthStencil = false;
#endif
#ifdef VK_PSO_TEST_NO_EDS3
	vkc.dynamicState.blend = false;
#endif
	NOTICE_LOG(RENDERER, "Vulkan: general pipelines drawn with %s depth, stencil, cull and topology, and %s blend\n",
			vkc.dynamicState.depthStencil ? "dynamic" : "fixed", vkc.dynamicState.blend ? "dynamic" : "fixed");
	query_vram_budget();
	/* what SSAA can have: the sample counts colour, depth and stencil attachments can all have, and memory that is only there if used */
	vkc.ssaaSampleCounts = (u32)(properties->limits.framebufferColorSampleCounts & properties->limits.framebufferDepthSampleCounts
			& properties->limits.framebufferStencilSampleCounts);
	vkc.lazyMemory = false;
	vkGetPhysicalDeviceMemoryProperties(vkc.gpu, &memory);
	for (i = 0; i < memory.memoryTypeCount; i++)
		if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
			vkc.lazyMemory = true;

	vkc.optimalTilingSupported1555 = blit_format(VK_FORMAT_R5G5B5A1_UNORM_PACK16, "eR5G5B5A1UnormPack16");
	vkc.optimalTilingSupported565 = blit_format(VK_FORMAT_R5G6B5_UNORM_PACK16, "eR5G6B5UnormPack16");
	vkc.optimalTilingSupported4444 = blit_format(VK_FORMAT_R4G4B4A4_UNORM_PACK16, "eR4G4B4A4UnormPack16");

	/* the descriptor pool */
	pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLER;                pool_sizes[0].descriptorCount = 2;
	pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; pool_sizes[1].descriptorCount = 4000;
	pool_sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;          pool_sizes[2].descriptorCount = 2;
	pool_sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          pool_sizes[3].descriptorCount = 24;
	pool_sizes[4].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;   pool_sizes[4].descriptorCount = 2;
	pool_sizes[5].type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;   pool_sizes[5].descriptorCount = 2;
	pool_sizes[6].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         pool_sizes[6].descriptorCount = 64;
	pool_sizes[7].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         pool_sizes[7].descriptorCount = 64;
	pool_sizes[8].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; pool_sizes[8].descriptorCount = 2;
	pool_sizes[9].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; pool_sizes[9].descriptorCount = 2;
	pool_sizes[10].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;      pool_sizes[10].descriptorCount = 96;
	memset(&poolInfo, 0, sizeof(poolInfo));
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	poolInfo.maxSets = 10000;
	poolInfo.poolSizeCount = ARRAY_SIZE(pool_sizes);
	poolInfo.pPoolSizes = pool_sizes;
	if (vkCreateDescriptorPool(vkc.device, &poolInfo, NULL, &vkc.descriptorPool) != VK_SUCCESS)
	{
		vkc.descriptorPool = VK_NULL_HANDLE;
		ERROR_LOG(RENDERER, "Vulkan: the descriptor pool could not be made\n");
		return false;
	}

	load_pipeline_cache();
	if (!heap_init() || !find_depth_format())
		return false;

	/* the HDR output, when the frontend took it (libretro.c) */
	if (hdr_output.format != HDR_OUTPUT_NONE)
	{
		/* (by enum hdr_bloom_stage) */
		static const int bloom[HDR_BLOOM_STAGES] = { RSPV_HDR_BLOOM_BRIGHT, RSPV_HDR_BLOOM_BLUR,
				RSPV_HDR_BLOOM_DOWN, RSPV_HDR_BLOOM_DOWN_KARIS, RSPV_HDR_BLOOM_UP };
		hdr_vk_code_t code;
		vkc.hdrFormat = hdr_output.format == HDR_OUTPUT_SCRGB ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		code.vert = RasterSpvWords(RSPV_SSAA_RESOLVE_VERT, &code.vert_size);
		code.frag = RasterSpvWords(RSPV_HDR_ENCODE_FRAG, &code.frag_size);
		code.frag_subpass = RasterSpvWords(RSPV_HDR_ENCODE_FRAG_SUBPASS, &code.frag_subpass_size);
		code.frag_subpass_ssaa = RasterSpvWords(RSPV_HDR_ENCODE_FRAG_SUBPASS_SSAA, &code.frag_subpass_ssaa_size);
		code.frag_sums = RasterSpvWords(RSPV_HDR_ENCODE_FRAG_SUMS, &code.frag_sums_size);
		code.frag_linear = RasterSpvWords(RSPV_HDR_ENCODE_FRAG_LINEAR, &code.frag_linear_size);
		code.frag_sums_linear = RasterSpvWords(RSPV_HDR_ENCODE_FRAG_SUMS_LINEAR, &code.frag_sums_linear_size);
		for (i = 0; i < HDR_BLOOM_STAGES; i++)
			code.bloom[i] = RasterSpvWords(bloom[i], &code.bloom_size[i]);
		code.post = RasterSpvWords(RSPV_HDR_POST_FRAG, &code.post_size);
		vkc.hdr = hdr_vk_new(iface, &vkc.heap, vkc.pipelineCache, vkc.hdrFormat, (unsigned)vkc.swapChainSize, &code);
		if (vkc.hdr == NULL)
			ERROR_LOG(RENDERER, "Vulkan: the HDR output could not be set up; the picture goes out as 24-bit\n");
		else
			NOTICE_LOG(RENDERER, "Vulkan: HDR output, %s\n", hdr_output.format == HDR_OUTPUT_SCRGB ? "scRGB (RGBA16F)" : "HDR10 (A2B10G10R10)");
	}

	vkc.retro_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&vkc.retro_image.create_info, 0, sizeof(vkc.retro_image.create_info));
	vkc.retro_image.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vkc.retro_image.create_info.format = vkc.colorFormat;
	vkc.retro_image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vkc.retro_image.create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	vkc.retro_image.create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	vkc.retro_image.create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	vkc.retro_image.create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	vkc.retro_image.create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkc.retro_image.create_info.subresourceRange.baseArrayLayer = 0;
	vkc.retro_image.create_info.subresourceRange.layerCount = 1;
	vkc.retro_image.create_info.subresourceRange.baseMipLevel = 0;
	vkc.retro_image.create_info.subresourceRange.levelCount = 1;
	return true;
}

void vkc_set_window_size(u32 width, u32 height)
{
	vkc.width = width;
	vkc.height = height;
	screen_width = (int)width;
	screen_height = (int)height;
}

void vkc_submit(u32 count, const VkCommandBuffer *buffers, VkFence fence)
{
	VkSubmitInfo info;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	info.commandBufferCount = count;
	info.pCommandBuffers = buffers;
	vkc.iface->lock_queue(vkc.iface->handle);
	vkQueueSubmit(vkc.queue, 1, &info, fence);
	vkc.iface->unlock_queue(vkc.iface->handle);
}

void vkc_present_frame(VkImage image, VkImageView view, s32 width, s32 height, bool hdr)
{
	if (vkc.hdr != NULL)
		vkc.retro_image.create_info.format = hdr ? vkc.hdrFormat : vkc.colorFormat;
	vkc.retro_image.image_view = view;
	vkc.retro_image.create_info.image = image;
	vkc.presentedWidth = (u32)width;
	vkc.presentedHeight = (u32)height;
	vkc.iface->set_image(vkc.iface->handle, &vkc.retro_image, 0, NULL, VK_QUEUE_FAMILY_IGNORED);
	/* once a frame: memory left empty by a change of size goes back to the driver */
	if (vkc.trimIn != 0 && --vkc.trimIn == 0)
		heap_trim_spare();
}

static unsigned hdr_ring(const void *pool)
{
	unsigned i;
	for (i = 0; i < ARRAY_SIZE(vkc.hdrPools) && vkc.hdrPools[i] != NULL; i++)
		if (vkc.hdrPools[i] == pool)
			return i;
	if (i < ARRAY_SIZE(vkc.hdrPools))
		vkc.hdrPools[i] = pool;
	return i;	/* (past the last ring: the encode leaves it alone) */
}

void vkc_hdr_prepare(const void *pool, unsigned index, unsigned frames, VkCommandBuffer cmd)
{
	if (vkc.hdr != NULL)
		hdr_vk_prepare(vkc.hdr, &hdr_output, cmd, hdr_ring(pool), index, frames);
}

VkPipeline vkc_hdr_subpass_pipeline(VkRenderPass renderPass, u32 subpass, u32 samples)
{
	VkPipeline p;
	if (vkc.hdr == NULL)
		return VK_NULL_HANDLE;
	p = hdr_vk_subpass_pipeline(vkc.hdr, renderPass, subpass, samples);
	if (p == VK_NULL_HANDLE)
		ERROR_LOG(RENDERER, "Vulkan: the HDR output's subpass pipeline could not be made\n");
	return p;
}

void vkc_hdr_subpass_draw(const void *pool, unsigned index, VkCommandBuffer cmd, VkPipeline pipeline,
		VkImageView picture, VkImageView samples, u32 width, u32 height)
{
	/* (every picture is 8 bits a channel, dithered; with SSAA's samples,
	 * what is encoded is their exact average, which is not) */
	if (vkc.hdr != NULL)
		hdr_vk_subpass_draw(vkc.hdr, &hdr_output, cmd, hdr_ring(pool), index, pipeline, picture, samples,
				width, height, samples == VK_NULL_HANDLE);
}

bool vkc_hdr_post_active(void)
{
	return vkc.hdr != NULL && hdr_vk_post_active();
}

bool vkc_hdr_sums_ready(void)
{
	return vkc.hdr != NULL && hdr_vk_sums_ready(vkc.hdr);
}

bool vkc_hdr_encode(const void *pool, unsigned index, unsigned frames, VkCommandBuffer cmd,
		VkImage *image, VkImageView *view, u32 width, u32 height, VkImageView sums)
{
	if (vkc.hdr == NULL)
		return false;
	return hdr_vk_encode(vkc.hdr, &hdr_output, cmd, hdr_ring(pool), index, frames,
			(unsigned)vkc_current_image_index(), image, view, width, height, true, sums);
}

/* The frontend keeps the last picture it was handed and draws it again
 * for as long as no other comes: on a frame the game draws nothing new
 * in, behind its menu, and - with threaded video - from its video thread
 * while this thread has gone on, so that a picture handed over can still
 * be in a frame not yet drawn. wait_sync_index does not cover that (it is
 * for the index the core draws into next, and RetroArch's own driver
 * waits for nothing). A picture destroyed is a picture the frontend then
 * reads freed memory from. So the frontend is told to let go of it first,
 * by an image of none: RetroArch's threaded video draws what it has
 * queued and waits for its own submissions then. The queue the frontend
 * and the core share is then waited out, for a frontend that only forgets
 * the image and whatever it submitted with it. A change of renderer is
 * rare and the machine stands still for it; no frame waits here. The next
 * picture the new renderer draws is handed over as ever. */
void vkc_release_picture(void)
{
	if (vkc.iface == NULL || vkc.device == VK_NULL_HANDLE)
		return;
	vkc.iface->set_image(vkc.iface->handle, NULL, 0, NULL, VK_QUEUE_FAMILY_IGNORED);
	vkc_wait_queue_idle();
}

void vkc_wait_queue_idle(void)
{
	if (vkc.iface == NULL || vkc.device == VK_NULL_HANDLE)
		return;
	vkc.iface->lock_queue(vkc.iface->handle);
	(void)vkQueueWaitIdle(vkc.queue);
	vkc.iface->unlock_queue(vkc.iface->handle);
}

void vkc_term(void)
{
	unsigned i;
	vkc.presentedWidth = 0;
	vkc.presentedHeight = 0;
	if (vkc.device != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(vkc.device);
		/* (the worker's cache merged into this one) */
		vk_pso_worker_stop(vkc.pipelineCache);
		vkc_save_pipeline_cache();
	}
	/* (before the heap it has its memory from) */
	hdr_vk_free(vkc.hdr);
	vkc.hdr = NULL;
	for (i = 0; i < ARRAY_SIZE(vkc.hdrPools); i++)
		vkc.hdrPools[i] = NULL;
	if (vkc.descriptorPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(vkc.device, vkc.descriptorPool, NULL);
	vkc.descriptorPool = VK_NULL_HANDLE;
	heap_term();
	if (vkc.pipelineCache != VK_NULL_HANDLE)
		vkDestroyPipelineCache(vkc.device, vkc.pipelineCache, NULL);
	vkc.pipelineCache = VK_NULL_HANDLE;
	vk_pso_worker_set_draw_cache(VK_NULL_HANDLE);
	vk_pso_keys_term(&vkc.psoKeys);
	vkc.extendedDynamicState = false;
	vkc.extendedDynamicState3 = false;
	vkc.pipelineCacheControl = false;
	memset(&vkc.dynamicState, 0, sizeof(vkc.dynamicState));
}
