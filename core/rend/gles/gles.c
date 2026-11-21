#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
#include <math.h>
#include "rend/rtt_watch.h"
#include <string.h>

#include <libretro.h>

#include "gles.h"
#include "rend/grow_buf.h"
#include "ta_decoder_gl.h"
#include "glprogram_cache.h"

#ifndef GL_RED
#define GL_RED                            0x1903
#endif
#ifndef GL_R8
#define GL_R8                             0x8229
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION                  0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION                  0x821C
#endif

gl_ctx gl;

struct ShaderUniforms_t ShaderUniforms;

u32 gcflip;


/* Fragment and vertex shaders code */

static const char* VertexShaderSource = "%s\n"
"#define TARGET_GL %s\n"
"#define pp_Gouraud %d\n"
"#define DEPTH_UBO %d\n"
"\n"
"#define GLES2 0\n"
"#define GLES3 1\n"
"#define GL2 2\n"
"#define GL3 3\n"
"\n"
"// (uniform buffers: OpenGL 3.1's, an extension to GLSL 1.30; see depth_scale below)\n"
"#if DEPTH_UBO == 1 && TARGET_GL == GL3\n"
"#extension GL_ARB_uniform_buffer_object : enable\n"
"#endif\n"
"#if TARGET_GL == GL2\n"
"#define highp\n"
"#define lowp\n"
"#define mediump\n"
"#endif\n"
"#if TARGET_GL == GLES2 || TARGET_GL == GL2\n"
"#define in attribute\n"
"#define out varying\n"
"#endif\n"
"\n"
"\n"
"#if TARGET_GL == GL3 || TARGET_GL == GLES3\n"
"#if pp_Gouraud == 0\n"
"#define INTERPOLATION flat\n"
"#else\n"
"#define INTERPOLATION smooth\n"
"#endif\n"
"#else\n"
"#define INTERPOLATION\n"
"#endif\n"
"\n"
"/* The depth is the position's (rend/depth_scale.h): the vertex goes out at\n"
"   (x*w, y*w, a*S + b*w, w) with w = 1/z, a depth of a*S*z + b, 1/w as the\n"
"   chip has it, linear in screen space. depth_scale is (a, b, -, S): S is\n"
"   1/fZ_max, which a frame decoded on the GPU has only on the GPU, so where\n"
"   the context has uniform buffers it is one the renderer copies into\n"
"   (DEPTH_UBO; gl_ctx::depth_ubo). The same text places the vertices of\n"
"   every program, and the position is invariant: a pass that tests EQUAL\n"
"   against another's depth is exact. */\n"
"invariant gl_Position;\n"
"#if DEPTH_UBO == 1\n"
"layout(std140) uniform DepthScaleBlock { highp vec4 depth_scale; };\n"
"#else\n"
"uniform highp vec4      depth_scale;\n"
"#endif\n"
"/* Vertex constants*/ \n"
"uniform highp mat4 normal_matrix;\n"
"uniform highp float sp_FOG_DENSITY;\n"
"\n"
"/* Vertex input */\n"
"in highp vec4    in_pos;\n"
"in mediump vec4  in_base;\n"
"in mediump vec4  in_offs;\n"
"in highp vec2    in_uv;\n"
"/* output */\n"
"INTERPOLATION out mediump vec4 vtx_base;\n"
"INTERPOLATION out mediump vec4 vtx_offs;\n"
"              out highp vec2 vtx_uv;\n"
"#if TARGET_GL == GLES2\n"
"              out highp float fog_depth;\n"
"#endif \n"
"\n"
"void main()\n"
"{\n"
"	vtx_base = in_base;\n"
"	vtx_offs = in_offs;\n"
"	vtx_uv = in_uv;\n"
"	highp vec4 vpos = normal_matrix * in_pos;\n"
"	vpos.w = 1.0 / vpos.z;\n"
"#if TARGET_GL == GLES2\n"
"   // Interpolated with perspective correction, 1/depth comes out as one over\n"
"   // the depth at the pixel, which is what fog goes by. depth itself does not:\n"
"   // it came out too large inside any triangle that spans a range of depths.\n"
"   fog_depth = vpos.w;\n"
"#endif\n"
"	vpos.xy *= vpos.w;\n"
"	// (a z beyond fZ_max is at the nearest depth, not clipped)\n"
"	vpos.z = min(depth_scale.x * depth_scale.w + depth_scale.y * vpos.w, vpos.w);\n"
"	gl_Position = vpos;\n"
"}\n";

