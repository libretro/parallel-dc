/* Textures made on the GPU from their bytes in video memory, for the
 * Direct3D 9 renderer: what dx11/dx11_texdecoder.h is for the Direct3D 11
 * ones, with the pixel shader of rend/texdecode_sm3.h - Direct3D 9 has no
 * compute shaders, and shader model 3 no integers.
 *
 * The texture's bytes - the texels, the codebook of a compressed texture,
 * the part of the palette a paletted one uses - are copied as they are
 * into a ring: one dynamic A8R8G8B8 texture, made when the renderer
 * starts, 1024 texels (4 KB) a row, locked once a frame with
 * D3DLOCK_DISCARD at the first texture the frame makes and unlocked when
 * the frame's are drawn. Each level is then one draw, of a triangle that
 * covers it, into the level of the texture - a render target texture of
 * the pool's, in the format the texture is sampled in: R5G6B5, A1R5G5B5,
 * A4R4G4B4, A8R8G8B8 or A8. All of a frame's are drawn before its scene
 * (dx9_texdec_flush()). A frame that asks for more than the ring holds has the rest
 * converted on the CPU, as before; so has a format the card cannot draw
 * into.
 *
 * Nothing is compiled while a game runs: the shaders are bytecode made
 * when the core was built (rend/texdecode_dx9.inc). And not on by
 * default: the GPU makes them only with the core option's "Enabled,
 * Direct3D 9 too" (settings.rend.GpuTextureDecodeD3D9). A planar 16-bit
 * texture needs none of this: its texels go to the texture as they are
 * (dx9_texture.c, tex_gpu_decode()). */
#pragma once
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d9.h>
#include "types.h"
#include "rend/texdecode.h"
#include "rend/TexCache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything made, if the textures are to be made on the GPU; nothing
 * otherwise */
void dx9_texdec_init(void);
void dx9_texdec_term(void);
bool dx9_texdec_ready(void);

/* Whether a texture of @type (TEX_TYPE_*) can be drawn into (the pool
 * has render targets of its format) */
bool dx9_texdec_takes(int type);

/* @bytes of the ring for a texture of @levels levels, and where they
 * are in words; null if there is no room */
u8 *dx9_texdec_begin(u32 bytes, u32 levels, u32 *base);
/* A level of it, drawn into @surface (@type's format), with the six
 * words of rend/texdecode.h */
void dx9_texdec_add(IDirect3DSurface9 *surface, int type, const u32 pc[6]);
u32 dx9_texdec_next(void);
u32 dx9_texdec_serial(void);
/* The @count levels from @job on, added under @serial, are not made
 * after all */
void dx9_texdec_cancel(u32 job, u32 count, u32 serial);

/* What the frame asked for, drawn: before its scene */
void dx9_texdec_flush(void);

#ifdef __cplusplus
}
#endif
