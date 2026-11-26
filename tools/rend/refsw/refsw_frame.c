/* A frame of this core's, drawn by the reference software renderer
 * (refsw.c, refsw_pixel.c) the way the chip draws it: tile by tile,
 * the opaque and punch-through lists into the depth and tag buffers, the
 * modifier volumes into the stencil, the tags shaded, then the translucent
 * list peeled a layer a pass from the back. The frame is a dump of what the
 * TA parse left (refsw_dump.h, written by tools/rend/tile_dump.c).
 *
 *   refsw_frame FRAME.bin OUT.ppm [PROBE...]
 *
 * OUT.ppm is the picture, 8 bits a channel (the chip writes 16-bit 565 to
 * its framebuffer: the picture is what it has before that). Each PROBE is
 * "X,Y": for that pixel, every triangle that covers it is printed as it is
 * rasterized, with the depth (1/w) the renderer interpolates for it and the
 * depth, tag and stencil the tile buffers hold after the list, so that what
 * the chip compares at a pixel can be read off.
 *
 * This file is the part of libswirl's refrend_base.cpp that walked the
 * chip's object lists in video memory, written again over the parse's
 * lists (this core's TA keeps the polygons in its own lists and writes no
 * object lists): the sequence of operations a tile goes through is that
 * file's RenderPVR(), step for step. The triangles themselves are
 * rasterized and shaded by refsw.c and refsw_pixel.c.
 *
 * Textures are made by this core's texture cache (core/rend/TexCache.o is
 * linked in) and laid out as libswirl's software renderers want them. The
 * background polygon is read from video memory as the chip reads it. */
/* the registers the frame is drawn with, as the texture cache reads them */
#define PVR_REGS_FOR_RENDERER
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "../../pvr/ta_structs.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/TexCache.h"
#include "rend/rtt_watch.h"
#include "refsw_dump.h"
#include "refsw.h"
#include "refsw_pixel.h"
#include "rend/tile/pvr_scaler.h"
#include "hw/mem/_vmem.h"

/* what the texture cache takes from the rest of the core */
settings_t settings;
u32 FrameCount = 1;
unsigned VRAM_SIZE = 8 * 1024 * 1024;
unsigned VRAM_MASK = 8 * 1024 * 1024 - 1;
u8 *rend_pvr_regs;
void _vmem_protect_vram(u32 addr, u32 size) { (void)addr; (void)size; }
void _vmem_unprotect_vram(u32 addr, u32 size) { (void)addr; (void)size; }
u32 _vmem_get_vram_offset(void *addr) { (void)addr; return (u32)-1; }
bool _vmem_vram_page_protected(u32 page) { (void)page; return false; }
void os_DebugBreak(void) { abort(); }
/* (no log) */
retro_log_printf_t log_cb;
/* (no render to a texture is watched here: a texture over one is made from
 * video memory, as any other) */
bool rtt_watch_pending(u32 addr, u32 size, RttWatch *watch) { (void)addr; (void)size; (void)watch; return false; }
void rtt_watch_overwrite(u32 addr, u32 size) { (void)addr; (void)size; }
extern VArray2 vram;
u32 DYNACALL pvr_read_area1_32(u32 addr) { return *(u32 *)&vram.data[addr & VRAM_MASK]; }
u16 DYNACALL pvr_read_area1_16(u32 addr) { return *(u16 *)&vram.data[addr & VRAM_MASK]; }

static u8 *vram_bytes;
u32 vri(u32 addr) { return *(u32 *)&vram_bytes[addr & VRAM_MASK]; }
f32 vrf(u32 addr) { return *(f32 *)&vram_bytes[addr & VRAM_MASK]; }

/* (std::min and std::max of floats, as the C++ took them) */
static float fmin2(float a, float b) { return b < a ? b : a; }
static float fmax2(float a, float b) { return a < b ? b : a; }

/*
 * Textures
 */
typedef struct RefTexture
{
	texcache_data base;
	u8 *texels;	/* the top level, RGBA as the cache makes it */
	u32 got_w, got_h;
} RefTexture;

static void ref_get_id(texcache_data *t, char *s, size_t n) { (void)t; snprintf(s, n, "refsw"); }
static void ref_upload_to_gpu(texcache_data *base, int width, int height, u8 *data, bool mipmapped, bool mipmaps_included)
{
	RefTexture *t = (RefTexture *)base;
	(void)mipmapped; (void)mipmaps_included;
	if (base->tex_type != TEX_TYPE_8888)
		return;
	free(t->texels);
	t->texels = (u8 *)malloc((size_t)width * height * 4);
	memcpy(t->texels, data, (size_t)width * height * 4);
	t->got_w = width;
	t->got_h = height;
}
static void *ref_upload_memory(texcache_data *t, u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	(void)t; (void)width; (void)height; (void)bytes; (void)pixel; (void)mipmaps;
	return NULL;
}
static bool ref_gpu_decode(texcache_data *t, const struct texd_plan *plan) { (void)t; (void)plan; return false; }
static bool ref_part_memory(texcache_data *t, size_t bytes, void **memory) { (void)t; (void)bytes; (void)memory; return false; }
static void ref_upload_part(texcache_data *t, const TexRegion *regions, u32 count, u8 *data, const size_t *offset)
{
	(void)t; (void)regions; (void)count; (void)data; (void)offset;
}
static int ref_gpu_decode_part(texcache_data *t, const struct texd_plan *plan, const TexRegion *regions, u32 count)
{
	(void)t; (void)plan; (void)regions; (void)count;
	return TEXP_CPU;
}
static bool ref_copy_render(texcache_data *t, texcache_data *from, u32 x, u32 y, u32 scale, u32 columns)
{
	(void)t; (void)from; (void)x; (void)y; (void)scale; (void)columns;
	return false;
}
static bool ref_force_32bit(const texcache_data *t, int type) { (void)t; (void)type; return true; }
static bool ref_del(texcache_data *t) { return texcache_delete(t); }

static const texcache_ops ref_ops =
{
	ref_get_id, ref_upload_to_gpu, ref_upload_memory, ref_gpu_decode, ref_part_memory, ref_upload_part,
	ref_gpu_decode_part, ref_copy_render, ref_force_32bit, ref_del
};

/* the textures made, by their words */
typedef struct texture_made
{
	u64 key;
	text_info info;
} texture_made;
static texture_made *textures;
static u32 textures_count, textures_room;

/* The texture a polygon draws with, as refsw's TextureFetch reads it: 16
 * bytes a texel, the 2x2 block of (u+1,v+1), (u,v+1), (u+1,v), (u,v) in
 * 32-bit BGRA, the neighbours wrapping at the edges (libswirl's
 * raw_GetTexture laid it out like this for its software renderers). The
 * largest level only (settings.rend.ForceTextureLOD0): refsw has no
 * mipmapping. */