const char* PixelPipelineShader =
"%s\n"
"#define TARGET_GL %s\n"
"\n"
"#define cp_AlphaTest %d\n"
"#define pp_ClipInside %d\n"
"#define pp_UseAlpha %d\n"
"#define pp_Texture %d\n"
"#define pp_IgnoreTexA %d\n"
"#define pp_ShadInstr %d\n"
"#define pp_Offset %d\n"
"#define pp_FogCtrl %d\n"
"#define pp_Gouraud %d\n"
"#define pp_BumpMap %d\n"
"#define FogClamping %d\n"
"#define pp_TriLinear %d\n"
"#define pp_Palette %d\n"
"#define pp_Shadowed %d\n"
"\n"
"#define PI 3.1415926\n"
"\n"
"\n"
"#define GLES2 0\n"
"#define GLES3 1\n"
"#define GL2 2\n"
"#define GL3 3\n"
"\n"
"\n"
"#if TARGET_GL == GL2\n"
"#define highp\n"
"#define lowp\n"
"#define mediump\n"
"#endif\n"
"/* The general program (glprogram_cache.h): one for each pp_Gouraud, the\n"
" * other bits of a polygon's key (GetProgram()) read from a uniform. The\n"
" * code below is the same for both, so what one draws the other draws.\n"
" * OpenGL ES 2 and OpenGL 2 have no integer bit operations: there the key\n"
" * comes as its four nibbles, each a float a mediump one holds exactly,\n"
" * and a field is taken out with floor() and mod(). */\n"
"#ifdef GENERAL_PROGRAM\n"
"#undef cp_AlphaTest\n"
"#undef pp_ClipInside\n"
"#undef pp_UseAlpha\n"
"#undef pp_Texture\n"
"#undef pp_IgnoreTexA\n"
"#undef pp_ShadInstr\n"
"#undef pp_Offset\n"
"#undef pp_FogCtrl\n"
"#undef pp_BumpMap\n"
"#undef FogClamping\n"
"#undef pp_TriLinear\n"
"#undef pp_Palette\n"
"#undef pp_Shadowed\n"
"#if TARGET_GL == GLES2 || TARGET_GL == GL2\n"
"uniform mediump vec4 key_nibbles;\n"
"#define KEY_FIELD(n, shift, values) int(mod(floor(key_nibbles.n / shift), values))\n"
"#define pp_ClipInside KEY_FIELD(w, 8.0, 2.0)\n"
"#define cp_AlphaTest KEY_FIELD(w, 4.0, 2.0)\n"
"#define pp_Texture KEY_FIELD(w, 2.0, 2.0)\n"
"#define pp_UseAlpha KEY_FIELD(w, 1.0, 2.0)\n"
"#define pp_IgnoreTexA KEY_FIELD(z, 8.0, 2.0)\n"
"#define pp_ShadInstr KEY_FIELD(z, 2.0, 4.0)\n"
"#define pp_Offset KEY_FIELD(z, 1.0, 2.0)\n"
"#define pp_FogCtrl KEY_FIELD(y, 4.0, 4.0)\n"
"#define pp_BumpMap KEY_FIELD(y, 1.0, 2.0)\n"
"#define FogClamping KEY_FIELD(x, 8.0, 2.0)\n"
"#define pp_TriLinear KEY_FIELD(x, 4.0, 2.0)\n"
"#define pp_Palette KEY_FIELD(x, 2.0, 2.0)\n"
"#define pp_Shadowed KEY_FIELD(x, 1.0, 2.0)\n"
"#else\n"
"uniform highp int key_bits;\n"
"#define pp_ClipInside ((key_bits >> 15) & 1)\n"
"#define cp_AlphaTest ((key_bits >> 14) & 1)\n"
"#define pp_Texture ((key_bits >> 13) & 1)\n"
"#define pp_UseAlpha ((key_bits >> 12) & 1)\n"
"#define pp_IgnoreTexA ((key_bits >> 11) & 1)\n"
"#define pp_ShadInstr ((key_bits >> 9) & 3)\n"
"#define pp_Offset ((key_bits >> 8) & 1)\n"
"#define pp_FogCtrl ((key_bits >> 6) & 3)\n"
"#define pp_BumpMap ((key_bits >> 4) & 1)\n"
"#define FogClamping ((key_bits >> 3) & 1)\n"
"#define pp_TriLinear ((key_bits >> 2) & 1)\n"
"#define pp_Palette ((key_bits >> 1) & 1)\n"
"#define pp_Shadowed (key_bits & 1)\n"
"#endif\n"
"#endif\n"
"#if TARGET_GL == GLES3\n"
"out highp vec4 FragColor;\n"
"#define gl_FragColor FragColor\n"
"#define FOG_CHANNEL r\n"
"#elif TARGET_GL == GL3\n"
"out highp vec4 FragColor;\n"
"#define gl_FragColor FragColor\n"
"#define FOG_CHANNEL r\n"
"#else\n"
"#define in varying\n"
"#define texture texture2D\n"
"#define FOG_CHANNEL a\n"
"#endif\n"
"\n"
"#if TARGET_GL == GL3 || TARGET_GL == GLES3\n"
"#if pp_Gouraud == 0\n"
"#define INTERPOLATION flat\n"
"#else\n"
"#define INTERPOLATION smooth\n"
"#endif\n"
"#else\n"
"#define INTERPOLATION\n"
"#endif\n"
"\n"
"/* Shader program params*/\n"
"/* gles has no alpha test stage, so its emulated on the shader */\n"
"uniform highp float cp_AlphaTestValue;\n"
"uniform highp vec4 pp_ClipTest;\n"
"uniform mediump vec3 sp_FOG_COL_RAM,sp_FOG_COL_VERT;\n"
"uniform highp float sp_FOG_DENSITY;\n"
"uniform mediump sampler2D tex,fog_table;\n"
"uniform lowp float trilinear_alpha;\n"
"uniform mediump vec4 fog_clamp_min;\n"
"uniform mediump vec4 fog_clamp_max;\n"
"uniform mediump sampler2D palette;\n"
"uniform mediump int palette_index;\n"
"#if TARGET_GL != GLES2\n"
"// The depth prepass of sorted translucent polygons (rend/sorter.h): its least alpha, 0 for the colours\n"
"uniform highp float prepass_alpha;\n"
"#endif\n"
"uniform lowp float shade_scale_factor;\n"
"\n"
"/* Precision. The colours that come in are eight bits each, so many 255ths.\n"
" * lowp need not hold those: it may be a number of 256ths and nothing finer,\n"
" * and no 255th but 0 and 1 is one of them - a colour, a texel, the fog\n"
" * colour or a clamp limit kept in lowp can be half a step out before\n"
" * anything is done with it, and comes out a step out about as often as\n"
" * not. Nor need lowp reach past 2, and a texel times 255, which is how the\n"
" * fog table and a palette index are read, is done in the precision of the\n"
" * texel: a sampler is lowp unless it says otherwise. So colours, samplers\n"
" * and what is worked out from them (texture times colour, plus offset, into\n"
" * fog) are mediump, which has ten bits at the least. (Only what is a number\n"
" * of quarters or of 256ths as it comes stays lowp.) Texture coordinates are\n"
" * highp: in mediump, which may be a 16-bit float, a coordinate that repeats\n"
" * a large texture a few times no longer says which texel. The clip\n"
" * rectangle is in pixels and was lowp, which need not reach past 2. */\n"
"/* Vertex input*/\n"
"INTERPOLATION in mediump vec4 vtx_base;\n"
"INTERPOLATION in mediump vec4 vtx_offs;\n"
"in highp vec2 vtx_uv;\n"
"#if TARGET_GL == GLES2\n"
"in highp float fog_depth;\n"
"#endif\n"
"\n"
"// The fog coefficient for a depth, worked out the way the PowerVR2 does it.\n"
"// The depth times the fog density, between 1 and just under 256, is taken\n"
"// apart as a float: its exponent and the top four bits of its mantissa\n"
"// pick one of the 128 table entries, and the next eight bits of mantissa\n"
"// blend that entry with the following one. The blend is done in whole\n"
"// numbers out of 256 and so is the result, which is returned over 256.\n"
"// Splitting the float is done by comparing and halving, which is exact\n"
"// and costs less than the log2() and pow() this used to take.\n"
"highp float fog_mode2(highp float w)\n"
"{\n"
"#if TARGET_GL == GLES2\n"
"	// fog_depth is 1/depth, the one thing a varying carries across a triangle exactly\n"
"	highp float z = sp_FOG_DENSITY / fog_depth;\n"
"#else\n"
"	highp float z = w * sp_FOG_DENSITY;\n"
"#endif\n"
"	// Not clamp(): where the depth is infinite and the density zero their\n"
"	// product is not a number, and that has to come out as 1 like anything\n"
"	// else below 1. The texture lookup this used to end in swallowed it.\n"
"	if (!(z >= 1.0))\n"
"		z = 1.0;\n"
"	if (z > 255.9999)\n"
"		z = 255.9999;\n"
"	highp float e = 0.0;\n"
"	if (z >= 16.0) { z *= 0.0625; e = 4.0; }\n"
"	if (z >= 4.0) { z *= 0.25; e += 2.0; }\n"
"	if (z >= 2.0) { z *= 0.5; e += 1.0; }\n"
"	highp float t = (z - 1.0) * 16.0;\n"
"	highp float i = floor(t);\n"
"	highp float blend = floor((t - i) * 256.0);\n"
"	highp float u = (e * 16.0 + i + 0.5) / 128.0;\n"
"	// The table is two rows: the entry's own value under 0.75, the next entry's under 0.25\n"
"	highp float next = floor(texture(fog_table, vec2(u, 0.25)).FOG_CHANNEL * 255.0 + 0.5);\n"
"	highp float cur = floor(texture(fog_table, vec2(u, 0.75)).FOG_CHANNEL * 255.0 + 0.5);\n"
"	return floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;\n"
"}\n"
"\n"
"highp vec4 fog_clamp(highp vec4 col)\n"
"{\n"
"	if (FogClamping == 1)\n"
"		return clamp(col, fog_clamp_min, fog_clamp_max);\n"
"	return col;\n"
"}\n"
"\n"
"// The palette is a row of 1024 colours, and a colour is read at the middle\n"
"// of its texel. (It used to be read at index/1023: the first few a\n"
"// thousandth of a texel from the one before, the last at the very end of\n"
"// the row.)\n"
"mediump vec4 palettePixel(highp vec2 coords)\n"
"{\n"
"	highp float index = texture(tex, coords).FOG_CHANNEL;\n"
"	highp vec2 c = vec2((index * 255.0 + float(palette_index) + 0.5) / 1024.0, 0.5);\n"
"	return texture(palette, c);\n"
"}\n"
"\n"
"void main()\n"
"{\n"
"	// Clip inside the box\n"
"	if (pp_ClipInside == 1)\n"
"	{\n"
"		if (gl_FragCoord.x >= pp_ClipTest.x && gl_FragCoord.x <= pp_ClipTest.z\n"
"				&& gl_FragCoord.y >= pp_ClipTest.y && gl_FragCoord.y <= pp_ClipTest.w)\n"
"			discard;\n"
"	}\n"
"	\n"
"	mediump vec4 color=vtx_base;\n"
"	if (pp_UseAlpha==0)\n"
"		color.a=1.0;\n"
"	mediump vec4 offset = vtx_offs;\n"
"	if (pp_Shadowed == 1)\n"
"	{\n"
"		// A pixel inside a modifier volume: what the polygon is shaded with,\n"
"		// its base and offset colours, is scaled before the texture is\n"
"		// combined with them. The texture itself is not.\n"
"		color.rgb *= shade_scale_factor;\n"
"		offset = vec4(vtx_offs.rgb * shade_scale_factor, vtx_offs.a);\n"
"	}\n"
"	if (pp_Texture==1)\n"
"	{\n"
"		mediump vec4 texcol;\n"
"		if (pp_Palette == 0)\n"
"			texcol = texture(tex, vtx_uv);\n"
"		else\n"
"			texcol = palettePixel(vtx_uv);\n"
"		\n"
"		if (pp_BumpMap == 1)\n"
"		{\n"
"			highp float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;\n"
"			highp float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;\n"
"			texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);\n"
"			texcol.rgb = vec3(1.0, 1.0, 1.0);	\n"
"		}\n"
"		else if (pp_IgnoreTexA==1)\n"
"			texcol.a=1.0;	\n"
"		if (pp_ShadInstr==0)\n"
"		{\n"
"			color=texcol;\n"
"		}\n"
"		if (pp_ShadInstr==1)\n"
"		{\n"
"			color.rgb*=texcol.rgb;\n"
"			color.a=texcol.a;\n"
"		}\n"
"		if (pp_ShadInstr==2)\n"
"		{\n"
"			color.rgb=mix(color.rgb,texcol.rgb,texcol.a);\n"
"		}\n"
"		if (pp_ShadInstr==3)\n"
"		{\n"
"			color*=texcol;\n"
"		}\n"
"		\n"
"		if (pp_Offset==1 && pp_BumpMap == 0)\n"
"		{\n"
"			color.rgb+=offset.rgb;\n"
"		}\n"
"	}\n"
"	\n"
"	color = fog_clamp(color);\n"
"	\n"
"	// Look-up table mode 2: the fog colour takes the place of the pixel's\n"
"	// colour and the fog coefficient the place of its alpha. This is a mode\n"
"	// of the fog unit, which comes after the texture has been combined in,\n"
"	// so it is the combined colour that is replaced: a textured polygon\n"
"	// comes out in plain fog colour. It used to be done to the base colour,\n"
"	// before texturing, and the texture was then applied on top.\n"
"	if (pp_FogCtrl == 3)\n"
"		color = vec4(sp_FOG_COL_RAM.rgb, fog_mode2(gl_FragCoord.w) * (256.0 / 255.0));\n"
"	if (pp_FogCtrl == 0)\n"
"	{\n"
"		highp float fog = fog_mode2(gl_FragCoord.w);\n"
"		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;\n"
"	}\n"
"	if (pp_FogCtrl == 1 && pp_Offset==1 && pp_BumpMap == 0)\n"
"	{\n"
"		highp float fog = offset.a * (255.0 / 256.0);\n"
"		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;\n"
"	}\n"
"	\n"
"	if (pp_TriLinear == 1)\n"
"		color *= trilinear_alpha;\n"
"	\n"
"	// Punch-through polygons are tested on the alpha of the finished colour,\n"
"	// after the texture and the vertex colours are combined and clamped.\n"
"	if (cp_AlphaTest == 1)\n"
"	{\n"
"		if (cp_AlphaTestValue > color.a)\n"
"			discard;\n"
"		color.a=1.0;\n"
"	}\n"
"	//color.rgb=vec3(gl_FragCoord.w * sp_FOG_DENSITY / 128.0);\n"
"#if TARGET_GL != GLES2\n"
"	// (the sorted translucent depth prepass: rend/sorter.h; the depth itself is the position's)\n"
"	if (color.a < prepass_alpha)\n"
"		discard;\n"
"#endif\n"
"	gl_FragColor =color;\n"
"}\n";

static const char* ModifierVolumeShader = 
"%s\n"
"#define TARGET_GL %s\n"
"\n"
"#define GLES2 0\n"
"#define GLES3 1\n"
"#define GL2 2\n"
"#define GL3 3\n"
"\n"
"#if TARGET_GL == GL2\n"
"#define highp\n"
"#define lowp\n"
"#define mediump\n"
"#endif\n"
"#if TARGET_GL != GLES2 && TARGET_GL != GL2\n"
"out highp vec4 FragColor;\n"
"#define gl_FragColor FragColor\n"
"#endif\n"
"\n"
"uniform lowp float sp_ShaderColor;\n"
"// (the depth is the position's; the colour is the final pass's tint)\n"
"void main()\n"
"{\n"
"   gl_FragColor=vec4(0.0, 0.0, 0.0, sp_ShaderColor);\n"
"}\n";


int screen_width  = 640;
int screen_height = 480;
GLuint fogTextureId;
GLuint paletteTextureId;

xform ViewportMatrix = { 1.f, 1.f, 1.f, 0.f, 0.f, 0.f };

/* (tests: every polygon drawn with the general programs, to see they draw
 * what the polygons' own ones do) */
#ifdef GL_PROGRAM_TEST_GENERAL
static const bool PipelineTestGeneral = true;
#else
static const bool PipelineTestGeneral = false;
#endif

/* A polygon's key: what its program is made for. (Bit 5 is pp_Gouraud,
 * the one the general programs do not read from it.) */
