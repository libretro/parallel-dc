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
#pragma once
#include "types.h"

#include <vulkan/vulkan_symbol_wrapper.h>

extern PFN_vkGetInstanceProcAddr vulkan_symbol_wrapper_vkGetInstanceProcAddr;
#define vkGetInstanceProcAddr vulkan_symbol_wrapper_vkGetInstanceProcAddr

#undef VK_NO_PROTOTYPES
#include "vulkan/vulkan.hpp"

/* One descriptor set from @pool, given back to it when let go: what
 * allocateDescriptorSetsUnique() makes, without the std::vector it makes
 * it in (nothing allocated but the set). None if the pool has no room. */
static inline vk::UniqueDescriptorSet vk_alloc_desc_set(vk::Device device, vk::DescriptorPool pool, vk::DescriptorSetLayout layout)
{
	static const vk::DispatchLoaderDefault dispatch;
	const vk::DescriptorSetAllocateInfo info(pool, 1, &layout);
	vk::DescriptorSet set;
	if (device.allocateDescriptorSets(&info, &set) != vk::Result::eSuccess)
		return vk::UniqueDescriptorSet();
	return vk::UniqueDescriptorSet(set, vk::PoolFree<vk::Device, vk::DescriptorPool, vk::DispatchLoaderDefault>(device, pool, dispatch));
}

//#define VK_DEBUG