text_info raw_GetTexture(u8 *vram_unused, TSP tsp, TCW tcw)
{
	const u64 key = ((u64)tcw.full << 32) | (tsp.full & 0x3F);	/* the format and address, and the size */
	RefTexture t;
	text_info info;
	u32 w, h, x, y, i;
	u8 *blocks;
	static u8 white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	const u8 *texels;
	(void)vram_unused;

	for (i = 0; i < textures_count; i++)
		if (textures[i].key == key)
			return textures[i].info;
	memset(&t, 0, sizeof(t));
	t.base.ops = &ref_ops;
	t.base.tcw = tcw.full;
	t.base.tsp = tsp.full;
	t.base.area = 0;
	texcache_create(&t.base);
	texcache_update(&t.base);
	texcache_delete(&t.base);
	memset(&info, 0, sizeof(info));
	texels = t.texels;
	if (t.got_w == 0 || t.got_h == 0)
	{
		fprintf(stderr, "refsw_frame: texture %08x/%08x not converted (type %d)\n", tcw.full, tsp.full, (int)t.base.tex_type);
		/* a 1x1 white texture, so that the polygon is still drawn */
		t.got_w = t.got_h = 1;
		texels = white;
	}
	w = t.got_w;
	h = t.got_h;
	if (getenv("REFSW_TEXTURES") != NULL)
	{
		u32 sum = 0;
		size_t k;
		for (k = 0; k < (size_t)w * h * 4; k++)
			sum += texels[k];
		fprintf(stderr, "refsw_frame: texture tcw %08x tsp %08x: %u x %u, format %s, type %d, sum %u, first %02x%02x%02x%02x\n", tcw.full, tsp.full, w, h,
				texcache_pixel_format_name(&t.base), (int)t.base.tex_type, sum, texels[0], texels[1], texels[2], texels[3]);
	}
	blocks = (u8 *)malloc((size_t)w * h * 16);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			u32 *out = (u32 *)(blocks + ((size_t)y * w + x) * 16);
			const u32 x1 = (x + 1) % w, y1 = (y + 1) % h;
			u32 at[4][2];
			int k;
			at[0][0] = x1; at[0][1] = y1;
			at[1][0] = x; at[1][1] = y1;
			at[2][0] = x1; at[2][1] = y;
			at[3][0] = x; at[3][1] = y;
			for (k = 0; k < 4; k++)
			{
				const u8 *p = &texels[((size_t)at[k][1] * w + at[k][0]) * 4];
				out[k] = (u32)p[2] | ((u32)p[1] << 8) | ((u32)p[0] << 16) | ((u32)p[3] << 24);	/* RGBA to BGRA */
			}
		}
	free(t.texels);
	info.width = w;
	info.height = h;
	info.pdata = blocks;
	info.textype = 0;
	if (textures_count == textures_room)
	{
		textures_room = textures_room ? textures_room * 2 : 64;
		textures = (texture_made *)realloc(textures, textures_room * sizeof(texture_made));
	}
	textures[textures_count].key = key;
	textures[textures_count].info = info;
	textures_count++;
	return info;
}

static void textures_clear(void)
{
	u32 i;
	for (i = 0; i < textures_count; i++)
		free(textures[i].info.pdata);
	free(textures);
	textures = NULL;
	textures_count = textures_room = 0;
}

/*
 * The frame
 */
typedef struct Frame
{
	struct refsw_dump_header h;
	u8 *regs, *vram;
	Vertex *verts;
	u32 *idx;
	PolyParam *op, *pt, *tr;
	ModTriangle *modtrig;
	ModifierVolumeParam *mvo, *mvo_tr;
	RenderPass *passes;
	u32 *region;
	u32 n_passes;
} Frame;

/* @n elements of @size bytes (this program's @want) into @out */
static bool get(FILE *f, void **out, u32 n, u32 size, size_t want)
{
	if (size != want)
	{
		fprintf(stderr, "refsw_frame: the dump's structures are %u bytes, this program's %u\n", size, (unsigned)want);
		return false;
	}
	*out = malloc(n ? (size_t)n * want : 1);
	return n == 0 || fread(*out, want, n, f) == n;
}

static bool load(const char *path, Frame *fr)
{
	FILE *f = fopen(path, "rb");
	bool ok;
	if (f == NULL)
	{
		perror(path);
		return false;
	}
	ok = fread(&fr->h, sizeof(fr->h), 1, f) == 1 && fr->h.magic == REFSW_DUMP_MAGIC && fr->h.version == REFSW_DUMP_VERSION;
	if (ok)
	{
		fr->regs = (u8 *)calloc(fr->h.regs_size + 1, 1);
		fr->vram = (u8 *)calloc(fr->h.vram_size + 1, 1);
		ok = fread(fr->regs, 1, fr->h.regs_size, f) == fr->h.regs_size
			&& fread(fr->vram, 1, fr->h.vram_size, f) == fr->h.vram_size
			&& get(f, (void **)&fr->verts, fr->h.n_verts, fr->h.sizeof_vertex, sizeof(Vertex))
			&& get(f, (void **)&fr->idx, fr->h.n_idx, 4, sizeof(u32))
			&& get(f, (void **)&fr->op, fr->h.n_op, fr->h.sizeof_polyparam, sizeof(PolyParam))
			&& get(f, (void **)&fr->pt, fr->h.n_pt, fr->h.sizeof_polyparam, sizeof(PolyParam))
			&& get(f, (void **)&fr->tr, fr->h.n_tr, fr->h.sizeof_polyparam, sizeof(PolyParam))
			&& get(f, (void **)&fr->modtrig, fr->h.n_modtrig, fr->h.sizeof_modtriangle, sizeof(ModTriangle))
			&& get(f, (void **)&fr->mvo, fr->h.n_mvo, fr->h.sizeof_mvparam, sizeof(ModifierVolumeParam))
			&& get(f, (void **)&fr->mvo_tr, fr->h.n_mvo_tr, fr->h.sizeof_mvparam, sizeof(ModifierVolumeParam))
			&& get(f, (void **)&fr->passes, fr->h.n_passes, fr->h.sizeof_renderpass, sizeof(RenderPass))
			&& get(f, (void **)&fr->region, fr->h.n_region_tiles, 4, sizeof(u32));
		fr->n_passes = fr->h.n_passes;
	}
	fclose(f);
	if (!ok)
		fprintf(stderr, "refsw_frame: %s is not a frame this program reads\n", path);
	return ok;
}

/*
 * The walk
 */
typedef struct Probe
{
	int x, y;
} Probe;
static Probe *probes;
static int n_probes;
static int cur_pass;
static const char *cur_list;

/* refsw's RasterizeTriangle() decides whether a pixel is in the triangle
 * with these half-edge functions at the pixel's sampling position; the same
 * here, so that a probe reports the triangles refsw shades at the pixel. */
