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

private:
	vk_tex_pool p;
};

class Texture : public TexCacheImpl<Texture>
{
public:
	~Texture();
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded = false);
	void *UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps);
	bool GpuDecode(const texd_plan& plan);
	bool PartMemory(size_t bytes, void **memory);
	void UploadPart(const TexRegion *regions, u32 count, u8 *data, const size_t *offset);
	int GpuDecodePart(const texd_plan& plan, const TexRegion *regions, u32 count);
	bool CopyRender(BaseTextureCacheData *from, u32 x, u32 y, u32 scale, u32 columns);
	static UploadRing& Ring();
	/* The renderer's, while it has one */
	static void SetDecoder(TexDecoder *decoder) { Texture::decoder = decoder; }
	static void SetPool(TexturePool *pool) { Texture::pool = pool; }
	static TexturePool *Pool() { return pool; }
	/* A texture of the cache's: its images come from the pool. (Not the
	 * renderer's own pictures, which go when it says.) */
	void SetCached() { cached = true; }
	u64 GetIntId() { return (u64)reinterpret_cast<uintptr_t>(this); }
	void GetId(char *s, size_t n) { snprintf(s, n, "%p", (void *)this); }
	bool IsNew() const { return !image.get() && pooled == nullptr; }
	vk::ImageView GetImageView() const { return pooled != nullptr ? vk::ImageView(pooled->view) : *imageView; }
	u64 GetViewSerial() const { return pooled != nullptr ? pooled->viewSerial : ownSerial; }
	vk::Image GetImage() const { return pooled != nullptr ? vk::Image(pooled->image) : *image; }
	vk::ImageView GetReadOnlyImageView() const { return readOnlyImageView ? readOnlyImageView : GetImageView(); }
	/* Which view GetReadOnlyImageView() is, of every one made: a number no
	 * other view ever has, which a handle - made again at the same value
	 * once its view is gone - is not. What a descriptor set is kept by. */
	u64 ReadOnlyViewSerial() const { return readOnlyImageView ? readOnlySerial : pooled != nullptr ? pooled->viewSerial : ownSerial; }
	/* The view the frames in flight sample, kept while a new image is made */
	void KeepReadOnlyView() { readOnlyImageView = GetImageView(); readOnlySerial = pooled != nullptr ? pooled->viewSerial : ownSerial; }
	/* A serial for a view just made */
	static u64 NewViewSerial() { return vk_new_view_serial(); }
	void SetCommandBuffer(vk::CommandBuffer commandBuffer) { this->commandBuffer = commandBuffer; }
	/* Where a command buffer comes from when an upload needs one: a
	 * texture made on the GPU records nothing of its own */
	void SetCommandPool(CommandPool *commandPool) { this->commandPool = commandPool; }
	bool Force32BitTexture(TextureType type) const { return !VulkanContext::Instance()->IsFormatSupported(type); }

	void SetPhysicalDevice(vk::PhysicalDevice physicalDevice) { this->physicalDevice = physicalDevice; }
	void SetDevice(vk::Device device) { this->device = device; }

private:
	/* What an image is made for: written by the CPU as before; by the
	 * decoding kernel; by a copy of the texture's texels as they are */
	enum Maker { CPU, KERNEL, COPY };
	void Init(u32 width, u32 height, vk::Format format ,u32 dataSize, bool mipmapped, bool mipmapsIncluded, Maker maker = CPU);
	bool PartReady(vk::Format format) const;
	bool Prepare(int width, int height, bool mipmapped, bool mipmapsIncluded, Maker maker = CPU, vk::Format native = vk::Format::eUndefined);
	void SetImage(u32 size, void *data, bool isNew, bool genMipmaps);
   void CreateImage(vk::ImageTiling tiling, const vk::ImageUsageFlags& usage, vk::ImageLayout initialLayout,
			const vk::ImageAspectFlags& aspectMask, vk::ImageCreateFlags flags = vk::ImageCreateFlags());
	/* The job this texture has with the decoder this frame is not done
	 * after all: it is made another way, or goes */
	void CancelDecode();
	/* The image from the pool, for one of this shape; false if the pool
	 * has no such shape */
	bool TakePooled(u32 width, u32 height, bool mipmapped);
	/* The pool's image given back: when the frames that may use it are
	 * done, or now */
	void ReleasePooled(bool now);
	vk::ImageView StorageView(u32 level) const { return pooled != nullptr ? vk::ImageView(pooled->storageViews[level]) : *storageViews[level]; }
	/* The image rendered to, extent by extent, with @usage: the pool's, or
	 * one made (CreateImage()) where the pool has none of that shape */
	void CreateRenderTarget(const vk::ImageUsageFlags& usage);
	/* No room in the upload ring: the texture is made with the next frame,
	 * and until then its image is left as it is */
	void Defer(bool isNew);
	/* what the image was made to be used for */
	vk::ImageUsageFlags imageUsage;
	/* A render to a texture draws its own pixels, @width by @height from
	 * the first (Render To Texture Upscaling's), not the whole image, a
	 * power of two: the rest is black, as it was when the render pass
	 * cleared the whole image. Said before the render pass, with the
	 * image's @layout, which is the one it is in after: cleared when it is
	 * not black already - the image new, or written since by another
	 * render, or anything else. */
	void ClearOutside(vk::CommandBuffer cmd, u32 width, u32 height, vk::ImageLayout& layout);
	/* (the last render's pixels, width << 16 | height, outside which the
	 * image is black; 0 once anything else writes it) */
	u32 rttArea = 0;
	static u32 outsideCleared;	// (how many times: said with the renders to textures, tools/vulkan/vk_vram.sh)
	void GenerateMipmaps();

	vk::Format format = vk::Format::eUndefined;
	vk::Extent2D extent;
	u32 mipmapLevels = 1;
	bool needsStaging = false;
