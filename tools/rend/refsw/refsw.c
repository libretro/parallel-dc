/*
	This file is part of libswirl

   Implementes the Reference SoftWare renderer (RefRendInterface) backend for refrend_base.
   This includes buffer operations and rasterization

   Pixel operations are in refsw_pixel.c
*/
#include "license/bsd"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "refsw.h"
#include "refsw_pixel.h"

/* (std::min and std::max, as libswirl's C++ took them) */
static float mmin(float a, float b, float c, float d)
{
    float rv = b < a ? b : a;
    rv = rv < c ? rv : c;
    return d < rv ? rv : d;
}

static float mmax(float a, float b, float c, float d)
{
    float rv = a < b ? b : a;
    rv = c < rv ? rv : c;
    return rv < d ? rv : d;
}

refsw *refsw_create(u8 *vram)
{
    refsw *r = (refsw *)calloc(1, sizeof(refsw));
    r->vram = vram;
    return r;
}

void refsw_destroy(refsw *r)
{
    free(r->fpu_entires);
    free(r->lookup_key);
    free(r->lookup_tag);
    free(r->lookup_stamp);
    free(r);
}

void refsw_ClearBuffers(refsw *r, u32 paramValue, float depthValue, u32 stencilValue)
{
    float *zb = (float *)(r->render_buffer + DEPTH1_BUFFER_PIXEL_OFFSET);
    u32 *stencil = (u32 *)(r->render_buffer + STENCIL_BUFFER_PIXEL_OFFSET);
    parameter_tag_t *pb = (parameter_tag_t *)(r->render_buffer + PARAM_BUFFER_PIXEL_OFFSET);
    int i;

    for (i = 0; i < MAX_RENDER_PIXELS; i++) {
        zb[i] = depthValue;
        stencil[i] = stencilValue;
        pb[i] = paramValue;
    }
}

void refsw_ClearParamBuffer(refsw *r, u32 paramValue) {
    parameter_tag_t *pb = (parameter_tag_t *)(r->render_buffer + PARAM_BUFFER_PIXEL_OFFSET);
    int i;

    for (i = 0; i < MAX_RENDER_PIXELS; i++) {
        pb[i] = paramValue;
    }
}

void refsw_PeelBuffers(refsw *r, float depthValue, u32 stencilValue)
{
    float *zb = (float *)(r->render_buffer + DEPTH1_BUFFER_PIXEL_OFFSET);
    float *zb2 = (float *)(r->render_buffer + DEPTH2_BUFFER_PIXEL_OFFSET);
    u32 *stencil = (u32 *)(r->render_buffer + STENCIL_BUFFER_PIXEL_OFFSET);
    int i;

    for (i = 0; i < MAX_RENDER_PIXELS; i++) {
        zb2[i] = zb[i];     /* keep old ZB for reference */
        zb[i] = depthValue;    /* set the "closest" test to furthest value possible */
        stencil[i] = stencilValue;
    }
}

void refsw_SummarizeStencilOr(refsw *r) {
    u32 *stencil = (u32 *)(r->render_buffer + STENCIL_BUFFER_PIXEL_OFFSET);
    int i;

    /* post movdol merge INSIDE */
    for (i = 0; i < MAX_RENDER_PIXELS; i++) {
        stencil[i] |= (stencil[i] >>1);
    }
}

void refsw_SummarizeStencilAnd(refsw *r) {
    u32 *stencil = (u32 *)(r->render_buffer + STENCIL_BUFFER_PIXEL_OFFSET);
    int i;

    /* post movdol merge OUTSIDE */
    for (i = 0; i < MAX_RENDER_PIXELS; i++) {
        stencil[i] &= (stencil[i] >>1);
    }
}

void refsw_ClearPixelsDrawn(refsw *r)
{
    r->PixelsDrawn = 0;
}

u32 refsw_GetPixelsDrawn(refsw *r)
{
    return r->PixelsDrawn;
}

/* Lookup/create cached TSP parameters, and call PixelFlush_tsp */
static bool PixelFlush_tsp(refsw* backend, float x, float y, u8 *rb, float invW, parameter_tag_t tag)
{
    const FpuEntry *entry;
    if (tag & TAG_INVALID)
        return false;

    entry = &backend->fpu_entires[(tag>>1) - 1];

    return refsw_pixel_tsp(entry, x, y, 1/invW, rb);
}

 /* Render to ACCUM from TAG buffer
  * TAG holds references to triangles, ACCUM is the tile framebuffer */
void refsw_RenderParamTags(refsw *r, RenderMode rm, int tileX, int tileY) {

    u32 *rb = r->render_buffer;
    float halfpixel = HALF_OFFSET.tsp_pixel_half_offset ? 0.5f : 0;
    int x, y;
    (void)rm;

    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            PixelFlush_tsp(r, tileX + x + halfpixel, tileY + y + halfpixel, (u8*)rb,  *(f32*)&rb[DEPTH1_BUFFER_PIXEL_OFFSET],  *(parameter_tag_t*)&rb[PARAM_BUFFER_PIXEL_OFFSET]);
            rb++;
        }
    }
}

