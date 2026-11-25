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
#include <memory>
#include "rend/sorter.h"
#include "rend/transform_matrix.h"
#include "../vulkan.h"
#include "../buffer.h"
#include "../commandpool.h"
#include "oit_pipeline.h"
#include "oit_shaders.h"
#include "../texture.h"
#include "../quad.h"
#include "oit_buffer.h"
#include "../drawer.h"
#include "../motion_pass.h"
#include "vk_oit_drawer.h"

/* The C++ per-pixel drawers' view of a vk_oit_drawer (vk_oit_drawer.h) */
class OITDrawer : public BaseDrawer
{
public:
	OITDrawer() { vk_oit_drawer_construct(&o, &b, Kind(), this); }
	virtual ~OITDrawer() { vk_oit_drawer_term(&o); }
	bool Draw(const Texture *fogTexture, const Texture *paletteTexture)
	{
		const vk::CommandBuffer cmdBuffer = NewFrame();
		if (!vk_oit_drawer_draw(&o, (VkCommandBuffer)cmdBuffer, (VkImageView)fogTexture->GetImageView(),
				(VkImageView)paletteTexture->GetImageView()))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_oit_drawer_draw");
		return !pvrrc.isRTT;
	}

	virtual vk::CommandBuffer NewFrame() = 0;
	virtual void EndFrame() = 0;

	/* Clipped to the band being drawn, when a frame is drawn in bands */
	void SetScissor(const vk::CommandBuffer& cmdBuffer, const vk::Rect2D& scissor)
	{
		vk_oit_drawer_set_scissor(&o, (VkCommandBuffer)cmdBuffer, &static_cast<const VkRect2D&>(scissor));
	}

	/* The attachments made, for another drawer to draw into as well (see
	 * OITTextureDrawer::ShareAttachments()): they are at least this large */
	bool AttachmentsCover(u32 width, u32 height) const
	{
		return o.colorAttachments[0].image != VK_NULL_HANDLE && (int)width <= o.maxWidth && (int)height <= o.maxHeight;
	}
	FramebufferAttachment *ColorAttachment(int i) { return reinterpret_cast<FramebufferAttachment *>(&o.colorAttachments[i]); }
	FramebufferAttachment *DepthAttachment() { return reinterpret_cast<FramebufferAttachment *>(&o.depthAttachment); }
	vk_oit_drawer *Raw() { return &o; }

protected:
	// SSAA's channel sums, when the buffers have them (OITBuffers::SetSums())
	vk::ImageView GetSumsView() const { return vk::ImageView(o.buffers->sumsAttachment.view); }
	void Init(SamplerManager *samplerManager, OITPipelineManager *pipelineManager, OITBuffers *oitBuffers)
	{
		this->pipelineManager = pipelineManager;
		if (!vk_oit_drawer_init(&o, samplerManager->Raw(), pipelineManager->Raw(), &oitBuffers->o))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_oit_drawer_init");
	}
	void Term() { vk_oit_drawer_term(&o); }
	int GetCurrentImage() const { return o.imageIndex; }
	void NewImage() { vk_oit_drawer_new_image(&o); }
	OITDescriptorSets& GetCurrentDescSet() { return reinterpret_cast<OITDescriptorSets&>(*vk_oit_drawer_desc_set(&o)); }