public:
	// handed to the frontend as its frame: copied from as well (a frontend may read it back)
	bool presented = false;
private:
	/* the piece of the upload ring the texture was converted into, between
	 * UploadMemory() and the UploadToGPU() that follows; and whether the
	 * image was made anew for it */
	u8 *slot = nullptr;
	vk::Buffer slotBuffer;
	vk::DeviceSize slotOffset = 0;
	/* the piece PartMemory() handed out, for the UploadPart() that follows */
	u8 *partSlot = nullptr;
	vk::Buffer partBuffer;
	vk::DeviceSize partOffset = 0;
	bool prepared = false;
	bool preparedNew = false;
	vk::CommandBuffer commandBuffer;
	CommandPool *commandPool = nullptr;
	/* the image has never been written: it is made from nothing */
	bool fresh = false;
	/* made for the kernel to write, with a view for each level */
	bool storage = false;
	vk::UniqueImageView storageViews[TEXD_MAX_LEVELS];
	int decodeJob = -1;
	u32 decodeSerial = 0;
	/* For the texture debug log (settings.rend.TextureDebug): how the image
	 * was last written (DebugPath), and the checksums of what was uploaded,
	 * by image level, where the CPU made it */
	enum DebugPath : u8 { DBG_NONE, DBG_STAGED, DBG_LINEAR, DBG_KERNEL, DBG_COPY, DBG_TARGET };
	u8 debugPath = DBG_NONE;
	u8 debugLevels = 0;	// how many levels debugSum has, from level 0
	u32 debugSum[TEXD_MAX_LEVELS];
	static TexDecoder *decoder;
	static TexturePool *pool;
	PooledImage *pooled = nullptr;
	bool cached = false;

	Allocation allocation;
	vk::UniqueImage image;
	vk::UniqueImageView imageView;
	u64 ownSerial = 0;          /* imageView's */
	vk::ImageView readOnlyImageView;
	/* The frames (the command pool's places, bit by bit) whose in-flight
	 * lists have it: TextureCache */
	u32 inFlightSlots = 0;
	u64 readOnlySerial = 0;     /* readOnlyImageView's */

	vk::PhysicalDevice physicalDevice;
	vk::Device device;

	friend class TextureDrawer;
	friend class OITTextureDrawer;
	friend class TextureCache;
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

class TextureCache : public BaseTextureCache<Texture>
{
public:
	TextureCache() = default;
	TextureCache(const TextureCache&) = delete;
	TextureCache& operator=(const TextureCache&) = delete;
	~TextureCache()
	{
		for (u32 i = 0; i < inFlightCount; i++)
			grow_buf_free(&((InFlightList *)inFlightLists.data)[i].textures);
		grow_buf_free(&inFlightLists);
	}

	void SetCurrentIndex(int index) {
		InFlightList *lists = (InFlightList *)inFlightLists.data;
		if (currentIndex < inFlightCount)
		{
			Texture **list = InFlight(currentIndex);
			if (settings.rend.TextureDebug)
				DebugLog(list, lists[currentIndex].count);
			for (u32 i = 0; i < lists[currentIndex].count; i++)
				if (list[i] != nullptr)
					list[i]->readOnlyImageView = vk::ImageView();
		}
		currentIndex = index;
		Texture::Ring().SetCurrentIndex(index);
		if (Texture::Pool() != nullptr)
			Texture::Pool()->Frame(index);
		/* the textures this place had: in no frame now but those after it */
		if (GrowInFlight(currentIndex + 1))
			EmptyInFlight(currentIndex);
		EmptyTrash(trashedImageViews);
		EmptyTrash(trashedImages);
		EmptyTrash(trashedMem);
	}

	bool IsInFlight(Texture *texture)
	{
		return (texture->inFlightSlots & ~SlotBit(currentIndex)) != 0;
	}

	void SetInFlight(Texture *texture)
	{
		const u32 bit = SlotBit(currentIndex);
		if ((texture->inFlightSlots & bit) != 0 || !GrowInFlight(currentIndex + 1))
			return;
		InFlightList& l = ((InFlightList *)inFlightLists.data)[currentIndex];
		Texture **list = (Texture **)grow_buf_reserve_keep(&l.textures, (l.count + 1) * sizeof(Texture *));
		if (list == nullptr)
			return;
		list[l.count++] = texture;
		texture->inFlightSlots |= bit;
	}

