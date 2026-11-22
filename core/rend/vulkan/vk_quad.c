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
#include <stddef.h>
#include <string.h>
#include <retro_miscellaneous.h>
#include "vk_quad.h"

#define COLOR_ALL (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)

void vk_quad_input_state(VkPipelineVertexInputStateCreateInfo *info, bool uv)
{
	static const VkVertexInputBindingDescription bindings[] = {
		{ 0, sizeof(QuadVertex), VK_VERTEX_INPUT_RATE_VERTEX },
	};
	static const VkVertexInputAttributeDescription attributes[] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(QuadVertex, pos) },	/* pos */
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(QuadVertex, uv) },	/* tex coord */
	};
	memset(info, 0, sizeof(*info));
	info->sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	info->vertexBindingDescriptionCount = ARRAY_SIZE(bindings);
	info->pVertexBindingDescriptions = bindings;
	info->vertexAttributeDescriptionCount = ARRAY_SIZE(attributes) - (uv ? 0 : 1);
	info->pVertexAttributeDescriptions = attributes;
}

/* The states of a pipeline over the framebuffer: viewport and scissor
 * dynamic, no depth */
typedef struct quad_states
{
	VkPipelineInputAssemblyStateCreateInfo inputAssembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depthStencil;
	VkPipelineColorBlendAttachmentState blendAttachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic;
} quad_states;

static void quad_states_init(quad_states *s, VkPrimitiveTopology topology, u32 samples)
{
	memset(s, 0, sizeof(*s));
	s->inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	s->inputAssembly.topology = topology;
	s->viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	s->viewport.viewportCount = 1;
	s->viewport.scissorCount = 1;
	s->rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	s->rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	s->rasterization.cullMode = VK_CULL_MODE_NONE;
	s->rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	s->rasterization.lineWidth = 1.0f;
	s->multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	s->multisample.rasterizationSamples = (VkSampleCountFlagBits)samples;
	s->depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	s->depthStencil.depthCompareOp = VK_COMPARE_OP_NEVER;
	s->depthStencil.front.compareOp = VK_COMPARE_OP_NEVER;
	s->depthStencil.back.compareOp = VK_COMPARE_OP_NEVER;
	s->blendAttachment.colorWriteMask = COLOR_ALL;
	s->blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	s->blend.logicOp = VK_LOGIC_OP_NO_OP;
	s->blend.attachmentCount = 1;
	s->blend.pAttachments = &s->blendAttachment;
	s->dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
}

static VkPipeline quad_create(vk_quad_pipeline *p, bool alpha, VkPipelineCache cache)
{
	static const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
	VkPipelineVertexInputStateCreateInfo vertexInput;
	VkPipelineShaderStageCreateInfo stages[2];
	VkGraphicsPipelineCreateInfo info;
	quad_states s;
	const VkBlendFactor src = alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_CONSTANT_ALPHA;
	const VkBlendFactor dst = alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;

	vk_quad_input_state(&vertexInput, true);
	quad_states_init(&s, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, p->samples);
	s.blendAttachment.blendEnable = VK_TRUE;
	s.blendAttachment.srcColorBlendFactor = src;
	s.blendAttachment.dstColorBlendFactor = dst;
	s.blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	s.blendAttachment.srcAlphaBlendFactor = src;
	s.blendAttachment.dstAlphaBlendFactor = dst;
	s.blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	s.blend.blendConstants[0] = s.blend.blendConstants[1] = s.blend.blendConstants[2] = s.blend.blendConstants[3] = 1.0f;
	s.dynamic.dynamicStateCount = ARRAY_SIZE(dynamicStates);
	s.dynamic.pDynamicStates = dynamicStates;

	memset(stages, 0, sizeof(stages));
	stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = p->modules->m[RSPV_QUAD_VERT];
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = p->modules->m[RSPV_QUAD_FRAG];
	stages[1].pName = "main";

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &s.inputAssembly;
	info.pViewportState = &s.viewport;
	info.pRasterizationState = &s.rasterization;
	info.pMultisampleState = &s.multisample;
	info.pDepthStencilState = &s.depthStencil;
	info.pColorBlendState = &s.blend;
	info.pDynamicState = &s.dynamic;
	info.layout = p->pipelineLayout;
	info.renderPass = p->renderPass;
	info.subpass = (u32)p->subpass;
	return vk_pso_create(vkc.device, cache, &info);
}

