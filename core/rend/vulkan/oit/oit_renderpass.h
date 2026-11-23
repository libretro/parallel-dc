/*
    Created on: Nov 10, 2019

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
/* The C++ renderers' view of vk_oit_renderpass.h, until they are C too */
#include "../vulkan_context.h"
#include "vk_oit_renderpass.h"

class RenderPasses
{
public:
	RenderPasses() { vk_oit_passes_construct(&p, false); }
	RenderPasses(const RenderPasses&) = delete;
	RenderPasses& operator=(const RenderPasses&) = delete;
	~RenderPasses() { Reset(); }

	vk::RenderPass GetRenderPass(bool initial, bool last, bool depthWrites = false, bool resumed = false)
	{
		const VkRenderPass renderPass = vk_oit_passes_get(&p, initial, last, depthWrites, resumed);
		if (renderPass == VK_NULL_HANDLE)
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateRenderPass");
		return vk::RenderPass(renderPass);
	}
	void MakeAll()
	{
		if (!vk_oit_passes_make_all(&p))
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateRenderPass");
	}
	static vk::ImageLayout DepthInputLayout(bool depthWrites) { return (vk::ImageLayout)vk_oit_depth_input_layout(depthWrites); }
	void Reset() { vk_oit_passes_reset(&p); }
	void SetSamples(u32 samples) { vk_oit_passes_set_samples(&p, samples); }
	u32 GetSamples() const { return p.samples; }
	void SetKeepDepth(bool keep) { vk_oit_passes_set_keep_depth(&p, keep); }
	bool KeepsDepth() const { return p.keepDepth; }
	static constexpr int SsaaFinalSubpass = VK_OIT_SSAA_FINAL_SUBPASS;
	int GetFinalSubpass() const { return vk_oit_final_subpass(&p); }

protected:
	vk_oit_passes p;
};

/* renders to a texture's: an R8G8B8A8 picture, sampled after */
class RttRenderPasses : public RenderPasses
{
public:
	RttRenderPasses() { p.rtt = true; }
};
