/* The texture cache works for the renderer: it reads the registers the
 * frame is rendered with, not the ones the emulation has moved on to. */
#define PVR_REGS_FOR_RENDERER
#include "TexCache.h"
#include "lockfree.h"
#include "CustomTexture.h"
#ifdef HAVE_TEXUPSCALE
#include "deps/xbrz/xbrz.h"
#endif
#include "hw/pvr/pvr_mem.h"
#include "hw/mem/_vmem.h"
#include "hw/sh4/modules/mmu.h"
#include "texdecode.h"

#include <algorithm>
// for a hash state that can be a local: ComputeHash() allocates nothing
#define XXH_STATIC_LINKING_ONLY
#include <deps/xxhash/xxhash.h>

#if defined(HAVE_TEXUPSCALE) && !defined(TARGET_NO_OPENMP)
#include <omp.h>
#endif

u8* vq_codebook;
u32 palette_index;
bool KillTex=false;
u32 palette16_ram[1024];
u32 palette32_ram[1024];
u32 pal_hash_256[4];
u32 pal_hash_16[64];
bool palette_updated;

// Rough approximation of LoD bias from D adjust param, only used to increase LoD
const std::array<f32, 16> D_Adjust_LoD_Bias = {
		0.f, -4.f, -2.f, -1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f
};
static void rend_text_invl(vram_block* bl);

u32 detwiddle[2][11][1024];
//input : address in the yyyyyxxxxx format
//output : address in the xyxyxyxy format
//U : x resolution , V : y resolution
//twiddle works on 64b words


static u32 twiddle_slow(u32 x,u32 y,u32 x_sz,u32 y_sz)
{
	u32 rv=0;//low 2 bits are directly passed  -> needs some misc stuff to work.However
			 //Pvr internally maps the 64b banks "as if" they were twiddled :p

	u32 sh=0;
	x_sz>>=1;
	y_sz>>=1;
	while(x_sz!=0 || y_sz!=0)
	{
		if (y_sz)
		{
			u32 temp=y&1;
			rv|=temp<<sh;

			y_sz>>=1;
			y>>=1;
			sh++;
		}
		if (x_sz)
		{
			u32 temp=x&1;
			rv|=temp<<sh;

			x_sz>>=1;
			x>>=1;
			sh++;
		}
	}	
	return rv;
}

static void BuildTwiddleTables()
{
	for (u32 s = 0; s < 11; s++)
	{
		u32 x_sz = 1024;
		u32 y_sz = 1 << s;
		for (u32 i = 0; i < x_sz; i++)
		{
			detwiddle[0][s][i] = twiddle_slow(i, 0, x_sz, y_sz);
			detwiddle[1][s][i] = twiddle_slow(0, i, y_sz, x_sz);
		}
	}
}

static OnLoad btt(&BuildTwiddleTables);

/* Converts the palette for the renderer when it is not the one converted
 * last. Called by the renderer for each frame, with the registers that
 * frame is drawn with.
 *
 * This used to be driven by a flag the register writes set, and was run
 * by the emulation thread when the game started a render. With Threaded
 * Rendering the frame before could still be drawing then, reading the very
 * tables this rewrites. Now the tables belong to the thread that draws, and
 * whether they are stale is seen from the palette itself: 4 KB to compare. */
void palette_update()
{
	static u32 converted[1024];
	static u32 converted_ctrl = 0xFFFFFFFF;
	const u32 ctrl = PAL_RAM_CTRL & 3;

	if (ctrl == converted_ctrl && !memcmp(converted, PALETTE_RAM, sizeof(converted)))
		return;
	converted_ctrl = ctrl;
	memcpy(converted, PALETTE_RAM, sizeof(converted));
	palette_updated = true;
	switch (ctrl)
	{
	case 0:
		for (int i=0;i<1024;i++)
		{
			palette16_ram[i] = ARGB1555(PALETTE_RAM[i]);
			palette32_ram[i] = ARGB1555_32(PALETTE_RAM[i]);
		}
		break;

	case 1:
		for (int i=0;i<1024;i++)
		{
			palette16_ram[i] = ARGB565(PALETTE_RAM[i]);
			palette32_ram[i] = ARGB565_32(PALETTE_RAM[i]);
		}
		break;

	case 2:
		for (int i=0;i<1024;i++)
		{
			palette16_ram[i] = ARGB4444(PALETTE_RAM[i]);
			palette32_ram[i] = ARGB4444_32(PALETTE_RAM[i]);
		}
		break;

	case 3:
		for (int i=0;i<1024;i++)
		{
			palette16_ram[i] = ARGB8888(PALETTE_RAM[i]);
			palette32_ram[i] = ARGB8888_32(PALETTE_RAM[i]);
		}
		break;
	}
	for (int i = 0; i < 64; i++)
		pal_hash_16[i] = XXH32(&PALETTE_RAM[i << 4], 16 * 4, 7);
	for (int i = 0; i < 4; i++)
		pal_hash_256[i] = XXH32(&PALETTE_RAM[i << 8], 256 * 4, 7);
}

static std::vector<vram_block*> VramLocks[VRAM_SIZE_MAX / PAGE_SIZE];
VArray2 vram;  // vram 32-64b

//List functions
//
void vramlock_list_remove(vram_block* block)
{
	u32 base = block->start / PAGE_SIZE;
	u32 end = block->end / PAGE_SIZE;

	for (u32 i = base; i <= end; i++)
	{
		std::vector<vram_block*>& list = VramLocks[i];
		for (auto& lock : list)
		{
			if (lock == block)
				lock = nullptr;
		}
	}
}
 
void vramlock_list_add(vram_block* block)
{
	u32 base = block->start / PAGE_SIZE;
	u32 end = block->end / PAGE_SIZE;

	for (u32 i = base; i <= end; i++)
	{
		std::vector<vram_block*>& list = VramLocks[i];
		// If the list is empty then we need to protect vram, otherwise it's already been done
		if (list.empty() || std::all_of(list.begin(), list.end(), [](vram_block *block) { return block == nullptr; }))
			_vmem_protect_vram(i * PAGE_SIZE, PAGE_SIZE);
		auto it = std::find(list.begin(), list.end(), nullptr);
		if (it != list.end())
			*it = block;
		else
			list.push_back(block);
	}
}
 
/* Writes to protected video memory, by page.
 *
 * The lock lists above belong to the render thread; nothing else reads or
 * changes them. The emulation thread finds out that a protected page was
 * written when the write faults, possibly inside a signal handler, and all
 * it does there is open the page up again and mark it here. The render
 * thread collects the marks before it looks at any texture and drops every
 * lock on those pages, which is what marks their textures as changed.
 *
 * The emulation thread unprotects first and marks second. A page it has
 * opened up is therefore always marked afterwards, so the render thread
 * can go on treating "a page with locks on it is protected" as true: when
 * it is not, a mark is on its way that takes those locks off again. */
static cMarkSet<VRAM_SIZE_MAX / PAGE_SIZE> vram_writes;

/* Render thread: moved for every written page it collects. In a render
 * nothing else changes video memory under a texture already hashed in it:
 * a picture rendered to a texture is packed in when a read first touches
 * it, and the hash is such a read. */
static u32 vram_write_epoch;

static void libCore_vramlock_Unlock_block_wb(vram_block* block);

/* Render thread: apply the writes the emulation thread has marked. */
static void vramlock_collect()
{
	vram_writes.Collect([](unsigned page) {
		std::vector<vram_block *>& list = VramLocks[page];

		vram_write_epoch++;

		for (auto& lock : list)
		{
			if (lock != nullptr)
			{
            rend_text_invl(lock);

				if (lock != nullptr)
				{
					ERROR_LOG(PVR, "Error : pvr is supposed to remove lock");
					die("Invalid state");
				}
			}
		}
		list.clear();
	});
}

void libCore_vramlock_Lock(u32 start_offset64, u32 end_offset64, BaseTextureCacheData *texture)
{
	if (end_offset64>(VRAM_SIZE-1))
	{
		WARN_LOG(PVR, "vramlock_Lock_64: end_offset64>(VRAM_SIZE-1) \n Tried to lock area out of vram , possibly bug on the pvr plugin");
		end_offset64=(VRAM_SIZE-1);
	}

	if (start_offset64>end_offset64)
	{
		WARN_LOG(PVR, "vramlock_Lock_64: start_offset64>end_offset64 \n Tried to lock negative block , possibly bug on the pvr plugin");
		start_offset64=0;
	}

	/* Writes that came before this lock are not its business. */
	vramlock_collect();

	if (texture->lock_block == nullptr)
	{
		vram_block* block=(vram_block* )malloc(sizeof(vram_block));

		block->end=end_offset64;
		block->start=start_offset64;
		block->len=end_offset64-start_offset64+1;
		block->userdata = texture;
		block->type=64;

		// This also protects vram if needed
		vramlock_list_add(block);
		texture->lock_block = block;
	}
}

