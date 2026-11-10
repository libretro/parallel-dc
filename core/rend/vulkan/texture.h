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
#include "hw/pvr/Renderer_if.h"

#include <algorithm>
#include <memory>
#include <map>
#include <string>
#include <vector>

void setImageLayout(vk::CommandBuffer const& commandBuffer, vk::Image image, vk::Format format, u32 mipmapLevels, vk::ImageLayout oldImageLayout, vk::ImageLayout newImageLayout);

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
	/* The ring, @bytes of it, for the frames of @pool */
	void Init(size_t bytes, CommandPool *pool);
	/* @index is the current frame's from here on: whatever was handed out
	 * under it the last time is free again */
	void SetCurrentIndex(int index);
	/* @bytes of mapped memory, and where they are: in @buffer at @offset,
	 * a multiple of 16; null if the ring has no room */
	u8 *Allocate(size_t bytes, vk::Buffer *buffer, vk::DeviceSize *offset);
	void Term();
	bool Ready() const { return mapped != nullptr; }

	// what has been done so far, for measuring
	u64 statFull = 0;		// pieces there was no room for
	u64 statPolled = 0;		// frames found done before their turn

private:
	enum { MAX_FRAMES = 8 };
	std::unique_ptr<BufferData> buffer;
	u8 *mapped = nullptr;
	u64 size = 0;
	u64 head = 0, tail = 0;			// bytes handed out and given back, ever
	u64 frameEnd[MAX_FRAMES] = {};	// where each place's last frame ended,
	bool frameLive[MAX_FRAMES] = {};	// while it may be on the GPU
	u32 current = 0;
	CommandPool *pool = nullptr;
};

/* An image of the texture pool's: optimal, sampled, copied to and from,
 * and written by the decoding kernel too where the decoder takes its
 * format, with a storage view of each level for that */
struct PooledImage : TexPoolEntry
{
	vk::UniqueImage image;
	Allocation allocation;
	vk::UniqueImageView view;
	u64 viewSerial = 0;     /* view's (Texture::ReadOnlyViewSerial()) */
	vk::UniqueImageView storageViews[TEXD_MAX_LEVELS];
	vk::ImageUsageFlags usage;
	vk::Format format = vk::Format::eUndefined;
	u32 levels = 1;
	bool storage = false;
	PooledImage *all = nullptr;		// every image there is, for Term()
};

/* The cache's textures' images (rend/tex_pool.h): made when the renderer
 * starts, in the device memory's large blocks, and handed out to the
 * textures that need one of their shape. And the pictures rendered to
 * (TakeTarget()), RGBA8, of their own kind of image. */
class TexturePool
{
public:
	/* (again with the renderer's next start, or a new size: nothing is done
	 * if it is made already) */
	void Init(const TexDecoder *decoder);
	void Term();
	bool Ready() const { return ready; }
	/* An image for a texture of @format, @width by @height, with its
	 * levels or not: a free one of that shape, or one made now if there is
	 * none. Null if the shape is not one of the pool's. */
	PooledImage *Take(vk::Format format, u32 width, u32 height, bool mips);
	/* An image to render to, @width by @height, with @usage */
	PooledImage *TakeTarget(u32 width, u32 height, const vk::ImageUsageFlags& usage);
	/* Before Init(): the images rendered to are also to be written by
	 * compute shaders (the compute renderer's renders to a texture) */
	void SetTargetStorage(bool on) { if (!ready) targetStorage = on; }
	/* ...and, after it, whether they are: the device can have it (an
	 * image made outside the pool for a render is made with the usage the
	 * drawer asks for, which then has it too) */
	bool TargetsStorable() const { return ready && targetStorage; }
	/* Free again now; or when the frames that may still use it are done */
	void Give(PooledImage *image) { book.Give(image); }
	void Retire(PooledImage *image) { book.Retire(image); }
	// (the texture command pool's frame index)
	void Frame(u32 index) { if (ready) book.Frame(index); }
	const TexPoolBook& Book() const { return book; }

private:
	enum { FORMATS = 8, TARGET = 7 };
	static int FormatIndex(vk::Format format);
	PooledImage *Make(int cls, bool late);

	TexPoolBook book;
	bool ready = false;
	const TexDecoder *decoder = nullptr;
	vk::Format formats[FORMATS];
	bool storable[FORMATS] = {};
	bool usable[FORMATS] = {};
	bool targetStorage = false;
	vk::ImageUsageFlags targetUsage;	// of the images rendered to
	PooledImage *all = nullptr;
};