static u32 pipeline_key(bool cp_AlphaTest, bool pp_InsideClipping,
		bool pp_Texture, bool pp_UseAlpha, bool pp_IgnoreTexA, u32 pp_ShadInstr, bool pp_Offset,
		u32 pp_FogCtrl, bool pp_Gouraud, bool pp_BumpMap, bool fog_clamping, bool trilinear,
		bool palette, bool shadowed)
{
	u32 rv=0;

	rv |= pp_InsideClipping;
	rv<<=1; rv|=cp_AlphaTest;
	rv<<=1; rv|=pp_Texture;
	rv<<=1; rv|=pp_UseAlpha;
	rv<<=1; rv|=pp_IgnoreTexA;
	rv<<=2; rv|=pp_ShadInstr;
	rv<<=1; rv|=pp_Offset;
	rv<<=2; rv|=pp_FogCtrl;
	rv<<=1; rv|=pp_Gouraud;
	rv<<=1; rv|=pp_BumpMap;
	rv<<=1; rv|=fog_clamping;
	rv<<=1; rv|=trilinear;
	rv<<=1; rv|=palette;
	rv<<=1; rv|=shadowed;
	return rv;
}

static void pipeline_from_key(PipelineShader *s, u32 key)
{
	s->key = key;
	s->pp_InsideClipping = (key >> 15) & 1;
	s->cp_AlphaTest = (key >> 14) & 1;
	s->pp_Texture = (key >> 13) & 1;
	s->pp_UseAlpha = (key >> 12) & 1;
	s->pp_IgnoreTexA = (key >> 11) & 1;
	s->pp_ShadInstr = (key >> 9) & 3;
	s->pp_Offset = (key >> 8) & 1;
	s->pp_FogCtrl = (key >> 6) & 3;
	s->pp_Gouraud = (key >> 5) & 1;
	s->pp_BumpMap = (key >> 4) & 1;
	s->fog_clamping = (key >> 3) & 1;
	s->trilinear = (key >> 2) & 1;
	s->palette = (key >> 1) & 1;
	s->shadowed = key & 1;
}

/* A program's depth scale block (DEPTH_UBO in the vertex shader) given the renderer's uniform buffer */
void gl_bind_depth_ubo(GLuint program)
{
	GLuint block;
#ifndef HAVE_OPENGLES2
	if (gl.depth_ubo == 0)
		return;
	block = glGetUniformBlockIndex(program, "DepthScaleBlock");
	if (block != GL_INVALID_INDEX)
		glUniformBlockBinding(program, block, DEPTH_UBO_BINDING);
#endif
}

/* A polygon's program asked of the driver (glprogram_cache.h): its own, or
 * (@general) the general one of its pp_Gouraud */
/* The shaders' text, made from their sources (the renderer's thread's: the
 * driver keeps its own copy, glShaderSource()) */
static char gl_vshader[8192], gl_fshader[16384];

/* Whether @fmt with @strings of its %s's and @ints %d's fits in @room */
static bool gl_shader_fits(size_t room, const char *fmt, const char *a, const char *b, int ints)
{
	return strlen(fmt) + strlen(a) + strlen(b) + (size_t)ints * 11 < room;
}

/* The vertex shader's text, of a program of @pp_Gouraud */
static const char *gl_vertex_text(int pp_Gouraud)
{
	verify(gl_shader_fits(sizeof(gl_vshader), VertexShaderSource, gl.glsl_version_header, gl.gl_version, 2));
	sprintf(gl_vshader, VertexShaderSource, gl.glsl_version_header, gl.gl_version, pp_Gouraud, gl.depth_ubo != 0);
	return gl_vshader;
}

static GLuint make_pipeline_program(const PipelineShader *s, bool general, bool background)
{
	char header[128];
	const char *vshader = gl_vertex_text(s->pp_Gouraud);

	verify(strlen(gl.glsl_version_header) + 32 < sizeof(header));
	sprintf(header, "%s\n#define GENERAL_PROGRAM", gl.glsl_version_header);
	verify(gl_shader_fits(sizeof(gl_fshader), PixelPipelineShader, header, gl.gl_version, 12));
	sprintf(gl_fshader, PixelPipelineShader, general ? header : gl.glsl_version_header, gl.gl_version,
			s->cp_AlphaTest, s->pp_InsideClipping, s->pp_UseAlpha,
			s->pp_Texture, s->pp_IgnoreTexA, s->pp_ShadInstr, s->pp_Offset, s->pp_FogCtrl, s->pp_Gouraud, s->pp_BumpMap,
			s->fog_clamping, s->trilinear, s->palette, s->shadowed);

	return glprog_make(vshader, gl_fshader, background);
}

/* Its uniforms found, the samplers and this frame's uniforms given (the
 * program bound). A general program has all of them. */
static void pipeline_locations(PipelineShader *s, bool general)
{
	GLint gu;
	glc_use_program(s->program);

	/* setup texture 0 as the input for the shader */
	gu = glGetUniformLocation(s->program, "tex");
	if (s->pp_Texture==1 || general)
		glUniform1i(gu,0);

	/* get the uniform locations */
	s->depth_scale      = glGetUniformLocation(s->program, "depth_scale");
	gl_bind_depth_ubo(s->program);

	s->pp_ClipTest      = glGetUniformLocation(s->program, "pp_ClipTest");

	s->sp_FOG_DENSITY   = glGetUniformLocation(s->program, "sp_FOG_DENSITY");

	s->cp_AlphaTestValue= glGetUniformLocation(s->program, "cp_AlphaTestValue");

	/* FOG_COL_RAM,FOG_COL_VERT,FOG_DENSITY; */
	if ((s->pp_FogCtrl==1 && s->pp_Texture==1) || general)
		s->sp_FOG_COL_VERT=glGetUniformLocation(s->program, "sp_FOG_COL_VERT");
	else
		s->sp_FOG_COL_VERT=-1;
	if (s->pp_FogCtrl==0 || s->pp_FogCtrl==3 || general)
	{
		s->sp_FOG_COL_RAM=glGetUniformLocation(s->program, "sp_FOG_COL_RAM");
	}
	else
	{
		s->sp_FOG_COL_RAM=-1;
	}
	/* Setup texture 1 as the fog table */
	gu = glGetUniformLocation(s->program, "fog_table");
	if (gu != -1)
		glUniform1i(gu, 1);
	/* And texture 2 as palette */
	gu = glGetUniformLocation(s->program, "palette");
	if (gu != -1)
		glUniform1i(gu, 2);
	s->palette_index = glGetUniformLocation(s->program, "palette_index");

	s->trilinear_alpha = glGetUniformLocation(s->program, "trilinear_alpha");
	
	if (s->fog_clamping || general)
	{
		s->fog_clamp_min = glGetUniformLocation(s->program, "fog_clamp_min");
		s->fog_clamp_max = glGetUniformLocation(s->program, "fog_clamp_max");
	}
	else
	{
		s->fog_clamp_min = -1;
		s->fog_clamp_max = -1;
	}
	s->normal_matrix = glGetUniformLocation(s->program, "normal_matrix");
	s->shade_scale_factor = glGetUniformLocation(s->program, "shade_scale_factor");
	s->prepass_alpha = glGetUniformLocation(s->program, "prepass_alpha");
	s->key_bits = !general ? -1 : glGetUniformLocation(s->program, gl.gl_major >= 3 ? "key_bits" : "key_nibbles");
	s->key_now = ~0u;
	s->shadow_set = 0;

	shader_uniforms_set(s);
	s->uniforms_set = gl.uniforms_frame;
}

bool CompilePipelineShader(	PipelineShader* s)
{
	s->program = make_pipeline_program(s, false, false);
	if (glprog_failed())
		die("shader compile fail\n");
	pipeline_locations(s, false);

	return glIsProgram(s->program)==GL_TRUE;
}

/* A polygon drawn with the general program @g until its own is there */
static void use_general(PipelineShader *s, PipelineShader *g)
{
	s->program = g->program;
	s->depth_scale = g->depth_scale;
	s->pp_ClipTest = g->pp_ClipTest;
	s->cp_AlphaTestValue = g->cp_AlphaTestValue;
	s->sp_FOG_COL_RAM = g->sp_FOG_COL_RAM;
	s->sp_FOG_COL_VERT = g->sp_FOG_COL_VERT;
	s->sp_FOG_DENSITY = g->sp_FOG_DENSITY;
	s->trilinear_alpha = g->trilinear_alpha;
	s->fog_clamp_min = g->fog_clamp_min;
	s->fog_clamp_max = g->fog_clamp_max;
	s->normal_matrix = g->normal_matrix;
	s->palette_index = g->palette_index;
	s->shade_scale_factor = g->shade_scale_factor;
	s->prepass_alpha = g->prepass_alpha;
	s->key_bits = g->key_bits;
	s->general = g;
}

/* A key first drawn (in a frame): drawn with the general program for the
 * rest of the run, with no compile or link here (AGENTS.md); the key is
 * kept, and its own program made at the start of the next. Without a
 * general program (it did not link), made now as ever. */
static void pipeline_first(PipelineShader *s, u32 key)
{
	PipelineShader *g;
	pipeline_from_key(s, key);
	glprog_note(GLPROG_GLES, key);
	g = &gl.general[s->pp_Gouraud];
	if (g->program == 0)
	{
		CompilePipelineShader(s);
		return;
	}
	use_general(s, g);
	glprog_general_used();
}

void UsePipelineShader(PipelineShader *s)
{
	PipelineShader *owner;
	glc_use_program(s->program);
	owner = s->general != NULL ? s->general : s;
	if (owner->uniforms_set != gl.uniforms_frame)
	{
		owner->uniforms_set = gl.uniforms_frame;
		shader_uniforms_set(owner);
	}
	if (s->general != NULL && owner->key_now != s->key)
	{
		owner->key_now = s->key;
		if (gl.gl_major >= 3)
			glUniform1i(owner->key_bits, (GLint)s->key);
		else
			glUniform4f(owner->key_bits, (GLfloat)(s->key & 15), (GLfloat)((s->key >> 4) & 15),
					(GLfloat)((s->key >> 8) & 15), (GLfloat)((s->key >> 12) & 15));
	}
}

/* The keys more than one game draws with (glprogram_cache.h), of Jet Grind
 * Radio, Dead or Alive 2, Sega Rally 2 and Virtua Fighter 4: made at the
 * start with those of earlier runs. (The general program draws the
 * others the first time.) */
static const u16 CommonKeys[] = {
	0x00a0,	/* Gouraud, untextured, no alpha, no fog */
	0x0aa0,	/* ...the same, with texture bits a polygon without a texture leaves */
	0x37a0,	/* Gouraud, textured, alpha, modulated alpha, offset, no fog */
};

/* At the renderer's start: the general programs, and the programs of the
 * keys of earlier runs and the common ones, all asked of the driver before
 * any is waited for */
