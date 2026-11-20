/* The tile accelerator's data parsed: a frame's 32-byte blocks, as the
 * game sent them, made into the vertices, indices, polygon parameters,
 * modifier volumes and render passes the renderers draw (ta_parse_vdrc()),
 * and the header pass over the same blocks that GPU-side TA processing
 * decodes from (ta_gpu_prepare()). */
#include <math.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "ta_index.h"
#include "ta.h"
#include "bg_plane.h"
#include "ta_ctx.h"
#include "pvr_mem.h"
#include "Renderer_if.h"
#include "rend/tile/pvr_tile.h"
#include "rend/ta_gpu.h"
#include "rend/sorter.h"
#include "rend/shadows.h"
#include "rend/modvol_clip.h"
#include <retro_atomic.h>
#include <features/features_cpu.h>

/* The larger and the smaller of two, as std::max() and std::min() have
 * them (which of two equal ones, and what a NaN does, included). */
#define ta_max(a, b) ((a) < (b) ? (b) : (a))
#define ta_min(a, b) ((b) < (a) ? (b) : (a))

/* (inlined: the vertex loops are made of these, each for one format) */
#if defined(_MSC_VER)
#define TA_FORCE __forceinline
#elif defined(__GNUC__)
#define TA_FORCE __inline__ __attribute__((always_inline))
#else
#define TA_FORCE TYPES_INLINE
#endif

/* A list of the frame's (ta_list.h) as the parse uses it: its elements of
 * type T */
#define TL_HEAD(lp, T) ((T *)(lp)->base)
#define TL_CUR(lp, T) ((T *)(lp)->cur)
#define TL_USED(lp, T) ((int)((T *)(lp)->cur - (T *)(lp)->base))
#define TL_AVAIL(lp, T) ((int)((T *)(lp)->end - (T *)(lp)->cur))
#define TL_BYTES(lp) ((int)((lp)->cur - (lp)->base))
#define TL_APPEND(lp, T) ((T *)ta_list_append((lp), sizeof(T)))
#define TL_APPENDN(lp, T, n) ((T *)ta_list_append((lp), (size_t)(n) * sizeof(T)))
#define TL_LAST(lp, T) ((T *)(lp)->cur - 1)
#define TL_POP(lp, T) ((lp)->cur -= sizeof(T))
#define TL_SET_CUR(lp, p) ((lp)->cur = (unsigned char *)(p))
#define TL_CLEAR(lp) ta_list_clear(lp)

/* A float's bits, and a float of bits */
static TA_FORCE s32 fbits(float f)
{
	s32 b;
	memcpy(&b, &f, sizeof(b));
	return b;
}

static TA_FORCE float bitsf(u32 b)
{
	float f;
	memcpy(&f, &b, sizeof(f));
	return f;
}

/* The texture of a polygon's words (the renderer's) */
#define get_texture(tsp, tcw, area) renderer_get_texture((tsp), (tcw), (area))

/* Read by the OpenGL renderer's bump maps (gltex.cpp) */
u32 decoded_colors[3][65536];

/* (cache state vars) */
static u32 tileclip_val = 0;

/* Whether flat strips are looked for (ta_set_flat_strips()), and the AND
 * and the OR of the bits of the depths of the strip being decoded so far,
 * for whether it is flat (PVR_FLAT_RATE_BIT()): the strip's end puts the
 * answer in its control word's single-rate bit. Looked for, the vertices
 * are decoded by another copy of the decoders, which does that; not, by
 * the ones that do not, and nothing on the way costs a thing - a
 * polygon's single-rate bit is then what the game left in that reserved
 * bit, and only the sprites' and the background's mean anything. */
static bool flat_strips_wanted;
static u32 strip_z_and = PVR_FLAT_AND_INIT;
static u32 strip_z_or = PVR_FLAT_OR_INIT;

/* (from the thread that renders, which is the one that parses) */
void ta_set_flat_strips(bool on)
{
	flat_strips_wanted = on;
}

bool ta_flat_strips(void)
{
	return flat_strips_wanted;
}

/* Whether the region array's table of tiles is built (rend_context's
 * region_tiles), from the same thread */
static bool region_table_wanted;

void ta_set_region_table(bool on)
{
	region_table_wanted = on;
}

/* Whether the raster renderers' modifier volume triangles are made
 * (rend_context's mv_clip), from the same thread */
static bool modvol_clip_wanted;

void ta_set_modvol_clip(bool on)
{
	modvol_clip_wanted = on;
}

/* A colour component given as a float, as the hardware takes it: clamped to
 * 0..1 (anything negative is 0; anything above 1, infinity and NaN are 1).
 *
 * NaN with either sign. The SH4's own NaN has the sign bit clear, and one
 * a game computes there counts as above 1; computed on this host it can
 * come out with the bit set, and was taken for negative: black where the
 * console has full brightness (upstream: Gaiamaster Kessen!'s textures). */
static TA_FORCE float saturate01(float val)
{
	const s32 bits = fbits(val);
	if (bits < 0)
		return (bits & 0x7fffffff) > 0x7f800000 ? 1.f : 0.f;
	return bits > 0x3f800000 ? 1.f : val;
}

/* ...and as eight bits. This used to go through a table of 65536 entries
 * looked up by the top 16 bits of the float, which leaves seven bits of
 * mantissa: any colour from a half up came out as the value for the start
 * of its 1/256th, one too low about half the time. */
static TA_FORCE u8 float_to_satu8(float val)
{
	return (u8)(saturate01(val) * 255.f);
}

/* ---- The tile accelerator's blocks, as the game sends them ----
 *
 * Every word of control (PCW, ISP_TSP, TSP, TCW) is the u32 it is, its
 * fields read with ta_words.h's TAW_GET(). Each block is 32 bytes. */
#if defined(_MSC_VER) || defined(__GNUC__)
#pragma pack(push, 1)
#endif

typedef struct Ta_Dma
{
	u32 pcw;
	u32 data_32[7];
} Ta_Dma;

typedef struct TA_PolyParam0
{
	u32 pcw, isp, tsp, tcw;
	u32 ign1, ign2;
	u32 SDMA_SIZE, SDMA_ADDR;
} TA_PolyParam0;

typedef struct TA_PolyParam1
{
	u32 pcw, isp, tsp, tcw;
	f32 FaceColorA, FaceColorR, FaceColorG, FaceColorB;
} TA_PolyParam1;

typedef TA_PolyParam0 TA_PolyParam2A;

typedef struct TA_PolyParam2B
{
	f32 FaceColorA, FaceColorR, FaceColorG, FaceColorB;
	f32 FaceOffsetA, FaceOffsetR, FaceOffsetG, FaceOffsetB;
} TA_PolyParam2B;

typedef struct TA_PolyParam3
{
	u32 pcw, isp;
	u32 tsp, tcw;	/* the first volume's */
	u32 tsp1, tcw1;	/* the second's */
	u32 SDMA_SIZE, SDMA_ADDR;
} TA_PolyParam3;

typedef TA_PolyParam3 TA_PolyParam4A;

typedef struct TA_PolyParam4B
{
	f32 FaceColor0A, FaceColor0R, FaceColor0G, FaceColor0B;
	f32 FaceColor1A, FaceColor1R, FaceColor1G, FaceColor1B;
} TA_PolyParam4B;

typedef struct TA_ModVolParam
{
	u32 pcw, isp;
	u32 ign[6];
} TA_ModVolParam;

typedef struct TA_SpriteParam
{
	u32 pcw, isp, tsp, tcw;
	u32 BaseCol, OffsCol;
	u32 SDMA_SIZE, SDMA_ADDR;
} TA_SpriteParam;

/* The vertices: the first 28 bytes after a block's control word, and the
 * second block of a 64-byte one. A 16-bit UV pair has V first. */
#ifdef MSB_FIRST
#define TA_UV16(u, v) u16 u, v
#else
#define TA_UV16(u, v) u16 v, u
#endif

typedef struct TA_Vertex0 { f32 xyz[3]; u32 ignore_1, ignore_2; u32 BaseCol; u32 ignore_3; } TA_Vertex0;
typedef struct TA_Vertex1 { f32 xyz[3]; f32 BaseA, BaseR, BaseG, BaseB; } TA_Vertex1;
typedef struct TA_Vertex2 { f32 xyz[3]; u32 ignore_1, ignore_2; f32 BaseInt; u32 ignore_3; } TA_Vertex2;
typedef struct TA_Vertex3 { f32 xyz[3]; f32 u, v; u32 BaseCol, OffsCol; } TA_Vertex3;
typedef struct TA_Vertex4 { f32 xyz[3]; TA_UV16(u, v); u32 ignore_1; u32 BaseCol, OffsCol; } TA_Vertex4;
typedef struct TA_Vertex5A { f32 xyz[3]; f32 u, v; u32 ignore_1, ignore_2; } TA_Vertex5A;
typedef struct TA_Vertex5B { f32 BaseA, BaseR, BaseG, BaseB; f32 OffsA, OffsR, OffsG, OffsB; } TA_Vertex5B;
typedef struct TA_Vertex6A { f32 xyz[3]; TA_UV16(u, v); u32 ignore_1, ignore_2, ignore_3; } TA_Vertex6A;
typedef TA_Vertex5B TA_Vertex6B;
typedef struct TA_Vertex7 { f32 xyz[3]; f32 u, v; f32 BaseInt, OffsInt; } TA_Vertex7;
typedef struct TA_Vertex8 { f32 xyz[3]; TA_UV16(u, v); u32 ignore_1; f32 BaseInt, OffsInt; } TA_Vertex8;
typedef struct TA_Vertex9 { f32 xyz[3]; u32 BaseCol0, BaseCol1; u32 ignore_1, ignore_2; } TA_Vertex9;
typedef struct TA_Vertex10 { f32 xyz[3]; f32 BaseInt0, BaseInt1; u32 ignore_1, ignore_2; } TA_Vertex10;
typedef struct TA_Vertex11A { f32 xyz[3]; f32 u0, v0; u32 BaseCol0, OffsCol0; } TA_Vertex11A;
typedef struct TA_Vertex11B { f32 u1, v1; u32 BaseCol1, OffsCol1; u32 ignore_1, ignore_2, ignore_3, ignore_4; } TA_Vertex11B;
typedef struct TA_Vertex12A { f32 xyz[3]; TA_UV16(u0, v0); u32 ignore_1; u32 BaseCol0, OffsCol0; } TA_Vertex12A;
typedef struct TA_Vertex12B { TA_UV16(u1, v1); u32 ignore_2; u32 BaseCol1, OffsCol1; u32 ignore_3, ignore_4, ignore_5, ignore_6; } TA_Vertex12B;
typedef struct TA_Vertex13A { f32 xyz[3]; f32 u0, v0; f32 BaseInt0, OffsInt0; } TA_Vertex13A;
typedef struct TA_Vertex13B { f32 u1, v1; f32 BaseInt1, OffsInt1; u32 ignore_1, ignore_2, ignore_3, ignore_4; } TA_Vertex13B;
typedef struct TA_Vertex14A { f32 xyz[3]; TA_UV16(u0, v0); u32 ignore_1; f32 BaseInt0, OffsInt0; } TA_Vertex14A;
typedef struct TA_Vertex14B { TA_UV16(u1, v1); u32 ignore_2; f32 BaseInt1, OffsInt1; u32 ignore_3, ignore_4, ignore_5, ignore_6; } TA_Vertex14B;
typedef struct TA_Sprite1A { f32 x0, y0, z0; f32 x1, y1, z1; f32 x2; } TA_Sprite1A;
typedef struct TA_Sprite1B { f32 y2, z2; f32 x3, y3; u32 ignore_1; TA_UV16(u0, v0); TA_UV16(u1, v1); TA_UV16(u2, v2); } TA_Sprite1B;
typedef struct TA_ModVolA { u32 pcw; f32 x0, y0, z0; f32 x1, y1, z1; f32 x2; } TA_ModVolA;
typedef struct TA_ModVolB { f32 y2, z2; u32 ignore[6]; } TA_ModVolB;

#if defined(_MSC_VER) || defined(__GNUC__)
#pragma pack(pop)
#endif

/* (their sizes, as the blocks have them) */
typedef char ta_block_sizes[(sizeof(Ta_Dma) == 32 && sizeof(TA_PolyParam0) == 32 && sizeof(TA_Vertex0) == 28
		&& sizeof(TA_Vertex4) == 28 && sizeof(TA_Vertex12B) == 32 && sizeof(TA_Sprite1B) == 32
		&& sizeof(TA_ModVolA) == 32 && sizeof(TA_ModVolB) == 32) ? 1 : -1];

/* A vertex's fields in a block: past its control word, and in the second
 * block of a 64-byte one */
#define VTX_A(T, data) ((T *)((u8 *)(data) + 4))
#define VTX_B(T, data) ((T *)((u8 *)(data) + 32))
#define CVTX_A(T, data) ((const T *)((const u8 *)(data) + 4))

#define TA_PCW(data) ((data)->pcw)
#define PCW_GET(w, f) TAW_GET((w), PCW_##f)

/* (no list open) */
#define ListType_None 0xFFFFFFFFu
#define SZ32 1
#define SZ64 2

/* vdec state variables */
static ModTriangle* lmr;

static PolyParam* CurrentPP;
static ta_list_t* CurrentPPlist;
/* ...and its strips the fast vertex loop knows to be whole and drawable */
static ta_strip_ok_t* CurrentStripOk;

/* TA state vars */
DECL_ALIGN(4) static u8 FaceBaseColor[4];
DECL_ALIGN(4) static u8 FaceOffsColor[4];
DECL_ALIGN(4) static u8 FaceBaseColor1[4];
DECL_ALIGN(4) static u8 FaceOffsColor1[4];
/* The same face colours as they were given, floats clamped to 0..1, for the
 * intensity modes: red, green, blue. */
static float FaceBaseColorF[3];
static float FaceOffsColorF[3];
static float FaceBaseColor1F[3];
static float FaceOffsColor1F[3];
static u32 SFaceBaseColor;
static u32 SFaceOffsColor;

typedef Ta_Dma* TaListFP(Ta_Dma* data, Ta_Dma* data_end);
typedef void TaPolyParamFP(void* ptr);

static TaListFP* TaCmd;

static u32 CurrentList;
static TaListFP* VertexDataFP;
/* (by list type as a parameter has it, three bits: 5 to 7 are no list, and can be asked for all the same) */
static bool ListIsFinished[8];

static TA_FORCE f32 f16(u16 v)
{
	return bitsf((u32)v << 16);
}

#define vdrc vd_rc

/* For the fast vertex loop (ta_poly_data): position straight to a vertex
 * that is already there, and a packed colour turned from the PowerVR's
 * blue, green, red, alpha to red, green, blue, alpha in one word where
 * the host has its bytes in that order. */
#define vert_fast_base \
	cv->x = vtx->xyz[0]; \
	cv->y = vtx->xyz[1]; \
	cv->z = vtx->xyz[2];

#if !defined(MSB_FIRST) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#define TA_VERT_SSE2 1
/* (the vertex's fields where that code takes them to be) */
typedef char ta_vert_layout[(offsetof(Vertex, col) == 12 && offsetof(Vertex, vtx_spc) == 16
		&& offsetof(Vertex, u) == 20 && offsetof(Vertex, v) == 24) ? 1 : -1];
#endif

/* A colour given as four floats - alpha, red, green, blue, the order the
 * game sends them in - to the vertex's red, green, blue, alpha bytes, all
 * four at once: float_to_satu8() of each, bit for bit, for every float
 * there is. The clamp is done on the floats' bits, as saturate01() does it. */
#if defined(TA_VERT_SSE2)
#define TA_COLOUR_SIMD 1
static TA_FORCE void float_argb_to_satu8(u8 *to, const void *argb)
{
	const __m128i bits = _mm_loadu_si128((const __m128i *)argb);
	/* above 1, infinity and NaN with either sign */
	const __m128i one = _mm_or_si128(_mm_cmpgt_epi32(bits, _mm_set1_epi32(0x3f800000)),
			_mm_cmpgt_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7fffffff)), _mm_set1_epi32(0x7f800000)));
	/* anything else with the sign set is 0 */
	__m128i c = _mm_andnot_si128(_mm_srai_epi32(bits, 31), bits);
	u32 rgba;
	c = _mm_or_si128(_mm_andnot_si128(one, c), _mm_and_si128(one, _mm_set1_epi32(0x3f800000)));
	c = _mm_cvttps_epi32(_mm_mul_ps(_mm_castsi128_ps(c), _mm_set1_ps(255.f)));
	c = _mm_shuffle_epi32(c, _MM_SHUFFLE(0, 3, 2, 1));
	c = _mm_packs_epi32(c, c);
	c = _mm_packus_epi16(c, c);
	rgba = (u32)_mm_cvtsi128_si32(c);
	memcpy(to, &rgba, 4);
}
#elif !defined(MSB_FIRST) && (defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64))
#include <arm_neon.h>
#define TA_COLOUR_SIMD 1
static TA_FORCE void float_argb_to_satu8(u8 *to, const void *argb)
{
	const int32x4_t bits = vreinterpretq_s32_u8(vld1q_u8((const u8 *)argb));
	/* above 1, infinity and NaN with either sign */
	const uint32x4_t one = vorrq_u32(vcgtq_s32(bits, vdupq_n_s32(0x3f800000)),
			vcgtq_s32(vandq_s32(bits, vdupq_n_s32(0x7fffffff)), vdupq_n_s32(0x7f800000)));
	/* anything else with the sign set is 0 */
	int32x4_t c = vbicq_s32(bits, vshrq_n_s32(bits, 31));
	uint16x4_t n;
	uint8x8_t b;
	u32 rgba;
	c = vbslq_s32(one, vdupq_n_s32(0x3f800000), c);
	n = vmovn_u32(vcvtq_u32_f32(vmulq_n_f32(vreinterpretq_f32_s32(c), 255.f)));
	b = vmovn_u16(vcombine_u16(n, n));   /* A R G B A R G B */
	rgba = vget_lane_u32(vreinterpret_u32_u8(vext_u8(b, b, 1)), 0);
	memcpy(to, &rgba, 4);
}
#endif

/* Colour conversions */
#define vert_packed_color_(to,src) \
	{ \
	u32 t=src; \
	to[2] = (u8)(t);t>>=8;\
	to[1] = (u8)(t);t>>=8;\
	to[0] = (u8)(t);t>>=8;\
	to[3] = (u8)(t);      \
	}

#ifdef MSB_FIRST
#define vert_fast_packed_color(to,src) vert_packed_color_(cv->to,vtx->src)
#else
#define vert_fast_packed_color(to,src) \
	{ \
		const u32 t = vtx->src; \
		const u32 w = (t & 0xff00ff00u) | ((t >> 16) & 0xffu) | ((t & 0xffu) << 16); \
		memcpy(cv->to, &w, 4); \
	}
#endif

/* The vertex formats' type numbers, by object control (ta_type_lut()) */
static u32 type_lut[256];
static bool type_lut_made;

/* ---- Lists, polygons and their strips ---- */

static TA_FORCE void SetTileClip(u32 xmin, u32 ymin, u32 xmax, u32 ymax)
{
	u32 rv = tileclip_val & 0xF0000000;
	rv |= xmin;		/* 6 bits */
	rv |= xmax << 6;	/* 6 bits */
	rv |= ymin << 12;	/* 5 bits */
	rv |= ymax << 17;	/* 5 bits */
	tileclip_val = rv;
}

static TA_FORCE void TileClipMode(u32 mode)
{
	tileclip_val = (tileclip_val & (~0xF0000000u)) | (mode << 28);
}

static TA_FORCE void StartList(u32 ListType)
{
	if (ListType == ListType_Opaque)
		CurrentPPlist = &vdrc.global_param_op.l, CurrentStripOk = &vdrc.strip_ok[0];
	else if (ListType == ListType_Punch_Through)
		CurrentPPlist = &vdrc.global_param_pt.l, CurrentStripOk = &vdrc.strip_ok[1];
	else if (ListType == ListType_Translucent)
		CurrentPPlist = &vdrc.global_param_tr.l, CurrentStripOk = &vdrc.strip_ok[2];

	CurrentPP = NULL;
}

/* Modifier volumes */
static void EndModVol(void)
{
	ta_list_t *list = NULL;
	if (CurrentList == ListType_Opaque_Modifier_Volume)
		list = &vdrc.global_param_mvo.l;
	else if (CurrentList == ListType_Translucent_Modifier_Volume)
		list = &vdrc.global_param_mvo_tr.l;
	else
		return;
	if (TL_USED(list, ModifierVolumeParam) > 0)
	{
		ModifierVolumeParam *p = TL_LAST(list, ModifierVolumeParam);
		p->count = TL_USED(&vdrc.modtrig.l, ModTriangle) - p->first;
		if (p->count == 0)
			TL_POP(list, ModifierVolumeParam);
	}
}

static void StartModVol(const TA_ModVolParam* param)
{
	ModifierVolumeParam *p = NULL;
	EndModVol();
	if (CurrentList == ListType_Opaque_Modifier_Volume)
		p = TL_APPEND(&vdrc.global_param_mvo.l, ModifierVolumeParam);
	else if (CurrentList == ListType_Translucent_Modifier_Volume)
		p = TL_APPEND(&vdrc.global_param_mvo_tr.l, ModifierVolumeParam);
	else
		return;
	p->isp = TAW_SET(param->isp, ISP_Modvol_VolumeLast, PCW_GET(param->pcw, Volume) != 0);
	p->first = TL_USED(&vdrc.modtrig.l, ModTriangle);
	p->tileclip = tileclip_val;
}

static TA_FORCE void AppendModVolVertexA(const TA_ModVolA* mvv)
{
	if (CurrentList != ListType_Opaque_Modifier_Volume && CurrentList != ListType_Translucent_Modifier_Volume)
		return;
	lmr = TL_APPEND(&vdrc.modtrig.l, ModTriangle);

	lmr->x0 = mvv->x0;
	lmr->y0 = mvv->y0;
	lmr->z0 = mvv->z0;

	lmr->x1 = mvv->x1;
	lmr->y1 = mvv->y1;
	lmr->z1 = mvv->z1;

	lmr->x2 = mvv->x2;
}

static TA_FORCE void AppendModVolVertexB(const TA_ModVolB* mvv)
{
	if (CurrentList != ListType_Opaque_Modifier_Volume && CurrentList != ListType_Translucent_Modifier_Volume)
		return;
	lmr->y2 = mvv->y2;
	lmr->z2 = mvv->z2;
}

static TA_FORCE void EndList(u32 ListType)
{
	if (CurrentPP != NULL && CurrentPP->count == 0)
		TL_POP(CurrentPPlist, PolyParam);
	CurrentPP = NULL;
	CurrentPPlist = NULL;
	CurrentStripOk = NULL;

	if (ListType == ListType_Opaque_Modifier_Volume
			|| ListType == ListType_Translucent_Modifier_Volume)
		EndModVol();
}

/* Polys -- update code on sprites if that gets updated too -- (the words
 * every polygon parameter starts with: TA_PolyParam0's) */
static void glob_param_bdc(const void *vpp)
{
	const TA_PolyParam0 *pp = (const TA_PolyParam0 *)vpp;
	PolyParam* d_pp = CurrentPP;
	if (d_pp == NULL || d_pp->count != 0)
	{
		d_pp = TL_APPEND(CurrentPPlist, PolyParam);
		CurrentPP = d_pp;
	}
	d_pp->first = TL_USED(&vdrc.verts.l, Vertex);
	d_pp->count = 0;

	d_pp->isp = pp->isp;
	d_pp->tsp = pp->tsp;
	d_pp->tcw = pp->tcw;
	d_pp->pcw = pp->pcw;
	d_pp->tileclip = tileclip_val;

	d_pp->texid = (u64)-1;

	if (PCW_GET(d_pp->pcw, Texture))
		d_pp->texid = get_texture(d_pp->tsp, d_pp->tcw, 0);

	d_pp->tsp1 = 0xFFFFFFFFu;
	d_pp->tcw1 = 0xFFFFFFFFu;
	d_pp->texid1 = (u64)-1;
}

#ifdef TA_COLOUR_SIMD
#define poly_float_color_(to,a,r,g,b) \
	float_argb_to_satu8(to, &(a));
#else
#define poly_float_color_(to,a,r,g,b) \
	to[0] = float_to_satu8(r);	\
	to[1] = float_to_satu8(g);	\
	to[2] = float_to_satu8(b);	\
	to[3] = float_to_satu8(a);
#endif

