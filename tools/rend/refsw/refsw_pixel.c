/*
	This file is part of libswirl

    Implemenets Pixel operations (PixelPipeline) for RefSW

    Naive easy-to-read implementation
*/

#include "license/bsd"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "refsw_pixel.h"

/* (this core) the bilinear filter's four texels each clamped, flipped or
 * wrapped on its own, as the chip's are: libswirl's default took the 2x2
 * block of the clamped top left texel, whose other texels wrap, so that
 * within half a texel of a clamped edge the filter mixed the wrong texels
 * (a clamped texture's first column with its second, its last with its
 * first) */
#define FULL_FILTERING 1

typedef union mem128i {
    uint8_t m128i_u8[16];
    int8_t m128i_i8[16];
    int16_t m128i_i16[8];
    int32_t m128i_i32[4];
    uint32_t m128i_u32[4];
} mem128i;

/* (std::min of ints, as libswirl's C++ took it) */
static INLINE int imin(int a, int b) { return b < a ? b : a; }

/*

    textel = TextureFetch<pp_Texture, pp_ingoreAlpha, volume, filtering>();
    baseCol = BaseColor<pp_UseAlpha, cheap shadows>();
    offsCol = OffsetColor<pp_Texture, pp_Offset, cheap shadows>()

    [trilinear, color] = ColorCombiner<pp_Texture, pp_Offset, pp_ShadInstr, bump map> (textel, baseCol, offsCol.rgb);

    color.bgra = PostProcess<clamp, trilinear, fog_mode>(color, trilinear, offsCol.a);

    BlendUnit<src_buf, dst_buf, src_sel, dst_sel>(color);

    (libswirl made a function of each of these for every combination of
    the words' fields it takes; here the fields are read as they are used)
*/

/* Clamp and flip a texture coordinate */
static int ClampFlip(bool pp_Clamp, bool pp_Flip, int coord, int size) {
    if (pp_Clamp) { /* clamp */
        if (coord < 0) {
            coord = 0;
        } else if (coord >= size) {
            coord = size-1;
        }
    } else if (pp_Flip) { /* flip */
        coord &= size*2-1;
        if (coord & size) {
            coord ^= size*2-1;
        }
    } else { /* wrap */
        coord &= size-1;
    }

    return coord;
}

/* Fetch pixels from UVs, interpolate */
static Color TextureFetch(TSP tsp, const text_info *texture, float u, float v) {
    const bool pp_IgnoreTexA = tsp.IgnoreTexA, pp_ClampU = tsp.ClampU, pp_ClampV = tsp.ClampV;
    const bool pp_FlipU = tsp.FlipU, pp_FlipV = tsp.FlipV;
    const u32 pp_FilterMode = tsp.FilterMode;
    int halfpixel = HALF_OFFSET.texure_pixel_half_offset ? -127 : 0;

    int ui = u * 256 + halfpixel;
    int vi = v * 256 + halfpixel;
    mem128i px;
    Color textel;
    int i;

#if FULL_FILTERING
    {
        int offset00 = ClampFlip(pp_ClampU, pp_FlipU, (ui >> 8) + 1, texture->width) + ClampFlip(pp_ClampV, pp_FlipV, (vi >> 8) + 1, texture->height) * texture->width;
        int offset01 = ClampFlip(pp_ClampU, pp_FlipU, (ui >> 8) + 0, texture->width) + ClampFlip(pp_ClampV, pp_FlipV, (vi >> 8) + 1, texture->height) * texture->width;
        int offset10 = ClampFlip(pp_ClampU, pp_FlipU, (ui >> 8) + 1, texture->width) + ClampFlip(pp_ClampV, pp_FlipV, (vi >> 8) + 0, texture->height) * texture->width;
        int offset11 = ClampFlip(pp_ClampU, pp_FlipU, (ui >> 8) + 0, texture->width) + ClampFlip(pp_ClampV, pp_FlipV, (vi >> 8) + 0, texture->height) * texture->width;

        px.m128i_u32[0] =  ((mem128i *)texture->pdata)[offset00].m128i_u32[3];
        px.m128i_u32[1] =  ((mem128i *)texture->pdata)[offset01].m128i_u32[3];
        px.m128i_u32[2] =  ((mem128i *)texture->pdata)[offset10].m128i_u32[3];
        px.m128i_u32[3] =  ((mem128i *)texture->pdata)[offset11].m128i_u32[3];
    }
#else
    {
        int offset = ClampFlip(pp_ClampU, pp_FlipU, ui >> 8, texture->width) + ClampFlip(pp_ClampV, pp_FlipV, vi >> 8, texture->height) * texture->width;
        px = ((mem128i *)texture->pdata)[offset];
    }
#endif

    textel.raw = 0xAF674839;

    if (pp_FilterMode == 0) {
        /* Point sampling */
        for (i = 0; i < 4; i++)
        {
            textel.bgra[i] = px.m128i_u8[12 + i];
        }
    } else if (pp_FilterMode == 1) {
        /* Bilinear filtering */
        int ublend = ui & 255;
        int vblend = vi & 255;
        int nublend = 256 - ublend;
        int nvblend = 256 - vblend;

        for (i = 0; i < 4; i++)
        {
            textel.bgra[i] =
                (px.m128i_u8[0 + i] * ublend * vblend +
                px.m128i_u8[4 + i] * nublend * vblend +
                px.m128i_u8[8 + i] * ublend * nvblend +
                px.m128i_u8[12 + i] * nublend * nvblend) / 65536;
        }
    } else {
        /* trilinear filtering A and B */
        die("pp_FilterMode is trilinear");
    }


    if (pp_IgnoreTexA)
    {
        textel.a = 255;
    }

    return textel;
}

