/*
	Copyright 2020 flyinghead

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
#include "vulkan.h"
#include "rend/upscaler.h"
#include "rend/renderer_class.h"
#include "hw/pvr/ta.h"
#include "commandpool.h"
#include "pipeline.h"
#include "vmu.h"

#include "rtt_read.h"
#include "tex_decoder.h"
#include "ta_decoder.h"
#include "rend/rtt_watch.h"
#include "rend/last_picture.h"
#include "parse_arena_vk.h"
#include "drawer.h"
#include "vk_renderer.h"
#include <memory>
#include <vector>

/* The C++ renderers' view of a vk_renderer (vk_renderer.h): the per-pixel
 * renderer's, until it is C too */
class BaseVulkanRenderer : public Renderer
{
public:
	BaseVulkanRenderer() { vk_renderer_construct(&v, Kind(), this); }
	~BaseVulkanRenderer() { vk_renderer_free(&v); }
	BaseVulkanRenderer(const BaseVulkanRenderer&) = delete;
	BaseVulkanRenderer& operator=(const BaseVulkanRenderer&) = delete;

	void DropQuads() { vk_renderer_drop_quads(&v); }
	bool Ready() override { return vk_pso_worker_batch_ready(); }
	virtual void Term() override { vk_renderer_term(&v); }
	bool PresentKeptPicture() override { return vk_renderer_present_kept_picture(&v); }
	virtual void OptionsChanged() override { vk_renderer_options_changed(&v); }
	virtual u64 GetTexture(TSP tsp, TCW tcw, int area = 0) override { return vk_renderer_get_texture(&v, tsp.full, tcw.full, area); }
	virtual bool Process(TA_context* ctx) override { return vk_renderer_process(&v, ctx); }
	bool Render() override { return vk_renderer_render(&v); }

protected:
	virtual bool RenderFrame() = 0;
	VulkanContext *GetContext() const { return VulkanContext::Instance(); }
	void SetParsed(BaseDrawer& drawer) { vk_renderer_set_parsed(&v, drawer.Base()); }
	// how the drawers draw a frame decoded on the GPU (ta_gpu_prepare()'s TA_GPU_*)
	virtual u32 TaFlags() const = 0;
	bool BaseInit(vk::RenderPass renderPass, int subpass = 0)
	{
		if (!vk_renderer_base_init(&v, (VkRenderPass)renderPass, subpass))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_renderer_base_init");
		return true;
	}
	/* In a new context, the picture kept from the old one (rend/last_picture.h)
	 * is made a texture, and the screen's drawer starts its first frame from it. */
	virtual void SetRestoredPicture(vk::ImageView view, vk::Extent2D size) = 0;
	// the overlay's quad pipeline, for @pass (none: there is no write-out)
	void InitOverlay(vk::RenderPass pass)
	{
		if (!vk_renderer_init_overlay(&v, (VkRenderPass)pass))
			throwResultException(vk::Result::eErrorInitializationFailed, "vk_quad_pipeline_init");
	}
	Texture *FogTexture() const { return static_cast<Texture *>(v.fogTexture); }
	Texture *PaletteTexture() const { return static_cast<Texture *>(v.paletteTexture); }

	vk_renderer v;
	/* (its members, as the C++ renderers name them) */
	u32& samples = v.samples;
	ShaderManager& shaderManager = reinterpret_cast<ShaderManager&>(v.modules);
	CommandPool& texCommandPool = reinterpret_cast<CommandPool&>(v.texCommandPool);
	TexturePool& texPool = reinterpret_cast<TexturePool&>(v.texPool);
	TaDecoder& taDecoder = reinterpret_cast<TaDecoder&>(v.taDecoder);
	TextureCache& textureCache = reinterpret_cast<TextureCache&>(v.textureCache);
	QuadPipeline& quadPipeline = reinterpret_cast<QuadPipeline&>(v.quadPipeline);
	QuadPipeline& overlayQuadPipeline = reinterpret_cast<QuadPipeline&>(v.overlayQuadPipeline);
	VulkanOSD *const vmus = reinterpret_cast<VulkanOSD *>(&v.osd);

private:
	static bool RenderFrameOf(void *self) { return static_cast<BaseVulkanRenderer *>(self)->RenderFrame(); }
	static u32 TaFlagsOf(void *self) { return static_cast<BaseVulkanRenderer *>(self)->TaFlags(); }
	static void SetRestoredPictureOf(void *self, VkImageView view, u32 width, u32 height)
	{
		static_cast<BaseVulkanRenderer *>(self)->SetRestoredPicture(vk::ImageView(view), vk::Extent2D(width, height));
	}
	static const vk_renderer_kind *Kind()
	{
		static const vk_renderer_kind kind = { RenderFrameOf, TaFlagsOf, SetRestoredPictureOf };
		return &kind;
	}
};
static_assert(sizeof(ShaderManager) == sizeof(vk_raster_modules) && sizeof(CommandPool) == sizeof(vk_cmd_pool)
		&& sizeof(TexturePool) == sizeof(vk_tex_pool) && sizeof(TaDecoder) == sizeof(vk_ta_decoder)
		&& sizeof(TextureCache) == sizeof(vk_texcache) && sizeof(QuadPipeline) == sizeof(vk_quad_pipeline)
		&& sizeof(VulkanOSD) == sizeof(vk_osd), "the C++ views are their C structs");
