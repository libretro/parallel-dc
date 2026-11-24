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
#include "vk_drawer.h"
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

/* The C++ drawers' view of a vk_tri_drawer (vk_drawer.h) */
class Drawer : public BaseDrawer
{
public:
	Drawer() { vk_tri_drawer_construct(&t, &b); }
	virtual ~Drawer() { vk_tri_drawer_free(&t); }
	bool Draw(const Texture *fogTexture, const Texture *paletteTexture)
	{
		currentScissor = vk::Rect2D();
		/* (which works the matrices out for this render, and writes the
		 * vertex uniforms from them: taken before it, they were the last
		 * render's - of two renders to textures of different sizes in one
		 * frame, each was drawn with the other's, at the wrong scale) */
		const vk::CommandBuffer cmdBuffer = BeginRenderPass();
		const vk::Extent2D extent = ResolveExtent();
		if (!vk_tri_drawer_draw(&t, (VkCommandBuffer)cmdBuffer, (VkImageView)fogTexture->GetImageView(),
				(VkImageView)paletteTexture->GetImageView(), samples, ssaaResolve.Raw(), extent.width, extent.height))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_tri_drawer_draw");
		return !pvrrc.isRTT;
	}
	virtual void EndRenderPass() = 0;

protected:
	virtual vk::CommandBuffer BeginRenderPass() = 0;
	void NewImage() { vk_tri_drawer_new_image(&t); }

	void Init(SamplerManager *samplerManager, PipelineManager *pipelineManager)
	{
		this->samplerManager = samplerManager;
		if (!vk_tri_drawer_init(&t, samplerManager->Raw(), pipelineManager->Raw()))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_tri_drawer_init");
	}
	int GetCurrentImage() const { return t.imageIndex; }

	vk::CommandBuffer currentCommandBuffer;
	SamplerManager *samplerManager = nullptr;
	/* SSAA: the samples a pixel of this drawer's render pass (1: off), and
	 * the resolve its last subpass does at the end of Draw(), over this */
	u32 samples = 1;
	SsaaResolve ssaaResolve;
	virtual vk::Extent2D ResolveExtent() const = 0;
	vk_tri_drawer t;
};

class ScreenDrawer : public Drawer
{
public:
	// @samples: SSAA's (1: off)
	void Init(SamplerManager *samplerManager, ShaderManager *shaderManager, u32 samples);
	void Term()
	{
		writeOut.Term();
		ssaaResolve.Term();
		msQuadPipeline.Term();
		timer.Term();
		motionPass.Term(nullptr);
		/* (the pipeline thread finishes what it is making for it now,
		 * while the shaders those pipelines are made of are still there) */
		screenPipelineManager.reset();
	}
	vk::RenderPass GetRenderPass() const { return *renderPass; }
	/* The depth a render to a texture draws into as well, made large
	 * enough for one (TextureDrawer::ShareExtent()): null with SSAA or
	 * the upscaler, when the screen's is not one of a single sample and
	 * of no use past its frame */
	const FramebufferAttachment *SharedDepth() const { return samples == 1 && !upscaled ? depthAttachment.get() : nullptr; }
	// the subpass the screen's last things are drawn in (the VMUs, the OSD): after the resolve, with SSAA
	int GetOsdSubpass() const { return samples > 1 ? 1 : 0; }
	/* rend/last_picture.h: the context is going */
	void KeepPicture();
	/* ...and in the new one, the picture that was kept is this image */
	void SetRestoredPicture(vk::ImageView view, vk::Extent2D size) { restoredPicture = view; restoredSize = size; }
	/* What draws the last picture back: see BeginRenderPass(). */
	void SetQuadPipeline(QuadPipeline *pipeline)
	{
		quadPipeline = pipeline;
		// (with SSAA the picture goes into the samples: a pipeline of its own for them)
		lastPicture.Init(samples > 1 ? &msQuadPipeline : pipeline);
	}
	virtual void EndRenderPass() override;
	vk::CommandBuffer GetCurrentCommandBuffer() const { return currentCommandBuffer; }
	/* The chip's write-out (writeout.h), when the frame has one: the
	 * frame's render pass ended and the pass recorded now, before the VMUs
	 * and crosshairs, which are then drawn in the overlay's render pass
	 * (with a quad pipeline made for OverlayPass()); false if there is none */
	bool HasWriteOut() const { return (bool)OverlayPass(); }
	bool WriteOutBeforeOsd();
	vk::RenderPass OverlayPass() const { return writeOut.OverlayPass(); }

protected:
	virtual vk::CommandBuffer BeginRenderPass() override;
	virtual vk::Extent2D ResolveExtent() const override { return viewport; }

private:
	std::unique_ptr<PipelineManager> screenPipelineManager;

