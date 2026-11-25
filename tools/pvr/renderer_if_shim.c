/* The renderers renderer_if_test.c has made: each call they get said into
 * its hash, init() answering as it is told (fake_init_ok: 1 yes, 0 no, 2
 * yes for the one to fall back to only). One of each kind at a time, made
 * in create() and given back in release(). */
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"

void mix(u32 v);
extern int fake_init_ok;

static u32 made;

typedef struct fake_renderer
{
	u32 id;
	bool fallback;
} fake_renderer;

static void fake_create(fake_renderer *r, bool fallback)
{
	r->id = made++;
	r->fallback = fallback;
	mix(0x200); mix(r->id);
}
static void fake_release(fake_renderer *r) { mix(0x201); mix(r->id); }
static bool fake_init(fake_renderer *r)
{
	mix(0x202); mix(r->id);
	return fake_init_ok == 1 || (fake_init_ok == 2 && r->fallback);
}
static void fake_resize(fake_renderer *r, int w, int h) { mix(0x203); mix(r->id); mix((u32)w); mix((u32)h); }
static void fake_term(fake_renderer *r) { mix(0x204); mix(r->id); }
static bool fake_process(fake_renderer *r, TA_context *ctx)
{
	mix(0x205); mix(r->id); mix(ctx->Address);
	return (ctx->Address & 0x100000) == 0 || r->id % 3 != 0;
}
static bool fake_render(fake_renderer *r) { mix(0x206); mix(r->id); return r->id % 4 != 1; }
static void fake_present(fake_renderer *r) { mix(0x207); mix(r->id); }
static void fake_keep_picture(fake_renderer *r) { mix(0x208); mix(r->id); }
static bool fake_present_kept_picture(fake_renderer *r) { mix(0x209); mix(r->id); return r->id % 2 == 0; }
static u64 fake_get_texture(u32 tsp, u32 tcw, int area) { (void)tsp; (void)tcw; (void)area; return 0; }
static void fake_options_changed(void) { }
static bool fake_ready(void) { return true; }

/* The renderer chosen, and the one to fall back to: each its own */
#define FAKE(NAME, F) \
	static fake_renderer NAME##_r; \
	static void NAME##_create(void) { fake_create(&NAME##_r, F); } \
	static void NAME##_release(void) { fake_release(&NAME##_r); } \
	static bool NAME##_init(void) { return fake_init(&NAME##_r); } \
	static void NAME##_resize(int w, int h) { fake_resize(&NAME##_r, w, h); } \
	static void NAME##_term(void) { fake_term(&NAME##_r); } \
	static bool NAME##_process(TA_context *ctx) { return fake_process(&NAME##_r, ctx); } \
	static bool NAME##_render(void) { return fake_render(&NAME##_r); } \
	static void NAME##_keep_picture(void) { fake_keep_picture(&NAME##_r); } \
	static void NAME##_present(void) { fake_present(&NAME##_r); } \
	static bool NAME##_present_kept_picture(void) { return fake_present_kept_picture(&NAME##_r); } \
	static const renderer_ops NAME##_ops = { \
		NAME##_init, NAME##_resize, NAME##_term, NAME##_process, NAME##_render, \
		NAME##_keep_picture, fake_get_texture, fake_options_changed, \
		NAME##_create, NAME##_release, NAME##_present, \
		NAME##_present_kept_picture, fake_ready \
	}; \
	const renderer_ops *rend_##NAME(void) { return &NAME##_ops; }

FAKE(GLES2, true)
#if defined(HAVE_GL4)
FAKE(GL4, false)
#endif
#ifdef HAVE_VULKAN
FAKE(Vulkan, false)
FAKE(OITVulkan, false)
#endif
#ifdef HAVE_D3D11
FAKE(D3D11, true)
FAKE(OITD3D11, false)
#endif
#ifdef HAVE_D3D9
FAKE(D3D9, false)
#endif
#ifdef HAVE_D3D12
FAKE(D3D12, true)
FAKE(OITD3D12, false)
#endif
