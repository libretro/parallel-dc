/* The per-pixel OpenGL renderer (OpenGL 4.3): C89 */
#ifndef GL4_GL4_H
#define GL4_GL4_H

#include <string.h>
#include "rend/gles/gles.h"
#include "rend/grow_buf.h"
#include "rend/tex_map.h"

#ifdef __cplusplus
extern "C" {
#endif

void gl4DrawStrips(GLuint output_fbo, int width, int height);

/* A frame drawn in horizontal bands (abuffer.c, gl4OitChooseBands()): the
 * band being drawn, in pixels of the framebuffer; no height when the frame
 * is drawn whole. Every scissor rectangle the frame's draws set is cut to
 * it: gl4Scissor() in place of glc_scissor(), gl4ScissorOff() in place of
 * turning the scissor test off. */
typedef struct gl4Band
{
	int y, height;
} gl4Band;
extern gl4Band gl4band;
static INLINE void gl4Scissor(int x, int y, int width, int height)
{
	if (gl4band.height != 0)
	{
		const int top = y > gl4band.y ? y : gl4band.y;
		const int bottom = y + height < gl4band.y + gl4band.height ? y + height : gl4band.y + gl4band.height;
		y = top;
		height = bottom > top ? bottom - top : 0;
	}
	glc_enable(GL_SCISSOR_TEST);
	glc_scissor(x, y, width, height);
}
static INLINE void gl4ScissorOff(void)
{
	if (gl4band.height != 0)
	{
		glc_enable(GL_SCISSOR_TEST);
		glc_scissor(0, gl4band.y, 0x7FFF, gl4band.height);
	}
	else
		glc_disable(GL_SCISSOR_TEST);
}
/* The bands this frame (a render to a texture, or the screen's) is drawn
 * in: from the fragment counts of frames before, which nothing waits for */
u32 gl4OitChooseBands(bool rtt);
/* A band starts: its lists empty, its count at nothing; not before the
 * band before is on the picture */
void gl4OitBandBegin(void);
/* A band's translucent polygons are all drawn: its count copied out */
void gl4OitBandEnd(u32 band);
/* The frame's bands are drawn: a fence for their counts */
void gl4OitFrameEnd(u32 bands, bool rtt);

/* A program's pass */
#define PASS_DEPTH 0
#define PASS_COLOR 1
#define PASS_OIT 2

/* What a program's uniforms that change by the polygon were last set to:
 * set again only when another value is (gl4_uniforms_set()). A uniform is a
 * program's, so every record drawn with the program has its program's
 * (gl4PipelineShader's uniforms); one made anew has none. */
enum
{
	GL4U_ALPHA_TEST, GL4U_FOG_DENSITY, GL4U_FOG_COL_RAM, GL4U_FOG_COL_VERT, GL4U_SHADE_SCALE, GL4U_BLEND_MODE, GL4U_USE_ALPHA,
	GL4U_IGNORE_TEX_ALPHA, GL4U_SHADING_INSTR, GL4U_FOG_CONTROL, GL4U_PP_NUMBER, GL4U_TRILINEAR_ALPHA, GL4U_FOG_CLAMP_MIN,
	GL4U_FOG_CLAMP_MAX, GL4U_NORMAL_MATRIX, GL4U_PALETTE_INDEX, GL4U_KEY_BITS, GL4U_COUNT
};
typedef struct gl4UniformShadow
{
	u32 set;            /* which of them have been set, a bit each */
	u32 words[GL4U_COUNT][16];
} gl4UniformShadow;

/* true, and the words kept, if @count words at @value are not what uniform
 * @which was last set to (bit for bit: -0 is not 0) */
static INLINE bool gl4_uniform_changed(gl4UniformShadow *u, int which, const void *value, u32 count)
{
	if ((u->set & (1u << which)) && memcmp(u->words[which], value, count * 4) == 0)
		return false;
	memcpy(u->words[which], value, count * 4);
	u->set |= 1u << which;
	return true;
}

typedef struct gl4PipelineShader gl4PipelineShader;
struct gl4PipelineShader
{
	GLuint program;
	/* the program's uniforms as last set: its own, or the general program's
	 * whose record it is drawn with (gl4PipelineLocations()); NULL: none */
	gl4UniformShadow *uniforms;
	gl4UniformShadow ownUniforms;

	GLint pp_ClipTest;
	GLint cp_AlphaTestValue;
	GLint sp_FOG_COL_RAM;
	GLint sp_FOG_COL_VERT;
	GLint sp_FOG_DENSITY;
	GLint shade_scale_factor;
	GLint pp_Number;
	GLint blend_mode;
	GLint use_alpha;
	GLint ignore_tex_alpha;
	GLint shading_instr;
	GLint fog_control;
	GLint trilinear_alpha;
	GLint fog_clamp_min, fog_clamp_max;
	GLint normal_matrix;
	GLint palette_index;

	bool cp_AlphaTest;
	bool pp_InsideClipping;
	bool pp_Texture;
	bool pp_UseAlpha;
	bool pp_IgnoreTexA;
	u32 pp_ShadInstr;
	bool pp_Offset;
	u32 pp_FogCtrl;
	int pass;		/* PASS_* */
	bool pp_TwoVolumes;
	bool pp_Gouraud;
	bool pp_BumpMap;
	bool fog_clamping;
	bool palette;

	/* (rend/gles/glprogram_cache.h) The polygon's key (gl4GetProgram());
	 * for one drawn with a general program, that program's record, whose
	 * program and uniforms these are; the program of its own the start
	 * asked for and has not waited for yet (0: none); the uniform a general
	 * program reads the key from (-1: none) */
	u32 key;
	gl4PipelineShader *general;
	GLuint pending;
	GLint key_bits;
};

typedef struct gl4_ctx
{
	struct
	{
		GLuint program;

		GLint normal_matrix;
	} modvol_shader;

	/* by key: room for them made with the renderer (rend/tex_map.h), a key
	 * first drawn with taking none */
	tex_map_t shaders;
	/* The general programs (rend/gles/glprogram_cache.h), by pass and pp_Gouraud */
	gl4PipelineShader general[3][2];

	struct
	{
		GLuint geometry,modvols,idxs,idxs2;
		GLuint main_vao;
		GLuint modvol_vao;
		GLuint tr_poly_params;
	} vbo;
	/* (made with the resources) the translucent polygons' words of a frame
	 * the CPU parsed, made here (see PolyParam in SHADER_HEADER) */
	u32 trPolyMax;                  /* translucent polygons there is room for */
	grow_buf_t trPolyWords;         /* their words where the buffer cannot be mapped */
	/* what the state manager (glsm) has no function for */
	void (APIENTRYP vertexAttribDivisor)(GLuint index, GLuint divisor);
	void (APIENTRYP vertexAttribI4ui)(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
} gl4_ctx;

extern gl4_ctx gl4;

extern int max_image_width;
extern int max_image_height;

GLuint gl4BindRTT(u32 addy, u32 fbw, u32 fbh, u32 channels, u32 fmt);
void gl4DrawFramebuffer(float w, float h);
void abufferDrawQuad(void);

extern const char *gl4PixelPipelineShader;
/* @s's program, of @pixel_source (gl4PixelPipelineShader for a polygon's)
 * and @vertex_source (NULL: the polygons'), made now */
bool gl4CompilePipelineShader(gl4PipelineShader* s, const char *pixel_source, const char *vertex_source);
/* A polygon's program (glprogram_cache.h) asked of the driver: its own, or
 * (@general) the general one of its pass and pp_Gouraud; and its uniforms
 * found, once it is there (the program bound) */
GLuint gl4MakePipelineProgram(const gl4PipelineShader* s, bool general, bool background);
void gl4PipelineLocations(gl4PipelineShader* s, bool general);
/* At the renderer's start: the general programs and those of the keys of earlier runs */
void gl4MakePrograms(void);

extern GLuint stencilTexId;
extern GLuint depthTexId;
extern GLuint opaqueTexId;
extern GLuint depthSaveTexId;
extern GLuint geom_fbo;
extern GLuint texSamplers[2];
extern GLuint depth_fbo;

void gl4DrawVmuTexture(u8 vmu_screen_number);
void gl4DrawGunCrosshair(u8 port);

/* The translucent polygons' lists (abuffer.c): made with the renderer and
 * for the size rendered at, put on the picture, taken down */
void initABuffer(void);
void reshapeABuffer(int w, int h);
void termABuffer(void);
void renderABuffer(void);
void DrawTranslucentModVols(int first, int count, bool useOpaqueGeom);
/* the working framebuffer's textures, @width by @height (gldraw.c) */
void gl4CreateTextures(int width, int height);

#define SHADER_HEADER "#version 430 \n\
\n\
layout(r32ui, binding = 4) uniform coherent restrict uimage2D abufferPointerImg; \n\
struct Pixel { \n\
	uint color;		// RGBA8, as the PowerVR keeps a fragment's colour \n\
	float depth; \n\
	uint seq_num; \n\
	uint next; \n\
}; \n\
#define EOL 0xFFFFFFFFu \n\
layout (binding = 0, std430) coherent restrict buffer PixelBuffer { \n\
	Pixel pixels[]; \n\
}; \n\
layout(binding = 0, offset = 0) uniform atomic_uint buffer_index; \n\
\n\
#define ZERO				0 \n\
#define ONE					1 \n\
#define OTHER_COLOR			2 \n\
#define INVERSE_OTHER_COLOR	3 \n\
#define SRC_ALPHA			4 \n\
#define INVERSE_SRC_ALPHA	5 \n\
#define DST_ALPHA			6 \n\
#define INVERSE_DST_ALPHA	7 \n\
 \n\
uint getNextPixelIndex() \n\
{ \n\
	uint index = atomicCounterIncrement(buffer_index); \n\
	if (index >= pixels.length()) \n\
		// Buffer overflow \n\
		discard; \n\
	 \n\
	return index; \n\
} \n\
\n\
/* A fragment's depth: the position's, which the depth test took (rend/depth_scale.h) */ \n\
#define FRAG_DEPTH gl_FragCoord.z \n\
/* A translucent polygon's words, as the Vulkan per-pixel renderer and \n\
 * GPU-side TA processing have them (rend/ta_gpu.h, TAG_P_WORDS): its TSP \n\
 * word with the ISP word's depth function and depth write bits in 13-15 \n\
 * and 10, whether it has two volumes (its second TSP or TCW word not all \n\
 * ones: TAG_P_TWOVOL) in 1 and whether it takes shadows in 0; and its \n\
 * second volume's TSP word */ \n\
struct PolyParam { \n\
	int tsp_isp_pcw; \n\
	int tsp1; \n\
}; \n\
layout (binding = 1, std430) readonly buffer TrPolyParamBuffer { \n\
	PolyParam tr_poly_params[]; \n\
}; \n\
 \n\
#define GET_TSP_FOR_AREA int tsp; if (area1) tsp = pp.tsp1; else tsp = pp.tsp_isp_pcw; \n\
 \n\
int getSrcBlendFunc(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return (tsp >> 29) & 7; \n\
} \n\
\n\
int getDstBlendFunc(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return (tsp >> 26) & 7; \n\
} \n\
\n\
bool getSrcSelect(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return ((tsp >> 25) & 1) != 0; \n\
} \n\
\n\
bool getDstSelect(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return ((tsp >> 24) & 1) != 0; \n\
} \n\
\n\
int getFogControl(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return (tsp >> 22) & 3; \n\
} \n\
\n\
bool getUseAlpha(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return ((tsp >> 20) & 1) != 0; \n\
} \n\
\n\
bool getIgnoreTexAlpha(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return ((tsp >> 19) & 1) != 0; \n\
} \n\
\n\
int getShadingInstruction(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return (tsp >> 6) & 3; \n\
} \n\
\n\
int getDepthFunc(const PolyParam pp) \n\
{ \n\
	return (pp.tsp_isp_pcw >> 13) & 7; \n\
} \n\
\n\
bool getDepthMask(const PolyParam pp) \n\
{ \n\
	return ((pp.tsp_isp_pcw >> 10) & 1) != 1; \n\
} \n\
\n\
bool getShadowEnable(const PolyParam pp) \n\
{ \n\
	return (pp.tsp_isp_pcw & 1) != 0; \n\
} \n\
\n\
uint getPolyNumber(const Pixel pixel) \n\
{ \n\
	return pixel.seq_num & 0x3FFFFFFFu; \n\
} \n\
\n\
#define SHADOW_STENCIL 0x40000000u \n\
#define SHADOW_ACC	   0x80000000u \n\
\n\
bool isShadowed(const Pixel pixel) \n\
{ \n\
	return (pixel.seq_num & SHADOW_ACC) == SHADOW_ACC; \n\
} \n\
\n\
bool isTwoVolumes(const PolyParam pp) \n\
{ \n\
	return (pp.tsp_isp_pcw & 2) != 0; \n\
} \n\
 \n\
"


/* (the vertex and index buffers: 0, the renderer's own) */
void gl4SetupMainVBO(GLuint geometry, GLuint idxs);
/* (the volumes' triangles' buffer: 0, the renderer's own) */
void gl4SetupModvolVBO(GLuint modvols);
/* A frame decoded on the GPU (pvrrc.gpu, rend/gles/ta_decoder_gl.h): its
 * vertices, indices, modifier volume triangles and translucent polygons'
 * words bound in their places (the vertex arrays' and binding 1), and each
 * translucent strip's number given its fragments by the draw
 * (TA_GPU_INSTANCE); or, @gpu false, those of a frame the CPU parsed -
 * where it was parsed or copied to (gl_frame_vertex_buffer()) - or, @own,
 * the renderer's own buffers, for what is uploaded to them and drawn (the
 * VMU screens, the crosshairs, the framebuffer). Nothing is done when they
 * are bound already; gl4ForgetFrameBuffers(): the decoder's buffers are
 * made again, and are bound the next time. */
void gl4BindFrameBuffers(bool gpu, bool own);
void gl4ForgetFrameBuffers(void);

typedef struct gl4ShaderUniforms_t
{
	float PT_ALPHA;
	float fog_den_float;
	float ps_FOG_COL_RAM[3];
	float ps_FOG_COL_VERT[3];
	int poly_number;
	float trilinear_alpha;
	u32 tsp0;
	u32 tsp1;
	u32 tcw0;
	u32 tcw1;
	float fog_clamp_min[4];
	float fog_clamp_max[4];
	float normal_mat[16];
	float palette_index;

	struct {
		bool enabled;
		int x;
		int y;
		int width;
		int height;
	} base_clipping;
} gl4ShaderUniforms_t;
extern gl4ShaderUniforms_t gl4ShaderUniforms;

/* The uniforms that change by the polygon given to @s, those that changed */
void gl4_uniforms_set(gl4PipelineShader *s);

#ifdef __cplusplus
}
#endif

#endif
