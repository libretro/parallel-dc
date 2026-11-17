#include "Renderer_if.h"
#include "serialize.h"
#include "rend/last_picture.h"
#include "ta.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/texconv.h"
#include "rend/sorter.h"
#include "rend/rtt_watch.h"
#include "hw/mem/_vmem.h"
#include "cheats.h"
#include "spg.h"
#include "rend/rend_method.h"

/* (hw/sh4/sh4_core.h, which has a name for every register of the SH4) */
#include "hw/sh4/sh4_if.h"	/* (RestoreHostRoundingMode, HostFloatModeBack) */

/*

	rendv3 ideas
	- multiple backends
	  - ESish
	    - OpenGL ES2.0
	    - OpenGL ES3.0
	    - OpenGL 3.1
	  - OpenGL 4.x
	  - Direct3D 10+ ?
	- correct memory ordering model
	- resource pools
	- threaded TA
	- threaded rendering
	- RTTs
	- framebuffers
	- overlays


	PHASES
	- TA submission (memops, dma)

	- TA parsing (defered, rend thread)

	- CORE render (in-order, defered, rend thread)


	submission is done in-order
	- Partial handling of TA values
	- Gotchas with TA contexts

	parsing is done on demand and out-of-order, and might be skipped
	- output is only consumed by renderer

	render is queued on RENDER_START, and won't stall the emulation or might be skipped
	- VRAM integrity is an issue with out-of-order or delayed rendering.
	- selective vram snapshots require TA parsing to complete in order with REND_START / REND_END


	Complications
	- For some apis (gles2, maybe gl31) texture allocation needs to happen on the gpu thread
	- multiple versions of different time snapshots of the same texture are required
	- TA parsing vs frameskip logic


	Texture versioning and staging
	 A memory copy of the texture can be used to temporary store the texture before upload to vram
	 This can be moved to another thread
	 If the api supports async resource creation, we don't need the extra copy
	 Texcache lookups need to be versioned


	rendv2x hacks
	- Only a single pending render. Any renders while still pending are dropped (before parsing)
	- wait and block for parse/texcache. Render is async
*/

u32 VertexCount=0;
u32 FrameCount=1;

bool renderer_changed = false;	/* Signals the renderer interface to switch renderer */
/* No renderer could be set up on the frontend's context (its device was
 * refused, say): the frames run and none is drawn, until the context is
 * made again (renderer_changed). Not tried again each frame. */
static bool renderer_failed = false;

#if !defined(TARGET_NO_THREADS)
#include "libretro/emu_baton.h"
#endif
u32 fb_w_cur = 1;
bool rend_compute_wanted = false;	/* rend/rend_method.h */

int max_idx,max_mvo,max_op,max_pt,max_tr,max_vtx,max_modt, ovrn;
bool pend_rend = false;

static bool render_called = false;	/* a render started since the last vblank (rend_vblank) */
static bool frame_produced = false;	/* a picture to show made in this run (for is_dupe) */
static bool skip_screen = false;	/* rend_skip_screen */
/* a renderer was made again in place of another (rend_present_kept_picture()) */
static bool kept_picture_due = false;
u32 fb_watch_addr_start;
u32 fb_watch_addr_end;
bool fb_dirty;

TA_context* _pvrrc;

void rend_create_renderer(void)
{
	renderer_create();
}

/* The last two framebuffers the game rendered to. A game flips between two
 * and draws over what it drew there two frames before. When it renders to
 * a third - after the BIOS has handed over, or on a change of video mode -
 * what the console has there is not what was on screen: the renderers draw
 * into one picture, though, and whatever the game does not draw over would
 * be left from the frame before. The picture is cleared then, to the
 * border colour. (Upstream: the BIOS's last frame showing above and below
 * the picture in San Francisco Rush 2049.) */
static u32 fb_addr_history[2] = { 1, 1 };

void rend_init_renderer(void)
{
	/* (the translucent sort's room, and a render's read back, before the
	 * first frame) */
	SortReserve();
	rtt_watch_reserve();
	if (!renderer_init())
	{
		/* (not the frontend taken down with the core: no frame is drawn
		 * until there is a context a renderer can be set up on) */
		renderer_failed = true;
		ERROR_LOG(PVR, "Renderer initialization failed: no frame is drawn until the video context is made again\n");
	}
}

/* The context is going (libretro thread, the machine standing still). */
void rend_keep_picture(void)
{
	int w, h;
	renderer_keep_picture();
	if (last_picture(&w, &h) != NULL)
		NOTICE_LOG(RENDERER, "%d x %d picture kept for the next context\n", w, h);
}

