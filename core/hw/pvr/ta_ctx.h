#ifndef TA_CTX_H
#define TA_CTX_H
/* A frame of tile accelerator data and what its parse makes of it. C89,
 * for C and C++ alike; C++ has the lists typed (List<T>, helper_classes.h)
 * and the functions below as members too, as it always had them. */
#include "ta.h"
#include "pvr_regs.h"
#include "ta_list.h"
#include "ta_index.h"
#include "../../rend/parse_arena.h"
#ifdef __cplusplus
#include "helper_classes.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A list of T's: List<T> in C++, its ta_list_t in C */
#ifdef __cplusplus
#define TA_LIST(T) List<T>
#else
#define TA_LIST(T) struct { ta_list_t l; }
#endif

/* Vertex storage types */
typedef struct Vertex Vertex;
struct Vertex
{
	float x,y,z;

	u8 col[4];
	u8 vtx_spc[4];

	float u,v;

	/* Two volumes format */
	u8 col1[4];
	u8 spc1[4];

	float u1,v1;
};

/* (in C the control words are their u32s: ta_structs.h's unions are C++) */
#ifdef __cplusplus
#define TA_WORD(T) T
#else
#define TA_WORD(T) u32
#endif

typedef struct PolyParam PolyParam;
struct PolyParam
{
	u32 first;		/* entry index , holds vertex/pos data */
	u32 count;

	u64 texid;

	TA_WORD(TSP) tsp;
	TA_WORD(TCW) tcw;
	TA_WORD(PCW) pcw;
	TA_WORD(ISP_TSP) isp;
	float zvZ;
	u32 tileclip;
	TA_WORD(TSP) tsp1;
	TA_WORD(TCW) tcw1;
	u64 texid1;
};

typedef struct ModifierVolumeParam ModifierVolumeParam;
struct ModifierVolumeParam
{
	u32 first;
	u32 count;
	TA_WORD(ISP_Modvol) isp;
	u32 tileclip;	/* the clipping rectangle and mode it was sent under, as a polygon's */
};

typedef struct ModTriangle ModTriangle;
struct ModTriangle
{
	f32 x0,y0,z0,x1,y1,z1,x2,y2,z2;
};

/* (tad_context::ol_base: not known) */
#define TAD_OL_UNKNOWN 0xFFFFFFFFu

typedef struct tad_context tad_context;
static INLINE void tad_forget_ol_bases(tad_context *t);
static INLINE void tad_clear(tad_context *t);
static INLINE void tad_clear_partial(tad_context *t);
static INLINE void tad_continue(tad_context *t);
static INLINE u8 *tad_end(const tad_context *t);
static INLINE void tad_reset(tad_context *t, u8 *ptr);

struct tad_context
{
	u8* thd_data;
	u8* thd_root;
	u8* thd_old_data;
	u8 *render_passes[10];
	u32 render_pass_count;
	/* Where each render pass's object lists were written (TA_OL_BASE when
	 * its lists were started, TAD_OL_UNKNOWN when that is not known: a frame
	 * from a save state). A render draws the passes whose lists its region
	 * array points to (ta_select_passes()). */
	u32 ol_base[10];

#ifdef __cplusplus
	void ForgetOlBases() { tad_forget_ol_bases(this); }
	void Clear() { tad_clear(this); }
	void ClearPartial() { tad_clear_partial(this); }
	void Continue() { tad_continue(this); }
	u8* End() const { return tad_end(this); }
	void Reset(u8* ptr) { tad_reset(this, ptr); }
#endif
};

static INLINE void tad_forget_ol_bases(tad_context *t)
{
	u32 i;
	for (i = 0; i < sizeof(t->ol_base) / sizeof(t->ol_base[0]); i++)
		t->ol_base[i] = TAD_OL_UNKNOWN;
}

static INLINE void tad_clear(tad_context *t)
{
	t->thd_old_data = t->thd_data = t->thd_root;
	t->render_pass_count = 0;
	tad_forget_ol_bases(t);
}

static INLINE void tad_clear_partial(tad_context *t)
{
	t->thd_old_data = t->thd_data;
	t->thd_data = t->thd_root;
}

static INLINE u8 *tad_end(const tad_context *t)
{
	return t->thd_data == t->thd_root ? t->thd_old_data : t->thd_data;
}

