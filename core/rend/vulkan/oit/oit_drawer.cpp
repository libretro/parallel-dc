#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
    Created on: Nov 7, 2019

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
#include "oit_drawer.h"
#include "../rtt_read.h"
#include "rend/last_picture.h"
#include "hw/pvr/pvr_mem.h"

#include <algorithm>

void OITScreenDrawer::KeepPicture()
{
	if (!havePicture)
		return;
	u8 *pixels = last_picture_keep(viewport.extent.width, viewport.extent.height);
	if (pixels != NULL)
		vk_read_picture(finalColorAttachments[GetCurrentImage()]->GetImage(), GetContext()->GetColorFormat(),
				viewport.extent.width, viewport.extent.height, pixels);
}

void OITScreenDrawer::MakeFramebuffers()
{
	viewport.offset.x = 0;
	viewport.offset.y = 0;
	viewport.extent = GetContext()->GetViewPort();

	SetScreenSize(viewport.extent.width, viewport.extent.height);
	/* (renders to textures draw into the attachments too, when they can:
	 * as large as those need, vk_rtt_share_extent()) */
	{
		const vk::Extent2D size = SharesAttachments() ? vk::Extent2D(vk_rtt_share_extent(viewport.extent)) : viewport.extent;
		/* (the lists' heads as large as those renders too: made while
		 * the game runs, for the first of them, otherwise) */
		const vk::Extent2D heads = vk::Extent2D(vk_rtt_share_extent(viewport.extent));
		InitBuffers(heads.width, heads.height);
		MakeAttachments(size.width, size.height);
	}
	// (kept until the frames that could still use them have gone by: see the per-triangle screen drawer)
	if (commandPool != nullptr)
	{
		// the image with the last frame stays, as the picture the next render starts from
		if (carriedPicture)
			commandPool->DeferDelete(std::move(carriedPicture));
		if (havePicture && (size_t)GetCurrentImage() < finalColorAttachments.size() && finalColorAttachments[GetCurrentImage()])
		{
			carriedPicture = std::move(finalColorAttachments[GetCurrentImage()]);
			restoredPicture = carriedPicture->GetImageView();
			restoredSize = carriedPicture->getExtent();
		}
		for (auto& framebuffer : framebuffers)
			if (framebuffer)
				commandPool->DeferDelete(std::unique_ptr<vk::UniqueFramebuffer>(new vk::UniqueFramebuffer(std::move(framebuffer))));
		for (auto& attachment : finalColorAttachments)
			if (attachment)
				commandPool->DeferDelete(std::move(attachment));
	}
	framebuffers.clear();
	finalColorAttachments.clear();
	havePicture = false;
	while (finalColorAttachments.size() < GetContext()->GetSwapChainSize())
	{
		finalColorAttachments.push_back(std::unique_ptr<FramebufferAttachment>(
				new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice())));
		// (copied from when the context goes, to keep the picture: rend/last_picture.h)
		finalColorAttachments.back()->Init(viewport.extent.width, viewport.extent.height, GetContext()->GetColorFormat(),
				vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc
				| (storagePicture || VulkanWriteOut::Wanted() ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlags()), false);
		vk::ImageView attachments[] = {
				finalColorAttachments.back()->GetImageView(),
				ColorAttachment(0)->GetImageView(),
				upscaled ? vk::ImageView(o.depthAttachments[finalColorAttachments.size() - 1].view) : DepthAttachment()->GetImageView(),
		};
		vk::FramebufferCreateInfo createInfo(vk::FramebufferCreateFlags(), screenPipelineManager->GetRenderPass(true, true),
				ARRAY_SIZE(attachments), attachments, viewport.extent.width, viewport.extent.height, 1);
		framebuffers.push_back(GetContext()->GetDevice().createFramebufferUnique(createInfo));
	}
	// (the chip's write-out over the pictures: ../writeout.h)
	{
		std::vector<vk::ImageView> views;
		for (const auto& attachment : finalColorAttachments)
			views.push_back(attachment->GetImageView());
		writeOut.Init(views, viewport.extent);
	}
}

