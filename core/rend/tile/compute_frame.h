/* The compute renderer's frame (rend/tile/pvr_compute.h's PVRC_F_* words),
 * as both renderers that have it hand it to the kernels: Vulkan's
 * (vulkan/compute_drawer.h) and Direct3D 12's (dx12/dx12_compute.h). The
 * rules a frame's words are made by are the raster renderers': each
 * renderer gives its transform, its base scissor and its flat shading's
 * colours (compute_frame_renderer), which are the same in both. Also what
 * the tests build their frames with (tools/rend/tile_dump.cpp). C89; C++
 * has it as the ComputeFrame class too. */
#ifndef REND_TILE_COMPUTE_FRAME_H
#define REND_TILE_COMPUTE_FRAME_H
#include <retro_common_api.h>
#include <retro_inline.h>
#include "types.h"
#include "rend/xform.h"
#include "rend/tile/pvr_compute.h"
#ifdef __cplusplus
#include "rend/transform_matrix.h"
#endif

RETRO_BEGIN_DECLS

typedef struct compute_frame_layout
{
	u32 width, height;	/* the picture's */
	u32 tileLog2;		/* of the coarse tiles */
	u32 pool;		/* entries of the bin pool */
	u32 runs;		/* runs the bin buffer has room for */
	u32 chunks;		/* ...for this many chunks */
	bool lastPicture;	/* start from the picture before */
	bool lastScaled;	/* ...one of another size */
	u32 rttScale;		/* a render to a texture: Render To Texture Upscaling's (else 0) */
} compute_frame_layout;

/* The renderer's part of a frame */
typedef struct compute_frame_renderer
{
	void *self;
	/* a texture's slot (PVRC_TEX_SLOTS of them), or PVRC_NONE when they
	 * are all taken */
	u32 (*slot)(void *self, u64 texid, u32 tsp);
	/* its matrices made for the frame in pvrrc, the viewport's (pixels)
	 * returned */
	const xform *(*begin)(void *self);
	/* ...the screen's base scissor, in pixels (after begin) */
	void (*base_scissor)(void *self, int *x, int *y, int *width, int *height);
	/* ...the flat polygons' colours where it draws them from */
	void (*provoking_vertices)(void *self);
} compute_frame_renderer;

/* Why a frame is left to the raster renderer: compute_frame_pack()'s
 * reasons (after it returned 0), and the drawer's */
enum
{
	CF_WHY_NONE, CF_WHY_PASSES, CF_WHY_ROOM, CF_WHY_TEXTURES, CF_WHY_RTT,
	CF_WHY_OSD, CF_WHY_PICTURE, CF_WHY_DECODE, CF_WHY_COUNT
};

/* The words of the frame in pvrrc, in @out (room for @room); their count,
 * or 0 when the frame does not fit there, has more primitives or textures
 * than there is room for, or more render passes than the kernels take
 * (@why says which); @prims, the number of primitives. Flat polygons have
 * their first vertex's colours made those of their last, as the raster
 * renderers have them. */
u32 compute_frame_pack(const compute_frame_layout *layout, const compute_frame_renderer *r,
		u32 *out, u32 room, u32 *prims, u32 *why);

/* the most words a frame of this machine takes, and the most primitives */
u32 compute_frame_max_words(void);
u32 compute_frame_max_prims(void);
/* the coarse tiles' size for a picture this wide (log2) */
static INLINE u32 compute_frame_tile_log2(u32 width)
{
	return width > 1280 ? 6 : 5;
}
/* the most coarse tiles a texture rendered to has (at the most upscaling there is) */
u32 compute_frame_max_rtt_tiles(void);
/* Render To Texture Upscaling, as the renders to a texture have it */
u32 compute_frame_rtt_scale(void);
/* The chip's X and Y scalers (SCALER_CTL, Y_COEFF) left out of the frames
 * packed from now on: the core option reicast_pvr2_scaler */
extern bool compute_frame_scalers_off;
/* The chip's write-out for a picture @height lines high of the frame the
 * registers have now (not a render to a texture): the Y filter and the
 * framebuffer's packing and dither, as PVRC_F_FLAGS's bits (the walk's
 * PVRC_K_YFILTER, which the raster renderers run too); 0 when there is
 * none or compute_frame_scalers_off */
u32 compute_frame_write_out(u32 height);

RETRO_END_DECLS

#ifdef __cplusplus
class ComputeFrame
{
public:
	struct Slots
	{
		/* a texture's slot, or PVRC_NONE when they are all taken */
		virtual u32 Slot(u64 texid, TSP tsp) = 0;
		virtual ~Slots() = default;
	};
	typedef compute_frame_layout Layout;
	virtual ~ComputeFrame() = default;
	/* compute_frame_pack() with this renderer's part */
	u32 Pack(const Layout& layout, Slots& slots, u32 *out, u32 room, u32 *prims)
	{
		Bound b = { this, &slots };
		compute_frame_renderer r;
		r.self = &b;
		r.slot = SlotOf;
		r.begin = BeginOf;
		r.base_scissor = BaseScissorOf;
		r.provoking_vertices = ProvokingVerticesOf;
		return compute_frame_pack(&layout, &r, out, room, prims, &why);
	}
	enum
	{
		Why_None = CF_WHY_NONE, Why_Passes = CF_WHY_PASSES, Why_Room = CF_WHY_ROOM,
		Why_Textures = CF_WHY_TEXTURES, Why_Rtt = CF_WHY_RTT, Why_Osd = CF_WHY_OSD,
		Why_Picture = CF_WHY_PICTURE, Why_Decode = CF_WHY_DECODE, Why_Count = CF_WHY_COUNT
	};
	u32 why = Why_None;

	static u32 MaxWords() { return compute_frame_max_words(); }
	static u32 MaxPrims() { return compute_frame_max_prims(); }
	static u32 TileLog2(u32 width) { return compute_frame_tile_log2(width); }
	static u32 MaxRttTiles() { return compute_frame_max_rtt_tiles(); }
	static u32 RttScale() { return compute_frame_rtt_scale(); }
	static u32 WriteOut(u32 height) { return compute_frame_write_out(height); }

protected:
	/* The renderer's: compute_frame_renderer's */
	virtual const xform& Begin() = 0;
	virtual void BaseScissor(int& x, int& y, int& width, int& height) = 0;
	virtual void ProvokingVertices() = 0;

private:
	struct Bound
	{
		ComputeFrame *frame;
		Slots *slots;
	};
	static u32 SlotOf(void *self, u64 texid, u32 tsp)
	{
		TSP t;
		t.full = tsp;
		return ((Bound *)self)->slots->Slot(texid, t);
	}
	static const xform *BeginOf(void *self)
	{
		return &((Bound *)self)->frame->Begin();
	}
	static void BaseScissorOf(void *self, int *x, int *y, int *width, int *height)
	{
		((Bound *)self)->frame->BaseScissor(*x, *y, *width, *height);
	}
	static void ProvokingVerticesOf(void *self)
	{
		((Bound *)self)->frame->ProvokingVertices();
	}
};
#endif

#endif