/* Combine Base, Textel and Offset colors */
static Color ColorCombiner(REFSW_ISP_TSP isp, TSP tsp, Color base, Color textel, Color offset) {
    const bool pp_Texture = isp.Texture, pp_Offset = isp.Offset;
    const u32 pp_ShadInstr = tsp.ShadInstr;
    Color rv = base;
    int i;
    if (pp_Texture)
    {
        if (pp_ShadInstr == 0)
        {
            /* color.rgb = texcol.rgb; */
            /* color.a = texcol.a; */

            rv = textel;
        }
        else if (pp_ShadInstr == 1)
        {
            /* color.rgb *= texcol.rgb; */
            /* color.a = texcol.a; */
            for (i = 0; i < 3; i++)
            {
                rv.bgra[i] = textel.bgra[i] * (base.bgra[i] + 1) / 256;
            }

            rv.a = textel.a;
        }
        else if (pp_ShadInstr == 2)
        {
            /* color.rgb=mix(color.rgb,texcol.rgb,texcol.a); */
            u8 tb = textel.a;
            u8 cb = 255 - tb;

            for (i = 0; i < 3; i++)
            {
                rv.bgra[i] = (textel.bgra[i] * tb + base.bgra[i] * cb) / 256;
            }

            rv.a = base.a;
        }
        else if (pp_ShadInstr == 3)
        {
            /* color*=texcol */
            for (i = 0; i < 4; i++)
            {
                rv.bgra[i] = textel.bgra[i] * (base.bgra[i] + 1) / 256;
            }
        }

        if (pp_Offset) {
            /* mix only color, saturate */
            for (i = 0; i < 3; i++)
            {
                rv.bgra[i] = imin(rv.bgra[i] + offset.bgra[i], 255);
            }
        }
    }

    return rv;
}

/* An interpolated colour channel to the nearest of 0..255 */
static INLINE u32 Unorm8(float f) {
    f += 0.5f;
    return f >= 255.0f ? 255 : f > 0.0f ? (u32)f : 0;
}

/* Interpolate the base color, also cheap shadows modifier */
static INLINE Color InterpolateBase(bool pp_UseAlpha, bool pp_CheapShadows, const PlaneStepper3* Col, float x, float y, float W, u32 stencil) {
    Color rv;
    u32 mult = 256;

    if (pp_CheapShadows) {
        if (stencil & 1) {
            mult = FPU_SHAD_SCALE.scale_factor;
        }
    }

    rv.bgra[0] = Unorm8(PlaneStepper3_IpW(&Col[2], x, y, W)) * mult / 256;   /* FIXME: why is input in RGBA instead of BGRA here? */
    rv.bgra[1] = Unorm8(PlaneStepper3_IpW(&Col[1], x, y, W)) * mult / 256;
    rv.bgra[2] = Unorm8(PlaneStepper3_IpW(&Col[0], x, y, W)) * mult / 256;
    /* (the shadow scales the colour, not its alpha: see README) */
    rv.bgra[3] = Unorm8(PlaneStepper3_IpW(&Col[3], x, y, W));

    if (!pp_UseAlpha)
    {
        rv.a = 255;
    }

    return rv;
}