OITTextureDrawer::Size OITTextureDrawer::GetSize()
{
	Size s;
	// the frame from its first pixel, as far as the pixel clip goes (see TransformMatrix)
	s.origWidth = pvrrc.fb_X_CLIP.max + 1;
	s.origHeight = pvrrc.fb_Y_CLIP.max + 1;
	s.clipX = pvrrc.fb_X_CLIP.min;
	s.clipY = pvrrc.fb_Y_CLIP.min;
	s.clipWidth = s.origWidth - s.clipX;
	s.clipHeight = s.origHeight - s.clipY;
	s.upscaledWidth = s.origWidth;
	s.upscaledHeight = s.origHeight;
	s.heightPow2 = 8;
	while (s.heightPow2 < s.upscaledHeight)
		s.heightPow2 *= 2;
	s.widthPow2 = 8;
	while (s.widthPow2 < s.upscaledWidth)
		s.widthPow2 *= 2;

	if (settings.rend.RenderToTextureUpscale > 1)
	{
		s.upscaledWidth *= settings.rend.RenderToTextureUpscale;
		s.upscaledHeight *= settings.rend.RenderToTextureUpscale;
		s.widthPow2 *= settings.rend.RenderToTextureUpscale;
		s.heightPow2 *= settings.rend.RenderToTextureUpscale;
	}
	return s;
}