/* the pipeline thread's: @key 1 the alpha-blended one */
static VkPipeline quad_build(vk_pso_builder *self, u64 key, VkPipelineCache cache)
{
	return quad_create((vk_quad_pipeline *)self, key != 0, cache);
}

void vk_quad_pipeline_construct(vk_quad_pipeline *p)
{
	memset(p, 0, sizeof(*p));
	p->builder.build = quad_build;
	retro_atomic_int_init(&p->builder.generation, 0);
	p->samples = 1;
	PsoOwnInit(&p->builder, p->slots, 2);
}

static void quad_drop(vk_quad_pipeline *p)
{
	PsoOwnDrop(&p->builder, p->slots, 2, vkc.device);
}

static VkSampler make_sampler(VkFilter filter, VkSamplerMipmapMode mipmap, VkSamplerAddressMode address)
{
	VkSamplerCreateInfo info;
	VkSampler sampler = VK_NULL_HANDLE;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.magFilter = info.minFilter = filter;
	info.mipmapMode = mipmap;
	info.addressModeU = info.addressModeV = info.addressModeW = address;
	info.maxAnisotropy = 16.0f;
	info.compareOp = VK_COMPARE_OP_NEVER;
	info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
	if (vkCreateSampler(vkc.device, &info, NULL, &sampler) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return sampler;
}

bool vk_quad_pipeline_init(vk_quad_pipeline *p, vk_raster_modules *modules, VkRenderPass renderPass, int subpass, u32 samples)
{
	p->modules = modules;
	if (!vk_raster_modules_make(modules, RSPV_TRI_VERT_G0, RSPV_PREPASS_FRAG_T1_G1_R2))
		return false;
	if (p->pipelineLayout == VK_NULL_HANDLE)
	{
		VkDescriptorSetLayoutBinding binding;
		VkDescriptorSetLayoutCreateInfo setInfo;
		VkPipelineLayoutCreateInfo layoutInfo;
		memset(&binding, 0, sizeof(binding));
		binding.binding = 0;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;	/* texture */
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		memset(&setInfo, 0, sizeof(setInfo));
		setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		setInfo.bindingCount = 1;
		setInfo.pBindings = &binding;
		if (vkCreateDescriptorSetLayout(vkc.device, &setInfo, NULL, &p->descSetLayout) != VK_SUCCESS)
		{
			p->descSetLayout = VK_NULL_HANDLE;
			return false;
		}
		memset(&layoutInfo, 0, sizeof(layoutInfo));
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts = &p->descSetLayout;
		if (vkCreatePipelineLayout(vkc.device, &layoutInfo, NULL, &p->pipelineLayout) != VK_SUCCESS)
		{
			p->pipelineLayout = VK_NULL_HANDLE;
			return false;
		}
	}
	/* What is drawn with the linear one is a whole picture at another size:
	 * the screen as it was before the renderer changed size, or before
	 * the frontend took the context away, under a render that covers
	 * part of it. Past its edge there is nothing: the edge is repeated.
	 * With a black border there instead, a picture made larger had its
	 * outermost pixels mixed with black - a dark line around the screen
	 * that stayed for as long as the game did not draw there. */
	if (p->linearSampler == VK_NULL_HANDLE
			&& (p->linearSampler = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
					VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)) == VK_NULL_HANDLE)
		return false;
	if (p->nearestSampler == VK_NULL_HANDLE
			&& (p->nearestSampler = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
					VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)) == VK_NULL_HANDLE)
		return false;
	if (p->renderPass != renderPass || p->subpass != subpass || p->samples != samples)
	{
		PsoSlot *both[2];
		quad_drop(p);
		p->renderPass = renderPass;
		p->subpass = subpass;
		p->samples = samples;
		/* (both asked for now, not when first drawn: what the VMUs and the OSD are drawn with, the last picture) */
		both[0] = &p->slots[0];
		both[1] = &p->slots[1];
		vk_pso_worker_build_soon(both, 2, vkc.pipelineCache);
	}
	return true;
}

void vk_quad_pipeline_forget(vk_quad_pipeline *p)
{
	quad_drop(p);
	p->renderPass = VK_NULL_HANDLE;
}