static bool covers(const Vertex *v1, const Vertex *v2, const Vertex *v3, float x_ps, float y_ps)
{
	const float X1 = v1->x, X2 = v2->x, X3 = v3->x, Y1 = v1->y, Y2 = v2->y, Y3 = v3->y;
	int sgn = 1;
	float area, DX12, DX23, DX31, DY12, DY23, DY31, C1, C2, C3, Xhs12, Xhs23, Xhs31;
	if (refsw_far3(v1, v2, v3))
	{
		const double darea = ((double)X1 - X3) * ((double)Y2 - Y3) - ((double)Y1 - Y3) * ((double)X2 - X3);
		float X[3], Y[3];
		RefswExactEdges e;
		if ((float)darea > 0)
			sgn = -1;
		X[0] = X1; X[1] = X2; X[2] = X3;
		Y[0] = Y1; Y[1] = Y2; Y[2] = Y3;
		RefswExactEdges_Setup(&e, sgn, X, Y, 3);
		return RefswExactEdges_In(&e, x_ps, y_ps);
	}
	area = ((X1 - X3) * (Y2 - Y3) - (Y1 - Y3) * (X2 - X3));
	if (area > 0)
		sgn = -1;
	DX12 = sgn * (X1 - X2); DX23 = sgn * (X2 - X3); DX31 = sgn * (X3 - X1);
	DY12 = sgn * (Y1 - Y2); DY23 = sgn * (Y2 - Y3); DY31 = sgn * (Y3 - Y1);
	C1 = DY12 * X1 - DX12 * Y1; C2 = DY23 * X2 - DX23 * Y2; C3 = DY31 * X3 - DX31 * Y3;
	Xhs12 = C1 + DX12 * y_ps - DY12 * x_ps;
	Xhs23 = C2 + DX23 * y_ps - DY23 * x_ps;
	Xhs31 = C3 + DX31 * y_ps - DY31 * x_ps;
	return Xhs12 >= 0 && Xhs23 >= 0 && Xhs31 >= 0;
}

static void probe_triangle(const taRECT *rect, const DrawParameters *params, parameter_tag_t tag, const char *what, u32 index, u32 tri,
		const Vertex *v1, const Vertex *v2, const Vertex *v3)
{
	float halfpixel;
	int i;
	if (n_probes == 0)
		return;
	halfpixel = HALF_OFFSET.fpu_pixel_half_offset ? 0.5f : 0;
	for (i = 0; i < n_probes; i++)
	{
		const int px = probes[i].x, py = probes[i].y;
		float x_ps, y_ps, invW;
		PlaneStepper3 Z;
		union { float f; u32 u; } b;
		if (px < rect->left || px >= rect->right || py < rect->top || py >= rect->bottom)
			continue;
		x_ps = px + halfpixel;
		y_ps = py + halfpixel;
		if (!covers(v1, v2, v3, x_ps, y_ps))
			continue;
		PlaneStepper3_Setup(&Z, v1, v2, v3, v1->z, v2->z, v3->z);
		invW = PlaneStepper3_Ip(&Z, x_ps, y_ps);
		b.f = invW;
		printf("probe %d,%d: pass %d %s #%u tri %u tag %u: 1/w %.9g (0x%08x) w %.6f; depth mode %u zwrite %s cull %u tex %u offs %u gouraud %u; "
				"tsp %08x (src %u dst %u usealpha %u fog %u) tcw %08x; z %.9g %.9g %.9g\n",
				px, py, cur_pass, what, index, tri, tag, invW, b.u, 1.0 / invW,
				params->isp.DepthMode, params->isp.ZWriteDis ? "off" : "on", params->isp.CullMode, params->isp.Texture, params->isp.Offset, params->isp.Gouraud,
				params->tsp.full, params->tsp.SrcInstr, params->tsp.DstInstr, params->tsp.UseAlpha, params->tsp.FogCtrl, params->tcw.full, v1->z, v2->z, v3->z);
	}
}

static void probe_buffers(refsw *backend, const taRECT *rect, const char *when)
{
	const u32 *rb;
	int i;
	if (n_probes == 0)
		return;
	rb = (const u32 *)refsw_DebugGetAllBuffers(backend);
	for (i = 0; i < n_probes; i++)
	{
		const int px = probes[i].x, py = probes[i].y;
		u32 at, tag, stencil, accum;
		union { float f; u32 u; } d1, d2;
		if (px < rect->left || px >= rect->right || py < rect->top || py >= rect->bottom)
			continue;
		at = (py - rect->top) * MAX_RENDER_WIDTH + (px - rect->left);
		d1.u = rb[DEPTH1_BUFFER_PIXEL_OFFSET + at];
		d2.u = rb[DEPTH2_BUFFER_PIXEL_OFFSET + at];
		tag = rb[PARAM_BUFFER_PIXEL_OFFSET + at];
		stencil = rb[STENCIL_BUFFER_PIXEL_OFFSET + at];
		accum = rb[ACCUM1_BUFFER_PIXEL_OFFSET + at];
		printf("probe %d,%d: pass %d after %s: depth1 %.9g (0x%08x) depth2 %.9g (0x%08x) tag %u%s stencil %u colour %08x\n",
				px, py, cur_pass, when, d1.f, d1.u, d2.f, d2.u, tag >> 1, (tag & TAG_INVALID) ? " (shaded)" : "", stencil, accum);
	}
}

/* REFSW_GAPS=R: how close the depths the chip compares come to each
 * other, over the whole frame. For every pixel of a tile the two largest
 * 1/w of the opaque and punch-through lists are kept (as the chip's
 * depth test sees them), and the largest translucent 1/w with the
 * nearest other translucent one, and the frame's smallest relative gaps
 * are printed with their pixel, tags and bit patterns: a gap under R
 * (relative) is counted. The 1/w is the plane equation refsw evaluates,
 * at the pixel refsw samples. Analysis only: it changes nothing drawn. */
static float gap_rel = 0;
typedef struct GapSlot { float z; u32 tag; } GapSlot;
static GapSlot gap_op[MAX_RENDER_PIXELS][2];	/* the two largest opaque/PT depths */
static GapSlot gap_tr[MAX_RENDER_PIXELS][2];	/* the two largest translucent depths */
static u32 gap_count_op, gap_count_tr, gap_count_tr_op, gap_examples;
static float gap_min_op = 1, gap_min_tr = 1, gap_min_tr_op = 1;

static void gap_clear(void)
{
	int i;
	for (i = 0; i < MAX_RENDER_PIXELS; i++)
	{
		gap_op[i][0].z = gap_op[i][1].z = -1; gap_op[i][0].tag = gap_op[i][1].tag = 0;
		gap_tr[i][0].z = gap_tr[i][1].z = -1; gap_tr[i][0].tag = gap_tr[i][1].tag = 0;
	}
}

static void gap_put(GapSlot *two, float z, u32 tag)
{
	if (z > two[0].z) { two[1] = two[0]; two[0].z = z; two[0].tag = tag; }
	else if (z > two[1].z) { two[1].z = z; two[1].tag = tag; }
}

/* the triangle's 1/w at every pixel of the tile it covers, as refsw's
 * RasterizeTriangle() finds them */
static void gap_triangle(const taRECT *rect, RenderMode rm, u32 tag, const Vertex *v1, const Vertex *v2, const Vertex *v3)
{
	float halfpixel;
	PlaneStepper3 Z;
	int minx, maxx, miny, maxy, x, y;
	if (gap_rel == 0 || rm == RM_MODIFIER)
		return;
	halfpixel = HALF_OFFSET.fpu_pixel_half_offset ? 0.5f : 0;
	PlaneStepper3_Setup(&Z, v1, v2, v3, v1->z, v2->z, v3->z);
	minx = (int)fmax2(fmin2(fmin2(v1->x, v2->x), v3->x), (float)rect->left);
	maxx = (int)fmin2(fmax2(fmax2(v1->x, v2->x), v3->x) + 1, (float)rect->right - 1);
	miny = (int)fmax2(fmin2(fmin2(v1->y, v2->y), v3->y), (float)rect->top);
	maxy = (int)fmin2(fmax2(fmax2(v1->y, v2->y), v3->y) + 1, (float)rect->bottom - 1);
	for (y = miny; y <= maxy; y++)
		for (x = minx; x <= maxx; x++)
		{
			const float x_ps = x + halfpixel, y_ps = y + halfpixel;
			float invW;
			GapSlot *two;
			if (!covers(v1, v2, v3, x_ps, y_ps))
				continue;
			invW = PlaneStepper3_Ip(&Z, x_ps, y_ps);
			two = (rm == RM_TRANSLUCENT ? gap_tr : gap_op)[(y - rect->top) * MAX_RENDER_WIDTH + (x - rect->left)];
			gap_put(two, invW, tag);
		}
}

