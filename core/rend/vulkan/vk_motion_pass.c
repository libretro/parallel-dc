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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <retro_miscellaneous.h>
#include "vk_motion_pass.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta_words.h"
#include "rend/upscaler.h"

#define VECTORS_USAGE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)

static VkPipeline make(const vk_motion_pass *m, int cull, VkPipelineCache cache)
{
	/* the vertex as the colour pass has it, and where it was: its own stream */
	static const VkVertexInputBindingDescription bindings[] = {
		{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX },
		{ 1, sizeof(float) * 2, VK_VERTEX_INPUT_RATE_VERTEX },
	};
	static const VkVertexInputAttributeDescription attributes[] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, x) },
		{ 1, 1, VK_FORMAT_R32G32_SFLOAT, 0 },
	};
	static const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineVertexInputStateCreateInfo vertexInput;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depthStencil;
	VkPipelineColorBlendAttachmentState blendAttachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic;
	VkSpecializationMapEntry entries[2];
	VkSpecializationInfo specialization;
	VkPipelineShaderStageCreateInfo stages[2];
	VkGraphicsPipelineCreateInfo info;
	/* (the vectors in pixels: the size, as the shader's constants) */
	float sizeConstants[2];

	sizeConstants[0] = (float)m->size.width;
	sizeConstants[1] = (float)m->size.height;
	memset(&vertexInput, 0, sizeof(vertexInput));
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount = ARRAY_SIZE(bindings);
	vertexInput.pVertexBindingDescriptions = bindings;
	vertexInput.vertexAttributeDescriptionCount = ARRAY_SIZE(attributes);
	vertexInput.pVertexAttributeDescriptions = attributes;
	memset(&inputAssembly, 0, sizeof(inputAssembly));
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	/* the surface the colour pass left: its depth, equal or nearer (reversed); the sampled mode's shader does the test */
	memset(&depthStencil, 0, sizeof(depthStencil));
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = m->depthTest;
	depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
	memset(&blendAttachment, 0, sizeof(blendAttachment));
	blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
	blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.logicOp = VK_LOGIC_OP_NO_OP;
	blend.attachmentCount = 1;
	blend.pAttachments = &blendAttachment;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = ARRAY_SIZE(dynamicStates);
	dynamic.pDynamicStates = dynamicStates;
	entries[0].constantID = 10;
	entries[0].offset = 0;
	entries[0].size = sizeof(float);
	entries[1].constantID = 11;
	entries[1].offset = sizeof(float);
	entries[1].size = sizeof(float);
	specialization.mapEntryCount = 2;
	specialization.pMapEntries = entries;
	specialization.dataSize = sizeof(sizeConstants);
	specialization.pData = sizeConstants;
	memset(stages, 0, sizeof(stages));
	stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = m->vertexShader;
	stages[0].pName = "main";
	stages[0].pSpecializationInfo = &specialization;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = m->fragmentShader;
	stages[1].pName = "main";
	memset(&rasterization, 0, sizeof(rasterization));
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = cull == 2 ? VK_CULL_MODE_BACK_BIT : cull == 1 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.f;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &inputAssembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &rasterization;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = m->depthTest ? &depthStencil : NULL;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = m->pipelineLayout;
	info.renderPass = m->renderPass;
	return vk_pso_create(vkc.device, cache, &info);
}

/* the pipeline thread's: @key the cull mode's */
static VkPipeline build(vk_pso_builder *self, u64 key, VkPipelineCache cache)
{
	return make((const vk_motion_pass *)self, (int)key, cache);
}

void vk_motion_pass_construct(vk_motion_pass *m)
{
	memset(m, 0, sizeof(*m));
	m->builder.build = build;
	retro_atomic_int_init(&m->builder.generation, 0);
	PsoOwnInit(&m->builder, m->pipelines, 3);
}

static void free_sized(void *object)
{
	vk_motion_sized *s = (vk_motion_sized *)object;
	u32 i;
	for (i = 0; i < s->count; i++)
	{
		if (s->framebuffers[i] != VK_NULL_HANDLE)
			vkDestroyFramebuffer(vkc.device, s->framebuffers[i], NULL);
		vk_attachment_free(&s->attachments[i]);
	}
	free(s);
}