/* (libswirl's map of core tags to tags) */
static u32 lookup_slot(const refsw *r, u32 key)
{
    u32 at = (key * 2654435761u) & (r->lookup_room - 1);
    while (r->lookup_stamp[at] == r->lookup_now && r->lookup_key[at] != key)
        at = (at + 1) & (r->lookup_room - 1);
    return at;
}

static void lookup_put(refsw *r, u32 key, u32 tag)
{
    u32 at;
    if ((r->lookup_used + 1) * 2 > r->lookup_room)
    {
        u32 *keys = r->lookup_key, *tags = r->lookup_tag, *stamps = r->lookup_stamp;
        const u32 room = r->lookup_room, now = r->lookup_now;
        u32 i;
        r->lookup_room = room ? room * 2 : 1024;
        r->lookup_key = (u32 *)malloc(r->lookup_room * sizeof(u32));
        r->lookup_tag = (u32 *)malloc(r->lookup_room * sizeof(u32));
        r->lookup_stamp = (u32 *)calloc(r->lookup_room, sizeof(u32));
        r->lookup_now = 1;
        for (i = 0; i < room; i++)
            if (stamps[i] == now)
            {
                at = lookup_slot(r, keys[i]);
                r->lookup_key[at] = keys[i];
                r->lookup_tag[at] = tags[i];
                r->lookup_stamp[at] = 1;
            }
        free(keys);
        free(tags);
        free(stamps);
    }
    at = lookup_slot(r, key);
    if (r->lookup_stamp[at] != r->lookup_now)
        r->lookup_used++;
    r->lookup_key[at] = key;
    r->lookup_tag[at] = tag;
    r->lookup_stamp[at] = r->lookup_now;
}

parameter_tag_t refsw_AddFpuEntry(refsw *r, DrawParameters* params, Vertex* vtx, RenderMode render_mode, ISP_BACKGND_T_type core_tag)
{
    FpuEntry entry;
    parameter_tag_t tag;

    if (r->lookup_room != 0)
    {
        const u32 at = lookup_slot(r, core_tag.full);
        if (r->lookup_stamp[at] == r->lookup_now)
            return r->lookup_tag[at];
    }

    memset(&entry, 0, sizeof(entry));
    entry.params = *params;
    entry.render_mode = render_mode;
    /* generate */
    if (entry.params.isp.Texture)
    {
        entry.texture = raw_GetTexture(r->vram, entry.params.tsp, entry.params.tcw);
    }

    IPs3_Setup(&entry.ips, params, &entry.texture, &vtx[0], &vtx[1], &vtx[2]);

    if (r->fpu_entires_count == r->fpu_entires_room)
    {
        r->fpu_entires_room = r->fpu_entires_room ? r->fpu_entires_room * 2 : 1024;
        r->fpu_entires = (FpuEntry *)realloc(r->fpu_entires, r->fpu_entires_room * sizeof(FpuEntry));
    }
    r->fpu_entires[r->fpu_entires_count++] = entry;

    tag = (parameter_tag_t)(r->fpu_entires_count << 1);

    lookup_put(r, core_tag.full, tag);

    return tag;
}

void refsw_ClearFpuEntries(refsw *r) {
    r->fpu_entires_count = 0;
    r->lookup_used = 0;
    if (++r->lookup_now == 0)
    {
        if (r->lookup_room != 0)
            memset(r->lookup_stamp, 0, r->lookup_room * sizeof(u32));
        r->lookup_now = 1;
    }
}

#define FLUSH_NAN(a) isnan(a) ? 0 : a

