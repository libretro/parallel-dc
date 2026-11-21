/* The renderers renderer_if_test.c has made: each call they get said into
 * its hash, Init() answering as it is told (fake_init_ok: 1 yes, 0 no, 2
 * yes for the one to fall back to only), as the core's C++ renderers are
 * (RendererClass). */
#include "rend/renderer_class.h"

extern "C" {
void mix(u32 v);
extern int fake_init_ok;
}

static u32 made;

struct FakeRenderer : Renderer
{
	u32 id;
	bool fallback;
	explicit FakeRenderer(bool fallback) : id(made++), fallback(fallback) { mix(0x200); mix(id); }
	~FakeRenderer() { mix(0x201); mix(id); }
	bool Init() override
	{
		mix(0x202); mix(id);
		return fake_init_ok == 1 || (fake_init_ok == 2 && fallback);
	}
	void Resize(int w, int h) override { mix(0x203); mix(id); mix((u32)w); mix((u32)h); }
	void Term() override { mix(0x204); mix(id); }
	bool Process(TA_context *ctx) override { mix(0x205); mix(id); mix(ctx->Address); return (ctx->Address & 0x100000) == 0 || id % 3 != 0; }
	bool Render() override { mix(0x206); mix(id); return id % 4 != 1; }
	void Present() override { mix(0x207); mix(id); }
	void KeepPicture() override { mix(0x208); mix(id); }
	bool PresentKeptPicture() override { mix(0x209); mix(id); return id % 2 == 0; }
};

/* (the renderer chosen, and the one to fall back to: its own class) */
template <int K, bool F> struct Fake : FakeRenderer { Fake() : FakeRenderer(F) { } };

const renderer_ops *rend_GLES2(void) { return &RendererClass<Fake<0, true> >::ops; }
#if defined(HAVE_GL4)
const renderer_ops *rend_GL4(void) { return &RendererClass<Fake<3, false> >::ops; }
#endif
#ifdef HAVE_VULKAN
const renderer_ops *rend_Vulkan(void) { return &RendererClass<Fake<4, false> >::ops; }
const renderer_ops *rend_OITVulkan(void) { return &RendererClass<Fake<5, false> >::ops; }
#endif
#ifdef HAVE_D3D11
const renderer_ops *rend_D3D11(void) { return &RendererClass<Fake<6, true> >::ops; }
const renderer_ops *rend_OITD3D11(void) { return &RendererClass<Fake<7, false> >::ops; }
#endif
#ifdef HAVE_D3D9
const renderer_ops *rend_D3D9(void) { return &RendererClass<Fake<8, false> >::ops; }
#endif
#ifdef HAVE_D3D12
const renderer_ops *rend_D3D12(void) { return &RendererClass<Fake<9, true> >::ops; }
const renderer_ops *rend_OITD3D12(void) { return &RendererClass<Fake<10, false> >::ops; }
#endif

#ifdef RENDERER_IF_TEST_OLD
/* (the old Renderer_if.cpp's, of C++ linkage) */
void texcache_scratch_free() { mix(0x103); }
bool rend_single_frame();
extern "C" bool old_rend_single_frame(void) { return rend_single_frame(); }
#endif
