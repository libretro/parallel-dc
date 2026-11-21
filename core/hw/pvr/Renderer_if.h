#pragma once
#include "types.h"
#include "emulator.h"

#include "ta_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The frame being drawn (its rend_context: pvrrc) */
extern TA_context* _pvrrc;

#define pvrrc (_pvrrc->rend)

/* The size the frontend shows the picture at (gles/gles.c) */
extern int screen_width, screen_height;

extern u32 VertexCount;
/* (ta_vtx.c) */
extern u32 decoded_colors[3][65536];
extern u32 FrameCount;

void rend_term(void);

void rend_vblank(void);
void rend_start_render(void);
/* The frontend will not show the frame being made: renders to the screen
 * are not drawn and the vblank reports no new picture. Renders to a
 * texture are, the game reads them back; so is a picture the processor
 * wrote to the framebuffer, which is taken only once. */
void rend_skip_screen(bool skip);
void rend_end_render(void);
void rend_end_wait(void);

void rend_set_fb_scale(float x,float y);

extern u32 fb_watch_addr_start;
extern u32 fb_watch_addr_end;
extern bool fb_dirty;

void check_framebuffer_write(void);
bool rend_frame_produced(void);

void rend_create_renderer(void);
void rend_init_renderer(void);
/* The renderer made, or made again, if it is to be (renderer_changed, or
 * none yet): between frames - when the context is made, when an option
 * has it made again - so that the first frame does not; rend_frame() does
 * it otherwise */
void rend_prepare_renderer(void);
void rend_term_renderer(void);
void rend_keep_picture(void);
/* After a run that drew nothing to the screen: true if the frame handed to
 * the frontend is the picture kept across a change of renderer
 * (renderer_ops present_kept_picture), once after the change */
bool rend_present_kept_picture(void);
/* The size rendered at has changed and the context is the one there was. */
void rend_resize(int width, int height);

extern bool renderer_changed;
/* The most each list of a frame has had (for the log) and the frames
 * thrown away for overrunning */
extern int max_idx, max_mvo, max_op, max_pt, max_tr, max_vtx, max_modt, ovrn;
/* A render asked for and not yet drawn, and the framebuffer it was to be
 * shown in (their part of a state) */
extern bool pend_rend;
extern u32 fb_w_cur;

/* Every texture made again before it is next used (TexCache.c) */
extern bool KillTex;
/* The tile accelerator's contexts emptied (ta_ctx.c) */
void tactx_Reset(void);
/* The custom textures' loader stopped (custom_texture.c) */
void custom_texture_terminate(void);
/* The texture cache's room for converting and scaling up given back
 * (TexCache.c) */
void texcache_scratch_free(void);

/* The renderer in use (renderer_dispatch.c) */
/* The one the settings choose made, and the one to fall back to, if any */
void renderer_create(void);
/* Set up on the frontend's context, or the one to fall back to; false
 * and none if neither can be */
bool renderer_init(void);
/* Taken down, and the one to fall back to with it */
void renderer_destroy(void);
bool renderer_exists(void);
void renderer_keep_picture(void);
bool renderer_present_kept_picture(void);
void renderer_resize(int width, int height);
bool renderer_process(TA_context *ctx);
/* The renderer's texture of a polygon's words, made now if it is not made
 * (area 1: the second volume's) */
u64 renderer_get_texture(u32 tsp, u32 tcw, int area);
bool renderer_render(void);
void renderer_present(void);
/* Whether every pipeline a frame can want is made (true with none): a
 * renderer started in a frame (another one chosen while a game runs)
 * hands them to its worker, and until they are, retro_run() shows the
 * kept picture and does not run the machine (AGENTS.md: nothing made,
 * nothing waited for, on the draw path) */
bool renderer_ready(void);
/* The core options were read again, between frames: what they ask the
 * renderer to have is made, or taken down, now and not in the next frame */
void renderer_options_changed(void);
/* Draw the render that is queued (libretro.cpp, rend_start_render()) */
bool rend_single_frame(void);

/* A renderer: NULL for what one does by default (nothing; Ready: true,
 * PresentKeptPicture and GetTexture: none) */
typedef struct renderer_ops
{
	bool (*init)(void);
	void (*resize)(int w, int h);
	void (*term)(void);
	bool (*process)(TA_context *ctx);
	bool (*render)(void);
	void (*keep_picture)(void);
	u64 (*get_texture)(u32 tsp, u32 tcw, int area);
	void (*options_changed)(void);
	/* Made in renderer_create(), given back after term() or an init()
	 * that failed (a C++ class's: renderer_class.h) */
	void (*create)(void);
	void (*release)(void);
	void (*present)(void);
	/* The picture kept from another renderer handed to the frontend as
	 * the frame (a run that drew nothing to the screen); false where
	 * there is none */
	bool (*present_kept_picture)(void);
	bool (*ready)(void);
} renderer_ops;

/* The renderers renderer_create() chooses from: OpenGL per triangle or
 * strip (gles/gles.c) and per pixel (gl4/gles.c); the others are C++
 * classes (rend/renderer_class.h) */
const renderer_ops *rend_GLES2(void);
const renderer_ops *rend_GL4(void);
const renderer_ops *rend_Vulkan(void);
const renderer_ops *rend_OITVulkan(void);
const renderer_ops *rend_D3D11(void);
const renderer_ops *rend_OITD3D11(void);
const renderer_ops *rend_D3D9(void);
const renderer_ops *rend_D3D12(void);
const renderer_ops *rend_OITD3D12(void);

#ifdef __cplusplus
}
#endif
