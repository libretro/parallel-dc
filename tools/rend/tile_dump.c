/* The frames a game draws to the screen, written as the compute renderer's
 * words (rend/tile/pvr_compute.h, compute_frame_pack() with
 * rend/vulkan/vk_compute_drawer.c's frame), for tile_test to draw on a device and
 * with the reference. Linked into one program with the core's objects and
 * tools/threads/headless.c as the frontend (tile_test.sh): the link puts
 * __wrap_ta_parse_vdrc() in the place of the parse, and headless.c's
 * dlopen() opens the program itself.
 *
 *   TILE_DUMP_DIR   where the frames go (frame_NNNNN.bin)
 *   TILE_DUMP_EVERY one frame in this many is written (default 100)
 *   TILE_DUMP_MAX   at most this many (default 8)
 *   TILE_DUMP_FROM  the first frame that may be (default 0)
 *   TILE_W, TILE_H  the picture's size (default the core's screen)
 *
 * And renders to a texture, as the compute renderer draws them, at the
 * size of the texture they go to (vk_oit_rtt_texture_size(), with
 * Render To Texture Upscaling), from TILE_DUMP_FROM too (rtt_NNNNN.bin):
 *   TILE_DUMP_RTT       at most this many (default 4; 0 none)
 *   TILE_DUMP_RTT_EVERY one render to a texture in this many (default 10)
 *
 * And, for the reference software renderer (tools/rend/refsw), the same
 * frames as the parse left them, with the registers and video memory
 * (refsw/refsw_dump.h: refsw_NNNNN.bin), chosen by the same variables:
 *   REFSW_DUMP_DIR  where they go; nothing is written without it */
#define PVR_REGS_FOR_RENDERER
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/rtt_watch.h"
#include "rend/tile/compute_frame.h"
#include "rend/vulkan/vk_compute_drawer.h"
#include "rend/vulkan/oit/vk_oit_drawer.h"
#include "refsw/refsw_dump.h"

void *__real_dlopen(const char *file, int mode);
void *__wrap_dlopen(const char *file, int mode)
{
	return __real_dlopen(file != NULL && strcmp(file, "self") == 0 ? NULL : file, mode);
}

bool __real_ta_parse_vdrc(TA_context *ctx);

/* Test mode draws a texture from its slot and coordinates: any slot will
 * do, so they go by the order the frame asks for them */
typedef struct dump_slots
{
	u64 texid[PVRC_TEX_SLOTS];
	u32 mask[PVRC_TEX_SLOTS];
	u32 count;
} dump_slots;

static u32 dump_slot(void *self, u64 texid, u32 tsp)
{
	dump_slots *s = (dump_slots *)self;
	const u32 mask = tsp & 0x7ef00;
	u32 i;
	for (i = 0; i < s->count; i++)
		if (s->texid[i] == texid && s->mask[i] == mask)
			return i;
	if (s->count >= PVRC_TEX_SLOTS)
		return PVRC_NONE;
	s->texid[s->count] = texid;
	s->mask[s->count] = mask;
	return s->count++;
}

static vk_compute_frame packer;
static bool packerMade;
static u32 *words;
static u32 wordRoom;
static Vertex *keep;
static u32 keepRoom;
static u32 frames, written, rttFrames, rttWritten;

static const xform *dump_begin(void *self)
{
	(void)self;
	return vk_compute_frame_begin(&packer);
}

static void dump_base_scissor(void *self, int *x, int *y, int *width, int *height)
{
	(void)self;
	vk_compute_frame_base_scissor(&packer, x, y, width, height);
}

static void dump_provoking_vertices(void *self)
{
	(void)self;
	vk_compute_frame_provoking_vertices(&packer);
}

/* The frame in pvrrc packed into words as @layout says; their count (0:
 * not packed). The parse's vertices are changed for flat shading, as the
 * raster renderers change them: on a copy of the frame's, so that the
 * core's own renderer draws what it would. */
static u32 pack(TA_context *ctx, const compute_frame_layout *layout, u32 *prims)
{
	const u32 verts = TA_LIST_USED(ctx->rend.verts, Vertex);
	dump_slots slots;
	compute_frame_renderer r;
	u32 why = CF_WHY_NONE;
	u32 count;
	if (!packerMade)
	{
		vk_compute_frame_init(&packer);
		packerMade = true;
	}
	if (words == NULL)
	{
		wordRoom = compute_frame_max_words();
		words = (u32 *)malloc(wordRoom * sizeof(u32));
		if (words == NULL)
			return 0;
	}
	if (keepRoom < verts)
	{
		Vertex *more = (Vertex *)realloc(keep, verts * sizeof(Vertex));
		if (more == NULL)
			return 0;
		keep = more;
		keepRoom = verts;
	}
	memset(&slots, 0, sizeof(slots));
	r.self = &slots;
	r.slot = dump_slot;
	r.begin = dump_begin;
	r.base_scissor = dump_base_scissor;
	r.provoking_vertices = dump_provoking_vertices;
	memcpy(keep, TA_LIST_HEAD(ctx->rend.verts, Vertex), verts * sizeof(Vertex));
	count = compute_frame_pack(layout, &r, words, wordRoom, prims, &why);
	memcpy(TA_LIST_HEAD(ctx->rend.verts, Vertex), keep, verts * sizeof(Vertex));
	return count;
}