static INLINE void tad_continue(tad_context *t)
{
	t->render_passes[t->render_pass_count] = tad_end(t);
	if (t->render_pass_count < sizeof(t->render_passes) / sizeof(u8*) - 1)
		t->render_pass_count++;
}

static INLINE void tad_reset(tad_context *t, u8 *ptr)
{
	t->thd_data = t->thd_root = t->thd_old_data = ptr;
	t->render_pass_count = 0;
	tad_forget_ol_bases(t);
}

typedef struct RenderPass RenderPass;
struct RenderPass {
	bool autosort;
	bool z_clear;
	bool mv_op_tr_shared;	/* the translucent modifier volumes are the opaque ones */
	u32 op_count;
	u32 mvo_count;
	u32 pt_count;
	u32 tr_count;
	u32 mvo_tr_count;
};

typedef struct rend_context rend_context;
void rend_context_clear(rend_context *r);

struct rend_context
{
	u8* proc_start;
	u8* proc_end;

	f32 fZ_min;
	f32 fZ_max;

	bool Overrun;
	bool isRTT;
	bool clearFramebuffer;	/* drawn to a framebuffer the game has not drawn to lately: see rend_start_render() */

	bool isRenderFramebuffer;

	FB_X_CLIP_type    fb_X_CLIP;
	FB_Y_CLIP_type    fb_Y_CLIP;

	u32 fog_clamp_min;
	u32 fog_clamp_max;

	TA_LIST(Vertex)      verts;
	TA_LIST(u32)         idx;
	TA_LIST(ModTriangle) modtrig;
	TA_LIST(ModifierVolumeParam)  global_param_mvo;
	TA_LIST(ModifierVolumeParam)  global_param_mvo_tr;

	TA_LIST(PolyParam)   global_param_op;
	TA_LIST(PolyParam)   global_param_pt;
	TA_LIST(PolyParam)   global_param_tr;
	TA_LIST(RenderPass)  render_passes;

	/* The region array's table of tiles (rend/tile/pvr_tile.h,
	 * pvr_region_entry()): a word per entry of the array, in its order,
	 * for the tiled renderers. Built only while ta_set_region_table(true)
	 * asks for it; region_tile_count is then how many entries the array
	 * had, of which the first REGION_TILES_MAX are kept here, and 0 when it
	 * was not built. */
	u32 *region_tiles;
	u32 region_tile_count;

	/* The opaque, punch-through and translucent lists' strips known to
	 * need no look at their vertices for their indices (ta_index.h), in
	 * one allocation */
	ta_strip_ok_t strip_ok[3];
	u32 *strip_ok_words;

	/* GPU-side TA processing drew it (ta_gpu_prepare(), TA_GPU_DRAW): the
	 * polygon parameter lists hold draw groups, and the vertices, indices
	 * and gpu_modtrig modifier volume triangles are on the GPU only */
	bool gpu;
	u32 gpu_modtrig;
	/* The render passes of the frame's data this render draws, the first
	 * and the last (ta_select_passes()); and whether there are others, which
	 * the game may start another render of the same frame for (a NAOMI 2
	 * game drawing one list continuation to the screen and the next to a
	 * texture: Virtua Fighter 4's reflections). Not cleared with the lists:
	 * set for each render as it is started. */
	u8 pass_first;
	u8 pass_last;
	bool keep;
	/* ...and with TA_GPU_SHADOWS, what each render pass's opaque modifier
	 * volumes cover (ModVolBounds()): min x, min y, max x, max y */
	f32 gpu_shadow_area[10][4];
	/* The raster renderers' modifier volume triangles, when the frame has
	 * one with a vertex far off the screen (rend/modvol_clip.h): every
	 * volume's triangles in their order, those far out cut to the square
	 * about the screen as the fans they make, and each volume's first and
	 * count in them - two words a volume, global_param_mvo's then
	 * global_param_mvo_tr's. Built while ta_set_modvol_clip(true) asks for
	 * it, on both of the TA's ways (the parse's triangles or the frame's
	 * blocks); empty for a frame with none far out, whose volumes are drawn
	 * from modtrig or the GPU's as ever. Full: left empty, as with none. */
	TA_LIST(ModTriangle) mv_clip;
	TA_LIST(u32) mv_clip_range;
	bool mv_clip_full;

#ifdef __cplusplus
	void Clear() { rend_context_clear(this); }
#endif
};