	void DestroyLater(Texture *texture)
	{
		if (texture->pooled != nullptr)
		{
			texture->ReleasePooled(false);
			texture->format = vk::Format::eUndefined;
			return;
		}
		if (!texture->image)
			return;
		texture->CancelDecode();
		trashedImages[currentIndex].push_back(std::move(texture->image));
		trashedImageViews[currentIndex].push_back(std::move(texture->imageView));
		if (texture->storage)
			for (auto& view : texture->storageViews)
				if (view)
					trashedImageViews[currentIndex].push_back(std::move(view));
		texture->storage = false;
		trashedMem[currentIndex].push_back(std::move(texture->allocation));
		texture->format = vk::Format::eUndefined;
	}

	void Cleanup();

	/* Empties the cache between two frames. The frames before this one can
	 * still be on their way through the GPU, drawing with these textures, so
	 * the images are not destroyed here: they are retired, as a texture that
	 * is replaced is, and destroyed once those frames are done. */
	void ClearLater()
	{
		cache.Each([this](u64, Texture& texture) {
			DestroyLater(&texture);
			return true;
		});
		EmptyAllInFlight();
		BaseTextureCache::Clear();
	}

	/* Empties the cache and destroys everything now, what was retired too.
	 * For when the renderer is going away and nothing is drawing any more. */
	void Clear()
	{
		EmptyAllInFlight();
		BaseTextureCache::Clear();
		for (u32 i = 0; i < inFlightCount; i++)
			grow_buf_free(&((InFlightList *)inFlightLists.data)[i].textures);
		grow_buf_free(&inFlightLists);
		inFlightCount = 0;
		for (auto& v : trashedImageViews)
			v.clear();
		for (auto& v : trashedImages)
			v.clear();
		for (auto& v : trashedMem)
			v.clear();
		Texture::Ring().Term();
	}

private:
	/* The texture debug log (texture.cpp): the textures of the frame drawn
	 * last, now and then */
	void DebugLog(Texture *const *textures, u32 count);
	bool clearTexture(Texture *tex)
	{
		/* (out of the lists that have it: a texture gone is not seen there again) */
		for (u32 s = 0; s < inFlightCount && tex->inFlightSlots != 0; s++)
		{
			if ((tex->inFlightSlots & SlotBit(s)) == 0)
				continue;
			Texture **list = InFlight(s);
			const u32 n = ((InFlightList *)inFlightLists.data)[s].count;
			for (u32 i = 0; i < n; i++)
				if (list[i] == tex)
					list[i] = nullptr;
			tex->inFlightSlots &= ~SlotBit(s);
		}

		return tex->Delete();
	}

	/* The textures each frame (the command pool's place @s) draws with, once
	 * each: a list that grows, and a texture's bit for the place */
	struct InFlightList
	{
		grow_buf_t textures;	/* Texture *; nullptr where one has gone */
		u32 count;
	};
	/* (a swap chain has a few images: past 31 places, which none has, the last bit is shared) */
	static u32 SlotBit(u32 s)
	{
		return s < 32 ? 1u << s : 0x80000000u;
	}
	Texture **InFlight(u32 s)
	{
		return (Texture **)((InFlightList *)inFlightLists.data)[s].textures.data;
	}
	/* @n places at least, the new ones empty */
	bool GrowInFlight(u32 n)
	{
		if (n <= inFlightCount)
			return true;
		if (grow_buf_reserve_keep(&inFlightLists, n * sizeof(InFlightList)) == nullptr)
			return false;
		memset((InFlightList *)inFlightLists.data + inFlightCount, 0, (n - inFlightCount) * sizeof(InFlightList));
		inFlightCount = n;
		return true;
	}
	void EmptyInFlight(u32 s)
	{
		InFlightList& l = ((InFlightList *)inFlightLists.data)[s];
		Texture **list = (Texture **)l.textures.data;
		for (u32 i = 0; i < l.count; i++)
			if (list[i] != nullptr)
				list[i]->inFlightSlots &= ~SlotBit(s);
		l.count = 0;
	}
	void EmptyAllInFlight()
	{
		for (u32 s = 0; s < inFlightCount; s++)
			EmptyInFlight(s);
	}
	/* (each frame's lists with room for a game's textures of its own,
	 * those no image of the pool's has the shape of, retired in a frame:
	 * made with the frame's place, before the first, and kept) */
	template<typename T>
	void EmptyTrash(T& v)
	{
		if (v.size() < currentIndex + 1)
			v.resize(currentIndex + 1);
		v[currentIndex].clear();
		if (v[currentIndex].capacity() < 64)
			v[currentIndex].reserve(64);
	}
	grow_buf_t inFlightLists = GROW_BUF_INIT;	/* InFlightList, a place each */
	u32 inFlightCount = 0;
	std::vector<std::vector<vk::UniqueImageView>> trashedImageViews;
	std::vector<std::vector<vk::UniqueImage>> trashedImages;
	std::vector<std::vector<Allocation>> trashedMem;
	u32 currentIndex = 0;
};