/* A write to video memory hit a protected page. Called on the emulation
 * thread from the fault handler, so it stays within what a signal handler
 * may do: system calls and atomics, no locks and no allocation. The render
 * thread calls it too, before it writes to video memory itself. */
bool VramLockedWriteOffset(size_t offset)
{
	if (offset >= VRAM_SIZE)
		return false;

	_vmem_unprotect_vram((u32)(offset & ~PAGE_MASK), PAGE_SIZE);
	vram_writes.Mark((unsigned)(offset / PAGE_SIZE));

	return true;
}

/* The same for a run of pages, with one change of protection for all of
 * them. Any thread, the machine standing still. */
void VramLockedWriteRange(u32 offset, u32 size)
{
	if (size == 0 || offset >= VRAM_SIZE)
		return;
	if (size > VRAM_SIZE - offset)
		size = VRAM_SIZE - offset;

	const u32 first = offset / PAGE_SIZE, last = (offset + size - 1) / PAGE_SIZE;

	_vmem_unprotect_vram(first * PAGE_SIZE, (last - first + 1) * PAGE_SIZE);
	for (u32 page = first; page <= last; page++)
		vram_writes.Mark(page);
}

/* Video memory from a save state: @from is all of it. Any thread, the
 * machine standing still.
 *
 * It used to be copied over whole and every texture thrown away, to be made
 * again from the same bytes as often as not: with run-ahead a state is
 * loaded every frame, and it is the state of one frame ago. Here a page
 * that textures were made from - a protected one - is compared first. The
 * same in the state, it is left alone and so are its textures; another, it
 * is written as the game would have written it, which is what has them made
 * again. The pages nothing was made from are copied as before, a run of
 * them at a time. */
void vram_load(const u8 *from)
{
	u8 * const to = &vram[0];
	u32 run = 0;		// where the pages to be copied in one go start

	for (u32 addr = 0; addr < VRAM_SIZE; addr += PAGE_SIZE)
	{
		if (!_vmem_vram_page_protected(addr / PAGE_SIZE))
			continue;
		if (addr > run)
			memcpy(to + run, from + run, addr - run);
		run = addr + PAGE_SIZE;
		if (memcmp(to + addr, from + addr, PAGE_SIZE) == 0)
			continue;
		VramLockedWriteOffset(addr);
		memcpy(to + addr, from + addr, PAGE_SIZE);
	}
	if (VRAM_SIZE > run)
		memcpy(to + run, from + run, VRAM_SIZE - run);
}

bool VramLockedWrite(u8* address)
{
	u32 offset = _vmem_get_vram_offset(address);
	if (offset == (u32)-1)
		return false;
	return VramLockedWriteOffset(offset);
}

//unlocks mem
//also frees the handle
static void libCore_vramlock_Unlock_block_wb(vram_block* block)
{
	vramlock_list_remove(block);
	free(block);
}

#ifdef HAVE_TEXUPSCALE
#ifndef TARGET_NO_OPENMP
static inline int getThreadCount()
{
	int tcount = omp_get_num_procs() - 1;
	if (tcount < 1)
		tcount = 1;
	return std::min(tcount, 3);
}

template<typename Func>
void parallelize(Func func, int start, int end)
{
	int tcount = getThreadCount();
#pragma omp parallel num_threads(tcount)
	{
		int num_threads = omp_get_num_threads();
		int thread = omp_get_thread_num();
		int chunk = (end - start) / num_threads;
		func(start + chunk * thread,
				num_threads == thread + 1 ? end
						: (start + chunk * (thread + 1)));
	}
}

#endif

static struct xbrz::ScalerCfg xbrz_cfg;

void UpscalexBRZ(int factor, u32* source, u32* dest, int width, int height, bool has_alpha)
{
#ifndef TARGET_NO_OPENMP
	parallelize([=](int start, int end) {
		xbrz::scale(factor, source, dest, width, height, has_alpha ? xbrz::ColorFormat::ARGB : xbrz::ColorFormat::RGB,
				xbrz_cfg, start, end);
	}, 0, height);
#else
	xbrz::scale(factor, source, dest, width, height, has_alpha ? xbrz::ColorFormat::ARGB : xbrz::ColorFormat::RGB, xbrz_cfg);
#endif
}

#endif

struct PvrTexInfo
{
	const char* name;
	int bpp;        //4/8 for pal. 16 for yuv, rgb, argb
	TextureType type;
	// Conversion to 16 bpp
	TexConvFP *PL;
	TexConvFP *TW;
	TexConvFP *VQ;
	// Conversion to 32 bpp
	TexConvFP32 *PL32;
	TexConvFP32 *TW32;
	TexConvFP32 *VQ32;
	// Conversion to 8 bpp (palette)
	TexConvFP8 *TW8;
	// Planar and compressed, to 32 bpp
	TexConvFP32 *PLVQ32;
};

static const PvrTexInfo format[8] =
{	// name     bpp Final format			   Planar		Twiddled	 VQ				Planar(32b)    Twiddled(32b)  VQ (32b)      Palette (8b)
	{"1555", 	16,	TextureType::_5551,        tex1555_PL,  tex1555_TW,  tex1555_VQ,    tex1555_PL32,  tex1555_TW32,  tex1555_VQ32, nullptr, tex1555_PLVQ32 },	    //1555
	{"565", 	16, TextureType::_565,         tex565_PL,   tex565_TW,   tex565_VQ,     tex565_PL32,   tex565_TW32,   tex565_VQ32,  nullptr, tex565_PLVQ32 },	    //565
	{"4444", 	16, TextureType::_4444,        tex4444_PL,  tex4444_TW,  tex4444_VQ,    tex4444_PL32,  tex4444_TW32,  tex4444_VQ32, nullptr, tex4444_PLVQ32 },	    //4444
	{"yuv", 	16, TextureType::_8888,        nullptr,     nullptr,     nullptr,       texYUV422_PL,  texYUV422_TW,  texYUV422_VQ, nullptr, texYUV422_PLVQ },	    //yuv
	{"bumpmap", 16, TextureType::_4444,        texBMP_PL,   texBMP_TW,	 texBMP_VQ,     tex4444_PL32,  tex4444_TW32,  tex4444_VQ32, nullptr, tex4444_PLVQ32 },      //bump map
	{"pal4", 	4,	TextureType::_5551,		   nullptr,     texPAL4_TW,  texPAL4_VQ,    nullptr,       texPAL4_TW32,  texPAL4_VQ32, texPAL4PT_TW },	//pal4
	{"pal8", 	8,	TextureType::_5551,		   nullptr,     texPAL8_TW,  texPAL8_VQ,    nullptr,       texPAL8_TW32,  texPAL8_VQ32, texPAL8PT_TW },	//pal8
	{"ns/1555", 0},	                                                                                                                                // Not supported (1555)
};

static const u32 VQMipPoint[11] =
{
	0x00000,//1
	0x00001,//2
	0x00002,//4
	0x00006,//8
	0x00016,//16
	0x00056,//32
	0x00156,//64
	0x00556,//128
	0x01556,//256
	0x05556,//512
	0x15556//1024
};
static const u32 OtherMipPoint[11] =
{
	0x00003,//1
	0x00004,//2
	0x00008,//4
	0x00018,//8
	0x00058,//16
	0x00158,//32
	0x00558,//64
	0x01558,//128
	0x05558,//256
	0x15558,//512
	0x55558//1024
};

static const TextureType PAL_TYPE[4] = {
	TextureType::_5551, TextureType::_565, TextureType::_4444, TextureType::_8888
};

void BaseTextureCacheData::PrintTextureName()
{
	char str[512];
	sprintf(str, "Texture: %s", GetPixelFormatName());

	if (tcw.VQ_Comp)
		strcat(str, " VQ");
	else if (tcw.ScanOrder == 0)
		strcat(str, " TW");
	else if (tcw.StrideSel)
		strcat(str, " Stride");

	if (tcw.ScanOrder == 0 && tcw.MipMapped)
		strcat(str, " MM");
	if (tsp.FilterMode != 0)
		strcat(str, " Bilinear");

	sprintf(str + strlen(str), " %dx%d @ 0x%X", 8 << tsp.TexU, 8 << tsp.TexV, tcw.TexAddr << 3);
	std::string id = GetId();
	sprintf(str + strlen(str), " id=%s", id.c_str());
	DEBUG_LOG(RENDERER, "%s", str);
}

/* The vram content backing this texture, over the same range the
 * write-protection lock would cover, clamped to VRAM like the lock is,
 * hashed piece by piece. For frequently-updated textures that run
 * unprotected. */
