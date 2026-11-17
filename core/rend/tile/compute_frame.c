#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
/* The compute renderer's frame: see compute_frame.h */
#include <math.h>
#include <string.h>
#include "compute_frame.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta_words.h"
#include "rend/texconv.h"
#include "rend/tileclip.h"
#include "rend/depth_scale.h"
#include "rend/ssaa_rate.h"

/* the kernels read the parse's vertices word for word, and its modifier
 * volume triangles; a record keeps a vertex's number in PVRC_RX_VTX:
 * NAOMI 2's four times the TA data, all vertices */
typedef char cf_vertex_words[sizeof(Vertex) == PVRC_VTX_WORDS * 4 ? 1 : -1];
typedef char cf_modtrig_words[sizeof(ModTriangle) == 9 * 4 ? 1 : -1];
typedef char cf_vertex_number[4u * 4 * 1024 * 1024 / sizeof(Vertex) <= PVRC_RX_VTX ? 1 : -1];

bool compute_frame_scalers_off = true;

u32 compute_frame_write_out(u32 height)
{
	u32 lines, fbw;
	if (compute_frame_scalers_off)
		return 0;
	/* The Y scaler's filter when the vertical scale factor reduces, then
	 * the packing to a framebuffer of 16 bits and its dither
	 * (rend/tile/pvr_scaler.h): its taps a line of the chip's apart, the
	 * picture's lines over 480 (the internal resolution) */
	lines = (height + 240) / 480;
	if (lines < 1)
		lines = 1;
	if (lines > 31)
		lines = 31;
	fbw = pvr_fb_word(FB_W_CTRL.fb_packmode, FB_W_CTRL.fb_dither, FB_R_CTRL.fb_concat);
	if (pvr_scaler_yfilter(SCALER_CTL.full))
		return PVRC_FF_YFILTER(lines, Y_COEFF & 0xffff) | PVRC_FF_FBWRITE(0u, fbw);
	return fbw != 0 ? PVRC_FF_FBWRITE(lines, fbw) : 0;
}

/* ---- The frame's words ---- */

static u32 machine_scale(void)
{
	return ta_data_size() / TA_DATA_SIZE;
}

/* (what TA_context::Alloc() gives each list, the most a frame has) */
u32 compute_frame_max_prims(void)
{
	return (120 * 1024 + 16384) * machine_scale();
}

u32 compute_frame_max_words(void)
{
	const u32 s = machine_scale();
	const u32 polys = (16384 + 5120 + 10240) * s;
	const u32 mvps = 8192 * s;
	const u32 verts = (u32)(4 * 1024 * 1024 * s / sizeof(Vertex));
	return PVRC_F_WORDS + polys * PVRC_POLY_WORDS + mvps * PVRC_MVP_WORDS + (polys + mvps) * PVRC_SRC_WORDS
			+ verts * PVRC_VTX_WORDS + 120 * 1024 * s + 16384 * s * 9;
}

u32 compute_frame_rtt_scale(void)
{
	return settings.rend.RenderToTextureUpscale > 1 ? (u32)settings.rend.RenderToTextureUpscale : 1;
}

/* (a texture rendered to is at most 1024 x 1024, times Render To Texture
 * Upscaling's 8 at the most: OITTextureDrawer::GetSize()) */
u32 compute_frame_max_rtt_tiles(void)
{
	const u32 side = 1024 * 8;
	const u32 tiles = (side + (1u << compute_frame_tile_log2(side)) - 1) >> compute_frame_tile_log2(side);
	return tiles * tiles;
}

static INLINE int cf_min(int a, int b)
{
	return a < b ? a : b;
}

static INLINE u32 rect_word(int x, int y)
{
	return (u32)(x > 0 ? x : 0) | ((u32)(y > 0 ? y : 0) << 16);
}

static INLINE u32 fbits(float f)
{
	u32 u;
	memcpy(&u, &f, 4);
	return u;
}

/* What a frame's packing goes by */
typedef struct cf_pack
{
	const compute_frame_renderer *r;
	u32 *out;
	u32 oMvp, oSrc;
	u32 prim, sources;
	bool gpu;
	xform vp;
	int scissor[4];	/* the base scissor: x, y, width, height */
	int bx1, by1;
	u32 baseRect0, baseRect1;
} cf_pack;