bool rend_present_kept_picture(void)
{
	if (!kept_picture_due)
		return false;
	kept_picture_due = false;
	return renderer_present_kept_picture();
}

void rend_resize(int width, int height)
{
	renderer_resize(width, height);
}

void rend_term_renderer(void)
{
	/* A new renderer has no picture, and the first render to each
	 * framebuffer starts from an empty one - unless the last picture was
	 * kept for it: then the game goes on where it was. */
	int w, h;
	if (last_picture(&w, &h) == NULL)
		fb_addr_history[0] = fb_addr_history[1] = 1;
	renderer_destroy();
	texcache_scratch_free();
}

void rend_prepare_renderer(void)
{
   if (renderer_changed || (!renderer_exists() && !renderer_failed))
   {
	  renderer_changed = false;
	  renderer_failed = false;
	  if (renderer_exists())
	  {
		 rend_term_renderer();
		 kept_picture_due = true;
	  }
	  rend_create_renderer();
	  rend_init_renderer();
   }
}

static bool rend_frame(TA_context* ctx, bool draw_osd)
{
   bool proc, do_swp;
#if !defined(TARGET_NO_THREADS)
   bool hold;
#endif
   (void)draw_osd;
   rend_prepare_renderer();
   if (!renderer_exists())
   {
      /* (none could be set up: nothing drawn, and the emulation let go on
       * as a frame that draws nothing lets it) */
#if !defined(TARGET_NO_THREADS)
      if (settings.rend.ThreadedRendering)
         emu_baton_ack_work(&emu_baton);
#endif
      return false;
   }
#if !defined(TARGET_NO_THREADS)
   /* The renderer reads the registers as they were when the game started
    * this render. */
   if (settings.rend.ThreadedRendering)
      rend_pvr_regs = ctx->regs;
#endif
   /* The palette this frame is drawn with, converted here, on the thread
    * that draws, from the registers the frame is drawn with. */
   palette_update();

   proc = renderer_process(ctx);
#if !defined(TARGET_NO_THREADS)
   /* Process() has read everything the frame takes from the game's memory:
    * the display lists, the textures, the framebuffer. The emulation thread
    * has been asleep since it handed the render over, so what was read is
    * what non-threaded rendering would have read; from here it can run
    * while the frame is drawn. A render to a texture ends up back in the
    * game's memory, so that one keeps the emulation asleep until it is
    * finished. */
   hold = proc && ctx->rend.isRTT;
   if (settings.rend.ThreadedRendering && !hold)
      emu_baton_ack_work(&emu_baton);
#endif

   do_swp = proc && renderer_render();
   /* a picture kept across a context is for the first render to the screen
    * after it, and that has now been made */
   if (!ctx->rend.isRTT)
      last_picture_drop();

#if !defined(TARGET_NO_THREADS)
   if (settings.rend.ThreadedRendering && hold)
      emu_baton_ack_work(&emu_baton);
#endif

   return do_swp;
}

/* Draw the render that is queued. Non-threaded rendering calls this where
 * the game starts the render; threaded rendering calls it on the libretro
 * thread for the render the emulation thread has handed over. Either way
 * it is the same code doing the same thing to the same data. */
bool rend_single_frame(void)
{
	for (;;)
	{
		bool do_swp;
		if (_pvrrc == NULL)
		{
			_pvrrc = DequeueRender();
			if (_pvrrc == NULL)
				return false;
		}

		do_swp = rend_frame(_pvrrc, true);

		/* clear up & free data .. */
		FinishRender(_pvrrc);
		_pvrrc = NULL;
		rend_pvr_regs = pvr_regs;

		if (do_swp)
			return true;
	}
}

void rend_skip_screen(bool skip)
{
   skip_screen = skip;
}

static int most(int a, int b)
{
   return a > b ? a : b;
}