static void gap_report(const char *what, const taRECT *rect, int i, const GapSlot *a, const GapSlot *b, float rel, float *least, u32 *count)
{
	static u32 cap;
	static bool cap_made;
	union { float f; u32 u; } ua, ub;
	if (rel < *least)
		*least = rel;
	if (rel >= gap_rel)
		return;
	(*count)++;
	ua.f = a->z; ub.f = b->z;
	/* (REFSW_GAPS_EXAMPLES: up to this many lines of each kind, default 20) */
	if (!cap_made)
	{
		cap = getenv("REFSW_GAPS_EXAMPLES") ? atoi(getenv("REFSW_GAPS_EXAMPLES")) : 20;
		cap_made = true;
	}
	if (*count <= cap)
	{
		gap_examples++;
		printf("gap %s: pixel %d,%d: 1/w %.9g (0x%08x) tag %u and %.9g (0x%08x) tag %u: relative %.3g, %d ulps\n", what,
				rect->left + i % MAX_RENDER_WIDTH, rect->top + i / MAX_RENDER_WIDTH, a->z, ua.u, a->tag >> 1, b->z, ub.u, b->tag >> 1, rel, (int)(ua.u - ub.u));
	}
}

static void gap_tile(const taRECT *rect)
{
	int i;
	if (gap_rel == 0)
		return;
	for (i = 0; i < MAX_RENDER_PIXELS; i++)
	{
		if (gap_op[i][1].z > 0 && gap_op[i][0].tag != gap_op[i][1].tag)
			gap_report("opaque vs opaque", rect, i, &gap_op[i][0], &gap_op[i][1], (gap_op[i][0].z - gap_op[i][1].z) / gap_op[i][0].z, &gap_min_op, &gap_count_op);
		if (gap_tr[i][1].z > 0 && gap_tr[i][0].tag != gap_tr[i][1].tag)
			gap_report("translucent vs translucent", rect, i, &gap_tr[i][0], &gap_tr[i][1], (gap_tr[i][0].z - gap_tr[i][1].z) / gap_tr[i][0].z, &gap_min_tr, &gap_count_tr);
		if (gap_tr[i][0].z > 0 && gap_op[i][0].z > 0)
			gap_report("translucent vs opaque", rect, i, &gap_tr[i][0], &gap_op[i][0], fabsf(gap_tr[i][0].z - gap_op[i][0].z) / gap_op[i][0].z, &gap_min_tr_op, &gap_count_tr_op);
	}
}

/* REFSW_GEO=1: refsw's coverage held to the exact geometry, as
 * tools/rend/tile_geo.c holds the compute renderer's. Every triangle drawn
 * into a tile, polygons and modifier volumes, is culled and tested at
 * the pixels of the tile the way RasterizeTriangle() does (covers(), its
 * cull), and again in long double from the half-edge constants as
 * libswirl writes them (C1 = DY12 X1 - DX12 Y1, whose rounding at 64 bits
 * is under 1e-10 pixel for a vertex 1e9 pixels out) - every pixel within
 * 1.5 pixel of an exact edge, and one in 8 each way elsewhere (as
 * tile_geo.c looks at a band and a grid). A pixel one covers
 * and the other does not is a fault, unless its sampling position is
 * within 1/256 pixel of an edge (counted apart); so is a triangle one
 * culls and the other draws. Analysis only: it changes nothing drawn. */
static bool geo_on;
static u64 geo_tris, geo_samples, geo_near, geo_faults, geo_fault_tris, geo_cull_faults;

static bool geo_culled(const DrawParameters *params, int vertex_offset, double area)
{
	if (params->isp.CullMode == 0)
		return false;
	if (fabs(area) < FPU_CULL_VAL)
		return true;
	if (params->isp.CullMode >= 2)
	{
		const u32 mode = vertex_offset ^ (params->isp.CullMode & 1);
		return (mode == 0 && area < 0) || (mode == 1 && area > 0);
	}
	return false;
}

/* (std::min and std::max of long doubles) */
static long double lmin2(long double a, long double b) { return b < a ? b : a; }
static long double lmax2(long double a, long double b) { return a < b ? b : a; }

