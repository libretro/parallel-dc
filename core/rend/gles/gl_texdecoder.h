/* Textures made on the GPU from their bytes in video memory, for the
 * OpenGL renderers: what vulkan/tex_decoder.h and dx11/dx11_texdecoder.h
 * are for the Vulkan and Direct3D 11 ones.
 *
 * The texture's bytes - the texels, the codebook of a compressed texture,
 * the part of the palette a paletted one uses - are copied as they are
 * into a ring, and a kernel of rend/texdecode.h makes the texels from them
 * into the texture of the texture pool's it was given (gles.h, gltex.c):
 * the same text the CPU's converters are tested against.
 *  - OpenGL 4.3 and later: the ring is a buffer the kernels read as a
 *    shader storage buffer. 32-bit texels and palette indices are written
 *    by compute kernels through rgba8ui and r8ui images of the level;
 *    16-bit ones are drawn into it through a framebuffer, by a fragment
 *    shader, as the 16-bit formats cannot be images.
 *  - OpenGL 3.x: no storage buffers and no compute: the ring goes to an
 *    unsigned integer texture once a frame, and every level is drawn.
 *  - A planar 16-bit texture needs no kernel: its rows go from the ring to
 *    the texture as they are, as 5-6-5, 1-5-5-5 reversed (BGRA) and
 *    4-4-4-4 reversed (BGRA), which are the PowerVR's bits in its order.
 * The ring is one buffer made when the renderer starts, a share of the
 * video memory. With OpenGL 4.4 (or ARB_buffer_storage) it stays mapped,
 * written in place, and its pieces are taken back when the fence of the
 * frame that used them, polled, says that frame is done - nothing waits
 * for one: a texture there is no room for is converted on the CPU, as
 * before. Without, the frame's bytes are written to memory of this side's
 * and go to the buffer (or the texture) in one call when it is flushed.
 *
 * What a frame asks for is recorded at the end of its processing
 * (gl_tex_flush(), from ProcessFrame()), before its draws. The kernels are
 * made when the renderer starts, and never while a game runs: from their
 * SPIR-V with OpenGL 4.6 (ARB_gl_spirv), or compiled from their text, as
 * program binaries kept in the frontend's system directory the next time,
 * where the driver can (glGetProgramBinary), by its name and the text's.
 * OpenGL ES (the GLES builds) and OpenGL 2 convert on the CPU, as before.
 *
 * C89 (gl_texdecoder.c), for C and C++ alike; C++ has it as glTexDecoder. */
#ifndef GLES_GL_TEXDECODER_H
#define GLES_GL_TEXDECODER_H
#include "types.h"
#include "rend/TexCache.h"
#include "rend/texdecode.h"
#include <glsm/glsmsym.h>

#ifdef __cplusplus
extern "C" {
#endif

/* With the context current, the renderer's resources made */
void gltd_init(void);
void gltd_term(void);
bool gltd_ready(void);
/* Whether a texture of @type (TEX_TYPE_*), made into TEXD_OUT8, 16 or 32
 * (@out), can be made by the kernels */
bool gltd_takes(int type, u32 out);
/* Whether a planar 16-bit texture of @type goes to its texture as it is
 * (through the ring) */
bool gltd_native(int type);
/* @bytes of the ring for a texture of @levels levels to make (0 for a
 * copy), and where they are (a multiple of 16); NULL if there is no
 * room. What was asked for before goes to the GPU first if the lists have
 * no room for this. */
u8 *gltd_begin(u32 bytes, u32 levels, u32 *offset);
/* The plan's levels made into @texture (of @type) from the bytes at
 * @offset; what gltd_cancel() takes */
u32 gltd_add_decode(GLuint texture, int type, const struct texd_plan *plan, u32 offset);
/* @width by @height texels into level 0 of @texture, rows of @row_length
 * from @offset */
u32 gltd_add_copy(GLuint texture, int type, u32 offset, u32 row_length, u32 width, u32 height);
u32 gltd_serial(void);
/* What was added under @serial as @job is not done after all */
void gltd_cancel(u32 job, u32 serial);
/* What the frame asked for, recorded */
void gltd_flush(void);

#ifdef __cplusplus
}

/* The decoder as C++ has it */
class GLTexDecoder
{
public:
	void Init() { gltd_init(); }
	void Term() { gltd_term(); }
	bool Ready() const { return gltd_ready(); }
	bool Takes(TextureType type, u32 out) const { return gltd_takes((int)type, out); }
	bool Native(TextureType type) const { return gltd_native((int)type); }
	u8 *Begin(u32 bytes, u32 levels, u32 *offset) { return gltd_begin(bytes, levels, offset); }
	u32 AddDecode(GLuint texture, TextureType type, const texd_plan& plan, u32 offset) { return gltd_add_decode(texture, (int)type, &plan, offset); }
	u32 AddCopy(GLuint texture, TextureType type, u32 offset, u32 rowLength, u32 width, u32 height)
	{
		return gltd_add_copy(texture, (int)type, offset, rowLength, width, height);
	}
	u32 Serial() const { return gltd_serial(); }
	void Cancel(u32 job, u32 serial) { gltd_cancel(job, serial); }
	void Flush() { gltd_flush(); }
};

extern GLTexDecoder glTexDecoder;
#endif

#endif
