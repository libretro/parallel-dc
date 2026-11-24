/*
 *  Created on: Oct 3, 2019

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
#include "rend/grow_buf.h"
#include "vulkan_context.h"
#include "buffer.h"
#include "rend/TexCache.h"
#include "rend/texdecode.h"
#include "rend/tex_pool.h"
#include "vk_image.h"
#include "tex_decoder.h"
#include "vk_texture.h"
#include "hw/pvr/Renderer_if.h"

#include <algorithm>
#include <memory>
#include <map>
#include <string>
#include <vector>

static inline void setImageLayout(vk::CommandBuffer const& commandBuffer, vk::Image image, vk::Format format, u32 mipmapLevels,
		vk::ImageLayout oldImageLayout, vk::ImageLayout newImageLayout)
{
	vk_set_image_layout((VkCommandBuffer)commandBuffer, (VkImage)image, (VkFormat)format, mipmapLevels,
			(VkImageLayout)oldImageLayout, (VkImageLayout)newImageLayout);
}

class CommandPool;
class TexDecoder;

/* Where textures wait on the way to their images: memory that stays mapped,
 * handed out a piece per upload and taken back a frame at a time.
 *
 * Every upload used to make a buffer of its own for this - a Vulkan buffer
 * and a piece of the heap - and retire it afterwards, and the texture was
 * converted into the cache's memory and copied into the buffer. A texture
 * is converted straight into its piece now (Texture::UploadMemory), and the
 * pieces come out of one buffer, made when the renderer starts (Init()), a
 * share of the video memory budget, and never grown: a ring, the frames'
 * pieces one after the other.
 *
 * A piece is spoken for until the frame it was handed out in has been
 * through the GPU: when that frame's place in the chain of frames comes
 * round again (SetCurrentIndex(), after the texture command pool waited for
 * its fence), or before, when the ring is full and that frame's fence,
 * polled, says it is done. Nothing waits for it: when the frames still on
 * the GPU hold the whole ring, there is no piece (Allocate() is null), and
 * the texture is made with the next frame instead. */
class CommandPool;
class UploadRing
{
public:
	template<typename Pool>
	void Init(size_t bytes, Pool *pool)
	{
		if (!vk_upload_ring_init(r(), bytes, &pool->p))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_upload_ring_init");
	}
	void SetCurrentIndex(int index) { vk_upload_ring_set_current_index(r(), index); }
	u8 *Allocate(size_t bytes, vk::Buffer *buffer, vk::DeviceSize *offset)
	{
		VkBuffer b = VK_NULL_HANDLE;
		VkDeviceSize o = 0;
		u8 *memory = vk_upload_ring_allocate(r(), bytes, &b, &o);
		*buffer = vk::Buffer(b);
		*offset = o;
		return memory;
	}
	void Term() { vk_upload_ring_term(r()); }
	bool Ready() const { return vk_upload_ring_get()->mapped != nullptr; }

private:
	static vk_upload_ring *r() { return vk_upload_ring_get(); }
};

/* An image of the texture pool's: optimal, sampled, copied to and from,
 * and written by the decoding kernel too where the decoder takes its
 * format, with a storage view of each level for that */
typedef vk_pooled_image PooledImage;

/* The cache's textures' images (rend/tex_pool.h): made when the renderer
 * starts, in the device memory's large blocks, and handed out to the
 * textures that need one of their shape. And the pictures rendered to
 * (TakeTarget()), RGBA8, of their own kind of image. */
class TexturePool
{
public:
	TexturePool() { vk_tex_pool_construct(&p); }
	~TexturePool() { vk_tex_pool_term(&p); }
	TexturePool(const TexturePool&) = delete;
	TexturePool& operator=(const TexturePool&) = delete;
	void Init(const TexDecoder *decoder) { vk_tex_pool_init(&p, decoder != nullptr ? decoder->Raw() : nullptr); }
	void Term() { vk_tex_pool_term(&p); }
	bool Ready() const { return p.ready; }
	PooledImage *Take(vk::Format format, u32 width, u32 height, bool mips)
	{
		return vk_tex_pool_take(&p, (VkFormat)format, width, height, mips);
	}
	PooledImage *TakeTarget(u32 width, u32 height, const vk::ImageUsageFlags& usage)
	{
		return vk_tex_pool_take_target(&p, width, height, (VkImageUsageFlags)usage);
	}
	void SetTargetStorage(bool on) { vk_tex_pool_set_target_storage(&p, on); }
	bool TargetsStorable() const { return vk_tex_pool_targets_storable(&p); }
	void Give(PooledImage *image) { vk_tex_pool_give(&p, image); }
	void Retire(PooledImage *image) { vk_tex_pool_retire(&p, image); }
	void Frame(u32 index) { vk_tex_pool_frame(&p, index); }
	const tex_pool_book_t& Book() const { return p.book; }
	vk_tex_pool *Raw() { return &p; }

private:
	vk_tex_pool p;
};