/* @key's: found, or made (value-initialized: its program to come) */
static PipelineShader *shader_of(u32 key)
{
	PipelineShader *s = (PipelineShader *)tex_map_find(&gl.shaders, key);
	if (s == NULL && (s = (PipelineShader *)tex_map_add(&gl.shaders, key)) == NULL)
		die("Out of memory");
	return s;
}

/* (the keys made at the start, at most) */
#define GL_START_KEYS 1024

static void gl_make_programs(void)
{
	int g;
	u32 e;
	/* (the renderer's thread's, at its start) */
	static u32 keys[GL_START_KEYS];
	static u32 failed[1024];
	u32 nkeys, count, i, nfailed;
	const u32 *known;
	if (gl.shaders.elem == 0)
		tex_map_init(&gl.shaders, sizeof(PipelineShader), NULL, NULL);
	gl.shaders.name = "OpenGL programs";
	tex_map_reserve(&gl.shaders, 2048);
	for (g = 0; g < 2; g++)
	{
		PipelineShader *s = &gl.general[g];
		memset(s, 0, sizeof(*s));
		s->pp_Gouraud = g;
		s->program = make_pipeline_program(s, true, true);
	}
	count = 0;
	known = glprog_known(GLPROG_GLES, &count);
	nkeys = 0;
	for (i = 0; i < count; i++)
		if (known[i] <= 0xFFFF && nkeys < GL_START_KEYS)
			keys[nkeys++] = known[i];
	for (i = 0; i < (sizeof(CommonKeys) / sizeof(CommonKeys[0])) && nkeys < GL_START_KEYS; i++)
		keys[nkeys++] = CommonKeys[i];
	/* (the framebuffer's, the VMU screens' and the crosshairs': drawn with DrawQuad()) */
	if (nkeys < GL_START_KEYS)
		keys[nkeys++] = pipeline_key(false, false, true, false, true, 0, false, 2, false, false, false, false, false, false);
	if (nkeys < GL_START_KEYS)
		keys[nkeys++] = pipeline_key(false, false, true, true, false, 0, false, 2, false, false, false, false, false, false);
	if (!PipelineTestGeneral)
		for (i = 0; i < nkeys; i++)
		{
			PipelineShader *s = shader_of(keys[i]);
			if (s->program != 0 || s->pending != 0)
				continue;
			pipeline_from_key(s, keys[i]);
			s->pending = make_pipeline_program(s, false, true);
		}
	/* now waited for */
	for (g = 0; g < 2; g++)
	{
		PipelineShader *s = &gl.general[g];
		if (s->program == 0)
			continue;
		if (glprog_wait(s->program))
			pipeline_locations(s, true);
		else
		{
			WARN_LOG(RENDERER, "OpenGL: no general program (pp_Gouraud %d)\n", g);
			glDeleteProgram(s->program);
			s->program = 0;
		}
	}
	/* (those that did not link go: taken away after, not while they are
	 * gone through) */
	nfailed = 0;
	for (e = 0; e < tex_map_places(&gl.shaders); e++)
	{
		u64 key;
		PipelineShader *s = (PipelineShader *)tex_map_at(&gl.shaders, e, &key);
		if (s != NULL && s->pending != 0 && s->program == 0)
		{
			const GLuint p = s->pending;
			s->pending = 0;
			if (glprog_wait(p))
			{
				s->program = p;
				pipeline_locations(s, false);
			}
			else
			{
				WARN_LOG(RENDERER, "OpenGL: the program of key %x did not link\n", s->key);
				glDeleteProgram(p);
				if (nfailed < (sizeof(failed) / sizeof(failed[0])))
					failed[nfailed++] = (u32)key;
			}
		}
	}
	for (i = 0; i < nfailed; i++)
		tex_map_remove(&gl.shaders, failed[i]);
}

PipelineShader *GetProgram(bool cp_AlphaTest, bool pp_InsideClipping,
		bool pp_Texture, bool pp_UseAlpha, bool pp_IgnoreTexA, u32 pp_ShadInstr, bool pp_Offset,
		u32 pp_FogCtrl, bool pp_Gouraud, bool pp_BumpMap, bool fog_clamping, bool trilinear,
		bool palette, bool shadowed)
{
	const u32 rv = pipeline_key(cp_AlphaTest, pp_InsideClipping, pp_Texture, pp_UseAlpha, pp_IgnoreTexA,
			pp_ShadInstr, pp_Offset, pp_FogCtrl, pp_Gouraud, pp_BumpMap, fog_clamping, trilinear, palette, shadowed);

	PipelineShader *shader = shader_of(rv);
	if (shader->program == 0)
		pipeline_first(shader, rv);

#ifdef RPI4_SET_UNIFORM_ATTRIBUTES_BUG
	/* rpi 4 has a bug where it does not save uniform and attribute state with */
	/* the program, so they have to be reinit each time you reuse the program */
	glc_use_program(shader->program);
	if (shader->general != NULL)
	{
		/* (and the key, and this frame's uniforms, given again: UsePipelineShader()) */
		shader->general->key_now = ~0u;
		shader->general->uniforms_set = gl.uniforms_frame - 1;
	}
	/* setup texture 0 as the input for the shader */
	GLuint gu=glGetUniformLocation(shader->program, "tex");
	if (shader->pp_Texture==1)
		glUniform1i(gu,0);
	/* Setup texture 1 as the fog table */
	gu = glGetUniformLocation(shader->program, "fog_table");
	if (gu != -1)
		glUniform1i(gu, 1);
	shader_uniforms_set(shader);
#endif

	return shader;
}

/* What video memory there is to use (rend/vram_budget.h): what
 * GL_NVX_gpu_memory_info says is available (all of it counts as the
 * budget, none as used), or GL_ATI_meminfo's free texture memory; and
 * without either, a fixed figure small enough for any device that runs the
 * renderer, which a rasterizer in software has too. A device that has next
 * to no memory of its own (less than GL_UMA_DEDICATED_KIB: an integrated one
 * that says so) uses the system's: what the GPU can reach is its budget,
 * the free auxiliary memory by GL_ATI_meminfo. */
#define GL_UMA_DEDICATED_KIB (64 << 10)
#define GL_MOST_KIB (1 << 28)
static void gl_query_vram_budget(bool nvx_meminfo, bool ati_meminfo)
{
	const char *renderer_name;
	const GLenum nvx_dedicated = 0x9047;		/* GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX */
	const GLenum nvx_total_available = 0x9048;	/* GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX */
	const GLenum nvx_current_available = 0x9049;	/* GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX */
	const GLenum ati_texture_free = 0x87FC;		/* GL_TEXTURE_FREE_MEMORY_ATI */
	GLint kib[4] = { 0, 0, 0, 0 };

	memset(&gl.vram_budget, 0, sizeof(gl.vram_budget));
	/* (a rasterizer in software has no video memory, and what Mesa's say of
	 * it through GL_NVX_gpu_memory_info is what was on the stack - another
	 * figure each run: the budget is the estimate, every run the same) */
	renderer_name = (const char *)glGetString(GL_RENDERER);
	if (renderer_name != NULL && (strstr(renderer_name, "llvmpipe") != NULL || strstr(renderer_name, "softpipe") != NULL))
		nvx_meminfo = ati_meminfo = false;
	if (nvx_meminfo)
	{
		GLint total = 0, dedicated = 0;
		bool ok;
		glGetIntegerv(nvx_dedicated, &dedicated);
		glGetIntegerv(nvx_total_available, &total);
		glGetIntegerv(nvx_current_available, &kib[0]);
		/* (a figure no card has, 256 GB or more, is not one) */
		ok = glGetError() == GL_NO_ERROR && total > 0 && total < GL_MOST_KIB
				&& dedicated >= 0 && dedicated < GL_MOST_KIB;
		if (ok && dedicated > 0 && dedicated < GL_UMA_DEDICATED_KIB)
		{
			gl.vram_budget.budget_kib = (u32)total;
			gl.vram_budget.exact = true;
		}
		else if (ok && kib[0] > 0)
		{
			gl.vram_budget.budget_kib = (u32)total;
			gl.vram_budget.usage_kib = total > kib[0] ? (u32)(total - kib[0]) : 0;
			gl.vram_budget.exact = true;
		}
	}
	if (!gl.vram_budget.exact && ati_meminfo)
	{
		bool ok;
		glGetIntegerv(ati_texture_free, kib);
		ok = glGetError() == GL_NO_ERROR;
		if (ok && kib[0] >= 0 && kib[0] < GL_UMA_DEDICATED_KIB && kib[2] > kib[0])
		{
			gl.vram_budget.budget_kib = (u32)kib[0] + (u32)kib[2];
			gl.vram_budget.exact = true;
		}
		else if (ok && kib[0] > 0)
		{
			gl.vram_budget.budget_kib = (u32)kib[0];
			gl.vram_budget.exact = true;
		}
	}
	if (!gl.vram_budget.exact)
		gl.vram_budget.budget_kib = (gl.is_gles ? 256u : 512u) << 10;
	vram_budget_say("OpenGL", &gl.vram_budget);
}

