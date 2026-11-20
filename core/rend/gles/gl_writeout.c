/* The chip's write-out for the OpenGL renderers: see gl_writeout.h */
#include "gles.h"
#include "gl_writeout.h"
#include "glprogram_cache.h"
#include "rend/tile/compute_frame.h"

#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#endif
#ifndef GL_FRAMEBUFFER_BARRIER_BIT
#define GL_FRAMEBUFFER_BARRIER_BIT 0x00000400
#endif
#ifndef GL_READ_WRITE
#define GL_READ_WRITE 0x88BA
#endif

#if !defined(HAVE_OPENGLES2) && defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
#ifndef APIENTRY
#define APIENTRY
#endif
/* (what glsm does not wrap: as gl_texdecoder.cpp has it) */
extern void (APIENTRY *__rglgen_glDispatchCompute)(GLuint, GLuint, GLuint);
#define glDispatchCompute __rglgen_glDispatchCompute

/* The pass (rend/tile/pvr_compute.h's PVRC_K_YFILTER, in OpenGL's terms): a
 * thread a column and a remainder of the lines mod m, which walks down
 * its lines in place. OpenGL's first line is the picture's last: the
 * filter is the same either way up, the dither's pattern goes by the
 * picture's line */
static const char *const passHead = "#version 430\n#define PVR_SHADER_GLSL\n#define PVR_LANE_NONE\n";
static const char *const passResolve =
#include "rend/tile/pvr_resolve_text.inc"
;
static const char *const passScaler =
#include "rend/tile/pvr_scaler_text.inc"
;
static const char *const passMain =
	"layout(local_size_x = 64) in;\n"
	"layout(rgba8, binding = 0) uniform image2D picture;\n"
	"uniform uvec4 words;	// the width, the height, the frame's PVRC_F_FLAGS\n"
	"\n"
	"uint wo_load(ivec2 p)\n"
	"{\n"
	"   uvec4 v = uvec4(round(imageLoad(picture, p) * 255.0));\n"
	"   return v.x | (v.y << 8) | (v.z << 16) | (v.w << 24);\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"   uint w = words.x, h = words.y, flags = words.z;\n"
	"   uint m = (flags >> 3) & 31u, ycoeff = flags >> 16, fbw = (flags >> 8) & 63u;\n"
	"   int x = int(gl_GlobalInvocationID.x);\n"
	"   uint y = gl_WorkGroupID.y;\n"
	"   if (m == 0u || uint(x) >= w || y >= h)\n"
	"      return;\n"
	"   uint mid = wo_load(ivec2(x, int(y)));\n"
	"   uint above = mid;\n"
	"   for (; y < h; y += m)\n"
	"   {\n"
	"      uint below = y + m < h ? wo_load(ivec2(x, int(y + m))) : mid;\n"
	"      uint c = pvr_fb_write((flags & 0x4000u) != 0u ? pvr_scaler_y(above, mid, below, ycoeff) : mid, uint(x) / m, (h - 1u - y) / m, fbw);\n"
	"      imageStore(picture, ivec2(x, int(y)), (vec4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float(c >> 24)) + 0.25) / 255.0);\n"
	"      above = mid;\n"
	"      mid = below;\n"
	"   }\n"
	"}\n"
	"\n";
/* (the pass's flags, as pvr_compute.h has them) */
typedef char gl_writeout_flags_check[PVRC_FF_YON == 0x4000u ? 1 : -1];

static GLuint program, texture, fbo;
static GLint wordsAt = -1;
static int texW, texH;
static bool fresh;	/* the picture is new: the frontend's goes into it first */

void gl_writeout_term(void)
{
	if (fbo != 0)
		glDeleteFramebuffers(1, &fbo);
	if (texture != 0)
		glc_delete_textures(1, &texture);
	if (program != 0)
	{
		glc_use_program(0);
		glDeleteProgram(program);
	}
	program = texture = fbo = 0;
	wordsAt = -1;
	texW = texH = 0;
}

void gl_writeout_init(int width, int height, bool picture)
{
	bool complete;
	const bool wanted = !compute_frame_scalers_off && !gl.is_gles && glDispatchCompute != NULL
			&& (gl.gl_major > 4 || (gl.gl_major == 4 && gl.gl_minor >= 3)) && width > 0 && height > 0;
	if (!wanted)
	{
		gl_writeout_term();
		return;
	}
	if (program == 0)
	{
		const char *pieces[] = { passHead, passResolve, passScaler, passMain };
		program = glprog_make_compute(pieces, 4);
		if (program == 0)
		{
			WARN_LOG(RENDERER, "OpenGL: the PowerVR2 write-out pass could not be made: the picture goes without it\n");
			return;
		}
		wordsAt = glGetUniformLocation(program, "words");
	}
	if (!picture)
		return;
	if (texture == 0 || texW != width || texH != height)
	{
		if (fbo != 0)
			glDeleteFramebuffers(1, &fbo);
		if (texture != 0)
			glc_delete_textures(1, &texture);
		texture = glc_gen_texture();
		glc_bind_texture(GL_TEXTURE_2D, texture);
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
		glc_bind_texture(GL_TEXTURE_2D, 0);
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
		complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		glBindFramebuffer(GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
		texW = width;
		texH = height;
		fresh = true;
		if (!complete)
		{
			WARN_LOG(RENDERER, "OpenGL: no picture for the PowerVR2 write-out pass: the picture goes without it\n");
			if (fbo != 0)
				glDeleteFramebuffers(1, &fbo);
			glc_delete_textures(1, &texture);
			texture = fbo = 0;
			texW = texH = 0;
		}
	}
}

bool gl_writeout_ready(void)
{
	return program != 0;
}

void gl_writeout_pass(u32 picture, int width, int height)
{
	u32 flags;
	if (program == 0 || picture == 0)
		return;
	flags = compute_frame_write_out((u32)height);
	if (PVRC_FF_YLINES(flags) == 0)
		return;
	glc_use_program(program);
	glUniform4ui(wordsAt, (GLuint)width, (GLuint)height, flags, 0);
	glBindImageTexture(0, picture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
	glDispatchCompute(((u32)width + 63) / 64, PVRC_FF_YLINES(flags), 1);
	glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
}

u32 gl_writeout_target(u32 frontend, int width, int height)
{
	if (program == 0 || texture == 0 || width != texW || height != texH)
		return 0;
	if (fresh)
	{
		/* (a game that draws part of the screen has the rest from the picture there was) */
		glc_disable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, frontend);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
		glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		fresh = false;
	}
	return fbo;
}

void gl_writeout_present(u32 frontend, int width, int height, bool pass)
{
	if (pass)
		gl_writeout_pass(texture, width, height);
	glc_disable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frontend);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, frontend);
}
#else
void gl_writeout_init(int width, int height, bool picture) { (void)width; (void)height; (void)picture; }
void gl_writeout_term(void) {}
bool gl_writeout_ready(void) { return false; }
void gl_writeout_pass(u32 picture, int width, int height) { (void)picture; (void)width; (void)height; }
u32 gl_writeout_target(u32 frontend, int width, int height) { (void)frontend; (void)width; (void)height; return 0; }
void gl_writeout_present(u32 frontend, int width, int height, bool pass) { (void)frontend; (void)width; (void)height; (void)pass; }
#endif
