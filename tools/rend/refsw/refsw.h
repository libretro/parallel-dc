#pragma once
/*
	This file is part of libswirl
*/
#include "license/bsd"

#include "refsw_port.h" /* in place of libswirl's rend/gles/gles.h (see README) */
#include "refrend_base.h"
#include "hw/pvr/ta_ctx.h" /* for Vertex */
#include <math.h>


#define MAX_RENDER_WIDTH 32
#define MAX_RENDER_HEIGHT 32
#define MAX_RENDER_PIXELS (MAX_RENDER_WIDTH * MAX_RENDER_HEIGHT)

#define STRIDE_PIXEL_OFFSET MAX_RENDER_WIDTH

#define PARAM_BUFFER_PIXEL_OFFSET   0
#define DEPTH1_BUFFER_PIXEL_OFFSET  (MAX_RENDER_PIXELS*1)
#define DEPTH2_BUFFER_PIXEL_OFFSET  (MAX_RENDER_PIXELS*2)
#define STENCIL_BUFFER_PIXEL_OFFSET (MAX_RENDER_PIXELS*3)
#define ACCUM1_BUFFER_PIXEL_OFFSET  (MAX_RENDER_PIXELS*4)
#define ACCUM2_BUFFER_PIXEL_OFFSET  (MAX_RENDER_PIXELS*5)

/*
    (this core) A triangle with a vertex far out of the screen - a game's
    shadow volume near the eye: Jet Grind Radio's are 1e9 pixels out - is
    set up in double precision from its vertices nearer the screen: its
    half-edge functions (RefswExactEdges) and its plane equations
    (PlaneStepper3). libswirl's float setup takes an edge's constant as
    DY * X1 - DX * Y1, whose rounding at X1 = 1e9 is thousands of pixels,
    and a plane's constant the same way, so such a triangle covered pixels
    the geometry says it does not and had a wrong 1/w. Within REFSW_NEAR
    (2^16) pixels the float setup is kept to the bit: its edges are then
    within about 1/256 pixel of the exact ones, as near as the renderers
    keep a vertex (REFSW_GEO=1 in refsw_frame.c holds them to it).
*/
#define REFSW_NEAR 65536.f

static INLINE bool refsw_far(float x, float y)
{
    return !(fabsf(x) < REFSW_NEAR && fabsf(y) < REFSW_NEAR);
}

static INLINE bool refsw_far3(const Vertex *v1, const Vertex *v2, const Vertex *v3)
{
    return refsw_far(v1->x, v1->y) || refsw_far(v2->x, v2->y) || refsw_far(v3->x, v3->y);
}

/* the one of two vertices nearer the screen's origin */
static INLINE bool refsw_nearer(double xa, double ya, double xb, double yb)
{
    const double a = fabs(xa) < fabs(ya) ? fabs(ya) : fabs(xa);
    const double b = fabs(xb) < fabs(yb) ? fabs(yb) : fabs(xb);
    return a <= b;
}

/* a triangle's (or quad's) half-edge functions, each taken from its vertex
 * nearer the screen, so that a sample's place loses nothing to a vertex
 * billions of pixels out; the same sign convention and the same >= 0 test
 * as refsw_RasterizeTriangle()'s float ones */
typedef struct RefswExactEdges
{
    double dx[4], dy[4], x0[4], y0[4];
    int n;
} RefswExactEdges;

static INLINE void RefswExactEdges_Setup(RefswExactEdges *e, int sgn, const float* X, const float* Y, int count)
{
    int i;
    e->n = count;
    for (i = 0; i < e->n; i++)
    {
        int a = i, b = (i + 1) % e->n;
        e->dx[i] = sgn * ((double)X[a] - X[b]);
        e->dy[i] = sgn * ((double)Y[a] - Y[b]);
        if (!refsw_nearer(X[a], Y[a], X[b], Y[b]))
            a = b;
        e->x0[i] = X[a];
        e->y0[i] = Y[a];
    }
}

static INLINE bool RefswExactEdges_In(const RefswExactEdges *e, double x, double y)
{
    int i;
    for (i = 0; i < e->n; i++)
        if (e->dx[i] * (y - e->y0[i]) - e->dy[i] * (x - e->x0[i]) < 0)
            return false;
    return true;
}

/*
    Surface equation solver
*/
typedef struct PlaneStepper3
{
    float ddx, ddy;
    float c;
    /* (this core) a triangle with a vertex far out: the plane in double,
     * from its vertex nearer the screen (see REFSW_NEAR) */
    bool exact;
    double e_ddx, e_ddy, e_a, e_x, e_y;
} PlaneStepper3;