bool BaseTextureCacheData::HashPieces(u32 *changed)
{
	const u32 start = sa_tex & VRAM_MASK;
	u32 end = sa + size;
	u32 first, count, i, n;
	u32 *bits;
	bool all;

	*changed = 0;
	if (end > VRAM_SIZE)
		end = VRAM_SIZE;
	if (end <= start)
		return true;
	first = start / TEX_HASH_PIECE;
	count = (end - 1) / TEX_HASH_PIECE - first + 1;
	all = piece_hash == NULL || count != pieces || first != piece_first;
	if (all)
	{
		free(piece_hash);
		piece_hash = (u32 *)malloc(((size_t)count + (count + 31) / 32) * sizeof(u32));
		pieces = 0;
		if (piece_hash == NULL)
			return false;
		piece_first = first;
		pieces = count;
	}
	bits = piece_hash + count;
	memset(bits, 0, ((count + 31) / 32) * sizeof(u32));
	n = 0;
	for (i = 0; i < count; i++)
	{
		const u32 from = i == 0 ? start : (first + i) * TEX_HASH_PIECE;
		const u32 to = i == count - 1 ? end : (first + i + 1) * TEX_HASH_PIECE;
		const u32 hash = XXH32(&vram[from], to - from, 7);
		if (all || hash != piece_hash[i])
		{
			piece_hash[i] = hash;
			bits[i >> 5] |= 1u << (i & 31);
			n++;
		}
	}
	*changed = n;
	return true;
}

//true if : dirty or paletted texture and hashes don't match
bool BaseTextureCacheData::NeedsUpdate() {
	/* Take in what the emulation thread has written since the last look. */
	if (vram_writes.Pending())
		vramlock_collect();

	bool rc = dirty != 0;
	if (stride_read != 0)
	{
		u32 stride = w;
		if (tcw.StrideSel)
		{
			stride = (TEXT_CONTROL & 31) * 32;
			if (stride == 0)
				stride = w;
		}
		if (stride != stride_read)
			rc = true;
	}
	if (tex_type != TextureType::_8)
	{
		if (tcw.PixelFmt == PixelPal4 && palette_hash != pal_hash_16[tcw.PalSelect])
			rc = true;
		else if (tcw.PixelFmt == PixelPal8 && palette_hash != pal_hash_256[tcw.PalSelect >> 4])
			rc = true;
	}
	/* Frequently-updated texture running unprotected (see Update): compare the
	 * content hash of the vram range instead of relying on a write fault.
	 * Once a render: its parse asks for a texture for every polygon that
	 * uses it. */
	if (!rc && lock_block == nullptr && Updates >= FREQUENT_UPDATE_COUNT
			&& (hash_frame != FrameCount || hash_epoch != vram_write_epoch))
		rc = HashChanged();

	return rc;
}

/* (NeedsUpdate()'s, kept out of its way) */
bool BaseTextureCacheData::HashChanged()
{
	u32 changed;

	if (!HashPieces(&changed))
		return true;
	if (changed != 0)
	{
		/* (until the Update() that follows) */
		changed_pieces = true;
		return true;
	}
	hash_frame = FrameCount;
	hash_epoch = vram_write_epoch;
	return false;
}

void texcache_stop_custom_loads()
{
	custom_texture.Terminate();
}

bool BaseTextureCacheData::Delete()
{
	if (retro_atomic_load_acquire_int(&custom_load_in_progress) > 0)
		return false;

	if (lock_block)
		libCore_vramlock_Unlock_block_wb(lock_block);
	lock_block = nullptr;

	free(piece_hash);
	piece_hash = NULL;
	pieces = 0;
	free(custom_image_data);

	return true;
}

void BaseTextureCacheData::Create()
{
	//Reset state info ..
	Updates = 0;
	dirty = FrameCount;
	stride_read = 0;
	lock_block = nullptr;
	free(piece_hash);
	piece_hash = NULL;
	pieces = 0;
	made_type = 0;
	made_mips = false;
	changed_pieces = false;
	custom_image_data = nullptr;
	retro_atomic_store_release_int(&custom_load_in_progress, 0);
	retro_atomic_int_init(&custom_request_hash, 0);
	retro_atomic_int_init(&custom_request_old_hash, 0);
	retro_atomic_int_init(&custom_request_old_vqhash, 0);

	//decode info from tsp/tcw into the texture struct
	tex = &format[tcw.PixelFmt == PixelReserved ? Pixel1555 : tcw.PixelFmt];	//texture format table entry

	sa_tex = (tcw.TexAddr << 3) & VRAM_MASK;	//texture start address
	sa = sa_tex;								//data texture start address (modified for MIPs, as needed)
	w = 8 << tsp.TexU;							//tex width
	h = 8 << tsp.TexV;							//tex height

	//PAL texture
	if (tex->bpp == 4)
		palette_index = tcw.PalSelect << 4;
	else if (tex->bpp == 8)
		palette_index = (tcw.PalSelect >> 4) << 8;

	texconv8 = nullptr;

	/* Compressed and mipmapped, a texture is twiddled whatever this bit
	 * says: Star Wars Demolition sets it, and its textures came out
	 * corrupted (upstream: or took the emulator down). Not for a paletted
	 * texture, where the bit is part of the palette selection. */
	if (tcw.VQ_Comp == 1 && tcw.MipMapped == 1 && !IsPaletted())
		tcw.ScanOrder = 0;

	if (tcw.ScanOrder && (tex->PL || tex->PL32))
	{
		//Texture is stored 'planar' in memory, no deswizzle is needed
		if (tcw.MipMapped != 0)
		{
			WARN_LOG(RENDERER, "Warning: planar texture with mipmaps (invalid)");
			tcw.MipMapped = 0;
		}

		//Planar textures support stride selection, mostly used for non power of 2 textures (videos)
		int stride = w;
		if (tcw.StrideSel)
		{
			stride = (TEXT_CONTROL & 31) * 32;
			// a stride of nothing is no stride: the texture's own width
			if (stride == 0)
				stride = w;
		}

		if (tcw.VQ_Comp != 0 && tex->PLVQ32 != nullptr)
		{
			/* Compressed as well. This was taken for a mistake and the
			 * compression ignored; it is a texture like any other (the
			 * Genesis emulator in the Teenage Mutant Ninja Turtles
			 * collection draws its screen with one). */
			vq_codebook = sa;
			texconv = nullptr;
			texconv32 = tex->PLVQ32;
			size = 256 * 4 * 2 + stride * h / 4;
		}
		else
		{
			tcw.VQ_Comp = 0;
			//Call the format specific conversion code
			texconv = tex->PL;
			texconv32 = tex->PL32;
			//calculate the size, in bytes, for the locking
			size = stride * h * tex->bpp / 8;
		}
	}
	else
	{
		/* These two bits mean nothing for a twiddled texture and are taken
		 * out of the way - unless the texture is paletted. There they are
		 * the top two bits of the palette selection, which says which part
		 * of the palette the texture uses. With them cleared, NeedsUpdate()
		 * looked for changes in the wrong part: a texture using the upper
		 * half of the palette was not decoded again when its colours
		 * changed, and was when some other texture's did. */
		if (!IsPaletted())
		{
			tcw.ScanOrder = 0;
			tcw.StrideSel = 0;
		}
		// Quake 3 Arena uses one
		if (tcw.MipMapped)
			// Mipmapped texture must be square and TexV is ignored
			h = w;

		if (tcw.VQ_Comp)
		{
			verify(tex->VQ != NULL || tex->VQ32 != NULL);
			vq_codebook = sa;
			if (tcw.MipMapped)
				sa += VQMipPoint[tsp.TexU + 3];
			texconv = tex->VQ;
			texconv32 = tex->VQ32;
			/* From sa: the codebook the converters skip, and a byte for
			 * every four pixels. This was half the pixels' share and no
			 * codebook, so a write to the rest of the texture was not
			 * watched for, and the texture stayed as it was. */
			size = 256 * 4 * 2 + w * h / 4;
		}
		else
		{
			verify(tex->TW != NULL || tex->TW32 != NULL);
			if (tcw.MipMapped)
				sa += OtherMipPoint[tsp.TexU + 3] * tex->bpp / 8;
			texconv = tex->TW;
			texconv32 = tex->TW32;
			size = w * h * tex->bpp / 8;
			texconv8 = tex->TW8;
		}
	}
}

/* Where a texture is converted before it is handed to the GPU. One piece of
 * memory, kept from one texture to the next and only ever grown: every
 * update used to allocate a buffer the size of its texture and free it
 * again a moment later, sixty megabytes a second of that for a video
 * playing as a 512x512 texture. Whatever is in it is the last texture's and
 * of no use, so growing it copies nothing. Updates happen on the one thread
 * that renders. At most a 1024x1024 texture with its mipmaps, 5.6 MB. */
