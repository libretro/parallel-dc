/*
    Created on: Oct 27, 2019

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
/* The C++ renderers' view of the per-pixel renderer's shaders
 * (vk_oit_pipeline.h), until they are C too */
#include "../vulkan.h"
#include "../utils.h"
#include "../shaders.h"
#include "../vulkan_context.h"
#include "vk_oit_pipeline.h"

enum class Pass { Depth = VK_OIT_PASS_DEPTH, Color = VK_OIT_PASS_COLOR, OIT = VK_OIT_PASS_OIT };

class OITShaderManager
{
public:
	OITShaderManager() { vk_oit_shaders_construct(&sh); }
	~OITShaderManager() { vk_oit_shaders_term(&sh); }
	OITShaderManager(const OITShaderManager&) = delete;
	OITShaderManager& operator=(const OITShaderManager&) = delete;

	void Init()
	{
		if (!vk_oit_shaders_init(&sh))
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateShaderModule");
	}
	// the temporal upscaler's motion vector pass (../motion_pass.h), and its depth-sampling one
	vk::ShaderModule GetMotionVertexShader() const { return vk::ShaderModule(sh.modules.m[RSPV_MV_VERT]); }
	vk::ShaderModule GetMotionShader() const { return vk::ShaderModule(sh.modules.m[RSPV_MV_FRAG]); }
	vk::ShaderModule GetMotionDepthShader() const { return vk::ShaderModule(sh.modules.m[RSPV_MV_DEPTH_FRAG]); }
	/* SSAA's samples a pixel (1: off): what the pipelines are specialized
	 * for, and which modules they take */
	void SetSamples(u32 samples) { sh.samples = samples; }
	u32 GetSamples() const { return sh.samples; }
	void Term() { vk_oit_shaders_term(&sh); }
	vk_oit_shaders *Raw() { return &sh; }

private:
	vk_oit_shaders sh;
};