static bool put(FILE *f, const void *head, u32 size, u32 used)
{
	return used == 0 || fwrite(head, size, used, f) == used;
}
#define PUT(f, list, T) put(f, TA_LIST_HEAD(list, T), (u32)sizeof(T), TA_LIST_USED(list, T))

/* The frame for the reference renderer: the context's lists as the parse
 * left them, the registers (the renderer's copy: the ones this frame is
 * drawn with) and video memory, whole. */
static void refsw_dump(TA_context *ctx, const char *dir, u32 n, u32 width, u32 height)
{
	const rend_context *r = &ctx->rend;
	struct refsw_dump_header h;
	char path[1024];
	FILE *f;
	bool ok;
	memset(&h, 0, sizeof(h));
	h.magic = REFSW_DUMP_MAGIC;
	h.version = REFSW_DUMP_VERSION;
	h.width = width;
	h.height = height;
	h.regs_size = pvr_RendRegSize;
	h.vram_size = VRAM_SIZE;
	h.sizeof_vertex = sizeof(Vertex);
	h.sizeof_polyparam = sizeof(PolyParam);
	h.sizeof_modtriangle = sizeof(ModTriangle);
	h.sizeof_mvparam = sizeof(ModifierVolumeParam);
	h.sizeof_renderpass = sizeof(RenderPass);
	h.n_verts = TA_LIST_USED(r->verts, Vertex);
	h.n_idx = TA_LIST_USED(r->idx, u32);
	h.n_op = TA_LIST_USED(r->global_param_op, PolyParam);
	h.n_pt = TA_LIST_USED(r->global_param_pt, PolyParam);
	h.n_tr = TA_LIST_USED(r->global_param_tr, PolyParam);
	h.n_modtrig = TA_LIST_USED(r->modtrig, ModTriangle);
	h.n_mvo = TA_LIST_USED(r->global_param_mvo, ModifierVolumeParam);
	h.n_mvo_tr = TA_LIST_USED(r->global_param_mvo_tr, ModifierVolumeParam);
	h.n_passes = TA_LIST_USED(r->render_passes, RenderPass);
	h.n_region_tiles = r->region_tiles != NULL && r->region_tile_count <= REGION_TILES_MAX ? r->region_tile_count : 0;
	h.fZ_min = r->fZ_min;
	h.fZ_max = r->fZ_max;
	h.fb_x_clip = r->fb_X_CLIP.full;
	h.fb_y_clip = r->fb_Y_CLIP.full;
	h.fog_clamp_min = r->fog_clamp_min;
	h.fog_clamp_max = r->fog_clamp_max;
	h.frame = n;
	/* What a render to a texture left waiting goes into video memory first:
	 * its pages are closed till then, and the write below, a system call,
	 * fails on them rather than fault (Virtua Fighter 4's reflections) */
	rtt_watch_touch(0, VRAM_SIZE);
	snprintf(path, sizeof(path), "%s/refsw_%05u.bin", dir, n);
	f = fopen(path, "wb");
	ok = f != NULL
		&& fwrite(&h, sizeof(h), 1, f) == 1
		&& fwrite(rend_pvr_regs, 1, pvr_RendRegSize, f) == pvr_RendRegSize
		&& fwrite(vram.data, 1, VRAM_SIZE, f) == VRAM_SIZE
		&& PUT(f, r->verts, Vertex) && PUT(f, r->idx, u32) && PUT(f, r->global_param_op, PolyParam)
		&& PUT(f, r->global_param_pt, PolyParam) && PUT(f, r->global_param_tr, PolyParam)
		&& PUT(f, r->modtrig, ModTriangle) && PUT(f, r->global_param_mvo, ModifierVolumeParam)
		&& PUT(f, r->global_param_mvo_tr, ModifierVolumeParam) && PUT(f, r->render_passes, RenderPass)
		&& (h.n_region_tiles == 0 || fwrite(r->region_tiles, 4, h.n_region_tiles, f) == h.n_region_tiles);
	if (f != NULL)
		fclose(f);
	if (!ok)
		printf("tile_dump: %s not written\n", path);
	else
		printf("tile_dump: %s: %u vertices, %u indices, %u/%u/%u polygons, %u/%u modifier volumes (%u triangles), %u passes\n", path,
				h.n_verts, h.n_idx, h.n_op, h.n_pt, h.n_tr, h.n_mvo, h.n_mvo_tr, h.n_modtrig, h.n_passes);
	fflush(stdout);
}

static void layout_runs(compute_frame_layout *layout)
{
	layout->pool = 2u << 20;
	layout->chunks = (compute_frame_max_prims() + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2;
	layout->runs = layout->chunks * ((layout->width + (1u << layout->tileLog2) - 1) >> layout->tileLog2)
			* ((layout->height + (1u << layout->tileLog2) - 1) >> layout->tileLog2);
	layout->lastPicture = false;
	layout->lastScaled = false;
}

static void write_words(const char *path, u32 count)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL || fwrite(words, 4, count, f) != count)
		printf("tile_dump: %s not written\n", path);
	if (f != NULL)
		fclose(f);
}