#define poly_float_color(to,src) \
	poly_float_color_(to,pp->src##A,pp->src##R,pp->src##G,pp->src##B) \
	to##F[0] = saturate01(pp->src##R); \
	to##F[1] = saturate01(pp->src##G); \
	to##F[2] = saturate01(pp->src##B);

/* Poly param handling */

/* Packed/Floating Color */
static void AppendPolyParam0(void* vpp)
{
	glob_param_bdc(vpp);
}

/* Intensity, no Offset Color */
static void AppendPolyParam1(void* vpp)
{
	const TA_PolyParam1* pp = (const TA_PolyParam1*)vpp;

	glob_param_bdc(pp);
	poly_float_color(FaceBaseColor,FaceColor);
}

/* Intensity, use Offset Color */
static void AppendPolyParam2A(void* vpp)
{
	glob_param_bdc(vpp);
}

static void AppendPolyParam2B(void* vpp)
{
	const TA_PolyParam2B* pp = (const TA_PolyParam2B*)vpp;

	poly_float_color(FaceBaseColor,FaceColor);
	poly_float_color(FaceOffsColor,FaceOffset);
}

/* Packed Color, with Two Volumes */
static void AppendPolyParam3(void* vpp)
{
	const TA_PolyParam3* pp = (const TA_PolyParam3*)vpp;

	glob_param_bdc(pp);

	CurrentPP->tsp1 = pp->tsp1;
	CurrentPP->tcw1 = pp->tcw1;
	if (PCW_GET(pp->pcw, Texture))
		CurrentPP->texid1 = get_texture(pp->tsp1, pp->tcw1, 1);
}

/* Intensity, with Two Volumes */
static void AppendPolyParam4A(void* vpp)
{
	const TA_PolyParam4A* pp = (const TA_PolyParam4A*)vpp;

	glob_param_bdc(pp);

	CurrentPP->tsp1 = pp->tsp1;
	CurrentPP->tcw1 = pp->tcw1;
	if (PCW_GET(pp->pcw, Texture))
		CurrentPP->texid1 = get_texture(pp->tsp1, pp->tcw1, 1);
}

static void AppendPolyParam4B(void* vpp)
{
	const TA_PolyParam4B* pp = (const TA_PolyParam4B*)vpp;

	poly_float_color(FaceBaseColor,FaceColor0);
	poly_float_color(FaceBaseColor1, FaceColor1);
}

static void AppendPolyParam2Full(void* vpp)
{
	Ta_Dma* pp = (Ta_Dma*)vpp;

	AppendPolyParam2A(&pp[0]);
	AppendPolyParam2B(&pp[1]);
}

static void AppendPolyParam4Full(void* vpp)
{
	Ta_Dma* pp = (Ta_Dma*)vpp;

	AppendPolyParam4A(&pp[0]);
	AppendPolyParam4B(&pp[1]);
}

/* Poly Strip handling: @flat, the strip's flatness is worked out */
static TA_FORCE void EndPolyStrip(int flat)
{
	PolyParam* const pp = CurrentPP;

	CurrentPP->count = TL_USED(&vdrc.verts.l, Vertex) - CurrentPP->first;

	if (CurrentPP->count > 0)
	{
		PolyParam* d_pp = TL_APPEND(CurrentPPlist, PolyParam);
		*d_pp = *CurrentPP;
		CurrentPP = d_pp;
		d_pp->first = TL_USED(&vdrc.verts.l, Vertex);
		d_pp->count = 0;
	}
	/* (after the copy, whose wide loads would otherwise wait for this
	 * narrow store to reach memory; the copy's strip sets its own) */
	if (flat)
	{
		pp->pcw = (pp->pcw & ~PVR_PCW_SINGLE_RATE) | PVR_FLAT_RATE_BIT(strip_z_and, strip_z_or);
		strip_z_and = PVR_FLAT_AND_INIT;
		strip_z_or = PVR_FLAT_OR_INIT;
	}
}

static TA_FORCE void update_fz(float z)
{
	if (fbits(vdrc.fZ_max) < fbits(z) && fbits(z) < 0x49800000)
		vdrc.fZ_max = z;
}

/* Poly Vertex handlers: append vertex base (each vertex format starts
 * with its position) */
static TA_FORCE Vertex* vert_cvt_base_(const f32 *xyz)
{
	f32 invW = xyz[2];
	Vertex* cv = TL_APPEND(&vdrc.verts.l, Vertex);
	cv->x = xyz[0];
	cv->y = xyz[1];
	cv->z = invW;
	update_fz(invW);
	return cv;
}

#define vert_cvt_base Vertex* cv = vert_cvt_base_(vtx->xyz)

/* Resume vertex base (for B part) */
#define vert_res_base \
	Vertex* cv = TL_LAST(&vdrc.verts.l, Vertex);

/* uv 16/32 */
#define vert_uv_32(u_name,v_name) \
	cv->u = (vtx->u_name);\
	cv->v = (vtx->v_name);

#define vert_uv_16(u_name,v_name) \
	cv->u = f16(vtx->u_name);\
	cv->v = f16(vtx->v_name);

#define vert_uv1_32(u_name,v_name) \
	cv->u1 = (vtx->u_name);\
	cv->v1 = (vtx->v_name);

#define vert_uv1_16(u_name,v_name) \
	cv->u1 = f16(vtx->u_name);\
	cv->v1 = f16(vtx->v_name);

#define vert_face_base_color1(baseint) \
	{ const float intensity = saturate01(vtx->baseint) * 255.f; \
	cv->col1[0] = (u8)(FaceBaseColor1F[0] * intensity);  \
	cv->col1[1] = (u8)(FaceBaseColor1F[1] * intensity);  \
	cv->col1[2] = (u8)(FaceBaseColor1F[2] * intensity);  \
	cv->col1[3] = FaceBaseColor1[3]; }

#define vert_face_offs_color1(offsint) \
	{ const float intensity = saturate01(vtx->offsint) * 255.f; \
	cv->spc1[0] = (u8)(FaceOffsColor1F[0] * intensity);  \
	cv->spc1[1] = (u8)(FaceOffsColor1F[1] * intensity);  \
	cv->spc1[2] = (u8)(FaceOffsColor1F[2] * intensity);  \
	cv->spc1[3] = FaceOffsColor1[3]; }

#ifdef TA_COLOUR_SIMD
#define vert_float_color_(to,a,r,g,b) \
	float_argb_to_satu8(to, &(a));
#else
#define vert_float_color_(to,a,r,g,b) \
	to[0] = float_to_satu8(r); \
	to[1] = float_to_satu8(g); \
	to[2] = float_to_satu8(b); \
	to[3] = float_to_satu8(a);
#endif

#define vert_packed_color(to,src) \
	vert_packed_color_(cv->to,vtx->src);

#define vert_float_color(to,src) \
	vert_float_color_(cv->to,vtx->src##A,vtx->src##R,vtx->src##G,vtx->src##B)

/* Intensity handling.
 *
 * Alpha doesn't get intensity. Intensity is clamped before the mul, as
 * well as on face color to work the same as the hardware. [Fixes red dog]
 * The face colour and the intensity are multiplied as the floats they are
 * and the product made eight bits, once. Both used to be made eight bits
 * first and multiplied as a/256 fractions, which lost up to two levels on
 * the way and could not reach 255: full intensity on a white face came out
 * 254. */
#define vert_face_base_color(baseint) \
	{ const float intensity = saturate01(vtx->baseint) * 255.f; \
	cv->col[0] = (u8)(FaceBaseColorF[0] * intensity);  \
	cv->col[1] = (u8)(FaceBaseColorF[1] * intensity);  \
	cv->col[2] = (u8)(FaceBaseColorF[2] * intensity);  \
	cv->col[3] = FaceBaseColor[3]; }

#define vert_face_offs_color(offsint) \
	{ const float intensity = saturate01(vtx->offsint) * 255.f; \
	cv->vtx_spc[0] = (u8)(FaceOffsColorF[0] * intensity);  \
	cv->vtx_spc[1] = (u8)(FaceOffsColorF[1] * intensity);  \
	cv->vtx_spc[2] = (u8)(FaceOffsColorF[2] * intensity);  \
	cv->vtx_spc[3] = FaceOffsColor[3]; }

/* (Non-Textured, Packed Color) */
static TA_FORCE void AppendPolyVertex0(const TA_Vertex0* vtx)
{
	vert_cvt_base;
	vert_packed_color(col,BaseCol);
}

/* (Non-Textured, Floating Color) */
static TA_FORCE void AppendPolyVertex1(const TA_Vertex1* vtx)
{
	vert_cvt_base;
	vert_float_color(col,Base);
}

/* (Non-Textured, Intensity) */
static TA_FORCE void AppendPolyVertex2(const TA_Vertex2* vtx)
{
	vert_cvt_base;
	vert_face_base_color(BaseInt);
}

/* (Textured, Packed Color) */
static TA_FORCE void AppendPolyVertex3(const TA_Vertex3* vtx)
{
	vert_cvt_base;
	vert_packed_color(col,BaseCol);
	vert_packed_color(vtx_spc,OffsCol);
	vert_uv_32(u,v);
}

/* (Textured, Packed Color, 16bit UV) */
static TA_FORCE void AppendPolyVertex4(const TA_Vertex4* vtx)
{
	vert_cvt_base;
	vert_packed_color(col,BaseCol);
	vert_packed_color(vtx_spc,OffsCol);
	vert_uv_16(u,v);
}

/* (Textured, Floating Color): colours are on B */
static TA_FORCE void AppendPolyVertex5A(const TA_Vertex5A* vtx)
{
	vert_cvt_base;
	vert_uv_32(u,v);
}

static TA_FORCE void AppendPolyVertex5B(const TA_Vertex5B* vtx)
{
	vert_res_base;
	vert_float_color(col,Base);
	vert_float_color(vtx_spc,Offs);
}

/* (Textured, Floating Color, 16bit UV): colours are on B */
static TA_FORCE void AppendPolyVertex6A(const TA_Vertex6A* vtx)
{
	vert_cvt_base;
	vert_uv_16(u,v);
}

static TA_FORCE void AppendPolyVertex6B(const TA_Vertex6B* vtx)
{
	vert_res_base;
	vert_float_color(col,Base);
	vert_float_color(vtx_spc,Offs);
}

/* (Textured, Intensity) */
static TA_FORCE void AppendPolyVertex7(const TA_Vertex7* vtx)
{
	vert_cvt_base;
	vert_face_base_color(BaseInt);
	vert_face_offs_color(OffsInt);
	vert_uv_32(u,v);
}

/* (Textured, Intensity, 16bit UV) */
static TA_FORCE void AppendPolyVertex8(const TA_Vertex8* vtx)
{
	vert_cvt_base;
	vert_face_base_color(BaseInt);
	vert_face_offs_color(OffsInt);
	vert_uv_16(u,v);
}

/* (Non-Textured, Packed Color, with Two Volumes) */
static TA_FORCE void AppendPolyVertex9(const TA_Vertex9* vtx)
{
	vert_cvt_base;
	vert_packed_color(col,BaseCol0);
	vert_packed_color(col1, BaseCol1);
}

/* (Non-Textured, Intensity, with Two Volumes) */
static TA_FORCE void AppendPolyVertex10(const TA_Vertex10* vtx)
{
	vert_cvt_base;
	vert_face_base_color(BaseInt0);
	vert_face_base_color1(BaseInt1);
}

/* (Textured, Packed Color, with Two Volumes) */
static TA_FORCE void AppendPolyVertex11A(const TA_Vertex11A* vtx)
{
	vert_cvt_base;
	vert_packed_color(col,BaseCol0);
	vert_packed_color(vtx_spc,OffsCol0);
	vert_uv_32(u0,v0);
}

static TA_FORCE void AppendPolyVertex11B(const TA_Vertex11B* vtx)
{
	vert_res_base;
	vert_packed_color(col1, BaseCol1);
	vert_packed_color(spc1, OffsCol1);
	vert_uv1_32(u1, v1);
}

/* (Textured, Packed Color, 16bit UV, with Two Volumes) */
static TA_FORCE void AppendPolyVertex12A(const TA_Vertex12A* vtx)
{
	vert_cvt_base;
	vert_packed_color(col,BaseCol0);
	vert_packed_color(vtx_spc,OffsCol0);
	vert_uv_16(u0,v0);
}

static TA_FORCE void AppendPolyVertex12B(const TA_Vertex12B* vtx)
{
	vert_res_base;
	vert_packed_color(col1, BaseCol1);
	vert_packed_color(spc1, OffsCol1);
	vert_uv1_16(u1, v1);
}

/* (Textured, Intensity, with Two Volumes) */
static TA_FORCE void AppendPolyVertex13A(const TA_Vertex13A* vtx)
{
	vert_cvt_base;
	vert_face_base_color(BaseInt0);
	vert_face_offs_color(OffsInt0);
	vert_uv_32(u0,v0);
}

static TA_FORCE void AppendPolyVertex13B(const TA_Vertex13B* vtx)
{
	vert_res_base;
	vert_face_base_color1(BaseInt1);
	vert_face_offs_color1(OffsInt1);
	vert_uv1_32(u1,v1);
}

/* (Textured, Intensity, 16bit UV, with Two Volumes) */
static TA_FORCE void AppendPolyVertex14A(const TA_Vertex14A* vtx)
{
	vert_cvt_base;
	vert_face_base_color(BaseInt0);
	vert_face_offs_color(OffsInt0);
	vert_uv_16(u0,v0);
}

static TA_FORCE void AppendPolyVertex14B(const TA_Vertex14B* vtx)
{
	vert_res_base;
	vert_face_base_color1(BaseInt1);
	vert_face_offs_color1(OffsInt1);
	vert_uv1_16(u1, v1);
}

/* One vertex of a 32-byte format to @cv: what AppendPolyVertex<n>() does
 * past getting the vertex from the list and looking at its depth. */
static TA_FORCE void vert_fast(u32 poly_type, const Ta_Dma* data, Vertex* cv)
{
	switch (poly_type)
	{
	case 0:
		{
			const TA_Vertex0* vtx = CVTX_A(TA_Vertex0, data);
			vert_fast_base;
			vert_fast_packed_color(col,BaseCol);
		}
		break;
	case 2:
		{
			const TA_Vertex2* vtx = CVTX_A(TA_Vertex2, data);
			vert_fast_base;
			vert_face_base_color(BaseInt);
		}
		break;
	case 3:
#ifdef TA_VERT_SSE2
		{
			/* The format most vertices are in, and its fields lie in the
			 * block nearly as they do in the vertex: three loads and
			 * three stores. Position goes over with the word after it,
			 * which lands where the colours then go; the two colours
			 * are turned from blue, green, red, alpha to red, green,
			 * blue, alpha together. */
			const u8* from = (const u8*)data;
			const __m128i red_blue = _mm_set1_epi32(0x00ff00ff);
			const __m128i xyz = _mm_loadu_si128((const __m128i*)(from + 4));
			const __m128i uv  = _mm_loadl_epi64((const __m128i*)(from + 16));
			__m128i colours   = _mm_loadl_epi64((const __m128i*)(from + 24));
			const __m128i rb  = _mm_and_si128(colours, red_blue);

			colours = _mm_or_si128(_mm_andnot_si128(red_blue, colours),
					_mm_or_si128(_mm_slli_epi32(rb, 16), _mm_srli_epi32(rb, 16)));
			_mm_storeu_si128((__m128i*)cv, xyz);
			_mm_storel_epi64((__m128i*)cv->col, colours);
			_mm_storel_epi64((__m128i*)&cv->u, uv);
		}
#else
		{
			const TA_Vertex3* vtx = CVTX_A(TA_Vertex3, data);
			vert_fast_base;
			vert_fast_packed_color(col,BaseCol);
			vert_fast_packed_color(vtx_spc,OffsCol);
			vert_uv_32(u,v);
		}
#endif
		break;
	case 4:
		{
			const TA_Vertex4* vtx = CVTX_A(TA_Vertex4, data);
			vert_fast_base;
			vert_fast_packed_color(col,BaseCol);
			vert_fast_packed_color(vtx_spc,OffsCol);
			vert_uv_16(u,v);
		}
		break;
	case 7:
		{
			const TA_Vertex7* vtx = CVTX_A(TA_Vertex7, data);
			vert_fast_base;
			vert_face_base_color(BaseInt);
			vert_face_offs_color(OffsInt);
			vert_uv_32(u,v);
		}
		break;
	case 8:
		{
			const TA_Vertex8* vtx = CVTX_A(TA_Vertex8, data);
			vert_fast_base;
			vert_face_base_color(BaseInt);
			vert_face_offs_color(OffsInt);
			vert_uv_16(u,v);
		}
		break;
	}
}

/* Sprites */
static TA_FORCE void AppendSpriteParam(const TA_SpriteParam* spr)
{
	PolyParam* d_pp = CurrentPP;
	if (CurrentPP == NULL || CurrentPP->count != 0)
	{
		if (CurrentPPlist == NULL)	/* wldkickspw */
			return;
		d_pp = TL_APPEND(CurrentPPlist, PolyParam);
		CurrentPP = d_pp;
	}

	d_pp->first = TL_USED(&vdrc.verts.l, Vertex);
	d_pp->count = 0;
	d_pp->isp = spr->isp;
	d_pp->tsp = spr->tsp;
	d_pp->tcw = spr->tcw;
	d_pp->pcw = spr->pcw;
	d_pp->pcw |= PVR_PCW_SINGLE_RATE;	/* a sprite is drawn at a single rate */
	d_pp->tileclip = tileclip_val;

	d_pp->texid = (u64)-1;

	if (PCW_GET(d_pp->pcw, Texture))
		d_pp->texid = get_texture(d_pp->tsp, d_pp->tcw, 0);
	d_pp->tcw1 = 0xFFFFFFFFu;
	d_pp->tsp1 = 0xFFFFFFFFu;
	d_pp->texid1 = (u64)-1;

	SFaceBaseColor = spr->BaseCol;
	SFaceOffsColor = spr->OffsCol;

	d_pp->isp ^= (u32)1 << ISP_TSP_CullMode_S;	/* the cull mode's low bit */
}

#define append_sprite(indx) \
	vert_packed_color_(cv[indx].col,SFaceBaseColor)\
	vert_packed_color_(cv[indx].vtx_spc,SFaceOffsColor)

#define sprite_uv(indx,u_name,v_name) \
	cv[indx].u = f16(sv->u_name);\
	cv[indx].v = f16(sv->v_name);

/* Sprite Vertex Handlers */
static TA_FORCE void AppendSpriteVertexA(const TA_Sprite1A* sv)
{
	Vertex* cv;
	/* A sprite's vertices with no polygon to belong to: its parameter
	 * came when no list was open and was dropped. */
	if (CurrentPP == NULL)
		return;
	CurrentPP->count = 4;

	cv = TL_APPENDN(&vdrc.verts.l, Vertex, 4);

	/* Fill static stuff */
	append_sprite(0);
	append_sprite(1);
	append_sprite(2);
	append_sprite(3);

	cv[2].x = sv->x0;
	cv[2].y = sv->y0;
	cv[2].z = sv->z0;
	update_fz(sv->z0);

	cv[3].x = sv->x1;
	cv[3].y = sv->y1;
	cv[3].z = sv->z1;
	update_fz(sv->z1);

	cv[1].x = sv->x2;
}

/* The plane of a sprite's three given corners, at the fourth (also the GPU
 * sort's, for the depth of a sprite's corner: see ta_gpu_sort_strips()) */
static void CaclulateSpritePlane(Vertex* base)
{
	const Vertex* A = &base[2];
	const Vertex* B = &base[3];
	const Vertex* C = &base[1];
	Vertex* P = &base[0];
	float AC_x = C->x - A->x, AC_y = C->y - A->y, AC_z = C->z - A->z,
		AB_x = B->x - A->x, AB_y = B->y - A->y, AB_z = B->z - A->z,
		AP_x = P->x - A->x, AP_y = P->y - A->y;

	float P_y = P->y, P_x = P->x, A_x = A->x, A_y = A->y, A_z = A->z;

	float AB_v = B->v - A->v, AB_u = B->u - A->u,
		AC_v = C->v - A->v, AC_u = C->u - A->u;

	float A_v = A->v, A_u = A->u;

	float k3 = (AC_x * AB_y - AC_y * AB_x);
	float k2 = (AP_x * AB_y - AP_y * AB_x) / k3;
	float k1;

	if (AB_x == 0)
		k1 = (P_y - A_y - k2 * AC_y) / AB_y;
	else
		k1 = (P_x - A_x - k2 * AC_x) / AB_x;

	P->z = A_z + k1 * AB_z + k2 * AC_z;
	P->u = A_u + k1 * AB_u + k2 * AC_u;
	P->v = A_v + k1 * AB_v + k2 * AC_v;
}

static TA_FORCE void AppendSpriteVertexB(const TA_Sprite1B* sv)
{
	PolyParam* d_pp;
	Vertex* cv;
	if (CurrentPP == NULL)
		return;
	cv = TL_LAST(&vdrc.verts.l, Vertex);
	cv -= 3;

	cv[1].y = sv->y2;
	cv[1].z = sv->z2;
	update_fz(sv->z2);

	cv[0].x = sv->x3;
	cv[0].y = sv->y3;

	sprite_uv(2, u0,v0);
	sprite_uv(3, u1,v1);
	sprite_uv(1, u2,v2);

	CaclulateSpritePlane(cv);

	update_fz(cv[0].z);

	d_pp = TL_APPEND(CurrentPPlist, PolyParam);
	*d_pp = *CurrentPP;
	CurrentPP = d_pp;
	d_pp->first = TL_USED(&vdrc.verts.l, Vertex);
	d_pp->count = 0;
}

static void VDECInit(void)
{
	rend_context_clear(&vd_rc);
	ta_strip_ok_clear(&vd_rc.strip_ok[0]);
	ta_strip_ok_clear(&vd_rc.strip_ok[1]);
	ta_strip_ok_clear(&vd_rc.strip_ok[2]);

	/* allocate storage for BG poly */
	TL_APPEND(&vd_rc.global_param_op.l, PolyParam);
	TL_APPENDN(&vd_rc.verts.l, Vertex, 4);
}

/* ---- The blocks routed to their decoders ---- */

static Ta_Dma* ta_main(Ta_Dma* data, Ta_Dma* data_end);

/* The vertex decoders this frame uses (poly_data_luts) */
static TaListFP* const* poly_data_lut;

static void ta_list_start(u32 new_list)
{
	CurrentList = new_list;
	StartList(CurrentList);
}

static Ta_Dma* NullVertexData(Ta_Dma* data, Ta_Dma* data_end)
{
	(void)data_end;
	INFO_LOG(PVR, "TA: Invalid state, ignoring VTX data\n");
	return data + SZ32;
}

/* A vertex of @poly_type: @part 0 all of it, 1 its first 32 bytes, 2 its
 * last 32 (with @data at them) */
static TA_FORCE Ta_Dma* ta_handle_poly(u32 poly_type, u32 part, Ta_Dma* data)
{
	u32 rv = 0;

	if (part == 2)
		TaCmd = ta_main;

	switch (poly_type)
	{
#define ver_32B_def(num) \
	case num: \
		AppendPolyVertex##num(VTX_A(TA_Vertex##num, data)); \
		rv = SZ32; \
		break;

		/* 32b , always in one pass :) */
		ver_32B_def(0)	/* (Non-Textured, Packed Color) */
		ver_32B_def(1)	/* (Non-Textured, Floating Color) */
		ver_32B_def(2)	/* (Non-Textured, Intensity) */
		ver_32B_def(3)	/* (Textured, Packed Color) */
		ver_32B_def(4)	/* (Textured, Packed Color, 16bit UV) */
		ver_32B_def(7)	/* (Textured, Intensity) */
		ver_32B_def(8)	/* (Textured, Intensity, 16bit UV) */
		ver_32B_def(9)	/* (Non-Textured, Packed Color, with Two Volumes) */
		ver_32B_def(10)	/* (Non-Textured, Intensity, with Two Volumes) */
#undef ver_32B_def

#define ver_64B_def(num) \
	case num: \
		/* process first half */ \
		if (part != 2) \
		{ \
			rv += SZ32; \
			AppendPolyVertex##num##A(VTX_A(TA_Vertex##num##A, data)); \
		} \
		/* process second half */ \
		if (part == 0) \
		{ \
			AppendPolyVertex##num##B(VTX_B(TA_Vertex##num##B, data)); \
			rv += SZ32; \
		} \
		else if (part == 2) \
		{ \
			AppendPolyVertex##num##B((TA_Vertex##num##B*)data); \
			rv += SZ32; \
		} \
		break;

		/* 64b , may be on 2 pass */
		ver_64B_def(5)	/* (Textured, Floating Color) */
		ver_64B_def(6)	/* (Textured, Floating Color, 16bit UV) */
		ver_64B_def(11)	/* (Textured, Packed Color, with Two Volumes) */
		ver_64B_def(12)	/* (Textured, Packed Color, 16bit UV, with Two Volumes) */
		ver_64B_def(13)	/* (Textured, Intensity, with Two Volumes) */
		ver_64B_def(14)	/* (Textured, Intensity, 16bit UV, with Two Volumes) */
#undef ver_64B_def
	}

	return data + rv;
}

/* The second halves of the 64-byte vertex formats, as TaCmd */
#define TA_POLY_HALF(num) \
static Ta_Dma* ta_poly_half_##num(Ta_Dma* data, Ta_Dma* data_end) \
{ \
	(void)data_end; \
	return ta_handle_poly(num, 2, data); \
}
TA_POLY_HALF(5)
TA_POLY_HALF(6)
TA_POLY_HALF(11)
TA_POLY_HALF(12)
TA_POLY_HALF(13)
TA_POLY_HALF(14)
#undef TA_POLY_HALF

static TaListFP* const ta_poly_halves[15] =
{
	NULL, NULL, NULL, NULL, NULL, ta_poly_half_5, ta_poly_half_6, NULL, NULL,
	NULL, NULL, ta_poly_half_11, ta_poly_half_12, ta_poly_half_13, ta_poly_half_14
};

/* Code Splitter/routers */

/* helper function for dummy dma's.Handles 32B and then switches to ta_main for next data */
static Ta_Dma* ta_modvolB_32(Ta_Dma* data, Ta_Dma* data_end)
{
	(void)data_end;
	AppendModVolVertexB((const TA_ModVolB*)data);
	TaCmd = ta_main;
	return data + SZ32;
}

static Ta_Dma* ta_mod_vol_data(Ta_Dma* data, Ta_Dma* data_end)
{
	if (data == data_end)
	{
		AppendModVolVertexA((const TA_ModVolA*)data);
		/* 32B more needed , 32B done :) */
		TaCmd = ta_modvolB_32;
		return data + SZ32;
	}
	/* all 64B done */
	AppendModVolVertexA((const TA_ModVolA*)data);
	AppendModVolVertexB(VTX_B(TA_ModVolB, data));
	return data + SZ64;
}

static Ta_Dma* ta_spriteB_data(Ta_Dma* data, Ta_Dma* data_end)
{
	(void)data_end;
	/* 32B more needed , 32B done :) */
	TaCmd = ta_main;
	AppendSpriteVertexB((const TA_Sprite1B*)data);
	return data + SZ32;
}

static Ta_Dma* ta_sprite_data(Ta_Dma* data, Ta_Dma* data_end)
{
	if (data == data_end)
	{
		/* 32B more needed , 32B done :) */
		TaCmd = ta_spriteB_data;
		AppendSpriteVertexA(VTX_A(TA_Sprite1A, data));
		return data + SZ32;
	}
	AppendSpriteVertexA(VTX_A(TA_Sprite1A, data));
	AppendSpriteVertexB(VTX_B(TA_Sprite1B, data));
	return data + SZ64;
}

/* The bits of the depth (1/w) of the vertex parameter at @data: every
 * kind has it at the same place, in its first 32 bytes */
static TA_FORCE u32 ta_vertex_z_bits(const Ta_Dma* data)
{
	u32 z;
	memcpy(&z, (const u8 *)data + 4 + 8, sizeof(z));
	return z;
}

/* The vertices of a polygon of @poly_type, @poly_size blocks each; @flat:
 * the strips' flatness is worked out as they go (flat_strips_wanted) */
static TA_FORCE Ta_Dma* ta_poly_data(u32 poly_type, u32 poly_size, int flat, Ta_Dma* data, Ta_Dma* data_end)
{
	u32 z_and, z_or;

	/* (a strip that starts here starts from nothing: whatever a strip the
	 * game never ended left is not part of it) */
	if (flat && CurrentPP != NULL && CurrentPP->first == (u32)TL_USED(&vdrc.verts.l, Vertex))
	{
		strip_z_and = PVR_FLAT_AND_INIT;
		strip_z_or = PVR_FLAT_OR_INIT;
	}

	/* The formats nearly every vertex comes in - packed colour or
	 * intensity, with or without a texture - when the vertex list has
	 * room for all there are here: the list's place and the farthest
	 * depth are kept in registers for the strip, where the loop below
	 * reads and writes them in memory for every vertex (a vertex's
	 * colours are stored as bytes, which the compiler has to take as
	 * possibly being those very variables). The same vertices come out. */
	if (poly_size == SZ32
			&& (poly_type == 0 || poly_type == 2 || poly_type == 3 || poly_type == 4 || poly_type == 7 || poly_type == 8)
			&& (ptrdiff_t)(data_end - data) < (ptrdiff_t)TL_AVAIL(&vdrc.verts.l, Vertex))
	{
		Vertex* const first = TL_CUR(&vdrc.verts.l, Vertex);
		Vertex* const head = TL_HEAD(&vdrc.verts.l, Vertex);
		Vertex* cv = first;
		PolyParam* pp = CurrentPP;
		s32 z_max = fbits(vdrc.fZ_max);
		/* Whether a vertex of the strip is left out of its indices
		 * (ta_vertex_bad()), from the words in hand: a strip begun here
		 * with none is marked known good, and make_index() reads none
		 * of its vertices again. One begun before, by another loop, is
		 * left to it. */
		ta_strip_ok_t* const ok = CurrentStripOk;
		PolyParam* const pp_head = CurrentPPlist != NULL ? TL_HEAD(CurrentPPlist, PolyParam) : NULL;
		const u32 run_first = (u32)(first - head);
		ta_bad_t bad = ta_bad_none();
		z_and = strip_z_and;
		z_or = strip_z_or;

		for (;;)
		{
			s32 z;
			bool end_of_strip;

			bad = ta_bad_add(bad, (const u8*)data + 4);
			vert_fast(poly_type, data, cv);
			z = fbits(cv->z);
			if (z_max < z && z < 0x49800000)
				z_max = z;
			if (flat)
			{
				z_and &= (u32)z;
				z_or |= (u32)z;
			}
			cv++;
			end_of_strip = PCW_GET(TA_PCW(data), EndOfStrip) != 0;
			data++;
			if (end_of_strip)
			{
				/* EndPolyStrip(), from what is in hand. The strip's
				 * parameters are read for the copy before its count is
				 * written: read after, the wide loads of the copy wait
				 * for that narrow store to reach memory. */
				const u32 used = (u32)(cv - head);
				const u32 count = used - pp->first;
				const u32 pcw = flat ? (pp->pcw & ~PVR_PCW_SINGLE_RATE) | PVR_FLAT_RATE_BIT(z_and, z_or) : 0;
				if (!ta_bad_any(bad) && pp->first >= run_first && ok != NULL)
					ta_strip_ok_set(ok, (u32)(pp - pp_head));
				bad = ta_bad_none();
				if (count > 0)
				{
					const PolyParam copy = *pp;
					PolyParam* d_pp = TL_APPEND(CurrentPPlist, PolyParam);
					pp->count = count;
					if (flat)
						pp->pcw = pcw;	/* (the copy's strip sets its own) */
					*d_pp = copy;
					d_pp->first = used;
					d_pp->count = 0;
					pp = d_pp;
				}
				else
					pp->count = count;
				if (flat)
				{
					z_and = PVR_FLAT_AND_INIT;
					z_or = PVR_FLAT_OR_INIT;
				}
				/* The next strip of the same polygon follows at once,
				 * more often than not: it is gone on with here, where
				 * ta_main would only come back for it. */
				if (data > data_end || PCW_GET(TA_PCW(data), ParaType) != ParamType_Vertex_Parameter)
					break;
			}
			else if (data > data_end)
				break;
		}

		TL_SET_CUR(&vdrc.verts.l, cv);
		vdrc.fZ_max = bitsf((u32)z_max);
		if (flat)
		{
			strip_z_and = z_and;
			strip_z_or = z_or;
		}
		CurrentPP = pp;
		TaCmd = ta_main;
		return data;
	}

	/* (with @flat, the strip's depths kept in registers here, and in
	 * memory only from one call to the next) */
	z_and = strip_z_and;
	z_or = strip_z_or;

	/* (SZ64 and 32 bytes: the first half) */
	if (poly_size != SZ32 && data == data_end)
		goto first_half;

	do
	{
		ta_handle_poly(poly_type, 0, data);
		if (flat)
		{
			z_and &= ta_vertex_z_bits(data);
			z_or |= ta_vertex_z_bits(data);
		}
		if (PCW_GET(TA_PCW(data), EndOfStrip))
			goto strip_end;
		data += poly_size;
	} while (poly_size == SZ32 ? data <= data_end : data < data_end);
	if (flat)
	{
		strip_z_and = z_and;
		strip_z_or = z_or;
	}

	if (poly_size != SZ32 && data == data_end)
	{
	first_half:
		ta_handle_poly(poly_type, 1, data);
		if (flat)
		{
			strip_z_and &= ta_vertex_z_bits(data);
			strip_z_or |= ta_vertex_z_bits(data);
		}
		if (PCW_GET(TA_PCW(data), EndOfStrip))
			EndPolyStrip(flat);
		TaCmd = ta_poly_halves[poly_type];

		data += SZ32;
	}

	return data;

strip_end:
	if (flat)
	{
		strip_z_and = z_and;
		strip_z_or = z_or;
	}
	TaCmd = ta_main;
	if (PCW_GET(TA_PCW(data), EndOfStrip))
		EndPolyStrip(flat);
	return data + poly_size;
}

/* ...one for each format, with flat strips looked for and not */
#define TA_POLY_DATA(num, size) \
static Ta_Dma* ta_poly_data_##num(Ta_Dma* data, Ta_Dma* data_end) \
{ \
	return ta_poly_data(num, size, 0, data, data_end); \
} \
static Ta_Dma* ta_poly_data_flat_##num(Ta_Dma* data, Ta_Dma* data_end) \
{ \
	return ta_poly_data(num, size, 1, data, data_end); \
}
TA_POLY_DATA(0, SZ32)
TA_POLY_DATA(1, SZ32)
TA_POLY_DATA(2, SZ32)
TA_POLY_DATA(3, SZ32)
TA_POLY_DATA(4, SZ32)
TA_POLY_DATA(5, SZ64)
TA_POLY_DATA(6, SZ64)
TA_POLY_DATA(7, SZ32)
TA_POLY_DATA(8, SZ32)
TA_POLY_DATA(9, SZ32)
TA_POLY_DATA(10, SZ32)
TA_POLY_DATA(11, SZ64)
TA_POLY_DATA(12, SZ64)
TA_POLY_DATA(13, SZ64)
TA_POLY_DATA(14, SZ64)
#undef TA_POLY_DATA

/* The vertex decoders by vertex type: [1] the ones that look for flat
 * strips */
static TaListFP* const poly_data_luts[2][15] =
{
	{
		ta_poly_data_0, ta_poly_data_1, ta_poly_data_2, ta_poly_data_3, ta_poly_data_4,
		ta_poly_data_5, ta_poly_data_6, ta_poly_data_7, ta_poly_data_8, ta_poly_data_9,
		ta_poly_data_10, ta_poly_data_11, ta_poly_data_12, ta_poly_data_13, ta_poly_data_14
	},
	{
		ta_poly_data_flat_0, ta_poly_data_flat_1, ta_poly_data_flat_2, ta_poly_data_flat_3, ta_poly_data_flat_4,
		ta_poly_data_flat_5, ta_poly_data_flat_6, ta_poly_data_flat_7, ta_poly_data_flat_8, ta_poly_data_flat_9,
		ta_poly_data_flat_10, ta_poly_data_flat_11, ta_poly_data_flat_12, ta_poly_data_flat_13, ta_poly_data_flat_14
	}
};

/* Second part of poly data */
static Ta_Dma* ta_poly_B_32_2(Ta_Dma* data, Ta_Dma* data_end)
{
	(void)data_end;
	AppendPolyParam2B(data);
	TaCmd = ta_main;
	return data + SZ32;
}

static Ta_Dma* ta_poly_B_32_4(Ta_Dma* data, Ta_Dma* data_end)
{
	(void)data_end;
	AppendPolyParam4B(data);
	TaCmd = ta_main;
	return data + SZ32;
}

/* By a polygon header's type: 32/64b , full */
static TaPolyParamFP* const ta_poly_param_lut[5] =
{
	AppendPolyParam0,
	AppendPolyParam1,
	AppendPolyParam2Full,
	AppendPolyParam3,
	AppendPolyParam4Full
};
/* 64b , first part */
static TaPolyParamFP* const ta_poly_param_a_lut[5] =
{
	NULL,
	NULL,
	AppendPolyParam2A,
	NULL,
	AppendPolyParam4A
};
/* 64b , second part */
static TaListFP* const ta_poly_param_b_lut[5] =
{
	NULL,
	NULL,
	ta_poly_B_32_2,
	NULL,
	ta_poly_B_32_4
};

/* Group_En bit seems ignored, thanks p1pkin */
#define group_EN() { TileClipMode(PCW_GET(TA_PCW(data), User_Clip)); }

static Ta_Dma* ta_main(Ta_Dma* data, Ta_Dma* data_end)
{
	do
	{
		switch (PCW_GET(TA_PCW(data), ParaType))
		{
		/* Control parameter (32B) */
		case ParamType_End_Of_List:
			if (CurrentList == ListType_None)
				CurrentList = PCW_GET(TA_PCW(data), ListType);
			else
				/* end a list only if it was really started */
				EndList(CurrentList);

			ListIsFinished[CurrentList] = true;
			CurrentList = ListType_None;
			VertexDataFP = NullVertexData;
			data += SZ32;
			break;
		case ParamType_User_Tile_Clip:
			SetTileClip(data->data_32[3] & 63, data->data_32[4] & 31, data->data_32[5] & 63, data->data_32[6] & 31);
			data += SZ32;
			break;
		case ParamType_Object_List_Set:
			INFO_LOG(PVR, "Unsupported list type: ParamType_Object_List_Set\n");	/* NAOMI Virtual on Oratorio Tangram */
			data += SZ32;
			break;

		/* Global Parameter: ModVolue 32B, PolyType 32B/64B */
		case ParamType_Polygon_or_Modifier_Volume:
			group_EN();
			if (CurrentList == ListType_None && PCW_GET(TA_PCW(data), ListType) > ListType_Punch_Through)
			{
				/* A list of a type there is none of (5 to 7): there
				 * is nowhere for its polygons to go, and the first
				 * of them used to be put there all the same -
				 * through a null pointer. Passed over, as upstream
				 * does (a0b50cd41, from a crash report). */
				WARN_LOG(PVR, "Invalid TA list type %d\n", (int)PCW_GET(TA_PCW(data), ListType));
				data += SZ32;
				break;
			}
			if (CurrentList == ListType_None)
				ta_list_start(PCW_GET(TA_PCW(data), ListType));	/* start a list ;) */

			if (IsModVolList(CurrentList))
			{
				/* accept mod data */
				StartModVol((const TA_ModVolParam*)data);
				VertexDataFP = ta_mod_vol_data;
				data += SZ32;
			}
			else
			{
				const u32 uid = type_lut[(u8)TA_PCW(data)];
				const u32 psz = uid >> 30;
				const u32 pdid = (u8)(uid);
				const u32 ppid = (u8)(uid >> 8);

				if (uid == 0xFFFFFFFF)
				{
					/* A polygon of a type there is no such thing as.
					 * Its numbers would index past the ends of the
					 * tables below: it is passed over. */
					WARN_LOG(PVR, "Invalid TA type %08x\n", TA_PCW(data));
					data += SZ32;
					continue;
				}
				VertexDataFP = poly_data_lut[pdid];

				if (data != data_end || psz == 1)
				{
					/* poly , 32B/64B */
					ta_poly_param_lut[ppid](data);
					data += psz;
				}
				else
				{
					/* 64b , first part */
					ta_poly_param_a_lut[ppid](data);
					/* Handle next 32B ;) */
					TaCmd = ta_poly_param_b_lut[ppid];
					data += SZ32;
				}
			}
			break;

		/* Sets Sprite info , and switches to ta_sprite_data function (32B) */
		case ParamType_Sprite:
			group_EN();
			if (CurrentList == ListType_None && PCW_GET(TA_PCW(data), ListType) > ListType_Punch_Through)
			{
				/* (as for a polygon) */
				WARN_LOG(PVR, "Invalid TA list type %d\n", (int)PCW_GET(TA_PCW(data), ListType));
				data += SZ32;
				break;
			}
			if (CurrentList == ListType_None)
				ta_list_start(PCW_GET(TA_PCW(data), ListType));	/* start a list ;) */

			VertexDataFP = ta_sprite_data;
			AppendSpriteParam((const TA_SpriteParam*)data);
			data += SZ32;
			break;

		/* Variable size */
		case ParamType_Vertex_Parameter:
			data = VertexDataFP(data, data_end);
			break;

		/* not handled: assumed to be 32B */
		case 3:
		case 6:
			/* Passed over, which is what was meant: die() ended
			 * the emulator before the line after it was reached. */
			WARN_LOG(PVR, "Unhandled TA parameter %08x\n", TA_PCW(data));
			data += SZ32;
			break;
		}
	}
	while (data <= data_end);
	return data;
}

/*
	Volume,Col_Type,Texture,Offset,Gouraud,16bit_UV
	0   0   0   (0) x   invalid Polygon Type 0  Polygon Type 0
	0   0   1   x   x   0       Polygon Type 0  Polygon Type 3
	0   0   1   x   x   1       Polygon Type 0  Polygon Type 4
	0   1   0   (0) x   invalid Polygon Type 0  Polygon Type 1
	0   1   1   x   x   0       Polygon Type 0  Polygon Type 5
	0   1   1   x   x   1       Polygon Type 0  Polygon Type 6
	0   2   0   (0) x   invalid Polygon Type 1  Polygon Type 2
	0   2   1   0   x   0       Polygon Type 1  Polygon Type 7
	0   2   1   0   x   1       Polygon Type 1  Polygon Type 8
	0   2   1   1   x   0       Polygon Type 2  Polygon Type 7
	0   2   1   1   x   1       Polygon Type 2  Polygon Type 8
	0   3   0   (0) x   invalid Polygon Type 0  Polygon Type 2
	0   3   1   x   x   0       Polygon Type 0  Polygon Type 7
	0   3   1   x   x   1       Polygon Type 0  Polygon Type 8
	1   0   0   (0) x   invalid Polygon Type 3  Polygon Type 9
	1   0   1   x   x   0       Polygon Type 3  Polygon Type 11
	1   0   1   x   x   1       Polygon Type 3  Polygon Type 12
	1   2   0   (0) x   invalid Polygon Type 4  Polygon Type 10
	1   2   1   x   x   0       Polygon Type 4  Polygon Type 13
	1   2   1   x   x   1       Polygon Type 4  Polygon Type 14
	1   3   0   (0) x   invalid Polygon Type 3  Polygon Type 10
	1   3   1   x   x   0       Polygon Type 3  Polygon Type 13
	1   3   1   x   x   1       Polygon Type 3  Polygon Type 14
	Sprites :
	(0) (0) 0 (0) (0) invalid Sprite  Sprite Type 0
	(0) (0) 1  x   (0) (1)     Sprite  Sprite Type 1
*/
/* The vertex type, 0-14 (0xFFFFFFFF: there is none) */
static u32 poly_data_type_id(u32 pcw)
{
	const u32 col_type = PCW_GET(pcw, Col_Type), uv16 = PCW_GET(pcw, UV_16bit);
	if (PCW_GET(pcw, Texture))
	{
		if (PCW_GET(pcw, Volume) == 0)
		{
			/* single volume */
			if (col_type == 0)
				return uv16 == 0 ? 3 : 4;	/* (Textured, Packed Color, 32b/16b uv) */
			else if (col_type == 1)
				return uv16 == 0 ? 5 : 6;	/* (Textured, Floating Color, 32b/16b uv) */
			return uv16 == 0 ? 7 : 8;		/* (Textured, Intensity, 32b/16b uv) */
		}
		/* two volumes */
		if (col_type == 0)
			return uv16 == 0 ? 11 : 12;		/* (Textured, Packed Color, [16bit UV,] with Two Volumes) */
		else if (col_type == 1)
			return 0xFFFFFFFF;
		return uv16 == 0 ? 13 : 14;		/* (Textured, Intensity, [16bit UV,] with Two Volumes) */
	}
	/* non textured */
	if (PCW_GET(pcw, Volume) == 0)
	{
		/* single volume */
		if (col_type == 0)
			return 0;	/* (Non-Textured, Packed Color) */
		else if (col_type == 1)
			return 1;	/* (Non-Textured, Floating Color) */
		return 2;		/* (Non-Textured, Intensity) */
	}
	/* two volumes */
	if (col_type == 0)
		return 9;		/* (Non-Textured, Packed Color, with Two Volumes) */
	else if (col_type == 1)
		return 0xFFFFFFFF;
	return 10;		/* (Non-Textured, Intensity, with Two Volumes) */
}

/* The header's type, 0-4 | 0x80 for 64 bytes (0xFFDDEEAA: there is none) */
static u32 poly_header_type_size(u32 pcw)
{
	const u32 col_type = PCW_GET(pcw, Col_Type);
	if (PCW_GET(pcw, Volume) == 0)
	{
		if (col_type < 2)
			return 0 | 0;		/* Polygon Type 0 -- SZ32 */
		else if (col_type == 2)
		{
			if (PCW_GET(pcw, Texture) && PCW_GET(pcw, Offset))
				return 2 | 0x80;	/* Polygon Type 2 -- SZ64 */
			return 1 | 0;		/* Polygon Type 1 -- SZ32 */
		}
		return 0 | 0;			/* Polygon Type 0 -- SZ32 (col_type 3) */
	}
	if (col_type == 0)
		return 3 | 0;			/* Polygon Type 3 -- SZ32 */
	else if (col_type == 2)
		return 4 | 0x80;		/* Polygon Type 4 -- SZ64 */
	else if (col_type == 3)
		return 3 | 0;			/* Polygon Type 3 -- SZ32 */
	return 0xFFDDEEAA;			/* (col_type 1 with two volumes) */
}

/* By object control, a polygon's header and vertex types: the vertex type
 * in the low byte, the header's in the next, its size in the top two bits */
const u32 *ta_type_lut(void)
{
	if (!type_lut_made)
	{
		u32 i;
		for (i = 0; i < 256; i++)
		{
			u32 rv = poly_data_type_id(i);
			const u32 type = poly_header_type_size(i);

			if (rv == 0xFFFFFFFF || type == 0xFFDDEEAA)
			{
				/* no such type: see where the table is read */
				type_lut[i] = 0xFFFFFFFF;
				continue;
			}
			if (type & 0x80)
				rv |= (u32)SZ64 << 30;
			else
				rv |= (u32)SZ32 << 30;

			rv |= (type & 0x7F) << 8;

			type_lut[i] = rv;
		}
		type_lut_made = true;
	}
	return type_lut;
}

static void vdec_init(void)
{
	int i;
	ta_type_lut();
	VDECInit();
	TaCmd = ta_main;
	CurrentList = ListType_None;
	ListIsFinished[0] = ListIsFinished[1] = ListIsFinished[2] = ListIsFinished[3] = ListIsFinished[4] = false;
	VertexDataFP = NullVertexData;
	memset(FaceBaseColor, 0xff, sizeof(FaceBaseColor));
	memset(FaceOffsColor, 0xff, sizeof(FaceOffsColor));
	memset(FaceBaseColor1, 0xff, sizeof(FaceBaseColor1));
	memset(FaceOffsColor1, 0xff, sizeof(FaceOffsColor1));
	for (i = 0; i < 3; i++)
		FaceBaseColorF[i] = FaceOffsColorF[i] = FaceBaseColor1F[i] = FaceOffsColor1F[i] = 1.f;
	SFaceBaseColor = 0;
	SFaceOffsColor = 0;
	lmr = NULL;
	CurrentPP = NULL;
	CurrentPPlist = NULL;
	poly_data_lut = poly_data_luts[flat_strips_wanted ? 1 : 0];
	strip_z_and = PVR_FLAT_AND_INIT;
	strip_z_or = PVR_FLAT_OR_INIT;
	/* Until the game gives a clipping rectangle, it is everything: 40
	 * by 15 tiles. It was whatever the last frame left, at first the
	 * one tile at the top left, and a polygon that asked to be clipped
	 * before any rectangle was given was clipped to that (upstream:
	 * Irides - Master of Blocks). */
	SetTileClip(0, 0, 39, 14);
	TileClipMode(0);
}

static void getRegionSettings(int pass_number, RenderPass *pass);
static void getRegionTileClipping(u32 *xmin, u32 *xmax, u32 *ymin, u32 *ymax);

int ta_parse_cnt = 0;

/* ---- The vertex index, invalid vertices left out and strips joined
 * where they can be ---- */

/* (the words poly_same_state() takes two at a time are side by side) */
typedef char ta_poly_layout[(offsetof(PolyParam, tcw) == offsetof(PolyParam, tsp) + 4
		&& offsetof(PolyParam, isp) == offsetof(PolyParam, pcw) + 4
		&& offsetof(PolyParam, tcw1) == offsetof(PolyParam, tsp1) + 4) ? 1 : -1];

/* Whether two polygons are drawn in the same way, so that their strips can
 * be joined into one: the same control, depth, shading and texture words,
 * the same clipping rectangle and mode, and the same second shading and
 * texture words (those of a polygon under two volumes; all ones on any
 * other). Two words are compared at a time and there is one test for all
 * of them. The single-rate bit is left out of the control word's
 * comparison: a draw group's is its strips' together (flat_groups()). */
static TA_FORCE bool poly_same_state(const PolyParam *a, const PolyParam *b)
{
	static const u32 ctl_mask_words[2] = { ~PVR_PCW_SINGLE_RATE, ~0u };	/* (pcw, then isp) */
	u64 a_tex, b_tex, a_ctl, b_ctl, a_tex1, b_tex1, ctl_mask;

	memcpy(&a_tex,  &a->tsp,  sizeof(a_tex));
	memcpy(&b_tex,  &b->tsp,  sizeof(b_tex));
	memcpy(&a_ctl,  &a->pcw,  sizeof(a_ctl));
	memcpy(&b_ctl,  &b->pcw,  sizeof(b_ctl));
	memcpy(&a_tex1, &a->tsp1, sizeof(a_tex1));
	memcpy(&b_tex1, &b->tsp1, sizeof(b_tex1));
	memcpy(&ctl_mask, ctl_mask_words, sizeof(ctl_mask));

	return ((a_tex ^ b_tex) | ((a_ctl ^ b_ctl) & ctl_mask) | (a_tex1 ^ b_tex1)
			| (u64)(a->tileclip ^ b->tileclip)) == 0;
}

/* Where flat strips are looked for: a draw group - polygons one after
 * another in a render pass's list drawn the same way (poly_same_state()),
 * which make_index() joins and the GPU's decode draws as one - is drawn at
 * a single rate only if every strip of it is flat (or a sprite, or the
 * background), and then all of its strips have the bit; strips with no
 * vertices count for nothing. The groups are the GPU's decode's (see
 * ta_gpu_prepare()), so that a frame is drawn at the same rates whichever
 * of the two made it; the translucent strips, which are drawn one by one,
 * too. One pass over the polygon parameters, none over the vertices. */
static void flat_groups(const ta_list_t *polys, int first, int end)
{
	PolyParam *const pp = TL_HEAD(polys, PolyParam);
	int i = first;
	while (i < end)
	{
		u32 bit = PVR_PCW_SINGLE_RATE;
		int j = i, last = i, k;
		if (pp[i].count == 0)
		{
			i++;
			continue;
		}
		for (; j < end; j++)
		{
			if (pp[j].count == 0)
				continue;
			if (!poly_same_state(&pp[last], &pp[j]))
				break;
			bit &= pp[j].pcw;
			last = j;
		}
		for (k = i; k < j; k++)
			if (pp[k].count != 0)
				pp[k].pcw = (pp[k].pcw & ~PVR_PCW_SINGLE_RATE) | bit;
		i = j;
	}
}

static void make_index(const ta_list_t *polys, int first, int end, bool merge, rend_context* ctx)
{
	const Vertex *vertices = TL_HEAD(&ctx->verts.l, Vertex);
	/* (the strips the vertex loop found drawable whole: ta_strip_ok) */
	const ta_strip_ok_t *ok = polys == &ctx->global_param_op.l ? &ctx->strip_ok[0]
			: polys == &ctx->global_param_pt.l ? &ctx->strip_ok[1]
			: polys == &ctx->global_param_tr.l ? &ctx->strip_ok[2] : NULL;
	/* Where the next index goes and where the list ends are kept for the
	 * whole of it, and given back to the list at the end (ta_index.h) */
	ta_index_out_t o;
	PolyParam *last_poly = NULL;
	PolyParam *const head = TL_HEAD(polys, PolyParam);
	const PolyParam *end_poly = &head[end];
	PolyParam *poly;

	o.base = TL_HEAD(&ctx->idx.l, u32);
	o.out = TL_CUR(&ctx->idx.l, u32);
	o.end = o.out + TL_AVAIL(&ctx->idx.l, u32);
	o.list = &ctx->idx.l;

	for (poly = &head[first]; poly != end_poly; poly++)
	{
		u32 first_index;
		int dupe = 0;
		if (merge
				&& last_poly != NULL
				&& last_poly->count != 0	/* (one that came to no indices has no last of them to repeat) */
				&& poly_same_state(poly, last_poly))
		{
			ta_index_put(&o, o.base[last_poly->first + last_poly->count - 1]);
			dupe = 1;
			first_index = last_poly->first;
		}
		else
		{
			last_poly = poly;
			first_index = (u32)(o.out - o.base);
		}
		ta_index_strip(&o, &vertices[poly->first], sizeof(Vertex), poly->first, poly->count, dupe, first_index,
				ok != NULL && ta_strip_ok_get(ok, (u32)(poly - head)));
		if (last_poly == poly)
		{
			poly->first = first_index;
			poly->count = (u32)(o.out - o.base) - first_index;
		}
		else
		{
			last_poly->count = (u32)(o.out - o.base) - last_poly->first;
			poly->count = 0;
		}
	}
	TL_SET_CUR(&ctx->idx.l, o.out);
}

static void fix_texture_bleeding(const ta_list_t *list)
{
	const PolyParam *pp_end = TL_CUR(list, PolyParam);
	const u32 *idx_base = TL_HEAD(&vd_rc.idx.l, u32);
	Vertex *vtx_base = TL_HEAD(&vd_rc.verts.l, Vertex);
	const PolyParam *pp;
	for (pp = TL_HEAD(list, PolyParam); pp != pp_end; pp++)
	{
		u32 first, last, idx, tex_width, tex_height;
		bool need_fixing = true;
		float z = 0.f;
		if (!PCW_GET(pp->pcw, Texture) || pp->count < 3)
			continue;
		/* Find polygons that are facing the camera (constant z) and only
		 * use 0 and 1 for U and V (some tolerance around 1 for SA2), then
		 * apply a half-pixel correction on U and V. */
		first = idx_base[pp->first];
		last = idx_base[pp->first + pp->count - 1];
		for (idx = first; idx <= last && need_fixing; idx++)
		{
			const Vertex *vtx = &vtx_base[idx];

			if (vtx->u != 0.f && (vtx->u <= 0.995f || vtx->u > 1.f))
				need_fixing = false;
			else if (vtx->v != 0.f && (vtx->v <= 0.995f || vtx->v > 1.f))
				need_fixing = false;
			else if (idx == first)
				z = vtx->z;
			else if (z != vtx->z)
				need_fixing = false;
		}
		if (!need_fixing)
			continue;
		tex_width = 8u << TAW_GET(pp->tsp, TSP_TexU);
		tex_height = 8u << TAW_GET(pp->tsp, TSP_TexV);
		for (idx = first; idx <= last; idx++)
		{
			Vertex *vtx = &vtx_base[idx];
			if (vtx->u > 0.995f)
				vtx->u = 1.f;
			vtx->u = (0.5f + vtx->u * (tex_width - 1)) / tex_width;
			if (vtx->v > 0.995f)
				vtx->v = 1.f;
			vtx->v = (0.5f + vtx->v * (tex_height - 1)) / tex_height;
		}
	}
}

/* The modifier volume triangles of the parse, to be cut (mv_clip_build()) */
static ModTriangle mv_clip_parse_get(void *ctx, u32 i)
{
	return ((const ModTriangle *)ctx)[i];
}

/* The opaque polygons of a frame's first render pass are not blended,
 * whatever their blend instructions say: they are drawn as "source x 1,
 * destination x 0". It is only in the passes that follow, a list
 * continuation, that an opaque polygon's instructions are gone by.
 * Upstream flycast sets them here in the same way, and the renderers go by
 * what they find. */
static void first_pass_opaque(void)
{
	PolyParam *pp = TL_HEAD(&vd_rc.global_param_op.l, PolyParam);
	int i;
	for (i = TL_USED(&vd_rc.global_param_op.l, PolyParam); i > 0; i--, pp++)
	{
		pp->tsp = TAW_SET(pp->tsp, TSP_SrcInstr, 1);
		pp->tsp = TAW_SET(pp->tsp, TSP_DstInstr, 0);
	}
}

/* Whether the render pass just parsed made nothing (@first: it is the first) */
static bool pass_empty(bool first)
{
	const RenderPass *last = first ? NULL : TL_LAST(&vd_rc.render_passes.l, RenderPass);
	return TL_USED(&vd_rc.global_param_op.l, PolyParam) == (first ? 0 : (int)last->op_count)
			&& TL_USED(&vd_rc.global_param_pt.l, PolyParam) == (first ? 0 : (int)last->pt_count)
			&& TL_USED(&vd_rc.global_param_tr.l, PolyParam) == (first ? 0 : (int)last->tr_count);
}

/* The data of the render pass, as far as there is any */
static void parse_pass(TA_context* ctx)
{
	Ta_Dma* ta_data     = (Ta_Dma*)ctx->rend.proc_start;
	Ta_Dma* ta_data_end = (Ta_Dma*)ctx->rend.proc_end - 1;

	while (ta_data <= ta_data_end)
		ta_data = TaCmd(ta_data, ta_data_end);
}

static bool ta_parse_vdrc_full(TA_context* ctx)
{
	bool rv = false, overrun;
	vd_ctx = ctx;
	vd_rc = vd_ctx->rend;

	ta_parse_cnt++;

	if (ctx->rend.isRTT || 0 == (ta_parse_cnt % (settings.pvr.ta_skip + 1)))
	{
		bool empty_context = true;
		int op_poly_count = 0;
		int pt_poly_count = 0;
		int tr_poly_count = 0;
		PolyParam *bgpp;
		u32 first, last, pass;

		vdec_init();

		bgpp = TL_HEAD(&vd_rc.global_param_op.l, PolyParam);
		if (PCW_GET(bgpp->pcw, Texture))
		{
			bgpp->texid = get_texture(bgpp->tsp, bgpp->tcw, 0);
			empty_context = false;
		}

		/* the render passes this render draws: all of them, or those its
		 * region array points to (ta_select_passes()) */
		first = ctx->rend.pass_first;
		last = ctx->rend.pass_last;
		if (last > ctx->tad.render_pass_count)
			last = ctx->tad.render_pass_count;
		if (first > last)
			first = 0;
		for (pass = first; pass <= last; pass++)
		{
			bool empty_pass;
			tactx_mark_rend(ctx, pass);
			vd_rc.proc_start = ctx->rend.proc_start;
			vd_rc.proc_end = ctx->rend.proc_end;

			parse_pass(ctx);

			if (ctx->rend.Overrun)
				break;

			if (pass == first)
				first_pass_opaque();

			empty_pass = pass_empty(pass == first);
			empty_context = empty_context && empty_pass;

			if (pass == first || !empty_pass)
			{
				RenderPass *render_pass = TL_APPEND(&vd_rc.render_passes.l, RenderPass);
				render_pass->op_count = TL_USED(&vd_rc.global_param_op.l, PolyParam);
				if (flat_strips_wanted)
				{
					flat_groups(&vd_rc.global_param_op.l, op_poly_count, TL_USED(&vd_rc.global_param_op.l, PolyParam));
					flat_groups(&vd_rc.global_param_pt.l, pt_poly_count, TL_USED(&vd_rc.global_param_pt.l, PolyParam));
					flat_groups(&vd_rc.global_param_tr.l, tr_poly_count, TL_USED(&vd_rc.global_param_tr.l, PolyParam));
				}
				make_index(&vd_rc.global_param_op.l, op_poly_count,
						render_pass->op_count, true, &vd_rc);
				op_poly_count = render_pass->op_count;
				render_pass->mvo_count = TL_USED(&vd_rc.global_param_mvo.l, ModifierVolumeParam);
				render_pass->pt_count = TL_USED(&vd_rc.global_param_pt.l, PolyParam);
				make_index(&vd_rc.global_param_pt.l, pt_poly_count,
						render_pass->pt_count, true, &vd_rc);
				pt_poly_count = render_pass->pt_count;
				render_pass->tr_count = TL_USED(&vd_rc.global_param_tr.l, PolyParam);
				make_index(&vd_rc.global_param_tr.l, tr_poly_count,
						render_pass->tr_count, false, &vd_rc);
				tr_poly_count = render_pass->tr_count;
				render_pass->mvo_tr_count = TL_USED(&vd_rc.global_param_mvo_tr.l, ModifierVolumeParam);
				/* (the region array's entries are this render's passes, from its first) */
				getRegionSettings((int)(pass - first), render_pass);
			}
		}
		rv = !empty_context;
	}
	overrun = ctx->rend.Overrun;
	if (overrun)
		WARN_LOG(PVR, "ERROR: TA context overrun\n");
	else if (screen_height > 480)
	{
		fix_texture_bleeding(&vd_rc.global_param_op.l);
		fix_texture_bleeding(&vd_rc.global_param_pt.l);
		fix_texture_bleeding(&vd_rc.global_param_tr.l);
	}
	if (rv && !overrun)
	{
		u32 xmin, xmax, ymin, ymax;
		getRegionTileClipping(&xmin, &xmax, &ymin, &ymax);
		vd_rc.fb_X_CLIP.min = ta_max(vd_rc.fb_X_CLIP.min, xmin);
		vd_rc.fb_X_CLIP.max = ta_min(vd_rc.fb_X_CLIP.max, xmax + 31);
		vd_rc.fb_Y_CLIP.min = ta_max(vd_rc.fb_Y_CLIP.min, ymin);
		vd_rc.fb_Y_CLIP.max = ta_min(vd_rc.fb_Y_CLIP.max, ymax + 31);
	}

	if (rv && !overrun && modvol_clip_wanted)
		mv_clip_build(&vd_rc, (u32)TL_USED(&vd_rc.modtrig.l, ModTriangle), mv_clip_parse_get, NULL,
				TL_HEAD(&vd_rc.modtrig.l, ModTriangle));

	vd_ctx->rend = vd_rc;
	vd_ctx = 0;

	ctx->rend.Overrun = overrun;

	return rv && !overrun;
}

/* The header pass check (at the end of the file) */
static bool ta_walk_check_frame(TA_context *ctx);
static bool ta_walk_checking;

bool ta_parse_vdrc(TA_context* ctx)
{
	if (ta_walk_checking)
		return ta_walk_check_frame(ctx);
	return ta_parse_vdrc_full(ctx);
}

/* For the check: the lists of the frame strip by strip, as the parse above
 * makes them and gives each render pass's make_index() - which then turns
 * them into ranges of indices, and joins some - kept in the order of the
 * passes (ta_walk_snap_full()). The parse's own steps, but for the indices,
 * the texture bleeding fix and the clipping rectangle; it leaves the frame
 * for the parse to make again (from the same data, the same). */
static void ta_walk_snap_full(u32 op_from, u32 pt_from, u32 tr_from);

static void ta_walk_full_strips(TA_context* ctx)
{
	const bool overrun = ctx->rend.Overrun;
	u32 op_poly_count = 0, pt_poly_count = 0, tr_poly_count = 0, pass;
	PolyParam *bgpp;
	vd_ctx = ctx;
	vd_rc = vd_ctx->rend;

	vdec_init();

	bgpp = TL_HEAD(&vd_rc.global_param_op.l, PolyParam);
	if (PCW_GET(bgpp->pcw, Texture))
		bgpp->texid = get_texture(bgpp->tsp, bgpp->tcw, 0);

	for (pass = 0; pass <= ctx->tad.render_pass_count; pass++)
	{
		bool empty_pass;
		tactx_mark_rend(ctx, pass);
		parse_pass(ctx);
		if (ctx->rend.Overrun)
			break;
		if (pass == 0)
			first_pass_opaque();
		empty_pass = pass_empty(pass == 0);
		if (pass == 0 || !empty_pass)
		{
			RenderPass *render_pass = TL_APPEND(&vd_rc.render_passes.l, RenderPass);
			ta_walk_snap_full(op_poly_count, pt_poly_count, tr_poly_count);
			render_pass->op_count = op_poly_count = TL_USED(&vd_rc.global_param_op.l, PolyParam);
			render_pass->pt_count = pt_poly_count = TL_USED(&vd_rc.global_param_pt.l, PolyParam);
			render_pass->tr_count = tr_poly_count = TL_USED(&vd_rc.global_param_tr.l, PolyParam);
		}
	}
	vd_ctx = 0;
	ctx->rend.Overrun = overrun;
}

/* A vertex in the chip's own format (the background's) */
static void decode_pvr_vertex(u32 base, u32 ptr, Vertex* cv)
{
	const u32 isp = vri(base);
	u32 col;

	/* XYZ are _always_ there :) */
	cv->x = vrf(ptr);
	ptr += 4;
	cv->y = vrf(ptr);
	ptr += 4;
	cv->z = vrf(ptr);
	ptr += 4;

	if (TAW_GET(isp, ISP_TSP_Texture))
	{
		/* Do texture , if any */
		if (TAW_GET(isp, ISP_TSP_UV_16b))
		{
			u32 uv = vri(ptr);
			cv->u = f16((u16)uv);
			cv->v = f16((u16)(uv >> 16));
			ptr += 4;
		}
		else
		{
			cv->u = vrf(ptr);
			ptr += 4;
			cv->v = vrf(ptr);
			ptr += 4;
		}
	}

	col = vri(ptr);
	ptr += 4;
	vert_packed_color_(cv->col,col);

	if (TAW_GET(isp, ISP_TSP_Offset))
	{
		/* Intensity color (can be missing too ;p) */
		col = vri(ptr);
		ptr += 4;
		vert_packed_color_(cv->vtx_spc, col);
	}
}

void FillBGP(TA_context* ctx)
{
	/* Render pre-code: the BG poly */
	u32 param_base = PARAM_BASE & 0xF00000;
	PolyParam* bgpp = TL_HEAD(&ctx->rend.global_param_op.l, PolyParam);
	Vertex* cv = TL_HEAD(&ctx->rend.verts.l, Vertex);
	bool PSVM = FPU_SHAD_SCALE.intensity_shadow != 0;	/* double parameters for volumes */
	/* The strip base. This is *not* VRAM_MASK on purpose: it fixes the
	 * NAOMI BIOS and quite a few NAOMI games. (They manage to set the
	 * render target over there as well, and that area is *not* written by
	 * the games, which write the params at 000000 instead of 800000: a
	 * hardware bug? param_base is 400000 and tag is 100000*4.) */
	u32 strip_base = (param_base + ISP_BACKGND_T.tag_address * 4) & 0x7FFFFF;
	/* the vertex size */
	u32 strip_vs = 3 + ISP_BACKGND_T.skip;
	u32 strip_vert_num = ISP_BACKGND_T.tag_offset;
	u32 vertex_ptr, bg_bits;
	float scale_x;
	f32 bg_depth, min_u, max_u, min_v, max_v, diff_u;
	int i;

	if (PSVM && ISP_BACKGND_T.shadow)
		strip_vs += ISP_BACKGND_T.skip;	/* 2x the size needed :p */
	strip_vs *= 4;
	/* the vertex pointer */
	vertex_ptr = strip_vert_num * strip_vs + strip_base + 3 * 4;

	bgpp->texid = (u64)-1;

	bgpp->isp = vri(strip_base);
	bgpp->tsp = vri(strip_base + 4);
	bgpp->tcw = vri(strip_base + 8);
	bgpp->tcw1 = 0xFFFFFFFFu;
	bgpp->tsp1 = 0xFFFFFFFFu;
	bgpp->texid1 = (u64)-1;
	bgpp->count = 4;
	bgpp->first = 0;
	bgpp->tileclip = 0;	/* disabled ! HA ~ */

	bgpp->isp = TAW_SET(bgpp->isp, ISP_TSP_DepthMode, 7);	/* -> this makes things AWFULLY slow .. sometimes */
	bgpp->isp = TAW_SET(bgpp->isp, ISP_TSP_CullMode, 0);	/* -> so that its not culled, or somehow else hidden ! */
	/* Set some pcw bits .. I should really get rid of pcw .. */
	bgpp->pcw = TAW_SET(bgpp->pcw, PCW_UV_16bit, TAW_GET(bgpp->isp, ISP_TSP_UV_16b));
	bgpp->pcw = TAW_SET(bgpp->pcw, PCW_Gouraud, TAW_GET(bgpp->isp, ISP_TSP_Gouraud));
	bgpp->pcw = TAW_SET(bgpp->pcw, PCW_Offset, TAW_GET(bgpp->isp, ISP_TSP_Offset));
	bgpp->pcw = TAW_SET(bgpp->pcw, PCW_Texture, TAW_GET(bgpp->isp, ISP_TSP_Texture));
	bgpp->pcw = TAW_SET(bgpp->pcw, PCW_Shadow, ISP_BACKGND_T.shadow);
	bgpp->pcw |= PVR_PCW_SINGLE_RATE;	/* so is the background */

	scale_x = (SCALER_CTL.hscale) ? 2.f : 1.f;	/* if AA hack the hacked pos value hacks */
	for (i = 0; i < 3; i++)
	{
		decode_pvr_vertex(strip_base, vertex_ptr, &cv[i]);
		vertex_ptr += strip_vs;
	}

	bg_bits = ISP_BACKGND_D.i & 0xFFFFFFF0;	/* ISP_BACKGND_D has only 28 bits */
	bg_depth = bitsf(bg_bits);
	/* And a little further away than it says. The chip does not tell depths
	 * this close apart, and what a game draws at the background's own depth
	 * shows; here it lost to the background by a rounding. Upstream's
	 * figure, and its list: the sky in Xtreme Sports, Blue Stinger's (JP)
	 * intro, and the videos of many Windows CE games, which were black. */
	bg_depth = ta_max(bg_depth - 1e-6f, 1e-11f);

	/* The plane through the three vertices, over the whole screen: see
	 * bg_plane.h. What is drawn is wider than the screen by 256 pixels on
	 * either side, as before. The four corners are written over the three
	 * vertices, so what is needed of those is taken first, and no more
	 * than is needed: the texture coordinates, and the colours only where
	 * they vary - without Gouraud shading the colour is the third vertex's
	 * everywhere, and the fourth corner is all that has to be given it. */
	{
		bg_plane plane;
		if (bg_plane_init(&plane, cv[0].x, cv[0].y, cv[1].x, cv[1].y, cv[2].x, cv[2].y))
		{
			float u[3], v[3];
			const bool gouraud = PCW_GET(bgpp->pcw, Gouraud) != 0;
			u8 col[3][4], spc[3][4];
			u[0] = cv[0].u; u[1] = cv[1].u; u[2] = cv[2].u;
			v[0] = cv[0].v; v[1] = cv[1].v; v[2] = cv[2].v;
			if (gouraud)
				for (i = 0; i < 3; i++)
				{
					memcpy(col[i], cv[i].col, 4);
					memcpy(spc[i], cv[i].vtx_spc, 4);
				}
			else
			{
				memcpy(cv[3].col, cv[2].col, 4);
				memcpy(cv[3].vtx_spc, cv[2].vtx_spc, 4);
			}
			for (i = 0; i < 4; i++)
			{
				const float x = (i & 1) ? 896.f : -256.f;
				const float y = (i & 2) ? 480.f : 0.f;
				cv[i].x = x * scale_x;
				cv[i].y = y;
				cv[i].z = bg_depth;
				cv[i].u = bg_plane_at(&plane, u[0], u[1], u[2], x, y);
				cv[i].v = bg_plane_at(&plane, v[0], v[1], v[2], x, y);
				if (gouraud)
				{
					int c;
					for (c = 0; c < 4; c++)
					{
						cv[i].col[c] = bg_plane_colour(&plane, col[0][c], col[1][c], col[2][c], x, y);
						cv[i].vtx_spc[c] = bg_plane_colour(&plane, spc[0][c], spc[1][c], spc[2][c], x, y);
					}
				}
			}
			return;
		}
	}
	/* the three are not a plane: the picture stretched over the screen, as it always was */
	min_u = ta_min(cv[0].u, ta_min(cv[1].u, cv[2].u));
	max_u = ta_max(cv[0].u, ta_max(cv[1].u, cv[2].u));
	if (max_u == 0.f)
		max_u = 1.f;
	diff_u = (max_u - min_u) * 0.4f;
	max_u += diff_u;
	min_u -= diff_u;
	min_v = ta_min(cv[0].v, ta_min(cv[1].v, cv[2].v));
	max_v = ta_max(cv[0].v, ta_max(cv[1].v, cv[2].v));
	if (max_v == 0.f)
		max_v = 1.f;
	cv[0].x = -256.f * scale_x;
	cv[0].y = 0.f;
	cv[0].z = bg_depth;
	cv[0].u = min_u;
	cv[0].v = min_v;

	cv[1].x = 896.f * scale_x;
	cv[1].y = 0.f;
	cv[1].z = bg_depth;
	cv[1].u = max_u;
	cv[1].v = min_v;

	cv[2].x = -256.f * scale_x;
	cv[2].y = 480.f;
	cv[2].z = bg_depth;
	cv[2].u = min_u;
	cv[2].v = max_v;

	cv[3] = cv[2];
	cv[3].x = 896.f * scale_x;
	cv[3].y = 480.f;
	cv[3].u = max_u;
	cv[3].v = max_v;
}

/* A region array tile's word (ta_structs.h's RegionArrayTile) */
#define REGION_X(w) (((w) >> 2) & 0x3F)
#define REGION_Y(w) (((w) >> 8) & 0x3F)
#define REGION_PRESORT(w) (((w) >> 29) & 1)
#define REGION_NO_Z_CLEAR(w) (((w) >> 30) & 1)
#define REGION_LAST(w) (((w) >> 31) & 1)

/* @table: the entries are also kept in the frame's table of tiles */
static TA_FORCE void walkRegionArray(bool table, u32 *xmin, u32 *xmax, u32 *ymin, u32 *ymax)
{
	u32 addr = REGION_BASE;
	const bool type1_tile = ((FPU_PARAM_CFG >> 21) & 1) == 0;
	int tile_size = (type1_tile ? 5 : 6) * 4;
	bool empty_first_region = true;
	u32 tile, walked = 0, entries = 0, entry = 0;
	int i;

	*xmin = 20;
	*xmax = 0;
	*ymin = 15;
	*ymax = 0;

	for (i = type1_tile ? 4 : 5; i > 0; i--)
		if ((vri(addr + i * 4) & 0x80000000) == 0)
		{
			empty_first_region = false;
			break;
		}
	if (empty_first_region)
		addr += tile_size;

	/* The array ends at the entry marked last. A game that starts a render
	 * without having written one leaves no such entry, and vri() wraps
	 * around video memory, so stop once all of it has been walked: the
	 * render thread must not spin on what the guest left in VRAM. */
	do {
		tile = vri(addr);
		if (table)
		{
			entry = pvr_region_entry(tile, entry, entries == 0);
			if (entries < REGION_TILES_MAX)
				vd_rc.region_tiles[entries] = entry;
			entries++;
		}
		*xmin = ta_min(*xmin, REGION_X(tile));
		*xmax = ta_max(*xmax, REGION_X(tile));
		*ymin = ta_min(*ymin, REGION_Y(tile));
		*ymax = ta_max(*ymax, REGION_Y(tile));
		if (type1_tile && REGION_PRESORT(tile))
			/* Windows CE weirdness */
			tile_size = 6 * 4;
		addr += tile_size;
		walked += tile_size;
	} while (!REGION_LAST(tile) && walked < VRAM_SIZE);
	if (table)
		vd_rc.region_tile_count = entries;

	*xmin *= 32;
	*xmax *= 32;
	*ymin *= 32;
	*ymax *= 32;
}

/* The tiles the region array lists, as a rectangle of pixels; and the
 * frame's table of them, when it is wanted */
static void getRegionTileClipping(u32 *xmin, u32 *xmax, u32 *ymin, u32 *ymax)
{
	if (region_table_wanted)
		walkRegionArray(true, xmin, xmax, ymin, ymax);
	else
		walkRegionArray(false, xmin, xmax, ymin, ymax);
}

/* Where the region array entry of a render pass is */
static TA_FORCE u32 getRegionTileAddr(int pass_number)
{
	u32 addr = REGION_BASE;
	const bool type1_tile = ((FPU_PARAM_CFG >> 21) & 1) == 0;
	int tile_size = (type1_tile ? 5 : 6) * 4;
	bool empty_first_region = true;
	int i;
	for (i = type1_tile ? 4 : 5; i > 0; i--)
		if ((vri(addr + i * 4) & 0x80000000) == 0)
		{
			empty_first_region = false;
			break;
		}
	if (empty_first_region)
		addr += tile_size;

	if (type1_tile && REGION_PRESORT(vri(addr)))
		/* Windows CE weirdness */
		tile_size = 6 * 4;

	return addr + pass_number * tile_size;
}

static void getRegionSettings(int pass_number, RenderPass *pass)
{
	const u32 addr = getRegionTileAddr(pass_number);
	const u32 tile = vri(addr);
	u32 op_mv;

	if (((FPU_PARAM_CFG >> 21) & 1) == 0)
		/* Type 1 region header type */
		pass->autosort = (ISP_FEED_CFG & 1) == 0;
	else
		/* Type 2 */
		pass->autosort = !REGION_PRESORT(tile);
	pass->z_clear = !REGION_NO_Z_CLEAR(tile);
	/* The region's opaque and translucent modifier volume lists can be the
	 * same list: the translucent volumes are then the opaque ones (Crazy
	 * Taxi, Daytona USA). Two empty ones are not that. */
	op_mv = vri(addr + 8);
	pass->mv_op_tr_shared = op_mv == vri(addr + 16) && (op_mv & 0x80000000) == 0;
}

/* A game can start two renders of one frame's data: Virtua Fighter 4 (a
 * NAOMI 2 game) sends the scene, continues the lists with the scene as the
 * water reflects it at another object list address, and then draws the
 * first to the screen and the second to a texture, with a region array for
 * each that points at the lists of one. Each render draws the render
 * passes its region array's entries point to: the pass of an entry is the
 * one whose lists start nearest below where its lists are. The entries of
 * a tile are the render's passes in order; a frame whose passes do not
 * come out that way, one pass at a time from some first one, is drawn
 * whole, as every frame was. */
void ta_select_passes(TA_context* ctx)
{
	const tad_context *tad = &ctx->tad;
	rend_context *rc = &ctx->rend;
	const u32 passes = tad->render_pass_count + 1;
	u32 p, base, tile_size, tile, t, entries, lists, first, e;

	rc->pass_first = 0;
	rc->pass_last = (u8)tad->render_pass_count;
	rc->keep = false;
	if (passes == 1 || rc->isRenderFramebuffer)
		return;
	/* (a frame from a save state was begun before it: where its first
	 * pass's lists went is not known, and is below the others') */
	for (p = 1; p < passes; p++)
		if (tad->ol_base[p] == TAD_OL_UNKNOWN)
			return;

	/* the entries of the first tile: how many there are */
	base = getRegionTileAddr(0);
	tile_size = getRegionTileAddr(1) - base;
	tile = vri(base);
	entries = 0;
	do {
		t = vri(base + entries * tile_size);
		if (REGION_X(t) != REGION_X(tile) || REGION_Y(t) != REGION_Y(tile))
			break;
		entries++;
	} while (!REGION_LAST(t) && entries <= passes);
	if (entries == 0 || entries >= passes)
		return;

	/* Each entry's lists, in the first tile that has one for it (a tile can
	 * have nothing in some pass): opaque, its modifier volumes,
	 * translucent, theirs, punch-through */
	lists = tile_size / 4 - 1;
	first = 0;
	for (e = 0; e < entries; e++)
	{
		u32 ptr = 0x80000000u, n, pass;
		for (n = 0; n < 64 && (ptr & 0x80000000u); n++)
		{
			const u32 at = base + n * entries * tile_size;
			u32 l;
			tile = vri(at);
			t = vri(at + e * tile_size);
			if (REGION_X(t) != REGION_X(tile) || REGION_Y(t) != REGION_Y(tile))
				break;
			for (l = 1; l <= lists && (ptr & 0x80000000u); l++)
				ptr = vri(at + e * tile_size + l * 4);
			/* (the tile's last entry ends the array) */
			t = vri(at + (entries - 1) * tile_size);
			if (REGION_LAST(t))
				break;
		}
		if (ptr & 0x80000000u)
			return;
		ptr &= 0xFFFFFCu;
		pass = passes;
		for (p = 0; p < passes; p++)
			if (tad->ol_base[p] != TAD_OL_UNKNOWN && tad->ol_base[p] <= ptr
					&& (pass == passes || tad->ol_base[p] >= tad->ol_base[pass]))
				pass = p;
		if (pass == passes && tad->ol_base[0] == TAD_OL_UNKNOWN)
			pass = 0;
		if (pass == passes)
			return;
		/* (two passes started at that address: which it is cannot be told) */
		for (p = 0; p < passes; p++)
			if (p != pass && tad->ol_base[p] == tad->ol_base[pass])
				return;
		if (e == 0)
			first = pass;
		else if (pass != first + e)
			return;
	}
	rc->pass_first = (u8)first;
	rc->pass_last = (u8)(first + entries - 1);
	rc->keep = true;
}

/* ---- The header pass: a frame from its control words alone ----
 *
 * The CPU side of GPU-side TA processing. As the blocks come in, the
 * control-word walk (ta.c, ta_set_walk()) notes where each block that is
 * not a vertex is (TA_context::walk). From that table and the parameters
 * themselves, this pass makes what the full parse above makes of the
 * frame but for the vertices: the polygon parameters with their texture
 * lookups, the modifier volumes, the render passes and their region
 * settings, the clipping rectangle, and the draw groups (the polygons
 * make_index() may join). Each parameter is a header here, with the
 * blocks of its vertices, how big they are and in which format, and the
 * place its first vertex goes. A vertex is never read: the vertices
 * between two entries are counted from where the entries are.
 *
 * What it cannot know without the vertices is where strips end - the
 * full parse makes a polygon parameter of each strip - and what
 * make_index() keeps of them. ta_walk_check_frame() reads the vertices'
 * end-of-strip bits to cut its headers into strips the full parse's way,
 * and compares the two. */

#define TA_WALK_SPRITE   15		/* a header's format: a sprite */
#define TA_WALK_NO_GROUP 0xFFFFFFFFu	/* a header's group: it has no vertices */
#define TA_WALK_BG       0xFFFFFFFFu	/* the header of a strip: the background's */
#define TA_WALK_MODVOL   0x80000000u	/* a run's header: a modifier volume's */
#define TA_WALK_UNENDED  0xFFFFFFFFu	/* a modifier volume's count: its end did not come */

typedef struct TaWalkHeader
{
	PolyParam pp;	/* first: its first vertex; count: its vertices, all strips together */
	u32 block;	/* where it is in the frame's data, in 32-byte blocks */
	u32 group;	/* its draw group */
	u8 list;	/* ListType_Opaque, _Punch_Through or _Translucent */
	u8 format;	/* its vertices' format, 0-14, or TA_WALK_SPRITE */
	u8 vblocks;	/* blocks a vertex takes, 1 or 2 */
	u8 pass;	/* its render pass */
} TaWalkHeader;

/* Vertex blocks one after the other, all of one header */
typedef struct TaWalkRun
{
	u32 block;	/* the first */
	u32 blocks;
	u32 vertex;	/* the first vertex they make (a modifier volume's: triangle) */
	u32 header;	/* whose: a header, or TA_WALK_MODVOL | a modifier volume */
} TaWalkRun;

/* Polygon parameters as each render pass gives them to make_index(), in
 * the order of the passes: opaque, punch-through, translucent */
typedef struct TaWalkStrips
{
	PolyParam *pp[3];
	u32 *header[3];	/* (the header pass's) the header each is a strip of */
	u32 count[3];
	u32 room[3];
} TaWalkStrips;

/* What a header's vertices are coloured with besides their own words, for
 * the GPU decode: the face colours of the intensity formats as they are
 * once its parameter is in (FaceBaseColor, FaceOffsColor and their
 * second-volume twins), and a sprite's two colours */
typedef struct TaWalkFace
{
	u32 col[4];	/* base, offset, base 1, offset 1: red, green, blue, alpha bytes */
	u32 f[12];	/* ...and as the clamped floats' bits, red, green, blue each */
	u32 sprite[2];	/* a sprite's base and offset colours, as sent */
} TaWalkFace;

typedef struct TaWalkFrame
{
	/* in: what the frame had before it was parsed */
	PolyParam bg_in;
	FB_X_CLIP_type x_clip_in;
	FB_Y_CLIP_type y_clip_in;

	TaWalkHeader *header;
	u32 headers, header_room;
	TaWalkRun *run;
	u32 runs, run_room;
	ModifierVolumeParam *mvo[2];	/* opaque, translucent */
	u32 mvos[2], mvo_room;
	RenderPass pass[10];
	u32 passes;
	PolyParam bg;
	u32 verts, modtrig;
	FB_X_CLIP_type x_clip;
	FB_Y_CLIP_type y_clip;
	bool rv;

	/* For the GPU decode (gpu): each header's colours (face, room for
	 * header_room), and whether the frame is one it does not do as the
	 * full parse does (gpu_bad): a list open at the end of a render pass,
	 * or the data ending in the middle of something */
	bool gpu;
	bool gpu_bad;
	TaWalkFace *face;

	/* the check's: the lists strip by strip, as the full parse has them */
	PolyParam *list[3];
	u32 *list_header[3];
	u32 list_room[3];
	TaWalkStrips snap;
} TaWalkFrame;

/* What vertices are, as VertexDataFP has it */
enum { WALK_NONE, WALK_POLY, WALK_SPRITE, WALK_MODVOL };
/* The next block is the second half of... */
enum { WALK_WHOLE, WALK_HALF_PARAM, WALK_HALF_VERTEX };

/* @strips: the vertices' end-of-strip bits are read, and the lists made
 * strip by strip as the full parse makes them (for the check); otherwise
 * a vertex is never looked at. The code is the full parse's, ta_main()
 * and its helpers, with each vertex counted where it would be decoded. */
typedef struct TaWalker
{
	TaWalkFrame *f;
	bool strips;
	const u8 *root;
	const u32 *entry;
	u32 entries, ei;
	const u32 *type_lut;

	u32 list;	/* the list that is open (CurrentList), ListType_None for none */
	int pplist;	/* where polygon parameters go (CurrentPPlist): 0-2, -1 nowhere */
	bool has_pp;	/* there is one being filled (CurrentPP) */
	bool counted;	/* ...and its count is set: a sprite between its halves */
	u32 pp;		/* (strips) its place in its list */
	u32 hdr;	/* its header */
	u32 kind, vblocks, half;
	u32 tileclip;
	u32 data_blocks;	/* blocks the frame's buffer has */
	u32 used[3];	/* polygon parameters in each list: strips (strips), or headers */
	u32 indexed[3];	/* how many of them earlier render passes took */
	u32 pass_header;	/* the first header since the last render pass */
	bool pass_made;	/* (headers) this pass made or dropped a polygon parameter, or made vertices */
	u32 next_group;
	bool fail;
	PolyParam spare;
	TaWalkFace fc;	/* (gpu) the colours of the moment */
} TaWalker;

static int w_pp_list(u32 l)
{
	return l == ListType_Opaque ? 0 : l == ListType_Punch_Through ? 1 : l == ListType_Translucent ? 2 : -1;
}

static bool w_eos(const TaWalker *w, u32 block)
{
	u32 pcw;
	memcpy(&pcw, w->root + (size_t)block * 32, sizeof(pcw));
	return PCW_GET(pcw, EndOfStrip) != 0;
}

/* (strips) CurrentPPlist->Append() */
static PolyParam *w_append(TaWalker *w)
{
	TaWalkFrame *f = w->f;
	const u32 l = (u32)w->pplist;
	if (w->used[l] == f->list_room[l])
	{
		w->fail = true;
		return &w->spare;
	}
	w->pp = w->used[l]++;
	f->list_header[l][w->pp] = w->hdr;
	return &f->list[l][w->pp];
}

static TaWalkHeader *w_new_header(TaWalker *w, u32 block, u32 format, u32 vb)
{
	TaWalkFrame *f = w->f;
	TaWalkHeader *h;
	if (f->headers == f->header_room)
	{
		w->fail = true;
		return NULL;
	}
	w->hdr = f->headers++;
	h = &f->header[w->hdr];
	memset(h, 0, sizeof(*h));
	h->block = block;
	h->group = TA_WALK_NO_GROUP;
	h->list = (u8)(w->pplist == 0 ? ListType_Opaque : w->pplist == 1 ? ListType_Punch_Through : ListType_Translucent);
	h->format = (u8)format;
	h->vblocks = (u8)vb;
	h->pp.first = f->verts;
	return h;
}

/* glob_param_bdc() and AppendSpriteParam(): which polygon parameter it is */
static void w_start_pp(TaWalker *w, const PolyParam *p)
{
	if (w->strips)
	{
		PolyParam *d = (!w->has_pp || w->counted) ? w_append(w) : &w->f->list[w->pplist][w->pp];
		*d = *p;
		w->f->list_header[w->pplist][w->pp] = w->hdr;
	}
	else
		w->used[w->pplist]++;
	w->pass_made = true;
	w->has_pp = true;
	w->counted = false;
}

/* poly_float_color(): face colour set @set from the four floats alpha,
 * red, green, blue at @wd */
static void w_face_set(TaWalker *w, u32 set, const u32 *wd)
{
	float c[4];
	int i;
	memcpy(c, wd, sizeof(c));
	w->fc.col[set] = (u32)float_to_satu8(c[1]) | ((u32)float_to_satu8(c[2]) << 8)
			| ((u32)float_to_satu8(c[3]) << 16) | ((u32)float_to_satu8(c[0]) << 24);
	for (i = 0; i < 3; i++)
	{
		const float v = saturate01(c[1 + i]);
		memcpy(&w->fc.f[set * 3 + i], &v, sizeof(v));
	}
}

/* The face colours of parameter @ppid at @block (AppendPolyParam1(),
 * 2B, 4B), whose second half, if it has one, is the next block
 * whatever comes between (ta_main() takes it as that) */
static void w_face_param(TaWalker *w, u32 block, u32 ppid, u32 room)
{
	const u32 *wd = (const u32 *)(w->root + (size_t)block * 32);
	if (ppid == 1)
		w_face_set(w, 0, wd + 4);
	else if (ppid == 2 || ppid == 4)
	{
		if (block + 1 >= room)
		{
			w->f->gpu_bad = true;
			return;
		}
		w_face_set(w, 0, wd + 8);
		w_face_set(w, ppid == 2 ? 1 : 2, wd + 12);
	}
}

static void w_poly_param(TaWalker *w, const Ta_Dma *d, u32 block, u32 ppid, u32 format, u32 vb)
{
	TaWalkHeader *h;
	const TA_PolyParam0 *pp0 = (const TA_PolyParam0 *)d;
	PolyParam *p;
	if (w->pplist < 0)
	{
		/* (there is always a list here: see ta_main()) */
		w->fail = true;
		return;
	}
	h = w_new_header(w, block, format, vb);
	if (h == NULL)
		return;
	p = &h->pp;
	p->isp = pp0->isp;
	p->tsp = pp0->tsp;
	p->tcw = pp0->tcw;
	p->pcw = pp0->pcw;
	p->tileclip = w->tileclip;
	p->texid = (u64)-1;
	if (PCW_GET(p->pcw, Texture))
		p->texid = get_texture(p->tsp, p->tcw, 0);
	p->tsp1 = 0xFFFFFFFFu;
	p->tcw1 = 0xFFFFFFFFu;
	p->texid1 = (u64)-1;
	if (ppid == 3 || ppid == 4)
	{
		/* (TA_PolyParam4A has them in the same place) */
		const TA_PolyParam3 *pp3 = (const TA_PolyParam3 *)d;
		p->tsp1 = pp3->tsp1;
		p->tcw1 = pp3->tcw1;
		if (PCW_GET(pp3->pcw, Texture))
			p->texid1 = get_texture(pp3->tsp1, pp3->tcw1, 1);
	}
	w_start_pp(w, p);
	if (w->f->gpu)
	{
		w_face_param(w, block, ppid, w->data_blocks);
		w->f->face[w->hdr] = w->fc;
	}
}

static void w_sprite_param(TaWalker *w, const Ta_Dma *d, u32 block)
{
	TaWalkHeader *h;
	const TA_SpriteParam *spr = (const TA_SpriteParam *)d;
	PolyParam *p;
	if ((!w->has_pp || w->counted) && w->pplist < 0)
		return;	/* (wldkickspw: dropped, and its vertices with it) */
	h = w_new_header(w, block, TA_WALK_SPRITE, 2);
	if (h == NULL)
		return;
	p = &h->pp;
	p->isp = spr->isp;
	p->tsp = spr->tsp;
	p->tcw = spr->tcw;
	p->pcw = spr->pcw | PVR_PCW_SINGLE_RATE;
	p->tileclip = w->tileclip;
	p->texid = (u64)-1;
	if (PCW_GET(p->pcw, Texture))
		p->texid = get_texture(p->tsp, p->tcw, 0);
	p->tcw1 = 0xFFFFFFFFu;
	p->tsp1 = 0xFFFFFFFFu;
	p->texid1 = (u64)-1;
	p->isp ^= (u32)1 << ISP_TSP_CullMode_S;
	w_start_pp(w, p);
	if (w->f->gpu)
	{
		w->fc.sprite[0] = spr->BaseCol;
		w->fc.sprite[1] = spr->OffsCol;
		w->f->face[w->hdr] = w->fc;
	}
}

/* (strips) EndPolyStrip() */
static void w_strip_end(TaWalker *w)
{
	TaWalkFrame *f = w->f;
	PolyParam *c = &f->list[w->pplist][w->pp];
	c->count = f->verts - c->first;
	if (c->count > 0)
	{
		const PolyParam copy = *c;
		PolyParam *d = w_append(w);
		*d = copy;
		d->first = f->verts;
		d->count = 0;
	}
}

/* AppendSpriteVertexB()'s new polygon parameter */
static void w_sprite_b(TaWalker *w)
{
	if (w->strips)
	{
		const PolyParam copy = w->f->list[w->pplist][w->pp];
		PolyParam *d = w_append(w);
		*d = copy;
		d->first = w->f->verts;
		d->count = 0;
	}
	w->counted = false;
}

static void w_end_modvol(TaWalker *w)
{
	TaWalkFrame *f = w->f;
	const int m = w->list == ListType_Opaque_Modifier_Volume ? 0 : w->list == ListType_Translucent_Modifier_Volume ? 1 : -1;
	ModifierVolumeParam *p;
	if (m < 0 || f->mvos[m] == 0)
		return;
	p = &f->mvo[m][f->mvos[m] - 1];
	p->count = f->modtrig - p->first;
	if (p->count == 0)
		f->mvos[m]--;
}

static void w_start_modvol(TaWalker *w, const Ta_Dma *d)
{
	TaWalkFrame *f = w->f;
	int m;
	const TA_ModVolParam *param = (const TA_ModVolParam *)d;
	ModifierVolumeParam *p;
	w_end_modvol(w);
	m = w->list == ListType_Opaque_Modifier_Volume ? 0 : w->list == ListType_Translucent_Modifier_Volume ? 1 : -1;
	if (m < 0)
		return;
	if (f->mvos[m] == f->mvo_room)
	{
		w->fail = true;
		return;
	}
	p = &f->mvo[m][f->mvos[m]++];
	p->isp = TAW_SET(param->isp, ISP_Modvol_VolumeLast, PCW_GET(param->pcw, Volume) != 0);
	p->first = f->modtrig;
	p->count = TA_WALK_UNENDED;
	p->tileclip = w->tileclip;
}

static void w_end_list(TaWalker *w)
{
	if (w->has_pp && !w->counted)
	{
		/* (PopLast()) */
		if (w->strips)
			w->used[w->pplist]--;
		w->pass_made = true;
	}
	w->has_pp = false;
	w->pplist = -1;
	if (w->list == ListType_Opaque_Modifier_Volume || w->list == ListType_Translucent_Modifier_Volume)
		w_end_modvol(w);
}

static void w_list_start(TaWalker *w, u32 l)
{
	w->list = l;
	if (w_pp_list(l) >= 0)
		w->pplist = w_pp_list(l);
	w->has_pp = false;
}

static void w_add_run(TaWalker *w, u32 block, u32 blocks, u32 vertex, u32 header)
{
	TaWalkFrame *f = w->f;
	TaWalkRun *r;
	if (f->runs == f->run_room)
	{
		w->fail = true;
		return;
	}
	r = &f->run[f->runs++];
	r->block = block;
	r->blocks = blocks;
	r->vertex = vertex;
	r->header = header;
}

/* The vertex blocks from @pos up to the next entry @nb, in a render
 * pass that ends at @end. A vertex of 64 bytes that the entry cuts in
 * two takes the entry for its second half, as ta_main() does. */
static u32 w_vertices(TaWalker *w, u32 pos, u32 nb, u32 end)
{
	TaWalkFrame *f = w->f;
	const u32 n = nb - pos;
	u32 next = nb, a, blocks = n, halves;
	bool cut;

	if (w->kind == WALK_NONE)
		return nb;	/* NullVertexData() */
	if (w->kind == WALK_POLY && w->vblocks == 1)
	{
		w_add_run(w, pos, n, f->verts, w->hdr);
		if (w->strips)
		{
			u32 b;
			for (b = pos; b < nb; b++)
			{
				f->verts++;
				if (w_eos(w, b))
					w_strip_end(w);
			}
		}
		else
			f->verts += n;
		f->header[w->hdr].pp.count += n;
		w->pass_made = true;
		/* (gpu) A parameter where a strip has not ended: the full parse
		 * takes it for a vertex, the decode for what it is */
		if (f->gpu && nb < end && !w_eos(w, nb - 1))
			f->gpu_bad = true;
		return nb;
	}
	halves = (n + 1) / 2;	/* first halves */
	cut = (n & 1) != 0;	/* the last of them has its second half at nb */
	if (cut)
	{
		if (nb < end)
		{
			if (w->ei < w->entries && w->entry[w->ei] == nb)
				w->ei++;
			next = nb + 1;
			blocks++;
		}
		else
			w->half = WALK_HALF_VERTEX;
	}
	if (w->kind == WALK_MODVOL)
	{
		const int m = w->list == ListType_Opaque_Modifier_Volume ? 0 : 1;
		w_add_run(w, pos, blocks, f->modtrig, TA_WALK_MODVOL | (f->mvos[m] ? f->mvos[m] - 1 : 0));
		f->modtrig += halves;
		return next;
	}
	if (w->kind == WALK_SPRITE)
	{
		if (!w->has_pp)
			return next;	/* AppendSpriteVertexA() and B(): nothing */
		w_add_run(w, pos, blocks, f->verts, w->hdr);
		for (a = 0; a < halves; a++)
		{
			if (w->strips)
				f->list[w->pplist][w->pp].count = 4;
			w->counted = true;
			f->verts += 4;
			f->header[w->hdr].pp.count += 4;
			if (a + 1 < halves || w->half != WALK_HALF_VERTEX)
				w_sprite_b(w);
		}
		w->pass_made = true;
		return next;
	}
	/* 64-byte polygon vertices */
	if (f->gpu && nb < end && !w_eos(w, cut ? nb - 1 : nb - 2))
		f->gpu_bad = true;	/* (as above) */
	w_add_run(w, pos, blocks, f->verts, w->hdr);
	if (w->strips)
		for (a = 0; a < halves; a++)
		{
			f->verts++;
			if (w_eos(w, pos + a * 2))
				w_strip_end(w);
		}
	else
		f->verts += halves;
	f->header[w->hdr].pp.count += halves;
	w->pass_made = true;
	return next;
}

static void w_set_tile_clip(TaWalker *w, u32 xmin, u32 ymin, u32 xmax, u32 ymax)
{
	w->tileclip = (w->tileclip & 0xF0000000) | xmin | (xmax << 6) | (ymin << 12) | (ymax << 17);
}

static void w_tile_clip_mode(TaWalker *w, u32 mode)
{
	w->tileclip = (w->tileclip & ~0xF0000000u) | (mode << 28);
}

/* The parameter at @pos, an entry */
static u32 w_param(TaWalker *w, u32 pos, u32 end)
{
	const Ta_Dma *d = (const Ta_Dma *)(w->root + (size_t)pos * 32);
	const u32 pcw = d->pcw;
	u32 uid, format, vb;

	w->ei++;
	switch (PCW_GET(pcw, ParaType))
	{
	case ParamType_End_Of_List:
		if (w->list == ListType_None)
			w->list = PCW_GET(pcw, ListType);
		else
			w_end_list(w);
		w->list = ListType_None;
		w->kind = WALK_NONE;
		return pos + 1;
	case ParamType_User_Tile_Clip:
		w_set_tile_clip(w, d->data_32[3] & 63, d->data_32[4] & 31, d->data_32[5] & 63, d->data_32[6] & 31);
		return pos + 1;
	case ParamType_Polygon_or_Modifier_Volume:
		w_tile_clip_mode(w, PCW_GET(pcw, User_Clip));
		if (w->list == ListType_None && PCW_GET(pcw, ListType) > ListType_Punch_Through)
			return pos + 1;
		if (w->list == ListType_None)
			w_list_start(w, PCW_GET(pcw, ListType));
		if (IsModVolList(w->list))
		{
			w_start_modvol(w, d);
			w->kind = WALK_MODVOL;
			w->vblocks = 2;
			return pos + 1;
		}
		uid = w->type_lut[(u8)pcw];
		if (uid == 0xFFFFFFFF)
			return pos + 1;
		format = (u8)uid;
		vb = format == 5 || format == 6 || (format >= 11 && format <= 14) ? 2 : 1;
		w->kind = WALK_POLY;
		w->vblocks = vb;
		w_poly_param(w, d, pos, (u8)(uid >> 8), format, vb);
		if ((uid >> 30) == SZ32)
			return pos + 1;
		if (pos + 1 < end)
		{
			if (w->ei < w->entries && w->entry[w->ei] == pos + 1)
				w->ei++;
			return pos + 2;
		}
		w->half = WALK_HALF_PARAM;
		return pos + 1;
	case ParamType_Sprite:
		w_tile_clip_mode(w, PCW_GET(pcw, User_Clip));
		if (w->list == ListType_None && PCW_GET(pcw, ListType) > ListType_Punch_Through)
			return pos + 1;
		if (w->list == ListType_None)
			w_list_start(w, PCW_GET(pcw, ListType));
		w->kind = WALK_SPRITE;
		w->vblocks = 2;
		w_sprite_param(w, d, pos);
		return pos + 1;
	case ParamType_Vertex_Parameter:
		/* (the walk never notes one: the table is not this data's) */
		w->fail = true;
		return end;
	default:
		/* Object_List_Set, and the two there are none of */
		return pos + 1;
	}
}

/* (the first pass's opaque polygons are not blended) */
#define W_OPAQUE_NOT_BLENDED(tsp) \
	((tsp) = TAW_SET(TAW_SET((tsp), TSP_SrcInstr, 1), TSP_DstInstr, 0))

/* ta_parse_vdrc()'s work for a render pass that ends here */
static void w_pass_end(TaWalker *w, u32 pass, bool *empty_context)
{
	TaWalkFrame *f = w->f;
	u32 i, l;
	bool empty_pass;
	RenderPass *r;
	const PolyParam *last[2];
	u32 last_group[2];

	/* A list still open: the full parse has the polygon parameter it
	 * is filling in the pass's lists, with no vertices, and makes
	 * indices for it (make_index() links it to the strip before);
	 * and a modifier volume's count is not set. The GPU decode does
	 * not do such a frame. */
	if (w->list != ListType_None)
		f->gpu_bad = true;

	if (pass == 0)
	{
		W_OPAQUE_NOT_BLENDED(f->bg.tsp);
		for (i = 0; i < f->headers; i++)
			if (f->header[i].list == ListType_Opaque)
				W_OPAQUE_NOT_BLENDED(f->header[i].pp.tsp);
		if (w->strips)
			for (i = 0; i < w->used[0]; i++)
				W_OPAQUE_NOT_BLENDED(f->list[0][i].tsp);
	}
	if (pass == 0)
		empty_pass = w->used[0] == 0 && w->used[1] == 0 && w->used[2] == 0;
	else if (w->strips)
	{
		const RenderPass *lp = &f->pass[f->passes - 1];
		empty_pass = w->used[0] == lp->op_count && w->used[1] == lp->pt_count && w->used[2] == lp->tr_count;
	}
	else
		empty_pass = !w->pass_made;
	w->pass_made = false;
	*empty_context = *empty_context && empty_pass;
	if (pass != 0 && empty_pass)
		return;

	r = &f->pass[f->passes++];
	r->op_count = w->used[0];
	r->mvo_count = f->mvos[0];
	r->pt_count = w->used[1];
	r->tr_count = w->used[2];
	r->mvo_tr_count = f->mvos[1];
	getRegionSettings((int)pass, r);

	if (w->strips)
		for (l = 0; l < 3; l++)
		{
			TaWalkStrips *s = &f->snap;
			for (i = w->indexed[l]; i < w->used[l] && s->count[l] < s->room[l]; i++, s->count[l]++)
			{
				s->pp[l][s->count[l]] = f->list[l][i];
				s->header[l][s->count[l]] = f->list_header[l][i];
			}
			w->indexed[l] = w->used[l];
		}

	/* The draw groups: polygons one after another in a list, drawn the
	 * same way (poly_same_state()), are what make_index() joins; within
	 * a pass, and the translucent ones never. The background is the
	 * first opaque polygon of the first. */
	last[0] = f->passes == 1 ? &f->bg : NULL;
	last[1] = NULL;
	last_group[0] = last_group[1] = 0;
	for (i = w->pass_header; i < f->headers; i++)
	{
		TaWalkHeader *h = &f->header[i];
		h->pass = (u8)(f->passes - 1);
		if (h->pp.count == 0)
			continue;
		l = (u32)w_pp_list(h->list);
		if (l == 2)
			h->group = w->next_group++;
		else
		{
			if (last[l] != NULL && poly_same_state(last[l], &h->pp))
				h->group = last_group[l];
			else
				h->group = w->next_group++;
			last[l] = &h->pp;
			last_group[l] = h->group;
		}
	}
	w->pass_header = f->headers;
}

static bool w_run(TaWalker *w, const TA_context *ctx, u32 count)
{
	TaWalkFrame *f = w->f;
	const tad_context *tad = &ctx->tad;
	bool empty_context = true;
	u32 pass;
	int i;

	w->root = tad->thd_root;
	w->entry = ctx->walk;
	w->entries = count;
	w->type_lut = ta_type_lut();
	w->list = ListType_None;
	w->pplist = -1;
	w->has_pp = w->counted = false;
	w->pp = w->hdr = 0;
	w->kind = WALK_NONE;
	w->vblocks = 1;
	w->half = WALK_WHOLE;
	w->tileclip = 0;
	w_set_tile_clip(w, 0, 0, 39, 14);
	w_tile_clip_mode(w, 0);
	w->indexed[0] = w->indexed[1] = w->indexed[2] = 0;
	w->pass_header = 0;
	w->pass_made = false;
	w->next_group = 1;
	w->fail = false;
	w->data_blocks = ctx->data_size / 32;
	f->gpu_bad = false;
	if (f->gpu)
	{
		/* vdec_init()'s */
		for (i = 0; i < 4; i++)
			w->fc.col[i] = 0xFFFFFFFFu;
		for (i = 0; i < 12; i++)
			w->fc.f[i] = 0x3F800000u;
		w->fc.sprite[0] = w->fc.sprite[1] = 0;
	}

	f->headers = f->runs = 0;
	f->mvos[0] = f->mvos[1] = 0;
	f->passes = 0;
	f->verts = 4;
	f->modtrig = 0;
	f->bg = f->bg_in;
	if (PCW_GET(f->bg.pcw, Texture))
	{
		f->bg.texid = get_texture(f->bg.tsp, f->bg.tcw, 0);
		empty_context = false;
	}
	w->used[0] = 1;
	w->used[1] = w->used[2] = 0;
	if (w->strips)
	{
		f->list[0][0] = f->bg;
		f->list_header[0][0] = TA_WALK_BG;
		f->snap.count[0] = f->snap.count[1] = f->snap.count[2] = 0;
	}

	for (pass = 0; pass <= tad->render_pass_count && !w->fail; pass++)
	{
		const u8 *from = pass == 0 ? tad->thd_root : tad->render_passes[pass - 1];
		const u8 *to = pass == tad->render_pass_count ? tad_end(tad) : tad->render_passes[pass];
		const u32 start = (u32)(from - w->root) / 32;
		const u32 end = to > from ? (u32)(to - w->root) / 32 : start;
		u32 pos = start;
		u32 lo = 0, hi = w->entries;

		/* the first entry from here on */
		while (lo < hi)
		{
			const u32 mid = (lo + hi) / 2;
			if (w->entry[mid] < start)
				lo = mid + 1;
			else
				hi = mid;
		}
		w->ei = lo;
		if (w->half != WALK_WHOLE && pos < end)
		{
			/* the second half of what the last pass ended in */
			if (w->half == WALK_HALF_VERTEX && w->kind == WALK_SPRITE && w->has_pp)
				w_sprite_b(w);
			if (w->ei < w->entries && w->entry[w->ei] == pos)
				w->ei++;
			pos++;
			w->half = WALK_WHOLE;
		}
		while (pos < end && !w->fail)
		{
			const u32 nb = w->ei < w->entries && w->entry[w->ei] < end ? w->entry[w->ei] : end;
			pos = nb > pos ? w_vertices(w, pos, nb, end) : w_param(w, pos, end);
		}
		if (!w->fail)
			w_pass_end(w, pass, &empty_context);
	}
	if (w->fail)
		return false;
	/* (the data ends in the middle of a parameter or vertex) */
	if (w->half != WALK_WHOLE)
		f->gpu_bad = true;

	f->rv = !empty_context;
	f->x_clip = f->x_clip_in;
	f->y_clip = f->y_clip_in;
	if (f->rv)
	{
		u32 xmin, xmax, ymin, ymax;
		walkRegionArray(false, &xmin, &xmax, &ymin, &ymax);
		f->x_clip.min = ta_max(f->x_clip.min, xmin);
		f->x_clip.max = ta_min(f->x_clip.max, xmax + 31);
		f->y_clip.min = ta_max(f->y_clip.min, ymin);
		f->y_clip.max = ta_min(f->y_clip.max, ymax + 31);
	}
	return true;
}

/* [0] the header pass's, [1] the check's, strip by strip; what the full
 * parse gave make_index(). Made by ta_walk_init() (when the check is turned
 * on), for frames of up to walk_data_size, and never grown. */
static TaWalkFrame walk_frame[2];
static TaWalkStrips walk_full;
static u32 walk_data_size;

void ta_walk_term(void)
{
	int i, l;
	for (i = 0; i < 2; i++)
	{
		TaWalkFrame *f = &walk_frame[i];
		free(f->header);
		free(f->run);
		free(f->mvo[0]);
		free(f->mvo[1]);
		free(f->face);
		for (l = 0; l < 3; l++)
		{
			free(f->list[l]);
			free(f->list_header[l]);
			free(f->snap.pp[l]);
			free(f->snap.header[l]);
		}
		memset(f, 0, sizeof(*f));
	}
	for (l = 0; l < 3; l++)
		free(walk_full.pp[l]);
	memset(&walk_full, 0, sizeof(walk_full));
	walk_data_size = 0;
}

bool ta_walk_init(void)
{
	const u32 size = ta_data_size();
	const u32 scale = size / TA_DATA_SIZE;
	/* (the room of the lists in TA_context::Alloc()) */
	u32 list_room[3];
	bool ok = true;
	int i, l;

	list_room[0] = 16384 * scale;
	list_room[1] = 5120 * scale;
	list_room[2] = (STRIPS_AS_PPARAMS ? 10240 : 8192) * scale;
	if (walk_data_size >= size)
		return true;
	ta_walk_term();
	for (i = 0; i < 2; i++)
	{
		TaWalkFrame *f = &walk_frame[i];
		f->header_room = TA_WALK_MAX * scale;
		f->run_room = TA_WALK_MAX * scale + 16;
		f->mvo_room = 4096 * scale;
		f->header = (TaWalkHeader *)malloc(f->header_room * sizeof(TaWalkHeader));
		f->run = (TaWalkRun *)malloc(f->run_room * sizeof(TaWalkRun));
		f->mvo[0] = (ModifierVolumeParam *)malloc(f->mvo_room * sizeof(ModifierVolumeParam));
		f->mvo[1] = (ModifierVolumeParam *)malloc(f->mvo_room * sizeof(ModifierVolumeParam));
		ok = ok && f->header && f->run && f->mvo[0] && f->mvo[1];
		if (i == 0)
		{
			f->face = (TaWalkFace *)malloc(f->header_room * sizeof(TaWalkFace));
			ok = ok && f->face;
		}
		if (i == 1)
			for (l = 0; l < 3; l++)
			{
				f->list_room[l] = f->snap.room[l] = list_room[l];
				f->list[l] = (PolyParam *)malloc(list_room[l] * sizeof(PolyParam));
				f->list_header[l] = (u32 *)malloc(list_room[l] * sizeof(u32));
				f->snap.pp[l] = (PolyParam *)malloc(list_room[l] * sizeof(PolyParam));
				f->snap.header[l] = (u32 *)malloc(list_room[l] * sizeof(u32));
				ok = ok && f->list[l] && f->list_header[l] && f->snap.pp[l] && f->snap.header[l];
			}
	}
	for (l = 0; l < 3; l++)
	{
		walk_full.room[l] = list_room[l];
		walk_full.pp[l] = (PolyParam *)malloc(list_room[l] * sizeof(PolyParam));
		ok = ok && walk_full.pp[l];
	}
	if (!ok)
	{
		ta_walk_term();
		WARN_LOG(PVR, "TA header pass: no memory for it\n");
		return false;
	}
	/* (touched now, so that no frame takes the page faults of their first
	 * use) */
	for (i = 0; i < 2; i++)
	{
		TaWalkFrame *f = &walk_frame[i];
		memset(f->header, 0, f->header_room * sizeof(TaWalkHeader));
		memset(f->run, 0, f->run_room * sizeof(TaWalkRun));
		memset(f->mvo[0], 0, f->mvo_room * sizeof(ModifierVolumeParam));
		memset(f->mvo[1], 0, f->mvo_room * sizeof(ModifierVolumeParam));
		if (f->face)
			memset(f->face, 0, f->header_room * sizeof(TaWalkFace));
		for (l = 0; i == 1 && l < 3; l++)
		{
			memset(f->list[l], 0, f->list_room[l] * sizeof(PolyParam));
			memset(f->list_header[l], 0, f->list_room[l] * sizeof(u32));
			memset(f->snap.pp[l], 0, f->snap.room[l] * sizeof(PolyParam));
			memset(f->snap.header[l], 0, f->snap.room[l] * sizeof(u32));
		}
	}
	for (l = 0; l < 3; l++)
		memset(walk_full.pp[l], 0, walk_full.room[l] * sizeof(PolyParam));
	walk_data_size = size;
	return true;
}

/* The frame's table, if it is to be trusted: how many entries */
static bool ta_walk_table(const TA_context *ctx, u32 *count)
{
	const tad_context *tad = &ctx->tad;
	u32 n = ctx->walk_count;

	if (ctx->data_size > walk_data_size)
		return false;
	/* (a frame with no data has nothing in its table, walked or not) */
	if (tad_end(tad) == tad->thd_root)
	{
		*count = 0;
		return true;
	}
	if (n == TA_WALK_INVALID || ctx->walk == NULL)
		return false;
	/* The lists were started again and the new data has nothing but
	 * vertices so far (all the rest would have been noted): the table is
	 * still the old data's. */
	if (ctx->walk_restart && tad->thd_data != tad->thd_root)
		n = 0;
	if (n != 0 && ctx->walk[n - 1] >= (u32)(tad_end(tad) - tad->thd_root) / 32)
		return false;
	*count = n;
	return true;
}

static bool ta_walk_run(TA_context *ctx, TaWalkFrame *f, u32 count, bool strips)
{
	TaWalker w;
	memset(&w, 0, sizeof(w));
	w.f = f;
	w.strips = strips;
	return w_run(&w, ctx, count);
}

bool ta_walk_parse(TA_context *ctx)
{
	TaWalkFrame *f = &walk_frame[0];
	u32 count;

	if (!ta_walk_table(ctx, &count))
		return false;
	f->bg_in = *TL_HEAD(&ctx->rend.global_param_op.l, PolyParam);
	f->x_clip_in = ctx->rend.fb_X_CLIP;
	f->y_clip_in = ctx->rend.fb_Y_CLIP;
	return ta_walk_run(ctx, f, count, false);
}

/* ---- The GPU decode's tables (rend/ta_gpu.h) ----
 *
 * The header pass, and what it found written as the GPU decode reads it:
 * the headers in the order make_index() goes through them (render pass,
 * then list, then as they came), each with its vertices' place in that
 * order, its format, its draw group's command slot, the words that say
 * whether two are drawn the same way, and the colours its vertices take
 * from it; the runs of vertex blocks; the modifier volume runs; the
 * segments (a list of a render pass); and the background's four vertices,
 * which FillBGP() made on this side. Nothing here reads a vertex. */
static bool ta_gpu_on;
static retro_atomic_int_t ta_gpu_want = RETRO_ATOMIC_INT_INITIALIZER(0);

/* (a list's room, in elements) */
#define TL_CAP(lp, T) ((u32)((T *)(lp)->end - (T *)(lp)->base))

/* A clock for the times kept, in nanoseconds (features_cpu.h's) */
static u64 ta_now_ns(void)
{
	return (u64)cpu_features_get_time_usec() * 1000;
}

void ta_set_gpu_wanted(bool on)
{
	retro_atomic_store_release_int(&ta_gpu_want, on ? 1 : 0);
}

bool ta_gpu_wanted(void)
{
	return retro_atomic_load_acquire_int(&ta_gpu_want) != 0;
}

void ta_gpu_sizes(TaGpuSizes *sz)
{
	const u32 scale = ta_data_size() / TA_DATA_SIZE;
	/* (what TA_context::Alloc() gives each list, and the header pass's room) */
	sz->verts = 4 * 1024 * 1024 * scale / sizeof(Vertex);
	sz->idx = 120 * 1024 * scale;
	sz->list[0] = 16384 * scale;
	sz->list[1] = 5120 * scale;
	sz->list[2] = (STRIPS_AS_PPARAMS ? 10240 : 8192) * scale;
	sz->modtrig = 16384 * scale;
	sz->headers = TA_WALK_MAX * scale + 1;
	sz->runs = TA_WALK_MAX * scale + 16;
	sz->table_words = TAG_T_WORDS + sz->runs * TAG_R_WORDS + sz->headers * TAG_H_WORDS
			+ sz->runs * TAG_M_WORDS + TAG_MAX_SEG * TAG_G_WORDS + 4 * TAG_V_WORDS
			+ TAG_SH_ENTRY + sz->headers;
}

bool ta_gpu_processing(void)
{
	return ta_gpu_on;
}

/* Whether any render pass the frame's data can have is sorted (the region
 * array's settings, which getRegionSettings() reads for each pass the
 * parse makes): a few words of video memory */
static bool ta_gpu_any_autosort(const TA_context *ctx)
{
	RenderPass r;
	u32 pass;
	for (pass = 0; pass <= ctx->tad.render_pass_count; pass++)
	{
		getRegionSettings((int)pass, &r);
		if (r.autosort)
			return true;
	}
	return false;
}

/* ---- TA_GPU_SORT: the translucent triangles of the sorted render passes ----
 *
 * The per-triangle renderer draws a sorted pass's translucent triangles in
 * the order GenSortedTo() gives them, one draw for each run of them drawn the
 * same way, and only the CPU can choose the pipeline and texture of a draw.
 * So the order is worked out here, from the frame's own blocks: of each
 * translucent vertex of a sorted pass, only its position (whether it is an
 * infinite vertex, ta_index.c's, and its depth) and its end-of-strip
 * bit are read, and a sprite's fourth corner is solved as the parse solves
 * it. Each strip's indices are what make_index() gives it (the translucent
 * lists are never joined), its triangles are GenSortedTo()'s, with the
 * winding of the renderer's provoking vertex (the first, Vulkan's, with
 * TA_GPU_PROVOKE; else the last, OpenGL's), and their depths are each the
 * least of its three vertices'. A stable radix sort of those depths as
 * integers that order as the floats do (tag_fkey(); no vertex that makes
 * an index has a depth that is not a number) gives a stable sort's order;
 * the draws are made as GenSortedTo() makes them. The vertices, colours,
 * indices and the rest are the GPU's: the indices written here name the
 * GPU decode's vertices, by their number. Nothing is allocated: the room,
 * made by ta_gpu_sort_reserve(), is for as many triangles as a frame can
 * have indices. */

/* A polygon of the prepass, by what it is drawn with */
typedef struct TaGpuHeaderKey
{
	PrepassKey key;
	u32 header;
} TaGpuHeaderKey;

typedef struct TaGpuSortState
{
	u32 room;			/* triangles */
	u32 *tri;			/* 4 words a triangle: its three indices, its header */
	u64 *keys[2];			/* depth key << 32 | triangle */
	SortTrigDrawParam *run;
	u32 pass_first[TAG_MAX_PASS], pass_count[TAG_MAX_PASS];	/* each pass's triangles (0: not sorted) */
	u8 pass_buf[TAG_MAX_PASS];	/* ...and which of keys[] has them sorted */
	u32 pass_run[TAG_MAX_PASS], pass_runs[TAG_MAX_PASS];
	u32 tris;
	u64 ns;
	bool first_provoking;		/* (TA_GPU_PROVOKE) GenSortedTo()'s firstProvoking */
	/* Each sorted pass's depth prepass (rend/sorter.h): the triangles of
	 * its polygons that can hide what is behind them, by polygon and the
	 * polygons by what they are drawn with (porder: triangle numbers, ptris
	 * of them; their indices go after the sorted ones), and its draws */
	u32 *porder;
	u32 ptris;
	SortTrigDrawParam *prun;
	u32 pass_prun[TAG_MAX_PASS], pass_pruns[TAG_MAX_PASS];
	u32 hroom;			/* headers the next three have room for */
	u32 *hcount;			/* a header's triangles, then where they go (all 0 between frames) */
	u32 *htouched;
	TaGpuHeaderKey *hkey;
	/* The prepass's checks (TAG_K_PREPASS): each strip of the sorted
	 * passes, as ta_gpu_sort_strip made it - its first vertex, its vertices
	 * and its first triangle - (sroom of them; past that, no checks this
	 * frame), each triangle's place among the prepass's, and the checks:
	 * the strips in the prepass whose alpha is their vertices', for the GPU
	 * to look at them (TAG_PC_WORDS each) */
	u32 *strip;
	u32 strips, sroom;
	bool strips_full;
	u32 *tpos;
	u32 *check;
	u32 checks;
} TaGpuSortState;

static TaGpuSortState gsort;

bool ta_gpu_sort_reserve(bool on)
{
	TaGpuSortState *g = &gsort;
	TaGpuSizes sz;
	u32 room, hroom, sroom;
	if (on)
	{
		ta_gpu_sizes(&sz);
		if (g->room >= sz.idx)
			return true;
	}
	free(g->tri);
	free(g->keys[0]);
	free(g->keys[1]);
	free(g->run);
	free(g->porder);
	free(g->prun);
	free(g->hcount);
	free(g->htouched);
	free(g->hkey);
	free(g->strip);
	free(g->tpos);
	free(g->check);
	memset(g, 0, sizeof(*g));
	if (!on)
		return true;
	/* (a frame's indices are at most its room, and a triangle takes an
	 * index or more) */
	room = sz.idx;
	hroom = sz.headers;
	/* (a translucent strip is a polygon parameter of its own: as many as the list has room for) */
	sroom = sz.list[2];
	g->tri = (u32 *)malloc((size_t)room * 4 * sizeof(u32));
	g->keys[0] = (u64 *)malloc((size_t)room * sizeof(u64));
	g->keys[1] = (u64 *)malloc((size_t)room * sizeof(u64));
	g->run = (SortTrigDrawParam *)malloc((size_t)room * sizeof(SortTrigDrawParam));
	g->porder = (u32 *)malloc((size_t)room * sizeof(u32));
	g->prun = (SortTrigDrawParam *)malloc((size_t)hroom * sizeof(SortTrigDrawParam));
	g->hcount = (u32 *)calloc(hroom, sizeof(u32));
	g->htouched = (u32 *)malloc((size_t)hroom * sizeof(u32));
	g->hkey = (TaGpuHeaderKey *)malloc((size_t)hroom * sizeof(TaGpuHeaderKey));
	g->strip = (u32 *)malloc((size_t)sroom * 3 * sizeof(u32));
	g->tpos = (u32 *)malloc((size_t)room * sizeof(u32));
	g->check = (u32 *)malloc((size_t)sroom * TAG_PC_WORDS * sizeof(u32));
	if (g->tri == NULL || g->keys[0] == NULL || g->keys[1] == NULL || g->run == NULL
			|| g->porder == NULL || g->prun == NULL || g->hcount == NULL || g->htouched == NULL || g->hkey == NULL
			|| g->strip == NULL || g->tpos == NULL || g->check == NULL)
	{
		ta_gpu_sort_reserve(false);
		WARN_LOG(PVR, "TA processing: no memory for sorting translucent triangles\n");
		return false;
	}
	/* (touched now, so that no frame takes the page faults of their first use) */
	memset(g->tri, 0, (size_t)room * 4 * sizeof(u32));
	memset(g->keys[0], 0, (size_t)room * sizeof(u64));
	memset(g->keys[1], 0, (size_t)room * sizeof(u64));
	memset(g->run, 0, (size_t)room * sizeof(SortTrigDrawParam));
	memset(g->porder, 0, (size_t)room * sizeof(u32));
	memset(g->prun, 0, (size_t)hroom * sizeof(SortTrigDrawParam));
	memset(g->htouched, 0, (size_t)hroom * sizeof(u32));
	memset(g->hkey, 0, (size_t)hroom * sizeof(TaGpuHeaderKey));
	memset(g->strip, 0, (size_t)sroom * 3 * sizeof(u32));
	memset(g->tpos, 0, (size_t)room * sizeof(u32));
	memset(g->check, 0, (size_t)sroom * TAG_PC_WORDS * sizeof(u32));
	g->sroom = sroom;
	g->hroom = hroom;
	g->room = room;
	return true;
}

const SortTrigDrawParam *ta_gpu_sorted(u32 pass, u32 *count)
{
	if (pass >= TAG_MAX_PASS || gsort.pass_runs[pass] == 0)
	{
		*count = 0;
		return NULL;
	}
	*count = gsort.pass_runs[pass];
	return gsort.run + gsort.pass_run[pass];
}

const SortTrigDrawParam *ta_gpu_prepass(u32 pass, u32 *count)
{
	if (pass >= TAG_MAX_PASS || gsort.pass_pruns[pass] == 0)
	{
		*count = 0;
		return NULL;
	}
	*count = gsort.pass_pruns[pass];
	return gsort.prun + gsort.pass_prun[pass];
}

u32 ta_gpu_sorted_indices(void)
{
	return (gsort.tris + gsort.ptris) * 3;
}

u32 ta_gpu_prepass_checks(void)
{
	return gsort.checks;
}

u32 ta_gpu_sorted_words(void)
{
	return ta_gpu_sorted_indices() + (gsort.checks != 0 ? 1 + gsort.checks * TAG_PC_WORDS : 0);
}

void ta_gpu_sorted_write(u32 *dst)
{
	const TaGpuSortState *g = &gsort;
	u32 p, n, i;
	for (p = 0; p < TAG_MAX_PASS; p++)
	{
		const u64 *k = g->keys[g->pass_buf[p]] + g->pass_first[p];
		for (n = g->pass_count[p]; n != 0; n--, k++, dst += 3)
		{
			const u32 *t = &g->tri[(u32)*k * 4];
			dst[0] = t[0];
			dst[1] = t[1];
			dst[2] = t[2];
		}
	}
	for (i = 0; i < g->ptris; i++, dst += 3)
	{
		const u32 *t = &g->tri[g->porder[i] * 4];
		dst[0] = t[0];
		dst[1] = t[1];
		dst[2] = t[2];
	}
	/* (the prepass's checks: TAG_T_PREP) */
	if (g->checks != 0)
	{
		dst[0] = g->checks;
		memcpy(dst + 1, g->check, (size_t)g->checks * TAG_PC_WORDS * sizeof(u32));
	}
}

u64 ta_gpu_sorted_ns(void)
{
	return gsort.ns;
}

/* make_index() of one strip at a time, an index at a time (no strip is
 * joined to another in a translucent list), with each index's depth key,
 * and GenSortedTo()'s triangles of the indices as they come */
typedef struct TaGpuSortStrip
{
	TaGpuSortState *g;
	u32 hdr;		/* the strip's header */
	u32 count;		/* its indices so far */
	int last_good;		/* the last kept vertex's place, -1 for none yet */
	u32 last_good_key;
	bool dupe;		/* the next kept vertex goes in twice (a link) */
	bool link;		/* infinite vertices have come since the last kept one */
	u32 i;			/* the vertex's place in the strip */
	u32 h_place[2], h_key[2];	/* the two indices before */
	bool full;		/* more triangles than the room (cannot be: see ta_gpu_sort_reserve()) */
} TaGpuSortStrip;

static void gss_start(TaGpuSortStrip *s, u32 header)
{
	s->hdr = header;
	s->count = 0;
	s->last_good = -1;
	s->dupe = false;
	s->link = false;
	s->i = 0;
}

/* (the prepass's checks) the strip's vertex @place: its first makes its record */
static void gss_note(TaGpuSortStrip *s, u32 place)
{
	TaGpuSortState *g = s->g;
	if (s->i == 0)
	{
		u32 *r;
		if (g->strips == g->sroom)
		{
			g->strips_full = true;
			return;
		}
		r = &g->strip[g->strips++ * 3];
		r[0] = place;
		r[1] = 0;
		r[2] = g->tris;
	}
	if (!g->strips_full)
		g->strip[(g->strips - 1) * 3 + 1] = place - g->strip[(g->strips - 1) * 3] + 1;
}

static void gss_put(TaGpuSortStrip *s, u32 place, u32 key)
{
	TaGpuSortState *g = s->g;
	if (s->count >= 2)
	{
		const u32 t = g->tris;
		if (t == g->room)
			s->full = true;
		else
		{
			/* GenSortedTo(), firstProvoking: every other triangle starts
			 * with the vertex that holds its colour; without it, the
			 * first two of every other one change places */
			u32 *d = &g->tri[t * 4];
			u32 key3;
			if (((s->count - 2) & 1) && g->first_provoking)
			{
				d[0] = s->h_place[0];
				d[1] = place;
				d[2] = s->h_place[1];
			}
			else if ((s->count - 2) & 1)
			{
				d[0] = s->h_place[1];
				d[1] = s->h_place[0];
				d[2] = place;
			}
			else
			{
				d[0] = s->h_place[0];
				d[1] = s->h_place[1];
				d[2] = place;
			}
			d[3] = s->hdr;
			key3 = s->h_key[0] < s->h_key[1] ? s->h_key[0] : s->h_key[1];
			if (key < key3)
				key3 = key;
			g->keys[0][t] = ((u64)key3 << 32) | t;
			g->tris = t + 1;
		}
	}
	s->h_place[0] = s->h_place[1];
	s->h_key[0] = s->h_key[1];
	s->h_place[1] = place;
	s->h_key[1] = key;
	s->count++;
}

/* the strip's next vertex, number @place (make_index()'s loop, its look
 * ahead for the next kept vertex turned round) */
static void gss_vertex(TaGpuSortStrip *s, u32 place, u32 x, u32 y, u32 z)
{
	gss_note(s, place);
	if (tag_inf(x, y, z) != 0)
		s->link = true;
	else
	{
		const u32 key = tag_fkey(z);
		if (s->link && s->last_good >= 0)
		{
			gss_put(s, (u32)s->last_good, s->last_good_key);
			s->dupe = true;
		}
		s->link = false;
		s->last_good = (int)place;
		s->last_good_key = key;
		if (s->dupe)
		{
			gss_put(s, place, key);
			s->dupe = false;
		}
		if ((s->i ^ s->count) & 1)
			gss_put(s, place, key);
		gss_put(s, place, key);
	}
	s->i++;
}

/* The vertices of run @i: up to the next run of vertices, or the frame's last */
static u32 walk_run_end(const TaWalkFrame *f, u32 i)
{
	u32 j = i + 1;
	while (j < f->runs && (f->run[j].header & TA_WALK_MODVOL))
		j++;
	return j < f->runs ? f->run[j].vertex : f->verts;
}

/* A vertex block's end-of-strip bit */
static bool block_eos(const u32 *w)
{
	return TAW_GET(w[0], PCW_EndOfStrip) != 0;
}

/* The translucent strips of the sorted render passes, from their blocks,
 * in the order the parse makes them: the runs as they came, and each
 * header's strips in its runs */
static bool ta_gpu_sort_strips(const TaWalkFrame *f, const u8 *root, const bool *sorted)
{
	TaGpuSortState *g = &gsort;
	TaGpuSortStrip s;
	u32 cur_pass = TAG_MAX_PASS;
	u32 open_hdr = TA_WALK_NO_GROUP;
	u32 i, k;

	memset(&s, 0, sizeof(s));
	s.g = g;
	for (i = 0; i < f->runs; i++)
	{
		const TaWalkRun *run = &f->run[i];
		const TaWalkHeader *h;
		u32 n, place;
		if (run->header & TA_WALK_MODVOL)
			continue;
		h = &f->header[run->header];
		if (h->list != ListType_Translucent || !sorted[h->pass])
			continue;
		if (h->pass != cur_pass)
		{
			cur_pass = h->pass;
			g->pass_first[cur_pass] = g->tris;
		}
		n = walk_run_end(f, i) - run->vertex;
		/* (the GPU decode's vertex buffer has them in the order they came, as the parse's) */
		place = run->vertex;
		if (open_hdr != run->header)
		{
			/* (a strip does not go on from one header to the next: the header pass leaves such a frame to the parse) */
			gss_start(&s, run->header);
			open_hdr = run->header;
		}
		if (h->format == TA_WALK_SPRITE)
		{
			for (k = 0; k + 4 <= n; k += 4)
			{
				const u32 *w = (const u32 *)(root + (size_t)(run->block + 2 * (k >> 2)) * 32);
				/* the corners as AppendSpriteVertexA() and B() put them, and
				 * the first's depth as the parse works it out */
				Vertex c[4];
				u32 z0;
				memset(c, 0, sizeof(c));
				memcpy(&c[2].x, &w[1], 12);
				memcpy(&c[3].x, &w[4], 12);
				memcpy(&c[1].x, &w[7], 12);
				memcpy(&c[0].x, &w[10], 8);
				CaclulateSpritePlane(c);
				memcpy(&z0, &c[0].z, 4);
				gss_start(&s, run->header);
				gss_vertex(&s, place + k, w[10], w[11], z0);
				gss_vertex(&s, place + k + 1, w[7], w[8], w[9]);
				gss_vertex(&s, place + k + 2, w[1], w[2], w[3]);
				gss_vertex(&s, place + k + 3, w[4], w[5], w[6]);
			}
			open_hdr = TA_WALK_NO_GROUP;
		}
		else
		{
			const u32 vb = h->vblocks;
			const u32 *w = (const u32 *)(root + (size_t)run->block * 32);
			for (k = 0; k < n; k++, w += vb * 8)
			{
				gss_vertex(&s, place + k, w[1], w[2], w[3]);
				if (block_eos(w))
					gss_start(&s, run->header);
			}
		}
		g->pass_count[cur_pass] = g->tris - g->pass_first[cur_pass];
	}
	return !s.full;
}

static int u64_order(const void *a, const void *b)
{
	const u64 x = *(const u64 *)a, y = *(const u64 *)b;
	return x < y ? -1 : x > y;
}

/* A stable sort of @n keys from @k[0] by their top 32 bits, a digit of 11,
 * 11 and 10 bits at a time, from the lowest; a digit all of them share
 * takes no pass. Which of the two has them in the end. (A few hundred are
 * sorted whole, the triangle's number in the low half breaking ties: the
 * same order, without the passes' fixed cost.) */
static u32 ta_gpu_sort_keys(u64 *k[2], u32 first, u32 n)
{
	static u32 hist[3][2048];
	static const u32 shift[3] = { 32, 43, 54 };
	u32 at = 0, i, d, b;
	const u64 *a;
	if (n <= 256)
	{
		qsort(k[0] + first, n, sizeof(u64), u64_order);
		return 0;
	}
	memset(hist, 0, sizeof(hist));
	a = k[0] + first;
	for (i = 0; i < n; i++)
	{
		const u64 v = a[i];
		hist[0][(v >> 32) & 2047]++;
		hist[1][(v >> 43) & 2047]++;
		hist[2][(v >> 54) & 1023]++;
	}
	for (d = 0; d < 3; d++)
	{
		const u64 *src = k[at] + first;
		u64 *dst;
		u32 sum = 0;
		if (hist[d][(src[0] >> shift[d]) & 2047] == n)
			continue;
		for (b = 0; b < 2048; b++)
		{
			const u32 c = hist[d][b];
			hist[d][b] = sum;
			sum += c;
		}
		dst = k[at ^ 1] + first;
		for (i = 0; i < n; i++)
		{
			const u64 v = src[i];
			dst[hist[d][(v >> shift[d]) & 2047]++] = v;
		}
		at ^= 1;
	}
	return at;
}

/* The prepass's checks (TAG_K_PREPASS): each strip with triangles in the
 * prepass whose alpha is its vertices' (PrepassAlphaFromVertices()), for
 * the GPU to look at the vertices of and to leave out if none has an alpha
 * of 255, as GenPrepassTo() does. A strip's triangles there are one after
 * the other (its header's are, in the order they were made). */
static void ta_gpu_prepass_make_checks(const TaWalkFrame *f)
{
	TaGpuSortState *g = &gsort;
	u32 k, t;
	g->checks = 0;
	if (g->strips_full)
		return;
	for (k = 0; k < g->strips; k++)
	{
		const u32 *s = &g->strip[k * 3];
		const u32 tend = k + 1 < g->strips ? g->strip[(k + 1) * 3 + 2] : g->tris;
		u32 first = TAG_NONE, n = 0;
		u32 *c;
		for (t = s[2]; t < tend; t++)
		{
			const u32 *d = &g->tri[t * 4];
			u32 at;
			if (d[0] == d[1] || d[1] == d[2] || d[0] == d[2])
				continue;
			at = g->tpos[t];
			if (at == TAG_NONE)
				break;	/* (not in the prepass: none of the strip's is) */
			if (first == TAG_NONE)
				first = at;
			else if (at != first + n)
			{
				first = TAG_NONE;	/* (cannot be: no check, it is drawn) */
				break;
			}
			n++;
		}
		if (first == TAG_NONE || n == 0 || !PrepassAlphaFromVertices(&f->header[g->tri[s[2] * 4 + 3]].pp))
			continue;
		c = &g->check[g->checks++ * TAG_PC_WORDS];
		c[TAG_PC_V] = s[0];
		c[TAG_PC_NV] = s[1];
		c[TAG_PC_T] = g->tris + first;
		c[TAG_PC_NT] = n;
	}
	/* (in the sorted indices' room, which is six words a triangle a frame
	 * can have: three a triangle sorted and drawn in the prepass take all
	 * of it only where no strip is - see ta_gpu_sort_reserve()) */
	if (ta_gpu_sorted_words() > g->room * 6)
		g->checks = 0;
}

/* (by what they are drawn with, then by header) */
static int header_key_order(const void *pa, const void *pb)
{
	const TaGpuHeaderKey *a = (const TaGpuHeaderKey *)pa, *b = (const TaGpuHeaderKey *)pb;
	if (PrepassKeyLess(&a->key, &b->key))
		return -1;
	if (PrepassKeyLess(&b->key, &a->key))
		return 1;
	return a->header < b->header ? -1 : a->header > b->header;
}

/* The depth prepass of each sorted pass, as GenPrepassTo() makes it but for
 * the vertices, which are the GPU's: the polygons whose alpha is their
 * vertices' are drawn, and the GPU makes nothing of those of them none of
 * whose vertices has an alpha of 255 (ta_gpu_prepass_checks()). The
 * triangles are the sorted ones, before they were sorted. */
static void ta_gpu_prepass_frame(const TaWalkFrame *f)
{
	TaGpuSortState *g = &gsort;
	const u32 none = ~0u;
	u32 npr = 0, p;
	g->ptris = 0;
	memset(g->pass_pruns, 0, sizeof(g->pass_pruns));
	/* (no triangle in the prepass yet: TAG_NONE) */
	memset(g->tpos, 0xFF, (size_t)g->tris * sizeof(u32));
	if (f->headers > g->hroom)
		return;
	for (p = 0; p < f->passes && p < TAG_MAX_PASS; p++)
	{
		const u32 n = g->pass_count[p];
		const bool last = p == f->passes - 1;
		u32 t0, nh = 0, nt = 0, t, k, at;
		g->pass_prun[p] = npr;
		if (n == 0 || !PrepassWanted(last))
			continue;
		t0 = g->pass_first[p];
		/* the polygons that can hide, and their triangles' counts */
		for (t = t0; t < t0 + n; t++)
		{
			const u32 *d = &g->tri[t * 4];
			u32 *c;
			if (d[0] == d[1] || d[1] == d[2] || d[0] == d[2])
				continue;
			c = &g->hcount[d[3]];
			if (*c == 0)
			{
				const PolyParam *pp = &f->header[d[3]].pp;
				g->htouched[nt++] = d[3];
				if (!PrepassCandidate(pp, last))
				{
					*c = none;
					continue;
				}
				g->hkey[nh].key = PrepassMakeKey(pp, 0);
				g->hkey[nh].header = d[3];
				nh++;
			}
			else if (*c == none)
				continue;
			(*c)++;
		}
		qsort(g->hkey, nh, sizeof(TaGpuHeaderKey), header_key_order);
		/* where each one's triangles go, and the draws */
		at = g->ptris;
		for (k = 0; k < nh; k++)
		{
			const u32 h = g->hkey[k].header;
			const u32 c = g->hcount[h];
			g->hcount[h] = at;
			if (k == 0 || !PrepassKeyEq(&g->hkey[k].key, &g->hkey[k - 1].key))
			{
				SortTrigDrawParam *r = &g->prun[npr++];
				r->ppid = &f->header[h].pp;
				r->first = (g->tris + at) * 3;
				r->count = 0;
			}
			g->prun[npr - 1].count += c * 3;
			at += c;
		}
		for (t = t0; t < t0 + n; t++)
		{
			const u32 *d = &g->tri[t * 4];
			u32 *o;
			if (d[0] == d[1] || d[1] == d[2] || d[0] == d[2])
				continue;
			o = &g->hcount[d[3]];
			if (*o != none)
			{
				g->tpos[t] = *o;
				g->porder[(*o)++] = t;
			}
		}
		for (k = 0; k < nt; k++)
			g->hcount[g->htouched[k]] = 0;
		g->ptris = at;
		g->pass_pruns[p] = npr - g->pass_prun[p];
	}
}

/* The sorted passes of the frame the header pass made (before its lists
 * are made into draw groups): false if they do not fit (which cannot be) */
static bool ta_gpu_sort_frame(TaWalkFrame *f, const u8 *root)
{
	TaGpuSortState *g = &gsort;
	bool sorted[TAG_MAX_PASS];
	bool any = false;
	u32 p, nrun = 0;
	const u64 t0 = ta_now_ns();

	g->tris = 0;
	g->ptris = 0;
	g->strips = 0;
	g->strips_full = false;
	g->checks = 0;
	memset(g->pass_pruns, 0, sizeof(g->pass_pruns));
	memset(g->pass_first, 0, sizeof(g->pass_first));
	memset(g->pass_count, 0, sizeof(g->pass_count));
	memset(g->pass_buf, 0, sizeof(g->pass_buf));
	for (p = 0; p < TAG_MAX_PASS; p++)
	{
		sorted[p] = p < f->passes && f->pass[p].autosort;
		any = any || sorted[p];
	}
	if (!any)
		return true;
	if (!ta_gpu_sort_strips(f, root, sorted))
		return false;

	/* each pass: sorted, and its draws made as GenSortedTo() makes them */
	for (p = 0; p < f->passes; p++)
	{
		const u32 n = g->pass_count[p];
		const u64 *k;
		u32 cur, start = 0, j;
		g->pass_run[p] = nrun;
		if (n == 0)
			continue;
		g->pass_buf[p] = (u8)ta_gpu_sort_keys(g->keys, g->pass_first[p], n);
		k = g->keys[g->pass_buf[p]] + g->pass_first[p];
		cur = g->tri[(u32)k[0] * 4 + 3];
		for (j = 1; j <= n; j++)
		{
			u32 h = 0;
			SortTrigDrawParam *r;
			if (j < n)
			{
				h = g->tri[(u32)k[j] * 4 + 3];
				/* (another strip of the same polygon, or one drawn the same way: the same draw) */
				if (h == cur || SortSameDraw(&f->header[h].pp, &f->header[cur].pp))
					continue;
			}
			r = &g->run[nrun++];
			r->ppid = &f->header[cur].pp;
			r->first = (g->pass_first[p] + start) * 3;
			r->count = (j - start) * 3;
			cur = h;
			start = j;
		}
		g->pass_runs[p] = nrun - g->pass_run[p];
	}
	ta_gpu_prepass_frame(f);
	ta_gpu_prepass_make_checks(f);
	g->ns = ta_now_ns() - t0;
	return true;
}

/* The block of modifier volume triangle @t of the volumes' runs from @*r
 * on, which are numbered together, both lists', in their order: NULL if
 * there is none */
static const u32 *mv_triangle_words(const TaWalkFrame *f, const u8 *root, u32 *r, u32 t)
{
	while (*r < f->runs && (!(f->run[*r].header & TA_WALK_MODVOL) || f->run[*r].vertex + (f->run[*r].blocks + 1) / 2 <= t))
		(*r)++;
	if (*r == f->runs)
		return NULL;
	return (const u32 *)(root + (size_t)(f->run[*r].block + 2 * (t - f->run[*r].vertex)) * 32);
}

/* TA_GPU_SHADOWS: what each render pass's opaque modifier volumes cover,
 * from the triangles' blocks, as ModVolBounds() has it from the parse's */
static void ta_gpu_shadow_areas(const TaWalkFrame *f, const u8 *root, f32 (*area)[4])
{
	u32 r = 0, prev = 0, p, m, t;
	for (p = 0; p < f->passes; p++)
	{
		ScreenBounds b;
		b.min_x = b.min_y = 1e30f;
		b.max_x = b.max_y = -1e30f;
		for (m = prev; m < f->pass[p].mvo_count; m++)
		{
			const ModifierVolumeParam *mp = &f->mvo[0][m];
			for (t = mp->first; t < mp->first + mp->count; t++)
			{
				const u32 *w = mv_triangle_words(f, root, &r, t);
				float c[9];
				if (w == NULL)
					break;
				memcpy(c, &w[1], sizeof(c));
				screen_bounds_add(&b, c[0], c[1]);
				screen_bounds_add(&b, c[3], c[4]);
				screen_bounds_add(&b, c[6], c[7]);
			}
		}
		prev = f->pass[p].mvo_count;
		area[p][0] = b.min_x;
		area[p][1] = b.min_y;
		area[p][2] = b.max_x;
		area[p][3] = b.max_y;
	}
}

/* The raster renderers' modifier volume triangles (rend/modvol_clip.h),
 * from the triangles' blocks: each one's nine words after its first
 * block's control word, as the decode takes them (tag_modtrig_word()) */
typedef struct MvClipBlocks
{
	const TaWalkFrame *f;
	const u8 *root;
	u32 r;
} MvClipBlocks;

static ModTriangle mv_clip_blocks_get(void *ctx, u32 t)
{
	MvClipBlocks *g = (MvClipBlocks *)ctx;
	ModTriangle tri;
	const u32 *w;
	if (g->r >= g->f->runs || g->f->run[g->r].vertex > t)
		g->r = 0;
	w = mv_triangle_words(g->f, g->root, &g->r, t);
	if (w == NULL)
		memset(&tri, 0, sizeof(tri));
	else
		memcpy(&tri, &w[1], sizeof(tri));
	return tri;
}

static void mv_clip_blocks_restart(void *ctx)
{
	((MvClipBlocks *)ctx)->r = 0;
}

static void ta_gpu_modvol_clip(const TaWalkFrame *f, const u8 *root, rend_context *rc)
{
	MvClipBlocks get;
	get.f = f;
	get.root = root;
	get.r = 0;
	mv_clip_build(rc, f->modtrig, mv_clip_blocks_get, mv_clip_blocks_restart, &get);
}

/* Where flat strips are looked for (SSAA), each header's strips: whether
 * all of them are flat, in its pp's single-rate bit, as the full parse
 * finds it from its vertices. From the frame's own blocks, as the sort
 * reads them: of each vertex only its depth word and its end-of-strip
 * bit, the AND and the OR of the depths' bits over a strip
 * (PVR_FLAT_RATE_BIT()). A sprite's bit is set already, and the
 * background's. The draw groups are then given their strips' bits
 * together, as flat_groups() gives the parse's. */
static u64 gflat_ns;	/* the last frame's (ta_gpu_flat_ns()) */

u64 ta_gpu_flat_ns(void)
{
	return gflat_ns;
}

static void ta_gpu_flat_headers(TaWalkFrame *f, const u8 *root)
{
	const u64 t0 = ta_now_ns();
	u32 open_hdr = TA_WALK_NO_GROUP;
	u32 z_and = PVR_FLAT_AND_INIT, z_or = PVR_FLAT_OR_INIT;
	u32 i, k;

	for (i = 0; i < f->headers; i++)
		if (f->header[i].format != TA_WALK_SPRITE)
			f->header[i].pp.pcw |= PVR_PCW_SINGLE_RATE;
	for (i = 0; i < f->runs; i++)
	{
		const TaWalkRun *run = &f->run[i];
		TaWalkHeader *h;
		u32 n, stride;
		const u32 *w;
		if (run->header & TA_WALK_MODVOL)
			continue;
		h = &f->header[run->header];
		if (h->format == TA_WALK_SPRITE)
			continue;
		n = walk_run_end(f, i) - run->vertex;
		if (open_hdr != run->header)
		{
			/* (a strip does not go on from one header to the next: the header pass leaves such a frame to the parse) */
			open_hdr = run->header;
			z_and = PVR_FLAT_AND_INIT;
			z_or = PVR_FLAT_OR_INIT;
		}
		stride = h->vblocks * 8;
		w = (const u32 *)(root + (size_t)run->block * 32);
		for (k = 0; k < n; k++, w += stride)
		{
			z_and &= w[3];
			z_or |= w[3];
			if (block_eos(w))
			{
				h->pp.pcw &= ~PVR_PCW_SINGLE_RATE | PVR_FLAT_RATE_BIT(z_and, z_or);
				z_and = PVR_FLAT_AND_INIT;
				z_or = PVR_FLAT_OR_INIT;
			}
		}
	}
	gflat_ns = ta_now_ns() - t0;
}

/* ta_gpu_prepare()'s segments' bookkeeping, and its header records */
typedef struct TaGpuPut
{
	u32 *table;
	u32 off_hdr;
	bool draw;
	u32 nslots, ntrd;
	u32 seg_last[TAG_MAX_SEG], seg_slot[TAG_MAX_SEG];
	const PolyParam *seg_tr[TAG_MAX_SEG];
	u32 seg_trb[TAG_MAX_SEG], seg_trv[TAG_MAX_SEG], seg_groups[TAG_MAX_SEG];
	ta_list_t *lists[3];
} TaGpuPut;

/* A header's record. Its slot: a command for each draw group of the
 * opaque and punch-through lists. The translucent strips are each their
 * own polygon parameter, with a record of their own; for drawing, the
 * translucent headers one after another in a segment that are drawn
 * the same way (poly_same_state()) are a draw group too, whose records
 * the GPU copies to a place of the group's in the translucent draw
 * records: a word for how many there are, then room for one a vertex,
 * which is the most strips the group can have (TAG_H_TRB, TAG_H_TRV).
 * With TA_GPU_DRAW each draw group is a polygon parameter of the lists
 * here, in the lists' order, which is the parse's. */
static u32 put_header(TaGpuPut *g, u32 id, const PolyParam *pp, u32 first, u32 cnt, u32 vstart, u32 fmt, u32 vb,
		u32 list, u32 seg, u32 group, const TaWalkFace *face)
{
	u32 *w = &g->table[g->off_hdr + id * TAG_H_WORDS];
	u32 slot = TAG_NONE;
	bool fresh;
	if (list != 2)
	{
		fresh = g->seg_last[seg] != group;
		if (fresh)
		{
			g->seg_last[seg] = group;
			g->seg_slot[seg] = g->nslots++;
		}
		slot = g->seg_slot[seg];
	}
	else
	{
		fresh = g->seg_tr[seg] == NULL || !poly_same_state(g->seg_tr[seg], pp);
		if (fresh)
		{
			g->seg_tr[seg] = pp;
			g->seg_trb[seg] = g->ntrd++;
			g->seg_trv[seg] = vstart;
		}
		g->ntrd += cnt;
	}
	if (g->draw)
	{
		/* first: the command's slot, or the group's place in the translucent draw records; count: its vertices */
		if (fresh)
		{
			PolyParam *d = TL_APPEND(g->lists[list], PolyParam);
			*d = *pp;
			d->first = list != 2 ? slot : g->seg_trb[seg];
			d->count = cnt;
			g->seg_groups[seg]++;
		}
		else
			TL_LAST(g->lists[list], PolyParam)->count += cnt;
		/* (a group is drawn at a single rate where all of its strips are flat: see ta_gpu_flat_headers()) */
		if (!fresh && flat_strips_wanted)
			TL_LAST(g->lists[list], PolyParam)->pcw &= pp->pcw | ~PVR_PCW_SINGLE_RATE;
	}
	w[TAG_H_VFIRST] = first;
	w[TAG_H_VCOUNT] = cnt;
	w[TAG_H_VSTART] = vstart;
	w[TAG_H_INFO] = TAG_INFO(fmt, vb, list, seg, PCW_GET(pp->pcw, Texture));
	w[TAG_H_SLOT] = slot;
	w[TAG_H_STATE + TAG_S_TSP] = pp->tsp;
	w[TAG_H_STATE + TAG_S_TCW] = pp->tcw;
	w[TAG_H_STATE + TAG_S_PCW] = pp->pcw & ~PVR_PCW_SINGLE_RATE;
	w[TAG_H_STATE + TAG_S_ISP] = pp->isp;
	w[TAG_H_STATE + TAG_S_TSP1] = pp->tsp1;
	w[TAG_H_STATE + TAG_S_TCW1] = pp->tcw1;
	w[TAG_H_STATE + TAG_S_CLIP] = pp->tileclip;
	if (face != NULL)
	{
		memcpy(&w[TAG_H_FCOL], face->col, sizeof(face->col));
		memcpy(&w[TAG_H_FF], face->f, sizeof(face->f));
		memcpy(&w[TAG_H_SCOL], face->sprite, sizeof(face->sprite));
	}
	else
		memset(&w[TAG_H_FCOL], 0, (TAG_H_TRB - TAG_H_FCOL) * sizeof(u32));
	w[TAG_H_TRB] = list == 2 ? g->seg_trb[seg] : TAG_NONE;
	w[TAG_H_TRV] = list == 2 ? g->seg_trv[seg] : TAG_NONE;
	return id;
}

/* What the full parse drops a frame for that the decode would draw: see
 * ta_gpu_prepare() */
static bool ta_gpu_room(const TaWalkFrame *f, const TA_context *ctx)
{
	const rend_context *rc = &ctx->rend;
	u32 lv[3], lroom[3], i, k, l;
	lv[0] = lv[1] = lv[2] = 0;
	for (i = 0; i < f->headers; i++)
		if (f->header[i].pp.count != 0)
			lv[w_pp_list(f->header[i].list)] += f->header[i].pp.count;
	lv[0]++;	/* (the background) */
	lroom[0] = TL_CAP(&rc->global_param_op.l, PolyParam);
	lroom[1] = TL_CAP(&rc->global_param_pt.l, PolyParam);
	lroom[2] = TL_CAP(&rc->global_param_tr.l, PolyParam);
	if ((u64)f->verts * 4 > (u64)TL_CAP(&rc->idx.l, u32)
			|| f->mvos[0] > TL_CAP(&rc->global_param_mvo.l, ModifierVolumeParam)
			|| f->mvos[1] > TL_CAP(&rc->global_param_mvo_tr.l, ModifierVolumeParam))
		return false;
	if ((u64)lv[0] + 1 > lroom[0] || (u64)lv[1] + 1 > lroom[1] || (u64)lv[2] + 1 > lroom[2])
	{
		u32 ls[3];
		const u8 *root = ctx->tad.thd_root;
		ls[0] = 1;
		ls[1] = ls[2] = 0;
		for (i = 0; i < f->runs; i++)
		{
			const TaWalkRun *run = &f->run[i];
			const TaWalkHeader *h;
			u32 n;
			if (run->header & TA_WALK_MODVOL)
				continue;
			h = &f->header[run->header];
			n = walk_run_end(f, i) - run->vertex;
			l = (u32)w_pp_list(h->list);
			if (h->format == TA_WALK_SPRITE)
				ls[l] += n / 4;
			else
				for (k = 0; k < n; k++)
					ls[l] += block_eos((const u32 *)(root + (size_t)(run->block + k * h->vblocks) * 32));
		}
		if ((u64)ls[0] + 1 > lroom[0] || (u64)ls[1] + 1 > lroom[1] || (u64)ls[2] + 1 > lroom[2])
			return false;
	}
	return true;
}

bool ta_gpu_prepare(TA_context *ctx, const TaGpuLayout *layout, u32 *table, u32 table_room, TaGpuFrame *out, u32 flags)
{
	TaWalkFrame *f = &walk_frame[0];
	u32 count, i, l, nseg, nhdr, nruns, nmvruns, words;
	u32 off_run, off_hdr, off_mvrun, off_seg, off_bg, off_shd;
	u32 seg_hdr[TAG_MAX_SEG], seg_v[TAG_MAX_SEG];
	u32 seg_next_h[TAG_MAX_SEG], seg_next_v[TAG_MAX_SEG];
	const bool draw = (flags & TA_GPU_DRAW) != 0;
	bool ok, shadows;
	TaGpuSizes sz;
	rend_context *rc = &ctx->rend;
	TaGpuPut put;
	u32 *t;

	out->why = TA_GPU_WHY_TABLE;
	out->shadows = 0;
	/* (nothing sorted, until this frame is) */
	gsort.tris = 0;
	gsort.ptris = 0;
	gsort.ns = 0;
	memset(gsort.pass_count, 0, sizeof(gsort.pass_count));
	memset(gsort.pass_runs, 0, sizeof(gsort.pass_runs));
	memset(gsort.pass_pruns, 0, sizeof(gsort.pass_pruns));
	if (!ta_gpu_on || !ta_walk_table(ctx, &count))
		return false;
	/* A render of some of the frame's render passes (ta_select_passes())
	 * has the full parse, which makes those alone */
	out->why = TA_GPU_WHY_PARSE;
	if (!tactx_all_passes(ctx))
		return false;
	if (draw)
	{
		/* What only the full parse makes: a frame it passes over (frame
		 * skipping), whose bookkeeping it keeps; the flat strips' and the
		 * region array's table, which are made from the vertices */
		out->why = TA_GPU_WHY_PARSE;
		if (!(ctx->rend.isRTT || 0 == ((ta_parse_cnt + 1) % (settings.pvr.ta_skip + 1)))
				|| region_table_wanted)
			return false;
		/* A renderer that sorts translucent polygons on the CPU sorts them
		 * from the CPU's vertices: it has the full parse for a frame that
		 * may have a sorted pass */
		out->why = TA_GPU_WHY_SORT;
		if (((flags & TA_GPU_NO_SORT) || ((flags & TA_GPU_SORT) && gsort.room == 0)) && ta_gpu_any_autosort(ctx))
			return false;
	}
	out->why = TA_GPU_WHY_WALK;
	f->bg_in = *TL_HEAD(&ctx->rend.global_param_op.l, PolyParam);
	f->x_clip_in = ctx->rend.fb_X_CLIP;
	f->y_clip_in = ctx->rend.fb_Y_CLIP;
	f->gpu = true;
	ok = ta_walk_run(ctx, f, count, false);
	f->gpu = false;
	if (!ok || f->gpu_bad || f->passes > TAG_MAX_PASS)
		return false;

	ta_gpu_sizes(&sz);
	/* What the full parse overruns with is left to it: it drops the frame
	 * then, as it always has */
	if (f->verts > TL_CAP(&ctx->rend.verts.l, Vertex) || f->verts > sz.verts
			|| f->modtrig > TL_CAP(&ctx->rend.modtrig.l, ModTriangle) || f->modtrig > sz.modtrig)
		return false;

	/* the headers with vertices, by segment, and the vertices of each */
	nseg = f->passes * 3;
	memset(seg_hdr, 0, sizeof(seg_hdr));
	memset(seg_v, 0, sizeof(seg_v));
	seg_hdr[0] = 1;
	seg_v[0] = 4;
	nhdr = 1;
	nruns = nmvruns = 0;
	for (i = 0; i < f->headers; i++)
	{
		const TaWalkHeader *h = &f->header[i];
		u32 seg;
		if (h->pp.count == 0)
			continue;
		seg = h->pass * 3 + (u32)w_pp_list(h->list);
		seg_hdr[seg]++;
		seg_v[seg] += h->pp.count;
		nhdr++;
	}
	for (i = 0; i < f->runs; i++)
		if (f->run[i].header & TA_WALK_MODVOL)
			nmvruns++;
		else
			nruns++;

	off_run = TAG_T_WORDS;
	off_hdr = off_run + nruns * TAG_R_WORDS;
	off_mvrun = off_hdr + nhdr * TAG_H_WORDS;
	off_seg = off_mvrun + nmvruns * TAG_M_WORDS;
	off_bg = off_seg + nseg * TAG_G_WORDS;
	off_shd = off_bg + 4 * TAG_V_WORDS;
	shadows = draw && (flags & TA_GPU_SHADOWS) != 0;
	/* (an entry for each draw group at most, which is a header at most) */
	words = off_shd + (shadows ? TAG_SH_ENTRY + nhdr : 0);
	if (words > table_room || nhdr > sz.headers)
		return false;

	if (draw)
	{
		/* The full parse drops a frame that runs out of room for its
		 * indices or polygon parameters, and the screen keeps the last
		 * picture; the decode knows that only on the GPU, which can then
		 * leave out the draws but not keep the picture. So the frames that
		 * might run out are left to the full parse, by what can be known
		 * here: a list has one more polygon parameter than strips while it
		 * is being filled (and the opaque list the background's), and a
		 * strip has a vertex at least - or, where that is too many, as many
		 * strips as the vertices' end-of-strip bits say, which is one word
		 * read a vertex, and every fourth corner of a sprite; make_index()
		 * puts at most four indices a vertex - one for the vertex, one for
		 * the parity, and the two that link a strip, or a run of infinite
		 * vertices, to what came before. */
		out->why = TA_GPU_WHY_ROOM;
		if (!ta_gpu_room(f, ctx))
			return false;
		/* The sorted passes' translucent triangles, in the order the
		 * per-triangle renderer draws them */
		out->why = TA_GPU_WHY_SORT;
		gsort.first_provoking = (flags & TA_GPU_PROVOKE) != 0;
		if ((flags & TA_GPU_SORT) && !ta_gpu_sort_frame(f, ctx->tad.thd_root))
			return false;
		if (flat_strips_wanted)
			ta_gpu_flat_headers(f, ctx->tad.thd_root);
		/* Drawn from here on: the lists hold a polygon parameter for each
		 * draw group (see TA_GPU_DRAW) */
		TL_CLEAR(&rc->global_param_op.l);
		TL_CLEAR(&rc->global_param_pt.l);
		TL_CLEAR(&rc->global_param_tr.l);
	}
	out->why = TA_GPU_WHY_NONE;

	/* where each segment's headers and vertices start, in index order */
	memset(&put, 0, sizeof(put));
	put.table = table;
	put.off_hdr = off_hdr;
	put.draw = draw;
	put.lists[0] = &rc->global_param_op.l;
	put.lists[1] = &rc->global_param_pt.l;
	put.lists[2] = &rc->global_param_tr.l;
	{
		u32 h = 0, v = 0;
		for (i = 0; i < nseg; i++)
		{
			u32 *g = &table[off_seg + i * TAG_G_WORDS];
			seg_next_h[i] = h;
			seg_next_v[i] = v;
			g[TAG_G_VSTART] = v;
			g[TAG_G_VEND] = v + seg_v[i];
			g[2] = g[3] = 0;
			h += seg_hdr[i];
			v += seg_v[i];
			put.seg_last[i] = TAG_NONE;
			put.seg_slot[i] = TAG_NONE;
			put.seg_tr[i] = NULL;
			put.seg_trb[i] = put.seg_trv[i] = TAG_NONE;
			put.seg_groups[i] = 0;
		}
	}

	/* the background, first of the first pass's opaque list (its group is 0) */
	put_header(&put, seg_next_h[0]++, &f->bg, 0, 4, 0, TAG_FMT_BG, 1, 0, 0, 0, NULL);
	seg_next_v[0] += 4;
	/* the others, as they came (which is their order within a segment);
	 * the runs are given the headers' new numbers through the old ones'
	 * group words, which are not needed after this */
	for (i = 0; i < f->headers; i++)
	{
		TaWalkHeader *h = &f->header[i];
		u32 seg, id;
		if (h->pp.count == 0)
			continue;
		l = (u32)w_pp_list(h->list);
		seg = h->pass * 3 + l;
		id = put_header(&put, seg_next_h[seg]++, &h->pp, h->pp.first, h->pp.count, seg_next_v[seg],
				h->format, h->vblocks, l, seg, h->group, &f->face[i]);
		seg_next_v[seg] += h->pp.count;
		h->group = id;
	}
	{
		u32 r = 0, m = 0;
		for (i = 0; i < f->runs; i++)
		{
			const TaWalkRun *run = &f->run[i];
			if (run->header & TA_WALK_MODVOL)
			{
				u32 *w = &table[off_mvrun + m++ * TAG_M_WORDS];
				w[TAG_M_BLOCK] = run->block;
				w[TAG_M_FIRST] = run->vertex;
				w[TAG_M_COUNT] = (run->blocks + 1) / 2;
				w[3] = 0;
			}
			else
			{
				u32 *w = &table[off_run + r++ * TAG_R_WORDS];
				w[TAG_R_BLOCK] = run->block;
				w[TAG_R_VERTEX] = run->vertex;
				w[TAG_R_COUNT] = 0;
				w[TAG_R_HDR] = f->header[run->header].group;
			}
		}
	}
	memcpy(&table[off_bg], TL_HEAD(&ctx->rend.verts.l, Vertex), 4 * sizeof(Vertex));

	t = table;
	t[TAG_T_NVERTS] = f->verts;
	t[TAG_T_NRUNS] = nruns;
	t[TAG_T_NHDR] = nhdr;
	t[TAG_T_NMVRUN] = nmvruns;
	t[TAG_T_NMOD] = f->modtrig;
	t[TAG_T_NSEG] = nseg;
	t[TAG_T_NSLOT] = put.nslots;
	t[TAG_T_FLAGS] = (screen_height > 480 ? TAG_FLAG_BLEED : 0) | ((flags & TA_GPU_PROVOKE) ? TAG_FLAG_PROVOKE : 0)
			| ((flags & TA_GPU_INSTANCE) ? TAG_FLAG_INSTANCE : 0);
	t[TAG_T_ROOM_IDX] = TL_CAP(&ctx->rend.idx.l, u32);
	t[TAG_T_ROOM_OP] = TL_CAP(&ctx->rend.global_param_op.l, PolyParam);
	t[TAG_T_ROOM_PT] = TL_CAP(&ctx->rend.global_param_pt.l, PolyParam);
	t[TAG_T_ROOM_TR] = TL_CAP(&ctx->rend.global_param_tr.l, PolyParam);
	t[TAG_T_OFF_RUN] = off_run;
	t[TAG_T_OFF_HDR] = off_hdr;
	t[TAG_T_OFF_MVRUN] = off_mvrun;
	t[TAG_T_OFF_SEG] = off_seg;
	t[TAG_T_OFF_BG] = off_bg;
	memcpy(&t[TAG_T_TA_BASE], layout->words, sizeof(layout->words));
	t[TAG_T_NPASS] = f->passes;
	t[TAG_T_ROOM_VTX] = sz.verts;
	t[TAG_T_OFF_SHD] = off_shd;
	t[TAG_T_NSHD] = 0;
	t[TAG_T_SHD_CMD] = sz.headers;
	t[TAG_T_PREP] = (flags & TA_GPU_SORT) && gsort.checks != 0 ? ta_gpu_sorted_indices() : TAG_NONE;

	out->table_words = words;
	out->verts = f->verts;
	out->modtrig = f->modtrig;
	out->headers = nhdr;
	out->slots = put.nslots;
	out->segments = nseg;
	out->passes = f->passes;
	memcpy(out->pass, f->pass, sizeof(out->pass));
	out->x_clip = f->x_clip;
	out->y_clip = f->y_clip;
	out->rv = f->rv;
	out->trd = put.ntrd;

	if (draw)
	{
		/* The rest of what the full parse leaves for the renderer: the
		 * render passes, with each list's draw groups as its count, the
		 * modifier volumes, the clipping rectangle. The vertices, indices
		 * and modifier volume triangles are the GPU's; fZ_max too, which
		 * the Vulkan renderers do not use. */
		u32 groups[3], p;
		groups[0] = groups[1] = groups[2] = 0;
		TL_CLEAR(&rc->render_passes.l);
		for (p = 0; p < f->passes; p++)
		{
			RenderPass *rp = TL_APPEND(&rc->render_passes.l, RenderPass);
			*rp = f->pass[p];
			for (l = 0; l < 3; l++)
				groups[l] += put.seg_groups[p * 3 + l];
			rp->op_count = groups[0];
			rp->pt_count = groups[1];
			rp->tr_count = groups[2];
		}
		TL_CLEAR(&rc->global_param_mvo.l);
		TL_CLEAR(&rc->global_param_mvo_tr.l);
		if (f->mvos[0] != 0)
			memcpy(TL_APPENDN(&rc->global_param_mvo.l, ModifierVolumeParam, f->mvos[0]), f->mvo[0], f->mvos[0] * sizeof(ModifierVolumeParam));
		if (f->mvos[1] != 0)
			memcpy(TL_APPENDN(&rc->global_param_mvo_tr.l, ModifierVolumeParam, f->mvos[1]), f->mvo[1], f->mvos[1] * sizeof(ModifierVolumeParam));
		rc->fb_X_CLIP = f->x_clip;
		rc->fb_Y_CLIP = f->y_clip;
		rc->gpu = true;
		rc->gpu_modtrig = f->modtrig;
		if (modvol_clip_wanted)
			ta_gpu_modvol_clip(f, ctx->tad.thd_root, rc);
		ta_parse_cnt++;
		/* The draw groups the per-triangle renderer draws again for
		 * shadows (Drawer::DrawShadowed()), each with its render pass's
		 * volumes' bounds, for the GPU to say which reach into them */
		if (shadows)
		{
			u32 *sh = &table[off_shd];
			u32 nsh = 0, prev[2];
			prev[0] = prev[1] = 0;
			ta_gpu_shadow_areas(f, ctx->tad.thd_root, rc->gpu_shadow_area);
			for (p = 0; p < f->passes; p++)
			{
				ScreenBounds area;
				const RenderPass *rp = &TL_HEAD(&rc->render_passes.l, RenderPass)[p];
				u32 end[2];
				memcpy(&sh[TAG_SH_AREA + p * 4], rc->gpu_shadow_area[p], 4 * sizeof(u32));
				area.min_x = rc->gpu_shadow_area[p][0];
				area.min_y = rc->gpu_shadow_area[p][1];
				area.max_x = rc->gpu_shadow_area[p][2];
				area.max_y = rc->gpu_shadow_area[p][3];
				end[0] = rp->op_count;
				end[1] = rp->pt_count;
				for (l = 0; l < 2; l++)
				{
					for (i = prev[l]; i < end[l] && !screen_bounds_empty(&area); i++)
					{
						const PolyParam *pp = &TL_HEAD(put.lists[l], PolyParam)[i];
						if (CanDrawShadowed(l == 0 ? ListType_Opaque : ListType_Punch_Through, pp))
							sh[TAG_SH_ENTRY + nsh++] = TAG_SH(pp->first, p);
					}
					prev[l] = end[l];
				}
			}
			t[TAG_T_NSHD] = nsh;
			out->shadows = nsh;
		}
	}
	return true;
}

void ta_set_gpu_processing(bool on)
{
	if (on == ta_gpu_on && (!on || walk_data_size >= ta_data_size()))
		return;
	if (on && !ta_walk_init())
		on = false;
	ta_gpu_on = on;
	if (!on)
		ta_gpu_sort_reserve(false);
	if (!on && !ta_walk_checking)
		ta_walk_term();
	ta_set_walk(on || ta_walk_checking);
}

/* ---- The check: the header pass beside the full parse, every frame ---- */

/* Times, with a histogram of 256 ns steps (up to 16.7 ms) for the 99th percentile */
#define WALK_HIST 65536
typedef struct TaWalkTimes
{
	u64 sum;
	u64 max;
	u32 n;
	u32 hist[WALK_HIST];
} TaWalkTimes;

static void walk_times_add(TaWalkTimes *t, u64 ns)
{
	const u64 b = ns / 256;
	t->sum += ns;
	if (ns > t->max)
		t->max = ns;
	t->hist[b < WALK_HIST ? b : WALK_HIST - 1]++;
	t->n++;
}

static double walk_times_p99_us(const TaWalkTimes *t)
{
	u32 over = 0, b = WALK_HIST;
	while (b > 0 && over * 100 < t->n)
		over += t->hist[--b];
	return b * 0.256;
}

typedef struct TaWalkStats
{
	u32 checked, same, differ, no_table, full_overrun, no_room, skipped;
	u64 headers, entries, verts;
	TaWalkTimes full, pass;
} TaWalkStats;

static TaWalkStats *walk_stats;

/* (ta_walk_full_strips(): where make_index() of a render pass would be) */
static void ta_walk_snap_full(u32 op_from, u32 pt_from, u32 tr_from)
{
	const ta_list_t *lists[3];
	u32 from[3];
	int l;
	lists[0] = &vd_rc.global_param_op.l;
	lists[1] = &vd_rc.global_param_pt.l;
	lists[2] = &vd_rc.global_param_tr.l;
	from[0] = op_from;
	from[1] = pt_from;
	from[2] = tr_from;
	for (l = 0; l < 3; l++)
	{
		const u32 used = (u32)TL_USED(lists[l], PolyParam);
		u32 i;
		for (i = from[l]; i < used && walk_full.count[l] < walk_full.room[l]; i++)
			walk_full.pp[l][walk_full.count[l]++] = TL_HEAD(lists[l], PolyParam)[i];
	}
}

/* Whether two polygon parameters are the same, but for the depth the
 * renderer works out (zvZ), and the single-rate bit of strips where flat
 * strips are looked for (the full parse sets it from the vertices) */
static bool walk_pp_same(const PolyParam *a, const PolyParam *b)
{
	const u32 pcw_mask = flat_strips_wanted ? ~PVR_PCW_SINGLE_RATE : ~0u;
	return a->first == b->first && a->count == b->count && a->texid == b->texid
			&& a->tsp == b->tsp && a->tcw == b->tcw
			&& ((a->pcw ^ b->pcw) & pcw_mask) == 0 && a->isp == b->isp
			&& a->tileclip == b->tileclip && a->tsp1 == b->tsp1
			&& a->tcw1 == b->tcw1 && a->texid1 == b->texid1;
}

/* What differs between the full parse (vd_rc as it left it, walk_full)
 * and the check's frame @s and the header pass's @h: NULL for nothing */
static const char *ta_walk_compare(const TaWalkFrame *s, const TaWalkFrame *h, bool rv, char *why, size_t room)
{
	static const char *const names[3] = { "opaque", "punch-through", "translucent" };
	const ta_list_t *mv[2];
	u32 l, i, p;

	(void)room;
	if (rv != s->rv)
		return "whether there is anything to draw";
	if ((u32)TL_USED(&vd_rc.verts.l, Vertex) != s->verts)
	{
		sprintf(why, "vertices: %d, %u", TL_USED(&vd_rc.verts.l, Vertex), s->verts);
		return why;
	}
	if ((u32)TL_USED(&vd_rc.modtrig.l, ModTriangle) != s->modtrig)
	{
		sprintf(why, "modifier volume triangles: %d, %u", TL_USED(&vd_rc.modtrig.l, ModTriangle), s->modtrig);
		return why;
	}
	if ((u32)TL_USED(&vd_rc.render_passes.l, RenderPass) != s->passes)
	{
		sprintf(why, "render passes: %d, %u", TL_USED(&vd_rc.render_passes.l, RenderPass), s->passes);
		return why;
	}
	for (i = 0; i < s->passes; i++)
	{
		const RenderPass *a = &TL_HEAD(&vd_rc.render_passes.l, RenderPass)[i];
		const RenderPass *b = &s->pass[i];
		if (a->autosort != b->autosort || a->z_clear != b->z_clear || a->mv_op_tr_shared != b->mv_op_tr_shared
				|| a->op_count != b->op_count || a->mvo_count != b->mvo_count || a->pt_count != b->pt_count
				|| a->tr_count != b->tr_count || a->mvo_tr_count != b->mvo_tr_count)
		{
			sprintf(why, "render pass %u: op %u/%u pt %u/%u tr %u/%u mvo %u/%u mvo_tr %u/%u",
					i, a->op_count, b->op_count, a->pt_count, b->pt_count, a->tr_count, b->tr_count,
					a->mvo_count, b->mvo_count, a->mvo_tr_count, b->mvo_tr_count);
			return why;
		}
	}
	for (l = 0; l < 3; l++)
	{
		if (walk_full.count[l] != s->snap.count[l])
		{
			sprintf(why, "%s polygons: %u, %u", names[l], walk_full.count[l], s->snap.count[l]);
			return why;
		}
		for (i = 0; i < s->snap.count[l]; i++)
			if (!walk_pp_same(&walk_full.pp[l][i], &s->snap.pp[l][i]))
			{
				const PolyParam *a = &walk_full.pp[l][i], *b = &s->snap.pp[l][i];
				sprintf(why, "%s polygon %u (header %d): first %u/%u count %u/%u pcw %08x/%08x isp %08x/%08x tsp %08x/%08x tcw %08x/%08x clip %08x/%08x",
						names[l], i, (int)s->snap.header[l][i], a->first, b->first, a->count, b->count,
						a->pcw, b->pcw, a->isp, b->isp, a->tsp, b->tsp,
						a->tcw, b->tcw, a->tileclip, b->tileclip);
				return why;
			}
	}
	/* Each strip is drawn as its header says (the header pass's own
	 * polygon parameters, which have no strips) */
	for (l = 0; l < 3; l++)
		for (i = 0; i < s->snap.count[l]; i++)
		{
			const u32 hi = s->snap.header[l][i];
			PolyParam a = s->snap.pp[l][i];
			PolyParam b = hi == TA_WALK_BG ? s->bg : s->header[hi].pp;
			a.first = b.first = 0;
			a.count = b.count = 0;
			if (!walk_pp_same(&a, &b))
			{
				sprintf(why, "%s polygon %u and its header %d", names[l], i, (int)hi);
				return why;
			}
		}
	mv[0] = &vd_rc.global_param_mvo.l;
	mv[1] = &vd_rc.global_param_mvo_tr.l;
	for (l = 0; l < 2; l++)
	{
		if ((u32)TL_USED(mv[l], ModifierVolumeParam) != s->mvos[l])
		{
			sprintf(why, "modifier volumes (%u): %d, %u", l, TL_USED(mv[l], ModifierVolumeParam), s->mvos[l]);
			return why;
		}
		for (i = 0; i < s->mvos[l]; i++)
		{
			const ModifierVolumeParam *a = &TL_HEAD(mv[l], ModifierVolumeParam)[i], *b = &s->mvo[l][i];
			if (a->first != b->first || (b->count != TA_WALK_UNENDED && a->count != b->count)
					|| a->isp != b->isp || a->tileclip != b->tileclip)
			{
				sprintf(why, "modifier volume %u (%u)", i, l);
				return why;
			}
		}
	}
	if (rv && (vd_rc.fb_X_CLIP.full != s->x_clip.full || vd_rc.fb_Y_CLIP.full != s->y_clip.full))
		return "clipping rectangle";

	/* The draw groups: each strip with the one before in the same pass
	 * and list is joined by make_index()'s rule exactly when their headers
	 * are of a group (the translucent ones are never joined) */
	for (l = 0; l < 2; l++)
	{
		u32 at = 0;
		for (p = 0; p < s->passes; p++)
		{
			const int to = (int)(l == 0 ? s->pass[p].op_count : s->pass[p].pt_count);
			const int from = p == 0 ? 0 : (int)(l == 0 ? s->pass[p - 1].op_count : s->pass[p - 1].pt_count);
			const u32 n = to > from ? (u32)(to - from) : 0;
			for (i = 1; i < n && at + i < s->snap.count[l]; i++)
			{
				const u32 ha = s->snap.header[l][at + i - 1], hb = s->snap.header[l][at + i];
				const u32 ga = ha == TA_WALK_BG ? 0 : s->header[ha].group;
				const u32 gb = hb == TA_WALK_BG ? 0 : s->header[hb].group;
				const bool joined = poly_same_state(&s->snap.pp[l][at + i - 1], &s->snap.pp[l][at + i]);
				if (s->snap.pp[l][at + i - 1].count == 0 || s->snap.pp[l][at + i].count == 0)
					continue;	/* (no vertices: in no group) */
				if (joined != (ga == gb))
				{
					sprintf(why, "draw groups, %s polygon %u", names[l], at + i);
					return why;
				}
			}
			at += n;
		}
	}

	/* And the header pass by itself, which never saw a vertex, made the
	 * same headers, runs, volumes and passes as the check's */
	if (h->headers != s->headers || memcmp(h->header, s->header, h->headers * sizeof(TaWalkHeader)) != 0)
		return "the header pass's headers";
	if (h->runs != s->runs || memcmp(h->run, s->run, h->runs * sizeof(TaWalkRun)) != 0)
		return "the header pass's vertex runs";
	if (h->passes != s->passes || h->verts != s->verts || h->modtrig != s->modtrig || h->rv != s->rv
			|| h->mvos[0] != s->mvos[0] || h->mvos[1] != s->mvos[1])
		return "the header pass's totals";
	for (i = 0; i < h->passes; i++)
		if (h->pass[i].autosort != s->pass[i].autosort || h->pass[i].z_clear != s->pass[i].z_clear
				|| h->pass[i].mv_op_tr_shared != s->pass[i].mv_op_tr_shared
				|| h->pass[i].mvo_count != s->pass[i].mvo_count || h->pass[i].mvo_tr_count != s->pass[i].mvo_tr_count)
			return "the header pass's render passes";
	if (h->rv && (h->x_clip.full != s->x_clip.full || h->y_clip.full != s->y_clip.full))
		return "the header pass's clipping rectangle";
	return NULL;
}

static void ta_walk_log(void)
{
	const TaWalkStats *st = walk_stats;
	const double nf = st->full.n ? (double)st->full.n : 1.0;
	const double np = st->pass.n ? (double)st->pass.n : 1.0;
	NOTICE_LOG(PVR, "TA header pass check: %u frames compared, %u the same, %u different; not compared: %u without a table, %u overrun, %u out of room, %u skipped\n",
			st->checked, st->same, st->differ, st->no_table, st->full_overrun, st->no_room, st->skipped);
	NOTICE_LOG(PVR, "TA header pass check: full parse mean %.1f us, p99 %.1f us, max %.1f us; header pass mean %.1f us, p99 %.1f us, max %.1f us; per frame %.0f headers, %.0f entries, %.0f vertices\n",
			st->full.sum / nf / 1000.0, walk_times_p99_us(&st->full), st->full.max / 1000.0,
			st->pass.sum / np / 1000.0, walk_times_p99_us(&st->pass), st->pass.max / 1000.0,
			st->headers / np, st->entries / np, st->verts / np);
}

/* (a frame has been through the check, compared or not) */
static void ta_walk_tick(void)
{
	const TaWalkStats *st = walk_stats;
	const u32 frames = st->checked + st->no_table + st->full_overrun + st->no_room + st->skipped;
	if (frames == 30 || frames % 300 == 0)
		ta_walk_log();
}

static bool ta_walk_check_frame(TA_context *ctx)
{
	TaWalkStats *st = walk_stats;
	TaWalkFrame *h = &walk_frame[0], *s = &walk_frame[1];
	u32 count;
	u64 t0, t1, t2;
	bool hp_ok, rv;
	char why[512];	/* (the longest is under 400) */
	const char *differ;

	/* Frames the full parse passes over (frame skipping) and frames with no
	 * table to trust are parsed as ever */
	if (!(ctx->rend.isRTT || 0 == ((ta_parse_cnt + 1) % (settings.pvr.ta_skip + 1))))
	{
		st->skipped++;
		ta_walk_tick();
		return ta_parse_vdrc_full(ctx);
	}
	if (!ta_walk_table(ctx, &count))
	{
		st->no_table++;
		ta_walk_tick();
		return ta_parse_vdrc_full(ctx);
	}
	/* (a render of some of its render passes: ta_gpu_prepare() leaves it to the parse too) */
	if (!tactx_all_passes(ctx))
	{
		st->skipped++;
		ta_walk_tick();
		return ta_parse_vdrc_full(ctx);
	}
	h->bg_in = s->bg_in = *TL_HEAD(&ctx->rend.global_param_op.l, PolyParam);
	h->x_clip_in = s->x_clip_in = ctx->rend.fb_X_CLIP;
	h->y_clip_in = s->y_clip_in = ctx->rend.fb_Y_CLIP;

	/* The lists strip by strip first: what textures there are to convert
	 * are then converted, and neither of the two timed finds any */
	walk_full.count[0] = walk_full.count[1] = walk_full.count[2] = 0;
	ta_walk_full_strips(ctx);

	t0 = ta_now_ns();
	hp_ok = ta_walk_run(ctx, h, count, false);
	t1 = ta_now_ns();
	rv = ta_parse_vdrc_full(ctx);
	t2 = ta_now_ns();

	if (ctx->rend.Overrun)
	{
		st->full_overrun++;
		ta_walk_tick();
		return rv;
	}
	if (!hp_ok || !ta_walk_run(ctx, s, count, true))
	{
		st->no_room++;
		ta_walk_tick();
		return rv;
	}
	walk_times_add(&st->full, t2 - t1);
	walk_times_add(&st->pass, t1 - t0);
	st->headers += h->headers;
	st->entries += count;
	st->verts += h->verts;
	st->checked++;

	differ = ta_walk_compare(s, h, rv, why, sizeof(why));
	if (differ == NULL)
		st->same++;
	else if (st->differ++ < 20)
		WARN_LOG(PVR, "TA header pass check: frame %u differs from the full parse: %s\n", st->checked, differ);
	ta_walk_tick();
	return rv;
}

void ta_set_walk_check(bool on)
{
	if (on == ta_walk_checking && (!on || walk_data_size >= ta_data_size()))
		return;
	if (on)
	{
		if (!ta_walk_init())
			return;
		if (walk_stats == NULL)
			walk_stats = (TaWalkStats *)calloc(1, sizeof(TaWalkStats));
		if (walk_stats == NULL)
			return;
	}
	else
	{
		if (walk_stats != NULL && walk_stats->checked != 0)
			ta_walk_log();
		/* (the GPU decode's header pass works in the same memory) */
		if (!ta_gpu_on)
			ta_walk_term();
		free(walk_stats);
		walk_stats = NULL;
	}
	ta_walk_checking = on;
	ta_set_walk(on || ta_gpu_on);
}