#define TA_DATA_SIZE (8 * 1024 * 1024)

/* Entries of the region array a frame's table keeps: 40 x 15 tiles, the
 * most a frame has, each in up to 10 passes, and some more (32 KB) */
#define REGION_TILES_MAX 8192

/* A NAOMI 2's geometry processor sends several times what a game can send
 * by itself: everything a frame is kept in is that much bigger there. */
#define TA_NAOMI2_SCALE 4

/* The control-word walk's table of a frame (ta_set_walk()): an entry for
 * each block of the frame's data that is not a vertex, as the blocks come
 * in, for the header pass (ta_walk_parse()). Room for this many, times
 * TA_NAOMI2_SCALE on a NAOMI 2 (64 KB, 256 KB there): games send a few
 * thousand parameters a frame. A frame with more is parsed the way it
 * always is. */
#define TA_WALK_MAX 16384
/* (walk_count: the table is not the frame's, or not all of it) */
#define TA_WALK_INVALID 0xFFFFFFFFu

static INLINE u32 ta_data_size(void)
{
	return settings.System == DC_PLATFORM_NAOMI2 ? TA_DATA_SIZE * TA_NAOMI2_SCALE : TA_DATA_SIZE;
}

/* The most tile accelerator data a save state carries, of all the frames
 * being written together: a state is always counted as having this much
 * (see pvr_serialize_v21()). */
#define TA_STATE_MAX (ta_data_size() / 4)

#ifndef TA_CONTEXT_DECLARED
#define TA_CONTEXT_DECLARED
typedef struct TA_context TA_context;
#endif
bool tactx_all_passes(const TA_context *ctx);
void tactx_mark_rend(TA_context *ctx, u32 render_pass);
/* The room a frame's vertex and index lists have, in bytes, as
 * tactx_alloc_lists() makes them (rend/parse_arena.h has slices of it) */
size_t tactx_vert_room(void);
size_t tactx_idx_room(void);
/* the modifier volumes' triangles', and those cut's (a third of them as
 * fans of three more) */
size_t tactx_modtrig_room(void);
size_t tactx_mv_clip_room(void);
/* The lists a frame's parse can write where the card reads them
 * (rend/parse_arena.h's parts), their rooms and the lists themselves */
void tactx_arena_rooms(size_t *room);
void tactx_arena_lists(TA_context *ctx, ta_list_t **lists);
/* false: there was not the memory for it, and it holds nothing (a NAOMI
 * 2's frame takes about 60 megabytes). @data: the data goes there, a slice
 * of ta_data_size() bytes of the GPU pool's (@slot), and is not the
 * context's to free (NULL and -1: its own). */
bool tactx_alloc_lists(TA_context *ctx, u8 *data, int slot);
void tactx_reset_lists(TA_context *ctx);
void tactx_free_lists(TA_context *ctx);

/* vertex lists */
struct TA_context
{
	u32 Address;

	/* The registers as they were when the game started this render, for
	 * the renderer to read while the emulation moves on (threaded
	 * rendering). */
	u8 *regs;

	u32 data_size;		/* of the tile accelerator data: ta_data_size() when this was allocated */
	u32 lastFrameUsed;	/* tactx_frames when the game last selected it (tactx_Find()) */

	tad_context tad;
	rend_context rend;

	/* The control-word walk's table (TA_WALK_MAX): the block number of each
	 * block of the data that is not a vertex, in order. walk_restart: the
	 * lists were started again, and the next entry starts the table of a
	 * new frame's data. Written by the emulation thread as the blocks come
	 * in, read by whoever draws the frame once it is handed over. */
	u32 *walk;
	u32 walk_size;		/* entries it has room for */
	u32 walk_count;		/* entries it has; TA_WALK_INVALID: it is not to be trusted */
	bool walk_restart;

	/* GPU-side TA processing: which of the renderer's mapped slices the
	 * data is (tactx_gpu_pool_attach()), -1 for memory of its own; and
	 * the renderer's serial of the last frame whose GPU work reads it,
	 * 0 for none. A slice is written again only once that work is done. */
	int pool;
	u32 gpu_serial;
	/* A render drew some of its render passes and left it in the list for
	 * another (rend.keep), and the game has not begun a frame here since:
	 * it is not a frame being written (nor is anything it changes:
	 * tactx_Restart()), and a save state has it after those, if there is
	 * room (pvr_serialize_v21()) */
	bool drawn;

