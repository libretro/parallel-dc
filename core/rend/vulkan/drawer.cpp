#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
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
#include <math.h>
#include "rtt_read.h"
#include "rend/last_picture.h"
#include "drawer.h"
#include "vk_drawer_base.h"
#include "hw/pvr/pvr_mem.h"

void TextureDrawer::Init(SamplerManager *samplerManager, ShaderManager *shaderManager, TextureCache *textureCache, u32 samples)
{
	if (!rttPipelineManager)
		rttPipelineManager = std::unique_ptr<RttPipelineManager>(new RttPipelineManager());
	rttPipelineManager->Init(shaderManager, samples);
	Drawer::Init(samplerManager, rttPipelineManager.get());

	this->textureCache = textureCache;
	if (samples != this->samples)
	{
		// (the depth attachment is of the other kind: one of the right kind is made below or when first needed)
		if (depthAttachment && commandPool != nullptr)
			commandPool->DeferDelete(std::move(depthAttachment));
		depthAttachment.reset();
		if (samplesAttachment && commandPool != nullptr)
			commandPool->DeferDelete(std::move(samplesAttachment));
		samplesAttachment.reset();
		this->samples = samples;
	}
	/* The attachments are made now, never while the game runs: SSAA's, as
	 * large as a render to a texture can be (where SSAA is done to them,
	 * memory made only if it is needed backs them); the depth is the
	 * screen's, or one of its own (ShareDepth()). And a framebuffer's
	 * place for each image. */
	if (samples > 1)
	{
		if (!samplesAttachment || samplesAttachment->getExtent().width < MaxSide())
			MakeSsaaAttachments(MaxSide());
		ssaaResolve.Init(rttPipelineManager->GetRenderPass(), 1, samples, samplesAttachment->GetImageView());
	}
	else
		ssaaResolve.Term();
	framebuffers.resize(GetContext()->GetSwapChainSize());
}

void TextureDrawer::ShareDepth(const ScreenDrawer *screen)
{
	this->screen = screen;
	if (samples > 1)
		return;
	const FramebufferAttachment *shared = screen != nullptr ? screen->SharedDepth() : nullptr;
	if (shared != nullptr)
	{
		// (its own goes once the frames that may use it are done)
		if (depthAttachment && commandPool != nullptr)
			commandPool->DeferDelete(std::move(depthAttachment));
		depthAttachment.reset();
		return;
	}
	const vk::Extent2D common = CommonRender();
	if (!depthAttachment || depthAttachment->getExtent().width < common.width || depthAttachment->getExtent().height < common.height)
	{
		if (depthAttachment && commandPool != nullptr)
			commandPool->DeferDelete(std::move(depthAttachment));
		depthAttachment = std::unique_ptr<FramebufferAttachment>(new FramebufferAttachment(GetContext()->GetPhysicalDevice(),
				GetContext()->GetDevice()));
		depthAttachment->Init(common.width, common.height, GetContext()->GetDepthFormat(),
				vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransientAttachment);
	}
}

void TextureDrawer::MakeSsaaAttachments(u32 side)
{
	VulkanContext *context = GetContext();
	if (depthAttachment && commandPool != nullptr)
		commandPool->DeferDelete(std::move(depthAttachment));
	if (samplesAttachment && commandPool != nullptr)
		commandPool->DeferDelete(std::move(samplesAttachment));
	const vk::SampleCountFlagBits n = (vk::SampleCountFlagBits)samples;
	depthAttachment = std::unique_ptr<FramebufferAttachment>(new FramebufferAttachment(context->GetPhysicalDevice(), context->GetDevice()));
	depthAttachment->Init(side, side, context->GetDepthFormat(),
			vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransientAttachment, true, n);
	samplesAttachment = std::unique_ptr<FramebufferAttachment>(new FramebufferAttachment(context->GetPhysicalDevice(), context->GetDevice()));
	samplesAttachment->Init(side, side, vk::Format::eR8G8B8A8Unorm,
			vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment | vk::ImageUsageFlagBits::eTransientAttachment,
			false, n);
	if (ssaaResolve.Ready() && commandPool != nullptr)
		ssaaResolve.SetSamplesView(samplesAttachment->GetImageView(), commandPool);
}

