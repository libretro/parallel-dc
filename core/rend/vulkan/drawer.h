/*
	Created on: Oct 8, 2019

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
#include "rend/modvol_clip.h"
#include <memory>
#include "rend/sorter.h"
#include "rend/tileclip.h"
#include "rend/transform_matrix.h"
#include "vulkan.h"
#include "buffer.h"
#include "commandpool.h"
#include "pipeline.h"
#include "shaders.h"
#include "texture.h"
#include "quad.h"
#include "ta_decoder.h"
#include "rend/shadows.h"
#include "ssaa.h"
#include "gpu_timer.h"
#include "writeout.h"
#include "rend/depth_scale.h"
#include "vk_screen_drawer.h"
#include <type_traits>
#include "rend/ta_gpu.h"
#include "rend/upscaler.h"
#include "motion_pass.h"

/* The C++ drawers' view of a vk_base_drawer (vk_drawer_base.h) */
class BaseDrawer
{
public:
	BaseDrawer() { vk_base_drawer_construct(&b); }
	~BaseDrawer() { vk_base_drawer_term(&b); }
	BaseDrawer(const BaseDrawer&) = delete;
	BaseDrawer& operator=(const BaseDrawer&) = delete;

	void SetScissor(const vk::CommandBuffer& cmdBuffer, const vk::Rect2D& scissor)
	{
		vk_base_drawer_set_scissor(&b, (VkCommandBuffer)cmdBuffer, &static_cast<const VkRect2D&>(scissor));
	}
	void SetCommandPool(CommandPool *commandPool)
	{
		this->commandPool = commandPool;
		b.pool = commandPool != nullptr ? &commandPool->p : nullptr;
	}
	// what a frame decoded on the GPU is drawn from (pvrrc.gpu)
	void SetGpuDraw(const TaGpuDraw *gpuDraw) { b.gpuDraw = reinterpret_cast<const vk_ta_gpu_draw *>(gpuDraw); }
	typedef vk_parsed Parsed;
	/* This render's frame was parsed there; none (a null buffer): it is
	 * copied into the main buffer */
	void SetParsed(const Parsed& parsed) { b.parsed = parsed; }
	vk_base_drawer *Base() { return &b; }

protected:
	VulkanContext *GetContext() const { return VulkanContext::Instance(); }
	TileClipping SetTileClip(u32 val, vk::Rect2D& clipRect)
	{
		return (TileClipping)vk_base_drawer_tile_clip(&b, val, &static_cast<VkRect2D&>(clipRect));
	}
	void SetBaseScissor() { vk_base_drawer_set_base_scissor(&b); }
	void SetProvokingVertices() { vk_drawer_provoking_vertices(); }

	u32 align(vk::DeviceSize offset, u32 alignment)
	{
		return (u32)(alignment - (offset & (alignment - 1)));
	}

	template<typename T>
	T MakeFragmentUniforms()
	{
		T fragUniforms;
		vk_base_drawer_fragment_uniforms(&fragUniforms);
		return fragUniforms;
	}
	const TaGpuDraw *GpuDraw() const { return reinterpret_cast<const TaGpuDraw *>(b.gpuDraw); }

	void BindGeometry(const vk::CommandBuffer& cmdBuffer, vk::DeviceSize indexOffset) const
	{
		vk_base_drawer_bind_geometry(&b, (VkCommandBuffer)cmdBuffer, indexOffset);
	}
	bool UseParsed(u32 sortedBytes) { return vk_base_drawer_use_parsed(&b, sortedBytes); }
	void SetGeometry(vk::Buffer mainBuffer) { vk_base_drawer_set_geometry(&b, (VkBuffer)mainBuffer); }
	void BindModVolTriangles(const vk::CommandBuffer& cmdBuffer, vk::Buffer mainBuffer, vk::DeviceSize modVolOffset) const
	{
		vk_base_drawer_bind_modvol_triangles(&b, (VkCommandBuffer)cmdBuffer, (VkBuffer)mainBuffer, modVolOffset);
	}
	bool ModVolsOnCpu() const { return vk_base_drawer_modvols_on_cpu(); }
	const List<ModTriangle>& ModVolList() const { return pvrrc.mv_clip.used() != 0 ? pvrrc.mv_clip : pvrrc.modtrig; }
	void BindVertices(const vk::CommandBuffer& cmdBuffer) const { vk_base_drawer_bind_vertices(&b, (VkCommandBuffer)cmdBuffer); }
	u32 ModVolTriangles() const { return vk_base_drawer_modvol_triangles(); }
	void DrawIndices(const vk::CommandBuffer& cmdBuffer, u32 listType, u32 first, u32 count) const
	{
		vk_base_drawer_draw_indices(&b, (VkCommandBuffer)cmdBuffer, listType, first, count);
	}

	/* every swap chain image's vertex uniform buffer, before the first
	 * frame: nothing allocated while the game runs */
	void MakeVertexUniforms()
	{
		if (!vk_base_drawer_make_vertex_uniforms(&b, (u32)GetContext()->GetSwapChainSize()))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_base_drawer_make_vertex_uniforms");
	}
	vk::Buffer WriteVertexUniforms(const vk::CommandBuffer& cmdBuffer, int image)
	{
		const VkBuffer buffer = vk_base_drawer_write_vertex_uniforms(&b, (VkCommandBuffer)cmdBuffer, image);
		if (buffer == VK_NULL_HANDLE)
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_base_drawer_write_vertex_uniforms");
		return vk::Buffer(buffer);
	}

	vk_base_drawer b;
	/* (its members, as the C++ drawers name them) */
	vk::Rect2D& baseScissor = reinterpret_cast<vk::Rect2D&>(b.baseScissor);
	vk::Rect2D& currentScissor = reinterpret_cast<vk::Rect2D&>(b.currentScissor);
	TransformMatrix<false>& matrices = reinterpret_cast<TransformMatrix<false>&>(b.matrices);
	vk::Buffer& geometry = reinterpret_cast<vk::Buffer&>(b.geometry);
	vk::DeviceSize& geometryVertices = b.geometryVertices;
	vk::Buffer& vertexUniformBuffer = reinterpret_cast<vk::Buffer&>(b.vertexUniformBuffer);
	CommandPool *commandPool = nullptr;
};
static_assert(sizeof(TransformMatrix<false>) == sizeof(transform_matrix_t) && std::is_standard_layout<TransformMatrix<false>>::value,
		"TransformMatrix is transform_matrix_t");
static_assert(sizeof(vk::Rect2D) == sizeof(VkRect2D) && sizeof(vk::Buffer) == sizeof(VkBuffer), "vk:: wrappers are their handles");