	/*
		Dreamcast games use up to 20k vtx, 30k idx, 1k (in total) parameters.
		at 30 fps, thats 600kvtx (900 stripped)
		at 20 fps thats 1.2M vtx (~ 1.8M stripped)

		allocations allow much more than that !

		some stats:
			recv:   idx: 33528, vtx: 23451, op: 128, pt: 4, tr: 133, mvo: 14, modt: 342
			sc:     idx: 26150, vtx: 17417, op: 162, pt: 12, tr: 244, mvo: 6, modt: 2044
			doa2le: idx: 47178, vtx: 34046, op: 868, pt: 0, tr: 354, mvo: 92, modt: 976 (overruns)
			ika:    idx: 46748, vtx: 33818, op: 984, pt: 9, tr: 234, mvo: 10, modt: 16, ov: 0
			ct:     idx: 30920, vtx: 21468, op: 752, pt: 0, tr: 360, mvo: 101, modt: 732, ov: 0
			sa2:    idx: 36094, vtx: 24520, op: 1330, pt: 10, tr: 177, mvo: 39, modt: 360, ov: 0
	*/

#ifdef __cplusplus
	/* the render draws every render pass of the data */
	bool AllPasses() const { return tactx_all_passes(this); }
	void MarkRend(u32 render_pass) { tactx_mark_rend(this, render_pass); }
	static size_t VertRoom() { return tactx_vert_room(); }
	static size_t IdxRoom() { return tactx_idx_room(); }
	static size_t ModTrigRoom() { return tactx_modtrig_room(); }
	static size_t MvClipRoom() { return tactx_mv_clip_room(); }
	static void ArenaRooms(size_t *room) { tactx_arena_rooms(room); }
	void ArenaLists(ta_list_t **lists) { tactx_arena_lists(this, lists); }
	bool Alloc(u8 *data = NULL, int slot = -1) { return tactx_alloc_lists(this, data, slot); }
	void Reset() { tactx_reset_lists(this); }
	void Free() { tactx_free_lists(this); }
#endif
};

extern TA_context* ta_ctx;
extern tad_context ta_tad;

extern TA_context*  vd_ctx;
extern rend_context vd_rc;

TA_context* tactx_Find(u32 addr, bool allocnew);
void tactx_Reset(void);

TA_context* tactx_Alloc(void);
void tactx_Recycle(TA_context* poped_ctx);
/* The frame at @addr, for a render the game starts (NULL if there is
 * none), left where it is - as the frame being written, if it is - for
 * a render of some of its render passes (rend.keep: ta_select_passes()),
 * which the game may start another render of; otherwise tactx_Remove() it
 * and it is the render's. (Emulation thread.) */
TA_context* tactx_Get(u32 addr);
void tactx_Remove(TA_context* ctx);
/* The game begins a frame where one was drawn and kept (drawn): it is begun
 * as in a frame made anew there, the last in the list. (Emulation thread,
 * @ctx the current one.) */
void tactx_Restart(TA_context* ctx);
/* A render is done with @ctx: recycled, unless it was left where it was */
void tactx_Drawn(TA_context* ctx);
/* The frames being written: their tables of the control-word walk are not
 * to be trusted (from the emulation thread) */
void tactx_walk_forget(void);

/*
	Ta Context

	Rend Context
*/

#define TACTX_NONE (0xFFFFFFFF)

void SetCurrentTARC(u32 addr);
bool QueueRender(TA_context* ctx);
TA_context* DequeueRender(void);
void FinishRender(TA_context* ctx);
/* A frame skipped: not drawn, counted (libretro.cpp) */
extern u32 fskip;

/* must be moved to proper header */
void FillBGP(TA_context* ctx);
/* The lists' log said in the core's (ta_list.h), set once */
void ta_ctx_log_init(void);
/* The frames being written in a save state: from V21 (and the light gun,
 * the YUV converter), and from V28 their ages */