void findGLVersion(void)
{
	const char *version;
	bool ubo, clip_control;
	bool nvx_meminfo, ati_meminfo;
	gl.stencil_present = true;
	gl.index_type = GL_UNSIGNED_INT;

	while (true)
		if (glGetError() == GL_NO_ERROR)
			break;
	glGetIntegerv(GL_MAJOR_VERSION, &gl.gl_major);
	if (glGetError() == GL_INVALID_ENUM)
		gl.gl_major = 2;
	else
		glGetIntegerv(GL_MINOR_VERSION, &gl.gl_minor);

	version = (const char *)glGetString(GL_VERSION);
	NOTICE_LOG(RENDERER, "OpenGL version: %s\n", version);
	if (!strncmp(version, "OpenGL ES", 9))
	{
		GLint stencilBits;
		gl.is_gles = true;
		if (gl.gl_major >= 3)
		{
			gl.gl_version = "GLES3";
			gl.glsl_version_header = "#version 300 es";
			/* (GL_R8: a size the texture pool can make, as GL_ALPHA has none) */
			gl.single_channel_format = GL_RED;
			gl.single_channel_internal = GL_R8;
		}
		else
		{
			gl.gl_version = "GLES2";
			gl.glsl_version_header = "";
			gl.index_type = GL_UNSIGNED_SHORT;
			gl.single_channel_format = GL_ALPHA;
			gl.single_channel_internal = GL_ALPHA;
		}

		stencilBits = 0;
		glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
		if (stencilBits == 0)
			gl.stencil_present = false;
	}
	else
	{
		gl.is_gles = false;
		if (gl.gl_major >= 3)
		{
			gl.gl_version = "GL3";
			gl.glsl_version_header = "#version 130";
			gl.single_channel_format = GL_RED;
			gl.single_channel_internal = GL_RED;
		}
		else
		{
			gl.gl_version = "GL2";
			gl.glsl_version_header = "#version 120";
			gl.single_channel_format = GL_ALPHA;
			gl.single_channel_internal = GL_ALPHA;
		}
	}
	gl.max_anisotropy = 1.f;
	nvx_meminfo = false;
	ati_meminfo = false;
	/* The depth (rend/depth_scale.h): a 32-bit float depth buffer where
	 * this renderer makes its own (OpenGL 3.0, OpenGL ES 3.0; the frame
	 * buffer the frontend gives has what it gives), the depth scale in a
	 * uniform buffer where there are any (OpenGL 3.1 or its extension,
	 * OpenGL ES 3.0: so that a frame decoded on the GPU can write it), and
	 * clip space z in [0,1] where the context has ARB_clip_control
	 * (OpenGL 4.5's), which keeps a float depth buffer's precision */
	ubo = false;
	clip_control = false;
	gl.depth_format = gl.gl_major >= 3 ? GL_DEPTH32F_STENCIL8 : RARCH_GL_DEPTH24_STENCIL8;
#ifndef HAVE_OPENGLES2
	/* (a build with the OpenGL ES 2 headers has no uniform buffer functions: the uniform, on any context) */
	ubo = gl.is_gles ? gl.gl_major >= 3 : gl.gl_major * 10 + gl.gl_minor >= 31;
	gl.buffer_storage = false;
	/* (by the extension, which every driver with it lists, 4.5's or not: the other path can then be tried by turning the extension off) */
	if (gl.gl_major >= 3)
	{
		u32 i;
		for (i = 0; ; i++)
		{
			const char* extension = (const char *)glGetStringi(GL_EXTENSIONS, i);
			if (extension == NULL)
				break;
			if (!strcmp(extension, "GL_EXT_texture_filter_anisotropic"))
				glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &gl.max_anisotropy);
			else if (!strcmp(extension, "GL_NVX_gpu_memory_info"))
				nvx_meminfo = true;
			else if (!strcmp(extension, "GL_ATI_meminfo"))
				ati_meminfo = true;
			else if (!strcmp(extension, "GL_ARB_uniform_buffer_object"))
				ubo = true;
			else if (!strcmp(extension, "GL_ARB_clip_control") && !gl.is_gles)
				clip_control = true;
			/* (by the extension too, which every driver with OpenGL 4.4 lists) */
			else if (!strcmp(extension, "GL_ARB_buffer_storage") && !gl.is_gles)
				gl.buffer_storage = true;
		}
	}
	else
#endif
	{
		const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
		if (extensions != NULL)
		{
			nvx_meminfo = strstr(extensions, "GL_NVX_gpu_memory_info") != NULL;
			ati_meminfo = strstr(extensions, "GL_ATI_meminfo") != NULL;
		}
	}
	gl.clipControl = NULL;
	gl.clearDepthf = NULL;
	gl.copyBufferSubData = NULL;
	{
		glsm_ctx_proc_address_t proc = { NULL };
		if (glsm_ctl(GLSM_CTL_PROC_ADDRESS_GET, &proc) && proc.addr != NULL)
		{
			if (clip_control)
				gl.clipControl = (void (APIENTRYP)(GLenum, GLenum))proc.addr("glClipControl");
#ifndef HAVE_OPENGLES
			/* (an ES build has glsm's, which is ES's) */
			if (gl.is_gles)
				gl.clearDepthf = (void (APIENTRYP)(GLfloat))proc.addr("glClearDepthf");
#endif
			if (ubo)
				gl.copyBufferSubData = (void (APIENTRYP)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr))proc.addr("glCopyBufferSubData");
		}
	}
	if (gl.depth_ubo != 0)
		glDeleteBuffers(1, &gl.depth_ubo);
	gl.depth_ubo = 0;
	/* (the copy is OpenGL 3.1's too; without it the uniform, and no frame decoded on the GPU: gles/ta_decoder_gl.h needs 4.3) */
	if (ubo && gl.copyBufferSubData != NULL)
	{
		glGenBuffers(1, &gl.depth_ubo);
		glBindBuffer(GL_UNIFORM_BUFFER, gl.depth_ubo);
		glBufferData(GL_UNIFORM_BUFFER, 16, NULL, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
	}
	(void)clip_control;
	NOTICE_LOG(RENDERER, "OpenGL depth: %s depth buffers, the scale in a %s, clip space z in %s\n",
			gl.depth_format == GL_DEPTH32F_STENCIL8 ? "32-bit float" : "24-bit",
			gl.depth_ubo != 0 ? "uniform buffer" : "uniform", gl.clipControl != NULL ? "[0,1]" : "[-1,1]");
	while (glGetError() != GL_NO_ERROR)
		;
	gl_query_vram_budget(nvx_meminfo, ati_meminfo);
}

GLuint gl_CompileShader(const char* shader,GLuint type)
{
	GLint result;
	GLint compile_log_len;
	GLuint rv=glCreateShader(type);
	glShaderSource(rv, 1,&shader, NULL);
	glCompileShader(rv);

	/* lets see if it compiled ... */
	glGetShaderiv(rv, GL_COMPILE_STATUS, &result);
	glGetShaderiv(rv, GL_INFO_LOG_LENGTH, &compile_log_len);

	if (!result && compile_log_len>0)
	{
		char*compile_log;
		if (compile_log_len==0)
			compile_log_len=1;
		compile_log = (char*)malloc(compile_log_len);
		*compile_log=0;

		glGetShaderInfoLog(rv, compile_log_len, &compile_log_len, compile_log);
		WARN_LOG(RENDERER, "Shader: %s \n%s\n", result ? "compiled!" : "failed to compile", compile_log);

		free(compile_log);
	}

	return rv;
}

/* (through the programs kept: glprogram_cache.h) */
GLuint gl_CompileAndLink(const char* VertexShader, const char* FragmentShader)
{
	const GLuint program = glprog_make(VertexShader, FragmentShader, false);
	if (glprog_failed())
		die("shader compile fail\n");

	glc_use_program(program);

	verify(glIsProgram(program));

	return program;
}

/*
GL|ES 2
Slower, smaller subset of gl2

*Optimisation notes*
Keep stuff in packed ints
Keep data as small as possible
Keep vertex programs as small as possible
The drivers more or less suck. Don't depend on dynamic allocation, or any 'complex' feature
as it is likely to be problematic/slow
Do we really want to enable striping joins?

*Design notes*
Follow same architecture as the d3d renderer for now
Render to texture, keep track of textures in GL memory
Direct flip to screen (no vlbank/fb emulation)
Do we really need a combining shader? it is needlessly expensive for openGL | ES
Render contexts
Free over time? we actually care about ram usage here?
Limit max resource size? for psp 48k verts worked just fine

FB:
Pixel clip, mapping

SPG/VO:
mapping

TA:
Tile clip

*/

static void gl_delete_shaders(void)
{
	u32 e;
	int g;
	for (e = 0; e < tex_map_places(&gl.shaders); e++)
	{
		u64 key;
		const PipelineShader *s = (const PipelineShader *)tex_map_at(&gl.shaders, e, &key);
		if (s == NULL)
			continue;
		/* (one drawn with a general program has none of its own, but may have one coming) */
		if (s->program != 0 && s->general == NULL)
			glDeleteProgram(s->program);
		if (s->pending != 0)
			glDeleteProgram(s->pending);
	}
	tex_map_clear(&gl.shaders);
	for (g = 0; g < 2; g++)
	{
		if (gl.general[g].program != 0)
			glDeleteProgram(gl.general[g].program);
		gl.general[g].program = 0;
	}
	glDeleteProgram(gl.modvol_shader.program);
	gl.modvol_shader.program = 0;
}

static void own_target_term(void);	/* (RenderFrame()'s framebuffer) */

static void gl_term(void)
{
	gl_frame_buffers_term();
	glc_delete_textures((sizeof(vmuTextureId) / sizeof(vmuTextureId[0])), vmuTextureId);
	memset(vmuTextureId, 0, sizeof(vmuTextureId));
	glc_delete_textures((sizeof(lightgunTextureId) / sizeof(lightgunTextureId[0])), lightgunTextureId);
	memset(lightgunTextureId, 0, sizeof(lightgunTextureId));

	glDeleteBuffers(1, &gl.vbo.geometry);
	glDeleteBuffers(1, &gl.vbo.modvols);
	glDeleteBuffers(1, &gl.vbo.idxs);
	glDeleteBuffers(1, &gl.vbo.idxs2);
	if (gl.depth_ubo != 0)
		glDeleteBuffers(1, &gl.depth_ubo);
	gl.depth_ubo = 0;
	glc_delete_textures(1, &fbTextureId);
	fbTextureId = 0;
	glc_delete_textures(1, &fogTextureId);
	fogTextureId = 0;
	glc_delete_textures(1, &paletteTextureId);
	paletteTextureId = 0;
	own_target_term();

	gl_delete_shaders();
}

