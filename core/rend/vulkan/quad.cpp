#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
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
#include "quad.h"
#include "vulkan_context.h"
#include "commandpool.h"
#include "ssaa.h"
#include "rend/ssaa_pattern.h"

static VulkanContext *GetContext()
{
	return VulkanContext::Instance();
}

vk::PipelineVertexInputStateCreateInfo GetQuadInputStateCreateInfo(bool uv)
{
	// Vertex input state
	static const vk::VertexInputBindingDescription vertexBindingDescriptions[] =
	{
			{ 0, sizeof(QuadVertex) },
	};
	static const vk::VertexInputAttributeDescription vertexInputAttributeDescriptions[] =
	{
			vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(QuadVertex, pos)),	// pos
			vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32Sfloat, offsetof(QuadVertex, uv)),		// tex coord
	};
	return vk::PipelineVertexInputStateCreateInfo(
			vk::PipelineVertexInputStateCreateFlags(),
			ARRAY_SIZE(vertexBindingDescriptions),
			vertexBindingDescriptions,
			ARRAY_SIZE(vertexInputAttributeDescriptions) - (uv ? 0 : 1),
			vertexInputAttributeDescriptions);
}

QuadPipeline::QuadPipeline()
{
	PsoOwnInit(this, slots, 2);
}

VkPipeline QuadPipeline::BuildPipeline(u64 key, vk::PipelineCache cache)
{
	return Create(key != 0, cache, false);
}

vk::Pipeline QuadPipeline::MakeHere(bool alpha)
{
	return vk::Pipeline(PsoOwnMade(slots[alpha ? 1 : 0], Create(alpha, GetContext()->GetPipelineCache(), true)));
}

void QuadPipeline::Drop()
{
	PsoOwnDrop(this, slots, 2, GetContext() != nullptr ? GetContext()->GetDevice() : vk::Device());
}

/* @orThrow: as vulkan.hpp does when the device cannot make it, else
 * VK_NULL_HANDLE */
VkPipeline QuadPipeline::Create(bool alpha, vk::PipelineCache cache, bool orThrow)
{
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo = GetQuadInputStateCreateInfo(true);

	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(), vk::PrimitiveTopology::eTriangleStrip);

	// Viewport and scissor states
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);

	// Rasterization and multisample states
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo;
	pipelineRasterizationStateCreateInfo.lineWidth = 1.0;
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;
	pipelineMultisampleStateCreateInfo.rasterizationSamples = (vk::SampleCountFlagBits)samples;

	// Depth and stencil
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo;

	// Color flags and blending
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState(
			true,								// blendEnable
			alpha ? vk::BlendFactor::eSrcAlpha : vk::BlendFactor::eConstantAlpha,	// srcColorBlendFactor
			alpha ? vk::BlendFactor::eOneMinusSrcAlpha : vk::BlendFactor::eOneMinusConstantAlpha, // dstColorBlendFactor
			vk::BlendOp::eAdd,					// colorBlendOp
			alpha ? vk::BlendFactor::eSrcAlpha : vk::BlendFactor::eConstantAlpha,	// srcAlphaBlendFactor
					alpha ? vk::BlendFactor::eOneMinusSrcAlpha : vk::BlendFactor::eOneMinusConstantAlpha, // dstAlphaBlendFactor
			vk::BlendOp::eAdd,					// alphaBlendOp
			vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
						| vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
	);
	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo
	(
	  vk::PipelineColorBlendStateCreateFlags(),   // flags
	  false,                                      // logicOpEnable
	  vk::LogicOp::eNoOp,                         // logicOp
	  1,                                          // attachmentCount
	  &pipelineColorBlendAttachmentState,         // pAttachments
	  { { 1.0f, 1.0f, 1.0f, 1.0f } }              // blendConstants
	);

	vk::DynamicState dynamicStates[] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor, vk::DynamicState::eBlendConstants };
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(vk::PipelineDynamicStateCreateFlags(), ARRAY_SIZE(dynamicStates),
			dynamicStates);

	vk::PipelineShaderStageCreateInfo stages[] = {
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, shaderManager->GetQuadVertexShader(), "main" },
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, shaderManager->GetQuadFragmentShader(), "main" },
	};
	vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo
	(
	  vk::PipelineCreateFlags(),                  // flags
	  2,                                          // stageCount
	  stages,                                     // pStages
	  &pipelineVertexInputStateCreateInfo,        // pVertexInputState
	  &pipelineInputAssemblyStateCreateInfo,      // pInputAssemblyState
	  nullptr,                                    // pTessellationState
	  &pipelineViewportStateCreateInfo,           // pViewportState
	  &pipelineRasterizationStateCreateInfo,      // pRasterizationState
	  &pipelineMultisampleStateCreateInfo,        // pMultisampleState
	  &pipelineDepthStencilStateCreateInfo,       // pDepthStencilState
	  &pipelineColorBlendStateCreateInfo,         // pColorBlendState
	  &pipelineDynamicStateCreateInfo,            // pDynamicState
	  *pipelineLayout,                            // layout
	  renderPass,                                 // renderPass
	  subpass                                     // subpass
	);

	if (!orThrow)
		return PsoCreate(GetContext()->GetDevice(), cache, graphicsPipelineCreateInfo);
	return (VkPipeline)GetContext()->GetDevice().createGraphicsPipelineUnique(cache, graphicsPipelineCreateInfo).release();
}