static INLINE void PlaneStepper3_Setup(PlaneStepper3 *ps, const Vertex *v1, const Vertex *v2, const Vertex *v3, float v1_a, float v2_a, float v3_a)
{
    float Aa, Ba, C;
    ps->exact = refsw_far3(v1, v2, v3);
    if (ps->exact)
    {
        /* rotated so that the first vertex is the nearest, which keeps
         * the winding and so every sign below */
        const Vertex* v[3];
        double a[3];
        int k = 0, i;
        const Vertex *p, *q, *r;
        double pa, qa, ra, qx, qy, rx, ry, eAa, eBa, eC;
        v[0] = v1; v[1] = v2; v[2] = v3;
        a[0] = v1_a; a[1] = v2_a; a[2] = v3_a;
        for (i = 1; i < 3; i++)
            if (!refsw_nearer(v[k]->x, v[k]->y, v[i]->x, v[i]->y))
                k = i;
        p = v[k];
        q = v[(k + 1) % 3];
        r = v[(k + 2) % 3];
        pa = a[k]; qa = a[(k + 1) % 3]; ra = a[(k + 2) % 3];
        qx = (double)q->x - p->x; qy = (double)q->y - p->y;
        rx = (double)r->x - p->x; ry = (double)r->y - p->y;
        eAa = (ra - pa) * qy - (qa - pa) * ry;
        eBa = rx * (qa - pa) - qx * (ra - pa);
        eC = qx * ry - rx * qy;
        ps->e_ddx = -eAa / eC;
        ps->e_ddy = -eBa / eC;
        ps->e_a = pa;
        ps->e_x = p->x;
        ps->e_y = p->y;
        ps->ddx = ps->ddy = ps->c = 0;
        return;
    }
    Aa = ((v3_a - v1_a) * (v2->y - v1->y) - (v2_a - v1_a) * (v3->y - v1->y));
    Ba = ((v3->x - v1->x) * (v2_a - v1_a) - (v2->x - v1->x) * (v3_a - v1_a));

    C = ((v2->x - v1->x) * (v3->y - v1->y) - (v3->x - v1->x) * (v2->y - v1->y));

    ps->ddx = -Aa / C;
    ps->ddy = -Ba / C;

    ps->c = (v1_a - ps->ddx * v1->x - ps->ddy * v1->y);
}

static INLINE float PlaneStepper3_Ip(const PlaneStepper3 *ps, float x, float y)
{
    if (ps->exact)
        return (float)(ps->e_a + ps->e_ddx * (x - ps->e_x) + ps->e_ddy * (y - ps->e_y));
    return x * ps->ddx + y * ps->ddy + ps->c;
}

static INLINE float PlaneStepper3_IpW(const PlaneStepper3 *ps, float x, float y, float W)
{
    return PlaneStepper3_Ip(ps, x, y) * W;
}

/*
    Interpolation helper
*/
typedef struct IPs3
{
    PlaneStepper3 U;
    PlaneStepper3 V;
    PlaneStepper3 Col[4];
    PlaneStepper3 Ofs[4];
} IPs3;

static INLINE void IPs3_Setup(IPs3 *ips, DrawParameters* params, text_info* texture, const Vertex *v1, const Vertex *v2, const Vertex *v3)
{
    u32 w = 0, h = 0;
    int i;
    if (texture) {
        w = texture->width;
        h = texture->height;
    }

    PlaneStepper3_Setup(&ips->U, v1, v2, v3, v1->u * w * v1->z, v2->u * w * v2->z, v3->u * w * v3->z);
    PlaneStepper3_Setup(&ips->V, v1, v2, v3, v1->v * h * v1->z, v2->v * h * v2->z, v3->v * h * v3->z);
    if (params->isp.Gouraud) {
        for (i = 0; i < 4; i++)
            PlaneStepper3_Setup(&ips->Col[i], v1, v2, v3, v1->col[i] * v1->z, v2->col[i] * v2->z, v3->col[i] * v3->z);

        for (i = 0; i < 4; i++)
            PlaneStepper3_Setup(&ips->Ofs[i], v1, v2, v3, v1->vtx_spc[i] * v1->z, v2->vtx_spc[i] * v2->z, v3->vtx_spc[i] * v3->z);
    } else {
        for (i = 0; i < 4; i++)
            PlaneStepper3_Setup(&ips->Col[i], v1, v2, v3, v3->col[i] * v1->z, v3->col[i] * v2->z, v3->col[i] * v3->z);

        for (i = 0; i < 4; i++)
            PlaneStepper3_Setup(&ips->Ofs[i], v1, v2, v3, v3->vtx_spc[i] * v1->z, v3->vtx_spc[i] * v2->z, v3->vtx_spc[i] * v3->z);
    }
}

typedef union Color {
    u32 raw;
    u8 bgra[4];
    struct {
        u8 b;
        u8 g;
        u8 r;
        u8 a;
    };
} Color;

/* Used for deferred TSP processing lookups: the pixel pipeline's choices
 * (libswirl's function tables, by these words) are made from params, and
 * render_mode is the one it was added for (the punch-through alpha test) */
typedef struct FpuEntry
{
    IPs3 ips;
    DrawParameters params;
    text_info texture;
    RenderMode render_mode;
} FpuEntry;

struct refsw
{
    FpuEntry *fpu_entires;
    u32 fpu_entires_count, fpu_entires_room;
    /* (libswirl's map of core tags to tags: open addressing, a slot in use
     * where its stamp is the lookup's) */
    u32 *lookup_key, *lookup_tag, *lookup_stamp;
    u32 lookup_room, lookup_used, lookup_now;

    /* Used by layer peeling to determine end of processing */
    int PixelsDrawn;

    u32 render_buffer[MAX_RENDER_PIXELS * 6]; /* param pointers + depth1 + depth2 + stencil + acum 1 + acum 2 */
    u8* vram;
};