	void MakeBuffers(int width, int height)
	{
		if (!vk_oit_drawer_make_buffers(&o, width, height))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_oit_drawer_make_buffers");
	}
	// (MakeBuffers()'s buffers alone)
	void InitBuffers(int width, int height)
	{
		if (!vk_oit_buffers_init(o.buffers, width, height, b.pool))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_oit_buffers_init");
	}
	/* (MakeBuffers()'s attachments and their framebuffers, at least
	 * @width by @height) */
	void MakeAttachments(int width, int height)
	{
		if (!vk_oit_drawer_make_attachments(&o, width, height))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_oit_drawer_make_attachments");
	}
	// ...and none: they go once the frames that may use them are done
	void DropAttachments() { vk_oit_drawer_drop_attachments(&o); }
	vk::DeviceSize GetIndexOffset() const { return o.offsets.indexOffset; }
	// (what the lists of fragments a render pass asked for: OITBuffers::CountPass())
	void CountPass(vk::CommandBuffer cmdBuffer) { vk_oit_buffers_count_pass(o.buffers, (VkCommandBuffer)cmdBuffer); }
	void SetScreenSize(int width, int height) { vk_oit_buffers_set_screen_size(o.buffers, width, height); }
	virtual vk::Format GetColorFormat() const = 0;
	virtual vk::Framebuffer GetFinalFramebuffer() const = 0;
	/* In the colour subpass of a frame's first pass, before any polygon:
	 * the screen's drawer puts the last picture there when the render will
	 * cover only part of it. True if it drew, and what was bound is gone. */
	virtual bool DrawLastPicture(vk::CommandBuffer) { return false; }
	FramebufferAttachment *Depth() { return reinterpret_cast<FramebufferAttachment *>(vk_oit_drawer_depth(&o)); }

	vk_oit_drawer o;
	/* (its members, as the C++ drawers name them) */
	vk::Rect2D& viewport = reinterpret_cast<vk::Rect2D&>(o.viewport);
	bool& upscaled = o.upscaled;
	int& maxWidth = o.maxWidth;
	int& maxHeight = o.maxHeight;
	vk::CommandBuffer& currentCommandBuffer = reinterpret_cast<vk::CommandBuffer&>(o.cmd);
	OITPipelineManager *pipelineManager = nullptr;

private:
	static VkFramebuffer FinalFramebufferOf(void *self) { return (VkFramebuffer)static_cast<OITDrawer *>(self)->GetFinalFramebuffer(); }
	static bool DrawLastPictureOf(void *self, VkCommandBuffer cmd)
	{
		return static_cast<OITDrawer *>(self)->DrawLastPicture(vk::CommandBuffer(cmd));
	}
	static const vk_oit_drawer_kind *Kind()
	{
		static const vk_oit_drawer_kind kind = { FinalFramebufferOf, DrawLastPictureOf };
		return &kind;
	}
};
static_assert(sizeof(FramebufferAttachment) == sizeof(vk_attachment) && sizeof(OITDescriptorSets) == sizeof(vk_oit_desc_sets)
		&& sizeof(vk::CommandBuffer) == sizeof(VkCommandBuffer), "the C++ views are their C structs");

class OITScreenDrawer : public OITDrawer
{
public:
	/* Renders to textures draw into this drawer's attachments too, which
	 * are then made large enough for them (vk_rtt_share_extent()):
	 * not with SSAA or the upscaler, when they are not of a single sample
	 * and of no use past a frame */
	bool SharesAttachments() const
	{
		return pipelineManager != nullptr && pipelineManager->GetSamples() == 1 && !upscaled
				&& GetColorFormat() == vk::Format::eR8G8B8A8Unorm;
	}
	// @samples: SSAA's (1: off), which the shader manager has been given
	// @samples: SSAA's (1: off), which the shader manager has been given
	void Init(SamplerManager *samplerManager, OITShaderManager *shaderManager, OITBuffers *oitBuffers, u32 samples)
	{
		// (the upscaler wants the depth kept, which SSAA's samples are not)
		upscaled = upscaler_active() && samples == 1;
		o.colorFormat = (VkFormat)GetColorFormat();
		this->shaderManager = shaderManager;
		if (!screenPipelineManager)
			screenPipelineManager = std::unique_ptr<OITPipelineManager>(new OITPipelineManager());
		screenPipelineManager->Init(shaderManager, oitBuffers, samples, upscaled);
		OITDrawer::Init(samplerManager, screenPipelineManager.get(), oitBuffers);

		MakeFramebuffers();
		if (upscaled)
		{
			std::vector<vk::ImageView> depthViews;
			for (u32 i = 0; i < o.depthAttachmentCount; i++)
				depthViews.push_back(vk::ImageView(o.depthAttachments[i].view));
			motionPass.Init(viewport.extent, GetContext()->GetDepthFormat(), vk::ImageLayout::eDepthStencilReadOnlyOptimal,
					depthViews, screenPipelineManager->GetPipelineLayout(),
					shaderManager->GetMotionVertexShader(), shaderManager->GetMotionShader(), commandPool);
		}
		else
			motionPass.Term(commandPool);
		upscaler_reset();
		timer.Init(GetContext()->GetSwapChainSize(), "per-pixel renderer");
	}
	// the subpass the screen's last things are drawn in (the VMUs, the OSD)
	int GetOsdSubpass() const { return screenPipelineManager->GetFinalSubpass(); }
	void Term()
	{
		timer.Term();
		writeOut.Term();
		motionPass.Term(nullptr);
		computeMotionPass.Term(nullptr);
		screenPipelineManager.reset();
		framebuffers.clear();
		finalColorAttachments.clear();
		carriedPicture.reset();
		restoredPicture = nullptr;
		lastPicture.Term();
		havePicture = false;
		OITDrawer::Term();
	}

