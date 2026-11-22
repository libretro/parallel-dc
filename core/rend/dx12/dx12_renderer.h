/*
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
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/sorter.h"
#include "rend/transform_matrix.h"
#include "dx12context_lr.h"
#include "dx12_pso.h"
#include "parse_arena_d3d12.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Direct3D 12 renderer: translucent polygons sorted by triangle or by
 * strip. What the game sees is what it sees with the Direct3D 11 renderer
 * (dx11/dx11_renderer.c), which this follows step for step - the same
 * passes, the same shadows (rend/shadows.h), the same renders to textures
 * that stay on the card until the game touches them (rend/rtt_watch.h),
 * the picture kept across a new context (rend/last_picture.h) - with
 * Direct3D 12's means:
 *  - one command list a render, executed on the frontend's queue when the
 *    render is done (dx12c_submit()), and never waited for;
 *  - every polygon drawn by the same shaders, its bits in ten root
 *    constants (dx12_shader_flags.h); the pipeline is the fixed-function
 *    state alone (dx12_pso.h), from a table made at the start;
 *  - the textures in one bindless table, the samplers in another: a draw
 *    binds nothing, it says which;
 *  - a render's vertices, indexes and constants in the upload ring, mapped
 *    once, read by the GPU where they were written;
 *  - draws that differ only in their root constants and index range -
 *    sorted translucent triangles above all - recorded as one
 *    ExecuteIndirect, with their arguments in the ring;
 *  - the frame a picture for each of the frontend's sync indexes, handed
 *    over with the fence that says when it is drawn (version 2) or copied
 *    by the frontend (version 1).
 *
 * SSAA (the core option; rend/ssaa_pattern.h), as the Vulkan renderers
 * have it (vulkan/ssaa.h): the screen's frames are drawn into a picture of
 * 2, 4 or 8 samples a pixel in the standard pattern and a depth of as many
 * (ssaa_color, the samples' depth), made when the renderer starts or
 * changes size - never in a frame - and resolved into the screen's picture
 * by a shader with the shared rule (rend/tile/pvr_resolve.h), not the
 * device's resolve. How many: the most of those asked for that the device
 * draws with, then that the video memory budget the context read at the
 * start has room for (dx12c_ssaa_samples()). Polygons are shaded at
 * each sample, but for the ones drawn at a single rate
 * (rend/ssaa_rate.h: sprites, the background, flat strips), shaded once a
 * pixel as without SSAA; the per-pixel renderer's translucent polygons are
 * put in its lists once a pixel with the samples they cover. The VMUs'
 * screens and the crosshairs go on the picture after the resolve. Renders
 * to textures have one sample a pixel, as without SSAA (Direct3D 12 has no
 * memory that is only backed when used, which is where the Vulkan
 * renderers supersample them).
 *
 * The frontend's temporal upscaler (rend/upscaler.h), as the Vulkan
 * renderers draw for it (vulkan/motion_pass.h): while it has the frames
 * (SSAA is off then: the upscaler is the anti-aliasing, and reads the
 * depth, which SSAA's samples are not), each frame of the screen is drawn
 * under its jitter and ends with the motion vector pass over the depth it
 * left - the opaque and punch-through strips again, each vertex with where
 * it was last frame in a second stream, tested against that depth
 * (DX12_PSO_MOTION) - into a picture of this sync index's, and with the
 * depth buffer's first plane drawn whole into another of its own, a
 * picture of floats: both are handed to the frontend with the picture,
 * which reads them until the frame has been shown, as it does the picture. A frame decoded
 * on the GPU has no vertices the pass can match: it is drawn without
 * jitter, handed over without them, and shown as it is. */


/* What the renderer that draws the frame has its own: the per-pixel one's
 * (oit/), the per-triangle one's where a hook is null */
typedef struct dx12_renderer_kind
{
	/* The frame drawn: dx12r_render_frame() where null */
	bool (*render_frame)(void);
	/* The frame's polygons: into what dx12r_begin_*() bound */
	void (*draw_frame)(void);
	/* What is bound by the time the frame is drawn: the per-pixel renderer
	 * puts the frame together in pictures of its own, and binds the
	 * screen's only at the end */
	bool binds_screen;
	/* How a frame decoded on the GPU is drawn (ta_gpu_prepare()'s
	 * TA_GPU_*); 0: none is - every frame is parsed */
	u32 (*ta_flags)(void);
	/* Whether the decoder is to have room for sorted translucent triangles (TA_GPU_SORT) */
	bool ta_sorts;
	/* What a renderer has of SSAA's: how many pictures of samples, and how
	 * much video memory it takes for itself first (KiB) */
	u32 ssaa_color_images;
	u32 (*ssaa_fixed_kib)(u32 width, u32 height);
	/* The pipelines a renderer wants made before a game draws (at init,
	 * before SSAA's are made from them) */
	void (*prepare_pipelines)(void);
	/* The frame's scissors, cut to the band being drawn by the per-pixel
	 * renderer (null: as they are) */
	void (*cut_scissor)(D3D12_RECT *rect);
	/* With the HDR output, the channel sums the frame's resolve stored,
	 * where the renderer resolves the samples itself (the per-pixel one,
	 * with SSAA; its compute renderer, with SSAA or the X scaler): for the
	 * encode, which takes their exact average. Null without */
	dx12_image *(*hdr_sums)(void);
} dx12_renderer_kind;

