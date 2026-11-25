/*
    Created on: Nov 12, 2019

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
#include "../vulkan_context.h"
#include "../commandpool.h"
#include "vk_oit_buffer.h"

/* The C++ per-pixel drawers' view of a vk_oit_buffers (vk_oit_buffer.h) */
class OITBuffers
{
public:
	OITBuffers() { vk_oit_buffers_construct(&o); }
	~OITBuffers() { vk_oit_buffers_free(&o); }
	OITBuffers(const OITBuffers&) = delete;
	OITBuffers& operator=(const OITBuffers&) = delete;

	vk::DeviceSize PlannedSize(int width, int height) const { return vk_oit_buffers_planned_size(&o, width, height); }
	void SetCompact(bool on) { vk_oit_buffers_set_compact(&o, on); }
	/* With SSAA and the HDR output: an image of each pixel's channel sums,
	 * which the screen's last pass stores (OIT_FINAL_FRAG_S1_SUMS) for the
	 * encode to take the exact average from. Before the renderer is set
	 * up, or made again for a size; made at the next Init(). */
	void SetSums(bool on) { o.sums = on; }
	bool Sums() const { return o.sums; }
	// (null without)
	vk::ImageView GetSumsView() const { return vk::ImageView(o.sumsAttachment.view); }
	void SetScreenSize(int width, int height) { vk_oit_buffers_set_screen_size(&o, width, height); }
	void Init(int width, int height, CommandPool *commandPool)
	{
		if (!vk_oit_buffers_init(&o, width, height, commandPool != nullptr ? &commandPool->p : nullptr))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_oit_buffers_init");
	}
	void BindDescriptorSet(vk::CommandBuffer cmdBuffer, vk::PipelineLayout pipelineLayout, u32 firstSet)
	{
		vkCmdBindDescriptorSets((VkCommandBuffer)cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipelineLayout)pipelineLayout,
				firstSet, 1, &o.descSet, 0, nullptr);
	}
	void OnNewFrame(vk::CommandBuffer commandBuffer) { vk_oit_buffers_on_new_frame(&o, (VkCommandBuffer)commandBuffer); }
	void ResetPixelCounter(vk::CommandBuffer commandBuffer) { vk_oit_buffers_reset_pixel_counter(&o, (VkCommandBuffer)commandBuffer); }
	void CountPass(vk::CommandBuffer commandBuffer) { vk_oit_buffers_count_pass(&o, (VkCommandBuffer)commandBuffer); }
	u32 ChooseBands(int slot, bool rtt) { return vk_oit_buffers_choose_bands(&o, slot, rtt); }
	void SetBands(u32 bands) { vk_oit_buffers_set_bands(&o, bands); }
	vk::DeviceSize GetSize() const { return o.bufferSize; }
	void Term() { vk_oit_buffers_term(&o); }
	vk::DescriptorSetLayout GetDescriptorSetLayout() const { return vk::DescriptorSetLayout(o.descSetLayout); }
	bool isFirstFrameAfterInit() const { return o.firstFrameAfterInit; }

	mutable vk_oit_buffers o;
};