void pvr_serialize_v21(void **data, unsigned int *total_size);
bool pvr_unserialize_v21(void **data, unsigned int *total_size);
void pvr_state_before_v21(void);
void pvr_serialize_v28(void **data, unsigned int *total_size);
bool pvr_unserialize_v28(void **data, unsigned int *total_size);
void pvr_state_before_v28(void);
void SerializeTAContext(void **data, unsigned int *total_size);
void UnserializeTAContext(void **data, unsigned int *total_size, serialize_version_enum version);

/* ---- GPU-side TA processing, its decode (rend/ta_gpu.h) ---- */

/* The most a frame of the machine's size has of what the decode makes and
 * reads, for the renderer to size its buffers by */
typedef struct TaGpuSizes TaGpuSizes;
struct TaGpuSizes
{
	u32 verts, idx, list[3], modtrig;
	u32 headers, runs;	/* in the tables */
	u32 table_words;
};
void ta_gpu_sizes(TaGpuSizes *sizes);

/* Where the renderer has things: the tables' words TAG_T_TA_BASE to
 * TAG_T_REC_TR, in that order */
typedef struct TaGpuLayout TaGpuLayout;
struct TaGpuLayout
{
	u32 words[13];
};

/* What the header pass found, for the renderer */
typedef struct TaGpuFrame TaGpuFrame;
struct TaGpuFrame
{
	u32 table_words;	/* of the tables written */
	u32 verts, modtrig, headers, slots, segments, passes;
	RenderPass pass[10];	/* the region settings (the counts are the GPU's to give) */
	FB_X_CLIP_type x_clip;
	FB_Y_CLIP_type y_clip;
	bool rv;		/* there is something to draw */
	u32 trd;		/* translucent draw record slots the frame takes (TAG_H_TRB) */
	u32 shadows;		/* (TA_GPU_SHADOWS) the draw groups whose second drawing for shadows the GPU decides */
	u32 why;		/* TA_GPU_WHY_*: why it was not decoded, if it was not */
};

/* ta_gpu_prepare()'s @flags */
/* The renderer draws the frame from the decode, and the full parse is not
 * run: the lists of polygon parameters are made of the draw groups, in the
 * lists' order - for the opaque and punch-through lists, first the slot of
 * the group's draw command, count its vertices; for the translucent list,
 * first the group's place in the translucent draw records, count its
 * vertices, which is the most draw records it can have - and the render
 * passes count those. Frames that the full parse would drop for want of
 * room, or might, are left to it, and frames it passes over. */
#define TA_GPU_DRAW     1u
/* ...by a renderer that sorts translucent polygons on the CPU, from the
 * CPU's vertices: a frame that may have a sorted pass is left to the
 * full parse */
#define TA_GPU_NO_SORT  2u
/* The decode gives each triangle of a flat-shaded polygon the colours of
 * its last vertex in its first, as the Vulkan renderers draw them
 * (BaseDrawer::SetProvokingVertices()) */
#define TA_GPU_PROVOKE  4u
/* The translucent draw records number each strip as its polygon parameter
 * (firstInstance: the per-pixel renderer's polygon number) */
#define TA_GPU_INSTANCE 8u
/* ...by a renderer that sorts translucent triangles as GenSortedTo() does
 * (rend/sorter.h; its firstProvoking with TA_GPU_PROVOKE, for Vulkan, and
 * without it for OpenGL): each sorted render pass's
 * translucent triangles are sorted here from the depths of the blocks the
 * game sent - nothing else of a vertex is read - in GenSortedTo()'s order,
 * ties and all, for ta_gpu_sorted() and ta_gpu_sorted_write(). Needs
 * ta_gpu_sort_reserve(); without it, a frame that may have a sorted pass is
 * left to the full parse as with TA_GPU_NO_SORT. */
#define TA_GPU_SORT     16u
/* ...by a renderer that draws the polygons that take shadows a second
 * time where the opaque modifier volumes are (rend/shadows.h): each render
 * pass's volumes' bounds are worked out here from their blocks
 * (rend_context::gpu_shadow_area), and the GPU decides which of its draw
 * groups reach into them (TaGpuFrame::shadows, TAG_K_SHADOW) */
#define TA_GPU_SHADOWS  32u