	virtual vk::CommandBuffer NewFrame() override;
	virtual void EndFrame() override
	{
		havePicture = true;
		restoredPicture = nullptr;
		if (carriedPicture)
			commandPool->DeferDelete(std::move(carriedPicture));
		currentCommandBuffer.endRenderPass();
		if (overlaid)
			overlaid = false;
		else
			writeOut.Record(currentCommandBuffer, GetCurrentImage(), finalColorAttachments[GetCurrentImage()]->GetImage(),
					finalColorAttachments[GetCurrentImage()]->GetImageView(), viewport.extent.width, viewport.extent.height);
		if (motionPass.Recorded())
		{
			// the motion vectors over the depth the frame left (what went over the picture may have bound its own)
			GetCurrentDescSet().BindPerFrameDescriptorSets(currentCommandBuffer);
			motionPass.Record(currentCommandBuffer, GetCurrentImage(), geometry, GetIndexOffset(), baseScissor, geometryVertices);
		}
		CountPass(currentCommandBuffer);
		timer.End(currentCommandBuffer, commandPool->GetIndex());
		currentCommandBuffer.end();
		currentCommandBuffer = nullptr;
		/* (with SSAA, the samples' channel sums the last pass stored, when
		 * it did: no write-out pass came after it) */
		Present(screenPipelineManager->Sums() && !writeOut.Runs(viewport.extent.height) ? GetSumsView() : vk::ImageView());
		if (motionPass.Recorded())
			motionPass.Submit(GetCurrentImage(), Depth()->GetImage(), Depth()->GetImageView(), GetContext()->GetDepthFormat());
	}
	vk::RenderPass GetRenderPass() const { return screenPipelineManager->GetRenderPass(false, true); }
	/* The chip's write-out (../writeout.h), when the frame has one: the
	 * frame's render pass ended and the pass recorded now, before the VMUs
	 * and crosshairs, which are then drawn in the overlay's render pass
	 * (with a quad pipeline made for OverlayPass()); false if there is none */
	bool HasWriteOut() const { return (bool)OverlayPass(); }
	bool WriteOutBeforeOsd()
	{
		if (!writeOut.Runs(viewport.extent.height))
			return false;
		currentCommandBuffer.endRenderPass();
		writeOut.Record(currentCommandBuffer, GetCurrentImage(), finalColorAttachments[GetCurrentImage()]->GetImage(),
				finalColorAttachments[GetCurrentImage()]->GetImageView(), viewport.extent.width, viewport.extent.height);
		writeOut.BeginOverlay(currentCommandBuffer, GetCurrentImage());
		overlaid = true;
		return true;
	}
	vk::RenderPass OverlayPass() const { return writeOut.OverlayPass(); }
	vk::CommandBuffer GetCurrentCommandBuffer() const { return currentCommandBuffer; }

protected:
	virtual vk::Framebuffer GetFinalFramebuffer() const override { return *framebuffers[GetCurrentImage()]; }
	virtual vk::Format GetColorFormat() const override { return GetContext()->GetColorFormat(); }
	virtual bool DrawLastPicture(vk::CommandBuffer cmdBuffer) override
	{
		if ((!havePicture && !restoredPicture) || lastPicturePipeline == nullptr || pvrrc.clearFramebuffer || !matrices.IsClipped())
			return false;
		const int count = (int)finalColorAttachments.size();
		const std::array<float, 4> opaque = { 1.f, 1.f, 1.f, 1.f };

		SetScissor(cmdBuffer, viewport);
		cmdBuffer.setBlendConstants(opaque.data());
		lastPicturePipeline->BindPipeline(cmdBuffer);
		/* (the picture kept from another context can be of another size) */
		if (havePicture)
			lastPicture.Draw(cmdBuffer, finalColorAttachments[(GetCurrentImage() + count - 1) % count]->GetImageView(), nullptr, true, commandPool);
		else
		{
			QuadVertex area[4];
			lastPicture.Draw(cmdBuffer, restoredPicture, QuadGameArea(area, restoredSize, viewport.extent) ? area : nullptr, false, commandPool);
		}
		return true;
	}
public:
	/* rend/last_picture.h: the context is going */
	void KeepPicture();
	/* ...and in the new one, the picture that was kept is this image */
	void SetRestoredPicture(vk::ImageView view, vk::Extent2D size) { restoredPicture = view; restoredSize = size; }
	/* A quad pipeline made for the colour subpass of this drawer's render pass. */
	void SetLastPicturePipeline(QuadPipeline *pipeline)
	{
		lastPicturePipeline = pipeline;
		lastPicture.Init(pipeline);
	}

