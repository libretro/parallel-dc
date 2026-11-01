#pragma once
#include <string.h>
#include "rend/grow_buf.h"

#define TEXTURE_ID_CACHE_SIZE 32

class GLCache {
public:
	GLCache() { Reset(); }

	void BindTexture(GLenum target,  GLuint texture) {
		if (target == GL_TEXTURE_2D && !_disable_cache) {
			if (texture != _texture) {
				glBindTexture(target, texture);
				_texture = texture;
			}
		}
		else
			glBindTexture(target, texture);
	}

	void BlendFunc(GLenum sfactor, GLenum dfactor) {
		if (sfactor != _src_blend_factor || dfactor != _dst_blend_factor || _disable_cache) {
			_src_blend_factor = sfactor;
			_dst_blend_factor = dfactor;
			glBlendFunc(sfactor, dfactor);
		}
	}

	void ClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
		if (red != _clear_r || green != _clear_g || blue != _clear_b || alpha != _clear_a || _disable_cache) {
			_clear_r = red;
			_clear_g = green;
			_clear_b = blue;
			_clear_a = alpha;
			glClearColor(red, green, blue, alpha);
		}
	}

	void CullFace(GLenum mode) {
		if (mode != _cull_face || _disable_cache) {
			_cull_face = mode;
			glCullFace(mode);
		}
	}

	void DeleteTextures(GLsizei n, const GLuint *textures) {
		for (int i = 0; i < n; i++)
		{
			TextureParameters *params = Params(textures[i]);
			if (params != nullptr)
				memset(params, 0xFF, sizeof(*params));
			if (textures[i] == _texture)
				_texture = 0;
		}
		glDeleteTextures(n, textures);
	}

	void DepthFunc(GLenum func) {
		if (func != _depth_func || _disable_cache) {
			_depth_func = func;
			glDepthFunc(func);
		}
	}

	void DepthMask(GLboolean flag) {
		if (flag != _depth_mask || _disable_cache) {
			_depth_mask = flag;
			glDepthMask(flag);
		}
	}

	void Enable(GLenum cap) {
		switch (cap) {
		case GL_BLEND:
			if (_en_blend == GL_TRUE && !_disable_cache)
				return;
			_en_blend = GL_TRUE;
			glEnable(GL_BLEND);
			break;
		case GL_CULL_FACE:
			if (_en_cull_face == GL_TRUE && !_disable_cache)
				return;
			_en_cull_face = GL_TRUE;
			glEnable(GL_CULL_FACE);
			break;
		case GL_DEPTH_TEST:
			if (_en_depth_test == GL_TRUE && !_disable_cache)
				return;
			_en_depth_test = GL_TRUE;
			glEnable(GL_DEPTH_TEST);
			break;
		case GL_SCISSOR_TEST:
			if (_en_scissor_test == GL_TRUE && !_disable_cache)
				return;
			_en_scissor_test = GL_TRUE;
			glEnable(GL_SCISSOR_TEST);
			break;
		case GL_STENCIL_TEST:
			if (_en_stencil_test == GL_TRUE && !_disable_cache)
				return;
			_en_stencil_test = GL_TRUE;
			glEnable(GL_STENCIL_TEST);
			break;
		}
	}

	void Disable(GLenum cap) {
		switch (cap) {
		case GL_BLEND:
			if (_en_blend == GL_FALSE && !_disable_cache)
				return;
			_en_blend = false;
			glDisable(GL_BLEND);
			break;
		case GL_CULL_FACE:
			if (_en_cull_face == GL_FALSE && !_disable_cache)
				return;
			_en_cull_face = false;
			glDisable(GL_CULL_FACE);
			break;
		case GL_DEPTH_TEST:
			if (_en_depth_test == GL_FALSE && !_disable_cache)
				return;
			_en_depth_test = false;
			glDisable(GL_DEPTH_TEST);
			break;
		case GL_SCISSOR_TEST:
			if (_en_scissor_test == GL_FALSE && !_disable_cache)
				return;
			_en_scissor_test = false;
			glDisable(GL_SCISSOR_TEST);
			break;
		case GL_STENCIL_TEST:
			if (_en_stencil_test == GL_FALSE && !_disable_cache)
				return;
			_en_stencil_test = false;
			glDisable(GL_STENCIL_TEST);
			break;
		}
	}

	void UseProgram(GLuint program) {
		if (program != _program || _disable_cache) {
			_program = program;
			glUseProgram(program);
		}
	}

	void StencilFunc(GLenum func, GLint ref, GLuint mask) {
		if (_stencil_func != func || _stencil_ref != ref || _stencil_fmask != mask || _disable_cache) {
			_stencil_func = func;
			_stencil_ref = ref;
			_stencil_fmask = mask;

			glStencilFunc(func, ref, mask);
		}
	}

	void StencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) {
		if (_stencil_sfail != sfail ||_stencil_dpfail != dpfail || _stencil_dppass != dppass || _disable_cache) {
			_stencil_sfail = sfail;
			_stencil_dpfail = dpfail;
			_stencil_dppass = dppass;

			glStencilOp(sfail, dpfail, dppass);
		}
	}

	void StencilMask(GLuint mask) {
		if (_stencil_mask != mask || _disable_cache) {
			_stencil_mask = mask;
			glStencilMask(mask);
		}
	}

	void Scissor(GLint x, GLint y, GLsizei width, GLsizei height)
	{
		if (x != _scissor.x || y != _scissor.y || width != _scissor.width || height != _scissor.height || _disable_cache)
		{
			_scissor.x = x;
			_scissor.y = y;
			_scissor.width = width;
			_scissor.height = height;
			glScissor(x, y, width, height);
		}
	}

	void TexParameteri(GLenum target,  GLenum pname,  GLint param)
	{
		TextureParameters *cur_params = target == GL_TEXTURE_2D ? Params(_texture) : nullptr;
		if (cur_params != nullptr)
		{
			GLenum *field = nullptr;
			switch (pname) {
			case GL_TEXTURE_MIN_FILTER: field = &cur_params->_min_filter; break;
			case GL_TEXTURE_MAG_FILTER: field = &cur_params->_mag_filter; break;
			case GL_TEXTURE_WRAP_S: field = &cur_params->_wrap_s; break;
			case GL_TEXTURE_WRAP_T: field = &cur_params->_wrap_t; break;
			}
			if (field != nullptr)
			{
				if (*field == (GLenum)param)
					return;
				*field = param;
			}
		}
		glTexParameteri(target, pname, param);
	}

	/* The same for the parameters that are floats (bit for bit) */
	void TexParameterf(GLenum target,  GLenum pname,  GLfloat param)
	{
		TextureParameters *cur_params = target == GL_TEXTURE_2D ? Params(_texture) : nullptr;
		if (cur_params != nullptr)
		{
			GLfloat *field = nullptr;
#ifdef GL_TEXTURE_LOD_BIAS
			if (pname == GL_TEXTURE_LOD_BIAS)
				field = &cur_params->_lod_bias;
#endif
#ifdef GL_TEXTURE_MAX_ANISOTROPY_EXT
			if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT)
				field = &cur_params->_max_anisotropy;
#endif
			if (field != nullptr)
			{
				if (memcmp(field, &param, sizeof(param)) == 0)
					return;
				*field = param;
			}
		}
		glTexParameterf(target, pname, param);
	}

	GLuint GenTexture() {
		if (_texture_cache_size == 0) {
			_texture_cache_size = TEXTURE_ID_CACHE_SIZE;
			glGenTextures(_texture_cache_size,  _texture_ids);
		}
		return _texture_ids[--_texture_cache_size];
	}

	void Reset() {
		_texture = 0xFFFFFFFFu;
		if (_texture_params.data != nullptr)
			memset(_texture_params.data, 0xFF, _texture_params.size);
		_src_blend_factor = 0xFFFFFFFFu;
		_dst_blend_factor = 0xFFFFFFFFu;
		_clear_r = -1.f;
		_clear_g = -1.f;
		_clear_b = -1.f;
		_clear_a = -1.f;
		_en_blend = 0xFF;
		_en_cull_face = 0xFF;
		_en_depth_test = 0xFF;
		_en_scissor_test = 0xFF;
		_en_stencil_test = 0xFF;
		_cull_face = 0xFFFFFFFFu;
		_depth_func = 0xFFFFFFFFu;
		_depth_mask = 0xFF;
		_program = 0xFFFFFFFFu;
		_texture_cache_size = 0;
		_stencil_func = 0xFFFFFFFFu;
		_stencil_ref = -1;
		_stencil_fmask = 0;
		_stencil_sfail = 0xFFFFFFFFu;
		_stencil_dpfail = 0xFFFFFFFFu;
		_stencil_dppass = 0xFFFFFFFFu;
		_stencil_mask = 0;
		_scissor.x = -1;
		_scissor.y = -1;
		_scissor.width = -1;
		_scissor.height = -1;
		if (_texture_cache_size > 0)
		{
			glDeleteTextures(_texture_cache_size, _texture_ids);
			_texture_cache_size = 0;
		}
	}

	void DisableCache()
	{
	   _disable_cache = true;
	   Reset();
	}
	void EnableCache()
	{
		_disable_cache = false;
		Reset();
	}