/* A polygon's clip rectangle (x, y, width, height), as the raster
 * renderers' SetTileClip() makes it */
static int cf_tile_clip(const cf_pack *f, u32 val, int *rect)
{
	const int mode = tile_clip(val, &f->vp, rect);
	if (mode == TILECLIP_OUTSIDE)
		intersect_clip_rect(rect, f->scissor[0], f->scissor[1], f->scissor[2], f->scissor[3]);
	return mode;
}

/* The numbering, render pass by render pass, and the table of starts.
 * (A frame of the decode's: entries of the table where there would be
 * numbers, and the third word a volume's count or a group's draw.) */
static void cf_source(cf_pack *f, u32 kind, u32 index, u32 third)
{
	u32 *s = f->out + f->oSrc + f->sources * PVRC_SRC_WORDS;
	s[0] = f->prim;
	s[1] = kind | index;
	s[2] = third;
	f->sources++;
}

static u32 cf_at(const cf_pack *f)
{
	return f->gpu ? f->sources : f->prim;
}

static void cf_volumes(cf_pack *f, const ModifierVolumeParam *params, u32 first, u32 last, u32 indexBase)
{
	/* DrawModifierVolumes()'s rule: a parameter in an inclusion or
	 * exclusion mode ends the volume, which is every triangle since the
	 * last one that did; parameters without triangles are passed over */
	int base = -1;
	u32 m;
	for (m = first; m < last; m++)
	{
		const ModifierVolumeParam *p = &params[m];
		u32 mode;
		if (p->count == 0)
			continue;
		cf_source(f, PVRC_SRC_MODVOL, indexBase + m, p->count);
		f->prim += p->count;
		if (base == -1)
			base = (int)m;
		mode = TAW_GET(TAW_FULL(p->isp), ISP_Modvol_DepthMode);
		if (mode == 1 || mode == 2)
		{
			/* (its last triangle's number; a frame of the decode's: its entry) */
			const u32 end = f->gpu ? f->sources - 1 : f->prim - 1;
			u32 k;
			for (k = (u32)base; k <= m; k++)
			{
				u32 *w;
				if (params[k].count == 0)
					continue;
				w = f->out + f->oMvp + (indexBase + k) * PVRC_MVP_WORDS;
				w[PVRC_M_CLOSE_END] = end;
				w[PVRC_M_CLOSE_BY] = indexBase + m;
			}
			base = -1;
		}
	}
}

static void cf_strips(cf_pack *f, const PolyParam *head, u32 first, u32 last, u32 indexBase)
{
	const bool trd = f->gpu && head == TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam);
	u32 i;
	for (i = first; i < last; i++)
	{
		const PolyParam *pp = &head[i];
		/* (the decode's groups: count is their vertices, first their
		 * command's slot, or the translucent's place in the draw records) */
		if (pp->count <= 2)
			continue;
		cf_source(f, trd ? PVRC_SRC_TRD : 0, indexBase + i, pp->first);
		f->prim += pp->count - 2;
	}
}

/* A drawing rectangle's two words: @r's when it clips, else the base's */
static void cf_rect(const cf_pack *f, int clip, const int *r, u32 *w)
{
	if (clip == TILECLIP_OUTSIDE)
	{
		w[0] = rect_word(cf_min(r[0], f->bx1), cf_min(r[1], f->by1));
		w[1] = rect_word(cf_min(r[0] + r[2], f->bx1), cf_min(r[1] + r[3], f->by1));
	}
	else
	{
		w[0] = f->baseRect0;
		w[1] = f->baseRect1;
	}
}

/* The polygon parameters, with what the per-pixel renderer's pipeline key
 * and push constants have of them (OITPipelineManager::Key(),
 * OITDrawer::DrawPoly()) */
