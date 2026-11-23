/*
	Copyright 2026 flyinghead

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
#include "vulkan.h"
#include "texture.h"
#include "commandpool.h"
#include "vk_motion_pass.h"
#include <vector>

/* The C++ renderers' view of a vk_motion_pass (vk_motion_pass.h), until they are C too */
class MotionVectorPass
{
public:
	MotionVectorPass() { vk_motion_pass_construct(&m); }
	~MotionVectorPass() { vk_motion_pass_term(&m, nullptr); }
	MotionVectorPass(const MotionVectorPass&) = delete;
	MotionVectorPass& operator=(const MotionVectorPass&) = delete;
	void Init(vk::Extent2D size, vk::Format depthFormat, vk::ImageLayout depthLayout,
			const std::vector<vk::ImageView>& depthViews, vk::PipelineLayout pipelineLayout,
			vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader, CommandPool *commandPool)
	{
		if (!vk_motion_pass_init(&m, size, (VkFormat)depthFormat, (VkImageLayout)depthLayout,
				reinterpret_cast<const VkImageView *>(depthViews.data()), (u32)depthViews.size(), (VkPipelineLayout)pipelineLayout,
				(VkShaderModule)vertexShader, (VkShaderModule)fragmentShader, commandPool != nullptr ? &commandPool->p : nullptr))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_motion_pass_init");
	}
	void InitSampled(vk::Extent2D size, const std::vector<vk::ImageView>& depthViews,
			const std::vector<vk::Buffer>& uniformBuffers, vk::DeviceSize uniformBytes,
			vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader, CommandPool *commandPool)
	{
		if (!vk_motion_pass_init_sampled(&m, size, reinterpret_cast<const VkImageView *>(depthViews.data()), (u32)depthViews.size(),
				reinterpret_cast<const VkBuffer *>(uniformBuffers.data()), uniformBytes,
				(VkShaderModule)vertexShader, (VkShaderModule)fragmentShader, commandPool != nullptr ? &commandPool->p : nullptr))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_motion_pass_init_sampled");
	}
	void Term(CommandPool *commandPool) { vk_motion_pass_term(&m, commandPool != nullptr ? &commandPool->p : nullptr); }
	bool Begin(int image) { return vk_motion_pass_begin(&m, image); }
	void Record(vk::CommandBuffer commandBuffer, int image, vk::Buffer vertexBuffer, vk::DeviceSize indexOffset,
			const vk::Rect2D& scissor, vk::DeviceSize vertexOffset = 0)
	{
		if (!vk_motion_pass_record(&m, (VkCommandBuffer)commandBuffer, image, (VkBuffer)vertexBuffer, indexOffset,
				reinterpret_cast<const VkRect2D *>(&scissor), vertexOffset))
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateGraphicsPipelines");
	}
	void Submit(int image, vk::Image depthImage, vk::ImageView depthView, vk::Format depthFormat)
	{
		vk_motion_pass_submit(&m, image, (VkImage)depthImage, (VkImageView)depthView, (VkFormat)depthFormat);
	}
	bool Recorded() const { return m.recorded; }

private:
	vk_motion_pass m;
};