/* The screen's pictures: one for each of the frontend's sync indexes (as
 * many as dx12c_present_count() can say) */
#define DX12_SCREEN_SLOTS 32

/* The temporal upscaler's pictures of a sync index */
typedef struct dx12_upscaler_slot
{
	dx12_image motion;	/* the vectors (DX12MotionFormat), a render target */
	dx12_image depth;	/* the depth (DX12UpscalerDepthFormat), drawn or stored into */
} dx12_upscaler_slot;

/* The vertex shader's constants, as shaders/dx12_raster.hlsl reads them */
typedef struct dx12_vertex_constants
{
	float scale[4];		/* x, y, z, and the depth's scale */
	float offset[4];
} dx12_vertex_constants;

typedef struct dx12_fragment_constants
{
	float colorClampMin[4];
	float colorClampMax[4];
	float fogColRam[4];
	float fogColVert[4];
	float alphaTestValue;
	float fogDensity;
	float shadeScale;
	u32 pixelBufferCount;	/* the per-pixel renderer's: how many pixels its lists have room for */
	u32 ssaaMask;			/* SSAA's: every sample of a pixel, a bit each */
	u32 ssaaLog2;
} dx12_fragment_constants;

/* The polygon's root constants, as shaders/dx12_raster.hlsl reads them */
typedef struct dx12_poly_constants
{
	float clipTest[4];
	float trilinearAlpha;
	float paletteIndex;
	float prepassAlpha;	/* the depth prepass's least alpha (rend/sorter.h), 0 for the colour */
	u32 flags;			/* DX12_PF_ */
	u32 texture;		/* in the bindless table */
	u32 sampler;		/* in the sampler table */
	/* the per-pixel renderer's */
	u32 texture1;		/* the second volume's */
	u32 sampler1;
	u32 polyNumber;		/* the translucent polygon's, in its list */
	u32 oitFlags;		/* DX12_OF_ */
} dx12_poly_constants;

/* ---- The draws, batched ----
 *
 * A polygon's draw is its pipeline, scissor, stencil reference and
 * topology, and then its root constants and index range. Draws that share
 * the first four, one after the other, are recorded as one ExecuteIndirect
 * of the context's command signature, their constants and ranges written
 * to the ring; a run of one is recorded as itself. The run is closed by
 * anything that is not such a draw. */
typedef struct dx12_draw_state
{
	ID3D12PipelineState *pso;
	D3D12_RECT scissor;
	UINT stencilRef;
	D3D12_PRIMITIVE_TOPOLOGY topology;
} dx12_draw_state;

typedef struct dx12_draw_args
{
	u32 constants[DX12C_POLY_CONSTANT_COUNT];
	D3D12_DRAW_INDEXED_ARGUMENTS draw;
} dx12_draw_args;

#define DX12R_BATCH_MAX 4096