static void geo_triangle(const DrawParameters *params, int vertex_offset, const Vertex *v1, const Vertex *v2, const Vertex *v3, const taRECT *rect)
{
	const float X1 = v1->x, X2 = v2->x, X3 = v3->x, Y1 = v1->y, Y2 = v2->y, Y3 = v3->y;
	float l, r, t, b, area_r, halfpixel;
	int x0, x1, y0, y1, sgn, k, step, x, y;
	bool far, cull_r, cull_x;
	long double area_x, X[3], Y[3], dx[3], dy[3], c[3], len[3];
	u32 faults;
	if (isnan(X1) || isnan(X2) || isnan(X3) || isnan(Y1) || isnan(Y2) || isnan(Y3))
		return;
	/* the pixels of the tile within a pixel of the triangle's box: outside
	 * it neither covers anything (each end kept to the tile before it is
	 * an integer: a box billions of pixels out does not fit one) */
	l = (float)rect->left; r = (float)rect->right; t = (float)rect->top; b = (float)rect->bottom;
	x0 = (int)fmin2(fmax2(l, floorf(fmin2(fmin2(X1, X2), X3)) - 1), r);
	x1 = (int)fmax2(fmin2(r, ceilf(fmax2(fmax2(X1, X2), X3)) + 1), l);
	y0 = (int)fmin2(fmax2(t, floorf(fmin2(fmin2(Y1, Y2), Y3)) - 1), b);
	y1 = (int)fmax2(fmin2(b, ceilf(fmax2(fmax2(Y1, Y2), Y3)) + 1), t);
	if (x0 >= x1 || y0 >= y1)
		return;
	geo_tris++;
	/* refsw's cull, in its arithmetic */
	far = refsw_far3(v1, v2, v3);
	area_r = far ? (float)(((double)X1 - X3) * ((double)Y2 - Y3) - ((double)Y1 - Y3) * ((double)X2 - X3))
	             : ((X1 - X3) * (Y2 - Y3) - (Y1 - Y3) * (X2 - X3));
	area_x = ((long double)X1 - X3) * ((long double)Y2 - Y3) - ((long double)Y1 - Y3) * ((long double)X2 - X3);
	cull_r = geo_culled(params, vertex_offset, area_r);
	cull_x = geo_culled(params, vertex_offset, (double)area_x);
	if (cull_r != cull_x)
	{
		geo_cull_faults++;
		printf("geo: tile %d,%d: a triangle refsw %s and the geometry %s: area %.9g (exact %.9Lg), vertices %.9g,%.9g %.9g,%.9g %.9g,%.9g\n",
				rect->left, rect->top, cull_r ? "culls" : "draws", cull_x ? "culls" : "draws", area_r, area_x, X1, Y1, X2, Y2, X3, Y3);
		return;
	}
	if (cull_r)
		return;
	sgn = area_x > 0 ? -1 : 1;
	X[0] = X1; X[1] = X2; X[2] = X3;
	Y[0] = Y1; Y[1] = Y2; Y[2] = Y3;
	for (k = 0; k < 3; k++)
	{
		const int a = k, bb = (k + 1) % 3;
		dx[k] = sgn * (X[a] - X[bb]);
		dy[k] = sgn * (Y[a] - Y[bb]);
		c[k] = dy[k] * X[a] - dx[k] * Y[a];
		len[k] = sqrtl(dx[k] * dx[k] + dy[k] * dy[k]);
	}
	halfpixel = HALF_OFFSET.fpu_pixel_half_offset ? 0.5f : 0;
	/* where every pixel of the box lies more than 1.5 pixel inside all the
	 * edges, or outside one, only a pixel in 8 each way is looked at: a
	 * misplaced edge's error begins at the exact edge, which the pixels
	 * near it see, and the grid sees one that is not near (as tile_geo
	 * looks at a band about the edges and a grid) */
	step = 1;
	{
		bool all_in = true, all_out = false;
		for (k = 0; k < 3 && len[k] > 0; k++)
		{
			long double lo = 1e30L, hi = -1e30L;
			int corner;
			for (corner = 0; corner < 4; corner++)
			{
				const long double px = (corner & 1 ? x1 - 1 : x0) + halfpixel, py = (corner & 2 ? y1 - 1 : y0) + halfpixel;
				const long double e = (c[k] + dx[k] * py - dy[k] * px) / len[k];
				lo = lmin2(lo, e);
				hi = lmax2(hi, e);
			}
			if (lo <= 1.5L)
				all_in = false;
			if (hi < -1.5L)
				all_out = true;
		}
		if (all_in || all_out)
			step = 8;
	}
	faults = 0;
	for (y = y0; y < y1; y += step)
		for (x = x0; x < x1; x += step)
		{
			const float x_ps = x + halfpixel, y_ps = y + halfpixel;
			bool in_x = true;
			long double nearest = 1e30L;
			for (k = 0; k < 3; k++)
			{
				const long double e = c[k] + dx[k] * y_ps - dy[k] * x_ps;
				if (e < 0)
					in_x = false;
				if (len[k] > 0 && fabsl(e) / len[k] < nearest)
					nearest = fabsl(e) / len[k];
			}
			/* (and within the box, the same: a pixel 1.5 pixel or more from
			 * every edge only on the grid) */
			if (step == 1 && nearest >= 1.5L && ((x - x0) % 8 != 0 || (y - y0) % 8 != 0))
				continue;
			geo_samples++;
			if (covers(v1, v2, v3, x_ps, y_ps) == in_x)
				continue;
			if (nearest < 1.0L / 256)
			{
				geo_near++;
				continue;
			}
			geo_faults++;
			if (faults++ == 0 && geo_fault_tris++ < 8)	/* (every triangle at fault counted, the first 8 printed) */
				printf("geo: tile %d,%d: pixel %d,%d %s by refsw and %s, %.3Lg pixel from an edge; vertices %.9g,%.9g %.9g,%.9g %.9g,%.9g\n",
						rect->left, rect->top, x, y, in_x ? "not covered" : "covered", in_x ? "covered" : "not covered", nearest,
						X1, Y1, X2, Y2, X3, Y3);
		}
}

/* refrend_base.cpp's RenderTriangle(): a modifier volume's last triangle
 * has the stencil summarized after it */
static void render_triangle(refsw *backend, RenderMode render_mode, DrawParameters *params, parameter_tag_t tag, int vertex_offset,
		const Vertex *v1, const Vertex *v2, const Vertex *v3, const Vertex *v4, taRECT *area)
{
	if (geo_on && v4 == NULL)
		geo_triangle(params, vertex_offset, v1, v2, v3, area);
	refsw_RasterizeTriangle(backend, render_mode, params, tag, vertex_offset, v1, v2, v3, v4, area);
	if (render_mode == RM_MODIFIER)
	{
		if (params->isp.modvol.VolumeMode == 1)
			refsw_SummarizeStencilOr(backend);
		else if (params->isp.modvol.VolumeMode == 2)
			refsw_SummarizeStencilAnd(backend);
	}
}

/* The TA puts a polygon sent under a user tile clip only in the tiles the
 * clip says (inside the rectangle, or outside it): the same here */
static bool tile_clipped(u32 tileclip, int tx, int ty)
{
	const u32 mode = tileclip >> 28;
	int csx, cex, csy, cey;
	bool in;
	if (mode < 2)
		return false;
	csx = tileclip & 63; cex = (tileclip >> 6) & 63; csy = (tileclip >> 12) & 31; cey = (tileclip >> 17) & 31;
	in = tx >= csx && tx <= cex && ty >= csy && ty <= cey;
	return mode == 2 ? !in : in;
}

static ISP_BACKGND_T_type tag_key(u32 id)
{
	ISP_BACKGND_T_type t;
	t.full = 0x80000000u | id;
	return t;
}

/* A polygon list's polygons [from, to) rasterized into the tile. After
 * the parse a polygon is a triangle strip of indices into the vertices
 * (make_index()): the TA's strips of one state joined into one by repeating
 * a vertex, with the winding of each triangle kept (an index repeated where
 * needed), so the strip's i-th triangle has the parity the chip gives it.
 * The joins are triangles with a repeated vertex, which the chip never sees
 * and which are left out here. Each triangle's tag is its ordinal in the
 * frame (@base + the strip's first + i): the same triangle gets the same
 * tag each time the list is walked (the translucent list is, once a peeled
 * layer), which the peeling's coplanar tie-break relies on. */
static void render_polys(refsw *backend, RenderMode rm, const Frame *fr, const PolyParam *list, u32 list_n, u32 from, u32 to,
		u32 base, const u32 *first_tri, taRECT *rect, int tx, int ty, const char *what)
{
	u32 p;
	cur_list = what;
	for (p = from; p < to && p < list_n; p++)
	{
		const PolyParam *pp = &list[p];
		DrawParameters params;
		const u32 *idx;
		u32 i;
		if (pp->count < 3 || pp->first + pp->count > fr->h.n_idx)
			continue;
		if (tile_clipped(pp->tileclip, tx, ty))
			continue;
		memset(&params, 0, sizeof(params));
		params.isp.full = pp->isp;
		/* the TA writes the control word's texture, offset, gouraud and
		 * 16-bit UV bits into the ISP word it stores; the parse keeps
		 * the two words apart */
		params.isp.Texture = TAW_GET(pp->pcw, PCW_Texture);
		params.isp.Offset = TAW_GET(pp->pcw, PCW_Offset);
		params.isp.Gouraud = TAW_GET(pp->pcw, PCW_Gouraud);
		params.isp.UV_16b = TAW_GET(pp->pcw, PCW_UV_16bit);
		params.tsp.full = pp->tsp;
		params.tcw.full = pp->tcw;
		params.tsp2.full = pp->tsp1;
		params.tcw2.full = pp->tcw1;
		params.shadow = TAW_GET(pp->pcw, PCW_Shadow);
		idx = &fr->idx[pp->first];
		for (i = 0; i + 2 < pp->count; i++)
		{
			const u32 a = idx[i], b = idx[i + 1], c = idx[i + 2];
			Vertex tri[3];
			parameter_tag_t tag;
			if (a == b || b == c || a == c)
				continue;
			if (a >= fr->h.n_verts || b >= fr->h.n_verts || c >= fr->h.n_verts)
				continue;
			tri[0] = fr->verts[a];
			tri[1] = fr->verts[b];
			tri[2] = fr->verts[c];
			tag = refsw_AddFpuEntry(backend, &params, tri, rm, tag_key(base + first_tri[p] + i));
			probe_triangle(rect, &params, tag, what, p, i, &tri[0], &tri[1], &tri[2]);
			gap_triangle(rect, rm, tag, &tri[0], &tri[1], &tri[2]);
			render_triangle(backend, rm, &params, tag, i & 1, &tri[0], &tri[1], &tri[2], NULL, rect);
		}
	}
}

