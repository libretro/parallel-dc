/* The chip's write-out (rend/tile/pvr_scaler.h: the Y filter, then the
 * packing to a framebuffer of 16 bits a pixel and its dither) for the
 * Vulkan raster renderers' pictures, as the core option
 * reicast_pvr2_scaler asks: the compute renderer's own pass over its
 * picture (pvr_compute.h's PVRC_K_YFILTER, the same SPIR-V), run in place
 * on the raster renderer's after the frame's render pass. Its pipeline is
 * asked of the pipeline thread (pso.h) by the first Init(), as the
 * renderer starts, whether the option is on or not, and kept: a change of
 * size (a change of the option is one) makes only the pictures' sets,
 * slots and framebuffers again. Nothing is made in a frame, and a frame
 * without a write-out records nothing. What goes over
 * the picture (the VMUs, the crosshairs) is drawn after it, as on the
 * console: the frame's render pass is ended, the pass run, and they are
 * drawn in a render pass of their own that loads the picture
 * (BeginOverlay()), made in Init() too. */
#pragma once
#include "vulkan.h"
#include "buffer.h"
#include "pso.h"
#include "rend/tile/compute_frame.h"

#include <memory>
#include <vector>

class VulkanWriteOut : public PsoBuilder
{
public:
	VulkanWriteOut();
	// whether the pictures are to be made for it (vk::ImageUsageFlagBits::eStorage)
	static bool Wanted() { return !ComputeFrame::ScalersOff; }
	// the pictures @views, @extent: a set, a slot of words and a framebuffer of the overlay's for each
	void Init(const std::vector<vk::ImageView>& views, vk::Extent2D extent);
	// everything, the pipeline too (the renderer's end)
	void Term();
	// whether the frame in the registers has a write-out, to be recorded
	bool Runs(u32 height) const { return !sets.empty() && PVRC_FF_YLINES(ComputeFrame::WriteOut(height)) != 0; }
	VkPipeline BuildPipeline(u64 key, vk::PipelineCache cache) override;
	// the render pass of what goes over the picture after the pass (null when none was made)
	vk::RenderPass OverlayPass() const { return *overlayPass; }
	// ...begun on picture @index, after Record()
	void BeginOverlay(vk::CommandBuffer cmd, u32 index);
	/* The pass over picture @index, @image, @width x @height, which the
	 * render pass left in eShaderReadOnlyOptimal, and leaves so; nothing
	 * when the frame in the registers has no write-out */
	void Record(vk::CommandBuffer cmd, u32 index, vk::Image image, vk::ImageView view, u32 width, u32 height);

private:
	// what the pictures of a size have
	void DropSized();
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniqueShaderModule module;
	PsoSlot slot;
	vk::UniqueDescriptorPool pool;
	std::vector<vk::DescriptorSet> sets;
	std::unique_ptr<BufferData> words;
	u32 *mapped = nullptr;
	vk::UniqueRenderPass overlayPass;
	std::vector<vk::UniqueFramebuffer> overlayFramebuffers;
	vk::Extent2D extent;
};