static void *tex_scratch;
static size_t tex_scratch_size;

static void *texcache_scratch(size_t size)
{
	if (size > tex_scratch_size)
	{
		free(tex_scratch);
		tex_scratch = malloc(size);
		tex_scratch_size = tex_scratch != NULL ? size : 0;
	}
	return tex_scratch;
}

/* Where Update() converts a texture into: the renderer's memory, which the
 * upload then takes as it is, or the scratch memory, which it copies. */
void *BaseTextureCacheData::ConvertInto(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps, bool direct)
{
	void *memory = direct ? UploadMemory(width, height, bytes, pixel, mipmaps) : NULL;

	return memory != NULL ? memory : texcache_scratch(bytes);
}

void texcache_scratch_free()
{
	free(tex_scratch);
	tex_scratch = NULL;
	tex_scratch_size = 0;
}

void BaseTextureCacheData::ComputeHash()
{
	// twiddled and compressed; for a paletted texture the scan order bit is part of the palette selection
	if (tcw.VQ_Comp && !(tcw.ScanOrder && (tex->PL || tex->PL32)))
	{
		/* A compressed texture's name used to come from the first w*h/8
		 * bytes at sa - part of the codebook and none or some of the
		 * texture, so that two textures could share one. It is now the
		 * hash of the codebook and then of the whole texture, as upstream
		 * names them; the old name is kept, and custom textures saved
		 * under it are still found. */
		XXH32_state_t state;

		old_vqtexture_hash = XXH32(&vram[sa], w * h / 8, 7);
		if (IsPaletted())
			old_vqtexture_hash ^= palette_hash;
		old_texture_hash = old_vqtexture_hash;
		old_vqtexture_hash ^= tcw.full & 0xFC000000;

		XXH32_reset(&state, 7);
		XXH32_update(&state, &vram[sa_tex], 256 * 4 * 2);
		XXH32_update(&state, &vram[sa + 256 * 4 * 2], w * h / 4);
		texture_hash = XXH32_digest(&state);
		if (IsPaletted())
			texture_hash ^= palette_hash;
		texture_hash ^= tcw.full & 0xFC000000;
		return;
	}
	old_vqtexture_hash = 0;
	texture_hash = XXH32(&vram[sa], size, 7);
	if (IsPaletted())
		texture_hash ^= palette_hash;
	old_texture_hash = texture_hash;
	texture_hash ^= tcw.full & 0xFC000000;	// everything but texaddr, reserved and stride
}

/* What GpuDecode() is handed: the texture's bytes and, for each level,
 * what Update() below would convert it with - the same addresses, sizes
 * and converter, as rend/texdecode.h has them. @out is TEXD_OUT8, 16 or
 * 32, the converter Update() would take; @stride and @rows what it would
 * read a planar texture with. False if there is nothing the kernel does
 * for this texture. */
bool BaseTextureCacheData::PlanDecode(texd_plan& plan, u32 stride, u32 rows, u32 out, bool mipmapped)
{
	const u32 fmt = (u32)(tex - format);
	const bool planar = tcw.ScanOrder && (tex->PL || tex->PL32);
	const u32 scan = planar ? (tcw.VQ_Comp ? TEXD_PLVQ : TEXD_PL) : (tcw.VQ_Comp ? TEXD_VQ : TEXD_TW);
	const u32 lw = bitscanrev(w), lh = bitscanrev(h);
	const u32 first = sa - sa_tex + (tcw.VQ_Comp ? 256 * 4 * 2 : 0);

	if (fmt > TEXD_PAL8 || tex->bpp == 0 || stride > 0xFFFF || rows > 0xFFFF)
		return false;
	/* A twiddled texture that runs past the end of video memory - a
	 * paletted one whose palette selection has the bit planar textures are
	 * cut short by - is read there by the converters, from what follows
	 * (the memory's mirror). Not copied for the GPU: left to them. */
	if (!planar && sa + (tcw.VQ_Comp ? 256 * 4 * 2 + w * h / 4 : w * h * tex->bpp / 8) > VRAM_SIZE)
		return false;
	plan.src = sa_tex;
	plan.bytes = sa + size - sa_tex;
	plan.pal_entries = IsPaletted() && out != TEXD_OUT8 ? (tex->bpp == 4 ? 16 : 256) : 0;
	plan.pal_index = palette_index;
	plan.pal = (plan.bytes + 3) & ~3u;
	plan.out = out;
	plan.width = w;
	plan.height = h;
	plan.mipmapped = mipmapped;
	plan.native = scan == TEXD_PL && out == TEXD_OUT16 && fmt != TEXD_YUV && stride >= w && rows == h;
	if (!mipmapped)
	{
		texd_level& lv = plan.level[0];
		lv.data = first;
		lv.wh = w | (h << 16);
		lv.sr = stride | (rows << 16);
		lv.mode = TEXD_MODE(fmt, scan, out, 0, lw, lh);
		lv.mip = 0;
		plan.levels = 1;
		return true;
	}
	// smallest first, as the converters go; level i is 2^i texels square
	plan.levels = tsp.TexU + 3 + 1;
	if (plan.levels > TEXD_MAX_LEVELS)
		return false;
	for (u32 i = 0; i < plan.levels; i++)
	{
		texd_level& lv = plan.level[i];
		u32 f = fmt;
		u32 corner = 0;
		u32 l = i;
		if (tcw.VQ_Comp)
		{
			lv.data = 256 * 4 * 2 + VQMipPoint[i];
			if (i == 0)
			{
				/* the corner of a 2x2 block; of the 2x2 level's for YUV,
				 * whose 1x1 block is not a colour */
				corner = 1;
				l = 1;
				if (fmt == TEXD_YUV)
					lv.data = 256 * 4 * 2 + VQMipPoint[1];
			}
		}
		else
		{
			lv.data = OtherMipPoint[i] * tex->bpp / 8;
			if (fmt == TEXD_YUV && i == 0)
				f = TEXD_565;	// as Update() does it
		}
		lv.wh = (1u << i) | ((1u << i) << 16);
		lv.sr = 0;
		lv.mode = TEXD_MODE(f, scan, out, corner, l, l);
		lv.mip = plan.levels - 1 - i;
	}
	return true;
}

/* Where in a twiddled picture 2^@lw by 2^@lh texel @t is: its bits are y's
 * and x's in turn, y's first, as long as both have any, then the rest of
 * the larger one's (detwiddle[][][]) */
static void tw_position(u32 t, u32 lw, u32 lh, u32 *x, u32 *y)
{
	const u32 n = lw < lh ? lw : lh;
	u32 i;
	u32 px = 0, py = 0;

	for (i = 0; i < n; i++)
	{
		py |= ((t >> (2 * i)) & 1) << i;
		px |= ((t >> (2 * i + 1)) & 1) << i;
	}
	if (lw > lh)
		px |= (t >> (2 * n)) << n;
	else
		py |= (t >> (2 * n)) << n;
	*x = px;
	*y = py;
}

/* The regions a texture's changed texels are in, up to TEXP_REGIONS of
 * them; full if there would have to be more */
struct part_list
{
	TexRegion *r;
	u32 count;
	bool full;
};

/* Texels @x0, @y0 to @x1, @y1 (past the last) of level @level, @t by @t
 * squares of a @width by @height level from a multiple of @t on, into
 * @list: joined to a region of the level they overlap, or that the two
 * fill most of together; one region more otherwise, or where there is no
 * room for one, joined to the region that grows the least. The regions
 * never overlap. */
static void part_add(part_list *list, u32 level, u32 t, u32 width, u32 height, u32 x0, u32 y0, u32 x1, u32 y1)
{
	TexRegion r;
	u32 i;

	r.level = level;
	r.x = x0 & ~(t - 1);
	r.y = y0 & ~(t - 1);
	x1 = (x1 + t - 1) & ~(t - 1);
	y1 = (y1 + t - 1) & ~(t - 1);
	r.w = (x1 < width ? x1 : width) - r.x;
	r.h = (y1 < height ? y1 : height) - r.y;
	for (;;)
	{
		u32 best = list->count;
		u32 grows = 0xFFFFFFFF;
		bool joined = false;

		for (i = 0; i < list->count && !joined; i++)
		{
			const TexRegion *o = &list->r[i];
			const u32 ux0 = o->x < r.x ? o->x : r.x, uy0 = o->y < r.y ? o->y : r.y;
			const u32 ux1 = o->x + o->w > r.x + r.w ? o->x + o->w : r.x + r.w;
			const u32 uy1 = o->y + o->h > r.y + r.h ? o->y + o->h : r.y + r.h;
			const u32 both = o->w * o->h + r.w * r.h;
			const u32 u = (ux1 - ux0) * (uy1 - uy0);
			const bool overlap = o->x < r.x + r.w && r.x < o->x + o->w && o->y < r.y + r.h && r.y < o->y + o->h;

			if (o->level != level)
				continue;
			if (overlap || u <= both + both / 8)
			{
				r.x = ux0;
				r.y = uy0;
				r.w = ux1 - ux0;
				r.h = uy1 - uy0;
				list->r[i] = list->r[--list->count];
				joined = true;
			}
			else if (u - both < grows)
			{
				grows = u - both;
				best = i;
			}
		}
		if (joined)
			continue;	/* (and what it is now may join another) */
		if (list->count < TEXP_REGIONS)
		{
			list->r[list->count++] = r;
			return;
		}
		if (best == list->count)
		{
			list->full = true;
			return;
		}
		{
			const TexRegion *o = &list->r[best];
			const u32 ux0 = o->x < r.x ? o->x : r.x, uy0 = o->y < r.y ? o->y : r.y;
			const u32 ux1 = o->x + o->w > r.x + r.w ? o->x + o->w : r.x + r.w;
			const u32 uy1 = o->y + o->h > r.y + r.h ? o->y + o->h : r.y + r.h;
			r.x = ux0;
			r.y = uy0;
			r.w = ux1 - ux0;
			r.h = uy1 - uy0;
			list->r[best] = list->r[--list->count];
		}
	}
}