/* A modifier volume list's volumes [from, to): each is a run of triangles
 * under one parameter word; the last polygon's word carries the volume
 * instruction, which summarizes the stencil after each of its triangles */
static void render_modvols(refsw *backend, const Frame *fr, const ModifierVolumeParam *list, u32 list_n, u32 from, u32 to,
		taRECT *rect, int tx, int ty, const char *what)
{
	u32 p;
	cur_list = what;
	for (p = from; p < to && p < list_n; p++)
	{
		const ModifierVolumeParam *mv = &list[p];
		DrawParameters params;
		u32 i;
		if (mv->first + mv->count > fr->h.n_modtrig)
			continue;
		if (tile_clipped(mv->tileclip, tx, ty))
			continue;
		memset(&params, 0, sizeof(params));
		params.isp.full = mv->isp;
		for (i = 0; i < mv->count; i++)
		{
			const ModTriangle *t = &fr->modtrig[mv->first + i];
			Vertex v[3];
			memset(v, 0, sizeof(v));
			v[0].x = t->x0; v[0].y = t->y0; v[0].z = t->z0;
			v[1].x = t->x1; v[1].y = t->y1; v[1].z = t->z1;
			v[2].x = t->x2; v[2].y = t->y2; v[2].z = t->z2;
			probe_triangle(rect, &params, 0, what, p, i, &v[0], &v[1], &v[2]);
			render_triangle(backend, RM_MODIFIER, &params, 0, 0, &v[0], &v[1], &v[2], NULL, rect);
		}
	}
}
/* refrend_base.cpp's decode_pvr_vertex() / decode_pvr_vetrices(): the
 * background polygon, which the chip reads from video memory where
 * ISP_BACKGND_T points */
static f32 f16(u16 v)
{
	u32 z = v << 16;
	return *(f32 *)&z;
}

#define vert_packed_color_(to,src) \
	{ \
	u32 t=src; \
	to[2] = (u8)(t);t>>=8;\
	to[1] = (u8)(t);t>>=8;\
	to[0] = (u8)(t);t>>=8;\
	to[3] = (u8)(t);      \
	}

static void decode_pvr_vertex(DrawParameters *params, u32 ptr, Vertex *cv)
{
	u32 col;
	cv->x = vrf(ptr); ptr += 4;
	cv->y = vrf(ptr); ptr += 4;
	cv->z = vrf(ptr); ptr += 4;
	if (params->isp.Texture)
	{
		if (params->isp.UV_16b)
		{
			u32 uv = vri(ptr);
			cv->u = f16((u16)(uv >> 16));
			cv->v = f16((u16)(uv >> 0));
			ptr += 4;
		}
		else
		{
			cv->u = vrf(ptr); ptr += 4;
			cv->v = vrf(ptr); ptr += 4;
		}
	}
	col = vri(ptr); ptr += 4;
	vert_packed_color_(cv->col, col);
	if (params->isp.Offset)
	{
		u32 ocol = vri(ptr); ptr += 4;
		vert_packed_color_(cv->vtx_spc, ocol);
	}
}

static u32 decode_pvr_vetrices(DrawParameters *params, u32 base, u32 skip, u32 shadow, Vertex *vtx, int count)
{
	bool PSVM = FPU_SHAD_SCALE.intensity_shadow == 0;
	int i;
	if (!PSVM)
		shadow = 0;
	params->isp.full = vri(base);
	params->tsp.full = vri(base + 4);
	params->tcw.full = vri(base + 8);
	base += 12;
	if (shadow)
	{
		params->tsp2.full = vri(base + 0);
		params->tcw2.full = vri(base + 4);
		base += 8;
	}
	for (i = 0; i < count; i++)
	{
		decode_pvr_vertex(params, base, &vtx[i]);
		base += (3 + skip * (shadow + 1)) * 4;
	}
	return base;
}

/* The tile, through every render pass: refrend_base.cpp's RenderPVR() for
 * the region array entries of one tile, one an entry. The first pass
 * clears the buffers; a later one does when its region array entry says
 * (the parse's RenderPass::z_clear). */