void gl_buffer_room(GLuint buffer, size_t bytes)
{
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static bool gl_create_resources(void)
{
	/* create VBOs */
	glGenBuffers(1, &gl.vbo.geometry);
	glGenBuffers(1, &gl.vbo.modvols);
	glGenBuffers(1, &gl.vbo.idxs);
	glGenBuffers(1, &gl.vbo.idxs2);
	/* (each its room now: gl_buffer_room()) */
	gl_buffer_room(gl.vbo.geometry, GL_BUFFER_ROOM * 4);
	gl_buffer_room(gl.vbo.modvols, GL_BUFFER_ROOM);
	gl_buffer_room(gl.vbo.idxs, GL_BUFFER_ROOM);
	gl_buffer_room(gl.vbo.idxs2, GL_BUFFER_ROOM);
	/* (and the translucent sort's room: rend/sorter.h) */
	gl_sort_reserve();

   findGLVersion();
   glprog_open();

	verify(gl_shader_fits(sizeof(gl_fshader), ModifierVolumeShader, gl.glsl_version_header, gl.gl_version, 0));
	sprintf(gl_fshader, ModifierVolumeShader, gl.glsl_version_header, gl.gl_version);

	gl.modvol_shader.program = gl_CompileAndLink(gl_vertex_text(1), gl_fshader);
	gl_bind_depth_ubo(gl.modvol_shader.program);
	gl.modvol_shader.normal_matrix  = glGetUniformLocation(gl.modvol_shader.program, "normal_matrix");
	gl.modvol_shader.depth_scale = glGetUniformLocation(gl.modvol_shader.program, "depth_scale");
	gl.modvol_shader.sp_ShaderColor = glGetUniformLocation(gl.modvol_shader.program, "sp_ShaderColor");

	return true;
}

void UpdateFogTexture(u8 *fog_table, GLenum texture_slot, GLint fog_image_format)
{
	/* The table is 256 bytes, so whether it needs uploading is decided by
	 * looking at it: build the texture's contents from the registers this
	 * frame is rendered with and upload them if they are not what the GPU
	 * already has.
	 *
	 * This used to go by a flag the register write set and the renderer
	 * cleared. With Threaded Rendering the two are different threads, and
	 * the write could come after this frame's copy of the registers was
	 * taken: the renderer then uploaded the old table, cleared the flag,
	 * and the new table was never uploaded. A game that rewrites the same
	 * table every frame also no longer uploads it every frame. */
	static u8 uploaded[256];
	u8 temp_tex_buffer[256];
	MakeFogTexture(temp_tex_buffer);

	if (fogTextureId != 0 && !memcmp(uploaded, temp_tex_buffer, sizeof(uploaded)))
		return;

	glActiveTexture(texture_slot);
	if (fogTextureId == 0)
	{
		fogTextureId = glc_gen_texture();
		glc_bind_texture(GL_TEXTURE_2D, fogTextureId);
		/* The shader reads single entries, at the middle of their texels,
		 * and blends them itself: nothing for the card to filter, and
		 * filtering, a card whose weights are not exact there lets the
		 * entry next to it in. */
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else
		glc_bind_texture(GL_TEXTURE_2D, fogTextureId);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, fog_image_format == GL_RED ? GL_R8 : fog_image_format, 128, 2, 0, fog_image_format, GL_UNSIGNED_BYTE, temp_tex_buffer);
	glCheck();
	memcpy(uploaded, temp_tex_buffer, sizeof(uploaded));

	glActiveTexture(GL_TEXTURE0);
}

void UpdatePaletteTexture(GLenum texture_slot)
{
	glActiveTexture(texture_slot);
	if (paletteTextureId == 0)
	{
		paletteTextureId = glc_gen_texture();
		glc_bind_texture(GL_TEXTURE_2D, paletteTextureId);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glc_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else
		glc_bind_texture(GL_TEXTURE_2D, paletteTextureId);

   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, palette32_ram);
	glCheck();

	glActiveTexture(GL_TEXTURE0);
}


void vertex_buffer_unmap(void)
{
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void DoCleanup() {
}

static void upload_vertex_indices(void)
{
	if (gl.index_type == GL_UNSIGNED_SHORT)
	{
		u32 i;
		/* (kept from one frame to the next: grown, never made again) */
		static grow_buf_t short_idx;
		const u32 count = TA_LIST_USED(pvrrc.idx, u32);
		u16 *to = (u16 *)grow_buf_reserve(&short_idx, (size_t)count * sizeof(u16) + 2);
		const u32 *p = TA_LIST_HEAD(pvrrc.idx, u32);
		if (to == NULL)
			return;
		for (i = 0; i < count; i++)
			to[i] = (u16)p[i];
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(u16), to, GL_STREAM_DRAW);
	}
	else
		glBufferData(GL_ELEMENT_ARRAY_BUFFER,TA_LIST_BYTES(pvrrc.idx),TA_LIST_HEAD(pvrrc.idx, u32),GL_STREAM_DRAW);
}

/* The picture of a frame for the screen, drawn into a framebuffer of the
 * renderer's own and copied to the frontend's at the end.
 *
 * The depth is 1/w, scaled by 1/fZ_max (rend/depth_scale.h), and needs a
 * float depth buffer: a frame whose fZ_max is 100000 (Virtua Fighter 4's,
 * which has a vertex that far) puts its sky, at a 1/w of 0.003, at a depth
 * of 3e-8 - below the step of a 24-bit depth buffer, where it is the same
 * depth as the background plane behind it, and its depth test, greater,
 * fails: the sky was not drawn, and a wedge of the background plane's
 * colour showed through it, on one frame of the fade to white. The
 * frontend's framebuffer has the depth buffer it has (RetroArch's: 24
 * bits), so this renderer draws into one with gl.depth_format's - 32-bit
 * float, as the per-pixel renderer and Vulkan's have - wherever there is
 * one (OpenGL 3, OpenGL ES 3). It is made when the size of the picture
 * changes, not in a frame, and starts out as what the frontend's has (the
 * picture so far: a frame need not clear it). */
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058			/* (OpenGL ES 3's, under OpenGL ES 2's headers) */
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
static GLuint own_fbo, own_color, own_depth;
static int own_w, own_h;
/* With the chip's write-out (gl_writeout.h) the picture is a texture, which
 * its pass writes in place before it goes to the frontend's */
static bool own_color_texture;

static void own_target_term(void)
{
	if (own_fbo != 0)
		glDeleteFramebuffers(1, &own_fbo);
	if (own_color != 0 && own_color_texture)
		glc_delete_textures(1, &own_color);
	else if (own_color != 0)
		glDeleteRenderbuffers(1, &own_color);
	if (own_depth != 0)
		glDeleteRenderbuffers(1, &own_depth);
	own_fbo = own_color = own_depth = 0;
	own_w = own_h = 0;
}

/* The framebuffer the screen's picture is drawn in bound, w x h: true if
 * it is the renderer's own (own_target_present() copies it to the
 * frontend's), false if it is the frontend's */
static bool own_target_bind(int w, int h)
{
	GLuint target;
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
	const bool texture = gl_writeout_ready();
	if ((gl.depth_format != GL_DEPTH32F_STENCIL8 && !texture) || w <= 0 || h <= 0)
		return false;
	target = hw_render.get_current_framebuffer();
	if (own_fbo == 0 || own_w != w || own_h != h || own_color_texture != texture)
	{
		own_target_term();
		own_color_texture = texture;
		if (texture)
		{
			own_color = glc_gen_texture();
			glc_bind_texture(GL_TEXTURE_2D, own_color);
			glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
			glc_bind_texture(GL_TEXTURE_2D, 0);
		}
		else
		{
			glGenRenderbuffers(1, &own_color);
			glBindRenderbuffer(RARCH_GL_RENDERBUFFER, own_color);
			glRenderbufferStorage(RARCH_GL_RENDERBUFFER, GL_RGBA8, w, h);
		}
		glGenRenderbuffers(1, &own_depth);
		glBindRenderbuffer(RARCH_GL_RENDERBUFFER, own_depth);
		glRenderbufferStorage(RARCH_GL_RENDERBUFFER, gl.depth_format, w, h);
		glBindRenderbuffer(RARCH_GL_RENDERBUFFER, 0);
		glGenFramebuffers(1, &own_fbo);
		glBindFramebuffer(RARCH_GL_FRAMEBUFFER, own_fbo);
		if (texture)
			glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, own_color, 0);
		else
			glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, RARCH_GL_RENDERBUFFER, own_color);
		glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, RARCH_GL_RENDERBUFFER, own_depth);
		if (glCheckFramebufferStatus(RARCH_GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			WARN_LOG(RENDERER, "OpenGL: no framebuffer with a float depth buffer: drawing into the frontend's\n");
			own_target_term();
			glBindFramebuffer(RARCH_GL_FRAMEBUFFER, target);
			return false;
		}
		own_w = w;
		own_h = h;
		glc_disable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, own_fbo);
		glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, own_fbo);
	return true;
#else
	(void)w; (void)h;
	return false;
#endif
}

/* The picture drawn in the renderer's own framebuffer to the frontend's, which is left bound */
static void own_target_present(void)
{
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
	const GLuint target = hw_render.get_current_framebuffer();
	glc_disable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, own_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glBlitFramebuffer(0, 0, own_w, own_h, 0, 0, own_w, own_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, target);
#endif
}

/* (lroundf()'s: half away from zero) */
static long gl_round_away(float v)
{
	const double d = v;
	return d >= 0 ? (long)floor(d + 0.5) : -(long)floor(-d + 0.5);
}

static bool RenderFrame(void)
{
	bool is_rtt;
	u8*fog_colram_bgra;
	u8*fog_colvert_bgra;
	s32 fog_den_exp;
	float fog_den_mant;
	u8*fog_density;
	bool wide_screen_on;
	transform_matrix_t matrices;
	const xform *scissor_mat;
	int vmu_screen_number = 0 ;
	bool own_target = false;
	int lightgun_port = 0 ;

	DoCleanup();

	is_rtt = pvrrc.isRTT;

	transform_matrix_init(&matrices, true);
	transform_matrix_calc(&matrices, &pvrrc);
	xform_to_mat4(&matrices.normal, ShaderUniforms.normal_mat);
	scissor_mat = &matrices.scissor;
	ViewportMatrix = matrices.viewport;

	if (!is_rtt)
		gcflip = 0;
	else
		gcflip = 1;

	/* The depth is the position's (rend/depth_scale.h, and the vertex
	 * shader): a*S*z + b, with S = 1/fZ_max; a and b take the depth to the
	 * device's clip space - [0,1] with clip control, else [-1,1] - and undo
	 * what the matrix scales z by */
	{
		const float mul = 1.f / matrices.normal.sz;
		ShaderUniforms.depth_coefs[0] = gl.clipControl != NULL ? mul : 2.f * mul;
		ShaderUniforms.depth_coefs[1] = gl.clipControl != NULL ? 0.f : -1.f;
		ShaderUniforms.depth_coefs[2] = 0;
		ShaderUniforms.depth_coefs[3] = pvr_depth_scale(pvrrc.fZ_max);
#ifndef HAVE_OPENGLES2
		if (gl.depth_ubo != 0)
		{
			/* ...in the uniform buffer, and for a frame decoded on the GPU,
			 * S from the decode's word of it, copied over the host's (the
			 * copy is ordered after the decode's kernels: ta_decoder_gl.h) */
			glBindBuffer(GL_UNIFORM_BUFFER, gl.depth_ubo);
			glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ShaderUniforms.depth_coefs), ShaderUniforms.depth_coefs);
			if (pvrrc.gpu)
			{
				glBindBuffer(GL_COPY_READ_BUFFER, gltad_draw()->frame);
				gl.copyBufferSubData(GL_COPY_READ_BUFFER, GL_UNIFORM_BUFFER, TAG_O_ZSCALE * 4, 12, 4);
				glBindBuffer(GL_COPY_READ_BUFFER, 0);
			}
			glBindBuffer(GL_UNIFORM_BUFFER, 0);
			glBindBufferBase(GL_UNIFORM_BUFFER, DEPTH_UBO_BINDING, gl.depth_ubo);
		}