typedef struct dx12_renderer
{
	const dx12_renderer_kind *kind;
	ID3D12Device *device;		/* (the context's, while the renderer has it) */
	transform_matrix_t matrices;

	/* Where a frame is parsed when the card can draw it from there: a
	 * machine whose memory is the processor's (parse_arena_d3d12.h) */
	parse_arena_d3d12_t parse_arena;
	bool arena_missed;	/* (said once: a render copied, no slice free) */

	/* A frame decoded on the GPU's vertex constants, with the GPU's
	 * 1/fZ_max copied in: in a buffer of the default heap, which the
	 * ring's words are copied to */
	ID3D12Resource *gpu_vtx_constants;
	D3D12_RESOURCE_STATES gpu_vtx_constants_state;

	/* A render's vertices, indexes and modifier volumes, in the upload
	 * ring, one after the other: views of them */
	D3D12_VERTEX_BUFFER_VIEW vertex_view;	/* stride sizeof(Vertex) */
	D3D12_INDEX_BUFFER_VIEW index_view;
	D3D12_VERTEX_BUFFER_VIEW modvol_view;	/* stride three floats */
	bool have_modvols;
	/* ...and the constants */
	D3D12_GPU_VIRTUAL_ADDRESS vtx_constants_gpu;
	D3D12_GPU_VIRTUAL_ADDRESS frag_constants_gpu;

	dx12_image fog_texture;		/* 128 by 2, at DX12_SRV_FOG */
	u8 fog_uploaded[256];
	bool fog_uploaded_valid;
	dx12_image palette_texture;	/* 1024 by 1, at DX12_SRV_PALETTE */

	/* The depth and stencil, for the screen and renders to textures: as
	 * large as the largest of them so far. Its views in the depth heap: to
	 * draw with (depth.rtv), and for the per-pixel renderer, which reads the
	 * stencil or both from the shaders, ones that leave the stencil or both
	 * as they are. Its two planes are transitioned apart there: depth.state
	 * is the depth's, stencil_state the stencil's. */
	dx12_image depth;
	D3D12_RESOURCE_STATES stencil_state;
	u32 dsv_read_stencil;
	u32 dsv_read_only;
	/* (while the screen is drawn with SSAA, the samples' depth is the one
	 * there, and the depth of one sample is kept here: dx12r_ssaa_depth()) */
	dx12_image other_depth;
	D3D12_RESOURCE_STATES other_stencil_state;
	u32 other_dsv_read_stencil;
	u32 other_dsv_read_only;

	/* The screen: a picture for each of the frontend's sync indexes (one
	 * with version 1), the size rendered at */
	dx12_image screens[DX12_SCREEN_SLOTS];
	u32 screen_count;
	/* ...and one to draw a second render to the screen in, in a frame whose
	 * first one has been handed over (version 2) already: it takes the
	 * place of the one handed over, which stays the frontend's */
	dx12_image extra;
	/* The screen's pictures can be drawn into by the compute renderer
	 * (unordered access views of them): set before they are made */
	bool storage_pictures;
	/* ...or by the chip's write-out (dx12_writeout.h), as they were made */
	bool writeout_pictures;
	bool last_in_extra;		/* the last picture is in it */
	u32 screen_width;
	u32 screen_height;
	int screen_index;		/* the one being drawn */
	int last_screen;		/* the one the last picture is in */
	bool have_picture;
	/* ...or the picture kept from the context before (rend/last_picture.h),
	 * or from the size before (dx12r_resize()) */
	dx12_image restored_picture;

	/* A render to a texture: the texture, where in video memory */
	struct dx12_texture *rtt_texture;
	u32 rtt_addr;
	/* What the frame is drawn into, and the size of what is drawn of it */
	u32 target_rtv;
	u32 target_width;
	u32 target_height;

	D3D12_RECT base_scissor;
	u32 pixel_buffer_room;	/* the per-pixel renderer's: how many pixels its lists have room for */

	/* The frontend was waited for in process(), for the render() after it */
	bool held_from_process;

	/* ---- The temporal upscaler ---- */
	dx12_upscaler_slot upscaler_slots[DX12_SCREEN_SLOTS];	/* one a sync index, the screen's size */
	u32 upscaler_slot_count;
	/* The depth buffer's first plane as a texture of the table, which the
	 * copy into a slot's depth reads (DX12_DEPTH_COPY_PS): a place of its
	 * own for each depth buffer there is (the GPU may still read the last) */
	u32 depth_copy_srv;
	ID3D12Resource *depth_copy_viewed;
	bool upscaled;		/* the frame being drawn is drawn for the upscaler */
	D3D12_VERTEX_BUFFER_VIEW prev_xy_view;
	/* A frame the compute renderer drew (dx12_compute.h): its walk stored the
	 * depth in the slot's own image, which the pass's pixel shader tests
	 * against; its vertices and indices are the frame's in the compute
	 * renderer's ring */
	bool motion_sampled;
	D3D12_VERTEX_BUFFER_VIEW motion_vertex_view;
	D3D12_INDEX_BUFFER_VIEW motion_index_view;

	/* ---- SSAA ---- */
	u32 ssaa_samples;		/* the screen's samples a pixel (1: off) */
	u32 ssaa_log2;
	u32 ssaa_width, ssaa_height;	/* the size its pictures are made for */
	/* The pictures of samples the screen's frames are drawn into: one; the
	 * per-pixel renderer's two (dx12_renderer_kind::ssaa_color_images) */
	dx12_image ssaa_color[2];
	bool ssaa_depth_in;	/* the samples' depth is in depth (dx12r_ssaa_depth()) */

	/* Per-triangle sort results: a SortList a pass (their indices in the
	 * upload ring, sorted there by the upload); and the depth prepasses of
	 * the sorted lists (rend/sorter.h), their indexes after each pass's
	 * sorted ones */
	grow_buf_t sorted_passes;
	u32 sorted_index_count;
	grow_buf_t prepass_passes;
	/* (a frame decoded on the GPU) the draws are of the sorted indices,
	 * whose counts are the CPU's */
	bool gpu_sorted;
	bool prepass_draw;	/* the polygons are drawn for the depth prepass */

	dx12_draw_state batch_state;
	dx12_draw_args *batch;
	u32 batch_count;
	u32 stat_batches, stat_batched;

	/* The framebuffer the game wrote itself, for a frame that is that */
	dx12_image fb_texture;
} dx12_renderer;