class Texture : public BaseTextureCacheData
{
public:
	~Texture();
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded = false) override;
	void *UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps) override;
	bool GpuDecode(const texd_plan& plan) override;
	bool PartMemory(size_t bytes, void **memory) override;
	void UploadPart(const TexRegion *regions, u32 count, u8 *data, const size_t *offset) override;
	int GpuDecodePart(const texd_plan& plan, const TexRegion *regions, u32 count) override;
	bool CopyRender(BaseTextureCacheData *from, u32 x, u32 y, u32 scale, u32 columns) override;
	static UploadRing& Ring();
	/* The renderer's, while it has one */
	static void SetDecoder(TexDecoder *decoder) { Texture::decoder = decoder; }
	static void SetPool(TexturePool *pool) { Texture::pool = pool; }
	static TexturePool *Pool() { return pool; }
	/* A texture of the cache's: its images come from the pool. (Not the
	 * renderer's own pictures, which go when it says.) */
	void SetCached() { cached = true; }
	u64 GetIntId() { return (u64)reinterpret_cast<uintptr_t>(this); }
	void GetId(char *s, size_t n) override { snprintf(s, n, "%p", (void *)this); }
	bool IsNew() const { return !image.get() && pooled == nullptr; }
	vk::ImageView GetImageView() const { return pooled != nullptr ? *pooled->view : *imageView; }
	u64 GetViewSerial() const { return pooled != nullptr ? pooled->viewSerial : ownSerial; }
	vk::Image GetImage() const { return pooled != nullptr ? *pooled->image : *image; }
	vk::ImageView GetReadOnlyImageView() const { return readOnlyImageView ? readOnlyImageView : GetImageView(); }
	/* Which view GetReadOnlyImageView() is, of every one made: a number no
	 * other view ever has, which a handle - made again at the same value
	 * once its view is gone - is not. What a descriptor set is kept by. */
	u64 ReadOnlyViewSerial() const { return readOnlyImageView ? readOnlySerial : pooled != nullptr ? pooled->viewSerial : ownSerial; }
	/* The view the frames in flight sample, kept while a new image is made */
	void KeepReadOnlyView() { readOnlyImageView = GetImageView(); readOnlySerial = pooled != nullptr ? pooled->viewSerial : ownSerial; }
	/* A serial for a view just made */
	static u64 NewViewSerial() { return ++viewSerials; }
	void SetCommandBuffer(vk::CommandBuffer commandBuffer) { this->commandBuffer = commandBuffer; }
	/* Where a command buffer comes from when an upload needs one: a
	 * texture made on the GPU records nothing of its own */
	void SetCommandPool(CommandPool *commandPool) { this->commandPool = commandPool; }
	virtual bool Force32BitTexture(TextureType type) const override { return !VulkanContext::Instance()->IsFormatSupported(type); }

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
	vk::ImageView StorageView(u32 level) const { return pooled != nullptr ? *pooled->storageViews[level] : *storageViews[level]; }
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
	static u64 viewSerials;

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
	SamplerManager() { memset(samplers, 0, sizeof(samplers)); }
	~SamplerManager() { Term(); }

	void Init()
	{
		u32 i;
		Term();
		for (i = 0; i < SAMPLERS; i++)
			samplers[i] = Make(i);
	}
	vk::Sampler GetSampler(TSP tsp)
	{
		const u32 i = Index(tsp);
		/* (one asked for before Init(): made now) */
		if (!samplers[i])
			samplers[i] = Make(i);
		return vk::Sampler(samplers[i]);
	}
	void Term()
	{
		u32 i;
		for (i = 0; i < SAMPLERS; i++)
			if (samplers[i] != VK_NULL_HANDLE)
			{
				VulkanContext::Instance()->GetDevice().destroySampler(vk::Sampler(samplers[i]));
				samplers[i] = VK_NULL_HANDLE;
			}
	}
	static const u32 TSP_Mask = 0x7ef00;