static bool cf_poly(const cf_pack *f, const PolyParam *pp, u32 listType, u32 *q)
{
	const u32 tsp = TAW_FULL(pp->tsp), tcw = TAW_FULL(pp->tcw), pcw = TAW_FULL(pp->pcw);
	const u32 tsp1 = TAW_FULL(pp->tsp1), tcw1 = TAW_FULL(pp->tcw1);
	const bool two = tsp1 != (u32)-1 || tcw1 != (u32)-1;
	const u32 fmt = TAW_GET(tcw, TCW_PixelFmt);
	const bool texture = TAW_GET(pcw, PCW_Texture) != 0;
	bool palette;
	u32 flags = 0, slot0 = 0xffff, slot1 = 0xffff, pal = 0;
	float trilinearAlpha = 1.f;
	int r[4];
	int clip;

	if (listType == TA_LIST_PUNCH_THROUGH)
		flags |= PVRC_QF_ALPHA_TEST;
	if (fmt == TA_PIXEL_BUMPMAP)
		flags |= PVRC_QF_BUMP;
	if (TAW_GET(tsp, TSP_ColorClamp))
		flags |= PVRC_QF_CLAMP;
	if ((pp->tileclip >> 28) == 3)
		flags |= PVRC_QF_CLIP_INSIDE;
	flags |= TAW_GET(tsp, TSP_FogCtrl) << PVRC_QF_FOG_SHIFT;
	if (TAW_GET(tsp, TSP_IgnoreTexA) || fmt == TA_PIXEL_565)
		flags |= PVRC_QF_IGNORE_TEXA;
	if (TAW_GET(pcw, PCW_Offset))
		flags |= PVRC_QF_OFFSET;
	flags |= TAW_GET(tsp, TSP_ShadInstr) << PVRC_QF_SHAD_SHIFT;
	if (texture)
		flags |= PVRC_QF_TEXTURE;
	if (TAW_GET(tsp, TSP_UseAlpha))
		flags |= PVRC_QF_USE_ALPHA;
	palette = tex_gpu_paletted(tsp, tcw, 0);
	if (palette)
		flags |= PVRC_QF_PALETTE;
	if (TAW_GET(pcw, PCW_Gouraud))
		flags |= PVRC_QF_GOURAUD;
	if (TAW_GET(pcw, PCW_Shadow))
		flags |= PVRC_QF_SHADOW;
	if (ssaa_single_rate(pp))
		flags |= PVRC_QF_SINGLE_RATE;
	if (two)
	{
		flags |= PVRC_QF_TWO_VOLUMES;
		flags |= TAW_GET(tsp1, TSP_ShadInstr) << PVRC_QF_SHAD1_SHIFT;
		flags |= TAW_GET(tsp1, TSP_FogCtrl) << PVRC_QF_FOG1_SHIFT;
		if (TAW_GET(tsp1, TSP_UseAlpha))
			flags |= PVRC_QF_USE_ALPHA1;
		if (TAW_GET(tsp1, TSP_IgnoreTexA))
			flags |= PVRC_QF_IGNORE_TEXA1;
		if (TAW_GET(tsp, TSP_IgnoreTexA))
			flags |= PVRC_QF_IGNORE_TEXA0;
		if (TAW_GET(tsp, TSP_UseAlpha))
			flags |= PVRC_QF_USE_ALPHA0;
	}
	if (texture)
	{
		slot0 = f->r->slot(f->r->self, pp->texid, tsp);
		if (slot0 == PVRC_NONE)
			return false;
		slot1 = slot0;
		if (two && pp->texid1 != (u64)-1)
		{
			slot1 = f->r->slot(f->r->self, pp->texid1, tsp1);
			if (slot1 == PVRC_NONE)
				return false;
		}
	}
	if (TAW_GET(tsp, TSP_FilterMode) > 1 && texture && listType != TA_LIST_PUNCH_THROUGH
			&& TAW_GET(tcw, TCW_MipMapped) == 1)
	{
		trilinearAlpha = 0.25f * (TAW_GET(tsp, TSP_MipMapD) & 0x3);
		if (TAW_GET(tsp, TSP_FilterMode) == 2)
			trilinearAlpha = 1.f - trilinearAlpha;
	}
	if (palette)
		pal = fmt == TA_PIXEL_4BPP ? TAW_GET(tcw, TCW_PalSelect) << 4 : (TAW_GET(tcw, TCW_PalSelect) >> 4) << 8;
	r[0] = r[1] = r[2] = r[3] = 0;
	clip = cf_tile_clip(f, pp->tileclip, r);
	q[PVRC_Q_ISP] = TAW_FULL(pp->isp);
	q[PVRC_Q_TSP] = tsp;
	q[PVRC_Q_TCW] = tcw;
	q[PVRC_Q_PCW] = pcw;
	q[PVRC_Q_TSP1] = tsp1;
	q[PVRC_Q_TCW1] = tcw1;
	q[PVRC_Q_TEX] = slot0 | (slot1 << 16);
	q[PVRC_Q_FLAGS] = flags;
	q[PVRC_Q_TRILIN] = fbits(trilinearAlpha);
	q[PVRC_Q_PAL] = pal;
	cf_rect(f, clip, r, q + PVRC_Q_RECT);
	q[PVRC_Q_CLIP] = rect_word(cf_min(r[0], 0xffff), cf_min(r[1], 0xffff));
	q[PVRC_Q_CLIP + 1] = rect_word(cf_min(r[0] + r[2], 0xffff), cf_min(r[1] + r[3], 0xffff));
	q[PVRC_Q_FIRST] = pp->first;
	q[PVRC_Q_LIST] = listType;
	return true;
}