void rend_start_render(void)
{
   TA_context* ctx;
#if !defined(TARGET_NO_THREADS)
   /* One render at a time: the libretro thread may still be drawing the
    * last one. Nothing is ever dropped to make room. */
   if (settings.rend.ThreadedRendering)
      emu_baton_wait_work_done(&emu_baton);
#endif
   render_called = true;
   pend_rend = false;
   ctx = tactx_Get(CORE_CURRENT_CTX);

   /* No end of render interrupt when rendering the framebuffer. (A frame
    * drawn and kept is drawn again, but the machine's timing is as for a
    * frame that is not there - as it was before frames were kept; a
    * state has it, and loading one changes nothing here.) */
	if (!ctx || !ctx->rend.isRenderFramebuffer)
		SetREP(ctx != NULL && !ctx->drawn ? (int)(ctx->tad.thd_data - ctx->tad.thd_root) : -1);

   if (ctx)
   {
      const bool is_rtt=(FB_W_SOF1& 0x1000000)!=0 && !ctx->rend.isRenderFramebuffer;

      /* A render to the screen may be where this run ends (hw/pvr/spg.cpp):
       * the picture is shown once it is drawn, below. Whether or not it is
       * drawn - the frontend may not show this frame - or the frame is
       * there to draw: where a run ends is the machine's to say alone. */
      const bool screen = !is_rtt && !ctx->rend.isRenderFramebuffer;

      /* The render passes this render draws. A frame the game may start
       * another render of, for others, stays in the list (rend.keep): the
       * one after can find it there, as can a state saved in between.
       * (What the last render of it overran is not this one's.) */
      if (ctx->rend.keep)
         ctx->rend.Overrun = false;
      ta_select_passes(ctx);
      if (!ctx->rend.keep)
         tactx_Remove(ctx);
      else
      {
         /* (no longer the frame being written, as one taken out is not) */
         if (ta_ctx == ctx)
            SetCurrentTARC(TACTX_NONE);
         ctx->drawn = true;
      }

      if (!ctx->rend.Overrun)
      {
         if (!ctx->rend.isRenderFramebuffer)
            FillBGP(ctx);

         ctx->rend.isRTT      = is_rtt;
         ctx->rend.clearFramebuffer = false;
         if (!is_rtt && !ctx->rend.isRenderFramebuffer
               && FB_W_SOF1 != fb_addr_history[0] && FB_W_SOF1 != fb_addr_history[1])
         {
            ctx->rend.clearFramebuffer = true;
            fb_addr_history[0] = fb_addr_history[1];
            fb_addr_history[1] = FB_W_SOF1;
         }
         if (skip_screen && !is_rtt && !ctx->rend.isRenderFramebuffer)
         {
            /* Not shown, so not drawn. The game did render: the run
             * reports no new picture, and the vblank does not take the
             * framebuffer for one either. */
            tactx_Drawn(ctx);
            spg_screen_render();
            return;
         }

         ctx->rend.fb_X_CLIP  = FB_X_CLIP;
         ctx->rend.fb_Y_CLIP  = FB_Y_CLIP;

         ctx->rend.fog_clamp_min = FOG_CLAMP_MIN;
			ctx->rend.fog_clamp_max = FOG_CLAMP_MAX;

         max_idx              = most(max_idx,  TA_LIST_USED(ctx->rend.idx, u32));
         max_vtx              = most(max_vtx,  TA_LIST_USED(ctx->rend.verts, Vertex));
         max_op               = most(max_op,   TA_LIST_USED(ctx->rend.global_param_op, PolyParam));
         max_pt               = most(max_pt,   TA_LIST_USED(ctx->rend.global_param_pt, PolyParam));
         max_tr               = most(max_tr,   TA_LIST_USED(ctx->rend.global_param_tr, PolyParam));

         max_mvo              = most(max_mvo,  TA_LIST_USED(ctx->rend.global_param_mvo, ModifierVolumeParam));
         max_modt             = most(max_modt, TA_LIST_USED(ctx->rend.modtrig, ModTriangle));

         if (QueueRender(ctx))
         {
#if !defined(TARGET_NO_THREADS)
            if (settings.rend.ThreadedRendering)
            {
               /* Hand the render to the libretro thread, with the registers
                * as they are now, and sleep until it has read what it needs
                * of the game's memory. */
               memcpy(ctx->regs, pvr_regs, pvr_RendRegSize);
               emu_baton_hand_over(&emu_baton, ctx);
            }
            else
#endif
            {
               /* This is the thread that runs the SH4, in the middle of
                * running it: the frame is drawn with the thread's own
                * floating point settings and not the game's, as it is
                * when a thread of its own draws it, and the game's are
                * put back after. */
               HostFloatModeBack();
               rend_single_frame();
               RestoreHostRoundingMode();
            }
            pend_rend = true;
            if (!is_rtt)
               frame_produced = true;
         }
      }
      else
      {
         ovrn++;
         INFO_LOG(PVR, "WARNING: Rendering context is overrun (%d), aborting frame\n", ovrn);
         tactx_Drawn(ctx);
      }
      if (screen)
         spg_screen_render();
   }
}

