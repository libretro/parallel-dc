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
#include "buffer.h"
#include "texture.h"
#include "commandpool.h"
#include "rend/upscaler.h"
#include <memory>
#include <vector>

/* The temporal upscaler's motion vector pass (rend/upscaler.h), for
 * the screen drawers: a render pass of its own after the frame's, over
 * the depth the frame left (loaded, tested, not written, kept for the
 * upscaler), into the vectors' image, which the upscaler reads as the
 * colour. The vectors are of the strips rend/upscaler.cpp matched with
 * the frame before's. The pipelines are made with the drawer's layout,
 * whose per-frame descriptor set (the vertex uniforms) is bound when
 * Record() is called. */
class MotionVectorPass
{
public:
	/* Made for a size and the frame's depth images (one a frame slot,
	 * which the vectors' images are too), in the layout the frame leaves
	 * them in; made again when any of that changes */
	void Init(vk::Extent2D size, vk::Format depthFormat, vk::ImageLayout depthLayout,
			const std::vector<vk::ImageView>& depthViews, vk::PipelineLayout pipelineLayout,
			vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader, CommandPool *commandPool);
	/* The same over a depth stored by the compute renderer, which is no
	 * attachment but an image sampled (mv_depth.frag tests against it):
	 * the pass has a layout of its own then, with the vertex uniforms
	 * @uniformBuffers (one a frame slot, the drawer's) and the depth */
	void InitSampled(vk::Extent2D size, const std::vector<vk::ImageView>& depthViews,
			const std::vector<vk::Buffer>& uniformBuffers, vk::DeviceSize uniformBytes,
			vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader, CommandPool *commandPool);
	// what was made goes once the frames that could use it have gone by
	void Term(CommandPool *commandPool);

	/* Before the frame is drawn, under its jitter: whether this frame's
	 * vectors are drawn (the frame's strips are needed: not a GPU-parsed
	 * one); matches the strips and uploads where each vertex was */
	bool Begin(int image);
	// (the vertices and indices in @vertexBuffer, at their offsets)
	void Record(vk::CommandBuffer commandBuffer, int image, vk::Buffer vertexBuffer, vk::DeviceSize indexOffset,
			const vk::Rect2D& scissor, vk::DeviceSize vertexOffset = 0);
	// the vectors and the frame's depth, to the upscaler
	void Submit(int image, vk::Image depthImage, vk::ImageView depthView, vk::Format depthFormat);
	bool Recorded() const { return recorded; }

private:
	vk::Extent2D size;
	vk::ImageLayout depthLayout = vk::ImageLayout::eUndefined;
	vk::UniqueRenderPass renderPass;
	vk::UniquePipeline pipelines[3];
	std::vector<std::unique_ptr<FramebufferAttachment>> attachments;
	std::vector<vk::UniqueFramebuffer> framebuffers;
	std::vector<std::unique_ptr<BufferData>> prevXyBuffers;
	std::vector<float> prevXy;
	bool recorded = false;
	// the sampled-depth mode's own layout and sets
	bool sampled = false;
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniquePipelineLayout ownLayout;
	vk::UniqueDescriptorPool descriptorPool;
	std::vector<vk::DescriptorSet> sets;
	vk::UniqueSampler depthSampler;

	void MakePipelines(vk::PipelineLayout pipelineLayout, vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader, bool depthTest);
};
