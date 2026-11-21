/* GPU-side TA processing in OpenGL 4.3 (rend/ta_gpu.h): a frame's vertices,
 * indices and modifier volume triangles made by compute shaders from the
 * blocks the game sent, and the frame drawn from them by both OpenGL
 * renderers - per triangle (gles) and per pixel (gl4) - with nothing read
 * back. Vulkan's is vulkan/ta_decoder.h; this is the same, in OpenGL's terms.
 *
 * - The TA contexts' data are slices of one buffer, persistently mapped,
 *   coherent, in the client's memory (glBufferStorage(), OpenGL 4.4 or
 *   ARB_buffer_storage): the SH4 writes a frame straight into memory the
 *   GPU reads, and reads its control words back from it. Without that
 *   storage, or where reading it is far slower than ordinary memory (the
 *   driver gave write-combined memory), TA processing stays on the CPU.
 *   A slice is written again once the GPU is done with the frame that read
 *   it, which is known by polling that frame's fence (glClientWaitSync()
 *   with no time to wait), never by waiting.
 * - At the start of Process() the header pass (ta_gpu_prepare()) writes the
 *   frame's tables into this frame's piece of a persistently mapped ring,
 *   and makes the render context's lists of draw groups in the full
 *   parse's place; the decode is dispatched then. The draws that follow are
 *   indirect (DrawGroup()): their counts are the GPU's. A frame the header
 *   pass does not do, or one whose piece of the ring the GPU still has, is
 *   parsed and drawn as ever.
 * - The kernels are the SPIR-V for OpenGL made when the core is built
 *   (rend/ta_gpu_gl_spv.inc) where OpenGL takes SPIR-V (4.6 or
 *   ARB_gl_spirv); elsewhere the header's text (rend/ta_gpu_glsl.inc) is
 *   compiled, once, and the programs kept on disk with the renderers' own
 *   (glprogram_cache.h), by the driver and the text, for the next time.
 *   Every buffer (sized for the largest frame the machine has) is made in
 *   Setup(), when the renderer is set up or the option changes: nothing is
 *   allocated or compiled while a game runs.
 * - The outputs are on the GPU, one copy: OpenGL runs the decodes and the
 *   draws that read them in the order they were given.
 *
 * Off (TA processing: CPU), or in an OpenGL before 4.3 (and in OpenGL ES),
 * none of this exists, and gltad_ready() says no.
 *
 * C89 (ta_decoder_gl.c). */
#ifndef GLES_TA_DECODER_GL_H
#define GLES_TA_DECODER_GL_H
#include <retro_inline.h>
#include "types.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/ta_ctx.h"

#ifdef HAVE_GL4
#include <glsm/glsm.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_GL4
/* What a frame decoded on the GPU is drawn from (rend_context::gpu): as
 * vulkan/ta_decoder.h's TaGpuDraw has it */
typedef struct GlTaDraw
{
	GLuint vertices, indices, modtrig, trParams;
	/* the frame's words (TAG_O_*): 1/fZ_max at TAG_O_ZSCALE, which the
	 * renderers copy into their depth scale uniform buffer (rend/depth_scale.h) */
	GLuint frame;
	/* (the per-pixel renderer) a word for each translucent strip, its
	 * number: an attribute of one a draw, which a draw record's baseInstance
	 * picks (TA_GPU_INSTANCE) */
	GLuint instances;
	/* (TA_GPU_SORT) this frame's room for the sorted translucent triangles'
	 * indices (ta_gpu_sorted_write()), mapped, in the element buffer
	 * @sorted from @sortedOffset bytes on */
	GLuint sorted;
	GLintptr sortedOffset;
	u32 *sortedMap;
} GlTaDraw;

/* Made, or taken down, as the option says (@wanted): when the renderer is
 * set up and when the options change, never while a frame is drawn; with
 * the context bound. Off, or where it cannot be made, nothing of it
 * exists. */