void rend_end_render(void)
{
   /* Non-threaded only: in threaded rendering this is the emulation thread,
    * which leaves the renderer alone. */
   if (pend_rend && !settings.rend.ThreadedRendering && renderer_exists())
   {
      /* (the SH4's thread, as in rend_start_render()) */
      HostFloatModeBack();
      renderer_present();
      RestoreHostRoundingMode();
   }
}

void rend_term(void)
{
}

void rend_vblank(void)
{
   /* A framebuffer the game wrote itself, with no render since the last
    * vblank, is shown: it is made a render here. */
   if (!render_called && fb_dirty && FB_R_CTRL.fb_enable)
	{
		u32 saved_ctx_addr;
		u32 current_ctx;
		DEBUG_LOG(PVR, "Direct framebuffer write detected\n");
		saved_ctx_addr = PARAM_BASE;
		/* The frame the game is sending lists to, which this borrows the
		 * place of: it is the one to go back to. (What was gone back to
		 * was the frame PARAM_BASE names - the one the game last had
		 * drawn, another one. A game in the middle of sending a frame's
		 * lists had the rest of them go there, and that frame drawn
		 * without them.) */
		current_ctx = ta_ctx != NULL ? ta_ctx->Address : TACTX_NONE;
		PARAM_BASE = 0xF00000;
		SetCurrentTARC(CORE_CURRENT_CTX);
		/* (NULL: there was not the memory for it, and nothing is shown) */
		if (ta_ctx != NULL)
		{
			tactx_reset_lists(ta_ctx);
			ta_ctx->rend.isRenderFramebuffer = true;
			ta_ctx->rend.isRTT = false;
			rend_start_render();
		}
		PARAM_BASE = saved_ctx_addr;
		if (current_ctx != TACTX_NONE)
			SetCurrentTARC(current_ctx);
		fb_dirty = false;
	}
	render_called = false;
	check_framebuffer_write();
	cheats_apply();
}

/* Whether this run made a picture to show: a render to the screen (or of
 * the framebuffer the game wrote), drawn. If not, the picture shown is the
 * one before, and the frontend is told the frame is a duplicate - otherwise
 * stale content (a 30fps title's repeated frame, a static loading screen)
 * is delivered as a fresh frame and pacing breaks. (A render to a texture
 * alone is no new picture.) Used by os_DoEvents to set is_dupe for the
 * frontend. One-shot: cleared on read. */
bool rend_frame_produced(void)
{
	bool r = frame_produced;
	frame_produced = false;
	return r;
}

/* The save state, from V22: whether the game has written to the
 * framebuffer itself since that was last shown, and where the framebuffer
 * was taken to be. A machine loaded from a state had these as the machine
 * before the load left them, and showed the framebuffer at the next
 * vblank, or did not, by that.
 *
 * Bit 1 of the first byte: a render has been started since the last
 * vblank (render_called), which the vblank goes by as well. A frame used
 * to end at the vblank, where that is never so; now it may end after the
 * game's render, before the next vblank (hw/pvr/spg.cpp), and a machine
 * loaded there showed the framebuffer the game wrote over the render, or
 * not, as the machine before the load had it - run ahead, the frame after
 * was not the one run straight through. A state from before has the bit
 * clear, as it was where it was saved. */
void pvr_serialize_v22(void **data, unsigned int *total_size)
{
	u8 dirty = (fb_dirty ? 1 : 0) | (render_called ? 2 : 0);

	LIBRETRO_S(dirty);
	LIBRETRO_S(fb_watch_addr_start);
	LIBRETRO_S(fb_watch_addr_end);
}

void pvr_unserialize_v22(void **data, unsigned int *total_size)
{
	u8 dirty = 0;

	LIBRETRO_US(dirty);
	LIBRETRO_US(fb_watch_addr_start);
	LIBRETRO_US(fb_watch_addr_end);
	fb_dirty = (dirty & 1) != 0;
	render_called = (dirty & 2) != 0;
}

/* A state of before V22 does not say: nothing written, and the framebuffer
 * where the registers have it. */
void pvr_state_before_v22(void)
{
	fb_dirty = false;
	render_called = false;
	check_framebuffer_write();
}

void check_framebuffer_write(void)
{
   u32 fb_size = (FB_R_SIZE.fb_y_size + 1) * (FB_R_SIZE.fb_x_size + FB_R_SIZE.fb_modulus) * 4;
	fb_watch_addr_start = (SPG_CONTROL.interlace ? FB_R_SOF2 : FB_R_SOF1) & VRAM_MASK;
	fb_watch_addr_end = fb_watch_addr_start + fb_size;
}