/* Interpolate the offset color, also cheap shadows modifier */
static INLINE Color InterpolateOffs(bool pp_CheapShadows, const PlaneStepper3* Ofs, float x, float y, float W, u32 stencil) {
    Color rv;
    u32 mult = 256;

    if (pp_CheapShadows) {
        if (stencil & 1) {
            mult = FPU_SHAD_SCALE.scale_factor;
        }
    }

    rv.bgra[0] = PlaneStepper3_IpW(&Ofs[2], x, y, W) * mult / 256;   /* FIXME: why is input in RGBA instead of BGRA here? */
    rv.bgra[1] = PlaneStepper3_IpW(&Ofs[1], x, y, W) * mult / 256;
    rv.bgra[2] = PlaneStepper3_IpW(&Ofs[0], x, y, W) * mult / 256;
    rv.bgra[3] = PlaneStepper3_IpW(&Ofs[3], x, y, W);

    return rv;
}

/* select/calculate blend coefficient for the blend unit */
static INLINE Color BlendCoefs(u32 pp_AlphaInst, bool srcOther, Color src, Color dst) {
    Color rv;
    int i;

    rv.raw = 0;
    switch(pp_AlphaInst>>1) {
        /* zero */
        case 0: rv.raw = 0; break;
        /* other color */
        case 1: rv = srcOther? src : dst; break;
        /* src alpha */
        case 2: for (i = 0; i < 4; i++) rv.bgra[i] = src.a; break;
        /* dst alpha */
        case 3: for (i = 0; i < 4; i++) rv.bgra[i] = dst.a; break;
    }

    if (pp_AlphaInst & 1) {
        for (i = 0; i < 4; i++)
            rv.bgra[i] = 255 - rv.bgra[i];
    }

    return rv;
}

/* Blending Unit implementation. Alpha blend, accum buffers and such */
static bool BlendingUnit(bool pp_AlphaTest, TSP tsp, Color* cb, Color col)
{
    const u32 pp_SrcSel = tsp.SrcSelect, pp_DstSel = tsp.DstSelect, pp_SrcInst = tsp.SrcInstr, pp_DstInst = tsp.DstInstr;
    Color rv;
    Color src = pp_SrcSel ? cb[MAX_RENDER_PIXELS] : col;
    Color dst = cb[pp_DstSel ? MAX_RENDER_PIXELS : 0];

    Color src_blend = BlendCoefs(pp_SrcInst, false, src, dst);
    Color dst_blend = BlendCoefs(pp_DstInst, true, src, dst);
    int j;

    for (j = 0; j < 4; j++)
    {
        rv.bgra[j] = imin((src.bgra[j] * src_blend.bgra[j] + dst.bgra[j] * dst_blend.bgra[j]) >> 8, 255);
    }

    if (!pp_AlphaTest || src.a >= PT_ALPHA_REF)
    {
        cb[pp_DstSel ? MAX_RENDER_PIXELS : 0] = rv;
        return true;
    }
    else
    {
        return false;
    }
}

static INLINE u8 LookupFogTable(float invW) {
    u8* fog_density=(u8*)&FOG_DENSITY;
    float fog_den_mant=fog_density[1]/128.0f;  /* bit 7 -> x. bit, so [6:0] -> fraction -> /128 */
    s32 fog_den_exp=(s8)fog_density[0];

    float fog_den = fog_den_mant*powf(2.0f,fog_den_exp);

    f32 fogW = fog_den * invW;
    u32 bits, index, fog_m, fog_e;
    u8 blendFactor, blend_inv, fog_alpha;
    u8 *fog_entry;

    /* (std::max, then std::min) */
    fogW = fogW < 1.0f ? 1.0f : fogW;
    fogW = 255.999985f < fogW ? 255.999985f : fogW;

    /* (the float's fields: mantissa, exponent) */
    memcpy(&bits, &fogW, 4);
    fog_m = bits & 0x7FFFFF;
    fog_e = (bits >> 23) & 0xFF;

    index = (((fog_e +1) & 7) << 4) |  ((fog_m>>19) & 15);

    blendFactor = (fog_m>>11) & 255;
    blend_inv = 255^blendFactor;

    fog_entry = (u8*)&FOG_TABLE[index];

    fog_alpha = (fog_entry[0] * blendFactor + fog_entry[1] * blend_inv) >> 8;

    return fog_alpha;
}

