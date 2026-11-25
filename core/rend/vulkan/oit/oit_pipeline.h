/*
    Created on: Nov 6, 2019

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
/* The C++ renderers' view of vk_oit_pipeline.h, until they are C too */
#include "../vulkan.h"
#include "oit_shaders.h"
#include "oit_renderpass.h"
#include "oit_buffer.h"
#include "../texture.h"
#include "../ssaa.h"
#include "../pso.h"
#include "../desc_cache.h"
#include "hw/pvr/ta_ctx.h"
#include "vk_oit_pipeline.h"

class OITDescriptorSets
{
public:
	// std140 alignment required (the per-triangle renderer's: shaders.h)
	typedef ::VertexShaderUniforms VertexShaderUniforms;
	typedef ::FragmentShaderUniforms FragmentShaderUniforms;
	typedef vk_oit_push_constants PushConstants;

	OITDescriptorSets() { vk_oit_desc_sets_construct(&d); }
	~OITDescriptorSets() { vk_oit_desc_sets_term(&d); }
	OITDescriptorSets(const OITDescriptorSets&) = delete;
	OITDescriptorSets& operator=(const OITDescriptorSets&) = delete;
	/* (the frame slots' are in a vector: moved, the sets go with it) */
	OITDescriptorSets(OITDescriptorSets&& o) noexcept { d = o.d; vk_oit_desc_sets_construct(&o.d); }
	OITDescriptorSets& operator=(OITDescriptorSets&& o) noexcept
	{
		if (this != &o)
		{
			vk_oit_desc_sets_term(&d);
			d = o.d;
			vk_oit_desc_sets_construct(&o.d);
		}
		return *this;
	}

	void Init(SamplerManager* samplerManager, vk::PipelineLayout pipelineLayout, vk::DescriptorSetLayout perFrameLayout,
			vk::DescriptorSetLayout perPolyLayout, vk::DescriptorSetLayout colorInputLayout)
	{
		vk_oit_desc_sets_init(&d, samplerManager->Raw(), (VkPipelineLayout)pipelineLayout, (VkDescriptorSetLayout)perFrameLayout,
				(VkDescriptorSetLayout)perPolyLayout, (VkDescriptorSetLayout)colorInputLayout);
	}
	void UpdateUniforms(vk::Buffer vertexUniformBuffer, vk::Buffer buffer, u32 fragmentUniformOffset, vk::ImageView fogImageView,
			u32 polyParamsOffset, u32 polyParamsSize, vk::ImageView stencilImageView, vk::ImageView depthImageView,
			vk::ImageView paletteImageView, vk::ImageLayout depthInputLayout,
			vk::Buffer gpuPolyParams = vk::Buffer(), vk::DeviceSize gpuPolyParamsSize = 0)
	{
		vk_oit_desc_sets_update_uniforms(&d, (VkBuffer)vertexUniformBuffer, (VkBuffer)buffer, fragmentUniformOffset,
				(VkImageView)fogImageView, polyParamsOffset, polyParamsSize, (VkImageView)stencilImageView,
				(VkImageView)depthImageView, (VkImageView)paletteImageView, (VkImageLayout)depthInputLayout,
				(VkBuffer)gpuPolyParams, gpuPolyParamsSize);
	}
	void UpdateColorInputDescSet(int index, vk::ImageView colorImageView, vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal)
	{
		vk_oit_desc_sets_update_color_input(&d, index, (VkImageView)colorImageView, (VkImageLayout)layout);
	}
	vk::DescriptorSet SetTexture(u64 textureId0, TSP tsp0, u64 textureId1, TSP tsp1)
	{
		return vk::DescriptorSet(vk_oit_desc_sets_set_texture(&d, textureId0, tsp0.full, textureId1, tsp1.full));
	}
	void BindPerFrameDescriptorSets(vk::CommandBuffer cmdBuffer) { vk_oit_desc_sets_bind_per_frame(&d, (VkCommandBuffer)cmdBuffer); }
	void BindColorInputDescSet(vk::CommandBuffer cmdBuffer, int index)
	{
		vk_oit_desc_sets_bind_color_input(&d, (VkCommandBuffer)cmdBuffer, index);
	}
	void BindPerPolyDescriptorSets(vk::CommandBuffer cmdBuffer, vk::DescriptorSet set)
	{
		vk_oit_desc_sets_bind_per_poly(&d, (VkCommandBuffer)cmdBuffer, (VkDescriptorSet)set);
	}
	void Reset() { vk_oit_desc_sets_reset(&d); }

private:
	vk_oit_desc_sets d;
};