static void cf_mvp(const cf_pack *f, const ModifierVolumeParam *p, u32 *m)
{
	int r[4];
	r[0] = r[1] = r[2] = r[3] = 0;
	m[PVRC_M_ISP] = TAW_FULL(p->isp);
	cf_rect(f, cf_tile_clip(f, p->tileclip, r), r, m + PVRC_M_RECT);
	m[PVRC_M_FIRST] = p->first;
}

/* The header's constants: the raster renderers' fragment constants
 * (Vulkan's MakeFragmentUniforms(), Direct3D 12's MakeFragmentConstants()) */
static void cf_fog(u32 *out, const rend_context *rc)
{
	static const int shifts[4] = { 16, 8, 0, 24 };
	const u8 *fogVert = (const u8 *)&FOG_COL_VERT;	/* (BGRA) */
	const u8 *fogRam = (const u8 *)&FOG_COL_RAM;
	const u8 *density = (const u8 *)&FOG_DENSITY;
	int c;
	for (c = 0; c < 3; c++)
	{
		out[PVRC_F_FOG_RAM + c] = fbits(fogRam[2 - c] / 255.0f);
		out[PVRC_F_FOG_VERT + c] = fbits(fogVert[2 - c] / 255.0f);
	}
	out[PVRC_F_FOG_DEN] = fbits(density[1] / 128.0f * (float)pow(2.0, (double)(s8)density[0]));
	for (c = 0; c < 4; c++)
	{
		out[PVRC_F_CLAMP_MIN + c] = fbits(((rc->fog_clamp_min >> shifts[c]) & 0xFF) / 255.0f);
		out[PVRC_F_CLAMP_MAX + c] = fbits(((rc->fog_clamp_max >> shifts[c]) & 0xFF) / 255.0f);
	}
	/* (half a step under the reference: see the same in gles/gles.cpp) */
	out[PVRC_F_ALPHA_REF] = fbits(((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f);
}

u32 compute_frame_pack(const compute_frame_layout *layout, const compute_frame_renderer *r,
		u32 *out, u32 room, u32 *primsOut, u32 *why)
{
	const rend_context *rc = &pvrrc;
	const RenderPass *passes = TA_LIST_HEAD(rc->render_passes, RenderPass);
	const PolyParam *op = TA_LIST_HEAD(rc->global_param_op, PolyParam);
	const PolyParam *pt = TA_LIST_HEAD(rc->global_param_pt, PolyParam);
	const PolyParam *tr = TA_LIST_HEAD(rc->global_param_tr, PolyParam);
	const ModifierVolumeParam *mvo = TA_LIST_HEAD(rc->global_param_mvo, ModifierVolumeParam);
	const ModifierVolumeParam *mvt = TA_LIST_HEAD(rc->global_param_mvo_tr, ModifierVolumeParam);
	/* A frame of the GPU's TA decode (rc->gpu) has its vertices, indices and
	 * volume triangles in the decode's buffers, its draw groups' index
	 * ranges only there, and its 1/fZ_max: the numbering is
	 * PVRC_K_GPUSRC's (see PVRC_SRC_WORDS), and none of those buffers is
	 * copied */
	const bool gpu = rc->gpu;
	const u32 passCount = (u32)TA_LIST_USED(rc->render_passes, RenderPass);
	u32 nop, npt, ntr, nmvo, nmvt, polys, mvps;
	u32 oPoly, oMvp, oSrc, oVtx, oIdx, oMod, total;
	bool scalers, hscale;
	u32 across, drawnWidth, rttScale, tilesX, tilesY, i, p;
	int bx0, by0;
	float vs;
	RenderPass previous;
	cf_pack f;

	*why = CF_WHY_PASSES;
	if (passCount == 0 || passCount > PVRC_PASS_MAX)
		return 0;
	nop = (u32)TA_LIST_USED(rc->global_param_op, PolyParam);
	npt = (u32)TA_LIST_USED(rc->global_param_pt, PolyParam);
	ntr = (u32)TA_LIST_USED(rc->global_param_tr, PolyParam);
	nmvo = (u32)TA_LIST_USED(rc->global_param_mvo, ModifierVolumeParam);
	nmvt = (u32)TA_LIST_USED(rc->global_param_mvo_tr, ModifierVolumeParam);
	polys = nop + npt + ntr;
	mvps = nmvo + nmvt;
	oPoly = PVRC_F_WORDS;
	oMvp = oPoly + polys * PVRC_POLY_WORDS;
	oSrc = oMvp + mvps * PVRC_MVP_WORDS;
	oVtx = oSrc + (polys + mvps) * PVRC_SRC_WORDS;
	oIdx = gpu ? oVtx : oVtx + (u32)TA_LIST_USED(rc->verts, Vertex) * PVRC_VTX_WORDS;
	oMod = gpu ? oVtx : oIdx + (u32)TA_LIST_USED(rc->idx, u32);
	total = gpu ? oVtx : oMod + (u32)TA_LIST_USED(rc->modtrig, ModTriangle) * 9;
	*why = CF_WHY_ROOM;
	if (total > room)
		return 0;

	/* Through the X scaler (SCALER_CTL bit 16) the game draws twice as
	 * wide as the picture, and the kernels draw it so: the matrix, the
	 * base scissor and the width are twice the picture's across, and each
	 * pair of pixels drawn is resolved into one of the picture
	 * (PVRC_FF_HSCALE). A render to a texture keeps the matrix it has,
	 * which halves what is drawn. The chip's scalers are the core option
	 * "PVR2 scaler / flicker filter"'s: without it the frame is drawn as
	 * the raster renderers draw it. */
	scalers = !compute_frame_scalers_off && !rc->isRTT;
	hscale = scalers && SCALER_CTL.hscale;
	across = hscale ? 2 : 1;
	f.r = r;
	f.out = out;
	f.oMvp = oMvp;
	f.oSrc = oSrc;
	f.prim = 0;
	f.sources = 0;
	f.gpu = gpu;
	f.vp = *r->begin(r->self);
	f.vp.sx *= (float)across;
	f.vp.tx *= (float)across;
	drawnWidth = layout->width * across;
	rttScale = rc->isRTT ? (layout->rttScale > 1 ? layout->rttScale : 1) : 1;
	if (rc->isRTT)
	{
		/* only the pixel clip is written, as the renderers' renders to a
		 * texture have it; the texture is cleared around it */
		f.scissor[0] = rc->fb_X_CLIP.min * rttScale;
		f.scissor[1] = rc->fb_Y_CLIP.min * rttScale;
		f.scissor[2] = (rc->fb_X_CLIP.max + 1 - rc->fb_X_CLIP.min) * rttScale;
		f.scissor[3] = (rc->fb_Y_CLIP.max + 1 - rc->fb_Y_CLIP.min) * rttScale;
	}
	else
	{
		r->base_scissor(r->self, &f.scissor[0], &f.scissor[1], &f.scissor[2], &f.scissor[3]);
		f.scissor[0] *= (int)across;
		f.scissor[2] *= (int)across;
	}

	memset(out, 0, PVRC_F_WORDS * 4);
	bx0 = cf_min(f.scissor[0], (int)drawnWidth);
	by0 = cf_min(f.scissor[1], (int)layout->height);
	f.bx1 = cf_min(f.scissor[0] + f.scissor[2], (int)drawnWidth);
	f.by1 = cf_min(f.scissor[1] + f.scissor[3], (int)layout->height);
	f.baseRect0 = rect_word(bx0, by0);
	f.baseRect1 = rect_word(f.bx1, f.by1);

	/* the modifier volumes' parameters: where the volume each is in ends */
	for (i = 0; i < mvps; i++)
	{
		out[oMvp + i * PVRC_MVP_WORDS + PVRC_M_CLOSE_END] = PVRC_NONE;
		out[oMvp + i * PVRC_MVP_WORDS + PVRC_M_CLOSE_BY] = PVRC_NONE;
	}
	memset(&previous, 0, sizeof(previous));
	for (p = 0; p < passCount; p++)
	{
		const RenderPass *pass = &passes[p];
		u32 *pw = out + PVRC_F_PASS + p * PVRC_PASS_WORDS;
		pw[PVRC_P_OP] = cf_at(&f);
		cf_strips(&f, op, previous.op_count, pass->op_count, 0);
		pw[PVRC_P_MVO] = cf_at(&f);
		cf_volumes(&f, mvo, previous.mvo_count, pass->mvo_count, 0);
		pw[PVRC_P_PT] = cf_at(&f);
		cf_strips(&f, pt, previous.pt_count, pass->pt_count, nop);
		pw[PVRC_P_TR] = cf_at(&f);
		cf_strips(&f, tr, previous.tr_count, pass->tr_count, nop + npt);
		pw[PVRC_P_TR_END] = cf_at(&f);
		if (pass->mv_op_tr_shared)
		{
			pw[PVRC_P_MVT] = pw[PVRC_P_MVO];
			pw[PVRC_P_MVT_END] = pw[PVRC_P_PT];
		}
		else
		{
			pw[PVRC_P_MVT] = cf_at(&f);
			cf_volumes(&f, mvt, previous.mvo_tr_count, pass->mvo_tr_count, nmvo);
			pw[PVRC_P_MVT_END] = cf_at(&f);
		}
		pw[PVRC_P_FLAGS] = pass->autosort ? PVRC_PF_AUTOSORT : 0;
		previous = *pass;
	}
	/* (the decode's frame: its numbering keeps to the room, PVRC_F_PRIM_ROOM) */
	if (!gpu && (f.prim > compute_frame_max_prims()
			|| ((f.prim + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2) > layout->chunks))
		return 0;

	*why = CF_WHY_TEXTURES;
	for (i = 0; i < nop; i++)
		if (!cf_poly(&f, &op[i], TA_LIST_OPAQUE, out + oPoly + i * PVRC_POLY_WORDS))
			return 0;
	for (i = 0; i < npt; i++)
		if (!cf_poly(&f, &pt[i], TA_LIST_PUNCH_THROUGH, out + oPoly + (nop + i) * PVRC_POLY_WORDS))
			return 0;
	for (i = 0; i < ntr; i++)
		if (!cf_poly(&f, &tr[i], TA_LIST_TRANSLUCENT, out + oPoly + (nop + npt + i) * PVRC_POLY_WORDS))
			return 0;
	for (i = 0; i < nmvo; i++)
		cf_mvp(&f, &mvo[i], out + oMvp + i * PVRC_MVP_WORDS);
	for (i = 0; i < nmvt; i++)
		cf_mvp(&f, &mvt[i], out + oMvp + (nmvo + i) * PVRC_MVP_WORDS);

	/* The header */
	tilesX = (drawnWidth + (1u << layout->tileLog2) - 1) >> layout->tileLog2;
	tilesY = (layout->height + (1u << layout->tileLog2) - 1) >> layout->tileLog2;
	/* (twice the tiles across: the bins have room for a texture rendered to's, more than that) */
	*why = CF_WHY_ROOM;
	if ((u64)tilesX * tilesY * layout->chunks > layout->runs)
		return 0;
	out[PVRC_F_WIDTH] = drawnWidth;
	out[PVRC_F_HEIGHT] = layout->height;
	out[PVRC_F_TILES_X] = tilesX;
	out[PVRC_F_TILES_Y] = tilesY;
	out[PVRC_F_TILE_LOG2] = layout->tileLog2;
	out[PVRC_F_PRIMS] = gpu ? 0 : f.prim;
	out[PVRC_F_PASSES] = passCount;
	out[PVRC_F_FLAGS] = (layout->lastPicture ? (layout->lastScaled ? PVRC_FF_LAST_PICTURE | PVRC_FF_LAST_SCALED : PVRC_FF_LAST_PICTURE) : 0)
			| (hscale ? PVRC_FF_HSCALE : 0);
	if (scalers)
		out[PVRC_F_FLAGS] |= compute_frame_write_out(layout->height);
	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour; any other black (OITDrawer::Draw()), a texture rendered to too */
	out[PVRC_F_CLEAR] = rc->clearFramebuffer
			? (VO_BORDER_COL.Red | (VO_BORDER_COL.Green << 8) | (VO_BORDER_COL.Blue << 16) | 0xff000000u)
			: 0xff000000u;
	out[PVRC_F_SCISSOR] = (u32)bx0;
	out[PVRC_F_SCISSOR + 1] = (u32)by0;
	out[PVRC_F_SCISSOR + 2] = (u32)f.bx1;
	out[PVRC_F_SCISSOR + 3] = (u32)f.by1;
	/* (a render to a texture: the matrix is to the frame's own pixels, and
	 * the raster renderer's viewport is that many times larger) */
	vs = (float)rttScale;
	out[PVRC_F_XFORM] = fbits(f.vp.sx * vs);
	out[PVRC_F_XFORM + 1] = fbits(f.vp.sy * vs);
	out[PVRC_F_XFORM + 2] = fbits(f.vp.tx * vs);
	out[PVRC_F_XFORM + 3] = fbits(f.vp.ty * vs);
	out[PVRC_F_XFORM + 4] = fbits(f.vp.sz);
	/* Where the raster renderers clip a triangle with a vertex behind the
	 * eye, which the kernels clip there too (pvr_compute.h,
	 * rec_clip_plane()): their depth scale, over the kernels' 1/w scale */
	out[PVRC_F_ZCLIP] = fbits(gpu ? 1.f / f.vp.sz : pvr_depth_scale(rc->fZ_max) * (1.f / f.vp.sz));
	out[PVRC_F_GPU] = gpu ? (u32)((const u32 *)rc->idx.l.end - (const u32 *)rc->idx.l.base) : 0;
	out[PVRC_F_PRIM_ROOM] = compute_frame_max_prims();
	cf_fog(out, rc);
	out[PVRC_F_SHADE] = fbits(FPU_SHAD_SCALE.scale_factor / 256.f);
	out[PVRC_F_CHUNKS] = gpu ? 0 : (f.prim + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2;
	out[PVRC_F_SOURCES] = f.sources;
	out[PVRC_F_POOL] = layout->pool;
	out[PVRC_F_O_POLY] = oPoly;
	out[PVRC_F_O_MVP] = oMvp;
	out[PVRC_F_O_SRC] = oSrc;
	out[PVRC_F_O_VTX] = gpu ? 0 : oVtx;
	out[PVRC_F_O_IDX] = gpu ? 0 : oIdx;
	out[PVRC_F_O_MOD] = gpu ? 0 : oMod;
	out[PVRC_F_RUNS] = layout->runs;
	make_fog_texture((u8 *)(out + PVRC_F_FOG));

	/* The parse's buffers, with flat shading's colours where the raster
	 * renderers put them: only now, when nothing can leave the frame to
	 * them any more, for they change the vertices as well */
	if (!gpu)
	{
		r->provoking_vertices(r->self);
		memcpy(out + oVtx, TA_LIST_HEAD(rc->verts, Vertex), TA_LIST_USED(rc->verts, Vertex) * sizeof(Vertex));
		memcpy(out + oIdx, TA_LIST_HEAD(rc->idx, u32), TA_LIST_USED(rc->idx, u32) * 4);
		memcpy(out + oMod, TA_LIST_HEAD(rc->modtrig, ModTriangle), TA_LIST_USED(rc->modtrig, ModTriangle) * sizeof(ModTriangle));
	}

	*primsOut = gpu ? 0 : f.prim;
	*why = CF_WHY_NONE;
	return total;
}