void QuadPipeline::Init(ShaderManager *shaderManager, vk::RenderPass renderPass, int subpass, u32 samples)
{
	this->shaderManager = shaderManager;
	shaderManager->Init();

	if (!pipelineLayout)
	{
		vk::DescriptorSetLayoutBinding bindings[] = {
				{ 0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment }, // texture
		};
		descSetLayout = GetContext()->GetDevice().createDescriptorSetLayoutUnique(
				vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), ARRAY_SIZE(bindings), bindings));
		pipelineLayout = GetContext()->GetDevice().createPipelineLayoutUnique(
				vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), 1, &descSetLayout.get()));
	}
	if (!linearSampler)
	{
		/* What is drawn with this one is a whole picture at another size:
		 * the screen as it was before the renderer changed size, or before
		 * the frontend took the context away, under a render that covers
		 * part of it. Past its edge there is nothing: the edge is repeated.
		 * With a black border there instead, a picture made larger had its
		 * outermost pixels mixed with black - a dark line around the screen
		 * that stayed for as long as the game did not draw there. */
		linearSampler = GetContext()->GetDevice().createSamplerUnique(
				vk::SamplerCreateInfo(vk::SamplerCreateFlags(),
						vk::Filter::eLinear, vk::Filter::eLinear,
						vk::SamplerMipmapMode::eLinear,
						vk::SamplerAddressMode::eClampToEdge,
						vk::SamplerAddressMode::eClampToEdge,
						vk::SamplerAddressMode::eClampToEdge, 0.0f, false,
						16.0f, false, vk::CompareOp::eNever, 0.0f, 0.0f,
						vk::BorderColor::eFloatOpaqueBlack));
	}
	if (!nearestSampler)
	{
		nearestSampler = GetContext()->GetDevice().createSamplerUnique(
				vk::SamplerCreateInfo(vk::SamplerCreateFlags(),
						vk::Filter::eNearest, vk::Filter::eNearest,
						vk::SamplerMipmapMode::eNearest,
						vk::SamplerAddressMode::eClampToBorder,
						vk::SamplerAddressMode::eClampToBorder,
						vk::SamplerAddressMode::eClampToBorder, 0.0f, false,
						16.0f, false, vk::CompareOp::eNever, 0.0f, 0.0f,
						vk::BorderColor::eFloatOpaqueBlack));
	}
	if (this->renderPass != renderPass || this->subpass != subpass || this->samples != samples)
	{
		Drop();
		this->renderPass = renderPass;
		this->subpass = subpass;
		this->samples = samples;
		// (both asked for now, not when first drawn: what the VMUs and the OSD are drawn with, the last picture)
		PsoSlot *both[] = { &slots[0], &slots[1] };
		VkPsoWorker::Instance().BuildSoon(both, 2, GetContext()->GetPipelineCache());
	}
}

void QuadDrawer::Init(QuadPipeline *pipeline)
{
	this->pipeline = pipeline;
	buffer = std::unique_ptr<QuadBuffer>(new QuadBuffer());
	for (Slot& slot : slots)
		slot = Slot();
}

