/* The C++ renderers' view of the compute renderer (vk_compute_drawer.h),
 * until they are C too */
#pragma once
#include "vulkan.h"
#include "commandpool.h"
#include "texture.h"
#include "ta_decoder.h"
#include "motion_pass.h"
#include "vk_compute_drawer.h"
#include "rend/tile/compute_frame.h"

#include <vector>

/* The frame's words for the kernels (PVRC_F_*), from the render context
 * (pvrrc): what the tests build their frames with (tools/rend/tile_dump) */
class VulkanComputeFrame : public ComputeFrame
{
public:
	VulkanComputeFrame() { vk_compute_frame_init(&f); }

protected:
	const xform& Begin() override { return *vk_compute_frame_begin(&f); }
	void BaseScissor(int& x, int& y, int& width, int& height) override { vk_compute_frame_base_scissor(&f, &x, &y, &width, &height); }
	void ProvokingVertices() override { vk_compute_frame_provoking_vertices(&f); }

private:
	vk_compute_frame f;
};

class ComputeDrawer
{
public:
	ComputeDrawer() { vk_compute_drawer_construct(&d); }
	~ComputeDrawer() { vk_compute_drawer_term(&d); }
	ComputeDrawer(const ComputeDrawer&) = delete;
	ComputeDrawer& operator=(const ComputeDrawer&) = delete;

	bool Init(vk::Extent2D extent, u32 samples, CommandPool *pool, SamplerManager *samplerManager, bool depth = false)
	{
		return vk_compute_drawer_init(&d, extent.width, extent.height, samples, &pool->p, samplerManager->Raw(), depth);
	}
	void Term() { vk_compute_drawer_term(&d); }
	static const char *DeviceLacks() { return vk_compute_drawer_device_lacks(); }
	bool Ready() const { return d.ready; }
	void SetGpuDraw(const TaGpuDraw *gpuDraw) { d.gpuDraw = reinterpret_cast<const vk_ta_gpu_draw *>(gpuDraw); }
	u32 GetSamples() const { return d.samples; }
	bool CanDraw() const { return vk_compute_drawer_can_draw(&d); }
	bool Prepare(bool last, bool scaled, const Texture *paletteTexture, vk::Extent2D rtt = vk::Extent2D())
	{
		return vk_compute_drawer_prepare(&d, last, scaled, paletteTexture, rtt.width, rtt.height);
	}
	void Record(vk::Image image, vk::ImageView view, vk::ImageView last)
	{
		if (!vk_compute_drawer_record(&d, (VkImage)image, (VkImageView)view, (VkImageView)last))
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateComputePipelines");
	}
	std::vector<vk::ImageView> DepthViews() const
	{
		std::vector<vk::ImageView> views(d.pool != nullptr ? d.pool->slotCount : 0);
		views.resize(vk_compute_drawer_depth_views(&d, reinterpret_cast<VkImageView *>(views.data()), (u32)views.size()));
		return views;
	}
	vk::Image DepthImage() const { return vk::Image(vk_compute_drawer_depth_image(&d)); }
	vk::ImageView DepthView() const { return vk::ImageView(vk_compute_drawer_depth_view(&d)); }
	void SetMotionPass(MotionVectorPass *motion) { d.motion = motion != nullptr ? motion->Raw() : nullptr; }
	int Slot() const { return vk_compute_drawer_slot(&d); }
	vk::ImageView Sums() const { return vk::ImageView(vk_compute_drawer_sums(&d)); }
	void Fallback(u32 why) { vk_compute_drawer_fallback(&d, why); }

private:
	vk_compute_drawer d;
};