extern dx12_renderer dx12r;

/* The base's: what the per-pixel renderer has of this one (@storage: the
 * screen's pictures made for unordered access views too) */
bool dx12r_init(const dx12_renderer_kind *kind, bool storage);
void dx12r_term(void);
void dx12r_resize(int w, int h);
bool dx12r_process(TA_context *ctx);
bool dx12r_render(void);
bool dx12r_render_frame(void);
void dx12r_keep_picture(void);
u64 dx12r_get_texture(u32 tsp, u32 tcw, int area);
void dx12r_options_changed(void);
bool dx12r_ready(void);

/* Whether the frame being drawn is drawn into the samples */
static INLINE bool dx12r_ssaa_active(void)
{
	return dx12r.ssaa_depth_in;
}
/* The samples' depth in depth (and in the views of the per-pixel
 * renderer's table for them), or the depth of one sample */
void dx12r_ssaa_depth(bool in);
bool dx12r_ensure_depth(u32 width, u32 height);
/* The depth's planes into these states */
void dx12r_depth_states(D3D12_RESOURCE_STATES depth_plane, D3D12_RESOURCE_STATES stencil_plane);
/* An image of the renderer's own gone, once the GPU is done with it */
void dx12r_drop_image(dx12_image *image);
/* Whether the render starts from the last picture: it covers only part
 * of the screen, and the game has drawn to this framebuffer lately */
bool dx12r_keeps_last_picture(void);
/* Where it is */
dx12_image *dx12r_last_picture_image(void);
/* TILECLIP_*: @rect the polygon's clip */
int dx12r_set_tile_clip(u32 val, D3D12_RECT *rect);
void dx12r_set_base_scissor(void);
void dx12r_set_provoking_vertices(void);
/* @rect cut to the band being drawn (dx12_renderer_kind::cut_scissor) */
D3D12_RECT dx12r_cut_scissor(const D3D12_RECT *rect);
/* ...and the constants alone (the upload's, and a frame decoded on the
 * GPU's: its 1/fZ_max copied in from the decode's words) */
bool dx12r_upload_constants(void);
/* A modifier volume's pipeline */
ID3D12PipelineState *dx12r_modvol_pipeline(bool depth_test, bool depth_write, u32 depth_func, int stencil, u32 cull, bool write_color, u32 special);
void dx12r_draw(const dx12_draw_state *state, const u32 *constants, u32 index_count, u32 first_index);
void dx12r_flush_batch(void);
/* A draw group of a frame decoded on the GPU (pvrrc.gpu), drawn by its
 * arguments there: an opaque or punch-through one by its command (or,
 * @shadow, its second one, for shadows), a translucent one by its
 * records, with their count. Recorded as itself. */
void dx12r_draw_gpu(const dx12_draw_state *state, const u32 *constants, u32 list_type, bool shadow, u32 first, u32 count);

/* The screen's picture made ready (dx12r_ensure_screen()) and handed over
 * once the frame is drawn into it (dx12r_end_screen()); a render to a
 * texture's texture taken (dx12r_begin_rtt()) and given to the texture once
 * it is drawn (dx12r_end_rtt()): the render's, and the compute renderer's
 * (dx12_compute.h) */
bool dx12r_ensure_screen(u32 width, u32 height);
/* (@writeout: the chip's write-out over the picture first, a frame the
 * raster renderer drew: dx12_writeout.h) */
void dx12r_end_screen(bool writeout);
bool dx12r_begin_rtt(void);
void dx12r_end_rtt(void);
/* The size of the texture a render to a texture draws into: from the
 * frame's first pixel to the pixel clip's last, in powers of two, times
 * Render To Texture Upscaling */
void dx12r_rtt_size(u32 *width_pow2, u32 *height_pow2);
/* The frame of the screen drawn for the upscaler: its jitter taken, and
 * then its strips matched (the compute renderer's frame: once the frame is
 * its, after the jitter) */
bool dx12r_upscaler_jitter(void);
bool dx12r_upscaler_match(void);

#ifdef __cplusplus
}
#endif