void QuadDrawer::Draw(vk::CommandBuffer commandBuffer, vk::ImageView imageView, QuadVertex vertices[], bool nearestFilter, CommandPool *commandPool)
{
	VulkanContext *context = GetContext();
	Slot& slot = slots[context->GetCurrentImageIndex() % SLOTS];
	const vk::Sampler sampler = nearestFilter ? pipeline->GetNearestSampler() : pipeline->GetLinearSampler();
	const u64 now = commandPool->GetFrames();
	const u64 inFlight = (u64)std::max(std::max(commandPool->GetSize(), (int)context->GetSwapChainSize()), 1);

	/* The set says which image to draw and how to sample it, and that is the
	 * same from one frame to the next, so it is written when it changes and
	 * not every time. A set in use must not be written - the frame before
	 * can still be on its way through the GPU with it, the frontend's frame
	 * index not moving when the game renders twice in one run - so the
	 * picture changed is written into another of the slot's sets, one not
	 * drawn with in the frames the command pool has not waited for yet (a
	 * frame's fence is waited for when its place in the ring comes round:
	 * CommandPool::BeginFrame()). Only with none of them free is one given
	 * back to the pool, once its frames are done, and another made. */
	if (!slot.set[slot.current] || slot.view != imageView || slot.sampler != sampler)
	{
		u32 k = SETS;
		for (u32 i = 0; i < SETS && k == SETS; i++)
			if (i != slot.current && (!slot.set[i] || slot.used[i] + inFlight <= now))
				k = i;
		if (k == SETS)
		{
			// (the one drawn with longest ago)
			k = slot.current == 0 ? 1 : 0;
			for (u32 i = 0; i < SETS; i++)
				if (i != slot.current && slot.used[i] < slot.used[k])
					k = i;
			commandPool->DeferFree(std::move(slot.set[k]), context->GetDescriptorPool());
		}
		if (!slot.set[k])
			slot.set[k] = vk_alloc_desc_set(context->GetDevice(), context->GetDescriptorPool(), pipeline->GetDescSetLayout());
		slot.current = k;
		slot.view = imageView;
		slot.sampler = sampler;
		vk::DescriptorImageInfo imageInfo(sampler, imageView, vk::ImageLayout::eShaderReadOnlyOptimal);
		vk::WriteDescriptorSet write(*slot.set[k], 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo, nullptr, nullptr);
		context->GetDevice().updateDescriptorSets(1, &write, 0, nullptr);
	}
	slot.used[slot.current] = now;
	commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline->GetPipelineLayout(), 0, 1, &slot.set[slot.current].get(), 0, nullptr);

	buffer->Update(vertices);
	buffer->Bind(commandBuffer);
	buffer->Draw(commandBuffer);
}

SsaaResolve::SsaaResolve()
{
	PsoOwnInit(this, &slot, 1);
}

void SsaaResolve::Drop()
{
	PsoOwnDrop(this, &slot, 1, VulkanContext::Instance() != nullptr ? VulkanContext::Instance()->GetDevice() : vk::Device());
}

void SsaaResolve::Init(vk::RenderPass renderPass, u32 subpass, u32 samples, vk::ImageView samplesView)
{
	VulkanContext *context = VulkanContext::Instance();
	vk::Device device = context->GetDevice();
	Drop();
	if (!setLayout)
	{
		vk::DescriptorSetLayoutBinding binding(0, vk::DescriptorType::eInputAttachment, 1, vk::ShaderStageFlagBits::eFragment);
		setLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), 1, &binding));
		pipelineLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), 1, &setLayout.get()));
		// (shaders/ssaa_resolve.*: the count of samples is a specialization constant)
		vertexShader = MakeRasterModule(RSPV_SSAA_RESOLVE_VERT);
		fragmentShader = MakeRasterModule(RSPV_SSAA_RESOLVE_FRAG);
	}
	this->renderPass = renderPass;
	this->subpass = subpass;
	this->samples = samples;
	if (!descSet)
		descSet = vk_alloc_desc_set(device, context->GetDescriptorPool(), setLayout.get());
	vk::DescriptorImageInfo imageInfo(vk::Sampler(), samplesView, vk::ImageLayout::eShaderReadOnlyOptimal);
	vk::WriteDescriptorSet write(*descSet, 0, 0, 1, vk::DescriptorType::eInputAttachment, &imageInfo, nullptr, nullptr);
	device.updateDescriptorSets(write, nullptr);
	PsoSlot *one = &slot;
	VkPsoWorker::Instance().BuildSoon(&one, 1, context->GetPipelineCache());
}