/* Texels @t0 to @t1 of a twiddled level 2^@lw by 2^@lh, into @list: as
 * runs of a power of two on a multiple of it, each a rectangle */
static void part_add_twiddled(part_list *list, u32 level, u32 t0, u32 t1, u32 lw, u32 lh)
{
	const u32 n = lw < lh ? lw : lh;
	const u32 t = n < 4 ? 8 : 16;

	while (t0 < t1 && !list->full)
	{
		u32 k = 0;
		u32 x, y, bw, bh;

		while (k < lw + lh && (t0 & ((2u << k) - 1)) == 0 && t0 + (2u << k) <= t1)
			k++;
		tw_position(t0, lw, lh, &x, &y);
		if (k <= 2 * n)
		{
			bw = 1u << (k / 2);
			bh = 1u << ((k + 1) / 2);
		}
		else if (lw > lh)
		{
			bw = 1u << (k - n);
			bh = 1u << n;
		}
		else
		{
			bw = 1u << n;
			bh = 1u << (k - n);
		}
		part_add(list, level, t, 1u << lw, 1u << lh, x, y, x + bw, y + bh);
		t0 += 1u << k;
	}
}

/* Texels a converter makes of a block: 8 bytes of a twiddled texture, an
 * index of a compressed one */
static u32 block_texels(u32 bpp)
{
	return bpp == 4 ? 16 : bpp == 8 ? 8 : 4;
}

u32 BaseTextureCacheData::ChangedRegions(TexRegion *regions, u32 stride, u32 rows, bool mipmapped)
{
	const bool planar = tcw.ScanOrder && (tex->PL || tex->PL32);
	const u32 levels = mipmapped ? tsp.TexU + 3 + 1 : 1;
	const u32 start = sa_tex & VRAM_MASK;
	const u32 B = block_texels(tex->bpp);
	const u32 *bits;
	part_list list;
	u32 end = sa + size;
	u32 i, l, texels, all;

	if (piece_hash == NULL || pieces == 0 || levels > TEXD_MAX_LEVELS || tex->bpp == 0)
		return 0;
	if (end > VRAM_SIZE)
		end = VRAM_SIZE;
	bits = piece_hash + pieces;
	list.r = regions;
	list.count = 0;
	list.full = false;

	for (i = 0; i < pieces && !list.full; )
	{
		u32 j = i;
		u32 b0, b1;

		if ((bits[i >> 5] & (1u << (i & 31))) == 0)
		{
			i++;
			continue;
		}
		while (j < pieces && (bits[j >> 5] & (1u << (j & 31))) != 0)
			j++;
		b0 = (piece_first + i) * TEX_HASH_PIECE;
		b1 = (piece_first + j) * TEX_HASH_PIECE;
		if (b0 < start)
			b0 = start;
		if (b1 > end)
			b1 = end;
		i = j;
		/* the codebook is all of a compressed texture */
		if (tcw.VQ_Comp && b0 < sa_tex + 256 * 4 * 2)
			return 0;
		if (planar)
		{
			/* rows of the texture's width, read 'stride' texels apart */
			const u32 row = tcw.VQ_Comp ? stride / 4 : stride * 2;
			const u32 data = tcw.VQ_Comp ? sa + 256 * 4 * 2 : sa;
			const u32 data_end = data + row * rows;
			u32 r0 = b0 > data ? b0 - data : 0;
			u32 r1 = (b1 < data_end ? b1 : data_end) - data;

			if (b1 <= data || r0 >= r1)
				continue;
			part_add(&list, 0, 8, w, h, 0, r0 / row, w, (r1 + row - 1) / row);
			continue;
		}
		for (l = 0; l < levels; l++)
		{
			/* the level's data where its converter reads it (a codebook index for each block) */
			const u32 lw = mipmapped ? l : bitscanrev(w);
			const u32 lh = mipmapped ? l : bitscanrev(h);
			const u32 n = (1u << lw) << lh;
			u32 data, data_end, r0, r1;

			if (tcw.VQ_Comp)
			{
				data = (mipmapped ? sa_tex + VQMipPoint[l] : sa) + 256 * 4 * 2;
				data_end = data + (n + B - 1) / B;
			}
			else
			{
				data = mipmapped ? sa_tex + OtherMipPoint[l] * tex->bpp / 8 : sa;
				data_end = data + (n * tex->bpp + 7) / 8;
			}
			if (b1 <= data || b0 >= data_end)
				continue;
			/* the smallest levels (and their corners and special cases)
			 * are made whole with the texture */
			if (lw < 3 || lh < 3)
				return 0;
			r0 = b0 > data ? b0 - data : 0;
			r1 = (b1 < data_end ? b1 : data_end) - data;
			if (tcw.VQ_Comp)
				part_add_twiddled(&list, levels - 1 - l, r0 * B, r1 * B < n ? r1 * B : n, lw, lh);
			else
				part_add_twiddled(&list, levels - 1 - l, r0 / 8 * B, (r1 + 7) / 8 * B < n ? (r1 + 7) / 8 * B : n, lw, lh);
		}
	}
	if (list.full)
		return 0;

	/* half of it or more: made whole */
	texels = 0;
	for (i = 0; i < list.count; i++)
		texels += regions[i].w * regions[i].h;
	all = 0;
	for (l = 0; l < levels; l++)
		all += mipmapped ? 1u << (2 * l) : w * h;
	if (texels * 2 >= all)
		return 0;
	return list.count;
}

bool tex_plan_part(const texd_plan *plan, const TexRegion *regions, u32 count, TexPartPlan *part)
{
	u32 at, i, l;

	if (count > TEXP_REGIONS)
		return false;
	part->codebook = ((plan->level[0].mode >> 4) & 1) != 0 ? 256 * 4 * 2 : 0;
	at = part->codebook;
	for (i = 0; i < count; i++)
	{
		const TexRegion *r = &regions[i];
		const texd_level *lv = NULL;
		u32 mode, fmt, scan, lw, lh, first, last;

		for (l = 0; l < plan->levels; l++)
			if (plan->level[l].mip == r->level)
				lv = &plan->level[l];
		if (lv == NULL || r->w == 0 || r->h == 0)
			return false;
		part->level[i] = (u32)(lv - plan->level);
		mode = lv->mode;
		fmt = mode & 15;
		scan = (mode >> 4) & 3;
		lw = (mode >> 12) & 15;
		lh = (mode >> 16) & 15;
		/* the bytes the texels of the region are made from: texd_texel() */
		if (scan == TEXD_PL || scan == TEXD_PLVQ)
		{
			const u32 stride = lv->sr & 0xFFFF;
			const u32 rows = lv->sr >> 16;
			const u32 row = scan == TEXD_PL ? stride * 2 : stride / 4;
			const u32 y1 = r->y + r->h < rows ? r->y + r->h : rows;

			first = lv->data + r->y * row;
			last = r->y < y1 ? lv->data + y1 * row : first;
		}
		else
		{
			const u32 div = fmt == TEXD_PAL4 ? 16 : fmt == TEXD_PAL8 ? 8 : 4;
			/* (the first texel of a box of a twiddled picture is first in it, the last last) */
			const u32 t0 = texd_twiddle(r->x, r->y, lw, lh);
			const u32 t1 = texd_twiddle(r->x + r->w - 1, r->y + r->h - 1, lw, lh);

			if (scan == TEXD_VQ)
			{
				first = lv->data + t0 / div;
				last = lv->data + t1 / div + 1;
			}
			else
			{
				first = lv->data + t0 / div * 8;
				last = lv->data + (t1 / div + 1) * 8;
			}
		}
		first &= ~3u;
		if (last > plan->bytes)
			last = plan->bytes;
		if (last < first)
			last = first;
		part->from[i] = first;
		part->bytes[i] = last - first;
		part->at[i] = at;
		part->data[i] = lv->data + at - first;
		at += (part->bytes[i] + 3) & ~3u;
	}
	part->pal = at;
	part->total = at + plan->pal_entries * 4;
	return true;
}