typedef PsoChoice OITPsoChoice;

class OITPipelineManager
{
public:
	OITPipelineManager() { vk_oit_pipeline_mgr_construct(&m, false); }
	/* (@rtt: the renders to a texture's) */
	explicit OITPipelineManager(bool rtt) { vk_oit_pipeline_mgr_construct(&m, rtt); }
	~OITPipelineManager() { vk_oit_pipeline_mgr_term(&m); }
	OITPipelineManager(const OITPipelineManager&) = delete;
	OITPipelineManager& operator=(const OITPipelineManager&) = delete;

	/* @samples: SSAA's (1: off), which the shader manager has been given too.
	 * The general pipelines and the fixed ones are made here. */
	void Init(OITShaderManager *shaderManager, OITBuffers *oitBuffers, u32 samples = 1, bool keepDepth = false)
	{
		if (!vk_oit_pipeline_mgr_init(&m, shaderManager->Raw(), (VkDescriptorSetLayout)oitBuffers->GetDescriptorSetLayout(),
				oitBuffers->Sums(), samples, keepDepth))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_oit_pipeline_mgr_init");
	}
	OITPsoChoice GetPipeline(u32 listType, bool autosort, const PolyParam& pp, Pass pass)
	{
		return vk_oit_pipeline_mgr_get(&m, listType, autosort, &pp, (int)pass);
	}
	void SetDynamicState(vk::CommandBuffer cmdBuffer, u64 key) const
	{
		vk_oit_pipeline_mgr_set_dynamic_state(&m, (VkCommandBuffer)cmdBuffer, key);
	}
	vk::Pipeline GetModifierVolumePipeline(ModVolMode mode, int cullMode)
	{
		return vk::Pipeline(vk_oit_pipeline_mgr_modvol(&m, (int)mode, (u32)cullMode));
	}
	vk::Pipeline GetTrModifierVolumePipeline(ModVolMode mode, int cullMode)
	{
		return vk::Pipeline(vk_oit_pipeline_mgr_trmodvol(&m, (int)mode, (u32)cullMode));
	}
	void (VKAPI_PTR *CullSetter() const)(VkCommandBuffer, VkCullModeFlags) { return m.cullSetter; }
	vk::Pipeline GetFinalPipeline(bool perSample = false, bool sums = false)
	{
		return vk::Pipeline(vk_oit_pipeline_mgr_final(&m, perSample, sums));
	}
	bool Sums() const { return m.sums; }
	int GetFinalSubpass() const { return vk_oit_final_subpass(&m.passes); }
	u32 GetSamples() const { return m.passes.samples; }
	vk::Pipeline GetClearPipeline() { return vk::Pipeline(vk_oit_pipeline_mgr_clear(&m)); }
	vk::PipelineLayout GetPipelineLayout() const { return vk::PipelineLayout(m.pipelineLayout); }
	vk::DescriptorSetLayout GetPerFrameDSLayout() const { return vk::DescriptorSetLayout(m.perFrameLayout); }
	vk::DescriptorSetLayout GetPerPolyDSLayout() const { return vk::DescriptorSetLayout(m.perPolyLayout); }
	vk::DescriptorSetLayout GetColorInputDSLayout() const { return vk::DescriptorSetLayout(m.colorInputLayout); }
	vk::RenderPass GetRenderPass(bool initial, bool last, bool depthWrites = false, bool resumed = false)
	{
		const VkRenderPass renderPass = vk_oit_passes_get(&m.passes, initial, last, depthWrites, resumed);
		if (renderPass == VK_NULL_HANDLE)
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateRenderPass");
		return vk::RenderPass(renderPass);
	}
	void Reset() { vk_oit_pipeline_mgr_reset(&m); }
	vk_oit_pipeline_mgr *Raw() { return &m; }

protected:
	vk_oit_pipeline_mgr m;
};

class RttOITPipelineManager : public OITPipelineManager
{
public:
	RttOITPipelineManager() : OITPipelineManager(true) {}
	void Init(OITShaderManager *shaderManager, OITBuffers *oitBuffers, u32 samples = 1, bool keepDepth = false)
	{
		(void)samples;
		(void)keepDepth;
		if (!vk_oit_pipeline_mgr_rtt_init(&m, shaderManager->Raw(), (VkDescriptorSetLayout)oitBuffers->GetDescriptorSetLayout(),
				oitBuffers->Sums()))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_oit_pipeline_mgr_init");
	}
};
