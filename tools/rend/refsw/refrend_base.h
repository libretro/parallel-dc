#pragma once
/*
	This file is part of libswirl
*/
#include "license/bsd"

#include "refrend_regs.h"
#include "refsw_port.h"
#include "hw/pvr/ta_ctx.h" /* for Vertex */

typedef struct RegionArrayEntry {
    RegionArrayEntryControl control;
    ListPointer opaque;
    ListPointer opaque_mod;
    ListPointer trans;
    ListPointer trans_mod;
    ListPointer puncht;
} RegionArrayEntry;

typedef struct DrawParameters
{
    REFSW_ISP_TSP isp;
    TSP tsp;
    TCW tcw;
    TSP tsp2;
    TSP tcw2;
    u32 shadow; /* the object's shadow bit (PCW bit 7): modifier volumes act on it (added to libswirl's: see README) */
} DrawParameters;

typedef enum RenderMode {
    RM_OPAQUE,
    RM_PUNCHTHROUGH,
    RM_TRANSLUCENT,
    RM_MODIFIER,
    RM_COUNT
} RenderMode;

#define TAG_INVALID 1

typedef u32 parameter_tag_t;

typedef struct taRECT {
    int left, top, right, bottom;
} taRECT;

/* The renderer's interface (libswirl's RefRendInterface, whose one
 * backend is refsw.c's): a function a method */
typedef struct refsw refsw;

/* (libswirl's backend factory: refsw.c) */
refsw *refsw_create(u8 *vram);
void refsw_destroy(refsw *backend);

/* Clear the buffers */
void refsw_ClearBuffers(refsw *backend, u32 paramValue, float depthValue, u32 stencilValue);

/* Clear only the param buffer (used for peeling) */
void refsw_ClearParamBuffer(refsw *backend, u32 paramValue);

/* Clear and set DEPTH2 for peeling */
void refsw_PeelBuffers(refsw *backend, float depthValue, u32 stencilValue);

/* Summarize tile after rendering modvol (inside) */
void refsw_SummarizeStencilOr(refsw *backend);

/* Summarize tile after rendering modvol (outside) */
void refsw_SummarizeStencilAnd(refsw *backend);

/* Clear the pixel drawn counter */
void refsw_ClearPixelsDrawn(refsw *backend);

/* Get the pixel drawn counter. Used during layer peeling to determine when to stop processing */
u32 refsw_GetPixelsDrawn(refsw *backend);

/* Add an entry to the fpu parameters list */
parameter_tag_t refsw_AddFpuEntry(refsw *backend, DrawParameters *params, Vertex *vtx, RenderMode render_mode, ISP_BACKGND_T_type core_tag);

/* Clear the fpu parameters list */
void refsw_ClearFpuEntries(refsw *backend);

/* Get the final output of the 32x32 tile. Used to write to the VRAM framebuffer */
u8 *refsw_GetColorOutputBuffer(refsw *backend);

/* Render to ACCUM from TAG buffer
 * TAG holds references to triangles, ACCUM is the tile framebuffer */
void refsw_RenderParamTags(refsw *backend, RenderMode rm, int tileX, int tileY);

/* RasterizeTriangle */
void refsw_RasterizeTriangle(refsw *backend, RenderMode render_mode, DrawParameters *params, parameter_tag_t tag, int vertex_offset,
        const Vertex *v1, const Vertex *v2, const Vertex *v3, const Vertex *v4, taRECT *area);

/* Debug-level */
u8 *refsw_DebugGetAllBuffers(refsw *backend);
