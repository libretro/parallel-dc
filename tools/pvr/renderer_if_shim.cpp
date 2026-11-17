/* The renderers renderer_if_test.c has made: each call they get said into
 * its hash, Init() answering as it is told (fake_init_ok: 1 yes, 0 no, 2
 * yes for the one to fall back to only). */
#include "hw/pvr/Renderer_if.h"

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
Renderer *rend_GLES2() { return new FakeRenderer(true); }
#if defined(HAVE_GL4)
Renderer *rend_GL4() { return new FakeRenderer(false); }
#endif
#ifdef HAVE_VULKAN
Renderer *rend_Vulkan() { return new FakeRenderer(false); }
Renderer *rend_OITVulkan() { return new FakeRenderer(false); }
#endif
#ifdef HAVE_D3D11
Renderer *rend_D3D11() { return new FakeRenderer(true); }
Renderer *rend_OITD3D11() { return new FakeRenderer(false); }
#endif
#ifdef HAVE_D3D9
Renderer *rend_D3D9() { return new FakeRenderer(false); }
#endif
#ifdef HAVE_D3D12
Renderer *rend_D3D12() { return new FakeRenderer(true); }
Renderer *rend_OITD3D12() { return new FakeRenderer(false); }
#endif

#ifdef RENDERER_IF_TEST_OLD
/* (the old Renderer_if.cpp's, of C++ linkage) */
void texcache_scratch_free() { mix(0x103); }
bool rend_single_frame();
extern "C" bool old_rend_single_frame(void) { return rend_single_frame(); }
#endif