/* Color Clamp and Fog a pixel */
static INLINE Color FogUnit(bool pp_Offset, bool pp_ColorClamp, u32 pp_FogCtrl, Color col, float invW, u8 offs_a) {
    int i;

    if (pp_ColorClamp) {
        Color clamp_max, clamp_min;
        clamp_max.raw = FOG_CLAMP_MAX;
        clamp_min.raw = FOG_CLAMP_MIN;

        for (i = 0; i < 4; i++)
        {
            /* (std::min, then std::max) */
            col.bgra[i] = clamp_max.bgra[i] < col.bgra[i] ? clamp_max.bgra[i] : col.bgra[i];
            col.bgra[i] = col.bgra[i] < clamp_min.bgra[i] ? clamp_min.bgra[i] : col.bgra[i];
        }
    }

    switch(pp_FogCtrl) {
        /* Look up mode 1 */
        case 0:
        /* look up mode 2 */
        case 3:
            {
                u8 fog_alpha = LookupFogTable(invW);

                u8 fog_inv = 255^fog_alpha;

                Color col_ram;
                col_ram.raw = FOG_COL_RAM;

                if (pp_FogCtrl == 0) {
                    for (i = 0; i < 3; i++) {
                        col.bgra[i] = (col.bgra[i] * fog_inv + col_ram.bgra[i] * fog_alpha)>>8;
                    }
                } else {
                    for (i = 0; i < 3; i++) {
                        col.bgra[i] = col_ram.bgra[i];
                    }
                    col.a = col_ram.a;
                }
            }
            break;

        /* Per Vertex */
        case 1:
            if (pp_Offset) {
                Color col_vert;
                u8 alpha = offs_a;
                u8 inv = 255^alpha;
                col_vert.raw = FOG_COL_VERT;

                for (i = 0; i < 3; i++)
                {
                    col.bgra[i] = (col.bgra[i] * inv + col_vert.bgra[i] * alpha)>>8;
                }
            }
            break;


        /* No Fog */
        case 2:
            break;
    }

    return col;
}

/* Implement the full texture/shade pipeline for a pixel */
bool refsw_pixel_tsp(const FpuEntry *entry, float x, float y, float W, u8 *rb)
{
    const REFSW_ISP_TSP isp = entry->params.isp;
    const TSP tsp = entry->params.tsp;
    const bool pp_UseAlpha = tsp.UseAlpha, pp_Texture = isp.Texture, pp_Offset = isp.Offset, pp_ColorClamp = tsp.ColorClamp;
    const u32 pp_FogCtrl = tsp.FogCtrl;
    u32 *stencil = (u32 *)&rb[STENCIL_BUFFER_PIXEL_OFFSET * 4];
    Color *cb = (Color*)&rb[ACCUM1_BUFFER_PIXEL_OFFSET * 4];
    parameter_tag_t *pb = (parameter_tag_t*)&rb[PARAM_BUFFER_PIXEL_OFFSET * 4];
    Color base, textel, offs, col;
    u32 shadow_stencil;

    *pb |= TAG_INVALID;

    base.raw = textel.raw = offs.raw = 0;

    /* (the modifier volumes' stencil acts only on an object whose shadow bit is set: see README) */
    shadow_stencil = entry->params.shadow ? *stencil : 0;
    base = InterpolateBase(pp_UseAlpha, true, entry->ips.Col, x, y, W, shadow_stencil);

    if (pp_Texture) {
        float u = PlaneStepper3_IpW(&entry->ips.U, x, y, W);
        float v = PlaneStepper3_IpW(&entry->ips.V, x, y, W);

        textel = TextureFetch(tsp, &entry->texture, u, v);
        if (pp_Offset) {
            offs = InterpolateOffs(true, entry->ips.Ofs, x, y, W, shadow_stencil);
        }
    }

    col = ColorCombiner(isp, tsp, base, textel, offs);

    col = FogUnit(pp_Offset, pp_ColorClamp, pp_FogCtrl, col, 1/W, offs.a);

    return BlendingUnit(entry->render_mode == RM_PUNCHTHROUGH, tsp, cb, col);
}