	/* The compute renderer draws frames into this drawer's pictures
	 * (vulkan/compute_drawer.h), which are then made for it to write too */
	void SetStoragePicture(bool on) { storagePicture = on; }
	/* Whether the frame in pvrrc can be drawn by it here; @last, whether it
	 * starts from the picture before, as DrawLastPicture() says, and
	 * @scaled, whether that is the picture kept from another context, which
	 * can be of another size and is filtered over the screen as this
	 * drawer's quad does */
	bool ComputeCanStart(bool *last, bool *scaled)
	{
		matrices.CalcMatrices(&pvrrc);
		*last = false;
		*scaled = false;
		if ((!havePicture && !restoredPicture) || lastPicturePipeline == nullptr || pvrrc.clearFramebuffer || !matrices.IsClipped())
			return true;
		*last = true;
		*scaled = !havePicture;
		return true;
	}
	struct ComputeTarget
	{
		vk::Image image;
		vk::ImageView view;
		vk::ImageView last;	// the picture before, with @last
	};
	// the picture it draws the frame in
	ComputeTarget ComputeBegin(bool last, bool scaled)
	{
		NewImage();
		const int count = (int)finalColorAttachments.size();
		ComputeTarget t;
		t.image = finalColorAttachments[GetCurrentImage()]->GetImage();
		t.view = finalColorAttachments[GetCurrentImage()]->GetImageView();
		if (last)
			t.last = scaled ? restoredPicture : finalColorAttachments[(GetCurrentImage() + count - 1) % count]->GetImageView();
		return t;
	}
	// ...and once it is recorded, as EndFrame() ends one of this drawer's (@sums: ComputeDrawer::Sums())
	void ComputeEnd(vk::ImageView sums)
	{
		havePicture = true;
		restoredPicture = nullptr;
		if (carriedPicture)
			commandPool->DeferDelete(std::move(carriedPicture));
		Present(sums);
	}
	/* The temporal upscaler with the compute renderer (vulkan/compute_drawer.h):
	 * its motion vector pass, drawn in the compute frame's command buffer
	 * over the depth the walk stored (@depthViews, one a slot of the pool,
	 * which the pass is indexed by), with this drawer's vertex uniforms;
	 * null when the upscaler has no frames */
	MotionVectorPass *ComputeMotionPass(const std::vector<vk::ImageView>& depthViews)
	{
		if (!upscaled || depthViews.empty())
		{
			computeMotionPass.Term(commandPool);
			return nullptr;
		}
		std::vector<vk::Buffer> uniforms;
		if (!vk_base_drawer_make_vertex_uniforms(&b, (u32)depthViews.size()))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_base_drawer_make_vertex_uniforms");
		for (size_t i = 0; i < depthViews.size(); i++)
			uniforms.push_back(vk::Buffer(b.vertexUniformBuffers[i].buffer));
		computeMotionPass.InitSampled(viewport.extent, depthViews, uniforms, sizeof(OITDescriptorSets::VertexShaderUniforms),
				shaderManager->GetMotionVertexShader(), shaderManager->GetMotionDepthShader(), commandPool);
		return &computeMotionPass;
	}
	/* Once the compute frame is prepared (jittered as this drawer's matrices
	 * are here): the strips matched and the vertex uniforms written, for
	 * the motion vector pass of pool slot @slot */
	void ComputeMotionBegin(int slot)
	{
		if (!upscaled || !computeMotionPass.Begin(slot))
			return;
		float jx, jy;
		upscaler_jitter(&jx, &jy);
		matrices.Jitter(jx, jy);
		WriteVertexUniforms(vk::CommandBuffer(), slot);
	}
	// ...and after it is recorded: the vectors and the depth to the upscaler
	void ComputeMotionEnd(int slot, vk::Image depthImage, vk::ImageView depthView)
	{
		if (computeMotionPass.Recorded())
			computeMotionPass.Submit(slot, depthImage, depthView, vk::Format::eR32Sfloat);
	}
protected:
	/* The frame submitted and its picture handed over: with the HDR output,
	 * the picture encoded first, in the same submission (VulkanContext::HdrEncode()),
	 * from the channel sums @sums where there are */
	void Present(vk::ImageView sums)
	{
		vk::Image image = finalColorAttachments[GetCurrentImage()]->GetImage();
		vk::ImageView view = finalColorAttachments[GetCurrentImage()]->GetImageView();
		const bool hdr = GetContext()->HdrEncode(*commandPool, image, view,
				vk::Extent2D(viewport.extent.width, viewport.extent.height), sums);
		commandPool->EndFrame();
		GetContext()->PresentFrame(image, view, vk::Offset2D(viewport.extent.width, viewport.extent.height), hdr);
	}

private:
	void MakeFramebuffers();