vk::CommandBuffer TextureDrawer::BeginRenderPass()
{
	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d @ %08x\n", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max, FB_W_SOF1 & VRAM_MASK);
	matrices.CalcMatrices(&pvrrc);

	textureAddr = FB_W_SOF1 & VRAM_MASK;
	// the frame from its first pixel, as far as the pixel clip goes (see TransformMatrix)
	u32 origWidth = pvrrc.fb_X_CLIP.max + 1;
	u32 origHeight = pvrrc.fb_Y_CLIP.max + 1;
	const u32 clipX = pvrrc.fb_X_CLIP.min, clipY = pvrrc.fb_Y_CLIP.min;
	const u32 clipWidth = origWidth - clipX, clipHeight = origHeight - clipY;
	u32 upscaledWidth = origWidth;
	u32 upscaledHeight = origHeight;
	u32 heightPow2 = 8;
	while (heightPow2 < upscaledHeight)
		heightPow2 *= 2;
	u32 widthPow2 = 8;
	while (widthPow2 < upscaledWidth)
		widthPow2 *= 2;

	if (settings.rend.RenderToTextureUpscale > 1)
	{
		upscaledWidth *= settings.rend.RenderToTextureUpscale;
		upscaledHeight *= settings.rend.RenderToTextureUpscale;
		widthPow2 *= settings.rend.RenderToTextureUpscale;
		heightPow2 *= settings.rend.RenderToTextureUpscale;
	}

	VulkanContext *context = GetContext();
	vk::Device device = context->GetDevice();

	NewImage();
	vk::CommandBuffer commandBuffer = commandPool->Allocate();
	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	vertexUniformBuffer = WriteVertexUniforms(commandBuffer, GetCurrentImage());

	if (samples > 1)
	{
		/* (SSAA's attachments were made as large as a texture rendered to
		 * can be; a pixel clip beyond the texture's largest size makes one
		 * larger, which is the only time they are made while a game runs) */
		const u32 side = std::max(widthPow2, heightPow2);
		if (side > samplesAttachment->getExtent().width)
		{
			WARN_LOG(RENDERER, "Render to a texture of %u x %u: SSAA's attachments made larger\n", widthPow2, heightPow2);
			MakeSsaaAttachments(side);
		}
	}
	/* What the render draws: its own pixels, the texture's from the first;
	 * with SSAA all of the texture, its attachments being that large */
	const u32 areaWidth = samples > 1 ? widthPow2 : upscaledWidth;
	const u32 areaHeight = samples > 1 ? heightPow2 : upscaledHeight;
	/* The depth: the screen's when it is large enough - drawn into by the
	 * screen's frames and this one by one, on the same queue, each
	 * clearing it first: the render passes' dependencies on the depth
	 * before them are those of one frame after another -, or this
	 * drawer's own; which a render larger than both has made for it. */
	const FramebufferAttachment *depth = nullptr;
	if (samples > 1)
		depth = depthAttachment.get();
	else
	{
		const FramebufferAttachment *shared = screen != nullptr ? screen->SharedDepth() : nullptr;
		if (shared != nullptr && areaWidth <= shared->getExtent().width && areaHeight <= shared->getExtent().height)
		{
			depth = shared;
			drewShared++;
		}
		else
		{
			drewOwn++;
			if (!depthAttachment || areaWidth > depthAttachment->getExtent().width || areaHeight > depthAttachment->getExtent().height)
			{
				if (!saidLarger)
				{
					saidLarger = true;
					WARN_LOG(RENDERER, "Render to a texture of %u x %u: a depth made for it\n", areaWidth, areaHeight);
				}
				const u32 w = depthAttachment ? std::max(areaWidth, depthAttachment->getExtent().width) : areaWidth;
				const u32 h = depthAttachment ? std::max(areaHeight, depthAttachment->getExtent().height) : areaHeight;
				if (depthAttachment)
					commandPool->DeferDelete(std::move(depthAttachment));
				depthAttachment = std::unique_ptr<FramebufferAttachment>(new FramebufferAttachment(context->GetPhysicalDevice(), device));
				depthAttachment->Init(w, h, GetContext()->GetDepthFormat(),
						vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransientAttachment);
			}
			depth = depthAttachment.get();
		}
	}
	vk::Image colorImage;
	vk::ImageView colorImageView;
	vk::ImageLayout colorImageCurrentLayout;

	/* what an earlier render left waiting in this memory, while its picture is still what it was: see rtt_read.h */
	vk_rtt_supersede(textureAddr, clipX, clipY, clipWidth, clipHeight);
	// TexAddr : fb_rtt.TexAddr, Reserved : 0, StrideSel : 0, ScanOrder : 1
	TCW tcw = { { textureAddr >> 3, 0, 0, 1 } };
	switch (FB_W_CTRL.fb_packmode) {
	case 0:
	case 3:
		tcw.PixelFmt = Pixel1555;
		break;
	case 1:
		tcw.PixelFmt = Pixel565;
		break;
	case 2:
		tcw.PixelFmt = Pixel4444;
		break;
	}

	TSP tsp = { 0 };
	for (tsp.TexU = 0; tsp.TexU <= 7 && (8u << tsp.TexU) < origWidth; tsp.TexU++);
	for (tsp.TexV = 0; tsp.TexV <= 7 && (8u << tsp.TexV) < origHeight; tsp.TexV++);

	texture = textureCache->getTextureCacheData(tsp, tcw);
	if (texture->IsNew())
	{
		texture->Create();
		texture->SetPhysicalDevice(GetContext()->GetPhysicalDevice());
		texture->SetDevice(device);
	}
	else if (textureCache->IsInFlight(texture))
	{
		texture->KeepReadOnlyView();
		textureCache->DestroyLater(texture);
	}
	textureCache->SetInFlight(texture);

	/* A texture the game uploaded can become one it renders to, at the
	 * same address, the same size and in this format: its image was not
	 * made to be rendered to, nor to be copied from, and is made again. */
	const vk::ImageUsageFlags renderTargetUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
			| vk::ImageUsageFlagBits::eTransferSrc;	// read back when the game touches it in video memory
	if (texture->format != VK_FORMAT_R8G8B8A8_UNORM || texture->extent.width != widthPow2 || texture->extent.height != heightPow2
			|| (vk::ImageUsageFlags(texture->imageUsage) & renderTargetUsage) != renderTargetUsage)
	{
		texture->extent.width = widthPow2;
		texture->extent.height = heightPow2;
		texture->format = VK_FORMAT_R8G8B8A8_UNORM;
		texture->needsStaging = true;
		texture->CreateRenderTarget(renderTargetUsage);
		colorImageCurrentLayout = vk::ImageLayout::eUndefined;
	}
	else
	{
		colorImageCurrentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	colorImage = texture->GetImage();
	colorImageView = texture->GetImageView();
	width = areaWidth;
	height = areaHeight;
	if (samples == 1)
		texture->ClearOutside(commandBuffer, areaWidth, areaHeight, colorImageCurrentLayout);

	setImageLayout(commandBuffer, colorImage, vk::Format::eR8G8B8A8Unorm, 1, colorImageCurrentLayout, vk::ImageLayout::eColorAttachmentOptimal);

	// (with SSAA: the texture, then the samples' colour and depth)
	vk::ImageView imageViews[] = {
		colorImageView,
		samples > 1 ? samplesAttachment->GetImageView() : depth->GetImageView(),
		depth->GetImageView(),
	};
	const u64 viewSerials[] = {
		texture->GetViewSerial(),
		samples > 1 ? samplesAttachment->GetViewSerial() : depth->GetViewSerial(),
		depth->GetViewSerial(),
	};
	/* (made again only for other attachments: a texture rendered to every
	 * frame is the same one) */
	framebuffers.resize(GetContext()->GetSwapChainSize());
	const vk::Framebuffer framebuffer = framebuffers[GetCurrentImage()].Get(rttPipelineManager->GetRenderPass(),
			samples > 1 ? 3 : 2, imageViews, viewSerials, width, height);

	const vk::ClearValue clear_colors[] = { vk::ClearColorValue(std::array<float, 4> { 0.f, 0.f, 0.f, 1.f }), vk::ClearDepthStencilValue { 0.f, 0 } };
	const vk::ClearValue ssaa_clear_colors[] = { clear_colors[0], clear_colors[0], clear_colors[1] };
	commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(rttPipelineManager->GetRenderPass(), framebuffer,
			vk::Rect2D( { 0, 0 }, { width, height }), samples > 1 ? 3 : 2, samples > 1 ? ssaa_clear_colors : clear_colors),
			vk::SubpassContents::eInline);
	commandBuffer.setViewport(0, vk::Viewport(0.0f, 0.0f, (float)upscaledWidth, (float)upscaledHeight, 0.0f, 1.0f));
	{
		// only the pixel clip is written
		const u32 scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
		baseScissor = vk::Rect2D(vk::Offset2D(clipX * scale, clipY * scale), vk::Extent2D(clipWidth * scale, clipHeight * scale));
	}
	commandBuffer.setScissor(0, baseScissor);
	currentCommandBuffer = commandBuffer;

	return commandBuffer;
}

