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
#include "motion_pass.h"
#include "vulkan_context.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/transform_matrix.h"
#include <algorithm>

void MotionVectorPass::Init(vk::Extent2D size, vk::Format depthFormat, vk::ImageLayout depthLayout,
		const std::vector<vk::ImageView>& depthViews, vk::PipelineLayout pipelineLayout,
		vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader, CommandPool *commandPool)
{
	VulkanContext *context = VulkanContext::Instance();
	vk::Device device = context->GetDevice();
	if (size != this->size || depthLayout != this->depthLayout || attachments.size() != depthViews.size() || sampled)
		Term(commandPool);
	this->size = size;
	this->depthLayout = depthLayout;
	if (!renderPass)
	{
		vk::AttachmentDescription attachmentDescriptions[] = {
				vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), vk::Format::eR16G16Sfloat, vk::SampleCountFlagBits::e1,
						vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
						vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
						vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal),
				// (the depth as the frame left it, tested, not written, and left as it was)
				vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), depthFormat, vk::SampleCountFlagBits::e1,
						vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore,
						vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
						depthLayout, depthLayout),
		};
		vk::AttachmentReference colorReference(0, vk::ImageLayout::eColorAttachmentOptimal);
		vk::AttachmentReference depthReference(1, depthLayout);
		vk::SubpassDescription subpass(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
				0, nullptr, 1, &colorReference, nullptr, &depthReference);
		vk::SubpassDependency dependencies[] = {
				// the frame's depth, drawn, then tested here
				vk::SubpassDependency(VK_SUBPASS_EXTERNAL, 0,
						vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
						vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
						vk::AccessFlagBits::eDepthStencilAttachmentWrite, vk::AccessFlagBits::eDepthStencilAttachmentRead),
				// the vectors, and the depth, read by the upscaler
				vk::SubpassDependency(0, VK_SUBPASS_EXTERNAL,
						vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
						vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
						vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
						vk::AccessFlagBits::eShaderRead),
		};
		renderPass = device.createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(),
				ARRAY_SIZE(attachmentDescriptions), attachmentDescriptions, 1, &subpass,
				ARRAY_SIZE(dependencies), dependencies));
	}
	while (attachments.size() < depthViews.size())
	{
		// the vectors, two halves a pixel, sampled and copied from by the upscaler as it likes
		attachments.push_back(std::unique_ptr<FramebufferAttachment>(
				new FramebufferAttachment(context->GetPhysicalDevice(), device)));
		attachments.back()->Init(size.width, size.height, vk::Format::eR16G16Sfloat,
				vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
				| vk::ImageUsageFlagBits::eTransferSrc, false);
		const size_t i = framebuffers.size();
		vk::ImageView views[] = { attachments.back()->GetImageView(), depthViews[i] };
		framebuffers.push_back(device.createFramebufferUnique(vk::FramebufferCreateInfo(vk::FramebufferCreateFlags(),
				*renderPass, ARRAY_SIZE(views), views, size.width, size.height, 1)));
	}
	if (retro_atomic_load_relaxed_int(&pipelines[0].state) == PSO_EMPTY)
		MakePipelines(pipelineLayout, vertexShader, fragmentShader, true);
}

