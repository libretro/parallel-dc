/*
 *  Created on: Oct 3, 2019

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
/* The C++ renderers' view of vk_pipeline.h, until they are C too */
#include "vulkan.h"
#include "shaders.h"
#include "texture.h"
#include "utils.h"
#include "hw/pvr/ta_ctx.h"
#include "vulkan_context.h"
#include "ssaa.h"
#include "pso.h"
#include "desc_cache.h"
#include "vk_pipeline.h"

class DescriptorSets
{
public:
	DescriptorSets() { vk_desc_sets_construct(&d); }
	~DescriptorSets() { vk_desc_sets_term(&d); }
	DescriptorSets(const DescriptorSets&) = delete;
	DescriptorSets& operator=(const DescriptorSets&) = delete;
	/* (the frame slots' are in a vector: moved, the sets go with it) */
	DescriptorSets(DescriptorSets&& o) noexcept { d = o.d; vk_desc_sets_construct(&o.d); }
	DescriptorSets& operator=(DescriptorSets&& o) noexcept
	{
		if (this != &o)
		{
			vk_desc_sets_term(&d);
			d = o.d;
			vk_desc_sets_construct(&o.d);
		}
		return *this;
	}

	void Init(SamplerManager* samplerManager, vk::PipelineLayout pipelineLayout, vk::DescriptorSetLayout perFrameLayout, vk::DescriptorSetLayout perPolyLayout)
	{
		vk_desc_sets_init(&d, samplerManager->Raw(), (VkPipelineLayout)pipelineLayout, (VkDescriptorSetLayout)perFrameLayout,
				(VkDescriptorSetLayout)perPolyLayout);
	}
	void UpdateUniforms(vk::Buffer vertexUniformBuffer, vk::Buffer buffer, u32 fragmentUniformOffset, vk::ImageView fogImageView, vk::ImageView paletteImageView)
	{
		vk_desc_sets_update_uniforms(&d, (VkBuffer)vertexUniformBuffer, (VkBuffer)buffer, fragmentUniformOffset,
				(VkImageView)fogImageView, (VkImageView)paletteImageView);
	}
	vk::DescriptorSet SetTexture(u64 textureId, TSP tsp) { return vk::DescriptorSet(vk_desc_sets_set_texture(&d, textureId, tsp.full)); }
	void BindPerFrameDescriptorSets(vk::CommandBuffer cmdBuffer) { vk_desc_sets_bind_per_frame(&d, (VkCommandBuffer)cmdBuffer); }
	void BindPerPolyDescriptorSets(vk::CommandBuffer cmdBuffer, vk::DescriptorSet set)
	{
		vk_desc_sets_bind_per_poly(&d, (VkCommandBuffer)cmdBuffer, (VkDescriptorSet)set);
	}
	void Reset() { vk_desc_sets_reset(&d); }

private:
	vk_desc_sets d;
};

class PipelineManager
{
public:
	PipelineManager() { vk_pipeline_mgr_construct(&m, 1); }
	~PipelineManager() { vk_pipeline_mgr_term(&m); }
	PipelineManager(const PipelineManager&) = delete;
	PipelineManager& operator=(const PipelineManager&) = delete;

	void Init(ShaderManager *shaderManager, vk::RenderPass renderPass, u32 samples = 1)
	{
		if (!vk_pipeline_mgr_init(&m, shaderManager->Raw(), (VkRenderPass)renderPass, samples))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_pipeline_mgr_init");
	}

	enum class ShadowPass { None = VK_SHADOW_NONE, LaterWins = VK_SHADOW_LATER_WINS, EarlierWins = VK_SHADOW_EARLIER_WINS };

	PsoChoice GetPipeline(u32 listType, bool sortTriangles, const PolyParam& pp, ShadowPass shadowPass = ShadowPass::None)
	{
		return vk_pipeline_mgr_get(&m, listType, sortTriangles, &pp, (int)shadowPass);
	}
	void SetDynamicState(vk::CommandBuffer cmdBuffer, u64 key) const
	{
		vk_pipeline_mgr_set_dynamic_state(&m, (VkCommandBuffer)cmdBuffer, key);
	}
	vk::Pipeline GetModifierVolumePipeline(ModVolMode mode, int cullMode)
	{
		return vk::Pipeline(vk_pipeline_mgr_modvol(&m, (int)mode, (u32)cullMode));
	}
	void (VKAPI_PTR *CullSetter() const)(VkCommandBuffer, VkCullModeFlags) { return m.cullSetter; }
	vk::Pipeline GetPrepassPipeline(const PolyParam& pp) { return vk::Pipeline(vk_pipeline_mgr_prepass(&m, &pp)); }
	void Reset() { vk_pipeline_mgr_reset(&m); }

	vk::PipelineLayout GetPipelineLayout() const { return vk::PipelineLayout(m.pipelineLayout); }
	vk::DescriptorSetLayout GetPerFrameDSLayout() const { return vk::DescriptorSetLayout(m.perFrameLayout); }
	vk::DescriptorSetLayout GetPerPolyDSLayout() const { return vk::DescriptorSetLayout(m.perPolyLayout); }
	vk::RenderPass GetRenderPass() const { return vk::RenderPass(m.renderPass); }
	vk_pipeline_mgr *Raw() { return &m; }

protected:
	vk_pipeline_mgr m;
};

/* renders to a texture's: a render pass of their own */
class RttPipelineManager : public PipelineManager
{
public:
	RttPipelineManager() { m.who = 2; }
	void Init(ShaderManager *shaderManager, u32 samples = 1)
	{
		if (!vk_pipeline_mgr_rtt_init(&m, shaderManager->Raw(), samples))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_pipeline_mgr_rtt_init");
	}
};

static inline vk::UniqueRenderPass MakeSsaaRenderPass(vk::Format colorFormat, u32 samples, vk::ImageLayout pictureLayout,
		vk::Format hdrFormat = vk::Format::eUndefined)
{
	const VkRenderPass renderPass = vk_make_ssaa_render_pass((VkFormat)colorFormat, samples, (VkImageLayout)pictureLayout, (VkFormat)hdrFormat);
	if (renderPass == VK_NULL_HANDLE)
		throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateRenderPass");
	return vk::UniqueRenderPass(vk::RenderPass(renderPass), vk::ObjectDestroy<vk::Device, vk::DispatchLoaderStatic>(vk::Device(vkc.device)));
}
