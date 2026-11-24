#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
    Created on: Oct 2, 2019

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
#include <memory>
#include <math.h>
#include "vulkan.h"
#include "vulkan_renderer.h"
#include "drawer.h"
#include "shaders.h"
#include "rend/ssaa_pattern.h"

class VulkanRenderer final : public BaseVulkanRenderer
{
public:
	bool Init() override
	{
		DEBUG_LOG(RENDERER, "VulkanRenderer::Init\n");

		samples = SsaaSamples();
		samplerManager.Init();
		vk_rtt_drawer_set_pool(&textureDrawer, &texCommandPool.p);
		InitTextureDrawer();
		textureDrawer.base.gpuDraw = reinterpret_cast<const vk_ta_gpu_draw *>(taDecoder.Draw());

		vk_screen_drawer_set_pool(&screenDrawer, &texCommandPool.p);
		screenDrawer.base.gpuDraw = reinterpret_cast<const vk_ta_gpu_draw *>(taDecoder.Draw());
		InitScreenDrawer();
		taDecoder.WantSort(true);

		return true;
	}

	void Resize(int w, int h) override
	{
		// what is replaced below may be in use by frames still on the GPU
		texCommandPool.WaitAll();
		const u32 was = samples;
		samples = SsaaSamples();
		if (samples != was)
		{
			InitTextureDrawer();
			quadPipeline.Forget();
		}
		InitScreenDrawer();
		// what this size no longer needs is given back once the frames that used it are done
		GetContext()->GetHeap().TrimSoon();
	}

	void KeepPicture() override { vk_screen_drawer_keep_picture(&screenDrawer); }
	void SetRestoredPicture(vk::ImageView view, vk::Extent2D size) override
	{
		vk_screen_drawer_set_restored_picture(&screenDrawer, (VkImageView)view, (VkExtent2D)size);
	}
	VulkanRenderer()
	{
		vk_screen_drawer_construct(&screenDrawer);
		vk_rtt_drawer_construct(&textureDrawer);
	}
	~VulkanRenderer()
	{
		vk_rtt_drawer_free(&textureDrawer);
		vk_screen_drawer_free(&screenDrawer);
	}

protected:
	/* A sorted pass's translucent triangles are sorted from the frame's
	 * depths (TA_GPU_SORT); sorted by strip, the polygons are sorted from
	 * the CPU's vertices, and a frame that may have a sorted pass is
	 * parsed. The polygons drawn again for shadows are those the GPU says
	 * reach into the volumes. */
	u32 TaFlags() const override
	{
		return TA_GPU_DRAW | TA_GPU_PROVOKE | TA_GPU_SHADOWS
				| (!settings.pvr.Emulation.AlphaSortMode && taDecoder.CanSort() ? TA_GPU_SORT : TA_GPU_NO_SORT);
	}

public:
	void Term() override
	{
		DEBUG_LOG(RENDERER, "VulkanRenderer::Term\n");
		GetContext()->LogVramUsage();
		// (the frontend lets go of the pictures before they go)
		GetContext()->ReleasePicture();
		texCommandPool.WaitAll();
		DropQuads();
		vk_screen_drawer_term(&screenDrawer);
		vk_rtt_drawer_term(&textureDrawer);
		samplerManager.Term();
		BaseVulkanRenderer::Term();
	}

	bool RenderFrame() override
	{
		if (pvrrc.isRenderFramebuffer)
			return true;
		if (pvrrc.isRTT && (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min))
		{
			/* A pixel clip that ends before it starts holds no pixel: the
			 * chip writes nothing. (Its size would be negative here.) */
			WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d\n",
					pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
			return false;
		}

		const VkImageView fogView = (VkImageView)fogTexture->GetImageView();
		const VkImageView paletteView = (VkImageView)paletteTexture->GetImageView();
		if (pvrrc.isRTT)
		{
			SetParsed(&textureDrawer.base);
			if (!vk_rtt_drawer_draw(&textureDrawer, fogView, paletteView))
				throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_rtt_drawer_draw");
			vk_rtt_drawer_end(&textureDrawer);
			return false;
		}

		SetParsed(&screenDrawer.base);
		if (!vk_screen_drawer_draw(&screenDrawer, fogView, paletteView))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_screen_drawer_draw");
		/* (the chip's write-out before what goes over the picture, which
		 * then goes in a render pass of its own: writeout.h) */
		const bool after = vk_screen_drawer_has_writeout(&screenDrawer) && vmus->Drawing()
				&& vk_screen_drawer_writeout_before_osd(&screenDrawer);
		vmus->DrawOSD(vk::CommandBuffer(screenDrawer.cmd), vk::Extent2D(screen_width, screen_height), &texCommandPool,
				after ? &overlayQuadPipeline : nullptr);
		if (!vk_screen_drawer_end(&screenDrawer))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_screen_drawer_end");

		return true;
	}

private:
	/* SSAA's samples a pixel, for the screen: what the option asks, as far
	 * as the device and the video memory budget go for its attachments */
	u32 SsaaSamples() const
	{
		// (the upscaler is the anti-aliasing then, and reads the depth, which SSAA's samples are not)
		if (upscaler_active())
			return 1;
		const vk::Extent2D size = GetContext()->GetViewPort();
		return GetContext()->SsaaSamples(settings.rend.Ssaa, ssaa_per_sample_kib(size.width, size.height, 4 + GetContext()->GetDepthBytes()),
				0, true, "Vulkan per-triangle renderer");
	}
	/* ...and for the textures rendered to: the same where their sample
	 * attachments cost nothing (transient, never stored, in memory that
	 * is only backed when used); one elsewhere, since they are made as
	 * large as such a texture can be, which is more video memory than the
	 * screen's */
	u32 RttSamples() const { return GetContext()->HasLazyMemory() ? samples : 1; }

	void InitTextureDrawer()
	{
		if (!vk_rtt_drawer_init(&textureDrawer, samplerManager.Raw(), shaderManager.Raw(), textureCache.Raw(), RttSamples()))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_rtt_drawer_init");
	}
	/* (and the textures' depth, shared with it, and what draws over it) */
	void InitScreenDrawer()
	{
		if (!vk_screen_drawer_init(&screenDrawer, samplerManager.Raw(), shaderManager.Raw(), samples)
				|| !vk_rtt_drawer_share_depth(&textureDrawer, &screenDrawer))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_screen_drawer_init");
		BaseInit(vk::RenderPass(screenDrawer.renderPass), vk_screen_drawer_osd_subpass(&screenDrawer));
		InitOverlay(vk::RenderPass(screenDrawer.writeOut.overlayPass));
		if (!vk_screen_drawer_set_quad_pipeline(&screenDrawer, &quadPipeline.p))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_screen_drawer_set_quad_pipeline");
	}

	SamplerManager samplerManager;
	vk_screen_drawer screenDrawer;
	vk_rtt_drawer textureDrawer;
};

const renderer_ops *rend_Vulkan(void)
{
	return &RendererClass<VulkanRenderer>::ops;
}