void MotionVectorPass::InitSampled(vk::Extent2D size, const std::vector<vk::ImageView>& depthViews,
		const std::vector<vk::Buffer>& uniformBuffers, vk::DeviceSize uniformBytes,
		vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader, CommandPool *commandPool)
{
	VulkanContext *context = VulkanContext::Instance();
	vk::Device device = context->GetDevice();
	if (size != this->size || !sampled || attachments.size() != depthViews.size())
		Term(commandPool);
	this->size = size;
	this->depthLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	sampled = true;
	if (!renderPass)
	{
		// the vectors alone: the depth is sampled
		vk::AttachmentDescription attachmentDescription(vk::AttachmentDescriptionFlags(), vk::Format::eR16G16Sfloat, vk::SampleCountFlagBits::e1,
				vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
				vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
				vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal);
		vk::AttachmentReference colorReference(0, vk::ImageLayout::eColorAttachmentOptimal);
		vk::SubpassDescription subpass(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
				0, nullptr, 1, &colorReference, nullptr, nullptr);
		vk::SubpassDependency dependencies[] = {
				// the frame's depth, stored by the kernel, then sampled here
				vk::SubpassDependency(VK_SUBPASS_EXTERNAL, 0,
						vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eFragmentShader,
						vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead),
				// the vectors, read by the upscaler
				vk::SubpassDependency(0, VK_SUBPASS_EXTERNAL,
						vk::PipelineStageFlagBits::eColorAttachmentOutput,
						vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
						vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead),
		};
		renderPass = device.createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(),
				1, &attachmentDescription, 1, &subpass, ARRAY_SIZE(dependencies), dependencies));
	}
	if (!setLayout)
	{
		// the vertex uniforms as position.glsl has them, and the depth
		vk::DescriptorSetLayoutBinding bindings[] = {
				{ 0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex },
				{ 1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment },
		};
		setLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), ARRAY_SIZE(bindings), bindings));
		ownLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), 1, &setLayout.get()));
		depthSampler = device.createSamplerUnique(vk::SamplerCreateInfo(vk::SamplerCreateFlags(), vk::Filter::eNearest, vk::Filter::eNearest,
				vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
				vk::SamplerAddressMode::eClampToEdge));
	}
	if (sets.size() < depthViews.size())
	{
		const u32 count = (u32)depthViews.size();
		const vk::DescriptorPoolSize sizes[] = {
				{ vk::DescriptorType::eUniformBuffer, count },
				{ vk::DescriptorType::eCombinedImageSampler, count },
		};
		descriptorPool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlags(), count, ARRAY_SIZE(sizes), sizes));
		std::vector<vk::DescriptorSetLayout> layouts(count, *setLayout);
		sets = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptorPool, count, layouts.data()));
		std::vector<vk::DescriptorBufferInfo> bufferInfos;
		std::vector<vk::DescriptorImageInfo> imageInfos;
		std::vector<vk::WriteDescriptorSet> writes;
		bufferInfos.reserve(count);
		imageInfos.reserve(count);
		for (u32 i = 0; i < count; i++)
		{
			bufferInfos.push_back(vk::DescriptorBufferInfo(uniformBuffers[i], 0, uniformBytes));
			writes.push_back(vk::WriteDescriptorSet(sets[i], 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &bufferInfos.back()));
			imageInfos.push_back(vk::DescriptorImageInfo(*depthSampler, depthViews[i], vk::ImageLayout::eShaderReadOnlyOptimal));
			writes.push_back(vk::WriteDescriptorSet(sets[i], 1, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfos.back()));
		}
		device.updateDescriptorSets(writes, nullptr);
	}
	while (attachments.size() < depthViews.size())
	{
		attachments.push_back(std::unique_ptr<FramebufferAttachment>(
				new FramebufferAttachment(context->GetPhysicalDevice(), device)));
		attachments.back()->Init(size.width, size.height, vk::Format::eR16G16Sfloat,
				vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
				| vk::ImageUsageFlagBits::eTransferSrc, false);
		vk::ImageView views[] = { attachments.back()->GetImageView() };
		framebuffers.push_back(device.createFramebufferUnique(vk::FramebufferCreateInfo(vk::FramebufferCreateFlags(),
				*renderPass, 1, views, size.width, size.height, 1)));
	}
	if (retro_atomic_load_relaxed_int(&pipelines[0].state) == PSO_EMPTY)
		MakePipelines(*ownLayout, vertexShader, fragmentShader, false);
}

void MotionVectorPass::MakePipelines(vk::PipelineLayout pipelineLayout, vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader, bool depthTest)
{
	this->pipelineLayout = pipelineLayout;
	this->vertexShader = vertexShader;
	this->fragmentShader = fragmentShader;
	this->depthTest = depthTest;
	PsoSlot *all[] = { &pipelines[0], &pipelines[1], &pipelines[2] };
	VkPsoWorker::Instance().BuildSoon(all, 3, VulkanContext::Instance()->GetPipelineCache());
}