/* The converter of @out (TEXD_OUT8, 16 or 32) over @width by @height
 * texels from @src, into @dst in rows of @pitch texels */
static void part_convert(const BaseTextureCacheData *t, u32 out, void *dst, u32 pitch, u8 *src, u32 width, u32 height)
{
	if (out == TEXD_OUT32)
	{
		PixelBuffer<u32> pb;
		pb.init(pitch, height, false, dst);
		t->texconv32(&pb, src, width, height);
	}
	else if (out == TEXD_OUT8)
	{
		PixelBuffer<u8> pb;
		pb.init(pitch, height, false, dst);
		t->texconv8(&pb, src, width, height);
	}
	else
	{
		PixelBuffer<u16> pb;
		pb.init(pitch, height, false, dst);
		t->texconv(&pb, src, width, height);
	}
}

/* The regions the changed pieces are in made again, the rest of the image
 * left as it is: on the GPU, as a whole update would be made there, or
 * converted here. False if the texture is to be made whole. */
bool BaseTextureCacheData::UpdatePart(u32 stride, u32 rows, bool mipmapped, bool need_32bit_buffer)
{
	const bool planar = tcw.ScanOrder && (tex->PL || tex->PL32);
	const TextureType before = tex_type;
	TexRegion regions[TEXP_REGIONS];
	size_t offset[TEXP_REGIONS];
	size_t bytes = 0;
	TextureType type = tex_type;
	texd_plan plan;
	void *memory = NULL;
	u32 count, out, pixel, i;

	if (made_type == 0 || made_mips != mipmapped)
		return false;
	/* the converter a whole update takes, and the texture type it makes */
	if (texconv32 != NULL && need_32bit_buffer)
	{
		out = TEXD_OUT32;
		type = TextureType::_8888;
	}
	else if (texconv8 != NULL && tex_type == TextureType::_8)
		out = TEXD_OUT8;
	else if (texconv != NULL)
		out = TEXD_OUT16;
	else
		return false;
	if ((u32)type + 1 != made_type)
		return false;
	count = ChangedRegions(regions, stride, rows, mipmapped);
	if (count == 0)
		return false;

	tex_type = type;
	if (PlanDecode(plan, stride, rows, out, mipmapped) && (settings.rend.GpuTextureDecode || plan.native))
	{
		const int done = GpuDecodePart(plan, regions, count);
		if (done == TEXP_DONE)
			return true;
		if (done == TEXP_WHOLE)
		{
			tex_type = before;
			return false;
		}
	}

	pixel = out == TEXD_OUT32 ? 4 : out == TEXD_OUT8 ? 1 : 2;
	for (i = 0; i < count; i++)
	{
		offset[i] = bytes;
		bytes += ((size_t)regions[i].w * regions[i].h * pixel + 15) & ~(size_t)15;
	}
	/* (where a stride wider than the texture runs on past the last row) */
	if (planar && stride > w)
		bytes += (size_t)(stride - w) * pixel;
	if (!PartMemory(bytes, &memory))
	{
		tex_type = before;
		return false;
	}
	if (memory == NULL)
		memory = texcache_scratch(bytes);

	for (i = 0; i < count; i++)
	{
		const TexRegion *r = &regions[i];
		u8 * const dst = (u8 *)memory + offset[i];

		if (planar)
		{
			const u32 made = r->y >= rows ? 0 : r->y + r->h > rows ? rows - r->y : r->h;
			u32 y;

			if (made != 0)
				part_convert(this, out, dst, w, &vram[sa + r->y * (tcw.VQ_Comp ? stride / 4 : stride * 2)], stride, made);
			if (made < r->h)
				memset(dst + (size_t)made * w * pixel, 0, (size_t)(r->h - made) * w * pixel);
			if (stride < w)
				for (y = 0; y < made; y++)
					memset(dst + ((size_t)y * w + stride) * pixel, 0, (size_t)(w - stride) * pixel);
		}
		else
		{
			/* in the squares the texture is laid out in, each one a
			 * twiddled picture of its own from where the square starts */
			const u32 l = mipmapped ? tsp.TexU + 3 - r->level : 0;
			const u32 lw = mipmapped ? l : bitscanrev(w);
			const u32 lh = mipmapped ? l : bitscanrev(h);
			const u32 n = lw < lh ? lw : lh;
			const u32 t = n < 4 ? 8 : 16;
			const u32 B = block_texels(tex->bpp);
			u32 data, x, y;

			if (tcw.VQ_Comp)
				data = mipmapped ? sa_tex + VQMipPoint[l] : sa;
			else
				data = mipmapped ? sa_tex + OtherMipPoint[l] * tex->bpp / 8 : sa;
			for (y = r->y; y < r->y + r->h; y += t)
				for (x = r->x; x < r->x + r->w; x += t)
				{
					const u32 at = detwiddle[0][lh][x] + detwiddle[1][lw][y];
					part_convert(this, out, dst + ((size_t)(y - r->y) * r->w + (x - r->x)) * pixel, r->w,
							&vram[data + (tcw.VQ_Comp ? at / B : at / B * 8)], t, t);
				}
		}
	}
	UploadPart(regions, count, (u8 *)memory, offset);
	return true;
}

