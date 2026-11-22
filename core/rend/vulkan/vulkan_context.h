/*
    Created on: Nov 29, 2019

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
#include "vk_context.h"
#include "heap.h"
#include "quad.h"
#include "rend/TexCache.h"

/* The C++ renderers' view of the context (vk_context.h), until they are C
 * too: what they called it by, on the one there is */
class VulkanContext
{
public:
	static VulkanContext *Instance() { return reinterpret_cast<VulkanContext *>(&vkc); }

	u32 GetGraphicsQueueFamilyIndex() const { return vkc.iface->queue_index; }
	/* @hdr: the image is the HDR output's (HdrEncode(), the HDR subpass) */
	void PresentFrame(vk::Image image, vk::ImageView imageView, vk::Offset2D extent, bool hdr = false)
	{
		vkc_present_frame((VkImage)image, (VkImageView)imageView, extent.x, extent.y, hdr);
	}

	bool HdrActive() const { return vkc.hdr != nullptr; }
	bool HdrPostActive() const { return vkc_hdr_post_active(); }
	vk::Format GetHdrFormat() const { return (vk::Format)vkc.hdrFormat; }
	/* the HDR output's work of the frame, before any of it is encoded
	 * (HdrSubpassDraw()); @pool a CommandPool */
	template<typename Pool>
	void HdrPrepare(Pool& pool, vk::CommandBuffer cmd)
	{
		vkc_hdr_prepare(&pool, (unsigned)pool.GetIndex(), pool.GetFrames(), (VkCommandBuffer)cmd);
	}
	vk::UniquePipeline HdrSubpassPipeline(vk::RenderPass renderPass, u32 subpass, u32 samples = 1)
	{
		if (vkc.hdr == nullptr)
			return vk::UniquePipeline();
		return vk::UniquePipeline(vk::Pipeline(vkc_hdr_subpass_pipeline((VkRenderPass)renderPass, subpass, samples)),
				vk::ObjectDestroy<vk::Device, vk::DispatchLoaderStatic>(vk::Device(vkc.device)));
	}
	template<typename Pool>
	void HdrSubpassDraw(Pool& pool, vk::CommandBuffer cmd, vk::Pipeline pipeline, vk::ImageView picture,
			vk::ImageView samples, vk::Extent2D extent)
	{
		vkc_hdr_subpass_draw(&pool, (unsigned)pool.GetIndex(), (VkCommandBuffer)cmd, (VkPipeline)pipeline,
				(VkImageView)picture, (VkImageView)samples, extent.width, extent.height);
	}
	/* The picture encoded into the HDR output's image, in a command buffer
	 * of @pool's: @image and @view then that image's. False with no HDR
	 * output. @sums: the channel sums the picture's samples were resolved
	 * to (the compute renderer's), after HdrSumsReady() */
	template<typename Pool>
	bool HdrEncode(Pool& pool, vk::Image& image, vk::ImageView& view, vk::Extent2D size,
			vk::ImageView sums = vk::ImageView())
	{
		if (vkc.hdr == nullptr)
			return false;
		VkImage i = (VkImage)image;
		VkImageView v = (VkImageView)view;
		vk::CommandBuffer cmd = pool.Allocate();
		cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
		const bool done = vkc_hdr_encode(&pool, (unsigned)pool.GetIndex(), pool.GetFrames(), (VkCommandBuffer)cmd,
				&i, &v, size.width, size.height, (VkImageView)sums);
		cmd.end();
		if (done)
		{
			image = vk::Image(i);
			view = vk::ImageView(v);
		}
		return done;
	}
	/* The HDR output, and HdrEncode() able to take the channel sums: made
	 * once the output has had a frame */
	bool HdrSumsReady() { return vkc_hdr_sums_ready(); }