VkPipeline MotionVectorPass::Make(int cull, vk::PipelineCache cache, bool orThrow) const
{
	vk::Device device = VulkanContext::Instance()->GetDevice();
	// the vertex as the colour pass has it, and where it was: its own stream
	static const vk::VertexInputBindingDescription bindings[] = {
			{ 0, sizeof(Vertex) },
			{ 1, sizeof(float) * 2 },
	};
	static const vk::VertexInputAttributeDescription vertexAttributes[] = {
			vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, x)),
			vk::VertexInputAttributeDescription(1, 1, vk::Format::eR32G32Sfloat, 0),
	};
	vk::PipelineVertexInputStateCreateInfo vertexInput(vk::PipelineVertexInputStateCreateFlags(),
			ARRAY_SIZE(bindings), bindings, ARRAY_SIZE(vertexAttributes), vertexAttributes);
	vk::PipelineInputAssemblyStateCreateInfo inputAssembly(vk::PipelineInputAssemblyStateCreateFlags(), vk::PrimitiveTopology::eTriangleStrip);
	vk::PipelineViewportStateCreateInfo viewportState(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);
	vk::PipelineMultisampleStateCreateInfo multisample;
	// the surface the colour pass left: its depth, equal or nearer (reversed); the sampled mode's shader does the test
	vk::PipelineDepthStencilStateCreateInfo depthStencil(vk::PipelineDepthStencilStateCreateFlags(),
			depthTest, false, vk::CompareOp::eGreaterOrEqual, false, false);
	vk::PipelineColorBlendAttachmentState blendAttachment(false,
			vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
			vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
			vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG);
	vk::PipelineColorBlendStateCreateInfo blend(vk::PipelineColorBlendStateCreateFlags(), false, vk::LogicOp::eNoOp, 1, &blendAttachment);
	vk::DynamicState dynamicStates[] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo dynamic(vk::PipelineDynamicStateCreateFlags(), ARRAY_SIZE(dynamicStates), dynamicStates);
	// (the vectors in pixels: the size, as the shader's constants)
	const float sizeConstants[] = { (float)size.width, (float)size.height };
	const vk::SpecializationMapEntry entries[] = {
			vk::SpecializationMapEntry(10, 0, sizeof(float)),
			vk::SpecializationMapEntry(11, sizeof(float), sizeof(float)),
	};
	vk::SpecializationInfo specialization(ARRAY_SIZE(entries), entries, sizeof(sizeConstants), sizeConstants);
	vk::PipelineShaderStageCreateInfo stages[] = {
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertexShader, "main", &specialization },
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragmentShader, "main" },
	};
	vk::PipelineRasterizationStateCreateInfo rasterization(vk::PipelineRasterizationStateCreateFlags(), false, false,
			vk::PolygonMode::eFill,
			cull == 2 ? vk::CullModeFlagBits::eBack : cull == 1 ? vk::CullModeFlagBits::eFront : vk::CullModeFlagBits::eNone,
			vk::FrontFace::eCounterClockwise, false, 0.f, 0.f, 0.f, 1.f);
	vk::GraphicsPipelineCreateInfo createInfo(vk::PipelineCreateFlags(), ARRAY_SIZE(stages), stages,
			&vertexInput, &inputAssembly, nullptr, &viewportState, &rasterization, &multisample,
			depthTest ? &depthStencil : nullptr, &blend, &dynamic, pipelineLayout, *renderPass, 0);
	if (!orThrow)
		return PsoCreate(device, cache, createInfo);
	return (VkPipeline)device.createGraphicsPipelineUnique(cache, createInfo).release();
}

// (one the pipeline thread could not make: made here, or thrown for as Init() did)
vk::Pipeline MotionVectorPass::Pipeline(int cull)
{
	const VkPipeline made = PsoGet(pipelines[cull]);
	if (made != VK_NULL_HANDLE)
		return vk::Pipeline(made);
	return vk::Pipeline(PsoOwnMade(pipelines[cull], Make(cull, VulkanContext::Instance()->GetPipelineCache(), true)));
}

void MotionVectorPass::Term(CommandPool *commandPool)
{
	if (commandPool != nullptr)
	{
		for (auto& attachment : attachments)
			if (attachment)
				commandPool->DeferDelete(std::move(attachment));
		for (auto& framebuffer : framebuffers)
			if (framebuffer)
				commandPool->DeferDelete(std::unique_ptr<vk::UniqueFramebuffer>(new vk::UniqueFramebuffer(std::move(framebuffer))));
	}
	attachments.clear();
	framebuffers.clear();
	// (the pipeline thread done with what it has of them before what they are made of goes)
	PsoOwnDrop(this, pipelines, 3, VulkanContext::Instance() != nullptr ? VulkanContext::Instance()->GetDevice() : vk::Device());
	renderPass.reset();
	prevXyBuffers.clear();
	sets.clear();
	descriptorPool.reset();
	sampled = false;
	recorded = false;
}