#endif
	}
	if (gl.clipControl != NULL)
		gl.clipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

	/* VERT and RAM fog color constants */
	fog_colvert_bgra = (u8*)&FOG_COL_VERT;
	fog_colram_bgra = (u8*)&FOG_COL_RAM;
	ShaderUniforms.ps_FOG_COL_VERT[0] = fog_colvert_bgra[2] / 255.0f;
	ShaderUniforms.ps_FOG_COL_VERT[1] = fog_colvert_bgra[1] / 255.0f;
	ShaderUniforms.ps_FOG_COL_VERT[2] = fog_colvert_bgra[0] / 255.0f;

	ShaderUniforms.ps_FOG_COL_RAM[0] = fog_colram_bgra[2] / 255.0f;
	ShaderUniforms.ps_FOG_COL_RAM[1] = fog_colram_bgra[1] / 255.0f;
	ShaderUniforms.ps_FOG_COL_RAM[2] = fog_colram_bgra[0] / 255.0f;

	/* Fog density constant */
	fog_density = (u8*)&FOG_DENSITY;
	fog_den_mant = fog_density[1] / 128.0f;  /* bit 7 -> x. bit, so [6:0] -> fraction -> /128 */
	fog_den_exp = (s8)fog_density[0];
	ShaderUniforms.fog_den_float = fog_den_mant * (float)pow(2.0, fog_den_exp);

	ShaderUniforms.fog_clamp_min[0] = ((pvrrc.fog_clamp_min >> 16) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_min[1] = ((pvrrc.fog_clamp_min >> 8) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_min[2] = ((pvrrc.fog_clamp_min >> 0) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_min[3] = ((pvrrc.fog_clamp_min >> 24) & 0xFF) / 255.0f;

	ShaderUniforms.fog_clamp_max[0] = ((pvrrc.fog_clamp_max >> 16) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_max[1] = ((pvrrc.fog_clamp_max >> 8) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_max[2] = ((pvrrc.fog_clamp_max >> 0) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_max[3] = ((pvrrc.fog_clamp_max >> 24) & 0xFF) / 255.0f;


	UpdateFogTexture((u8 *)FOG_TABLE, GL_TEXTURE1, gl.single_channel_format);
	if (palette_updated)
	{
		UpdatePaletteTexture(GL_TEXTURE2);
		palette_updated = false;
	}

	glc_use_program(gl.modvol_shader.program);

	glUniform4fv(gl.modvol_shader.depth_scale, 1, ShaderUniforms.depth_coefs);
	glUniformMatrix4fv(gl.modvol_shader.normal_matrix, 1, GL_FALSE, ShaderUniforms.normal_mat);

	/* A punch-through texel is kept when its alpha, eight bits, is at least
	 * the reference. The shader compares floats, and the two come to it by
	 * different roads - a texel's alpha by the graphics card's own division -
	 * so that one equal to the reference could come out a hair under it and
	 * be dropped. The reference is given half a step low: then any alpha
	 * that is a whole number of steps is on the right side by half a one. */
	ShaderUniforms.PT_ALPHA = ((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f;
	ShaderUniforms.shade_scale_factor = FPU_SHAD_SCALE.scale_factor / 256.f;

	/* (given to each program the first time it is bound in the frame: UsePipelineShader()) */
	gl.uniforms_frame++;

	/* setup render target first */
	if (is_rtt)
	{
		GLuint channels = GL_RGBA, format = GL_UNSIGNED_BYTE;
		/* A pixel clip that ends before it starts holds no pixel: the chip
		 * writes nothing. (Its size would be negative here, and a texture
		 * that size is no texture at all.) */
		if (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min
				|| FB_X_CLIP.max < FB_X_CLIP.min || FB_Y_CLIP.max < FB_Y_CLIP.min)
		{
			WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d\n",
					pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
			return false;
		}
		switch(FB_W_CTRL.fb_packmode)
		{
			case 0: /* 0x0   0555 KRGB 16 bit  (default)	Bit 15 is the value of fb_kval[7]. */
				channels=GL_RGBA;
				format=GL_UNSIGNED_BYTE;
				break;

			case 1: /* 0x1   565 RGB 16 bit */
				channels=GL_RGB;
				/* Five or six bits a channel is what the game asked for, and
				 * what the picture is cut down to when it goes to video
				 * memory. As a texture it does not have to lose them: eight
				 * bits a channel, as the Vulkan renderer keeps it. (Where a
				 * driver will not render to that, BindRTT() goes back to
				 * 5-6-5.) */
				format = GL_UNSIGNED_BYTE;
				break;

			case 2: /* 0x2   4444 ARGB 16 bit */
				channels=GL_RGBA;
				format=GL_UNSIGNED_BYTE;
				break;

			case 3:/* 0x3    1555 ARGB 16 bit    The alpha value is determined by comparison with the value of fb_alpha_threshold. */
				channels=GL_RGBA;
				format=GL_UNSIGNED_BYTE;
				break;

			case 4: /* 0x4   888 RGB 24 bit packed */
			case 5: /* 0x5   0888 KRGB 32 bit    K is the value of fk_kval. */
			case 6: /* 0x6   8888 ARGB 32 bit */
				WARN_LOG(RENDERER, "Unsupported render to texture format: %d\n", FB_W_CTRL.fb_packmode);
				return false;
			case 7: /* 7     invalid */
				/* (a mode the chip does not have, which a game can write all the same: nothing is drawn) */
				WARN_LOG(RENDERER, "Invalid render to texture format: 7\n");
				return false;
		}
		DEBUG_LOG(RENDERER, "RTT packmode=%d stride=%d - %d,%d -> %d,%d\n", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
 				FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max);
		/* (from the frame's first pixel: see TransformMatrix) */
		BindRTT(FB_W_SOF1 & VRAM_MASK, FB_X_CLIP.max + 1, FB_Y_CLIP.max + 1, channels,format);
	}
	else
	{
		own_target = own_target_bind(screen_width, screen_height);
		glViewport(0, 0, screen_width, screen_height);
		/* in a new context, the picture kept from the old one (rend/last_picture.h) */
		gl_restore_picture();
	}

	wide_screen_on = !is_rtt && settings.rend.WideScreen && !transform_matrix_clipped(&matrices);

	/* Color is cleared by the background plane */

	glc_disable(GL_SCISSOR_TEST);

	glc_depth_mask(GL_TRUE);
	/* (a desktop build on an OpenGL ES context: glClearDepth() is not ES's,
	 * and does nothing there) */
	if (gl.clearDepthf != NULL)
		gl.clearDepthf(0.f);
	else
		glClearDepth(0.0);
	glc_stencil_mask(0xFF); glCheck();
	glClearStencil(0);
	if (!is_rtt && pvrrc.clearFramebuffer)
	{
		/* a framebuffer the game has not drawn to lately: nothing of the last picture belongs in it */
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glc_clear_color(VO_BORDER_COL.Red / 255.f, VO_BORDER_COL.Green / 255.f, VO_BORDER_COL.Blue / 255.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glCheck();
	}
	else
	{
		glClear(GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glCheck();
	}

	/* move vertex to gpu */

	if (!pvrrc.isRenderFramebuffer)
	{
		/* (a frame decoded on the GPU is there already: DrawStrips() draws from the decode's buffers) */
		if (!pvrrc.gpu)
		{
			/* Main VBO, but where the frame was parsed into the buffers the card reads */
			if (gl_frame_vertex_buffer(0) == 0)
			{
				glBindBuffer(GL_ARRAY_BUFFER, gl.vbo.geometry); glCheck();
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.vbo.idxs); glCheck();

				glBufferData(GL_ARRAY_BUFFER,TA_LIST_BYTES(pvrrc.verts),TA_LIST_HEAD(pvrrc.verts, Vertex),GL_STREAM_DRAW); glCheck();

				upload_vertex_indices();
			}

			/* Modvol VBO (but where they were parsed into the buffers the card reads) */
			if (TA_LIST_USED(pvrrc.modtrig, ModTriangle) && !TA_LIST_USED(pvrrc.mv_clip, ModTriangle) && gl_frame_modvol_buffer(0) == 0)
			{
				glBindBuffer(GL_ARRAY_BUFFER, gl.vbo.modvols); glCheck();
				glBufferData(GL_ARRAY_BUFFER,TA_LIST_BYTES(pvrrc.modtrig),TA_LIST_HEAD(pvrrc.modtrig, ModTriangle),GL_STREAM_DRAW); glCheck();
			}
		}
		/* the volumes' triangles with those far off the screen cut (rend/modvol_clip.h), however the frame was decoded */
		if (TA_LIST_USED(pvrrc.mv_clip, ModTriangle) && gl_frame_modvol_buffer(0) == 0)
		{
			glBindBuffer(GL_ARRAY_BUFFER, gl.vbo.modvols); glCheck();
			glBufferData(GL_ARRAY_BUFFER, TA_LIST_BYTES(pvrrc.mv_clip), TA_LIST_HEAD(pvrrc.mv_clip, ModTriangle), GL_STREAM_DRAW); glCheck();
		}

		if (!wide_screen_on)
		{
			float width;
			float height;
			float min_x;
			float min_y;
			if (!is_rtt)
			{
				min_x = xform_x(scissor_mat, pvrrc.fb_X_CLIP.min);
				min_y = xform_y(scissor_mat, pvrrc.fb_Y_CLIP.min);
				width = xform_w(scissor_mat, pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1);
				height = xform_h(scissor_mat, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
				if (width < 0)
				{
					min_x += width;
					width = -width;
				}
				if (height < 0)
				{
					min_y += height;
					height = -height;
				}

				if (matrices.sidebar_width > 0)
				{
					float rounded_offs_x = matrices.sidebar_width + 0.5f;

					glc_clear_color(0.f, 0.f, 0.f, 0.f);
					glc_enable(GL_SCISSOR_TEST);
					glc_scissor(0, 0, rounded_offs_x, screen_height);
					glClear(GL_COLOR_BUFFER_BIT);
					glc_scissor(screen_width - rounded_offs_x, 0, rounded_offs_x, screen_height);
					glClear(GL_COLOR_BUFFER_BIT);
				}
			}
			else
			{
				width = pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1;
				height = pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1;
				min_x = pvrrc.fb_X_CLIP.min;
				min_y = pvrrc.fb_Y_CLIP.min;
				if (settings.rend.RenderToTextureUpscale > 1)
				{
					min_x *= settings.rend.RenderToTextureUpscale;
					min_y *= settings.rend.RenderToTextureUpscale;
					width *= settings.rend.RenderToTextureUpscale;
					height *= settings.rend.RenderToTextureUpscale;
				}
			}

			ShaderUniforms.base_clipping.enabled = true;
			ShaderUniforms.base_clipping.x = (int)gl_round_away(min_x);
			ShaderUniforms.base_clipping.y = (int)gl_round_away(min_y);
			ShaderUniforms.base_clipping.width = (int)gl_round_away(width);
			ShaderUniforms.base_clipping.height = (int)gl_round_away(height);
			glc_scissor(ShaderUniforms.base_clipping.x, ShaderUniforms.base_clipping.y, ShaderUniforms.base_clipping.width, ShaderUniforms.base_clipping.height);
			glc_enable(GL_SCISSOR_TEST);
		}
		else
		{
			ShaderUniforms.base_clipping.enabled = false;
		}

		DrawStrips();
		/* (the chip's write-out, where the option asks for it, in place: gl_writeout.h) */
		if (!is_rtt && own_target && own_color_texture)
		{
			gl_writeout_pass(own_color, screen_width, screen_height);
			glBindFramebuffer(RARCH_GL_FRAMEBUFFER, own_fbo);
		}
	}
	else
	{
		glc_clear_color(0.f, 0.f, 0.f, 0.f);
		glClear(GL_COLOR_BUFFER_BIT);
		glBindBuffer(GL_ARRAY_BUFFER, gl.vbo.geometry); glCheck();
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.vbo.idxs); glCheck();
		DrawFramebuffer();
	}

	/* VMU/Crosshair code - libretro-specific */
	if (!is_rtt)
	{
		if (settings.System == DC_PLATFORM_DREAMCAST)
		{
			for ( vmu_screen_number = 0 ; vmu_screen_number < 4 ; vmu_screen_number++)
				if ( vmu_screen_params[vmu_screen_number].vmu_screen_display )
					DrawVmuTexture(vmu_screen_number);
		}

		for ( lightgun_port = 0 ; lightgun_port < 4 ; lightgun_port++)
			DrawGunCrosshair(lightgun_port);
	}

	KillTex = false;

	if (own_target)
		own_target_present();
	if (is_rtt)
		ReadRTTBuffer();
	/* (the frontend's own drawing has the context as it was) */
	if (gl.clipControl != NULL)
		gl.clipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);

	return !is_rtt;
}

void rend_set_fb_scale(float x, float y)
{
	fb_scale_x = x;
	fb_scale_y = y;
}

/* Where a frame is parsed when the card can draw it from there: the
 * parse's arena, made the first time a frame could go there */
#ifdef HAVE_GL4
#include "parse_arena_gl.h"
static parse_arena_gl_t parse_arena;
static bool parse_arena_tried;
#endif

/* (the buffer of part @part of the arena's the frame was parsed into, or @own) */
static GLuint frame_buffer(unsigned part, GLuint own)
{
#ifdef HAVE_GL4
	const GLuint b = parse_arena_gl_buffer(&parse_arena, part);
	if (b != 0)
		return b;
#endif
	return own;
}

GLuint gl_frame_vertex_buffer(GLuint own)
{
	return frame_buffer(PARSE_ARENA_VERTS, own);
}

GLuint gl_frame_index_buffer(GLuint own)
{
	return frame_buffer(PARSE_ARENA_IDX, own);
}

GLuint gl_frame_modvol_buffer(GLuint own)
{
	return frame_buffer(TA_LIST_USED(pvrrc.mv_clip, ModTriangle) != 0 ? PARSE_ARENA_MV_CLIP : PARSE_ARENA_MODTRIG, own);
}

#ifdef HAVE_GL4
/* renders parsed into the arena, and copied for want of a free slot */
static u32 parsed_here, parsed_copied;
#endif

u32 *gl_frame_sorted(size_t bytes, size_t *offset)
{
#ifdef HAVE_GL4
	return (u32 *)parse_arena_gl_sorted(&parse_arena, bytes, offset);
#else
	return NULL;
#endif
}

void gl_frame_drawn(void)
{
#ifdef HAVE_GL4
	parse_arena_gl_done(&parse_arena);
#endif
}

void gl_frame_buffers_term(void)
{
#ifdef HAVE_GL4
	if (parse_arena.ready)
	{
		NOTICE_LOG(RENDERER, "OpenGL: %u renders parsed where the card reads them, %u copied there\n", parsed_here, parsed_copied);
		parse_arena_gl_term(&parse_arena);
	}
	parsed_here = parsed_copied = 0;
	parse_arena_tried = false;
#endif
}

/* The frame's lists into a slot of the arena, where one is free */
static void parse_where_drawn(TA_context* ctx)
{
	ta_list_t *lists[PARSE_ARENA_PARTS];
#ifdef HAVE_GL4
	if (!gl.buffer_storage || gl.index_type != GL_UNSIGNED_INT)
		return;
	if (!parse_arena_tried)
	{
		size_t room[PARSE_ARENA_PARTS];
		parse_arena_tried = true;
		tactx_arena_rooms(room);
		if (parse_arena_gl_init(&parse_arena, room, 3))
			NOTICE_LOG(RENDERER, "OpenGL: renders parsed where the card reads them, not copied there\n");
		else
			INFO_LOG(RENDERER, "OpenGL: renders copied to the card (the buffers for them could not be had, or read slowly)\n");
	}
	/* (four vertices at the head the parse keeps: the background's, FillBGP()'s) */
	tactx_arena_lists(ctx, lists);
	if (parse_arena_gl_attach(&parse_arena, lists, 4 * sizeof(Vertex)))
		parsed_here++;
	else if (parse_arena.ready)
	{
		/* (no slot whose frame the card is done with: a frame of more
		 * renders than slots, the card behind - never waited for) */
		if (parsed_copied++ == 0)
			NOTICE_LOG(RENDERER, "OpenGL: a render copied to the card, no slot of the parse's free\n");
	}
#endif
}

static void parse_not_drawn(void)
{
#ifdef HAVE_GL4
	parse_arena_gl_cancel(&parse_arena);
#endif
}

bool ProcessFrame(TA_context* ctx, u32 taFlags)
{
   if (KillTex)
   {
      gl_texcache_clear();
      INFO_LOG(RENDERER, "Texture cache cleared\n");
   }

   gltad_begin_frame();
   /* the modifier volumes' triangles far off the screen cut before the GPU clips them (rend/modvol_clip.h) */
   ta_set_modvol_clip(true);
   if (ctx->rend.isRenderFramebuffer)
		RenderFramebuffer();
	else
	{
		/* GPU-side TA processing: the header pass makes the frame ready to
		 * be drawn from the GPU's decode, in the full parse's place, and the
		 * decode is dispatched; or it is parsed as ever */
		bool rv;
		if (taFlags == 0 || !gltad_ready() || !gltad_prepare(ctx, taFlags, &rv))
		{
			parse_where_drawn(ctx);
			rv = ta_parse_vdrc(ctx);
		}
		else if (rv)
			gltad_record(ctx);
		if (!rv || ctx->rend.Overrun)
			parse_not_drawn();
		if (!rv)
			return false;
		/* the textures the frame's polygons asked for that are made on the GPU, before the draws */
		gl_tex_flush();
	}
   gl_texcache_collect_cleanup();

   return !ctx->rend.Overrun;
}

static bool gles_init(void)
{
	if (!gl_create_resources())
		return false;

	glc_enable_cache();
	/* A new context has dithering on, and a driver may then dither what
	 * is drawn (to a target of less than eight bits a channel, as a rule).
	 * Nothing here wants it: what the console dithers is dithered when
	 * the picture is packed for video memory. */
	glDisable(GL_DITHER);

#ifdef HAVE_OPENGLES
	glHint(GL_GENERATE_MIPMAP_HINT, GL_FASTEST);
#endif

#ifdef HAVE_TEXUPSCALE
	if (settings.rend.TextureUpscale > 1)
	{
		/* Trick to preload the tables used by xBRZ */
		u32 src[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
		u32 dst[16];
		UpscalexBRZ(2, src, dst, 2, 2, false);
	}
#endif
	palette_updated = true;
	gl_texcache_clear();
	/* the textures' pool and the decoder, all of them now (gl_texdecoder.h) */
	gl_tex_init(true);

	/* GPU-side TA processing, if it is on and the context has OpenGL 4.3:
	 * here, when the renderer is set up, not in a frame */
	gltad_want_sort(true);
	gltad_setup(ta_gpu_wanted());

	/* the polygons' programs, so that none is made in a frame (glprogram_cache.h) */
	gl_make_programs();
	gl_writeout_init(screen_width, screen_height, false);
	glprog_save(false);
	glprog_started();

	return true;
}

static void gles_resize(int w, int h)
{
	gl_carry_picture(screen_width, screen_height, w, h);
	screen_width = w;
	screen_height = h;
	/* (the write-out's picture, of the new size: gl_writeout.h) */
	gl_writeout_init(w, h, false);
}

static void gles_term(void)
{
	/* (what is new kept, while the programs are there: glprogram_cache.h) */
	glprog_log("per triangle");
	glprog_close();
	gltad_term();
	ta_set_modvol_clip(false);
	gl_writeout_term();
	gl_texcache_clear();
	rtt_watch_term();
	gl_tex_term();

	gl_term();
}

static bool gles_process(TA_context *ctx)
{
#if !defined(TARGET_NO_THREADS)
	if (!settings.rend.ThreadedRendering)
#endif
		glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
	/* GPU-side TA processing: a sorted pass's translucent triangles
	 * sorted from the frame's depths (TA_GPU_SORT); sorted by strip, the
	 * polygons are sorted from the CPU's vertices, and a frame that may
	 * have a sorted pass is parsed. The polygons drawn again for shadows
	 * are those the GPU says reach into the volumes. */
	return ProcessFrame(ctx, TA_GPU_DRAW | TA_GPU_SHADOWS
			| (!settings.pvr.Emulation.AlphaSortMode && gltad_can_sort() ? TA_GPU_SORT : TA_GPU_NO_SORT));
}

static bool gles_render(void)
{
	const bool ret = RenderFrame();
	gl_frame_drawn();
	gltad_end_frame();
#if !defined(TARGET_NO_THREADS)
	if (!settings.rend.ThreadedRendering)
#endif
		glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
	return ret;
}

/* (TA processing switched on or off: between frames, the state bound) */
static void gles_options_changed(void)
{
	gltad_setup(ta_gpu_wanted());
}

const renderer_ops gles_renderer_ops =
{
	gles_init,
	gles_resize,
	gles_term,
	gles_process,
	gles_render,
	gl_keep_picture,
	gl_get_texture,
	gles_options_changed
};
