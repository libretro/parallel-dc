/* The OpenGL state cache: glcache.h */
#include <glsm/glsm.h>
#include <glsm/glsmsym.h>
#include "glcache.h"

/* (as the C++ class's constructor left it: Reset()) */
gl_cache_t glc = {
	0xFFFFFFFFu,			/* texture */
	0xFFFFFFFFu, 0xFFFFFFFFu,	/* blend factors */
	-1.f, -1.f, -1.f, -1.f,		/* clear colour */
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF,	/* enables */
	0xFFFFFFFFu, 0xFFFFFFFFu,	/* cull face, depth func */
	0xFF,				/* depth mask */
	0xFFFFFFFFu,			/* program */
	0xFFFFFFFFu, -1, 0,		/* stencil func, ref, mask */
	0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,	/* stencil ops */
	0,				/* stencil mask */
	-1, -1, -1, -1			/* scissor */
};

void glc_reset(void)
{
	glc.texture = 0xFFFFFFFFu;
	if (glc.texture_params.data != NULL)
		memset(glc.texture_params.data, 0xFF, glc.texture_params.size);
	glc.src_blend_factor = 0xFFFFFFFFu;
	glc.dst_blend_factor = 0xFFFFFFFFu;
	glc.clear_r = -1.f;
	glc.clear_g = -1.f;
	glc.clear_b = -1.f;
	glc.clear_a = -1.f;
	glc.en_blend = 0xFF;
	glc.en_cull_face = 0xFF;
	glc.en_depth_test = 0xFF;
	glc.en_scissor_test = 0xFF;
	glc.en_stencil_test = 0xFF;
	glc.cull_face = 0xFFFFFFFFu;
	glc.depth_func = 0xFFFFFFFFu;
	glc.depth_mask = 0xFF;
	glc.program = 0xFFFFFFFFu;
	glc.texture_cache_size = 0;
	glc.stencil_func = 0xFFFFFFFFu;
	glc.stencil_ref = -1;
	glc.stencil_fmask = 0;
	glc.stencil_sfail = 0xFFFFFFFFu;
	glc.stencil_dpfail = 0xFFFFFFFFu;
	glc.stencil_dppass = 0xFFFFFFFFu;
	glc.stencil_mask = 0;
	glc.scissor_x = -1;
	glc.scissor_y = -1;
	glc.scissor_w = -1;
	glc.scissor_h = -1;
	if (glc.texture_cache_size > 0)
	{
		glDeleteTextures(glc.texture_cache_size, glc.texture_ids);
		glc.texture_cache_size = 0;
	}
}

void glc_disable_cache(void)
{
	glc.disable_cache = true;
	glc_reset();
}

void glc_enable_cache(void)
{
	glc.disable_cache = false;
	glc_reset();
}

glc_texture_params *glc_params(GLuint texture)
{
	size_t had;
	if (glc.disable_cache || texture >= GLC_MAX_CACHED_NAME)
		return NULL;
	had = glc.texture_params.size / sizeof(glc_texture_params);
	if (texture >= had)
	{
		size_t want = had != 0 ? had : 256;
		while (want <= texture)
			want *= 2;
		if (grow_buf_reserve_keep(&glc.texture_params, want * sizeof(glc_texture_params)) == NULL)
			return NULL;
		/* (the buffer may have grown past what was asked: all of it marked) */
		memset((glc_texture_params *)glc.texture_params.data + had, 0xFF,
				glc.texture_params.size - had * sizeof(glc_texture_params));
	}
	return (glc_texture_params *)glc.texture_params.data + texture;
}

void glc_delete_textures(GLsizei n, const GLuint *textures)
{
	GLsizei i;
	for (i = 0; i < n; i++)
	{
		glc_texture_params *params = glc_params(textures[i]);
		if (params != NULL)
			memset(params, 0xFF, sizeof(*params));
		if (textures[i] == glc.texture)
			glc.texture = 0;
	}
	glDeleteTextures(n, textures);
}

GLuint glc_gen_texture(void)
{
	if (glc.texture_cache_size == 0)
	{
		glc.texture_cache_size = TEXTURE_ID_CACHE_SIZE;
		glGenTextures(glc.texture_cache_size, glc.texture_ids);
	}
	return glc.texture_ids[--glc.texture_cache_size];
}

void glc_tex_parameteri(GLenum target, GLenum pname, GLint param)
{
	glc_texture_params *cur = target == GL_TEXTURE_2D ? glc_params(glc.texture) : NULL;
	if (cur != NULL)
	{
		GLenum *field = NULL;
		switch (pname)
		{
		case GL_TEXTURE_MIN_FILTER: field = &cur->min_filter; break;
		case GL_TEXTURE_MAG_FILTER: field = &cur->mag_filter; break;
		case GL_TEXTURE_WRAP_S: field = &cur->wrap_s; break;
		case GL_TEXTURE_WRAP_T: field = &cur->wrap_t; break;
		}
		if (field != NULL)
		{
			if (*field == (GLenum)param)
				return;
			*field = param;
		}
	}
	glTexParameteri(target, pname, param);
}

void glc_tex_parameterf(GLenum target, GLenum pname, GLfloat param)
{
	glc_texture_params *cur = target == GL_TEXTURE_2D ? glc_params(glc.texture) : NULL;
	if (cur != NULL)
	{
		GLfloat *field = NULL;
#ifdef GL_TEXTURE_LOD_BIAS
		if (pname == GL_TEXTURE_LOD_BIAS)
			field = &cur->lod_bias;
#endif
#ifdef GL_TEXTURE_MAX_ANISOTROPY_EXT
		if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT)
			field = &cur->max_anisotropy;
#endif
		if (field != NULL)
		{
			if (memcmp(field, &param, sizeof(param)) == 0)
				return;
			*field = param;
		}
	}
	glTexParameterf(target, pname, param);
}