/* The C++ renderers' view of a vk_texture (vk_texture.h), until they are C
 * too: no data of its own, so that the cache's textures are views too */
struct Texture : public vk_texture
{
	Texture() { vk_texture_construct(this); }
	~Texture() { vk_texture_destroy(this); }
	Texture(const Texture&) = delete;
	Texture& operator=(const Texture&) = delete;

	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded = false)
	{
		vk_texture_upload_to_gpu(this, width, height, data, mipmapped, mipmapsIncluded);
	}
	void *UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
	{
		return vk_texture_upload_memory(this, width, height, bytes, pixel, mipmaps);
	}
	static UploadRing& Ring() { static UploadRing ring; return ring; }
	/* The renderer's, while it has one */
	static void SetDecoder(TexDecoder *decoder) { vk_texture_decoder = decoder != nullptr ? decoder->Raw() : nullptr; }
	static void SetPool(TexturePool *pool) { vk_texture_pool = pool != nullptr ? pool->Raw() : nullptr; }
	/* A texture of the cache's: its images come from the pool. (Not the
	 * renderer's own pictures, which go when it says.) */
	void SetCached() { cached = true; }
	u64 GetIntId() { return (u64)reinterpret_cast<uintptr_t>(this); }
	bool IsNew() const { return vk_texture_is_new(this); }
	vk::ImageView GetImageView() const { return vk::ImageView(vk_texture_view(this)); }
	u64 GetViewSerial() const { return vk_texture_view_serial(this); }
	vk::Image GetImage() const { return vk::Image(vk_texture_image(this)); }
	vk::ImageView GetReadOnlyImageView() const { return vk::ImageView(vk_texture_read_only_view(this)); }
	u64 ReadOnlyViewSerial() const { return vk_texture_read_only_serial(this); }
	void KeepReadOnlyView() { vk_texture_keep_read_only_view(this); }
	/* A serial for a view just made */
	static u64 NewViewSerial() { return vk_new_view_serial(); }
	void SetCommandBuffer(vk::CommandBuffer cmd) { commandBuffer = (VkCommandBuffer)cmd; }
	void SetCommandBuffer(std::nullptr_t) { commandBuffer = VK_NULL_HANDLE; }
	/* Where a command buffer comes from when an upload needs one: a
	 * texture made on the GPU records nothing of its own */
	template<typename Pool>
	void SetCommandPool(Pool *pool) { commandPool = pool != nullptr ? &pool->p : nullptr; }
	void SetCommandPool(std::nullptr_t) { commandPool = nullptr; }
	void SetPhysicalDevice(vk::PhysicalDevice) {}
	void SetDevice(vk::Device) {}
	void CreateRenderTarget(const vk::ImageUsageFlags& usage)
	{
		if (!vk_texture_create_render_target(this, (VkImageUsageFlags)usage))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_texture_create_render_target");
	}
	void ClearOutside(vk::CommandBuffer cmd, u32 width, u32 height, vk::ImageLayout& layout)
	{
		VkImageLayout l = (VkImageLayout)layout;
		vk_texture_clear_outside(this, (VkCommandBuffer)cmd, width, height, &l);
		layout = (vk::ImageLayout)l;
	}

	/* the cache's (rend/TexCache.h) */
	void Create() { texcache_create(&tc); }
	bool NeedsUpdate() { return texcache_needs_update(&tc); }
	void Update() { texcache_update(&tc); }
	bool IsCustomTextureAvailable() const { return texcache_custom_available(&tc); }
	void CheckCustomTexture() { texcache_check_custom(&tc); }
};

/* The polygons' samplers, every one a TSP can ask for made before the
 * first frame (Init()): filtered or not, each axis clamped, mirrored or
 * repeated, and the 16 mipmap biases - 288 - in a plain table, nothing
 * made or allocated while the game runs */