	std::vector<std::unique_ptr<FramebufferAttachment>> finalColorAttachments;
	QuadPipeline *lastPicturePipeline = nullptr;
	MotionVectorPass motionPass;
	MotionVectorPass computeMotionPass;
	OITShaderManager *shaderManager = nullptr;
	QuadDrawer lastPicture;
	bool havePicture = false;	/* the image before this one holds the last frame */
	vk::ImageView restoredPicture;	/* ...or this does: the picture kept from the context before */
	vk::Extent2D restoredSize;		// (its size: of another shape, its 4:3 drawn where this one's is)
	/* ...or the image the last frame was drawn to at the size before this one (see the per-triangle screen drawer) */
	std::unique_ptr<FramebufferAttachment> carriedPicture;
	std::vector<vk::UniqueFramebuffer> framebuffers;
	std::unique_ptr<OITPipelineManager> screenPipelineManager;
	GpuFrameTimer timer;
	bool storagePicture = false;
	// the chip's write-out over the pictures (../writeout.h), which are then made for it to write
	VulkanWriteOut writeOut;
	bool overlaid = false;		// this frame's write-out was recorded before the overlays
};

class OITTextureDrawer : public OITDrawer
{
public:
	void Init(SamplerManager *samplerManager, OITShaderManager *shaderManager,
			TextureCache *textureCache, OITBuffers *oitBuffers)
	{
		if (!rttPipelineManager)
			rttPipelineManager = std::unique_ptr<RttOITPipelineManager>(new RttOITPipelineManager());
		rttPipelineManager->Init(shaderManager, oitBuffers);
		OITDrawer::Init(samplerManager, rttPipelineManager.get(), oitBuffers);

		this->textureCache = textureCache;
		// (a framebuffer's place for each image; the attachments: ShareAttachments())
		framebuffers.resize(GetContext()->GetSwapChainSize());
	}
	/* The screen's drawer's attachments, which this draws into when they
	 * are large enough (OITScreenDrawer::SharesAttachments(), and
	 * vk_rtt_share_extent()); made of its own, as large as
	 * vk_rtt_common_render(), where there are none to share - now,
	 * not while the game runs. Said after the screen's drawer is made, or
	 * made again. */
	void ShareAttachments(OITScreenDrawer *screen)
	{
		this->screen = screen;
		o.attachmentsOf = &o;
		if (screen != nullptr && screen->SharesAttachments())
			DropAttachments();
		else
		{
			const vk::Extent2D common = vk::Extent2D(vk_rtt_common_render());
			MakeAttachments((int)common.width, (int)common.height);
		}
	}
	void Term()
	{
		if (drewShared + drewOwn != 0)
			NOTICE_LOG(RENDERER, "Renders to textures: %u drawn with the screen's attachments, %u with their own; %u cleared outside\n",
					drewShared, drewOwn, vk_texture_outside_cleared);
		vk_texture_outside_cleared = 0;
		drewShared = drewOwn = 0;
		framebuffers.clear();
		rttPipelineManager.reset();
		OITDrawer::Term();
	}