enum { TA_GPU_WHY_NONE, TA_GPU_WHY_TABLE, TA_GPU_WHY_PARSE, TA_GPU_WHY_SORT, TA_GPU_WHY_WALK, TA_GPU_WHY_ROOM, TA_GPU_WHY_COUNT };

/* Whether frames are decoded on the GPU: the control-word walk on, and the
 * header pass's memory made for frames of the machine's size. (From the
 * thread that renders, between frames.) */
void ta_set_gpu_processing(bool on);
bool ta_gpu_processing(void);
/* What the core option asks for (TA Processing: GPU), for the renderers
 * that can to take up (from any thread) */
void ta_set_gpu_wanted(bool on);
bool ta_gpu_wanted(void);
/* The header pass over @ctx (after FillBGP(), before the full parse) and
 * the decode's tables written to @table, up to @table_room words, with
 * @layout's places. false: this frame is not one the GPU decode does - no
 * table to trust, a list left open at the end of a render pass, the data
 * ending in the middle of something, a parameter where a strip has not
 * ended, more than there is room for - and only the full parse can do it
 * (@out->why says which); with TA_GPU_DRAW, it is also made ready to
 * draw in the full parse's place (@flags: TA_GPU_*). (From the thread that renders.) */
bool ta_gpu_prepare(TA_context *ctx, const TaGpuLayout *layout, u32 *table, u32 table_room, TaGpuFrame *out, u32 flags);

/* TA_GPU_SORT's memory, made (or freed) now, for frames of the machine's
 * size: about 50 bytes for each triangle a frame can have. (From the
 * thread that renders, between frames.) */
bool ta_gpu_sort_reserve(bool on);
typedef struct SortTrigDrawParam SortTrigDrawParam;
/* What the last ta_gpu_prepare() with TA_GPU_SORT sorted, until the next:
 * render pass @pass's draws (GenSortedTo()'s @draws, first counting from
 * the first pass's first index, as Drawer::SortTriangles() has them), and
 * how many there are; NULL and 0 for a pass that is not sorted. The
 * polygon parameters they point to are the header pass's. */
const SortTrigDrawParam *ta_gpu_sorted(u32 pass, u32 *count);
/* ...and its depth prepass's draws (GenPrepassTo()'s; rend/sorter.h), first
 * counting from the first pass's first sorted index as well */
const SortTrigDrawParam *ta_gpu_prepass(u32 pass, u32 *count);
/* ...the indices of all of them (GenSortedTo()'s @out, pass after pass, then the prepasses') */
u32 ta_gpu_sorted_indices(void);
/* ...and the prepass's checks after them (TAG_K_PREPASS, at TAG_T_PREP):
 * how many, for a dispatch of a workgroup each, which the renderer makes
 * after the decode's TAG_K_UNPACK where there are any; all of it written
 * to @dst, ta_gpu_sorted_words() words, before that dispatch is made */
u32 ta_gpu_prepass_checks(void);
u32 ta_gpu_sorted_words(void);
void ta_gpu_sorted_write(u32 *dst);
/* ...and what the last sort took, in nanoseconds (0 for none) */
u64 ta_gpu_sorted_ns(void);
/* (TA_GPU_DRAW, with flat strips looked for) how long the last frame's took to find, in ns */
u64 ta_gpu_flat_ns(void);

/* The GPU pool: @count contexts made now whose data are the slices of
 * @slice bytes from @base (the renderer's persistently mapped, host-cached
 * buffer), given to new frames before any other while it is there. One is
 * written again only once the renderer says the GPU work that last read it
 * is done (tactx_gpu_done()); if none is free, a frame gets a context of
 * its own memory: the spare made with the pool, and past that one made then
 * as without the pool. Detached, the contexts still in use
 * have their data copied to memory of their own and go on. Both with the
 * machine standing still and the GPU idle. */
bool tactx_gpu_pool_attach(u8 *base, u32 slice, u32 count);
void tactx_gpu_pool_detach(void);
/* Since the pool was attached: frames that found it all taken (and went to
 * a spare context of their own memory, drawn from the CPU's parse), and of
 * them those that found no spare either and had one allocated */
void tactx_gpu_pool_stats(u32 *short_frames, u32 *allocated);
/* The GPU work of every frame up to renderer serial @serial is done (from
 * the thread that renders) */
void tactx_gpu_done(u32 serial);

#ifdef __cplusplus
}
#endif

#endif