/* A render to a texture, as the compute renderer packs it */
static void rtt_dump(TA_context *ctx, const char *dir, u32 n)
{
	const VkExtent2D size = vk_oit_rtt_texture_size();
	compute_frame_layout layout;
	char path[1024];
	u32 prims, count;
	if (ctx->rend.fb_X_CLIP.max < ctx->rend.fb_X_CLIP.min || ctx->rend.fb_Y_CLIP.max < ctx->rend.fb_Y_CLIP.min)
		return;	/* (no pixel: the renderers draw nothing) */
	layout.width = size.width;
	layout.height = size.height;
	layout.tileLog2 = compute_frame_tile_log2(layout.width);
	layout_runs(&layout);
	layout.rttScale = compute_frame_rtt_scale();
	count = pack(ctx, &layout, &prims);
	if (count == 0)
	{
		printf("tile_dump: render to a texture %u not packed\n", n);
		return;
	}
	snprintf(path, sizeof(path), "%s/rtt_%05u.bin", dir, n);
	write_words(path, count);
	printf("tile_dump: render to a texture %u: %u x %u (upscaled %u times), clip %u-%u x %u-%u, %u primitives, %u render passes, %u words\n",
			n, layout.width, layout.height, layout.rttScale, ctx->rend.fb_X_CLIP.min, ctx->rend.fb_X_CLIP.max,
			ctx->rend.fb_Y_CLIP.min, ctx->rend.fb_Y_CLIP.max, prims, words[PVRC_F_PASSES], count);
	fflush(stdout);
	rttWritten++;
}

static u32 env_u32(const char *name, u32 otherwise)
{
	const char *v = getenv(name);
	return v != NULL ? (u32)atoi(v) : otherwise;
}

bool __wrap_ta_parse_vdrc(TA_context *ctx)
{
	const bool rv = __real_ta_parse_vdrc(ctx);
	const char *dir = getenv("TILE_DUMP_DIR");
	const char *refsw_dir = getenv("REFSW_DUMP_DIR");
	const u32 every = env_u32("TILE_DUMP_EVERY", 100);
	const u32 most = env_u32("TILE_DUMP_MAX", 8);
	const u32 from = env_u32("TILE_DUMP_FROM", 0);
	const u32 rttMost = env_u32("TILE_DUMP_RTT", 4);
	const u32 rttEvery = getenv("TILE_DUMP_RTT_EVERY") != NULL && atoi(getenv("TILE_DUMP_RTT_EVERY")) > 1
			? (u32)atoi(getenv("TILE_DUMP_RTT_EVERY")) : (getenv("TILE_DUMP_RTT_EVERY") != NULL ? 1 : 10);
	const u32 n = frames++;
	compute_frame_layout layout;
	char path[1024];
	u32 prims, count;
	if (rv && refsw_dir != NULL)
		printf("tile_dump: render %u: %s%u/%u/%u polygons, %u vertices\n", n, ctx->rend.isRTT ? "to a texture, " : "",
				TA_LIST_USED(ctx->rend.global_param_op, PolyParam), TA_LIST_USED(ctx->rend.global_param_pt, PolyParam),
				TA_LIST_USED(ctx->rend.global_param_tr, PolyParam), TA_LIST_USED(ctx->rend.verts, Vertex));
	if (rv && dir != NULL && ctx->rend.isRTT && n >= from)
	{
		const u32 k = rttFrames++;
		if (rttWritten < rttMost && k % rttEvery == 0)
			rtt_dump(ctx, dir, n);
		return rv;
	}
	if (!rv || (dir == NULL && refsw_dir == NULL) || ctx->rend.isRTT || ctx->rend.isRenderFramebuffer || written >= most || n < from
			|| (n - from) % every != 0)
		return rv;
	if (refsw_dir != NULL)
		refsw_dump(ctx, refsw_dir, n, screen_width > 0 ? (u32)screen_width : 640, screen_height > 0 ? (u32)screen_height : 480);
	if (dir == NULL)
	{
		written++;
		return rv;
	}
	layout.width = env_u32("TILE_W", screen_width > 0 ? (u32)screen_width : 640);
	layout.height = env_u32("TILE_H", screen_height > 0 ? (u32)screen_height : 480);
	layout.tileLog2 = layout.width > 1280 ? 6 : 5;
	layout_runs(&layout);
	layout.rttScale = 0;
	count = pack(ctx, &layout, &prims);
	if (count == 0)
	{
		printf("tile_dump: frame %u not packed\n", n);
		return rv;
	}
	snprintf(path, sizeof(path), "%s/frame_%05u.bin", dir, n);
	write_words(path, count);
	printf("tile_dump: frame %u: %u x %u, %u primitives, %u render passes, %u words\n", n, layout.width, layout.height,
			prims, words[PVRC_F_PASSES], count);
	fflush(stdout);
	written++;
	return rv;
}