void vk_motion_pass_term(vk_motion_pass *m, vk_cmd_pool *pool)
{
	u32 i;
	if (m->sized != NULL)
	{
		if (pool != NULL)
			vk_cmd_pool_defer(pool, m->sized, free_sized);
		else
			free_sized(m->sized);
		m->sized = NULL;
	}
	/* (the pipeline thread done with what it has of them before what they are made of goes) */
	PsoOwnDrop(&m->builder, m->pipelines, 3, vkc.device);
	if (m->renderPass != VK_NULL_HANDLE)
		vkDestroyRenderPass(vkc.device, m->renderPass, NULL);
	m->renderPass = VK_NULL_HANDLE;
	for (i = 0; i < m->prevXyCount; i++)
		vk_buffer_free(&m->prevXy[i]);
	free(m->prevXy);
	m->prevXy = NULL;
	m->prevXyCount = 0;
	free(m->sets);
	m->sets = NULL;
	m->setCount = 0;
	if (m->descriptorPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(vkc.device, m->descriptorPool, NULL);
	m->descriptorPool = VK_NULL_HANDLE;
	m->sampled = false;
	m->recorded = false;
	if (pool == NULL)
	{
		/* (the renderer's end: the sampled mode's layout too) */
		if (m->depthSampler != VK_NULL_HANDLE)
			vkDestroySampler(vkc.device, m->depthSampler, NULL);
		if (m->ownLayout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(vkc.device, m->ownLayout, NULL);
		if (m->setLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(vkc.device, m->setLayout, NULL);
		m->depthSampler = VK_NULL_HANDLE;
		m->ownLayout = VK_NULL_HANDLE;
		m->setLayout = VK_NULL_HANDLE;
	}
}

static bool make_render_pass(vk_motion_pass *m, const VkAttachmentDescription *attachments, u32 count,
		const VkAttachmentReference *depthReference, const VkSubpassDependency *dependencies)
{
	VkAttachmentReference colorReference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo info;
	colorReference.attachment = 0;
	colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorReference;
	subpass.pDepthStencilAttachment = depthReference;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = count;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = dependencies;
	if (vkCreateRenderPass(vkc.device, &info, NULL, &m->renderPass) != VK_SUCCESS)
	{
		m->renderPass = VK_NULL_HANDLE;
		return false;
	}
	return true;
}

static void dependency(VkSubpassDependency *d, u32 src, u32 dst, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
		VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
	memset(d, 0, sizeof(*d));
	d->srcSubpass = src;
	d->dstSubpass = dst;
	d->srcStageMask = srcStage;
	d->dstStageMask = dstStage;
	d->srcAccessMask = srcAccess;
	d->dstAccessMask = dstAccess;
}

/* the vectors, two halves a pixel, sampled and copied from by the upscaler
 * as it likes; their framebuffers with @depthViews, if there are */
static bool make_sized(vk_motion_pass *m, const VkImageView *depthViews, u32 count)
{
	vk_motion_sized *s;
	u32 i;
	if (m->sized != NULL && m->sized->count >= count)
		return true;
	s = (vk_motion_sized *)calloc(1, sizeof(vk_motion_sized) + count * (sizeof(vk_attachment) + sizeof(VkFramebuffer)));
	if (s == NULL)
		return false;
	s->attachments = (vk_attachment *)(s + 1);
	s->framebuffers = (VkFramebuffer *)(s->attachments + count);
	s->count = count;
	if (m->sized != NULL)
		free_sized(m->sized);
	m->sized = s;
	for (i = 0; i < count; i++)
	{
		VkFramebufferCreateInfo info;
		VkImageView views[2];
		if (!vk_attachment_init(&s->attachments[i], m->size.width, m->size.height, VK_FORMAT_R16G16_SFLOAT, VECTORS_USAGE,
				false, VK_SAMPLE_COUNT_1_BIT))
			return false;
		views[0] = s->attachments[i].view;
		views[1] = depthViews != NULL && !m->sampled ? depthViews[i] : VK_NULL_HANDLE;
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		info.renderPass = m->renderPass;
		info.attachmentCount = m->sampled ? 1 : 2;
		info.pAttachments = views;
		info.width = m->size.width;
		info.height = m->size.height;
		info.layers = 1;
		if (vkCreateFramebuffer(vkc.device, &info, NULL, &s->framebuffers[i]) != VK_SUCCESS)
		{
			s->framebuffers[i] = VK_NULL_HANDLE;
			return false;
		}
	}
	return true;
}

static void make_pipelines(vk_motion_pass *m, VkPipelineLayout layout, VkShaderModule vertexShader,
		VkShaderModule fragmentShader, bool depthTest)
{
	PsoSlot *all[3];
	m->pipelineLayout = layout;
	m->vertexShader = vertexShader;
	m->fragmentShader = fragmentShader;
	m->depthTest = depthTest;
	all[0] = &m->pipelines[0];
	all[1] = &m->pipelines[1];
	all[2] = &m->pipelines[2];
	vk_pso_worker_build_soon(all, 3, vkc.pipelineCache);
}

bool vk_motion_pass_init(vk_motion_pass *m, VkExtent2D size, VkFormat depthFormat, VkImageLayout depthLayout,
		const VkImageView *depthViews, u32 count, VkPipelineLayout pipelineLayout,
		VkShaderModule vertexShader, VkShaderModule fragmentShader, vk_cmd_pool *pool)
{
	if (size.width != m->size.width || size.height != m->size.height || depthLayout != m->depthLayout
			|| (m->sized != NULL ? m->sized->count : 0) != count || m->sampled)
		vk_motion_pass_term(m, pool);
	m->size = size;
	m->depthLayout = depthLayout;
	if (m->renderPass == VK_NULL_HANDLE)
	{
		VkAttachmentDescription attachments[2];
		VkAttachmentReference depthReference;
		VkSubpassDependency dependencies[2];
		memset(attachments, 0, sizeof(attachments));
		attachments[0].format = VK_FORMAT_R16G16_SFLOAT;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		/* (the depth as the frame left it, tested, not written, and left as it was) */
		attachments[1].format = depthFormat;
		attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[1].initialLayout = depthLayout;
		attachments[1].finalLayout = depthLayout;
		depthReference.attachment = 1;
		depthReference.layout = depthLayout;
		/* the frame's depth, drawn, then tested here */
		dependency(&dependencies[0], VK_SUBPASS_EXTERNAL, 0,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
		/* the vectors, and the depth, read by the upscaler */
		dependency(&dependencies[1], 0, VK_SUBPASS_EXTERNAL,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		if (!make_render_pass(m, attachments, 2, &depthReference, dependencies))
			return false;
	}
	if (!make_sized(m, depthViews, count))
		return false;
	if (retro_atomic_load_relaxed_int(&m->pipelines[0].state) == PSO_EMPTY)
		make_pipelines(m, pipelineLayout, vertexShader, fragmentShader, true);
	return true;
}

bool vk_motion_pass_init_sampled(vk_motion_pass *m, VkExtent2D size, const VkImageView *depthViews, u32 count,
		const VkBuffer *uniformBuffers, VkDeviceSize uniformBytes,
		VkShaderModule vertexShader, VkShaderModule fragmentShader, vk_cmd_pool *pool)
{
	u32 i;
	if (size.width != m->size.width || size.height != m->size.height || !m->sampled
			|| (m->sized != NULL ? m->sized->count : 0) != count)
		vk_motion_pass_term(m, pool);
	m->size = size;
	m->depthLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	m->sampled = true;
	if (m->renderPass == VK_NULL_HANDLE)
	{
		/* the vectors alone: the depth is sampled */
		VkAttachmentDescription attachment;
		VkSubpassDependency dependencies[2];
		memset(&attachment, 0, sizeof(attachment));
		attachment.format = VK_FORMAT_R16G16_SFLOAT;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		/* the frame's depth, stored by the kernel, then sampled here */
		dependency(&dependencies[0], VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		/* the vectors, read by the upscaler */
		dependency(&dependencies[1], 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		if (!make_render_pass(m, &attachment, 1, NULL, dependencies))
			return false;
	}
	if (m->setLayout == VK_NULL_HANDLE)
	{
		/* the vertex uniforms as position.glsl has them, and the depth */
		VkDescriptorSetLayoutBinding bindings[2];
		VkDescriptorSetLayoutCreateInfo setInfo;
		VkPipelineLayoutCreateInfo layoutInfo;
		VkSamplerCreateInfo samplerInfo;
		memset(bindings, 0, sizeof(bindings));
		bindings[0].binding = 0;
		bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		bindings[0].descriptorCount = 1;
		bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		bindings[1].binding = 1;
		bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[1].descriptorCount = 1;
		bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		memset(&setInfo, 0, sizeof(setInfo));
		setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		setInfo.bindingCount = 2;
		setInfo.pBindings = bindings;
		if (vkCreateDescriptorSetLayout(vkc.device, &setInfo, NULL, &m->setLayout) != VK_SUCCESS)
		{
			m->setLayout = VK_NULL_HANDLE;
			return false;
		}
		memset(&layoutInfo, 0, sizeof(layoutInfo));
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts = &m->setLayout;
		if (vkCreatePipelineLayout(vkc.device, &layoutInfo, NULL, &m->ownLayout) != VK_SUCCESS)
		{
			m->ownLayout = VK_NULL_HANDLE;
			return false;
		}
		memset(&samplerInfo, 0, sizeof(samplerInfo));
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.compareOp = VK_COMPARE_OP_NEVER;
		samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
		if (vkCreateSampler(vkc.device, &samplerInfo, NULL, &m->depthSampler) != VK_SUCCESS)
		{
			m->depthSampler = VK_NULL_HANDLE;
			return false;
		}
	}
	if (m->setCount < count)
	{
		VkDescriptorPoolSize sizes[2];
		VkDescriptorPoolCreateInfo poolInfo;
		VkDescriptorSetAllocateInfo allocInfo;
		VkDescriptorSetLayout *layouts;
		VkResult res;
		sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		sizes[0].descriptorCount = count;
		sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		sizes[1].descriptorCount = count;
		memset(&poolInfo, 0, sizeof(poolInfo));
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.maxSets = count;
		poolInfo.poolSizeCount = 2;
		poolInfo.pPoolSizes = sizes;
		if (m->descriptorPool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(vkc.device, m->descriptorPool, NULL);
		free(m->sets);
		m->setCount = 0;
		m->sets = (VkDescriptorSet *)calloc(count, sizeof(VkDescriptorSet));
		layouts = (VkDescriptorSetLayout *)malloc(count * sizeof(VkDescriptorSetLayout));
		if (m->sets == NULL || layouts == NULL
				|| vkCreateDescriptorPool(vkc.device, &poolInfo, NULL, &m->descriptorPool) != VK_SUCCESS)
		{
			m->descriptorPool = VK_NULL_HANDLE;
			free(layouts);
			return false;
		}
		for (i = 0; i < count; i++)
			layouts[i] = m->setLayout;
		memset(&allocInfo, 0, sizeof(allocInfo));
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = m->descriptorPool;
		allocInfo.descriptorSetCount = count;
		allocInfo.pSetLayouts = layouts;
		res = vkAllocateDescriptorSets(vkc.device, &allocInfo, m->sets);
		free(layouts);
		if (res != VK_SUCCESS)
			return false;
		m->setCount = count;
		for (i = 0; i < count; i++)
		{
			VkDescriptorBufferInfo bufferInfo;
			VkDescriptorImageInfo imageInfo;
			VkWriteDescriptorSet writes[2];
			bufferInfo.buffer = uniformBuffers[i];
			bufferInfo.offset = 0;
			bufferInfo.range = uniformBytes;
			imageInfo.sampler = m->depthSampler;
			imageInfo.imageView = depthViews[i];
			imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			memset(writes, 0, sizeof(writes));
			writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[0].dstSet = writes[1].dstSet = m->sets[i];
			writes[0].dstBinding = 0;
			writes[0].descriptorCount = 1;
			writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			writes[0].pBufferInfo = &bufferInfo;
			writes[1].dstBinding = 1;
			writes[1].descriptorCount = 1;
			writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[1].pImageInfo = &imageInfo;
			vkUpdateDescriptorSets(vkc.device, 2, writes, 0, NULL);
		}
	}
	if (!make_sized(m, NULL, count))
		return false;
	if (retro_atomic_load_relaxed_int(&m->pipelines[0].state) == PSO_EMPTY)
		make_pipelines(m, m->ownLayout, vertexShader, fragmentShader, false);
	return true;
}

bool vk_motion_pass_begin(vk_motion_pass *m, int image)
{
	size_t bytes;
	vk_buffer *buffer;
	m->recorded = false;
	if (pvrrc.gpu || m->sized == NULL || m->sized->count == 0)
		return false;
	/* the frame's strips matched with the frame before's: where each vertex was */
	bytes = (size_t)TA_LIST_USED(pvrrc.verts, Vertex) * 2 * sizeof(float);
	if (bytes < 1024)
		bytes = 1024;
	if ((u32)image >= m->prevXyCount)
	{
		vk_buffer *grown = (vk_buffer *)realloc(m->prevXy, (image + 1) * sizeof(vk_buffer));
		if (grown == NULL)
			return false;
		memset(grown + m->prevXyCount, 0, (image + 1 - m->prevXyCount) * sizeof(vk_buffer));
		m->prevXy = grown;
		m->prevXyCount = image + 1;
	}
	buffer = &m->prevXy[image];
	if (buffer->buffer == VK_NULL_HANDLE || buffer->size < bytes)
	{
		vk_buffer_free(buffer);
		if (vk_buffer_init(buffer, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
			return false;
	}
	/* (written where the pass reads them: the buffer, mapped) */
	upscaler_match(&pvrrc, (float *)vk_buffer_map(buffer));
	m->recorded = true;
	return true;
}

/* (one the pipeline thread could not make: made here, or failed for as init did) */
static VkPipeline pipeline(vk_motion_pass *m, int cull)
{
	const VkPipeline made = vk_pso_get(&m->pipelines[cull]);
	if (made != VK_NULL_HANDLE)
		return made;
	return PsoOwnMade(&m->pipelines[cull], make(m, cull, vkc.pipelineCache));
}

bool vk_motion_pass_record(vk_motion_pass *m, VkCommandBuffer cmd, int image, VkBuffer vertexBuffer, VkDeviceSize indexOffset,
		const VkRect2D *scissor, VkDeviceSize vertexOffset)
{
	VkClearValue clearValues[2];
	VkRenderPassBeginInfo begin;
	VkViewport viewport;
	VkBuffer buffers[2];
	VkDeviceSize offsets[2];
	int bound = -1, l, i;
	bool made = true;

	memset(clearValues, 0, sizeof(clearValues));
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass = m->renderPass;
	begin.framebuffer = m->sized->framebuffers[image];
	begin.renderArea.extent = m->size;
	begin.clearValueCount = m->sampled ? 1 : 2;
	begin.pClearValues = clearValues;
	vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	viewport.x = viewport.y = 0.f;
	viewport.width = (float)m->size.width;
	viewport.height = (float)m->size.height;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, scissor);
	if (m->sampled)
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m->ownLayout, 0, 1, &m->sets[image], 0, NULL);
	buffers[0] = vertexBuffer;
	buffers[1] = m->prevXy[image].buffer;
	offsets[0] = vertexOffset;
	offsets[1] = 0;
	vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
	vkCmdBindIndexBuffer(cmd, vertexBuffer, indexOffset, VK_INDEX_TYPE_UINT32);
	for (l = 0; l < 2; l++)
	{
		const PolyParam *pp = l == 0 ? TA_LIST_HEAD(pvrrc.global_param_op, PolyParam) : TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam);
		const int used = l == 0 ? TA_LIST_USED(pvrrc.global_param_op, PolyParam) : TA_LIST_USED(pvrrc.global_param_pt, PolyParam);
		for (i = 0; i < used; i++, pp++)
		{
			u32 mode;
			int cull;
			if (pp->count < 3)
				continue;
			mode = TAW_GET(TAW_FULL(pp->isp), ISP_TSP_CullMode);
			cull = mode == 3 ? 2 : mode == 2 ? 1 : 0;
			if (cull != bound)
			{
				const VkPipeline p = pipeline(m, cull);
				if (p == VK_NULL_HANDLE)
				{
					made = false;
					break;
				}
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
				bound = cull;
			}
			vkCmdDrawIndexed(cmd, pp->count, 1, pp->first, 0, 0);
		}
	}
	vkCmdEndRenderPass(cmd);
	return made;
}

void vk_motion_pass_submit(vk_motion_pass *m, int image, VkImage depthImage, VkImageView depthView, VkFormat depthFormat)
{
	struct retro_hw_upscaler_image motion, depth;
	motion.image  = (uint64_t)m->sized->attachments[image].image;
	motion.view   = (uint64_t)m->sized->attachments[image].view;
	motion.format = (unsigned)VK_FORMAT_R16G16_SFLOAT;
	motion.state  = (unsigned)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	motion.width  = m->size.width;
	motion.height = m->size.height;
	depth.image   = (uint64_t)depthImage;
	depth.view    = (uint64_t)depthView;
	depth.format  = (unsigned)depthFormat;
	depth.state   = (unsigned)m->depthLayout;
	depth.width   = m->size.width;
	depth.height  = m->size.height;
	/* (the depth is 1/w: nearer is greater) */
	upscaler_submit(&motion, &depth, RETRO_HW_UPSCALER_DEPTH_INVERTED);
	m->recorded = false;
}