	virtual void EndFrame() override;

	/* The compute renderer draws renders to a texture into the texture this
	 * drawer would (vulkan/compute_drawer.h): its images are then made for
	 * it to write too */
	void SetStorageTarget(bool on) { storageUsage = on ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlags(); }
	// the size of the texture the render in pvrrc goes to
	static vk::Extent2D ComputeSize()
	{
		const Size s = GetSize();
		return vk::Extent2D(s.widthPow2, s.heightPow2);
	}
	struct ComputeTarget
	{
		vk::Image image;
		vk::ImageView view;
	};
	// the texture it draws the render in, as NewFrame() takes it...
	ComputeTarget ComputeBegin();
	// ...and once it is recorded, as EndFrame() ends one of this drawer's
	void ComputeEnd();

protected:
	virtual vk::CommandBuffer NewFrame() override;
	virtual vk::Framebuffer GetFinalFramebuffer() const override { return framebuffers[GetCurrentImage()].Handle(); }
	virtual vk::Format GetColorFormat() const override { return vk::Format::eR8G8B8A8Unorm; }

private:
	/* The render's sizes: the frame from its first pixel to the pixel
	 * clip's last, the clip, and the texture, a power of two, all times
	 * Render To Texture Upscaling */
	struct Size
	{
		u32 origWidth, origHeight;
		u32 clipX, clipY, clipWidth, clipHeight;
		u32 upscaledWidth, upscaledHeight;
		u32 widthPow2, heightPow2;
	};
	static Size GetSize();
	/* The texture the render goes to, taken from the cache and given an
	 * image to render to, with @usage too; the layout it is in */
	vk::ImageLayout TakeTexture(const Size& size, const vk::ImageUsageFlags& usage);
	// the render is submitted: video memory gets it when something first touches it there
	void Watch();

	u32 textureAddr = 0;
	OITScreenDrawer *screen = nullptr;	// whose attachments are shared
	bool saidLarger = false;
	u32 drewShared = 0, drewOwn = 0;	// (said when it goes: tools/vulkan/vk_vram.sh)
	vk::ImageUsageFlags storageUsage;

	Texture *texture = nullptr;
	vk::Image colorImage;
	std::vector<KeptFramebuffer> framebuffers;
	std::unique_ptr<RttOITPipelineManager> rttPipelineManager;

	TextureCache *textureCache = nullptr;
};
