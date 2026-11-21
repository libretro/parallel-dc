/* The C++ renderers' class, and one of them as the core's renderer_ops
 * (hw/pvr/Renderer_if.h): RendererClass<R>::ops makes an R in create()
 * and gives it back in release(), one R at a time. */
#pragma once
#include "hw/pvr/Renderer_if.h"

struct Renderer
{
	virtual ~Renderer() = default;

	virtual bool Init() = 0;
	virtual void Resize(int w, int h) = 0;
	virtual void Term() = 0;
	virtual bool Process(TA_context *ctx) = 0;
	virtual bool Render() = 0;
	/* Presentation only: hardware renderers present via video_cb in
	 * retro_run, control coming back there at the vblank boundary in
	 * os_DoEvents(), never here */
	virtual void Present() { }
	/* The context is about to be destroyed: put the screen's last picture
	 * in rend/last_picture.h. The renderer's first render to the screen
	 * after that starts from what is there. */
	virtual void KeepPicture() { }
	/* renderer_ops' present_kept_picture */
	virtual bool PresentKeptPicture() { return false; }
	/* renderer_ready()'s */
	virtual bool Ready() { return true; }
	/* @area: 1 for the texture of a polygon's second volume */
	virtual u64 GetTexture(TSP tsp, TCW tcw, int area = 0) { return 0; }
	/* renderer_options_changed()'s */
	virtual void OptionsChanged() { }
};

template <class R>
struct RendererClass
{
	static Renderer *r;

	static void create() { r = new R(); }
	static void release() { delete r; r = nullptr; }
	static bool init() { return r->Init(); }
	static void resize(int w, int h) { r->Resize(w, h); }
	static void term() { r->Term(); }
	static bool process(TA_context *ctx) { return r->Process(ctx); }
	static bool render() { return r->Render(); }
	static void keep_picture() { r->KeepPicture(); }
	static u64 get_texture(u32 tsp, u32 tcw, int area)
	{
		TSP t;
		TCW c;
		t.full = tsp;
		c.full = tcw;
		return r->GetTexture(t, c, area);
	}
	static void options_changed() { r->OptionsChanged(); }
	static void present() { r->Present(); }
	static bool present_kept_picture() { return r->PresentKeptPicture(); }
	static bool ready() { return r->Ready(); }

	static const renderer_ops ops;
};

template <class R> Renderer *RendererClass<R>::r;
template <class R> const renderer_ops RendererClass<R>::ops =
{
	init,
	resize,
	term,
	process,
	render,
	keep_picture,
	get_texture,
	options_changed,
	create,
	release,
	present,
	present_kept_picture,
	ready
};