void gltad_setup(bool wanted);
void gltad_term(void);
bool gltad_ready(void);
/* Whether the context the renderer has (findGLVersion()) is one this can
 * be made in: OpenGL 4.3 with persistently mapped buffers. (Whether it is
 * made is found out in gltad_setup().) */
bool gltad_context_can_do(void);
/* At the start of each Process(): the GPU's progress, polled, told to the
 * TA contexts' pool; nothing is made here */
void gltad_begin_frame(void);
/* The frame's tables, and with TA_GPU_DRAW (in @flags, TA_GPU_*) its
 * render context made ready to draw in the full parse's place; whether it
 * is decoded, and then (@rv) whether it has something to draw */
bool gltad_prepare(TA_context *ctx, u32 flags, bool *rv);
/* ...and its decode dispatched */
void gltad_record(TA_context *ctx);
/* After the frame's draws (Render()): its fence, by which its slice and its
 * piece of the ring are known to be free again */
void gltad_end_frame(void);
const GlTaDraw *gltad_draw(void);
/* The per-triangle renderer's: room for the translucent triangles it sorts
 * (TA_GPU_SORT), made with the rest; and whether there is */
void gltad_want_sort(bool on);
bool gltad_can_sort(void);
/* A draw group of a frame decoded on the GPU (PolyParam::first and count:
 * see TA_GPU_DRAW) drawn as triangle strips from the decode's indices,
 * which are bound: an opaque or punch-through one by its command - or with
 * @shadows by its second command, which the GPU wrote for its second
 * drawing for shadows (TA_GPU_SHADOWS) - a translucent one by its draw
 * records, with their count (ARB_indirect_parameters, OpenGL 4.6) or all
 * of its room, whose records past the count draw nothing */
void gltad_draw_group(u32 list_type, u32 first, u32 count, bool shadows);
/* The decode's vertices bound as GL_ARRAY_BUFFER and its indices as
 * GL_ELEMENT_ARRAY_BUFFER; its modifier volume triangles as
 * GL_ARRAY_BUFFER; and (TA_GPU_SORT) this frame's sorted indices written
 * (ta_gpu_sorted_write(): by gltad_record(), before the prepass's kernel
 * reads them), and bound as GL_ELEMENT_ARRAY_BUFFER, from the byte offset
 * it returns */
void gltad_bind_geometry(void);
void gltad_bind_modvols(void);
void gltad_write_sorted(void);
GLintptr gltad_bind_sorted(void);
#else
/* (no OpenGL 4: never ready, and a frame is never drawn from the GPU) */
typedef struct GlTaDraw
{
	unsigned frame;	/* (the frame's words of a decode that never is: no buffer) */
} GlTaDraw;
static INLINE void gltad_setup(bool wanted) { (void)wanted; }
static INLINE void gltad_term(void) {}
static INLINE bool gltad_ready(void) { return false; }
static INLINE bool gltad_context_can_do(void) { return false; }
static INLINE void gltad_begin_frame(void) {}
static INLINE bool gltad_prepare(TA_context *ctx, u32 flags, bool *rv) { (void)ctx; (void)flags; (void)rv; return false; }
static INLINE void gltad_record(TA_context *ctx) { (void)ctx; }
static INLINE void gltad_end_frame(void) {}
static INLINE const GlTaDraw *gltad_draw(void) { static const GlTaDraw none = { 0 }; return &none; }
static INLINE void gltad_want_sort(bool on) { (void)on; }
static INLINE bool gltad_can_sort(void) { return false; }
static INLINE void gltad_draw_group(u32 list_type, u32 first, u32 count, bool shadows) { (void)list_type; (void)first; (void)count; (void)shadows; }
static INLINE void gltad_bind_geometry(void) {}
static INLINE void gltad_bind_modvols(void) {}
static INLINE void gltad_write_sorted(void) {}
static INLINE intptr_t gltad_bind_sorted(void) { return 0; }
#endif

#ifdef __cplusplus
}
#endif

#endif