/* Rasterize a single triangle to ISP (or ISP+TSP for PT) */
void refsw_RasterizeTriangle(refsw *r, RenderMode render_mode, DrawParameters* params, parameter_tag_t tag, int vertex_offset,
        const Vertex *v1, const Vertex *v2, const Vertex *v3, const Vertex* v4, taRECT* area)
{
    const int stride_bytes = STRIDE_PIXEL_OFFSET * 4;
    /* Plane equation */

    const float Y1 = FLUSH_NAN(v1->y);
    const float Y2 = FLUSH_NAN(v2->y);
    const float Y3 = FLUSH_NAN(v3->y);
    const float Y4 = v4 ? FLUSH_NAN(v4->y) : 0;

    const float X1 = FLUSH_NAN(v1->x);
    const float X2 = FLUSH_NAN(v2->x);
    const float X3 = FLUSH_NAN(v3->x);
    const float X4 = v4 ? FLUSH_NAN(v4->x) : 0;

    int sgn = 1;

    /* (this core) a vertex far out: the triangle is set up in double
     * (see REFSW_NEAR in refsw.h) */
    const bool far = refsw_far(X1, Y1) || refsw_far(X2, Y2) || refsw_far(X3, Y3) || (v4 && refsw_far(X4, Y4));

    int minx, miny, spanx, spany, x, y;
    float DX12, DX23, DX31, DX41, DY12, DY23, DY31, DY41, C1, C2, C3, C4;
    float halfpixel, y_ps, minx_ps;
    RefswExactEdges exact_edges;
    PlaneStepper3 Z;
    u8* cb_y;

    /* cull */
    {
        /* area: (X1-X3)*(Y2-Y3)-(Y1-Y3)*(X2-X3) */
        float tri_area = far ? (float)(((double)X1 - X3) * ((double)Y2 - Y3) - ((double)Y1 - Y3) * ((double)X2 - X3))
                             : ((X1 - X3) * (Y2 - Y3) - (Y1 - Y3) * (X2 - X3));

        if (tri_area > 0)
            sgn = -1;

        if (params->isp.CullMode != 0) {
            float abs_area = fabsf(tri_area);

            if (abs_area < FPU_CULL_VAL)
                return;

            if (params->isp.CullMode >= 2) {
                u32 mode = vertex_offset ^ (params->isp.CullMode & 1);

                if (
                    (mode == 0 && tri_area < 0) ||
                    (mode == 1 && tri_area > 0)) {
                    return;
                }
            }
        }
    }

    /* Bounding rectangle */
    minx = mmin(X1, X2, X3, area->left);
    miny = mmin(Y1, Y2, Y3, area->top);

    spanx = mmax(X1+1, X2+1, X3+1, area->right - 1) - minx + 1;
    spany = mmax(Y1+1, Y2+1, Y3+1, area->bottom - 1) - miny + 1;

    /* Inside scissor area? */
    if (spanx < 0 || spany < 0)
        return;

    /* Half-edge constants */
    DX12 = sgn * (X1 - X2);
    DX23 = sgn * (X2 - X3);
    DX31 = v4 ? sgn * (X3 - X4) : sgn * (X3 - X1);
    DX41 = v4 ? sgn * (X4 - X1) : 0;

    DY12 = sgn * (Y1 - Y2);
    DY23 = sgn * (Y2 - Y3);
    DY31 = v4 ? sgn * (Y3 - Y4) : sgn * (Y3 - Y1);
    DY41 = v4 ? sgn * (Y4 - Y1) : 0;

    C1 = DY12 * X1 - DX12 * Y1;
    C2 = DY23 * X2 - DX23 * Y2;
    C3 = DY31 * X3 - DX31 * Y3;
    C4 = v4 ? DY41 * X4 - DX41 * Y4 : 1;

    exact_edges.n = 0;
    if (far)
    {
        float X[4], Y[4];
        X[0] = X1; X[1] = X2; X[2] = X3; X[3] = X4;
        Y[0] = Y1; Y[1] = Y2; Y[2] = Y3; Y[3] = Y4;
        RefswExactEdges_Setup(&exact_edges, sgn, X, Y, v4 ? 4 : 3);
    }

    cb_y = (u8*)r->render_buffer;
    cb_y += (miny - area->top) * stride_bytes + (minx - area->left) * 4;

    PlaneStepper3_Setup(&Z, v1, v2, v3, v1->z, v2->z, v3->z);

    halfpixel = HALF_OFFSET.fpu_pixel_half_offset ? 0.5f : 0;
    y_ps = miny + halfpixel;
    minx_ps = minx + halfpixel;

    /* Loop through pixels */
    for (y = spany; y > 0; y -= 1)
    {
        u8* cb_x = cb_y;
        float x_ps = minx_ps;
        for (x = spanx; x > 0; x -= 1)
        {
            float Xhs12 = C1 + DX12 * y_ps - DY12 * x_ps;
            float Xhs23 = C2 + DX23 * y_ps - DY23 * x_ps;
            float Xhs31 = C3 + DX31 * y_ps - DY31 * x_ps;
            float Xhs41 = C4 + DX41 * y_ps - DY41 * x_ps;

            bool inTriangle = far ? RefswExactEdges_In(&exact_edges, x_ps, y_ps)
                                  : Xhs12 >= 0 && Xhs23 >= 0 && Xhs31 >= 0 && Xhs41 >= 0;

            if (inTriangle)
            {
                float invW = PlaneStepper3_Ip(&Z, x_ps, y_ps);
                refsw_pixel_isp(r, render_mode, params->isp.DepthMode, x_ps, y_ps, invW, cb_x, tag);
            }

            cb_x += 4;
            x_ps = x_ps + 1;
        }
        cb_y += stride_bytes;
        y_ps = y_ps + 1;
    }
}

u8 *refsw_GetColorOutputBuffer(refsw *r) {
    return (u8 *)(r->render_buffer + ACCUM1_BUFFER_PIXEL_OFFSET);
}

u8 *refsw_DebugGetAllBuffers(refsw *r) {
    return (u8 *)r->render_buffer;
}