private:
	enum { SAMPLERS = 16 * 2 * 3 * 3 };

	/* an axis: clamped, mirrored or repeated */
	static u32 Axis(u32 clamp, u32 flip) { return clamp ? 0 : flip ? 1 : 2; }
	static u32 Index(TSP tsp)
	{
		return ((tsp.MipMapD * 2 + (tsp.FilterMode != 0 ? 1 : 0)) * 3 + Axis(tsp.ClampU, tsp.FlipU)) * 3
				+ Axis(tsp.ClampV, tsp.FlipV);
	}
	static vk::SamplerAddressMode Mode(u32 axis)
	{
		return axis == 0 ? vk::SamplerAddressMode::eClampToEdge : axis == 1 ? vk::SamplerAddressMode::eMirroredRepeat
				: vk::SamplerAddressMode::eRepeat;
	}
	static VkSampler Make(u32 i)
	{
		VulkanContext *context = VulkanContext::Instance();
		const vk::SamplerAddressMode vRepeat = Mode(i % 3);
		const vk::SamplerAddressMode uRepeat = Mode(i / 3 % 3);
		const vk::Filter filter = i / 9 % 2 ? vk::Filter::eLinear : vk::Filter::eNearest;
		const u32 mipMapD = i / 18;
		const bool anisotropicFiltering = settings.rend.AnisotropicFiltering > 1 && context->SupportsSamplerAnisotropy()
				&& filter == vk::Filter::eLinear;
		const vk::SamplerCreateInfo info(vk::SamplerCreateFlags(), filter, filter,
				vk::SamplerMipmapMode::eNearest, uRepeat, vRepeat, vk::SamplerAddressMode::eClampToEdge, D_Adjust_LoD_Bias[mipMapD],
				anisotropicFiltering, std::min((float)settings.rend.AnisotropicFiltering, context->GetMaxSamplerAnisotropy()),
				false, vk::CompareOp::eNever,
				0.0f, 256.0f, vk::BorderColor::eFloatOpaqueBlack);
		vk::Sampler sampler;
		if (context->GetDevice().createSampler(&info, nullptr, &sampler) != vk::Result::eSuccess)
			return VK_NULL_HANDLE;
		return (VkSampler)sampler;
	}

	VkSampler samplers[SAMPLERS];
};

class FramebufferAttachment
{
public:
	FramebufferAttachment(vk::PhysicalDevice physicalDevice, vk::Device device)
		: format(vk::Format::eUndefined), physicalDevice(physicalDevice), device(device)
		{}
   /* @readable: with memory this side can read, to copy the image into (it is as large as the image) */
   void Init(u32 width, u32 height, vk::Format format, const vk::ImageUsageFlags& usage, bool readable = true,
		   vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1);
	void Reset() { image.reset(); imageView.reset(); }

	vk::ImageView GetImageView() const { return *imageView; }
	vk::Image GetImage() const { return *image; }
	const BufferData* GetBufferData() const { return stagingBufferData.get(); }
	vk::ImageView GetStencilView() const { return *stencilView; }
	vk::Extent2D getExtent() const { return extent; }
	/* GetImageView()'s serial (Texture::ReadOnlyViewSerial()) */
	u64 GetViewSerial() const { return viewSerial; }

private:
	vk::Format format;
	vk::Extent2D extent;

	std::unique_ptr<BufferData> stagingBufferData;
	Allocation allocation;
	vk::UniqueImage image;
	vk::UniqueImageView imageView;
	vk::UniqueImageView stencilView;
	u64 viewSerial = 0;

	vk::PhysicalDevice physicalDevice;
	vk::Device device;
};

/* A framebuffer kept for the attachments it was made of, known by their
 * views' serials: made again only when one of them, the render pass or the
 * size is another. (Made again in a frame slot whose last frame is done.) */
class KeptFramebuffer
{
public:
	vk::Framebuffer Get(vk::RenderPass pass, u32 count, const vk::ImageView *views, const u64 *serials, u32 width, u32 height)
	{
		bool same = framebuffer && pass == this->pass && count == this->count && width == this->width && height == this->height;
		for (u32 i = 0; same && i < count; i++)
			same = serials[i] == this->serials[i] && serials[i] != 0;
		if (!same)
		{
			framebuffer = VulkanContext::Instance()->GetDevice().createFramebufferUnique(vk::FramebufferCreateInfo(
					vk::FramebufferCreateFlags(), pass, count, views, width, height, 1));
			this->pass = pass;
			this->count = count;
			this->width = width;
			this->height = height;
			for (u32 i = 0; i < count && i < 3; i++)
				this->serials[i] = serials[i];
		}
		return *framebuffer;
	}
	vk::Framebuffer Handle() const { return *framebuffer; }

private:
	vk::UniqueFramebuffer framebuffer;
	vk::RenderPass pass;
	u32 count = 0, width = 0, height = 0;
	u64 serials[3] = {};
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
	template<typename T>
	void EmptyTrash(T& v)
	{
		if (v.size() < currentIndex + 1)
			v.resize(currentIndex + 1);
		v[currentIndex].clear();
	}
	grow_buf_t inFlightLists = GROW_BUF_INIT;	/* InFlightList, a place each */
	u32 inFlightCount = 0;
	std::vector<std::vector<vk::UniqueImageView>> trashedImageViews;
	std::vector<std::vector<vk::UniqueImage>> trashedImages;
	std::vector<std::vector<Allocation>> trashedMem;
	u32 currentIndex = 0;
};