void TextureDrawer::EndRenderPass()
{
	currentCommandBuffer.endRenderPass();

	currentCommandBuffer.end();

	currentCommandBuffer = nullptr;
	commandPool->EndFrame();

	texture->tc.dirty = 0;
	texture->tc.stride_read = 0;	// (rendered, not read)
	texture->tc.made_type = 0;
   libCore_vramlock_Lock(texture->tc.sa_tex, texture->tc.sa + texture->tc.size - 1, &texture->tc);
	/* video memory gets it when something first touches it there */
	vk_rtt_watch(texture, textureAddr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
}

void ScreenDrawer::Init(SamplerManager *samplerManager, ShaderManager *shaderManager, u32 samples)
{
	this->shaderManager = shaderManager;
	/* (a change of SSAA is one of size: everything made for the size is
	 * made again; so is one of the write-out, whose pictures are made for
	 * it to write) */
	// (the upscaler wants the depth kept, which SSAA's samples are not)
	const bool upscaled = upscaler_active() && samples == 1;
	const bool remade = viewport != GetContext()->GetViewPort() || samples != this->samples
			|| storagePictures != VulkanWriteOut::Wanted() || upscaled != this->upscaled;
	if (remade)
	{
		/* Not destroyed on the spot: the last picture was handed to the
		 * frontend, which shows it again for as long as no new one comes -
		 * and may, before the first frame at the new size is drawn. They
		 * go when the frames that could still use them have gone by.
		 * (Upstream: a crash starting the 240p test suite.) */
		if (commandPool != nullptr)
		{
			/* The image with the last frame does not go with the rest. A
			 * game that draws part of the screen has the rest from the
			 * last picture, and this is it: the next render starts from it
			 * as it does from one kept across a new context, at whatever
			 * size the screen now is - and here nothing is read back or
			 * loaded again. It goes after the next frame. */
			if (carriedPicture)
				commandPool->DeferDelete(std::move(carriedPicture));
			if (havePicture && (size_t)GetCurrentImage() < colorAttachments.size() && colorAttachments[GetCurrentImage()])
			{
				carriedPicture = std::move(colorAttachments[GetCurrentImage()]);
				restoredPicture = carriedPicture->GetImageView();
				restoredSize = carriedPicture->getExtent();
			}
			for (auto& framebuffer : framebuffers)
				if (framebuffer)
					commandPool->DeferDelete(std::unique_ptr<vk::UniqueFramebuffer>(new vk::UniqueFramebuffer(std::move(framebuffer))));
			for (auto& attachment : colorAttachments)
				if (attachment)
					commandPool->DeferDelete(std::move(attachment));
			for (auto& attachment : hdrAttachments)
				if (attachment)
					commandPool->DeferDelete(std::move(attachment));
			if (depthAttachment)
				commandPool->DeferDelete(std::move(depthAttachment));
			if (samplesAttachment)
				commandPool->DeferDelete(std::move(samplesAttachment));
			for (auto& attachment : depthAttachments)
				if (attachment)
					commandPool->DeferDelete(std::move(attachment));
			motionPass.Term(commandPool);
		}
		framebuffers.clear();
		colorAttachments.clear();
		hdrAttachments.clear();
		depthAttachment.reset();
		samplesAttachment.reset();
		depthAttachments.clear();
		havePicture = false;
		if (samples != this->samples || upscaled != this->upscaled)
			// (Resize() waited for the frames on the GPU: nothing uses it)
			renderPass.reset();
		this->samples = samples;
		this->upscaled = upscaled;
		storagePictures = VulkanWriteOut::Wanted();
		upscaler_reset();
	}
	viewport = GetContext()->GetViewPort();
	const vk::SampleCountFlagBits n = (vk::SampleCountFlagBits)samples;
	if (!depthAttachment && !upscaled)
	{
		/* (without SSAA, renders to textures draw into it too: as large as
		 * they need, TextureDrawer::ShareExtent()) */
		const vk::Extent2D depthSize = samples == 1 ? TextureDrawer::ShareExtent(viewport) : viewport;
		depthAttachment = std::unique_ptr<FramebufferAttachment>(
			new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice()));
		depthAttachment->Init(depthSize.width, depthSize.height, GetContext()->GetDepthFormat(),
				vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransientAttachment, true, n);
	}
	// SSAA: the samples' colour, which the last subpass resolves into the picture
	if (samples > 1 && !samplesAttachment)
	{
		samplesAttachment = std::unique_ptr<FramebufferAttachment>(
			new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice()));
		samplesAttachment->Init(viewport.width, viewport.height, GetContext()->GetColorFormat(),
				vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment
					| vk::ImageUsageFlagBits::eTransientAttachment, false, n);
	}

	/* The HDR output: a last subpass encodes the picture, its input, into
	 * the HDR image, the frontend's (hdr_encode_vk.h) */
	const bool hdr = GetContext()->HdrActive();
	if (!renderPass && samples > 1)
	{
		renderPass = MakeSsaaRenderPass(GetContext()->GetColorFormat(), samples, vk::ImageLayout::eShaderReadOnlyOptimal,
				hdr ? GetContext()->GetHdrFormat() : vk::Format::eUndefined);
		hdrPipeline.reset();
	}
	if (!renderPass)
	{
		hdrPipeline.reset();
		vk::AttachmentDescription attachmentDescriptions[] = {
				// Color attachment
				vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetContext()->GetColorFormat(), vk::SampleCountFlagBits::e1,
						vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
						vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
						vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal),
				// Depth attachment (kept for the upscaler, when there is one)
				vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetContext()->GetDepthFormat(), vk::SampleCountFlagBits::e1,
						vk::AttachmentLoadOp::eClear, upscaled ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare,
						vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
						vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal),
				// The HDR output's image
				vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetContext()->GetHdrFormat(), vk::SampleCountFlagBits::e1,
						vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
						vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
						vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal),
		};
		vk::AttachmentReference colorReference(0, vk::ImageLayout::eColorAttachmentOptimal);
		vk::AttachmentReference depthReference(1, vk::ImageLayout::eDepthStencilAttachmentOptimal);
		vk::AttachmentReference pictureInput(0, vk::ImageLayout::eShaderReadOnlyOptimal);
		vk::AttachmentReference hdrReference(2, vk::ImageLayout::eColorAttachmentOptimal);

		vk::SubpassDescription subpasses[] = {
				vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
						0, nullptr,
						1, &colorReference,
						nullptr,
						&depthReference),
				// the HDR output: the picture encoded where it is
				vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
						1, &pictureInput,
						1, &hdrReference,
						nullptr,
						nullptr),
		};

		std::vector<vk::SubpassDependency> dependencies;
		/* (the frontend reads the picture anywhere, from a render pass of its
		 * own or a copy: not by region, or it reads tiles not drawn yet) */
		dependencies.emplace_back(0, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer,
				vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead);
		/* The depth attachment is the same one every frame: the frame before
		 * has to be done with it before this one clears it. Nothing said
		 * so. (The per-pixel renderer's render pass has the same, from
		 * upstream.) */
		// (the colour attachment is in it as well: once one dependency into the
		// subpass is spelled out, the one Vulkan would have assumed for the rest is gone)
		dependencies.emplace_back(VK_SUBPASS_EXTERNAL, 0,
				vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests
					| vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests
					| vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::AccessFlagBits::eDepthStencilAttachmentWrite | vk::AccessFlagBits::eColorAttachmentWrite,
				vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite
					| vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite);
		/* The picture is cleared and drawn into an image the frontend was
		 * handed before, on the queue the two share: whatever read it there -
		 * a shader, a compute upscaler, a copy - is done before this clears it,
		 * or a GPU that draws in tiles shows tiles of the next frame with only
		 * its opaque polygons drawn yet. */
		dependencies.emplace_back(VK_SUBPASS_EXTERNAL, 0,
				vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer,
				vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::AccessFlags(), vk::AccessFlagBits::eColorAttachmentWrite);
		// the depth the frame leaves, for the motion vector pass and the upscaler
		if (upscaled)
			dependencies.emplace_back(0, VK_SUBPASS_EXTERNAL,
					vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
					vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests
						| vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
					vk::AccessFlagBits::eDepthStencilAttachmentWrite,
					vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eShaderRead);
		if (hdr)
		{
			// the picture drawn, then encoded where it is...
			dependencies.emplace_back(0, 1, vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
					vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eInputAttachmentRead, vk::DependencyFlagBits::eByRegion);
			// ...into the image the frontend was done with...
			dependencies.emplace_back(VK_SUBPASS_EXTERNAL, 1, vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eTransfer,
					vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::AccessFlags(), vk::AccessFlagBits::eColorAttachmentWrite);
			// ...and which it is handed to read
			dependencies.emplace_back(1, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eTransfer,
					vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead);
		}

		renderPass = GetContext()->GetDevice().createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(),
				ARRAY_SIZE(attachmentDescriptions) - (hdr ? 0 : 1), attachmentDescriptions,
				ARRAY_SIZE(subpasses) - (hdr ? 0 : 1), subpasses,
				dependencies.size(), dependencies.data()));
	}
	if (hdr && !hdrPipeline)
		hdrPipeline = GetContext()->HdrSubpassPipeline(*renderPass, samples > 1 ? 2 : 1, samples);
	size_t size = VulkanContext::Instance()->GetSwapChainSize();
	if (colorAttachments.size() > size)
	{
		colorAttachments.resize(size);
		hdrAttachments.resize(hdr ? size : 0);
		framebuffers.resize(size);
		if (upscaled)
			depthAttachments.resize(size);
	}
	else
	{
		// (with SSAA: the picture, then the samples' colour and depth; then the HDR output's image)
		vk::ImageView attachments[] = {
				nullptr,
				samples > 1 ? samplesAttachment->GetImageView() : upscaled ? vk::ImageView() : depthAttachment->GetImageView(),
				samples > 1 ? depthAttachment->GetImageView() : nullptr,
				nullptr,
		};
		const u32 count = (samples > 1 ? 3 : 2) + (hdr ? 1 : 0);
		while (colorAttachments.size() < size)
		{
			if (upscaled)
			{
				// the frame's depth, kept, sampled and copied from (the frontend reads it as it likes)
				depthAttachments.push_back(std::unique_ptr<FramebufferAttachment>(
						new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice())));
				depthAttachments.back()->Init(viewport.width, viewport.height, GetContext()->GetDepthFormat(),
						vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled
						| vk::ImageUsageFlagBits::eTransferSrc, false);
				attachments[1] = depthAttachments.back()->GetImageView();
			}
			colorAttachments.push_back(std::unique_ptr<FramebufferAttachment>(
					new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice())));
			// (copied from when the context goes, to keep the picture: rend/last_picture.h; the HDR output's input)
			colorAttachments.back()->Init(viewport.width, viewport.height, GetContext()->GetColorFormat(),
					vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc
					| (storagePictures ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlags())
					| (hdr ? vk::ImageUsageFlagBits::eInputAttachment : vk::ImageUsageFlags()), false);
			attachments[0] = colorAttachments.back()->GetImageView();
			if (hdr)
			{
				hdrAttachments.push_back(std::unique_ptr<FramebufferAttachment>(
						new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice())));
				hdrAttachments.back()->Init(viewport.width, viewport.height, GetContext()->GetHdrFormat(),
						vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc, false);
				attachments[count - 1] = hdrAttachments.back()->GetImageView();
			}
			vk::FramebufferCreateInfo createInfo(vk::FramebufferCreateFlags(), *renderPass,
					count, attachments, viewport.width, viewport.height, 1);
			framebuffers.push_back(GetContext()->GetDevice().createFramebufferUnique(createInfo));
		}
	}

	if (!screenPipelineManager)
		screenPipelineManager = std::unique_ptr<PipelineManager>(new PipelineManager());
	screenPipelineManager->Init(shaderManager, *renderPass, samples);
	Drawer::Init(samplerManager, screenPipelineManager.get());
	if (samples > 1)
	{
		ssaaResolve.Init(*renderPass, 1, samples, samplesAttachment->GetImageView());
		msQuadPipeline.Init(shaderManager, *renderPass, 0, samples);
	}
	if (upscaled)
	{
		std::vector<vk::ImageView> depthViews;
		for (const auto& attachment : depthAttachments)
			depthViews.push_back(attachment->GetImageView());
		motionPass.Init(viewport, GetContext()->GetDepthFormat(), vk::ImageLayout::eDepthStencilAttachmentOptimal,
				depthViews, screenPipelineManager->GetPipelineLayout(),
				shaderManager->GetMotionVertexShader(), shaderManager->GetMotionShader(), commandPool);
	}
	timer.Init(GetContext()->GetSwapChainSize(), "per-triangle renderer");
	if (remade)
	{
		std::vector<vk::ImageView> views;
		for (const auto& attachment : colorAttachments)
			views.push_back(attachment->GetImageView());
		writeOut.Init(storagePictures ? views : std::vector<vk::ImageView>(), viewport);
	}
}