	vk::PhysicalDevice GetPhysicalDevice() const { return vk::PhysicalDevice(vkc.gpu); }
	vk::Device GetDevice() const { return vk::Device(vkc.device); }
	vk::PipelineCache GetPipelineCache() const { return vk::PipelineCache(vkc.pipelineCache); }
	vk::DescriptorPool GetDescriptorPool() const { return vk::DescriptorPool(vkc.descriptorPool); }
	vk::Extent2D GetViewPort() const { return vk::Extent2D(vkc.width, vkc.height); }
	int GetSwapChainSize() const { return vkc.swapChainSize; }
	int GetCurrentImageIndex() const { return vkc_current_image_index(); }
	void WaitIdle() const { vkc.iface->wait_sync_index(vkc.iface->handle); }
	void ReleasePicture() { vkc_release_picture(); }
	void WaitQueueIdle() { vkc_wait_queue_idle(); }
	void SubmitCommandBuffers(u32 bufferCount, vk::CommandBuffer *buffers, vk::Fence fence)
	{
		vkc_submit(bufferCount, reinterpret_cast<const VkCommandBuffer *>(buffers), (VkFence)fence);
	}
	vk::DeviceSize GetUniformBufferAlignment() const { return vkc.uniformBufferAlignment; }
	vk::DeviceSize GetStorageBufferAlignment() const { return vkc.storageBufferAlignment; }
	bool IsFormatSupported(TextureType textureType) { return vkc_format_supported((int)textureType); }
	vk::Format GetColorFormat() const { return (vk::Format)vkc.colorFormat; }
	vk::Format GetDepthFormat() const { return (vk::Format)vkc.depthFormat; }
	bool SupportsSamplerAnisotropy() const { return vkc.samplerAnisotropy; }
	bool SupportsShaderInt64() const { return vkc.shaderInt64; }
	bool SupportsSampledImageArrayIndexing() const { return vkc.sampledImageArrayIndexing; }
	bool SupportsNonUniformTextureIndexing() const { return vkc.nonUniformTextureIndexing; }
	bool SupportsSubgroupBallotCompute() const { return vkc.subgroupBallotCompute; }
	bool SupportsStorageImageExtendedFormats() const { return vkc.storageImageExtendedFormats; }
	bool SupportsPostDepthCoverage() const { return vkc.postDepthCoverage; }
	u32 GetApiVersion() const { return vkc.apiVersion; }
	bool SupportsDedicatedAllocation() const { return vkc.dedicatedAllocationSupported; }
	VulkanHeap& GetHeap() { static VulkanHeap heap; return heap; }
	vk::DeviceSize GetMaxMemoryAllocationSize() const { return vkc.maxMemoryAllocationSize; }
	u32 GetMaxStorageBufferRange() const { return vkc.maxStorageBufferRange; }
	f32 GetMaxSamplerAnisotropy() const { return vkc.samplerAnisotropy ? vkc.maxSamplerAnisotropy : 1.f; }
	u32 GetVendorID() const { return vkc.vendorID; }
	const VramBudget& GetVramBudget() const { return vkc.vramBudget; }
	bool SupportsMultiDrawIndirect() const { return vkc.multiDrawIndirect; }
	u32 GetMaxDrawIndirectCount() const { return vkc.multiDrawIndirect ? vkc.maxDrawIndirectCount : 1; }
	bool SupportsDrawIndirectFirstInstance() const { return vkc.drawIndirectFirstInstance; }
	PFN_vkCmdDrawIndexedIndirectCountKHR GetDrawIndexedIndirectCount() const { return vkc.cmdDrawIndexedIndirectCount; }
	u32 SsaaSamples(u32 requested, u32 perSampleKib, u32 fixedKib, bool transient, const char *what) const
	{
		return vkc_ssaa_samples(requested, perSampleKib, fixedKib, transient, what);
	}
	u32 GetDepthBytes() const { return vkc_depth_bytes(); }
	bool HasLazyMemory() const { return vkc.lazyMemory; }
	void LogVramUsage() { vkc_log_vram_usage(); }
	const pso_ext::Functions& DynamicState() const { return vkc.dynamicState; }
	PsoKeyLog& PsoKeys() { return *reinterpret_cast<PsoKeyLog *>(&vkc.psoKeys); }
	void SavePipelineCache() { vkc_save_pipeline_cache(); }

private:
	VulkanContext();
};
