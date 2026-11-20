#ifndef GLES_GLCACHE_H
#define GLES_GLCACHE_H
/* The OpenGL state the renderers set, as last set, so that what is set
 * again to what it is costs no call (glcache.c). C89, for C and C++
 * alike; C++ has it as glcache, a GLCache. Render thread only. The
 * OpenGL headers (glsm) come first. */
#include <string.h>
#include <retro_inline.h>
#include "types.h"
#include "rend/grow_buf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEXTURE_ID_CACHE_SIZE 32

/* A texture's parameters as last set: by its name, in a table that grows
 * (names are small numbers; one past GLC_MAX_CACHED_NAME is not cached).
 * All bits set is none yet: no enum is that, and the floats are NaN,
 * which compares with nothing. */
typedef struct glc_texture_params
{
	GLenum min_filter;
	GLenum mag_filter;
	GLenum wrap_s;
	GLenum wrap_t;
	GLfloat lod_bias;
	GLfloat max_anisotropy;
} glc_texture_params;
#define GLC_MAX_CACHED_NAME (1 << 20)

typedef struct gl_cache
{
	GLuint texture;
	GLenum src_blend_factor;
	GLenum dst_blend_factor;
	GLclampf clear_r, clear_g, clear_b, clear_a;
	GLboolean en_blend;
	GLboolean en_cull_face;
	GLboolean en_depth_test;
	GLboolean en_scissor_test;
	GLboolean en_stencil_test;
	GLenum cull_face;
	GLenum depth_func;
	GLboolean depth_mask;
	GLuint program;
	GLenum stencil_func;
	GLint stencil_ref;
	GLuint stencil_fmask;
	GLenum stencil_sfail;
	GLenum stencil_dpfail;
	GLenum stencil_dppass;
	GLuint stencil_mask;
	GLint scissor_x, scissor_y;
	GLsizei scissor_w, scissor_h;
	GLuint texture_ids[TEXTURE_ID_CACHE_SIZE];
	GLuint texture_cache_size;
	grow_buf_t texture_params;
	bool disable_cache;
} gl_cache_t;

extern gl_cache_t glc;

/* Everything forgotten: set again the next time */
void glc_reset(void);
void glc_disable_cache(void);
void glc_enable_cache(void);
/* A texture's parameters as last set; NULL if not cached */
glc_texture_params *glc_params(GLuint texture);
void glc_delete_textures(GLsizei n, const GLuint *textures);
/* A texture name, from a few asked for at once */
GLuint glc_gen_texture(void);
void glc_tex_parameteri(GLenum target, GLenum pname, GLint param);
/* The same for the parameters that are floats (bit for bit) */
void glc_tex_parameterf(GLenum target, GLenum pname, GLfloat param);

static INLINE void glc_bind_texture(GLenum target, GLuint texture)
{
	if (target == GL_TEXTURE_2D && !glc.disable_cache)
	{
		if (texture != glc.texture)
		{
			glBindTexture(target, texture);
			glc.texture = texture;
		}
	}
	else
		glBindTexture(target, texture);
}

static INLINE void glc_blend_func(GLenum sfactor, GLenum dfactor)
{
	if (sfactor != glc.src_blend_factor || dfactor != glc.dst_blend_factor || glc.disable_cache)
	{
		glc.src_blend_factor = sfactor;
		glc.dst_blend_factor = dfactor;
		glBlendFunc(sfactor, dfactor);
	}
}

static INLINE void glc_clear_color(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
	if (red != glc.clear_r || green != glc.clear_g || blue != glc.clear_b || alpha != glc.clear_a || glc.disable_cache)
	{
		glc.clear_r = red;
		glc.clear_g = green;
		glc.clear_b = blue;
		glc.clear_a = alpha;
		glClearColor(red, green, blue, alpha);
	}
}

static INLINE void glc_cull_face(GLenum mode)
{
	if (mode != glc.cull_face || glc.disable_cache)
	{
		glc.cull_face = mode;
		glCullFace(mode);
	}
}

static INLINE void glc_depth_func(GLenum func)
{
	if (func != glc.depth_func || glc.disable_cache)
	{
		glc.depth_func = func;
		glDepthFunc(func);
	}
}

static INLINE void glc_depth_mask(GLboolean flag)
{
	if (flag != glc.depth_mask || glc.disable_cache)
	{
		glc.depth_mask = flag;
		glDepthMask(flag);
	}
}

/* (glsm has glEnable() and glDisable() of a capability's name, not of a
 * value: one each, of the five kept) */