void BaseTextureCacheData::Update()
{
	/* only what NeedsUpdate() just hashed is known to be the rest of it */
	const bool part = changed_pieces;
	bool watched = false;
	u32 changed;

	changed_pieces = false;
	//texture state tracking stuff
	Updates++;
	dirty=0;

	tex_type = tex->type;

	bool has_alpha = false;
	if (IsPaletted())
	{
		if (IsGpuHandledPaletted(tsp, tcw, area))
			tex_type = TextureType::_8;
		else
		{
			tex_type = PAL_TYPE[PAL_RAM_CTRL&3];
			if (tex_type != TextureType::_565)
				has_alpha = true;
		}

		// Get the palette hash to check for future updates
		if (tcw.PixelFmt == PixelPal4)
			palette_hash = pal_hash_16[tcw.PalSelect];
		else
			palette_hash = pal_hash_256[tcw.PalSelect >> 4];
	}

	::palette_index = this->palette_index; // might be used if pal. tex
	::vq_codebook = &vram[vq_codebook];    // might be used if VQ tex

	//texture conversion work
	u32 stride = w;

	if (tcw.ScanOrder && (tex->PL || tex->PL32))
	{
		u32 bytes;

		if (tcw.StrideSel)
		{
			stride = (TEXT_CONTROL & 31) * 32;
			if (stride == 0)
				stride = w;
		}
		/* What is watched and hashed is what this stride reads, which the
		 * stride the texture was made or last read with need not be. */
		bytes = tcw.VQ_Comp ? 256 * 4 * 2 + stride * h / 4 : stride * h * tex->bpp / 8;
		if (bytes != size && lock_block != nullptr)
		{
			libCore_vramlock_Unlock_block_wb(lock_block);
			lock_block = nullptr;
		}
		size = bytes;
		stride_read = stride;
	}

	/* How many of the texture's lines there are to read. All of them, but
	 * for a texture that runs past the end of video memory: Shenmue's Space
	 * Harrier mini-arcade loads one, and only uses the top of it.
	 *
	 * That was done by cutting the texture's height and size down for the
	 * update and putting the height back after. The size stayed cut - it is
	 * what is locked and hashed, and has to end inside video memory - so the
	 * second update found nothing running past the end, cut nothing, and
	 * read all the lines, the ones outside video memory included. And the
	 * first update converted into a buffer made for the cut height, then
	 * uploaded the whole. (From upstream, done here by what the texture
	 * would take whole rather than by the size as it stands.) */
	u32 rows = h;
	const bool cut_short = tcw.ScanOrder && stride > 0 && !tcw.VQ_Comp && tex->bpp != 0;
	const u32 whole = cut_short ? stride * h * tex->bpp / 8 : size;
	if (sa_tex > VRAM_SIZE || size == 0 || sa + (whole > size ? whole : size) > VRAM_SIZE)
	{
		if (sa < VRAM_SIZE && cut_short)
		{
			rows = (VRAM_SIZE - sa) * 8 / stride / tex->bpp;
			size = stride * rows * tex->bpp/8;
		}
		else
		{
			WARN_LOG(RENDERER, "Warning: invalid texture. Address %08X %08X size %d", sa_tex, sa, size);
			return;
		}
	}
	if (settings.rend.CustomTextures)
		custom_texture.LoadCustomTextureAsync(this);

	/* Watched by its hashes from now on: of what is converted, before it
	 * is (NeedsUpdate()'s, if it has just hashed it) */
	if (Updates >= FREQUENT_UPDATE_COUNT)
	{
		watched = part || HashPieces(&changed);
		hash_frame = FrameCount;
		hash_epoch = vram_write_epoch;
	}

	void *temp_tex_buffer = NULL;
	bool converted = true;
	u32 upscaled_w = w;
	u32 upscaled_h = h;

	PixelBuffer<u16> pb16;
	PixelBuffer<u32> pb32;
	PixelBuffer<u8> pb8;

	/* A planar texture is converted a row of 'stride' pixels at a time into
	 * rows of the texture's width. With a stride wider than the texture,
	 * every row runs on into the next, which is then written over it - and
	 * the last runs past the end of the buffer by the difference: room is
	 * left for that. With a narrower one the rest of each row was whatever
	 * the memory held, and went to the GPU so; it is cleared. */
	const u32 stride_over = stride > w ? stride - w : 0;

	// Figure out if we really need to use a 32-bit pixel buffer
	bool textureUpscaling = settings.rend.TextureUpscale > 1
			// Don't process textures that are too big
			&& (int)(w * h) <= settings.rend.MaxFilteredTextureSize * settings.rend.MaxFilteredTextureSize
			// Don't process YUV textures
			&& tcw.PixelFmt != PixelYUV;
	bool need_32bit_buffer = true;
	if (!textureUpscaling
		&& (!IsPaletted() || tex_type != TextureType::_8888)
		&& texconv != NULL
		&& !Force32BitTexture(tex_type))
		need_32bit_buffer = false;
	// TODO avoid upscaling/depost. textures that change too often

	bool mipmapped = IsMipmapped() && !settings.rend.DumpTextures;
	/* Straight into the renderer's memory, if it gives any: not a texture
	 * that is scaled up, which is converted and then scaled into memory of
	 * the scaler's own; nor when textures are dumped, which reads them back. */
	const bool direct = !textureUpscaling && !settings.rend.DumpTextures;

	/* Only the pieces that changed made again, where they can be */
	if (part && direct && !settings.rend.CustomTextures && UpdatePart(stride, rows, mipmapped, need_32bit_buffer))
	{
		PrintTextureName();
		return;
	}
	made_type = 0;

	/* Or made from its bytes on the GPU, where the renderer can: with the
	 * converter, the format and the levels it would have here. Not a
	 * texture scaled up or dumped, which need its texels on this side. A
	 * planar 16-bit texture whose texels the renderer takes as they are
	 * (plan.native) is offered whether the GPU makes textures or not: it
	 * is a copy, which converting it here would only make a slower one. */
	if (direct)
	{
		u32 out = 0xFF;
		TextureType type = tex_type;
		if (texconv32 != NULL && need_32bit_buffer)
		{
			out = TEXD_OUT32;
			type = TextureType::_8888;
		}
		else if (texconv8 != NULL && tex_type == TextureType::_8)
			out = TEXD_OUT8;
		else if (texconv != NULL)
			out = TEXD_OUT16;
		texd_plan plan;
		if (out != 0xFF && PlanDecode(plan, stride, rows, out, mipmapped) && (settings.rend.GpuTextureDecode || plan.native))
		{
			const TextureType before = tex_type;
			tex_type = type;
			made_type = (u8)type + 1;
			made_mips = plan.mipmapped != 0;
			if (GpuDecode(plan))
			{
				if (!watched)
					libCore_vramlock_Lock(sa_tex, sa + size - 1, this);
				PrintTextureName();
				return;
			}
			made_type = 0;
			tex_type = before;
		}
	}

	if (texconv32 != NULL && need_32bit_buffer)
	{
		if (textureUpscaling)
			// don't use mipmaps if upscaling
			mipmapped = false;
		// Force the texture type since that's the only 32-bit one we know
		tex_type = TextureType::_8888;

		if (mipmapped)
		{
			pb32.init(w, h, true, ConvertInto(w, h, PixelBuffer<u32>::bytes(w, h, true), 4, true, direct));
			for (u32 i = 0; i <= tsp.TexU + 3u; i++)
			{
				pb32.set_mipmap(i);
				u32 vram_addr;
				if (tcw.VQ_Comp)
				{
					vram_addr = sa_tex + VQMipPoint[i];
					if (i == 0)
					{
						/* (room for what a paletted texture's converter
						 * writes: a block of 4x4 texels, rows 2 apart) */
						u32 corner[16];
						PixelBuffer<u32> pb0;
						pb0.init(2, 2, false, corner);
						/* The 1x1 level is the corner of a 2x2 block. For a
						 * YUV texture that block is not a colour - it came
						 * out magenta, behind the pitch in World Series
						 * Baseball 2K2 - and the 2x2 level's is used. */
						if (tcw.PixelFmt == PixelYUV)
							vram_addr = sa_tex + VQMipPoint[1];
						texconv32(&pb0, (u8*)&vram[vram_addr], 2, 2);
						*pb32.data() = *pb0.data(1, 1);
						continue;
					}
				}
				else
					vram_addr = sa_tex + OtherMipPoint[i] * tex->bpp / 8;
				if (tcw.PixelFmt == PixelYUV && i == 0)
					// Special case for YUV at 1x1 LoD
					format[Pixel565].TW32(&pb32, &vram[vram_addr], 1, 1);
				else
					texconv32(&pb32, &vram[vram_addr], 1 << i, 1 << i);
			}
			pb32.set_mipmap(0);
		}
		else
		{
			pb32.init(w, h, false, ConvertInto(w, h, PixelBuffer<u32>::bytes(w, h, false) + stride_over * sizeof(u32), 4, false, direct));
			texconv32(&pb32, (u8*)&vram[sa], stride, rows);
			if (rows < h)
				memset((u32 *)pb32.data() + rows * w, 0, (size_t)(h - rows) * w * sizeof(u32));
			if (stride < w)
				for (u32 y = 0; y < h; y++)
					memset(pb32.data(stride, y), 0, (w - stride) * sizeof(u32));

#ifdef HAVE_TEXUPSCALE
			// xBRZ scaling
			if (textureUpscaling)
			{
				PixelBuffer<u32> tmp_buf;
				tmp_buf.init(w * settings.rend.TextureUpscale, h * settings.rend.TextureUpscale);

				if (tcw.PixelFmt == Pixel1555 || tcw.PixelFmt == Pixel4444)
					// Alpha channel formats. Palettes with alpha are already handled
					has_alpha = true;
				UpscalexBRZ(settings.rend.TextureUpscale, pb32.data(), tmp_buf.data(), w, h, has_alpha);
				pb32.steal_data(tmp_buf);
				upscaled_w *= settings.rend.TextureUpscale;
				upscaled_h *= settings.rend.TextureUpscale;
			}
#endif
		}
		temp_tex_buffer = pb32.data();
	}
	else if (texconv8 != NULL && tex_type == TextureType::_8)
	{
		if (mipmapped)
		{
			pb8.init(w, h, true, ConvertInto(w, h, PixelBuffer<u8>::bytes(w, h, true), 1, true, direct));
			for (u32 i = 0; i <= tsp.TexU + 3u; i++)
			{
				pb8.set_mipmap(i);
				u32 vram_addr = sa_tex + OtherMipPoint[i] * tex->bpp / 8;
				texconv8(&pb8, &vram[vram_addr], 1 << i, 1 << i);
			}
			pb8.set_mipmap(0);
		}
		else
		{
			pb8.init(w, h, false, ConvertInto(w, h, PixelBuffer<u8>::bytes(w, h, false), 1, false, direct));
			texconv8(&pb8, &vram[sa], stride, rows);
			if (rows < h)
				memset((u8 *)pb8.data() + rows * w, 0, (size_t)(h - rows) * w);
		}
		temp_tex_buffer = pb8.data();
	}
	else if (texconv != NULL)
	{
		if (mipmapped)
		{
			pb16.init(w, h, true, ConvertInto(w, h, PixelBuffer<u16>::bytes(w, h, true), 2, true, direct));
			for (u32 i = 0; i <= tsp.TexU + 3u; i++)
			{
				pb16.set_mipmap(i);
				u32 vram_addr;
				if (tcw.VQ_Comp)
				{
					vram_addr = sa_tex + VQMipPoint[i];
					if (i == 0)
					{
						u16 corner[16];	// (as above)
						PixelBuffer<u16> pb0;
						pb0.init(2, 2, false, corner);
						texconv(&pb0, (u8*)&vram[vram_addr], 2, 2);
						*pb16.data() = *pb0.data(1, 1);
						continue;
					}
				}
				else
					vram_addr = sa_tex + OtherMipPoint[i] * tex->bpp / 8;
				texconv(&pb16, (u8*)&vram[vram_addr], 1 << i, 1 << i);
			}
			pb16.set_mipmap(0);
		}
		else
		{
			pb16.init(w, h, false, ConvertInto(w, h, PixelBuffer<u16>::bytes(w, h, false) + stride_over * sizeof(u16), 2, false, direct));
			texconv(&pb16,(u8*)&vram[sa],stride,rows);
			if (rows < h)
				memset((u16 *)pb16.data() + rows * w, 0, (size_t)(h - rows) * w * sizeof(u16));
			if (stride < w)
				for (u32 y = 0; y < h; y++)
					memset(pb16.data(stride, y), 0, (w - stride) * sizeof(u16));
		}
		temp_tex_buffer = pb16.data();
	}
	else
	{
		//fill it in with a temp color
		WARN_LOG(RENDERER, "UNHANDLED TEXTURE");
		converted = false;
		pb16.init(w, h, false, texcache_scratch(PixelBuffer<u16>::bytes(w, h, false)));
		memset(pb16.data(), 0x80, w * h * 2);
		temp_tex_buffer = pb16.data();
		mipmapped = false;
	}
	//lock the texture to detect changes in it, unless it is frequently
	//updated: each update of a protected texture costs a SIGSEGV (the write
	//fault), the invalidation, a full re-upload, and a re-protect -- measured
	//as 1-2 faults per frame sustained, with matching frame-time jitter, on
	//titles that rewrite textures every frame (e.g. VF3tb). Past the update
	//threshold, leave the pages writable and detect changes with a content
	//hash at lookup time instead (see NeedsUpdate): a hash over the same range
	//is cheaper than the fault path, and when the game rewrites identical
	//content it avoids the re-upload entirely.
	if (!watched)
		libCore_vramlock_Lock(sa_tex, sa + size - 1, this);

	/* (an image a part of is made again: not scaled up, not grey) */
	if (direct && converted)
	{
		made_type = (u8)tex_type + 1;
		made_mips = mipmapped;
	}
	UploadToGPU(upscaled_w, upscaled_h, (u8*)temp_tex_buffer, IsMipmapped(), mipmapped);
	if (settings.rend.DumpTextures)
	{
		ComputeHash();
		custom_texture.DumpTexture(texture_hash, upscaled_w, upscaled_h, tex_type, temp_tex_buffer);
		NOTICE_LOG(RENDERER, "Dumped texture %x.png. Old hash %x", texture_hash, old_texture_hash);
	}
	PrintTextureName();
}