class SamplerManager
{
public:
	SamplerManager() { memset(&s, 0, sizeof(s)); }
	~SamplerManager() { vk_samplers_term(&s); }
	SamplerManager(const SamplerManager&) = delete;
	SamplerManager& operator=(const SamplerManager&) = delete;
	void Init() { vk_samplers_init(&s); }
	vk::Sampler GetSampler(TSP tsp) { return vk::Sampler(vk_samplers_get(&s, tsp.full)); }
	void Term() { vk_samplers_term(&s); }
	vk_samplers *Raw() { return &s; }
	static const u32 TSP_Mask = VK_SAMPLER_TSP_MASK;

private:
	vk_samplers s;
};

class FramebufferAttachment
{
public:
	FramebufferAttachment(vk::PhysicalDevice, vk::Device) { memset(&a, 0, sizeof(a)); }
	~FramebufferAttachment() { vk_attachment_free(&a); }
	FramebufferAttachment(const FramebufferAttachment&) = delete;
	FramebufferAttachment& operator=(const FramebufferAttachment&) = delete;
	void Init(u32 width, u32 height, vk::Format format, const vk::ImageUsageFlags& usage, bool readable = true,
			vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1)
	{
		if (!vk_attachment_init(&a, width, height, (VkFormat)format, (VkImageUsageFlags)usage, readable, (VkSampleCountFlagBits)samples))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_attachment_init");
	}
	void Reset() { vk_attachment_free(&a); }
	vk::ImageView GetImageView() const { return vk::ImageView(a.view); }
	vk::Image GetImage() const { return vk::Image(a.image); }
	vk::ImageView GetStencilView() const { return vk::ImageView(a.stencilView); }
	vk::Extent2D getExtent() const { return vk::Extent2D(a.extent.width, a.extent.height); }
	u64 GetViewSerial() const { return a.viewSerial; }

private:
	vk_attachment a;
};

/* A framebuffer kept for the attachments it was made of, known by their
 * views' serials: made again only when one of them, the render pass or the
 * size is another. (Made again in a frame slot whose last frame is done.) */
class KeptFramebuffer
{
public:
	KeptFramebuffer() { memset(&k, 0, sizeof(k)); }
	~KeptFramebuffer() { vk_kept_framebuffer_free(&k); }
	KeptFramebuffer(const KeptFramebuffer&) = delete;
	KeptFramebuffer& operator=(const KeptFramebuffer&) = delete;
	/* (the drawers' are in a vector: moved, the framebuffer goes with it) */
	KeptFramebuffer(KeptFramebuffer&& o) noexcept { k = o.k; memset(&o.k, 0, sizeof(o.k)); }
	KeptFramebuffer& operator=(KeptFramebuffer&& o) noexcept
	{
		if (this != &o)
		{
			vk_kept_framebuffer_free(&k);
			k = o.k;
			memset(&o.k, 0, sizeof(o.k));
		}
		return *this;
	}
	vk::Framebuffer Get(vk::RenderPass pass, u32 count, const vk::ImageView *views, const u64 *serials, u32 width, u32 height)
	{
		const VkFramebuffer f = vk_kept_framebuffer_get(&k, (VkRenderPass)pass, count, reinterpret_cast<const VkImageView *>(views),
				serials, width, height);
		if (f == VK_NULL_HANDLE)
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vkCreateFramebuffer");
		return vk::Framebuffer(f);
	}
	vk::Framebuffer Handle() const { return vk::Framebuffer(k.framebuffer); }

private:
	vk_kept_framebuffer k;
};

/* The C++ renderers' view of a vk_texcache (vk_texture.h) */
class TextureCache
{
public:
	TextureCache() { vk_texcache_construct(&c); }
	~TextureCache() { vk_texcache_free(&c); }
	TextureCache(const TextureCache&) = delete;
	TextureCache& operator=(const TextureCache&) = delete;

	void ReserveFor(u32 images) { vk_texcache_reserve_for(&c, images); }
	Texture *getTextureCacheData(TSP tsp, TCW tcw, int area = 0)
	{
		return static_cast<Texture *>(vk_texcache_get(&c, tsp.full, tcw.full, area));
	}
	void SetCurrentIndex(int index) { vk_texcache_set_current_index(&c, index); }
	bool IsInFlight(Texture *texture) { return vk_texcache_is_in_flight(&c, texture); }
	void SetInFlight(Texture *texture) { vk_texcache_set_in_flight(&c, texture); }
	void DestroyLater(Texture *texture) { vk_texcache_destroy_later(&c, texture); }
	void Cleanup() { vk_texcache_cleanup(&c); }
	void ClearLater() { vk_texcache_clear_later(&c); }
	void Clear() { vk_texcache_clear(&c); }
	vk_texcache *Raw() { return &c; }

private:
	vk_texcache c;
};