#define GLC_SWITCH(on_off, call) \
	switch (cap) \
	{ \
	case GL_BLEND: \
		if (glc.en_blend == on_off && !glc.disable_cache) return; \
		glc.en_blend = on_off; call(GL_BLEND); break; \
	case GL_CULL_FACE: \
		if (glc.en_cull_face == on_off && !glc.disable_cache) return; \
		glc.en_cull_face = on_off; call(GL_CULL_FACE); break; \
	case GL_DEPTH_TEST: \
		if (glc.en_depth_test == on_off && !glc.disable_cache) return; \
		glc.en_depth_test = on_off; call(GL_DEPTH_TEST); break; \
	case GL_SCISSOR_TEST: \
		if (glc.en_scissor_test == on_off && !glc.disable_cache) return; \
		glc.en_scissor_test = on_off; call(GL_SCISSOR_TEST); break; \
	case GL_STENCIL_TEST: \
		if (glc.en_stencil_test == on_off && !glc.disable_cache) return; \
		glc.en_stencil_test = on_off; call(GL_STENCIL_TEST); break; \
	}

static INLINE void glc_enable(GLenum cap)
{
	GLC_SWITCH(GL_TRUE, glEnable)
}

static INLINE void glc_disable(GLenum cap)
{
	GLC_SWITCH(GL_FALSE, glDisable)
}

static INLINE void glc_use_program(GLuint program)
{
	if (program != glc.program || glc.disable_cache)
	{
		glc.program = program;
		glUseProgram(program);
	}
}

static INLINE void glc_stencil_func(GLenum func, GLint ref, GLuint mask)
{
	if (glc.stencil_func != func || glc.stencil_ref != ref || glc.stencil_fmask != mask || glc.disable_cache)
	{
		glc.stencil_func = func;
		glc.stencil_ref = ref;
		glc.stencil_fmask = mask;
		glStencilFunc(func, ref, mask);
	}
}

static INLINE void glc_stencil_op(GLenum sfail, GLenum dpfail, GLenum dppass)
{
	if (glc.stencil_sfail != sfail || glc.stencil_dpfail != dpfail || glc.stencil_dppass != dppass || glc.disable_cache)
	{
		glc.stencil_sfail = sfail;
		glc.stencil_dpfail = dpfail;
		glc.stencil_dppass = dppass;
		glStencilOp(sfail, dpfail, dppass);
	}
}

static INLINE void glc_stencil_mask(GLuint mask)
{
	if (glc.stencil_mask != mask || glc.disable_cache)
	{
		glc.stencil_mask = mask;
		glStencilMask(mask);
	}
}

static INLINE void glc_scissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	if (x != glc.scissor_x || y != glc.scissor_y || width != glc.scissor_w || height != glc.scissor_h || glc.disable_cache)
	{
		glc.scissor_x = x;
		glc.scissor_y = y;
		glc.scissor_w = width;
		glc.scissor_h = height;
		glScissor(x, y, width, height);
	}
}

#ifdef __cplusplus
}

/* The cache as C++ has it: glcache.BindTexture() and the rest */
class GLCache
{
public:
	void BindTexture(GLenum target, GLuint texture) { glc_bind_texture(target, texture); }
	void BlendFunc(GLenum sfactor, GLenum dfactor) { glc_blend_func(sfactor, dfactor); }
	void ClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) { glc_clear_color(red, green, blue, alpha); }
	void CullFace(GLenum mode) { glc_cull_face(mode); }
	void DeleteTextures(GLsizei n, const GLuint *textures) { glc_delete_textures(n, textures); }
	void DepthFunc(GLenum func) { glc_depth_func(func); }
	void DepthMask(GLboolean flag) { glc_depth_mask(flag); }
	void Enable(GLenum cap) { glc_enable(cap); }
	void Disable(GLenum cap) { glc_disable(cap); }
	void UseProgram(GLuint program) { glc_use_program(program); }
	void StencilFunc(GLenum func, GLint ref, GLuint mask) { glc_stencil_func(func, ref, mask); }
	void StencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) { glc_stencil_op(sfail, dpfail, dppass); }
	void StencilMask(GLuint mask) { glc_stencil_mask(mask); }
	void Scissor(GLint x, GLint y, GLsizei width, GLsizei height) { glc_scissor(x, y, width, height); }
	void TexParameteri(GLenum target, GLenum pname, GLint param) { glc_tex_parameteri(target, pname, param); }
	void TexParameterf(GLenum target, GLenum pname, GLfloat param) { glc_tex_parameterf(target, pname, param); }
	GLuint GenTexture() { return glc_gen_texture(); }
	void Reset() { glc_reset(); }
	void DisableCache() { glc_disable_cache(); }
	void EnableCache() { glc_enable_cache(); }
};

extern GLCache glcache;
#endif

#endif