void BaseTextureCacheData::CheckCustomTexture()
{
	if (IsCustomTextureAvailable())
	{
		made_type = 0;
		tex_type = TextureType::_8888;
		UploadToGPU(custom_width, custom_height, custom_image_data, IsMipmapped(), false);
		free(custom_image_data);
		custom_image_data = NULL;
	}
}

/* Where the framebuffer is, its format and size, as the registers have it */
void ReadFramebufferGeometry(FramebufferRead *fb)
{
	int width = (FB_R_SIZE.fb_x_size + 1) << 1;     // in 16-bit words
	int height = FB_R_SIZE.fb_y_size + 1;
	int modulus = (FB_R_SIZE.fb_modulus - 1) << 1;
	int bpp;
	u32 addr;

	switch (FB_R_CTRL.fb_depth)
	{
		case FBDE_0555:
		case FBDE_565:
			bpp = 2;
			break;
		case FBDE_888:
			bpp = 3;
			width = (width * 2) / 3;		// in pixels
			modulus = (modulus * 2) / 3;	// in pixels
			break;
		case FBDE_C888:
			bpp = 4;
			width /= 2;             // in pixels
			modulus /= 2;           // in pixels
			break;
		default:
			die("Invalid framebuffer format\n");
			bpp = 4;
			break;
	}

	addr = FB_R_SOF1;
	if (SPG_CONTROL.interlace)
	{
		if (width == modulus && FB_R_SOF2 == FB_R_SOF1 + width * bpp)
		{
			// Typical case alternating even and odd lines -> take the whole buffer at once
			modulus = 0;
			height *= 2;
		}
		else
		{
			addr = SPG_STATUS.fieldnum ? FB_R_SOF2 : FB_R_SOF1;
		}
	}
	else if (FB_R_CTRL.vclk_div == 0)
	{
		/* Not interlaced, on a television: 240 lines are all there are,
		 * whatever the size register says. Upstream: the Echelon intro of
		 * Confidential Mission (PAL). */
		height = std::min(height, 240);
	}

	fb->addr = addr;
	fb->width = width;
	fb->height = height;
	fb->skip = modulus * bpp;
	fb->depth = FB_R_CTRL.fb_depth;
	fb->concat = FB_R_CTRL.fb_concat;
}

#define FB_RGBX(r, g, b) ((u32)(u8)(r) | ((u32)(u8)(g) << 8) | ((u32)(u8)(b) << 16) | 0xFF000000u)

/* The picture as RGBA8, rows @pitch bytes apart from @dst on: each texel
 * stored once, whole and in order, so @dst may be memory the card reads
 * (write-combined, never read back) */
void ReadFramebufferTo(const FramebufferRead *fb, u8 *dst, size_t pitch)
{
	const int width = fb->width;
	const int height = fb->height;
	const u32 skip = fb->skip;
	const u32 c = fb->concat;
	u32 addr = fb->addr;
	int x;
	int y;

	switch (fb->depth)
	{
		case FBDE_0555:    // 555 RGB
			for (y = 0; y < height; y++, dst += pitch)
			{
				u32 *out = (u32 *)dst;
				for (x = 0; x < width; x++, addr += 2)
				{
					u32 src = pvr_read_area1<u16>(addr);
					out[x] = FB_RGBX((((src >> 10) & 0x1F) << 3) + c,
							(((src >> 5) & 0x1F) << 3) + c,
							((src & 0x1F) << 3) + c);
				}
				addr += skip;
			}
			break;

		case FBDE_565:    // 565 RGB
			for (y = 0; y < height; y++, dst += pitch)
			{
				u32 *out = (u32 *)dst;
				for (x = 0; x < width; x++, addr += 2)
				{
					u32 src = pvr_read_area1<u16>(addr);
					out[x] = FB_RGBX((((src >> 11) & 0x1F) << 3) + c,
							(((src >> 5) & 0x3F) << 2) + (c & 3),
							((src & 0x1F) << 3) + c);
				}
				addr += skip;
			}
			break;

		case FBDE_888:		// 888 RGB: four pixels in three words
			for (y = 0; y < height; y++, dst += pitch)
			{
				u32 *out = (u32 *)dst;
				for (x = 0; x < width; x += 4)
				{
					u32 src = pvr_read_area1<u32>(addr);
					u32 src2;
					u32 src3;
					addr += 4;
					*out++ = FB_RGBX(src >> 16, src >> 8, src);
					if (x + 1 >= width)
						break;
					src2 = pvr_read_area1<u32>(addr);
					addr += 4;
					*out++ = FB_RGBX(src2 >> 8, src2, src >> 24);
					if (x + 2 >= width)
						break;
					src3 = pvr_read_area1<u32>(addr);
					addr += 4;
					*out++ = FB_RGBX(src3, src2 >> 24, src2 >> 16);
					if (x + 3 >= width)
						break;
					*out++ = FB_RGBX(src3 >> 24, src3 >> 16, src3 >> 8);
				}
				addr += skip;
			}
			break;

		case FBDE_C888:     // 0888 RGB
			for (y = 0; y < height; y++, dst += pitch)
			{
				u32 *out = (u32 *)dst;
				for (x = 0; x < width; x++, addr += 4)
				{
					u32 src = pvr_read_area1<u32>(addr);
					out[x] = FB_RGBX(src >> 16, src >> 8, src);
				}
				addr += skip;
			}
			break;
	}
}

#undef FB_RGBX

/* For a renderer with nowhere of its own to convert to: where textures are converted */
void ReadFramebuffer(PixelBuffer<u32>& pb, int& width, int& height)
{
	FramebufferRead fb;
	ReadFramebufferGeometry(&fb);
	width = fb.width;
	height = fb.height;
	pb.init(width, height, false, texcache_scratch(PixelBuffer<u32>::bytes(width, height, false)));
	ReadFramebufferTo(&fb, (u8 *)pb.data(), (size_t)width * 4);
}

static void rend_text_invl(vram_block* bl)
{
	BaseTextureCacheData* tcd = (BaseTextureCacheData*)bl->userdata;
	tcd->dirty = FrameCount;
	tcd->lock_block = nullptr;

	libCore_vramlock_Unlock_block_wb(bl);
}