vk::ImageLayout OITTextureDrawer::TakeTexture(const Size& size, const vk::ImageUsageFlags& usage)
{
	const u32 origWidth = size.origWidth, origHeight = size.origHeight;
	const u32 widthPow2 = size.widthPow2, heightPow2 = size.heightPow2;
	vk::ImageLayout colorImageCurrentLayout;

	/* what an earlier render left waiting in this memory, while its picture is still what it was: see rtt_read.h */
	vk_rtt_supersede(textureAddr, size.clipX, size.clipY, size.clipWidth, size.clipHeight);
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

	TSP tsp = {};
	for (tsp.TexU = 0; tsp.TexU <= 7 && (8u << tsp.TexU) < origWidth; tsp.TexU++);
	for (tsp.TexV = 0; tsp.TexV <= 7 && (8u << tsp.TexV) < origHeight; tsp.TexV++);

	texture = textureCache->getTextureCacheData(tsp, tcw);
	if (texture->IsNew())
	{
		texture->Create();
		texture->SetPhysicalDevice(GetContext()->GetPhysicalDevice());
		texture->SetDevice(GetContext()->GetDevice());
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
			| vk::ImageUsageFlagBits::eTransferSrc	// read back when the game touches it in video memory
			| storageUsage | usage;
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
	viewport.offset.x = 0;
	viewport.offset.y = 0;
	viewport.extent.width = widthPow2;
	viewport.extent.height = heightPow2;
	return colorImageCurrentLayout;
}

vk::CommandBuffer OITTextureDrawer::NewFrame()
{
	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d\n", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max);
	NewImage();

	matrices.CalcMatrices(&pvrrc);

	textureAddr = FB_W_SOF1 & VRAM_MASK;
	const Size size = GetSize();
	const u32 clipX = size.clipX, clipY = size.clipY;
	const u32 clipWidth = size.clipWidth, clipHeight = size.clipHeight;
	const u32 upscaledWidth = size.upscaledWidth, upscaledHeight = size.upscaledHeight;

	VulkanContext *context = GetContext();
	vk::Device device = context->GetDevice();

	vk::CommandBuffer commandBuffer = commandPool->Allocate();
	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	vertexUniformBuffer = WriteVertexUniforms(commandBuffer, GetCurrentImage());

	/* What the render draws: its own pixels, the texture's from the first
	 * (Texture::ClearOutside()) */
	InitBuffers(upscaledWidth, upscaledHeight);
	/* The attachments: the screen's drawer's when they are large enough -
	 * drawn into by the screen's frames and this one by one, on the same
	 * queue, each one's first render pass starting them anew -, or this
	 * drawer's own; which a render larger than both has made for it. */
	o.attachmentsOf = &o;
	if (screen != nullptr && screen->SharesAttachments() && screen->AttachmentsCover(upscaledWidth, upscaledHeight))
	{
		o.attachmentsOf = screen->Raw();
		drewShared++;
	}
	else
	{
		drewOwn++;
		if (!AttachmentsCover(upscaledWidth, upscaledHeight))
		{
			if (!saidLarger)
			{
				saidLarger = true;
				WARN_LOG(RENDERER, "Render to a texture of %u x %u: attachments made for it\n", upscaledWidth, upscaledHeight);
			}
		}
		MakeAttachments(upscaledWidth, upscaledHeight);
	}
	const vk_oit_drawer *const att = o.attachmentsOf;

	vk::ImageLayout colorImageCurrentLayout = TakeTexture(size, vk::ImageUsageFlags());
	const vk::ImageView colorImageView = texture->GetImageView();
	viewport.extent.width = upscaledWidth;
	viewport.extent.height = upscaledHeight;
	texture->ClearOutside(commandBuffer, upscaledWidth, upscaledHeight, colorImageCurrentLayout);

	setImageLayout(commandBuffer, colorImage, vk::Format::eR8G8B8A8Unorm, 1, colorImageCurrentLayout, vk::ImageLayout::eColorAttachmentOptimal);

	vk::ImageView imageViews[] = {
		colorImageView,
		vk::ImageView(att->colorAttachments[0].view),
		vk::ImageView(att->depthAttachment.view),
	};
	const u64 viewSerials[] = {
		texture->GetViewSerial(),
		att->colorAttachments[0].viewSerial,
		att->depthAttachment.viewSerial,
	};
	/* (made again only for other attachments: a texture rendered to every
	 * frame is the same one) */
	framebuffers.resize(GetContext()->GetSwapChainSize());
	framebuffers[GetCurrentImage()].Get(rttPipelineManager->GetRenderPass(true, true), ARRAY_SIZE(imageViews), imageViews,
			viewSerials, upscaledWidth, upscaledHeight);

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

void OITTextureDrawer::EndFrame()
{
	currentCommandBuffer.endRenderPass();
	CountPass(currentCommandBuffer);

	currentCommandBuffer.end();

	colorImage = nullptr;
	currentCommandBuffer = nullptr;
	commandPool->EndFrame();

	Watch();
}

void OITTextureDrawer::Watch()
{
	texture->tc.dirty = 0;
	texture->tc.stride_read = 0;	// (rendered, not read)
	texture->tc.made_type = 0;
   libCore_vramlock_Lock(texture->tc.sa_tex, texture->tc.sa + texture->tc.size - 1, &texture->tc);
	/* video memory gets it when something first touches it there */
	vk_rtt_watch(texture, textureAddr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
}

/* The compute renderer's render to a texture goes to the texture a render
 * of this drawer's would, which is left as one of its is: in the cache,
 * locked in video memory and watched */
OITTextureDrawer::ComputeTarget OITTextureDrawer::ComputeBegin()
{
	NewImage();
	textureAddr = FB_W_SOF1 & VRAM_MASK;
	// (the compute renderer's own barrier makes it ready to be written, from whatever it held)
	TakeTexture(GetSize(), vk::ImageUsageFlagBits::eStorage);
	texture->rttArea = 0;	// (written as the compute renderer writes it: Texture::ClearOutside())
	ComputeTarget t;
	t.image = colorImage;
	t.view = texture->GetImageView();
	return t;
}

void OITTextureDrawer::ComputeEnd()
{
	colorImage = nullptr;
	commandPool->EndFrame();
	Watch();
}

vk::CommandBuffer OITScreenDrawer::NewFrame()
{
	NewImage();
	vk::CommandBuffer commandBuffer = commandPool->Allocate();
	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	timer.Begin(commandBuffer, commandPool->GetIndex());

	matrices.CalcMatrices(&pvrrc);
	// the frame drawn under this frame's jitter, its strips matched for the motion vector pass
	if (upscaled && motionPass.Begin(GetCurrentImage()))
	{
		float jx, jy;
		upscaler_jitter(&jx, &jy);
		matrices.Jitter(jx, jy);
	}
	vertexUniformBuffer = WriteVertexUniforms(commandBuffer, GetCurrentImage());

	SetBaseScissor();

	commandBuffer.setScissor(0, baseScissor);
	commandBuffer.setViewport(0, vk::Viewport(viewport.offset.x, viewport.offset.y, viewport.extent.width, viewport.extent.height, 0.0f, 1.0f));
	currentCommandBuffer = commandBuffer;

	return commandBuffer;
}