vk::CommandBuffer ScreenDrawer::BeginRenderPass()
{
	NewImage();
	vk::CommandBuffer commandBuffer = commandPool->Allocate();
	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	matrices.CalcMatrices(&pvrrc);
	// the frame drawn under this frame's jitter, its strips matched for the motion vector pass
	if (upscaled && motionPass.Begin(GetCurrentImage()))
	{
		float jx, jy;
		upscaler_jitter(&jx, &jy);
		matrices.Jitter(jx, jy);
	}
	vertexUniformBuffer = WriteVertexUniforms(commandBuffer, GetCurrentImage());
	// (the HDR output's transform, for the render pass's last subpass)
	if (hdrPipeline)
		GetContext()->HdrPrepare(*commandPool, commandBuffer);

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour, which is what the OpenGL renderers clear it to; any other
	 * starts black, and has the last picture put into it below. */
	const std::array<float, 4> clear_color = pvrrc.clearFramebuffer
			? std::array<float, 4> { VO_BORDER_COL.Red / 255.f, VO_BORDER_COL.Green / 255.f, VO_BORDER_COL.Blue / 255.f, 1.f }
			: std::array<float, 4> { 0.f, 0.f, 0.f, 1.f };
	timer.Begin(commandBuffer, commandPool->GetIndex());
	// (with SSAA: the picture, all of which the resolve writes, then the samples' colour and depth)
	const vk::ClearValue clear_colors[] = { vk::ClearColorValue(clear_color), vk::ClearDepthStencilValue { 0.f, 0 } };
	const vk::ClearValue ssaa_clear_colors[] = { clear_colors[0], clear_colors[0], clear_colors[1] };
	commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(*renderPass, *framebuffers[GetCurrentImage()],
			vk::Rect2D( { 0, 0 }, viewport), samples > 1 ? 3 : 2, samples > 1 ? ssaa_clear_colors : clear_colors),
			vk::SubpassContents::eInline);
	commandBuffer.setViewport(0, vk::Viewport(0.0f, 0.0f, viewport.width, viewport.height, 0.0f, 1.0f));

	SetBaseScissor();
	/* A render that covers only part of the screen leaves the rest of the
	 * framebuffer as it was. Every frame here goes to a fresh image,
	 * cleared, so the part that is not drawn was black: the last frame is
	 * drawn into it first, whole, and the render goes over it. Only for
	 * such a render - one that covers the screen pays nothing - and not
	 * into a framebuffer the game has not drawn to lately, which starts
	 * empty. */
	if ((havePicture || restoredPicture) && quadPipeline != nullptr && !pvrrc.clearFramebuffer && matrices.IsClipped())
	{
		const int count = (int)colorAttachments.size();
		const std::array<float, 4> opaque = { 1.f, 1.f, 1.f, 1.f };

		commandBuffer.setScissor(0, vk::Rect2D( { 0, 0 }, viewport));
		commandBuffer.setBlendConstants(opaque.data());
		(samples > 1 ? &msQuadPipeline : quadPipeline)->BindPipeline(commandBuffer);
		/* (the picture kept from another context can be of another size) */
		if (havePicture)
			lastPicture.Draw(commandBuffer, colorAttachments[(GetCurrentImage() + count - 1) % count]->GetImageView(), nullptr, true, commandPool);
		else
		{
			QuadVertex area[4];
			lastPicture.Draw(commandBuffer, restoredPicture, QuadGameArea(area, restoredSize, viewport) ? area : nullptr, false, commandPool);
		}
	}
	commandBuffer.setScissor(0, baseScissor);
	currentCommandBuffer = commandBuffer;

	return commandBuffer;
}