void vk_quad_pipeline_term(vk_quad_pipeline *p)
{
	vk_quad_pipeline_forget(p);
	if (p->linearSampler != VK_NULL_HANDLE)
		vkDestroySampler(vkc.device, p->linearSampler, NULL);
	if (p->nearestSampler != VK_NULL_HANDLE)
		vkDestroySampler(vkc.device, p->nearestSampler, NULL);
	if (p->pipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(vkc.device, p->pipelineLayout, NULL);
	if (p->descSetLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(vkc.device, p->descSetLayout, NULL);
	p->linearSampler = p->nearestSampler = VK_NULL_HANDLE;
	p->pipelineLayout = VK_NULL_HANDLE;
	p->descSetLayout = VK_NULL_HANDLE;
}

VkPipeline vk_quad_pipeline_get(vk_quad_pipeline *p, bool alpha)
{
	PsoSlot *slot = &p->slots[alpha ? 1 : 0];
	const VkPipeline made = vk_pso_get(slot);
	if (made != VK_NULL_HANDLE)
		return made;
	/* (the pipeline thread could not make it: made here, and the draw fails if this cannot either) */
	return PsoOwnMade(slot, quad_create(p, alpha, vkc.pipelineCache));
}

static void free_slot_sets(vk_quad_slot *slot)
{
	unsigned i;
	for (i = 0; i < VK_QUAD_SETS; i++)
		vkc_free_desc_set(slot->set[i]);
	memset(slot, 0, sizeof(*slot));
}

bool vk_quad_drawer_init(vk_quad_drawer *d, vk_quad_pipeline *pipeline)
{
	unsigned i;
	vk_quad_drawer_term(d);
	d->pipeline = pipeline;
	for (i = 0; i < VK_QUAD_SLOTS; i++)
		memset(&d->slots[i], 0, sizeof(d->slots[i]));
	return vk_buffer_init(&d->buffer, sizeof(QuadVertex) * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == VK_SUCCESS;
}

void vk_quad_drawer_term(vk_quad_drawer *d)
{
	unsigned i;
	if (d->pipeline == NULL)
		return;
	for (i = 0; i < VK_QUAD_SLOTS; i++)
		free_slot_sets(&d->slots[i]);
	vk_buffer_free(&d->buffer);
	d->pipeline = NULL;
}

void vk_quad_drawer_draw(vk_quad_drawer *d, VkCommandBuffer cmd, VkImageView view, const QuadVertex *vertices,
		bool nearestFilter, vk_cmd_pool *pool)
{
	static const QuadVertex whole[4] = {
		{ { -1.f, -1.f, 0.f }, { 0.f, 0.f } },
		{ {  1.f, -1.f, 0.f }, { 1.f, 0.f } },
		{ { -1.f,  1.f, 0.f }, { 0.f, 1.f } },
		{ {  1.f,  1.f, 0.f }, { 1.f, 1.f } },
	};
	vk_quad_slot *slot = &d->slots[vkc_current_image_index() % VK_QUAD_SLOTS];
	const VkSampler sampler = nearestFilter ? d->pipeline->nearestSampler : d->pipeline->linearSampler;
	const u64 now = pool->frames;
	u64 inFlight = (u64)(pool->slotCount > (u32)vkc.swapChainSize ? pool->slotCount : (u32)vkc.swapChainSize);
	const VkDeviceSize zero = 0;
	if (inFlight == 0)
		inFlight = 1;

	/* The set says which image to draw and how to sample it, and that is the
	 * same from one frame to the next, so it is written when it changes and
	 * not every time. A set in use must not be written - the frame before
	 * can still be on its way through the GPU with it, the frontend's frame
	 * index not moving when the game renders twice in one run - so the
	 * picture changed is written into another of the slot's sets, one not
	 * drawn with in the frames the command pool has not waited for yet (a
	 * frame's fence is waited for when its place in the ring comes round:
	 * vk_cmd_pool_begin_frame()). Only with none of them free is one given
	 * back to the pool, once its frames are done, and another made. */
	if (slot->set[slot->current] == VK_NULL_HANDLE || slot->view != view || slot->sampler != sampler)
	{
		VkDescriptorImageInfo imageInfo;
		VkWriteDescriptorSet write;
		u32 i, k = VK_QUAD_SETS;
		for (i = 0; i < VK_QUAD_SETS && k == VK_QUAD_SETS; i++)
			if (i != slot->current && (slot->set[i] == VK_NULL_HANDLE || slot->used[i] + inFlight <= now))
				k = i;
		if (k == VK_QUAD_SETS)
		{
			/* (the one drawn with longest ago) */
			k = slot->current == 0 ? 1 : 0;
			for (i = 0; i < VK_QUAD_SETS; i++)
				if (i != slot->current && slot->used[i] < slot->used[k])
					k = i;
			vk_cmd_pool_defer_set(pool, slot->set[k], vkc.descriptorPool);
			slot->set[k] = VK_NULL_HANDLE;
		}
		if (slot->set[k] == VK_NULL_HANDLE)
			slot->set[k] = vkc_alloc_desc_set(d->pipeline->descSetLayout);
		slot->current = k;
		slot->view = view;
		slot->sampler = sampler;
		imageInfo.sampler = sampler;
		imageInfo.imageView = view;
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		memset(&write, 0, sizeof(write));
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = slot->set[k];
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &imageInfo;
		vkUpdateDescriptorSets(vkc.device, 1, &write, 0, NULL);
	}
	slot->used[slot->current] = now;
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipeline->pipelineLayout, 0, 1,
			&slot->set[slot->current], 0, NULL);

	vk_buffer_upload(&d->buffer, sizeof(QuadVertex) * 4, vertices != NULL ? vertices : whole, 0);
	vkCmdBindVertexBuffers(cmd, 0, 1, &d->buffer.buffer, &zero);
	vkCmdDraw(cmd, 4, 1, 0, 0);
}

static VkPipeline resolve_create(vk_ssaa_resolve *r, VkPipelineCache cache)
{
	static const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkSpecializationMapEntry entries[3];
	VkSpecializationInfo specialization;
	s32 values[3];
	u32 log2n = 0;
	VkPipelineVertexInputStateCreateInfo vertexInput;
	VkPipelineShaderStageCreateInfo stages[2];
	VkGraphicsPipelineCreateInfo info;
	quad_states s;

	/* (shaders/ssaa_resolve.*: the count of samples is a specialization constant) */
	while ((2u << log2n) <= r->samples)
		log2n++;
	values[0] = 0;
	values[1] = (s32)r->samples;
	values[2] = (s32)log2n;
	entries[0].constantID = PSO_SC_KEY;
	entries[0].offset = 0;
	entries[0].size = sizeof(s32);
	entries[1].constantID = PSO_SC_SSAA_SAMPLES;
	entries[1].offset = sizeof(s32);
	entries[1].size = sizeof(s32);
	entries[2].constantID = PSO_SC_SSAA_LOG2N;
	entries[2].offset = 2 * sizeof(s32);
	entries[2].size = sizeof(s32);
	specialization.mapEntryCount = 3;
	specialization.pMapEntries = entries;
	specialization.dataSize = sizeof(values);
	specialization.pData = values;

	memset(&vertexInput, 0, sizeof(vertexInput));
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	quad_states_init(&s, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1);
	s.blendAttachment.srcColorBlendFactor = s.blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
	s.blendAttachment.srcAlphaBlendFactor = s.blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	s.dynamic.dynamicStateCount = ARRAY_SIZE(dynamicStates);
	s.dynamic.pDynamicStates = dynamicStates;

	memset(stages, 0, sizeof(stages));
	stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = r->vertexShader;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = r->fragmentShader;
	stages[1].pName = "main";
	stages[1].pSpecializationInfo = &specialization;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &s.inputAssembly;
	info.pViewportState = &s.viewport;
	info.pRasterizationState = &s.rasterization;
	info.pMultisampleState = &s.multisample;
	info.pDepthStencilState = &s.depthStencil;
	info.pColorBlendState = &s.blend;
	info.pDynamicState = &s.dynamic;
	info.layout = r->pipelineLayout;
	info.renderPass = r->renderPass;
	info.subpass = r->subpass;
	return vk_pso_create(vkc.device, cache, &info);
}

static VkPipeline resolve_build(vk_pso_builder *self, u64 key, VkPipelineCache cache)
{
	(void)key;
	return resolve_create((vk_ssaa_resolve *)self, cache);
}

void vk_ssaa_resolve_construct(vk_ssaa_resolve *r)
{
	memset(r, 0, sizeof(*r));
	r->builder.build = resolve_build;
	retro_atomic_int_init(&r->builder.generation, 0);
	PsoOwnInit(&r->builder, &r->slot, 1);
}

static void resolve_write(vk_ssaa_resolve *r, VkImageView samplesView)
{
	VkDescriptorImageInfo imageInfo;
	VkWriteDescriptorSet write;
	imageInfo.sampler = VK_NULL_HANDLE;
	imageInfo.imageView = samplesView;
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = r->descSet;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
	write.pImageInfo = &imageInfo;
	vkUpdateDescriptorSets(vkc.device, 1, &write, 0, NULL);
}

bool vk_ssaa_resolve_init(vk_ssaa_resolve *r, VkRenderPass renderPass, u32 subpass, u32 samples, VkImageView samplesView)
{
	PsoSlot *one = &r->slot;
	PsoOwnDrop(&r->builder, &r->slot, 1, vkc.device);
	if (r->setLayout == VK_NULL_HANDLE)
	{
		VkDescriptorSetLayoutBinding binding;
		VkDescriptorSetLayoutCreateInfo setInfo;
		VkPipelineLayoutCreateInfo layoutInfo;
		memset(&binding, 0, sizeof(binding));
		binding.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		memset(&setInfo, 0, sizeof(setInfo));
		setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		setInfo.bindingCount = 1;
		setInfo.pBindings = &binding;
		if (vkCreateDescriptorSetLayout(vkc.device, &setInfo, NULL, &r->setLayout) != VK_SUCCESS)
		{
			r->setLayout = VK_NULL_HANDLE;
			return false;
		}
		memset(&layoutInfo, 0, sizeof(layoutInfo));
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts = &r->setLayout;
		if (vkCreatePipelineLayout(vkc.device, &layoutInfo, NULL, &r->pipelineLayout) != VK_SUCCESS)
		{
			r->pipelineLayout = VK_NULL_HANDLE;
			return false;
		}
		r->vertexShader = vk_raster_module(RSPV_SSAA_RESOLVE_VERT);
		r->fragmentShader = vk_raster_module(RSPV_SSAA_RESOLVE_FRAG);
		if (r->vertexShader == VK_NULL_HANDLE || r->fragmentShader == VK_NULL_HANDLE)
			return false;
	}
	r->renderPass = renderPass;
	r->subpass = subpass;
	r->samples = samples;
	if (r->descSet == VK_NULL_HANDLE)
		r->descSet = vkc_alloc_desc_set(r->setLayout);
	resolve_write(r, samplesView);
	vk_pso_worker_build_soon(&one, 1, vkc.pipelineCache);
	return true;
}

bool vk_ssaa_resolve_draw(vk_ssaa_resolve *r, VkCommandBuffer cmd, u32 width, u32 height)
{
	VkViewport viewport;
	VkRect2D scissor;
	VkPipeline pipeline = vk_pso_get(&r->slot);
	/* (the pipeline thread could not make it: made here, or failed for as init() did) */
	if (pipeline == VK_NULL_HANDLE)
		pipeline = PsoOwnMade(&r->slot, resolve_create(r, vkc.pipelineCache));
	if (pipeline == VK_NULL_HANDLE)
		return false;
	viewport.x = viewport.y = 0.f;
	viewport.width = (float)width;
	viewport.height = (float)height;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;
	scissor.offset.x = scissor.offset.y = 0;
	scissor.extent.width = width;
	scissor.extent.height = height;
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipelineLayout, 0, 1, &r->descSet, 0, NULL);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	return true;
}

void vk_ssaa_resolve_set_samples_view(vk_ssaa_resolve *r, VkImageView samplesView, vk_cmd_pool *pool)
{
	if (r->descSet != VK_NULL_HANDLE)
		vk_cmd_pool_defer_set(pool, r->descSet, vkc.descriptorPool);
	r->descSet = vkc_alloc_desc_set(r->setLayout);
	resolve_write(r, samplesView);
}

void vk_ssaa_resolve_term(vk_ssaa_resolve *r)
{
	PsoOwnDrop(&r->builder, &r->slot, 1, vkc.device);
	vkc_free_desc_set(r->descSet);
	r->descSet = VK_NULL_HANDLE;
	if (r->pipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(vkc.device, r->pipelineLayout, NULL);
	if (r->setLayout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(vkc.device, r->setLayout, NULL);
	if (r->vertexShader != VK_NULL_HANDLE)
		vkDestroyShaderModule(vkc.device, r->vertexShader, NULL);
	if (r->fragmentShader != VK_NULL_HANDLE)
		vkDestroyShaderModule(vkc.device, r->fragmentShader, NULL);
	r->pipelineLayout = VK_NULL_HANDLE;
	r->setLayout = VK_NULL_HANDLE;
	r->vertexShader = r->fragmentShader = VK_NULL_HANDLE;
	r->samples = 0;
}