static void render_tile(refsw *backend, const Frame *fr, int tx, int ty, u32 *const first_tri[3], const u32 base[3], u8 *drawn, u32 drawn_w)
{
	taRECT rect;
	u32 op0 = 0, pt0 = 0, tr0 = 0, mvo0 = 0, mvotr0 = 0, pass;
	const u8 *src;
	int x, y;

	rect.top = ty * 32;
	rect.left = tx * 32;
	rect.bottom = rect.top + 32;
	rect.right = rect.left + 32;
	gap_clear();

	for (pass = 0; pass < fr->n_passes; pass++)
	{
		const RenderPass *rp = &fr->passes[pass];
		parameter_tag_t bgTag;
		cur_pass = (int)pass;

		/* register BGPOLY to fpu */
		{
			DrawParameters params;
			Vertex vtx[8];
			memset(&params, 0, sizeof(params));
			memset(vtx, 0, sizeof(vtx));
			decode_pvr_vetrices(&params, PARAM_BASE + ISP_BACKGND_T.tag_address * 4, ISP_BACKGND_T.skip, ISP_BACKGND_T.shadow, vtx, 8);
			params.shadow = ISP_BACKGND_T.shadow;
			bgTag = refsw_AddFpuEntry(backend, &params, &vtx[ISP_BACKGND_T.tag_offset], RM_OPAQUE, ISP_BACKGND_T);
			if (pass == 0)
				probe_triangle(&rect, &params, bgTag, "background", 0, ISP_BACKGND_T.tag_offset,
						&vtx[ISP_BACKGND_T.tag_offset], &vtx[ISP_BACKGND_T.tag_offset + 1], &vtx[ISP_BACKGND_T.tag_offset + 2]);
		}

		/* Tile needs clear? */
		if (pass == 0 || rp->z_clear)
			refsw_ClearBuffers(backend, bgTag, ISP_BACKGND_D.f, 0);

		/* Render OPAQ to TAGS */
		render_polys(backend, RM_OPAQUE, fr, fr->op, fr->h.n_op, op0, rp->op_count, base[0], first_tri[0], &rect, tx, ty, "opaque");
		probe_buffers(backend, &rect, "opaque list");

		/* render PT to TAGS */
		render_polys(backend, RM_PUNCHTHROUGH, fr, fr->pt, fr->h.n_pt, pt0, rp->pt_count, base[1], first_tri[1], &rect, tx, ty, "punch-through");
		probe_buffers(backend, &rect, "punch-through list");

		/* Render OPAQ modvols */
		render_modvols(backend, fr, fr->mvo, fr->h.n_mvo, mvo0, rp->mvo_count, &rect, tx, ty, "opaque modvol");
		probe_buffers(backend, &rect, "opaque modifier volumes");

		/* Render TAGS to ACCUM */
		refsw_RenderParamTags(backend, RM_OPAQUE, rect.left, rect.top);
		probe_buffers(backend, &rect, "opaque shading");

		/* layer peeling rendering */
		if (rp->tr_count > tr0)
		{
			int layer = 0;
			refsw_ClearParamBuffer(backend, TAG_INVALID);
			do
			{
				const float keep_rel = gap_rel;
				const bool keep_geo = geo_on;
				char when[64];
				refsw_ClearPixelsDrawn(backend);
				refsw_PeelBuffers(backend, FLT_MAX, 0);
				if (layer > 0)
				{
					/* (the list is the same each layer: counted once) */
					gap_rel = 0;
					geo_on = false;
				}
				render_polys(backend, RM_TRANSLUCENT, fr, fr->tr, fr->h.n_tr, tr0, rp->tr_count, base[2], first_tri[2], &rect, tx, ty, "translucent");
				gap_rel = keep_rel;
				if (rp->mv_op_tr_shared)
					render_modvols(backend, fr, fr->mvo, fr->h.n_mvo, mvo0, rp->mvo_count, &rect, tx, ty, "translucent modvol (the opaque ones)");
				else
					render_modvols(backend, fr, fr->mvo_tr, fr->h.n_mvo_tr, mvotr0, rp->mvo_tr_count, &rect, tx, ty, "translucent modvol");
				geo_on = keep_geo;
				snprintf(when, sizeof(when), "translucent layer %d (%u pixels)", layer, refsw_GetPixelsDrawn(backend));
				probe_buffers(backend, &rect, when);
				refsw_RenderParamTags(backend, RM_TRANSLUCENT, rect.left, rect.top);
				snprintf(when, sizeof(when), "translucent layer %d shading", layer);
				probe_buffers(backend, &rect, when);
				layer++;
			} while (refsw_GetPixelsDrawn(backend) != 0 && layer < 1024);
		}

		op0 = rp->op_count;
		pt0 = rp->pt_count;
		tr0 = rp->tr_count;
		mvo0 = rp->mvo_count;
		mvotr0 = rp->mvo_tr_count;

		/* clear the tsp cache */
		refsw_ClearFpuEntries(backend);
	}
	gap_tile(&rect);

	/* The accumulation buffer, as the chip hands it to its scaler on the
	 * way to video memory (scaler_write()): here to the picture as drawn,
	 * 8 bits a channel, BGRA to RGBA */
	src = refsw_GetColorOutputBuffer(backend);
	for (y = 0; y < 32; y++)
		for (x = 0; x < 32; x++, src += 4)
		{
			const int px = rect.left + x, py = rect.top + y;
			u8 *dst;
			if (px >= (int)drawn_w || py >= (int)fr->h.height)
				continue;
			dst = drawn + ((size_t)py * drawn_w + px) * 4;
			dst[0] = src[2];
			dst[1] = src[1];
			dst[2] = src[0];
			dst[3] = src[3];
		}
}

/* The tiles' copy to video memory through the chip's X and Y scalers
 * (SCALER_CTL, Y_COEFF; rend/tile/pvr_scaler.h has the arithmetic):
 * with horizontal scaling the game draws twice as wide and each pair of
 * pixels is averaged into one; when the vertical scale factor reduces
 * (above 0x400), each line is filtered with the lines above and below,
 * the picture's first and last lines with themselves in the place of the
 * line there is not; then the packing to a framebuffer of 16 bits a
 * pixel and its dither (FB_W_CTRL), read back as the video output reads
 * it (FB_R_CTRL's fb_concat in the low bits: pvr_fb_write()). The pixel
 * clip (FB_X_CLIP, FB_Y_CLIP) is taken on
 * the way out, in the framebuffer's pixels (a game that draws 1280 wide
 * through the X scaler clips at 639), the scaler having filtered what
 * the tiles hold beyond it. The Y scaler's choice of lines (the scale
 * factor itself) is not applied: the picture is the lines drawn, as the
 * renderers present them. Alpha is made opaque in the picture. */
static void scaler_write(const Frame *fr, const u8 *drawn, u32 drawn_w, u8 *picture)
{
	SCALER_CTL_type scaler;
	u32 ycoeff, fbw, w, h, across, x, y;
	FB_W_CTRL_type wctrl;
	fb_r_ctrl rctrl;
	FB_X_CLIP_type xclip;
	FB_Y_CLIP_type yclip;
	u32 *line;
	bool filter;

	scaler.full = *(const u32 *)&fr->regs[SCALER_CTL_addr];
	ycoeff = *(const u32 *)&fr->regs[Y_COEFF_addr];
	wctrl.full = *(const u32 *)&fr->regs[FB_W_CTRL_addr];
	rctrl.full = *(const u32 *)&fr->regs[FB_R_CTRL_addr];
	fbw = pvr_fb_word(wctrl.fb_packmode, wctrl.fb_dither, rctrl.fb_concat);
	xclip.full = fr->h.fb_x_clip;
	yclip.full = fr->h.fb_y_clip;
	w = fr->h.width;
	h = fr->h.height;
	across = scaler.hscale ? 2 : 1;
	line = (u32 *)malloc((size_t)w * h * sizeof(u32));
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			const u8 *p = drawn + ((size_t)y * drawn_w + x * across) * 4;
			u32 c;
			memcpy(&c, p, 4);
			if (scaler.hscale)
			{
				u32 d;
				memcpy(&d, p + 4, 4);
				c = pvr_scaler_x(c, d);
			}
			line[y * w + x] = c;
		}
	filter = pvr_scaler_yfilter(scaler.full) != 0;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			u32 c;
			u8 *dst;
			if (x < xclip.min || x > xclip.max || y < yclip.min || y > yclip.max)
				continue;
			c = line[y * w + x];
			if (filter)
				c = pvr_scaler_y(line[(y == 0 ? 0 : y - 1) * w + x], c, line[(y + 1 == h ? y : y + 1) * w + x], ycoeff);
			c = pvr_fb_write(c, x, y, fbw);
			dst = picture + ((size_t)y * w + x) * 4;
			memcpy(dst, &c, 4);
			dst[3] = 0xFF;
		}
	free(line);
}

static u32 *first_triangles(const PolyParam *list, u32 n_list, u32 *total)
{
	u32 *first = (u32 *)malloc((n_list ? n_list : 1) * sizeof(u32));
	u32 n = 0, i;
	for (i = 0; i < n_list; i++)
	{
		first[i] = n;
		n += list[i].count >= 3 ? list[i].count - 2 : 0;
	}
	*total = n;
	return first;
}