private:
	/* A texture's parameters as last set: by its name, in a table that
	 * grows (names are small numbers; one past MaxCachedName is not cached).
	 * All bits set is none yet: no enum is that, and the floats are NaN,
	 * which compares with nothing. */
	struct TextureParameters {
		GLenum _min_filter;
		GLenum _mag_filter;
		GLenum _wrap_s;
		GLenum _wrap_t;
		GLfloat _lod_bias;
		GLfloat _max_anisotropy;
	};
	enum { MaxCachedName = 1 << 20 };
	TextureParameters *Params(GLuint texture)
	{
		if (_disable_cache || texture >= MaxCachedName)
			return nullptr;
		const size_t had = _texture_params.size / sizeof(TextureParameters);
		if (texture >= had)
		{
			size_t want = had != 0 ? had : 256;
			while (want <= texture)
				want *= 2;
			if (grow_buf_reserve_keep(&_texture_params, want * sizeof(TextureParameters)) == nullptr)
				return nullptr;
			/* (the buffer may have grown past what was asked: all of it marked) */
			memset((TextureParameters *)_texture_params.data + had, 0xFF,
					_texture_params.size - had * sizeof(TextureParameters));
		}
		return (TextureParameters *)_texture_params.data + texture;
	}
	GLuint _array_buffer;
	GLuint _element_array_buffer;
	GLuint _texture;
	GLenum _src_blend_factor;
	GLenum _dst_blend_factor;
	GLclampf _clear_r;
	GLclampf _clear_g;
	GLclampf _clear_b;
	GLclampf _clear_a;
	GLboolean _en_blend;
	GLboolean _en_cull_face;
	GLboolean _en_depth_test;
	GLboolean _en_scissor_test;
	GLboolean _en_stencil_test;
	GLenum _cull_face;
	GLenum _depth_func;
	GLboolean _depth_mask;
	GLuint _program;
	GLenum _stencil_func;
	GLint _stencil_ref;
	GLuint _stencil_fmask;
	GLenum _stencil_sfail;
	GLenum _stencil_dpfail;
	GLenum _stencil_dppass;
	GLuint _stencil_mask;
	struct {
		GLint x;
		GLint y;
		GLsizei width;
		GLsizei height;
	} _scissor;
	GLuint _texture_ids[TEXTURE_ID_CACHE_SIZE];
	GLuint _texture_cache_size;
	grow_buf_t _texture_params = GROW_BUF_INIT;
	bool _disable_cache;
};

extern GLCache glcache;
