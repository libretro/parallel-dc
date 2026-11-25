/* The CPU's side of texdecode_test.c: the texture cache itself
 * (core/rend/TexCache.o) making a texture, with what it would hand a
 * renderer that converts on the GPU caught on the way.
 *
 * A texture is made with texcache_create() and texcache_update(), as the
 * renderers make them. texcache_update() first offers the texture to
 * gpu_decode, which takes the plan and says no; then converts it on the
 * CPU and hands the texels to upload_to_gpu, which takes those. So one
 * update gives both, from the same state. */
/* the registers the frame is rendered with, as the texture cache reads them */
#define PVR_REGS_FOR_RENDERER
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "rend/TexCache.h"
#include "rend/rtt_watch.h"
#include "rend/texdecode.h"
#include "hw/pvr/pvr_regs.h"
#include "texdecode_case.h"
#include "hw/mem/_vmem.h"

/* what the texture cache takes from the rest of the core */
settings_t settings;
u32 FrameCount = 1;
unsigned VRAM_SIZE = 8 * 1024 * 1024;
unsigned VRAM_MASK = 8 * 1024 * 1024 - 1;
static u8 pvr_regs_mem[0x10000];
u8 *rend_pvr_regs = pvr_regs_mem;
void _vmem_protect_vram(u32 addr, u32 size) { (void)addr; (void)size; }
void _vmem_unprotect_vram(u32 addr, u32 size) { (void)addr; (void)size; }
u32 _vmem_get_vram_offset(void *addr) { (void)addr; return (u32)-1; }
bool _vmem_vram_page_protected(u32 page) { (void)page; return false; }
void os_DebugBreak(void) { abort(); }
/* (no log) */
retro_log_printf_t log_cb;
/* (no render to a texture is watched here: rend/rtt_watch.h) */
bool rtt_watch_pending(u32 addr, u32 size, RttWatch *watch) { (void)addr; (void)size; (void)watch; return false; }
void rtt_watch_overwrite(u32 addr, u32 size) { (void)addr; (void)size; }
extern VArray2 vram;

typedef struct test_texture
{
	texcache_data base;
	bool force32;
	bool planned;
	struct texd_plan plan;
	u8 *out;
	size_t cap, bytes;
} test_texture;

static void tt_get_id(texcache_data *t, char *s, size_t n) { (void)t; snprintf(s, n, "test"); }
static void tt_upload_to_gpu(texcache_data *t, int width, int height, u8 *data, bool mipmapped, bool mipmaps_included)
{
	test_texture *tt = (test_texture *)t;
	const size_t pixel = t->tex_type == TEX_TYPE_8888 ? 4 : t->tex_type == TEX_TYPE_8 ? 1 : 2;
	size_t n = (size_t)width * height * pixel;
	int w;
	(void)mipmapped;
	if (mipmaps_included)
		for (w = width / 2; w != 0; w /= 2)
			n += (size_t)w * w * pixel;
	tt->bytes = n <= tt->cap ? n : 0;
	if (tt->bytes != 0)
		memcpy(tt->out, data, n);
}
static void *tt_upload_memory(texcache_data *t, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	(void)t; (void)width; (void)height; (void)bytes; (void)pixel; (void)mipmaps;
	return NULL;
}
static bool tt_gpu_decode(texcache_data *t, const struct texd_plan *p)
{
	test_texture *tt = (test_texture *)t;
	tt->plan = *p;
	tt->planned = true;
	return false;
}
static bool tt_part_memory(texcache_data *t, size_t bytes, void **memory) { (void)t; (void)bytes; (void)memory; return false; }
static void tt_upload_part(texcache_data *t, const TexRegion *regions, u32 count, u8 *data, const size_t *offset)
{
	(void)t; (void)regions; (void)count; (void)data; (void)offset;
}
static int tt_gpu_decode_part(texcache_data *t, const struct texd_plan *p, const TexRegion *regions, u32 count)
{
	(void)t; (void)p; (void)regions; (void)count;
	return TEXP_CPU;
}
static bool tt_copy_render(texcache_data *t, texcache_data *from, u32 x, u32 y, u32 scale, u32 columns)
{
	(void)t; (void)from; (void)x; (void)y; (void)scale; (void)columns;
	return false;
}
static bool tt_force_32bit(const texcache_data *t, int type) { (void)type; return ((const test_texture *)t)->force32; }
static bool tt_del(texcache_data *t) { return texcache_delete(t); }

static const texcache_ops tt_ops =
{
	tt_get_id, tt_upload_to_gpu, tt_upload_memory, tt_gpu_decode, tt_part_memory, tt_upload_part,
	tt_gpu_decode_part, tt_copy_render, tt_force_32bit, tt_del
};

void texd_cpu_init(void *video_memory)
{
	vram.data = (u8 *)video_memory;
	vram.size = VRAM_SIZE;
	settings.rend.TextureUpscale = 1;
	settings.rend.MaxFilteredTextureSize = 256;
	texconv_init();	/* (the twiddle tables: libPvr_Init()'s) */
	settings.rend.GpuTextureDecode = true;
}

int texd_cpu_run(const struct texd_case *c, struct texd_plan *plan, void *out, size_t cap, size_t *bytes, int *type)
{
	test_texture t;
	u32 tcw = 0, tsp = 0;

	memset(&t, 0, sizeof(t));
	t.base.ops = &tt_ops;
	tcw = TAW_SET(tcw, TCW_TexAddr, c->addr >> 3);
	tcw = TAW_SET(tcw, TCW_PixelFmt, c->fmt);
	tcw = TAW_SET(tcw, TCW_ScanOrder, c->scan_order);
	tcw = TAW_SET(tcw, TCW_VQ_Comp, c->vq);
	tcw = TAW_SET(tcw, TCW_MipMapped, c->mip);
	tcw = TAW_SET(tcw, TCW_StrideSel, c->stride_sel);
	if (c->fmt == TA_PIXEL_4BPP || c->fmt == TA_PIXEL_8BPP)
		tcw = TAW_SET(tcw, TCW_PalSelect, c->pal_select);	/* (over the bits above it: as the hardware has it) */
	tsp = TAW_SET(tsp, TSP_TexU, c->tex_u);
	tsp = TAW_SET(tsp, TSP_TexV, c->tex_v);
	tsp = TAW_SET(tsp, TSP_FilterMode, c->filter);
	t.base.tcw = tcw;
	t.base.tsp = tsp;
	t.base.area = 0;
	t.force32 = c->force32 != 0;
	settings.rend.ForceTextureLOD0 = c->lod0 != 0;
	TEXT_CONTROL = c->text_control;
	PAL_RAM_CTRL = c->pal_ctrl;
	palette_update();
	t.out = (u8 *)out;
	t.cap = cap;

	texcache_create(&t.base);
	texcache_update(&t.base);
	texcache_delete(&t.base);
	*bytes = t.bytes;
	*type = (int)t.base.tex_type;
	if (t.planned)
		*plan = t.plan;
	return t.planned;
}

/* For the palette: the registers' memory */
void *texd_cpu_palette_ram(void)
{
	return PALETTE_RAM;
}
