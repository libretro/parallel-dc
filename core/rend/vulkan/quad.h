/*
    Created on: Nov 7, 2019

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
#include "vulkan.h"
#include "buffer.h"
#include "shaders.h"
#include "pso.h"
#include "vk_quad.h"

/* The C++ renderers' view of vk_quad.h, until they are C too */
static inline vk::PipelineVertexInputStateCreateInfo GetQuadInputStateCreateInfo(bool uv)
{
	VkPipelineVertexInputStateCreateInfo info;
	vk_quad_input_state(&info, uv);
	return vk::PipelineVertexInputStateCreateInfo(info);
}

static inline bool QuadGameArea(QuadVertex v[4], vk::Extent2D from, vk::Extent2D to)
{
	return vk_quad_game_area(v, from.width, from.height, to.width, to.height);
}

/* Four vertices of a quad, the whole framebuffer's unless given others */
class QuadBuffer
{
public:
	QuadBuffer()
	{
		const VkResult res = vk_buffer_init(&b, sizeof(QuadVertex) * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (res != VK_SUCCESS)
			throwResultException((vk::Result)res, "vk_buffer_init");
	}
	~QuadBuffer() { vk_buffer_free(&b); }
	QuadBuffer(const QuadBuffer&) = delete;
	QuadBuffer& operator=(const QuadBuffer&) = delete;
	void Bind(vk::CommandBuffer commandBuffer)
	{
		const VkDeviceSize zero = 0;
		vkCmdBindVertexBuffers((VkCommandBuffer)commandBuffer, 0, 1, &b.buffer, &zero);
	}
	void Draw(vk::CommandBuffer commandBuffer) { vkCmdDraw((VkCommandBuffer)commandBuffer, 4, 1, 0, 0); }
	void Update(QuadVertex vertices[] = nullptr)
	{
		static const QuadVertex whole[4] = {
			{ { -1.f, -1.f, 0.f }, { 0.f, 0.f } },
			{ {  1.f, -1.f, 0.f }, { 1.f, 0.f } },
			{ { -1.f,  1.f, 0.f }, { 0.f, 1.f } },
			{ {  1.f,  1.f, 0.f }, { 1.f, 1.f } },
		};
		vk_buffer_upload(&b, sizeof(QuadVertex) * 4, vertices != nullptr ? vertices : whole, 0);
	}

private:
	vk_buffer b;
};

class QuadPipeline
{
public:
	QuadPipeline() { vk_quad_pipeline_construct(&p); }
	~QuadPipeline() { vk_quad_pipeline_term(&p); }
	QuadPipeline(const QuadPipeline&) = delete;
	QuadPipeline& operator=(const QuadPipeline&) = delete;
	/* @samples: of the subpass's attachments (SSAA) */
	void Init(ShaderManager *shaderManager, vk::RenderPass renderPass, int subpass = 0, u32 samples = 1)
	{
		if (!vk_quad_pipeline_init(&p, shaderManager->Raw(), (VkRenderPass)renderPass, subpass, samples))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_quad_pipeline_init");
	}
	void Term() { vk_quad_pipeline_term(&p); }
	void Forget() { vk_quad_pipeline_forget(&p); }
	void BindPipeline(vk::CommandBuffer commandBuffer, bool alpha = false)
	{
		const VkPipeline pipeline = vk_quad_pipeline_get(&p, alpha);
		if (pipeline == VK_NULL_HANDLE)
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateGraphicsPipelines");
		vkCmdBindPipeline((VkCommandBuffer)commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	}
	vk::DescriptorSetLayout GetDescSetLayout() const { return vk::DescriptorSetLayout(p.descSetLayout); }
	vk::PipelineLayout GetPipelineLayout() const { return vk::PipelineLayout(p.pipelineLayout); }
	vk::Sampler GetLinearSampler() const { return vk::Sampler(p.linearSampler); }
	vk::Sampler GetNearestSampler() const { return vk::Sampler(p.nearestSampler); }

	vk_quad_pipeline p;
};

class QuadDrawer
{
public:
	QuadDrawer() { memset(&d, 0, sizeof(d)); }
	~QuadDrawer() { vk_quad_drawer_term(&d); }
	QuadDrawer(const QuadDrawer&) = delete;
	QuadDrawer& operator=(const QuadDrawer&) = delete;
	/* its sets and buffer given back */
	void Term() { vk_quad_drawer_term(&d); }
	void Init(QuadPipeline *pipeline)
	{
		if (!vk_quad_drawer_init(&d, &pipeline->p))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_quad_drawer_init");
	}
	/* commandPool: the pool commandBuffer is of. A descriptor set that has to
	 * be replaced is left with it, to be destroyed once the frames that can
	 * be using it are done. */
	template<typename Pool>
	void Draw(vk::CommandBuffer commandBuffer, vk::ImageView imageView, QuadVertex vertices[], bool nearestFilter, Pool *commandPool)
	{
		vk_quad_drawer_draw(&d, (VkCommandBuffer)commandBuffer, (VkImageView)imageView, vertices, nearestFilter, &commandPool->p);
	}

private:
	vk_quad_drawer d;
};