void ScreenDrawer::KeepPicture()
{
	if (!havePicture)
		return;
	u8 *pixels = last_picture_keep(viewport.width, viewport.height);
	if (pixels != NULL)
		vk_read_picture(colorAttachments[GetCurrentImage()]->GetImage(), GetContext()->GetColorFormat(),
				viewport.width, viewport.height, pixels);
}

bool ScreenDrawer::WriteOutBeforeOsd()
{
	if (!writeOut.Runs(viewport.height))
		return false;
	/* (the HDR output's subpass passed over: the picture is encoded once it is written out) */
	if (hdrPipeline)
		currentCommandBuffer.nextSubpass(vk::SubpassContents::eInline);
	currentCommandBuffer.endRenderPass();
	writeOut.Record(currentCommandBuffer, GetCurrentImage(), colorAttachments[GetCurrentImage()]->GetImage(),
			colorAttachments[GetCurrentImage()]->GetImageView(), viewport.width, viewport.height);
	writeOut.BeginOverlay(currentCommandBuffer, GetCurrentImage());
	overlaid = true;
	return true;
}

void ScreenDrawer::EndRenderPass()
{
	havePicture = true;
	restoredPicture = nullptr;
	// (this frame may have drawn it: it goes when the frame is done with)
	if (carriedPicture)
		commandPool->DeferDelete(std::move(carriedPicture));
	/* The HDR output: the picture encoded in the render pass's last
	 * subpass, where it is; on a frame the chip's write-out changes after
	 * the render pass, once that is done (and what goes over it drawn), in
	 * the same submission */
	bool hdrAfter = false;
	if (hdrPipeline && !overlaid)
	{
		currentCommandBuffer.nextSubpass(vk::SubpassContents::eInline);
		/* (or with the post effects, which only the pass of its own draws) */
		hdrAfter = writeOut.Runs(viewport.height) || GetContext()->HdrPostActive();
		if (!hdrAfter)
			GetContext()->HdrSubpassDraw(*commandPool, currentCommandBuffer, *hdrPipeline,
					colorAttachments[GetCurrentImage()]->GetImageView(),
					samples > 1 ? samplesAttachment->GetImageView() : vk::ImageView(), viewport);
	}
	else if (hdrPipeline)
		hdrAfter = true;
	currentCommandBuffer.endRenderPass();
	if (overlaid)
		overlaid = false;
	else
		writeOut.Record(currentCommandBuffer, GetCurrentImage(), colorAttachments[GetCurrentImage()]->GetImage(),
				colorAttachments[GetCurrentImage()]->GetImageView(), viewport.width, viewport.height);
	if (motionPass.Recorded())
	{
		// (what went over the picture may have bound its own)
		vk_desc_sets_bind_per_frame(vk_tri_drawer_desc_set(&t), (VkCommandBuffer)currentCommandBuffer);
		motionPass.Record(currentCommandBuffer, GetCurrentImage(), geometry, t.offsets.indexOffset, baseScissor, geometryVertices);
	}
	timer.End(currentCommandBuffer, commandPool->GetIndex());
	currentCommandBuffer.end();
	currentCommandBuffer = nullptr;
	vk::Image image = colorAttachments[GetCurrentImage()]->GetImage();
	vk::ImageView view = colorAttachments[GetCurrentImage()]->GetImageView();
	bool hdr = false;
	if (hdrAfter)
		hdr = GetContext()->HdrEncode(*commandPool, image, view, vk::Extent2D(viewport.width, viewport.height));
	else if (hdrPipeline)
	{
		image = hdrAttachments[GetCurrentImage()]->GetImage();
		view = hdrAttachments[GetCurrentImage()]->GetImageView();
		hdr = true;
	}
	commandPool->EndFrame();
	GetContext()->PresentFrame(image, view, vk::Offset2D(viewport.width, viewport.height), hdr);
	if (motionPass.Recorded())
		motionPass.Submit(GetCurrentImage(), depthAttachments[GetCurrentImage()]->GetImage(),
				depthAttachments[GetCurrentImage()]->GetImageView(), GetContext()->GetDepthFormat());
}