int main(int argc, char **argv)
{
	Frame fr;
	u32 *first_tri[3];
	u32 base[3], n, drawn_w, i;
	refsw *backend;
	u8 *picture, *drawn;
	SCALER_CTL_type scaler;
	int tiles_x, tiles_y, tx, ty;
	FILE *out;
	RenderPass one;

	if (argc < 3)
	{
		fprintf(stderr, "usage: refsw_frame FRAME.bin OUT.ppm [X,Y ...]\n");
		return 2;
	}
	probes = (Probe *)malloc(argc * sizeof(Probe));
	for (i = 3; i < (u32)argc; i++)
	{
		Probe p;
		if (sscanf(argv[i], "%d,%d", &p.x, &p.y) == 2)
			probes[n_probes++] = p;
	}
	memset(&fr, 0, sizeof(fr));
	if (!load(argv[1], &fr))
		return 1;
	if (fr.h.regs_size < pvr_RendRegSize)
	{
		fprintf(stderr, "refsw_frame: the frame has %u bytes of registers\n", fr.h.regs_size);
		return 1;
	}
	if (fr.h.width == 0 || fr.h.height == 0 || fr.h.width > 4096 || fr.h.height > 4096)
	{
		fprintf(stderr, "refsw_frame: the frame is %u x %u\n", fr.h.width, fr.h.height);
		return 1;
	}

	/* the core's texture cache sees this frame's memory and registers */
	rend_pvr_regs = fr.regs;
	vram_bytes = fr.vram;
	vram.data = fr.vram;
	vram.size = fr.h.vram_size;
	VRAM_SIZE = fr.h.vram_size;
	VRAM_MASK = fr.h.vram_size - 1;
	memset(&settings, 0, sizeof(settings));
	settings.rend.TextureUpscale = 1;
	settings.rend.MaxFilteredTextureSize = 256;
	settings.rend.GpuTextureDecode = false;
	settings.rend.ForceTextureLOD0 = true;
	palette_update();

	printf("refsw_frame: frame %u, %u x %u, %u passes, %u/%u/%u polygons, %u/%u modifier volumes (%u triangles); "
			"ISP_BACKGND_D %.9g, HALF_OFFSET %u, FPU_CULL_VAL %g, PT_ALPHA_REF %u, FPU_SHAD_SCALE %02x/%u, ISP_FEED_CFG %08x\n",
			fr.h.frame, fr.h.width, fr.h.height, fr.h.n_passes, fr.h.n_op, fr.h.n_pt, fr.h.n_tr, fr.h.n_mvo, fr.h.n_mvo_tr, fr.h.n_modtrig,
			ISP_BACKGND_D.f, HALF_OFFSET.full, FPU_CULL_VAL, PT_ALPHA_REF, FPU_SHAD_SCALE.scale_factor, FPU_SHAD_SCALE.intensity_shadow, ISP_FEED_CFG);
	for (i = 0; i < fr.n_passes; i++)
		printf("refsw_frame: pass %u: autosort %d z_clear %d op %u pt %u tr %u mvo %u mvo_tr %u%s\n", i, fr.passes[i].autosort, fr.passes[i].z_clear,
				fr.passes[i].op_count, fr.passes[i].pt_count, fr.passes[i].tr_count, fr.passes[i].mvo_count, fr.passes[i].mvo_tr_count,
				fr.passes[i].mv_op_tr_shared ? " (translucent modvols are the opaque ones)" : "");
	if (fr.n_passes == 0)
	{
		memset(&one, 0, sizeof(one));
		one.autosort = true;
		one.z_clear = true;
		one.op_count = fr.h.n_op;
		one.pt_count = fr.h.n_pt;
		one.tr_count = fr.h.n_tr;
		one.mvo_count = fr.h.n_mvo;
		one.mvo_tr_count = fr.h.n_mvo_tr;
		fr.passes = &one;
		fr.n_passes = 1;
	}

	/* the tags: a triangle's ordinal in the frame, opaque then
	 * punch-through then translucent */
	first_tri[0] = first_triangles(fr.op, fr.h.n_op, &n);
	base[0] = 1;
	base[1] = base[0] + n;
	first_tri[1] = first_triangles(fr.pt, fr.h.n_pt, &n);
	base[2] = base[1] + n;
	first_tri[2] = first_triangles(fr.tr, fr.h.n_tr, &n);

	if (getenv("REFSW_GAPS") != NULL)
		gap_rel = (float)atof(getenv("REFSW_GAPS"));
	geo_on = getenv("REFSW_GEO") != NULL && atoi(getenv("REFSW_GEO")) != 0;
	backend = refsw_create(fr.vram);
	picture = (u8 *)calloc((size_t)fr.h.width * fr.h.height * 4, 1);
	/* (the game draws twice as wide where the X scaler halves it) */
	scaler.full = *(const u32 *)&fr.regs[SCALER_CTL_addr];
	drawn_w = fr.h.width * (scaler.hscale ? 2 : 1);
	printf("refsw_frame: SCALER_CTL %08x Y_COEFF %08x: drawn %u x %u\n", scaler.full, *(const u32 *)&fr.regs[Y_COEFF_addr], drawn_w, fr.h.height);
	drawn = (u8 *)calloc((size_t)drawn_w * fr.h.height * 4, 1);
	tiles_x = (drawn_w + 31) / 32;
	tiles_y = (fr.h.height + 31) / 32;
	for (ty = 0; ty < tiles_y; ty++)
		for (tx = 0; tx < tiles_x; tx++)
			render_tile(backend, &fr, tx, ty, first_tri, base, drawn, drawn_w);
	scaler_write(&fr, drawn, drawn_w, picture);
	refsw_destroy(backend);
	textures_clear();
	if (gap_rel != 0)
		printf("gaps under %g: opaque vs opaque %u pixels (smallest %.3g), translucent vs translucent %u (smallest %.3g), translucent vs opaque %u (smallest %.3g)\n",
				gap_rel, gap_count_op, gap_min_op, gap_count_tr, gap_min_tr, gap_count_tr_op, gap_min_tr_op);
	if (geo_on)
		printf("geo: %llu triangles in tiles, %llu samples: %llu faults in %llu triangles, %llu triangles culled by one and not the other, "
				"%llu within 1/256 pixel of an edge: %s\n", (unsigned long long)geo_tris, (unsigned long long)geo_samples,
				(unsigned long long)geo_faults, (unsigned long long)geo_fault_tris, (unsigned long long)geo_cull_faults,
				(unsigned long long)geo_near, geo_faults == 0 && geo_cull_faults == 0 ? "PASS" : "FAIL");

	out = fopen(argv[2], "wb");
	if (out == NULL)
	{
		perror(argv[2]);
		return 1;
	}
	fprintf(out, "P6\n%u %u\n255\n", fr.h.width, fr.h.height);
	for (i = 0; i < fr.h.width * fr.h.height; i++)
		fwrite(&picture[(size_t)i * 4], 1, 3, out);
	fclose(out);
	printf("refsw_frame: %s written\n", argv[2]);
	free(picture);
	free(drawn);
	for (i = 0; i < 3; i++)
		free(first_tri[i]);
	free(fr.regs);
	free(fr.vram);
	free(fr.verts);
	free(fr.idx);
	free(fr.op);
	free(fr.pt);
	free(fr.tr);
	free(fr.modtrig);
	free(fr.mvo);
	free(fr.mvo_tr);
	if (fr.passes != &one)
		free(fr.passes);
	free(fr.region);
	free(probes);
	return 0;
}