VkPipeline SsaaResolve::BuildPipeline(u64 key, vk::PipelineCache cache)
{
	return Create(cache, false);
}

/* @orThrow: as vulkan.hpp does when the device cannot make it, else
 * VK_NULL_HANDLE */
VkPipeline SsaaResolve::Create(vk::PipelineCache cache, bool orThrow) const
{
	vk::Device device = VulkanContext::Instance()->GetDevice();
	const PsoSpecialization specialization(0, samples);
	vk::PipelineVertexInputStateCreateInfo vertexInput;
	vk::PipelineInputAssemblyStateCreateInfo inputAssembly(vk::PipelineInputAssemblyStateCreateFlags(), vk::PrimitiveTopology::eTriangleList);
	vk::PipelineViewportStateCreateInfo viewportState(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);
	vk::PipelineRasterizationStateCreateInfo rasterization;
	rasterization.lineWidth = 1.0;
	vk::PipelineMultisampleStateCreateInfo multisample;
	vk::PipelineDepthStencilStateCreateInfo depthStencil;
	vk::PipelineColorBlendAttachmentState blendAttachment(false, vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
			vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
			vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
	vk::PipelineColorBlendStateCreateInfo blend(vk::PipelineColorBlendStateCreateFlags(), false, vk::LogicOp::eNoOp, 1, &blendAttachment);
	vk::DynamicState dynamicStates[] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo dynamic(vk::PipelineDynamicStateCreateFlags(), ARRAY_SIZE(dynamicStates), dynamicStates);
	vk::PipelineShaderStageCreateInfo stages[] = {
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, *vertexShader, "main" },
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, *fragmentShader, "main", &specialization.info },
	};
	vk::GraphicsPipelineCreateInfo info(vk::PipelineCreateFlags(), 2, stages, &vertexInput, &inputAssembly, nullptr, &viewportState,
			&rasterization, &multisample, &depthStencil, &blend, &dynamic, *pipelineLayout, renderPass, subpass);
	if (!orThrow)
		return PsoCreate(device, cache, info);
	return (VkPipeline)device.createGraphicsPipelineUnique(cache, info).release();
}

void SsaaResolve::Draw(vk::CommandBuffer cmd, vk::Extent2D extent) const
{
	VkPipeline pipeline = PsoGet(slot);
	// (the pipeline thread could not make it: made here, or thrown for as Init() did)
	if (pipeline == VK_NULL_HANDLE)
		pipeline = PsoOwnMade(slot, Create(VulkanContext::Instance()->GetPipelineCache(), true));
	cmd.setViewport(0, vk::Viewport(0.f, 0.f, (float)extent.width, (float)extent.height, 0.f, 1.f));
	cmd.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), extent));
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, vk::Pipeline(pipeline));
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipelineLayout, 0, 1, &descSet.get(), 0, nullptr);
	cmd.draw(3, 1, 0, 0);
}

void SsaaResolve::SetSamplesView(vk::ImageView samplesView, CommandPool *commandPool)
{
	VulkanContext *context = VulkanContext::Instance();
	if (descSet)
		commandPool->DeferFree(std::move(descSet), context->GetDescriptorPool());
	descSet = vk_alloc_desc_set(context->GetDevice(), context->GetDescriptorPool(), setLayout.get());
	vk::DescriptorImageInfo imageInfo(vk::Sampler(), samplesView, vk::ImageLayout::eShaderReadOnlyOptimal);
	vk::WriteDescriptorSet write(*descSet, 0, 0, 1, vk::DescriptorType::eInputAttachment, &imageInfo, nullptr, nullptr);
	context->GetDevice().updateDescriptorSets(write, nullptr);
}
