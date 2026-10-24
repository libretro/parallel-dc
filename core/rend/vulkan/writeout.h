/* The chip's write-out (rend/tile/pvr_scaler.h: the Y filter, then the
 * packing to a framebuffer of 16 bits a pixel and its dither) for the
 * Vulkan raster renderers' pictures, as the core option
 * reicast_pvr2_scaler asks: the compute renderer's own pass over its
 * picture (pvr_compute.h's PVRC_K_YFILTER, the same SPIR-V), run in place
 * on the raster renderer's after the frame's render pass. Made in Init(),
 * when the renderer starts or changes size (a change of the option is
 * one), with the pictures made for it to write; nothing is made in a
 * frame, and a frame without a write-out records nothing. */
#pragma once
#include "vulkan.h"
#include "buffer.h"
#include "rend/tile/compute_frame.h"

#include <memory>
#include <vector>

class VulkanWriteOut
{
public:
	// whether the pictures are to be made for it (vk::ImageUsageFlagBits::eStorage)
	static bool Wanted() { return !ComputeFrame::ScalersOff; }
	// @count pictures, a set and a slot of words for each
	void Init(u32 count);
	void Term();
	/* The pass over picture @index, @image, @width x @height, which the
	 * render pass left in eShaderReadOnlyOptimal, and leaves so; nothing
	 * when the frame in the registers has no write-out */
	void Record(vk::CommandBuffer cmd, u32 index, vk::Image image, vk::ImageView view, u32 width, u32 height);

private:
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniquePipeline pipeline;
	vk::UniqueDescriptorPool pool;
	std::vector<vk::DescriptorSet> sets;
	std::unique_ptr<BufferData> words;
	u32 *mapped = nullptr;
};
