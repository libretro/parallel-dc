/* SSAA in the Vulkan raster renderers (both of them).
 *
 * With SSAA on, a frame is drawn into colour, depth and stencil
 * attachments of N samples a pixel (VulkanContext::SsaaSamples()), made
 * when the renderer starts or changes size and never stored where a frame
 * has one render pass: their contents go when the pass ends, and on a
 * device with memory that is only backed when used they take none. The
 * last subpass resolves them to the picture with the shared rule
 * (rend/tile/pvr_resolve.h, in integers), not the device's resolve.
 *
 * Polygons are shaded once per sample, at the sample, but for the ones
 * drawn at a single rate (rend/tile/pvr_tile.h): sprites, the background
 * and, where the decoder looks for them, flat strips. Those are shaded
 * once per pixel, at its centre, as with SSAA off; a pixel all of whose
 * samples they cover resolves to exactly what SSAA off draws. */
#pragma once
#include "vulkan_context.h"
#include "commandpool.h"
#include "pso.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/tile/pvr_tile.h"
// (SsaaSingleRate(): which polygons are drawn at a single rate)
#include "rend/ssaa_rate.h"

// a pipeline's multisample state: @samples a pixel, shaded at each sample or once a pixel
static inline vk::PipelineMultisampleStateCreateInfo SsaaMultisample(u32 samples, bool perSample)
{
	vk::PipelineMultisampleStateCreateInfo info;
	info.rasterizationSamples = (vk::SampleCountFlagBits)samples;
	info.sampleShadingEnable = perSample && samples > 1;
	info.minSampleShading = 1.f;
	return info;
}

#include "vk_quad.h"

/* The C++ renderers' view of a vk_ssaa_resolve (vk_quad.h) */
class SsaaResolve
{
public:
	SsaaResolve() { vk_ssaa_resolve_construct(&r); }
	~SsaaResolve() { vk_ssaa_resolve_term(&r); }
	SsaaResolve(const SsaaResolve&) = delete;
	SsaaResolve& operator=(const SsaaResolve&) = delete;
	void Init(vk::RenderPass renderPass, u32 subpass, u32 samples, vk::ImageView samplesView)
	{
		if (!vk_ssaa_resolve_init(&r, (VkRenderPass)renderPass, subpass, samples, (VkImageView)samplesView))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_ssaa_resolve_init");
	}
	void Draw(vk::CommandBuffer cmd, vk::Extent2D extent) const
	{
		if (!vk_ssaa_resolve_draw(&r, (VkCommandBuffer)cmd, extent.width, extent.height))
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateGraphicsPipelines");
	}
	bool Ready() const { return r.descSet != VK_NULL_HANDLE; }
	void SetSamplesView(vk::ImageView samplesView, CommandPool *commandPool)
	{
		vk_ssaa_resolve_set_samples_view(&r, (VkImageView)samplesView, &commandPool->p);
	}
	void Term() { vk_ssaa_resolve_term(&r); }

private:
	mutable vk_ssaa_resolve r;	/* (a draw may make its pipeline) */
};