/* Lookup/create cached TSP parameters, and call PixelFlush_tsp */
static bool AlphaTest_tsp(refsw* backend, float x, float y, u8 *rb, float invW, parameter_tag_t tag)
{
    const FpuEntry *entry = &backend->fpu_entires[(tag>>1) - 1];

    /* (this core) the TSP is run here only for the alpha test: the
     * pixel's tag and colour are left as they were (refsw_pixel_tsp()
     * marks the tag shaded and blends into the colour), so that the
     * pixel is shaded with the list's other tags afterwards - a
     * polygon behind a texel that fails the test included */
    parameter_tag_t *pb = (parameter_tag_t*)&rb[PARAM_BUFFER_PIXEL_OFFSET * 4];
    Color *cb = (Color*)&rb[ACCUM1_BUFFER_PIXEL_OFFSET * 4];
    const parameter_tag_t tag_was = *pb;
    const Color colour_was = cb[0], colour2_was = cb[MAX_RENDER_PIXELS];

    bool rv = refsw_pixel_tsp(entry, x, y, 1/invW, rb);

    *pb = tag_was;
    cb[0] = colour_was;
    cb[MAX_RENDER_PIXELS] = colour2_was;
    return rv;
}

/* Depth processing for a pixel -- render_mode 0: OPAQ, 1: PT, 2: TRANS */
void refsw_pixel_isp(refsw* backend, RenderMode render_mode, u32 depth_mode, float x, float y, float invW, u8 *pb, parameter_tag_t tag)
{
    float *zb = (float*)&pb [DEPTH1_BUFFER_PIXEL_OFFSET * 4];
    float *zb2 = (float*)&pb[DEPTH2_BUFFER_PIXEL_OFFSET * 4];
    u32 *stencil = (u32*)&pb[STENCIL_BUFFER_PIXEL_OFFSET * 4];

    u32 mode = depth_mode;

    if (render_mode == RM_PUNCHTHROUGH)
        mode = 6; /* TODO: FIXME */
    else if (render_mode == RM_TRANSLUCENT)
        mode = 3; /* TODO: FIXME */
    else if (render_mode == RM_MODIFIER)
        mode = 6;

    switch(mode) {
        /* never */
        case 0: return; break;
        /* less */
        case 1: if (invW >= *zb) return; break;
        /* equal */
        case 2: if (invW != *zb) return; break;
        /* less or equal */
        case 3: if (invW > *zb) return; break;
        /* greater */
        case 4: if (invW <= *zb) return; break;
        /* not equal */
        case 5: if (invW == *zb) return; break;
        /* greater or equal */
        case 6: if (invW < *zb) return; break;
        /* always */
        case 7: break;
    }

    switch (render_mode)
    {
        /* OPAQ */
        case RM_OPAQUE:
        {
            /* Z pre-pass only */

            *zb = invW;
            *(parameter_tag_t *)pb = tag;
        }
        break;

        case RM_MODIFIER:
        {
            /* Flip on Z pass */

            *stencil ^= 2;
        }
        break;

        /* PT */
        case RM_PUNCHTHROUGH:
        {
            /* Z + TSP syncronized for alpha test */

            if (AlphaTest_tsp(backend, x, y, pb, invW, tag))
            {
                *zb = invW;
                *(parameter_tag_t *)pb = tag;
            }
        }
        break;

        /* Layer Peeling. zb2 holds the reference depth, zb is used to find closest to reference */
        case RM_TRANSLUCENT:
        {
            if (invW < *zb2)
                return;

            if (invW == *zb || invW == *zb2) {
                parameter_tag_t tagExisting = *(parameter_tag_t *)pb;

                if (tagExisting & TAG_INVALID)
                {
                    if (tag < tagExisting)
                        return;
                }
                else
                {
                    if (tag >= tagExisting)
                        return;
                }
            }

            backend->PixelsDrawn++;

            *zb = invW;
            *(parameter_tag_t *)pb = tag;
        }
        break;

        default:
            break;
    }
}