	vk::UniqueRenderPass renderPass;
	std::vector<vk::UniqueFramebuffer> framebuffers;
	std::vector<std::unique_ptr<FramebufferAttachment>> colorAttachments;
	// the chip's write-out over the pictures (writeout.h), which are then made for it to write
	VulkanWriteOut writeOut;
	bool storagePictures = false;
	bool overlaid = false;		// this frame's write-out was recorded before the overlays
	/* The HDR output: its images, one for each picture, written by the
	 * render pass's last subpass from the picture (VulkanContext::HdrActive()) */
	std::vector<std::unique_ptr<FramebufferAttachment>> hdrAttachments;
	vk::UniquePipeline hdrPipeline;
	QuadPipeline *quadPipeline = nullptr;
	QuadDrawer lastPicture;
	bool havePicture = false;	/* the image before this one holds the last frame */
	vk::ImageView restoredPicture;	/* ...or this does: the picture kept from the context before */
	vk::Extent2D restoredSize;		// (its size: of another shape, its 4:3 drawn where this one's is)
	/* ...or the image the last frame was drawn to at the size before this
	 * one, which is then this: kept as it is, with nothing copied */
	std::unique_ptr<FramebufferAttachment> carriedPicture;
	std::unique_ptr<FramebufferAttachment> depthAttachment;
	std::unique_ptr<FramebufferAttachment> samplesAttachment;	// SSAA's colour
	QuadPipeline msQuadPipeline;	// ...and the last picture's pipeline into it
	GpuFrameTimer timer;
	vk::Extent2D viewport;
	ShaderManager *shaderManager = nullptr;
	/* For the upscaler (rend/upscaler.h): the depth is one an image, kept
	 * and readable, and the motion vector pass draws over it after the
	 * frame's render pass */
	bool upscaled = false;
	std::vector<std::unique_ptr<FramebufferAttachment>> depthAttachments;
	MotionVectorPass motionPass;
};

class TextureDrawer : public Drawer
{
public:
	// @samples: SSAA's (1: off)
	void Init(SamplerManager *samplerManager, ShaderManager *shaderManager, TextureCache *textureCache, u32 samples);
	void Term()
	{
		if (drewShared + drewOwn != 0)
			NOTICE_LOG(RENDERER, "Renders to textures: %u drawn with the screen's depth, %u with one of their own; %u cleared outside\n",
					drewShared, drewOwn, vk_texture_outside_cleared);
		vk_texture_outside_cleared = 0;
		drewShared = drewOwn = 0;
		ssaaResolve.Term();
		// (as the screen's drawer's)
		rttPipelineManager.reset();
	}
	virtual void EndRenderPass() override;
	// the largest texture rendered to, a side, at this upscale (SSAA's attachments are made that large)
	static u32 MaxSide() { return 1024u * (u32)std::max(1, settings.rend.RenderToTextureUpscale); }
	/* A render to a texture draws its own pixels (Texture::ClearOutside()),
	 * and its depth is the screen's, which is made at least as large as
	 * the render of a screen: 640x480 times Render To Texture Upscaling
	 * (@screen: the screen's size) - which is the screen's own size, or
	 * less, unless that upscaling is the higher. A larger render (PAL's
	 * whole 576 lines at the console's own resolution) has a depth of its
	 * own, made when it comes, as all were before. */
	static vk::Extent2D ShareExtent(vk::Extent2D screen)
	{
		const vk::Extent2D common = CommonRender();
		return vk::Extent2D(std::max(screen.width, common.width), std::max(screen.height, common.height));
	}
	static vk::Extent2D CommonRender()
	{
		const u32 scale = (u32)std::max(1, settings.rend.RenderToTextureUpscale);
		return vk::Extent2D(640u * scale, 480u * scale);
	}
	/* The screen's drawer's depth, the one this draws into when it is
	 * large enough (ScreenDrawer::SharedDepth()); made of its own, as
	 * large as CommonRender(), where there is none to share. Said after
	 * the screen's drawer is made, or made again. */
	void ShareDepth(const ScreenDrawer *screen);

protected:
	virtual vk::CommandBuffer BeginRenderPass() override;
	virtual vk::Extent2D ResolveExtent() const override { return vk::Extent2D(width, height); }
	void MakeSsaaAttachments(u32 side);

private:
	u32 width = 0;
	u32 height = 0;
	u32 textureAddr = 0;
	std::unique_ptr<RttPipelineManager> rttPipelineManager;

	Texture *texture = nullptr;
	std::vector<KeptFramebuffer> framebuffers;
	const ScreenDrawer *screen = nullptr;	// whose depth is shared
	std::unique_ptr<FramebufferAttachment> depthAttachment;	// ...or this, of its own
	bool saidLarger = false;
	u32 drewShared = 0, drewOwn = 0;	// (said when it goes: tools/vulkan/vk_vram.sh)
	std::unique_ptr<FramebufferAttachment> samplesAttachment;	// SSAA's colour
	TextureCache *textureCache = nullptr;
};
