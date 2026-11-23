/* Textures made on the GPU from their bytes in video memory.
 *
 * The texture cache converts a texture on the CPU, texel by texel, into
 * the upload ring, and the GPU copies the texels to the image. Here the
 * texture's bytes are copied into a ring of their own as they are - the
 * texels, the codebook of a compressed texture, the part of the palette a
 * paletted one uses - and a compute kernel makes the texels from them,
 * straight into the image: rend/texdecode.h, the same text the CPU side
 * is tested against, so the texels are the converters' to the bit. A
 * planar 16-bit texture in a format the image can have its texels in as
 * they are - 565 always, 1555 as A1R5G5B5, 4444 as B4G4R4A4 with its
 * channels swizzled back by the view - is not converted at all: its rows
 * are copied to the image as they are.
 *
 * What a frame asks for is recorded at the end of Process(), before the
 * frame's draws, in one command buffer of the texture pool's: a barrier for
 * all the images, the copies and the dispatches, a barrier for all of them
 * again. Everything is made when the renderer starts and sized then - the
 * pipelines, the ring (from the video memory budget), the descriptor pools
 * and the lists - and nothing is allocated while the game runs. A frame
 * that asks for more than there is room for has the rest converted on the
 * CPU, as before. The ring's piece and the descriptor pool of a frame are
 * used again when that frame's place comes round, which is when the
 * texture command pool has seen its fence (CommandPool::BeginFrame()). */
#pragma once
#include "vulkan.h"
#include "buffer.h"
#include "pso.h"
#include "vk_tex_decoder.h"

/* The C++ renderers' view of a vk_tex_decoder, until they are C too */
class TexDecoder
{
public:
	TexDecoder() { vk_tex_decoder_construct(&d); }
	~TexDecoder() { vk_tex_decoder_term(&d); }
	TexDecoder(const TexDecoder&) = delete;
	TexDecoder& operator=(const TexDecoder&) = delete;
	void Init() { vk_tex_decoder_init(&d); }
	void Term() { vk_tex_decoder_term(&d); }
	bool Ready() const { return d.ready; }
	bool Takes(vk::Format format) const { return vk_tex_decoder_takes(&d, (VkFormat)format); }
	vk::Format NativeFormat(TextureType type) const { return (vk::Format)vk_tex_decoder_native_format(&d, (int)type); }
	bool Room(u32 bytes, u32 dispatches) const { return vk_tex_decoder_room(&d, bytes, dispatches); }
	void BeginFrame(int index) { vk_tex_decoder_begin_frame(&d, index); }
	u8 *Reserve(u32 bytes, u32 *offset) { return vk_tex_decoder_reserve(&d, bytes, offset); }

	/* vk_texd_level's, with the view as vulkan.hpp has it */
	struct Level
	{
		vk::ImageView view;
		texd_u32 pc[6];
		u32 group[4];
	};
	int AddDecode(vk::Image image, bool fresh, u32 mipLevels, u32 out, const Level *levels, u32 count)
	{
		static_assert(sizeof(Level) == sizeof(vk_texd_level) && offsetof(Level, pc) == offsetof(vk_texd_level, pc),
				"TexDecoder::Level is vk_texd_level");
		return vk_tex_decoder_add_decode(&d, (VkImage)image, fresh, mipLevels, out,
				reinterpret_cast<const vk_texd_level *>(levels), count);
	}
	int AddCopy(vk::Image image, bool fresh, u32 offset, u32 rowLength, u32 width, u32 height, u32 y = 0)
	{
		return vk_tex_decoder_add_copy(&d, (VkImage)image, fresh, offset, rowLength, width, height, y);
	}
	bool RunsPart() const { return vk_tex_decoder_runs_part(&d); }
	bool Cancel(int job) { return vk_tex_decoder_cancel(&d, job); }
	u32 Serial() const { return d.serial; }
	template<typename Pool>
	void Flush(Pool& pool)
	{
		if (!vk_tex_decoder_flush(&d, &pool.p))
			throwResultException(vk::Result::eErrorInitializationFailed, "vkCreateComputePipelines");
	}

private:
	vk_tex_decoder d;
};
