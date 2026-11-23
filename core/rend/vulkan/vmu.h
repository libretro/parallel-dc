/*
    Created on: Dec 13, 2019

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
#include "quad.h"
#include "commandpool.h"
#include "vk_osd.h"

/* The C++ renderers' view of a vk_osd, until they are C too */
class VulkanOSD
{
public:
	VulkanOSD() { vk_osd_construct(&o); }
	~VulkanOSD() { vk_osd_term(&o); }
	VulkanOSD(const VulkanOSD&) = delete;
	VulkanOSD& operator=(const VulkanOSD&) = delete;
	void Init(QuadPipeline *pipeline)
	{
		if (!vk_osd_init(&o, &pipeline->p))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_osd_init");
	}
	void PrepareOSD(CommandPool *commandPool) { vk_osd_prepare(&o, &commandPool->p); }
	/* commandPool: the pool commandBuffer is of; @with, a pipeline of another render pass's to draw with (the same layout) */
	void DrawOSD(vk::CommandBuffer commandBuffer, vk::Extent2D viewport, CommandPool *commandPool, QuadPipeline *with = nullptr)
	{
		if (!vk_osd_draw(&o, (VkCommandBuffer)commandBuffer, viewport.width, viewport.height, &commandPool->p,
				with != nullptr ? &with->p : nullptr))
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateGraphicsPipelines");
	}
	/* whether DrawOSD() has anything to draw */
	bool Drawing() const { return vk_osd_drawing(&o); }

private:
	vk_osd o;
};