bool MotionVectorPass::Begin(int image)
{
	recorded = false;
	if (pvrrc.gpu || framebuffers.empty())
		return false;
	// the frame's strips matched with the frame before's: where each vertex was
	upscaler_match(pvrrc, prevXy);
	const u32 bytes = (u32)std::max<size_t>(prevXy.size() * sizeof(float), 1024);
	while (prevXyBuffers.size() <= (size_t)image)
		prevXyBuffers.emplace_back();
	std::unique_ptr<BufferData>& buffer = prevXyBuffers[image];
	if (!buffer || buffer->bufferSize < bytes)
		buffer = std::unique_ptr<BufferData>(new BufferData(bytes, vk::BufferUsageFlagBits::eVertexBuffer));
	if (!prevXy.empty())
		buffer->upload((u32)(prevXy.size() * sizeof(float)), prevXy.data());
	recorded = true;
	return true;
}

void MotionVectorPass::Record(vk::CommandBuffer commandBuffer, int image, vk::Buffer vertexBuffer, vk::DeviceSize indexOffset,
		const vk::Rect2D& scissor, vk::DeviceSize vertexOffset)
{
	const vk::ClearValue clearValues[] = { vk::ClearColorValue(std::array<float, 4> { 0.f, 0.f, 0.f, 0.f }), vk::ClearDepthStencilValue { 0.f, 0 } };
	commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(*renderPass, *framebuffers[image],
			vk::Rect2D( { 0, 0 }, size), sampled ? 1u : (u32)ARRAY_SIZE(clearValues), clearValues), vk::SubpassContents::eInline);
	commandBuffer.setViewport(0, vk::Viewport(0.0f, 0.0f, (float)size.width, (float)size.height, 0.0f, 1.0f));
	commandBuffer.setScissor(0, scissor);
	if (sampled)
		commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *ownLayout, 0, 1, &sets[image], 0, nullptr);
	const vk::Buffer buffers[] = { vertexBuffer, *prevXyBuffers[image]->buffer };
	const vk::DeviceSize offsets[] = { vertexOffset, 0 };
	commandBuffer.bindVertexBuffers(0, ARRAY_SIZE(buffers), buffers, offsets);
	commandBuffer.bindIndexBuffer(vertexBuffer, indexOffset, vk::IndexType::eUint32);
	int bound = -1;
	const List<PolyParam> *lists[] = { &pvrrc.global_param_op, &pvrrc.global_param_pt };
	for (const List<PolyParam> *list : lists)
		for (int i = 0; i < list->used(); i++)
		{
			const PolyParam& pp = list->head()[i];
			if (pp.count < 3)
				continue;
			const int cull = pp.isp.CullMode == 3 ? 2 : pp.isp.CullMode == 2 ? 1 : 0;
			if (cull != bound)
			{
				commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline(cull));
				bound = cull;
			}
			commandBuffer.drawIndexed(pp.count, 1, pp.first, 0, 0);
		}
	commandBuffer.endRenderPass();
}

void MotionVectorPass::Submit(int image, vk::Image depthImage, vk::ImageView depthView, vk::Format depthFormat)
{
	struct retro_hw_upscaler_image motion, depth;
	motion.image  = (uint64_t)(VkImage)attachments[image]->GetImage();
	motion.view   = (uint64_t)(VkImageView)attachments[image]->GetImageView();
	motion.format = (unsigned)VK_FORMAT_R16G16_SFLOAT;
	motion.state  = (unsigned)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	motion.width  = size.width;
	motion.height = size.height;
	depth.image   = (uint64_t)(VkImage)depthImage;
	depth.view    = (uint64_t)(VkImageView)depthView;
	depth.format  = (unsigned)(VkFormat)depthFormat;
	depth.state   = (unsigned)(VkImageLayout)depthLayout;
	depth.width   = size.width;
	depth.height  = size.height;
	// (the depth is 1/w: nearer is greater)
	upscaler_submit(motion, depth, RETRO_HW_UPSCALER_DEPTH_INVERTED);
	recorded = false;
}
