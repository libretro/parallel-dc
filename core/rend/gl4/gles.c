#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
#include <math.h>
#include "rend/rtt_watch.h"

#include <stdio.h>
#include "gl4.h"
#include "rend/gles/ta_decoder_gl.h"
#include "rend/ta_gpu.h"
#include "rend/gles/glprogram_cache.h"

gl4_ctx gl4;

gl4ShaderUniforms_t gl4ShaderUniforms;

#define FOG_CHANNEL "r"
#define FOG_IMG_TYPE GL_RED

/* Fragment and vertex shaders code */

static const char* VertexShaderSource = "#version 430\n"
"#define pp_Gouraud %d\n"
"\n"
"#if pp_Gouraud == 0\n"
"#define INTERPOLATION flat\n"
"#else\n"
"#define INTERPOLATION smooth\n"
"#endif\n"
"\n"
"/* The depth is the position's (rend/depth_scale.h; gles/gles.cpp has the\n"
"   same): a vertex goes out at (x*w, y*w, a*S + b*w, w) with w = 1/z, a\n"
"   depth of a*S*z + b, 1/w as the chip has it. depth_scale is (a, b, -, S),\n"
"   S = 1/fZ_max, in the uniform buffer a frame decoded on the GPU writes.\n"
"   The same text places the vertices of every program, and the position is\n"
"   invariant: a pass that tests EQUAL against another's depth is exact. */\n"
"invariant gl_Position;\n"
"layout(std140, binding = 0) uniform DepthScaleBlock { highp vec4 depth_scale; };\n"
"/* Vertex constants*/ \n"
"uniform highp mat4 normal_matrix;\n"
"/* Vertex input */\n"
"in vec4 in_pos;\n"
"in vec4 in_base;\n"
"in vec4 in_offs;\n"
"in vec2 in_uv;\n"
"in vec4 in_base1;\n"
"in vec4 in_offs1;\n"
"in vec2 in_uv1;\n"
"// a translucent strip's number, of a frame decoded on the GPU (TA_GPU_INSTANCE); 0 for the others\n"
"in uint in_pp;\n"
"/* output */\n"
"INTERPOLATION out vec4 vtx_base;\n"
"INTERPOLATION out vec4 vtx_offs;\n"
"			  out vec2 vtx_uv;\n"
"INTERPOLATION out vec4 vtx_base1;\n"
"INTERPOLATION out vec4 vtx_offs1;\n"
"			  out vec2 vtx_uv1;\n"
"flat out uint vtx_pp;\n"
"void main()\n"
"{\n"
"	vtx_pp = in_pp;\n"
"	vtx_base = in_base;\n"
"	vtx_offs = in_offs;\n"
"	vtx_uv = in_uv;\n"
"	vtx_base1 = in_base1;\n"
"	vtx_offs1 = in_offs1;\n"
"	vtx_uv1 = in_uv1;\n"
"	vec4 vpos = normal_matrix * in_pos;\n"
"	vpos.w = 1.0 / vpos.z;\n"
"	vpos.xy *= vpos.w;\n"
"	// (a z beyond fZ_max is at the nearest depth, not clipped)\n"
"	vpos.z = min(depth_scale.x * depth_scale.w + depth_scale.y * vpos.w, vpos.w);\n"
"	gl_Position = vpos;\n"
"}\n";

const char* gl4PixelPipelineShader = SHADER_HEADER
"\n"
"#define cp_AlphaTest %d\n"
"#define pp_ClipInside %d\n"
"#define pp_UseAlpha %d\n"
"#define pp_Texture %d\n"
"#define pp_IgnoreTexA %d\n"
"#define pp_ShadInstr %d\n"
"#define pp_Offset %d\n"
"#define pp_FogCtrl %d\n"
"#define pp_TwoVolumes %d\n"
"#define pp_Gouraud %d\n"
"#define pp_BumpMap %d\n"
"#define FogClamping %d\n"
"#define pp_Palette %d\n"
"#define PASS %d\n"
"#define GENERAL_PROGRAM %d\n"
"#define PI 3.1415926\n"
"\n"
"/* The general programs (rend/gles/glprogram_cache.h): one for each PASS and\n"
" * pp_Gouraud, the other bits of a polygon's key (gl4GetProgram()) read\n"
" * from a uniform. The code below is the same for both, so what one draws\n"
" * the other draws. */\n"
"#if GENERAL_PROGRAM == 1\n"
"uniform int key_bits;\n"
"#undef cp_AlphaTest\n"
"#undef pp_ClipInside\n"
"#undef pp_UseAlpha\n"
"#undef pp_Texture\n"
"#undef pp_IgnoreTexA\n"
"#undef pp_ShadInstr\n"
"#undef pp_Offset\n"
"#undef pp_FogCtrl\n"
"#undef pp_TwoVolumes\n"
"#undef pp_BumpMap\n"
"#undef FogClamping\n"
"#undef pp_Palette\n"
"#define pp_ClipInside ((key_bits >> 16) & 1)\n"
"#define cp_AlphaTest ((key_bits >> 15) & 1)\n"
"#define pp_Texture ((key_bits >> 14) & 1)\n"
"#define pp_UseAlpha ((key_bits >> 13) & 1)\n"
"#define pp_IgnoreTexA ((key_bits >> 12) & 1)\n"
"#define pp_ShadInstr ((key_bits >> 10) & 3)\n"
"#define pp_Offset ((key_bits >> 9) & 1)\n"
"#define pp_FogCtrl ((key_bits >> 7) & 3)\n"
"#define pp_TwoVolumes ((key_bits >> 6) & 1)\n"
"#define pp_BumpMap ((key_bits >> 4) & 1)\n"
"#define FogClamping ((key_bits >> 3) & 1)\n"
"#define pp_Palette ((key_bits >> 2) & 1)\n"
"#endif\n"
"\n"
"#define PASS_DEPTH 0\n"
"#define PASS_COLOR 1\n"
"#define PASS_OIT 2\n"
"\n"
"#if PASS == PASS_DEPTH || PASS == PASS_COLOR\n"
"	out vec4 FragColor;\n"
"#endif\n"
"\n"
"#if pp_Gouraud == 0\n"
"#define INTERPOLATION flat\n"
"#else\n"
"#define INTERPOLATION smooth\n"
"#endif\n"
"\n"
"/* Shader program params*/\n"
"uniform float cp_AlphaTestValue;\n"
"uniform vec4 pp_ClipTest;\n"
"uniform vec3 sp_FOG_COL_RAM,sp_FOG_COL_VERT;\n"
"uniform float sp_FOG_DENSITY;\n"
"uniform float shade_scale_factor;\n"
"uniform sampler2D tex0, tex1;\n"
"layout(binding = 5) uniform sampler2D fog_table;\n"
"uniform int pp_Number;\n"
"uniform usampler2D shadow_stencil;\n"
"uniform sampler2D DepthTex;\n"
"uniform float trilinear_alpha;\n"
"uniform vec4 fog_clamp_min;\n"
"uniform vec4 fog_clamp_max;\n"
"uniform sampler2D palette;\n"
"uniform float palette_index;\n"
"\n"
"uniform ivec2 blend_mode[2];\n"
"uniform bool use_alpha[2];\n"
"uniform bool ignore_tex_alpha[2];\n"
"uniform int shading_instr[2];\n"
"uniform int fog_control[2];\n"
"\n"
"/* Vertex input*/\n"
"INTERPOLATION in vec4 vtx_base;\n"
"INTERPOLATION in vec4 vtx_offs;\n"
"			  in vec2 vtx_uv;\n"
"INTERPOLATION in vec4 vtx_base1;\n"
"INTERPOLATION in vec4 vtx_offs1;\n"
"			  in vec2 vtx_uv1;\n"
"flat in uint vtx_pp;\n"
"\n"
"// The fog coefficient for a depth, worked out the way the PowerVR2 does it.\n"
"// The depth times the fog density, between 1 and just under 256, is taken\n"
"// apart as a float: its exponent and the top four bits of its mantissa\n"
"// pick one of the 128 table entries, and the next eight bits of mantissa\n"
"// blend that entry with the following one. The blend is done in whole\n"
"// numbers out of 256 and so is the result, which is returned over 256.\n"
"// Splitting the float is done by comparing and halving, which is exact\n"
"// and costs less than the log2() and pow() this used to take.\n"
"float fog_mode2(float w)\n"
"{\n"
"	float z = w * sp_FOG_DENSITY;\n"
"	// Not clamp(): where the depth is infinite and the density zero their\n"
"	// product is not a number, and that has to come out as 1 like anything\n"
"	// else below 1. The texture lookup this used to end in swallowed it.\n"
"	if (!(z >= 1.0))\n"
"		z = 1.0;\n"
"	if (z > 255.9999)\n"
"		z = 255.9999;\n"
"	float e = 0.0;\n"
"	if (z >= 16.0) { z *= 0.0625; e = 4.0; }\n"
"	if (z >= 4.0) { z *= 0.25; e += 2.0; }\n"
"	if (z >= 2.0) { z *= 0.5; e += 1.0; }\n"
"	float t = (z - 1.0) * 16.0;\n"
"	float i = floor(t);\n"
"	float blend = floor((t - i) * 256.0);\n"
"	float u = (e * 16.0 + i + 0.5) / 128.0;\n"
"	// The table is two rows: the entry's own value under 0.75, the next entry's under 0.25\n"
"	float next = floor(texture(fog_table, vec2(u, 0.25)).r * 255.0 + 0.5);\n"
"	float cur = floor(texture(fog_table, vec2(u, 0.75)).r * 255.0 + 0.5);\n"
"	return floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;\n"
"}\n"
"\n"
"vec4 fog_clamp(vec4 col)\n"
"{\n"
"	if (FogClamping == 1)\n"
"		return clamp(col, fog_clamp_min, fog_clamp_max);\n"
"	return col;\n"
"}\n"
"\n"
"vec4 palettePixel(sampler2D tex, vec2 coords)\n"
"{\n"
"	// palette_index is where the middle of the palette's first colour is in the row of 1024\n"
"	vec4 c = vec4(texture(tex, coords).r * (255.0 / 1024.0) + palette_index, 0.5, 0.0, 0.0);\n"
"	return texture(palette, c.xy);\n"
"}\n"
"\n"
"void main()\n"
"{\n"
"   #if PASS == PASS_OIT\n"
"      // Manual depth testing\n"
"      float frontDepth = texture(DepthTex, gl_FragCoord.xy / textureSize(DepthTex, 0)).r;\n"
"      if (FRAG_DEPTH < frontDepth)\n"
"         discard;\n"
"   #endif\n"
"  \n"
"	// Clip inside the box\n"
"	if (pp_ClipInside == 1)\n"
"	{\n"
"		if (gl_FragCoord.x >= pp_ClipTest.x && gl_FragCoord.x <= pp_ClipTest.z\n"
"				&& gl_FragCoord.y >= pp_ClipTest.y && gl_FragCoord.y <= pp_ClipTest.w)\n"
"			discard;\n"
"	}\n"
"	\n"
"   vec4 color = vtx_base;\n"
"   vec4 offset = vtx_offs;\n"
"   vec2 uv = vtx_uv;\n"
"   bool area1 = false;\n"
"   ivec2 cur_blend_mode = blend_mode[0];\n"
"   // (with one volume, the polygon's own; with two, the first's unless the pixel is in the second)\n"
"   bool cur_use_alpha = pp_UseAlpha == 1;\n"
"   bool cur_ignore_tex_alpha = pp_IgnoreTexA == 1;\n"
"   int cur_shading_instr = pp_ShadInstr;\n"
"   int cur_fog_control = pp_FogCtrl;\n"
"	\n"
"	if (pp_TwoVolumes == 1)\n"
"	{\n"
"      cur_use_alpha = use_alpha[0];\n"
"      cur_ignore_tex_alpha = ignore_tex_alpha[0];\n"
"      cur_shading_instr = shading_instr[0];\n"
"      cur_fog_control = fog_control[0];\n"
"      #if PASS == PASS_COLOR\n"
"			uvec4 stencil = texture(shadow_stencil, gl_FragCoord.xy / textureSize(shadow_stencil, 0));\n"
"			if (stencil.r == 0x81u) {\n"
"				color = vtx_base1;\n"
"				offset = vtx_offs1;\n"
"				uv = vtx_uv1;\n"
"				area1 = true;\n"
"            cur_blend_mode = blend_mode[1];\n"
"            cur_use_alpha = use_alpha[1];\n"
"            cur_ignore_tex_alpha = ignore_tex_alpha[1];\n"
"            cur_shading_instr = shading_instr[1];\n"
"            cur_fog_control = fog_control[1];\n"
"			}\n"
"      #endif\n"
"	}\n"
"	\n"
"	#if PASS == PASS_COLOR\n"
"	else\n"
"	{\n"
"		// Inside a modifier volume the PowerVR2 scales what the polygon is shaded\n"
"		// with, its base and offset colours, before the texture is combined with\n"
"		// them. Scaling the combined colour instead is the same thing for a\n"
"		// modulated texture, but also darkens a decal texture, which the\n"
"		// hardware leaves alone.\n"
"		uvec4 stencil = texture(shadow_stencil, gl_FragCoord.xy / textureSize(shadow_stencil, 0));\n"
"		if (stencil.r == 0x81u) {\n"
"			color.rgb *= shade_scale_factor;\n"
"			offset.rgb *= shade_scale_factor;\n"
"		}\n"
"	}\n"
"	#endif\n"
"	if (!cur_use_alpha)\n"
"		color.a=1.0;\n"
"	if (pp_Texture==1)\n"
"	{\n"
"      vec4 texcol;\n"
"      if (pp_Palette == 0)\n"
"      {\n"
"         if (area1)\n"
"            texcol = texture(tex1, uv);\n"
"         else\n"
"            texcol = texture(tex0, uv);\n"
"      }\n"
"      else\n"
"      {\n"
"         // only the first volume's texture is ever one of palette indices\n"
"         if (area1)\n"
"            texcol = texture(tex1, uv);\n"
"         else\n"
"            texcol = palettePixel(tex0, uv);\n"
"      }\n"
"\n"
"      if (pp_BumpMap == 1)\n"
"      {\n"
"			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;\n"
"			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;\n"
"			texcol.a = clamp(vtx_offs.a + vtx_offs.r * sin(s) + vtx_offs.g * cos(s) * cos(r - 2.0 * PI * vtx_offs.b), 0.0, 1.0);\n"
"			texcol.rgb = vec3(1.0, 1.0, 1.0);	\n"
"      }\n"
"      else if (cur_ignore_tex_alpha)\n"
"			texcol.a=1.0;	\n"
"      if (cur_shading_instr == 0) // DECAL\n"
"		{\n"
"         color=texcol;\n"
"		}\n"
"      if (cur_shading_instr == 1) // MODULATE\n"
"		{\n"
"			color.rgb*=texcol.rgb;\n"
"			color.a=texcol.a;\n"
"		}\n"
"      if (cur_shading_instr == 2) // DECAL ALPHA\n"
"		{\n"
"			color.rgb=mix(color.rgb,texcol.rgb,texcol.a);\n"
"		}\n"
"      if (cur_shading_instr == 3) // MODULATE ALPHA\n"
"		{\n"
"			color*=texcol;\n"
"		}\n"
"		\n"
"		if (pp_Offset==1 && pp_BumpMap == 0)\n"
"		{\n"
"         color.rgb += offset.rgb;\n"
"		}\n"
"	}\n"
"   \n"
"	color = fog_clamp(color);\n"
"	\n"
"	// Look-up table mode 2: the fog colour takes the place of the pixel's\n"
"	// colour and the fog coefficient the place of its alpha. This is a mode\n"
"	// of the fog unit, which comes after the texture has been combined in,\n"
"	// so it is the combined colour that is replaced: a textured polygon\n"
"	// comes out in plain fog colour. It used to be done to the base colour,\n"
"	// before texturing, and the texture was then applied on top.\n"
"	if (cur_fog_control == 3) // LUT Mode 2\n"
"		color = vec4(sp_FOG_COL_RAM.rgb, fog_mode2(gl_FragCoord.w) * (256.0 / 255.0));\n"
"	if (cur_fog_control == 0) // LUT\n"
"	{\n"
"		float fog = fog_mode2(gl_FragCoord.w);\n"
"		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;\n"
"	}\n"
"	if (pp_Offset==1 && pp_BumpMap == 0 && cur_fog_control == 1)  // Per vertex\n"
"	{\n"
"		float fog = offset.a * (255.0 / 256.0);\n"
"		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;\n"
"	}\n"
"   \n"
"   color *= trilinear_alpha;\n"
"	\n"
"	// Punch-through polygons are tested on the alpha of the finished colour,\n"
"	// after the texture and the vertex colours are combined and clamped.\n"
"	if (cp_AlphaTest == 1)\n"
"	{\n"
"      if (cp_AlphaTestValue > color.a)\n"
"         discard;\n"
"      color.a=1.0;\n"
"	}\n"
"  \n"
"   //color.rgb=vec3(gl_FragCoord.w * sp_FOG_DENSITY / 128.0);\n"
"	\n"
"	#if PASS == PASS_COLOR \n"
"      FragColor = color;\n"
"	#elif PASS == PASS_OIT\n"
"      // Discard as many pixels as possible\n"
"      switch (cur_blend_mode.y) // DST\n"
"		{\n"
"		case ONE:\n"
"         switch (cur_blend_mode.x) // SRC\n"
"			{\n"
"				case ZERO:\n"
"               discard;\n"
"				case ONE:\n"
"				case OTHER_COLOR:\n"
"				case INVERSE_OTHER_COLOR:\n"
"               if (color == vec4(0.0))\n"
"                  discard;\n"
"					break;\n"
"				case SRC_ALPHA:\n"
"               if (color.a == 0.0 || color.rgb == vec3(0.0))\n"
"                  discard;\n"
"               break;\n"
"				case INVERSE_SRC_ALPHA:\n"
"               if (color.a == 1.0 || color.rgb == vec3(0.0))\n"
"                  discard;\n"
"					break;\n"
"			}\n"
"			break;\n"
"		// Destination times a white source is not left alone when the\n"
"		// fragment turns out to be in a shadow: its colour is scaled then.\n"
"		case INVERSE_OTHER_COLOR:\n"
"         if (cur_blend_mode.x <= SRC_ALPHA && color == vec4(0.0))\n"
"            discard;\n"
"			break;\n"
"		case SRC_ALPHA:\n"
"         if ((cur_blend_mode.x == ZERO || cur_blend_mode.x == INVERSE_SRC_ALPHA) && color.a == 1.0)\n"
"            discard;\n"
"			break;\n"
"		case INVERSE_SRC_ALPHA:\n"
"         switch (cur_blend_mode.x) // SRC\n"
"			{\n"
"				case ZERO:\n"
"				case SRC_ALPHA:\n"
"               if (color.a == 0.0)\n"
"                  discard;\n"
"					break;\n"
"				case ONE:\n"
"				case OTHER_COLOR:\n"
"				case INVERSE_OTHER_COLOR:\n"
"               if (color == vec4(0.0))\n"
"                  discard;\n"
"					break;\n"
"			}\n"
"			break;\n"
"		}\n"
"		\n"
"      ivec2 coords = ivec2(gl_FragCoord.xy);\n"
"      uint idx =  getNextPixelIndex();\n"
"      \n"
"      Pixel pixel;\n"
"      pixel.color = packUnorm4x8(color);\n"
"      pixel.depth = FRAG_DEPTH;\n"
"      pixel.seq_num = uint(pp_Number) + vtx_pp;\n"
"      pixel.next = imageAtomicExchange(abufferPointerImg, coords, idx);\n"
"      pixels[idx] = pixel;\n"
"     \n"
"      discard;\n"
"		\n"
"	#endif\n"
"}\n";

/* (the depth is the position's; nothing is shaded) */
static const char* ModifierVolumeShader = SHADER_HEADER
"\n"
"void main()\n"
"{\n"
"}\n";

int max_image_width;
int max_image_height;

/* The shaders' text, made from their sources (the renderer's thread's: the
 * driver keeps its own copy, glShaderSource()) */
static char gl4_vshader[16384], gl4_fshader[16384];

/* A program of @pixel_source (the polygons' by default) for @s: compiled
 * now (@background: asked of the driver, not waited for; @general: the
 * general one of its pass and pp_Gouraud) */
static GLuint make_program(const gl4PipelineShader* s, const char *pixel_source, const char *vertex_source, bool general, bool background)
{
	const char *vfmt = vertex_source == NULL ? VertexShaderSource : vertex_source;
	/* (at most one %d, and fifteen) */
	verify(strlen(vfmt) + 11 < sizeof(gl4_vshader) && strlen(pixel_source) + 15 * 11 < sizeof(gl4_fshader));
	sprintf(gl4_vshader, vfmt, s->pp_Gouraud);
	/* (the other shaders made with this have no GENERAL_PROGRAM: the last number is not used) */
	sprintf(gl4_fshader, pixel_source,
			s->cp_AlphaTest, s->pp_InsideClipping, s->pp_UseAlpha,
			s->pp_Texture, s->pp_IgnoreTexA, s->pp_ShadInstr, s->pp_Offset, s->pp_FogCtrl,
			s->pp_TwoVolumes, s->pp_Gouraud, s->pp_BumpMap, s->fog_clamping, s->palette,
			s->pass, general ? 1 : 0);

	return glprog_make(gl4_vshader, gl4_fshader, background);
}

GLuint gl4MakePipelineProgram(const gl4PipelineShader* s, bool general, bool background)
{
	return make_program(s, gl4PixelPipelineShader, NULL, general, background);
}

void gl4PipelineLocations(gl4PipelineShader* s, bool general)
{
	GLint gu;
	glc_use_program(s->program);
	/* (a program just linked: none of its uniforms set yet) */
	s->ownUniforms.set = 0;
	s->uniforms = &s->ownUniforms;

	/* setup texture 0 as the input for the shader */
	gu = glGetUniformLocation(s->program, "tex0");
	if ((s->pp_Texture == 1 || general) && gu != -1)
		glUniform1i(gu, 0);
	/* Setup texture 1 as the input for area 1 in two volume mode */
	gu = glGetUniformLocation(s->program, "tex1");
	if ((s->pp_Texture == 1 || general) && gu != -1)
		glUniform1i(gu, 1);

	/* get the uniform locations */
	s->pp_ClipTest      = glGetUniformLocation(s->program, "pp_ClipTest");

	s->sp_FOG_DENSITY   = glGetUniformLocation(s->program, "sp_FOG_DENSITY");

	s->cp_AlphaTestValue= glGetUniformLocation(s->program, "cp_AlphaTestValue");

	/* FOG_COL_RAM,FOG_COL_VERT,FOG_DENSITY; */
	if ((s->pp_FogCtrl==1 && s->pp_Texture==1) || general)
		s->sp_FOG_COL_VERT = glGetUniformLocation(s->program, "sp_FOG_COL_VERT");
	else
		s->sp_FOG_COL_VERT = -1;
	if (s->pp_FogCtrl==0 || s->pp_FogCtrl==3 || general)
	{
		s->sp_FOG_COL_RAM=glGetUniformLocation(s->program, "sp_FOG_COL_RAM");
	}
	else
	{
		s->sp_FOG_COL_RAM=-1;
	}
	s->shade_scale_factor = glGetUniformLocation(s->program, "shade_scale_factor");

	/* Use texture 1 for depth texture */
	gu = glGetUniformLocation(s->program, "DepthTex");
	if (gu != -1)
		glUniform1i(gu, 2);		/* GL_TEXTURE2 */

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

	/* Shadow stencil for OP/PT rendering pass */
	gu = glGetUniformLocation(s->program, "shadow_stencil");
	if (gu != -1)
		glUniform1i(gu, 3);		/* GL_TEXTURE3 */

	s->pp_Number = glGetUniformLocation(s->program, "pp_Number");

	s->blend_mode = glGetUniformLocation(s->program, "blend_mode");
	s->use_alpha = glGetUniformLocation(s->program, "use_alpha");
	s->ignore_tex_alpha = glGetUniformLocation(s->program, "ignore_tex_alpha");
	s->shading_instr = glGetUniformLocation(s->program, "shading_instr");
	s->fog_control = glGetUniformLocation(s->program, "fog_control");

	gu = glGetUniformLocation(s->program, "palette");
	if (gu != -1)
		glUniform1i(gu, 6);		/* GL_TEXTURE6 */
	s->palette_index = glGetUniformLocation(s->program, "palette_index");

	s->key_bits = general ? glGetUniformLocation(s->program, "key_bits") : -1;
}

bool gl4CompilePipelineShader(gl4PipelineShader* s, const char *pixel_source, const char *vertex_source)
{
	s->program = make_program(s, pixel_source, vertex_source, false, false);
	if (glprog_failed())
		die("shader compile fail\n");
	gl4PipelineLocations(s, false);

	return glIsProgram(s->program)==GL_TRUE;
}

static void gl_term(void)
{
	u32 e;
	u64 key;
	int p, g;
	gl_frame_buffers_term();
	glDeleteProgram(gl4.modvol_shader.program);
	glDeleteVertexArrays(1, &gl4.vbo.main_vao);
	glDeleteVertexArrays(1, &gl4.vbo.modvol_vao);
	glDeleteBuffers(1, &gl4.vbo.geometry);
	glDeleteBuffers(1, &gl4.vbo.modvols);
	glDeleteBuffers(1, &gl4.vbo.idxs);
	glDeleteBuffers(1, &gl4.vbo.idxs2);
	glDeleteBuffers(1, &gl4.vbo.tr_poly_params);
	if (gl.depth_ubo != 0)
		glDeleteBuffers(1, &gl.depth_ubo);
	gl.depth_ubo = 0;
	if (gl4.shaders.elem != 0)
	{
		for (e = 0; e < tex_map_places(&gl4.shaders); e++)
		{
			const gl4PipelineShader *sh = (const gl4PipelineShader *)tex_map_at(&gl4.shaders, e, &key);
			if (sh == NULL)
				continue;
			/* (one drawn with a general program has none of its own, but may have one coming) */
			if (sh->program != 0 && sh->general == NULL)
				glDeleteProgram(sh->program);
			if (sh->pending != 0)
				glDeleteProgram(sh->pending);
		}
		tex_map_clear(&gl4.shaders);
	}
	for (p = 0; p < 3; p++)
		for (g = 0; g < 2; g++)
		{
			if (gl4.general[p][g].program != 0)
				glDeleteProgram(gl4.general[p][g].program);
			gl4.general[p][g].program = 0;
		}
	glc_delete_textures(1, &fbTextureId);
	fbTextureId = 0;
	glc_delete_textures(1, &fogTextureId);
	fogTextureId = 0;
	glc_delete_textures(1, &paletteTextureId);
	paletteTextureId = 0;
}

static bool gl_create_resources(void)
{
	glsm_ctx_proc_address_t proc;
	/* Create VAOs */
	glGenVertexArrays(1, &gl4.vbo.main_vao);
	glGenVertexArrays(1, &gl4.vbo.modvol_vao);

	/* Create VBOs */
	glGenBuffers(1, &gl4.vbo.geometry);
	glGenBuffers(1, &gl4.vbo.modvols);
	glGenBuffers(1, &gl4.vbo.idxs);
	glGenBuffers(1, &gl4.vbo.idxs2);
	/* (each its room now: gl_buffer_room()) */
	gl_buffer_room(gl4.vbo.geometry, GL_BUFFER_ROOM * 4);
	gl_buffer_room(gl4.vbo.modvols, GL_BUFFER_ROOM);
	gl_buffer_room(gl4.vbo.idxs, GL_BUFFER_ROOM);
	gl_buffer_room(gl4.vbo.idxs2, GL_BUFFER_ROOM);

	gl4SetupMainVBO(0, 0);
	gl4SetupModvolVBO(0);

	proc.addr = NULL;
	if (!glsm_ctl(GLSM_CTL_PROC_ADDRESS_GET, &proc) || proc.addr == NULL)
		return false;
	gl4.vertexAttribDivisor = (void (APIENTRYP)(GLuint, GLuint))proc.addr("glVertexAttribDivisor");
	gl4.vertexAttribI4ui = (void (APIENTRYP)(GLuint, GLuint, GLuint, GLuint, GLuint))proc.addr("glVertexAttribI4ui");
	if (gl4.vertexAttribDivisor == NULL || gl4.vertexAttribI4ui == NULL)
		return false;
	{
		TaGpuSizes sz;
		ta_gpu_sizes(&sz);
		/* (the most translucent polygons a frame can have) */
		gl4.trPolyMax = sz.list[2];
	}

	verify(strlen(VertexShaderSource) + 11 < sizeof(gl4_vshader));
	sprintf(gl4_vshader, VertexShaderSource, 1);

	gl4.modvol_shader.program=gl_CompileAndLink(gl4_vshader, ModifierVolumeShader);
	gl4.modvol_shader.normal_matrix  = glGetUniformLocation(gl4.modvol_shader.program, "normal_matrix");

	/* Create the buffer for Translucent poly params: as many as a frame can have, now */
	glGenBuffers(1, &gl4.vbo.tr_poly_params);
	gl_buffer_room(gl4.vbo.tr_poly_params, (size_t)gl4.trPolyMax * 8);
	/* Bind it */
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl4.vbo.tr_poly_params);
	/* Declare storage */
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gl4.vbo.tr_poly_params);
	glCheck();

	return true;
}

static void resize(int w, int h)
{
	/* (as large as a render of a screen to a texture, at the upscale renders
	 * to textures have, from the first: made while the game runs, for the
	 * first such render, otherwise) */
	const int scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	if (w < 640 * scale)
		w = 640 * scale;
	if (h < 480 * scale)
		h = 480 * scale;
	if (w > max_image_width || h > max_image_height || stencilTexId == 0)
	{
		if (w > max_image_width)
			max_image_width = w;
		if (h > max_image_height)
			max_image_height = h;

		if (stencilTexId != 0)
		{
			glc_delete_textures(1, &stencilTexId);
			stencilTexId = 0;
		}
		if (depthTexId != 0)
		{
			glc_delete_textures(1, &depthTexId);
			depthTexId = 0;
		}
		if (opaqueTexId != 0)
		{
			glc_delete_textures(1, &opaqueTexId);
			opaqueTexId = 0;
		}
		if (depthSaveTexId != 0)
		{
			glc_delete_textures(1, &depthSaveTexId);
			depthSaveTexId = 0;
		}
		gl4CreateTextures(max_image_width, max_image_height);
		reshapeABuffer(max_image_width, max_image_height);
	}
}

/* (lroundf()'s: half away from zero) */
static long gl4_round_away(float v)
{
	const double d = v;
	return d >= 0 ? (long)floor(d + 0.5) : -(long)floor(-d + 0.5);
}

static bool RenderFrame(void)
{
	bool is_rtt;
	int rendering_height;
	int rendering_width;
	u8*fog_colram_bgra;
	u8*fog_colvert_bgra;
	s32 fog_den_exp;
	float fog_den_mant;
	u8*fog_density;
	GLuint frontend_fbo, writeout_fbo;
	GLuint output_fbo;
	transform_matrix_t matrices;
	const xform *scissor_mat;
	int vmu_screen_number = 0 ;
	int lightgun_port = 0 ;

	DoCleanup();

	is_rtt = pvrrc.isRTT;
	/* A pixel clip that ends before it starts holds no pixel: the chip
	 * writes nothing. (Its size would be negative here, and a texture or
	 * buffer that size is none at all.) */
	if (is_rtt && (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min))
	{
		WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d\n",
				pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
		return false;
	}

	transform_matrix_init(&matrices, true);
	transform_matrix_calc(&matrices, &pvrrc);
	xform_to_mat4(&matrices.normal, gl4ShaderUniforms.normal_mat);
	scissor_mat = &matrices.scissor;
	ViewportMatrix = matrices.viewport;

	/* The depth is the position's (rend/depth_scale.h, and the vertex
	 * shader): a*S*z + b, with S = 1/fZ_max; a and b take the depth to the
	 * device's clip space - [0,1] with clip control, else [-1,1] - and undo
	 * what the matrix scales z by. For a frame decoded on the GPU, S is the
	 * decode's word, copied over the host's (the copy is ordered after the
	 * decode's kernels: gles/ta_decoder_gl.h). */
	{
		const float mul = 1.f / matrices.normal.sz;
		float coefs[4];
		coefs[0] = gl.clipControl != NULL ? mul : 2.f * mul;
		coefs[1] = gl.clipControl != NULL ? 0.f : -1.f;
		coefs[2] = 0.f;
		coefs[3] = pvr_depth_scale(pvrrc.fZ_max);
		glBindBuffer(GL_UNIFORM_BUFFER, gl.depth_ubo);
		glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(coefs), coefs);
		if (pvrrc.gpu)
		{
			glBindBuffer(GL_COPY_READ_BUFFER, gltad_draw()->frame);
			gl.copyBufferSubData(GL_COPY_READ_BUFFER, GL_UNIFORM_BUFFER, TAG_O_ZSCALE * 4, 12, 4);
			glBindBuffer(GL_COPY_READ_BUFFER, 0);
		}
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
		glBindBufferBase(GL_UNIFORM_BUFFER, DEPTH_UBO_BINDING, gl.depth_ubo);
		if (gl.clipControl != NULL)
			gl.clipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
	}

	if (!is_rtt)
		gcflip=0;
	else
		gcflip=1;

	/*
		Handle Dc to screen scaling
	*/

	if (is_rtt)
	{
		int scaling = settings.rend.RenderToTextureUpscale;
		rendering_width = matrices.dc_viewport.x * scaling;
		rendering_height = matrices.dc_viewport.y * scaling;
	}
	else
	{
		rendering_width = screen_width;
		rendering_height = screen_height;
	}
	resize(rendering_width, rendering_height);

	/* VERT and RAM fog color constants */
	fog_colvert_bgra = (u8*)&FOG_COL_VERT;
	fog_colram_bgra = (u8*)&FOG_COL_RAM;
	gl4ShaderUniforms.ps_FOG_COL_VERT[0]=fog_colvert_bgra[2]/255.0f;
	gl4ShaderUniforms.ps_FOG_COL_VERT[1]=fog_colvert_bgra[1]/255.0f;
	gl4ShaderUniforms.ps_FOG_COL_VERT[2]=fog_colvert_bgra[0]/255.0f;

	gl4ShaderUniforms.ps_FOG_COL_RAM[0]=fog_colram_bgra [2]/255.0f;
	gl4ShaderUniforms.ps_FOG_COL_RAM[1]=fog_colram_bgra [1]/255.0f;
	gl4ShaderUniforms.ps_FOG_COL_RAM[2]=fog_colram_bgra [0]/255.0f;

	/* Fog density constant */
	fog_density = (u8*)&FOG_DENSITY;
	fog_den_mant = fog_density[1]/128.0f;  /* bit 7 -> x. bit, so [6:0] -> fraction -> /128 */
	fog_den_exp = (s8)fog_density[0];
	gl4ShaderUniforms.fog_den_float = fog_den_mant * (float)pow(2.0, fog_den_exp) * settings.rend.ExtraDepthScale;

	gl4ShaderUniforms.fog_clamp_min[0] = ((pvrrc.fog_clamp_min >> 16) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_min[1] = ((pvrrc.fog_clamp_min >> 8) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_min[2] = ((pvrrc.fog_clamp_min >> 0) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_min[3] = ((pvrrc.fog_clamp_min >> 24) & 0xFF) / 255.0f;

	gl4ShaderUniforms.fog_clamp_max[0] = ((pvrrc.fog_clamp_max >> 16) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_max[1] = ((pvrrc.fog_clamp_max >> 8) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_max[2] = ((pvrrc.fog_clamp_max >> 0) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_max[3] = ((pvrrc.fog_clamp_max >> 24) & 0xFF) / 255.0f;

	UpdateFogTexture((u8 *)FOG_TABLE, GL_TEXTURE5, GL_RED);
	if (palette_updated)
	{
		UpdatePaletteTexture(GL_TEXTURE6);
		palette_updated = false;
	}


	glc_use_program(gl4.modvol_shader.program);

	glUniformMatrix4fv(gl4.modvol_shader.normal_matrix, 1, GL_FALSE, gl4ShaderUniforms.normal_mat);

	/* half a step under the reference: see the same in gles/gles.c */
	gl4ShaderUniforms.PT_ALPHA = ((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f;

	frontend_fbo = 0;
	writeout_fbo = 0;

	/* setup render target first */
	if (is_rtt)
	{
		GLuint channels = GL_RGBA, format = GL_UNSIGNED_BYTE;
		switch(FB_W_CTRL.fb_packmode)
		{
		case 0: /* 0x0   0555 KRGB 16 bit  (default)	Bit 15 is the value of fb_kval[7]. */
			channels=GL_RGBA;
			format=GL_UNSIGNED_BYTE;
			break;

		case 1: /* 0x1   565 RGB 16 bit */
			channels=GL_RGB;
			/* Eight bits a channel, as the other renderers have it: the
			 * console blends at eight bits and cuts the result down to five
			 * and six when it writes it to video memory, which is done when
			 * the picture is read back. Asked for as 5-6-5, a driver may
			 * give just that, and every blend then rounds to five bits. */
			format=GL_UNSIGNED_BYTE;
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
		output_fbo = gl4BindRTT(FB_W_SOF1 & VRAM_MASK, (u32)gl4_round_away(matrices.dc_viewport.x),
				(u32)gl4_round_away(matrices.dc_viewport.y), channels, format);
	}
	else
	{
		glViewport(0, 0, screen_width, screen_height);
		/* (the screen drawn in the write-out's picture, when there is one,
		 * then to the frontend's after its pass: gles/gl_writeout.h) */
		frontend_fbo = hw_render.get_current_framebuffer();
		writeout_fbo = gl_writeout_target(frontend_fbo, screen_width, screen_height);
		output_fbo = writeout_fbo != 0 ? writeout_fbo : frontend_fbo;
	}

	glc_disable(GL_SCISSOR_TEST);

	/* move vertex to gpu */
	if (!pvrrc.isRenderFramebuffer && pvrrc.gpu)
		/* (decoded on the GPU: there already) */
		gl4BindFrameBuffers(true, false);
	else if (!pvrrc.isRenderFramebuffer)
	{
		const PolyParam *pp;
		u32 trCount;
		u32 used;
		u32 *mapped, *words;
		u32 i;
		gl4BindFrameBuffers(false, false);
		/* Main VBO, but where the frame was parsed into the buffers the card reads */
		if (gl_frame_vertex_buffer(0) == 0)
		{
			glBindVertexArray(gl4.vbo.main_vao);
			glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.geometry); glCheck();
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl4.vbo.idxs); glCheck();

			glBufferData(GL_ARRAY_BUFFER,TA_LIST_BYTES(pvrrc.verts),TA_LIST_HEAD(pvrrc.verts, Vertex),GL_STREAM_DRAW); glCheck();

			glBufferData(GL_ELEMENT_ARRAY_BUFFER,TA_LIST_BYTES(pvrrc.idx),TA_LIST_HEAD(pvrrc.idx, u32),GL_STREAM_DRAW); glCheck();
		}

		/* Modvol VBO (but where they were parsed into the buffers the card reads) */
		if (TA_LIST_USED(pvrrc.modtrig, ModTriangle) && !TA_LIST_USED(pvrrc.mv_clip, ModTriangle) && gl_frame_modvol_buffer(0) == 0)
		{
			glBindVertexArray(gl4.vbo.modvol_vao);
			glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.modvols); glCheck();
			glBufferData(GL_ARRAY_BUFFER,TA_LIST_BYTES(pvrrc.modtrig),TA_LIST_HEAD(pvrrc.modtrig, ModTriangle),GL_STREAM_DRAW); glCheck();
		}

		/* The translucent polygons' words: two each (see PolyParam in
		 * SHADER_HEADER), made where the GPU reads them - the buffer's
		 * memory, new each frame and mapped - or, where it cannot be
		 * mapped, into memory kept for them and copied there */
		used = (u32)TA_LIST_USED(pvrrc.global_param_tr, PolyParam);
		trCount = used < gl4.trPolyMax ? used : gl4.trPolyMax;
		pp = TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl4.vbo.tr_poly_params);
		glBufferData(GL_SHADER_STORAGE_BUFFER, trCount * 8, NULL, GL_STREAM_DRAW);
		mapped = trCount == 0 ? NULL
				: (u32 *)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, trCount * 8, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
		words = mapped != NULL ? mapped : (u32 *)grow_buf_reserve(&gl4.trPolyWords, (size_t)trCount * 8 + 8);
		for (i = 0; words != NULL && i < trCount; i++, pp++, words += 2)
		{
			words[0] = (pp->tsp & 0xffff00c0) | ((pp->isp >> 16) & 0xe400) | ((pp->pcw >> 7) & 1)
					| (pp->tsp1 != (u32)-1 || pp->tcw1 != (u32)-1 ? TAG_P_TWOVOL : 0u);
			words[1] = pp->tsp1;
		}
		if (mapped != NULL)
			glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
		else if (trCount != 0 && gl4.trPolyWords.data != NULL)
			glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, trCount * 8, gl4.trPolyWords.data);
		glCheck();
	}
	/* The volumes' triangles with those far off the screen cut
	 * (rend/modvol_clip.h), however the frame was decoded: the volumes'
	 * array then draws them, and is set again for the next frame */
	if (!pvrrc.isRenderFramebuffer && TA_LIST_USED(pvrrc.mv_clip, ModTriangle) && gl_frame_modvol_buffer(0) == 0)
	{
		gl4SetupModvolVBO(0);
		glBufferData(GL_ARRAY_BUFFER, TA_LIST_BYTES(pvrrc.mv_clip), TA_LIST_HEAD(pvrrc.mv_clip, ModTriangle), GL_STREAM_DRAW); glCheck();
		glBindVertexArray(gl4.vbo.main_vao);
		gl4ForgetFrameBuffers();
	}
	if (!pvrrc.isRenderFramebuffer)
	{
		if (is_rtt || !settings.rend.WideScreen || transform_matrix_clipped(&matrices))
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

			gl4ShaderUniforms.base_clipping.enabled = true;
			gl4ShaderUniforms.base_clipping.x = (int)gl4_round_away(min_x);
			gl4ShaderUniforms.base_clipping.y = (int)gl4_round_away(min_y);
			gl4ShaderUniforms.base_clipping.width = (int)gl4_round_away(width);
			gl4ShaderUniforms.base_clipping.height = (int)gl4_round_away(height);
			glc_scissor(gl4ShaderUniforms.base_clipping.x, gl4ShaderUniforms.base_clipping.y,
					gl4ShaderUniforms.base_clipping.width, gl4ShaderUniforms.base_clipping.height);
			glc_enable(GL_SCISSOR_TEST);
		}
		else
		{
			gl4ShaderUniforms.base_clipping.enabled = false;
		}

		if (!is_rtt)
		{
			/* in a new context, the picture kept from the old one (rend/last_picture.h) */
			glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
			gl_restore_picture();
		}
		gl4DrawStrips(output_fbo, rendering_width, rendering_height);
	}
	else
	{
		glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);

		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glc_clear_color(0.f, 0.f, 0.f, 0.f);
		glClear(GL_COLOR_BUFFER_BIT);

		gl4DrawFramebuffer(640.f, 480.f);
	}
	if (writeout_fbo != 0)
		gl_writeout_present(frontend_fbo, screen_width, screen_height, !pvrrc.isRenderFramebuffer);


	/* VMU/Crosshair code - libretro-specific */
	if (!is_rtt)
	{
		if (settings.System == DC_PLATFORM_DREAMCAST)
		{
			for ( vmu_screen_number = 0 ; vmu_screen_number < 4 ; vmu_screen_number++)
				if ( vmu_screen_params[vmu_screen_number].vmu_screen_display )
					gl4DrawVmuTexture(vmu_screen_number);
		}

		for ( lightgun_port = 0 ; lightgun_port < 4 ; lightgun_port++)
			gl4DrawGunCrosshair(lightgun_port);
	}

	KillTex = false;

	if (is_rtt)
		ReadRTTBuffer();

	glBindVertexArray(0);
	/* (the frontend's own drawing has the context as it was) */
	if (gl.clipControl != NULL)
		gl.clipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);

	return !is_rtt;
}

static bool gl4_init(void)
{
	findGLVersion();
	if (gl.gl_major < 4 || (gl.gl_major == 4 && gl.gl_minor < 3))
	{
		WARN_LOG(RENDERER, "Warning: OpenGL %d.%d doesn't support per-pixel sorting. 4.3 required\n", gl.gl_major, gl.gl_minor);
		return false;
	}
	/* (the vertex shaders take the depth scale from a uniform buffer: findGLVersion()) */
	if (gl.depth_ubo == 0)
	{
		WARN_LOG(RENDERER, "OpenGL: no uniform buffer for the depth scale\n");
		return false;
	}
	glprog_open();

	if (!gl_create_resources())
		return false;

	glc_disable_cache();
	/* (a new context has dithering on: see the other OpenGL renderer) */
	glDisable(GL_DITHER);

	initABuffer();

	/* GPU-side TA processing, if it is on: here, when the renderer is set up, not in a frame */
	gltad_want_sort(false);
	gltad_setup(ta_gpu_wanted());
	gl4ForgetFrameBuffers();

	palette_updated = true;
	gl_texcache_clear();
	/* the textures' pool and the decoder, all of them now (gles/gl_texdecoder.h) */
	gl_tex_init(false);

	/* the polygons' programs, so that none is made in a frame (glprogram_cache.h) */
	gl4MakePrograms();
	gl_writeout_init(screen_width, screen_height, true);
	glprog_save(false);
	glprog_started();

	return true;
}

static void gl4_resize(int w, int h)
{
	gl_carry_picture(screen_width, screen_height, w, h);
	screen_width = w;
	screen_height = h;
	resize(w, h);
	/* (the write-out's picture, of the new size: gles/gl_writeout.h) */
	gl_writeout_init(w, h, true);
}

static void gl4_term(void)
{
	/* (what is new kept, while the programs are there: glprogram_cache.h) */
	glprog_log("per pixel");
	glprog_close();
	gl_writeout_term();
	glc_delete_textures(4, vmuTextureId);
	memset(vmuTextureId, 0, sizeof(vmuTextureId));
	glc_delete_textures(4, lightgunTextureId);
	memset(lightgunTextureId, 0, sizeof(lightgunTextureId));

	gltad_term();
	ta_set_modvol_clip(false);
	termABuffer();
	if (stencilTexId != 0)
	{
		glc_delete_textures(1, &stencilTexId);
		stencilTexId = 0;
	}
	if (depthTexId != 0)
	{
		glc_delete_textures(1, &depthTexId);
		depthTexId = 0;
	}
	if (opaqueTexId != 0)
	{
		glc_delete_textures(1, &opaqueTexId);
		opaqueTexId = 0;
	}
	if (depthSaveTexId != 0)
	{
		glc_delete_textures(1, &depthSaveTexId);
		depthSaveTexId = 0;
	}
	if (geom_fbo != 0)
	{
		glDeleteFramebuffers(1, &geom_fbo);
		geom_fbo = 0;
	}
	if (texSamplers[0] != 0)
	{
		glDeleteSamplers(2, texSamplers);
		texSamplers[0] = texSamplers[1] = 0;
	}
	if (depth_fbo != 0)
	{
		glDeleteFramebuffers(1, &depth_fbo);
		depth_fbo = 0;
	}
	gl_texcache_clear();
	rtt_watch_term();
	gl_tex_term();
	grow_buf_free(&gl4.trPolyWords);

	gl_term();
}

static bool gl4_process(TA_context *ctx)
{
#if !defined(TARGET_NO_THREADS)
	if (!settings.rend.ThreadedRendering)
#endif
		glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
	/* GPU-side TA processing: the frame drawn from the decode, each
	 * translucent strip numbered by its draw (TA_GPU_INSTANCE) */
	return ProcessFrame(ctx, TA_GPU_DRAW | TA_GPU_INSTANCE);
}

static bool gl4_render(void)
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
static void gl4_options_changed(void)
{
	gltad_setup(ta_gpu_wanted());
	gl4ForgetFrameBuffers();
}

static const renderer_ops gl4_renderer_ops =
{
	gl4_init,
	gl4_resize,
	gl4_term,
	gl4_process,
	gl4_render,
	gl_keep_picture,
	gl_get_texture,
	gl4_options_changed,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

const renderer_ops *rend_GL4(void)
{
	return &gl4_renderer_ops;
}

static void gl4_uniform_pair(gl4UniformShadow *u, int which, GLint location, int v0, int v1)
{
	int array[2];
	array[0] = v0;
	array[1] = v1;
	if (gl4_uniform_changed(u, which, array, 2))
		glUniform1iv(location, 2, array);
}

void gl4_uniforms_set(gl4PipelineShader *s)
{
	const gl4ShaderUniforms_t *su = &gl4ShaderUniforms;
	const u32 tsp0 = su->tsp0, tsp1 = su->tsp1;
	gl4UniformShadow *u = s->uniforms;
	/* (a record without one, as ever: every uniform set) */
	gl4UniformShadow none;
	if (u == NULL)
	{
		none.set = 0;
		u = &none;
	}

	if (s->cp_AlphaTestValue != -1 && gl4_uniform_changed(u, GL4U_ALPHA_TEST, &su->PT_ALPHA, 1))
		glUniform1f(s->cp_AlphaTestValue, su->PT_ALPHA);

	if (s->sp_FOG_DENSITY != -1 && gl4_uniform_changed(u, GL4U_FOG_DENSITY, &su->fog_den_float, 1))
		glUniform1f(s->sp_FOG_DENSITY, su->fog_den_float);

	if (s->sp_FOG_COL_RAM != -1 && gl4_uniform_changed(u, GL4U_FOG_COL_RAM, su->ps_FOG_COL_RAM, 3))
		glUniform3fv(s->sp_FOG_COL_RAM, 1, su->ps_FOG_COL_RAM);

	if (s->sp_FOG_COL_VERT != -1 && gl4_uniform_changed(u, GL4U_FOG_COL_VERT, su->ps_FOG_COL_VERT, 3))
		glUniform3fv(s->sp_FOG_COL_VERT, 1, su->ps_FOG_COL_VERT);

	if (s->shade_scale_factor != -1)
	{
		const float scale = FPU_SHAD_SCALE.scale_factor / 256.f;
		if (gl4_uniform_changed(u, GL4U_SHADE_SCALE, &scale, 1))
			glUniform1f(s->shade_scale_factor, scale);
	}

	if (s->blend_mode != -1)
	{
		/* The per-pixel pass drops a fragment whose blend would leave the
		 * pixel as it is. It cannot tell that when the source is the second
		 * accumulation buffer, or when the polygon has a second volume and
		 * which blend applies is known only when the lists are put
		 * together: those get a blend (8) that none of its cases take. */
		const bool keep = TAW_GET(tsp0, TSP_SrcSelect) || tsp1 != (u32)-1 || su->tcw1 != (u32)-1;
		u32 blend_mode[4];
		blend_mode[0] = keep ? 8u : TAW_GET(tsp0, TSP_SrcInstr);
		blend_mode[1] = keep ? 8u : TAW_GET(tsp0, TSP_DstInstr);
		blend_mode[2] = TAW_GET(tsp1, TSP_SrcInstr);
		blend_mode[3] = TAW_GET(tsp1, TSP_DstInstr);
		if (gl4_uniform_changed(u, GL4U_BLEND_MODE, blend_mode, 4))
			glUniform2iv(s->blend_mode, 2, (GLint *)blend_mode);
	}

	if (s->use_alpha != -1)
		gl4_uniform_pair(u, GL4U_USE_ALPHA, s->use_alpha, TAW_GET(tsp0, TSP_UseAlpha), TAW_GET(tsp1, TSP_UseAlpha));

	if (s->ignore_tex_alpha != -1)
		gl4_uniform_pair(u, GL4U_IGNORE_TEX_ALPHA, s->ignore_tex_alpha, TAW_GET(tsp0, TSP_IgnoreTexA), TAW_GET(tsp1, TSP_IgnoreTexA));

	if (s->shading_instr != -1)
		gl4_uniform_pair(u, GL4U_SHADING_INSTR, s->shading_instr, TAW_GET(tsp0, TSP_ShadInstr), TAW_GET(tsp1, TSP_ShadInstr));

	if (s->fog_control != -1)
		gl4_uniform_pair(u, GL4U_FOG_CONTROL, s->fog_control, TAW_GET(tsp0, TSP_FogCtrl), TAW_GET(tsp1, TSP_FogCtrl));

	if (s->pp_Number != -1)
	{
		const GLint number = su->poly_number;
		if (gl4_uniform_changed(u, GL4U_PP_NUMBER, &number, 1))
			glUniform1i(s->pp_Number, number);
	}

	if (s->trilinear_alpha != -1 && gl4_uniform_changed(u, GL4U_TRILINEAR_ALPHA, &su->trilinear_alpha, 1))
		glUniform1f(s->trilinear_alpha, su->trilinear_alpha);

	if (s->fog_clamp_min != -1 && gl4_uniform_changed(u, GL4U_FOG_CLAMP_MIN, su->fog_clamp_min, 4))
		glUniform4fv(s->fog_clamp_min, 1, su->fog_clamp_min);
	if (s->fog_clamp_max != -1 && gl4_uniform_changed(u, GL4U_FOG_CLAMP_MAX, su->fog_clamp_max, 4))
		glUniform4fv(s->fog_clamp_max, 1, su->fog_clamp_max);

	if (s->normal_matrix != -1 && gl4_uniform_changed(u, GL4U_NORMAL_MATRIX, su->normal_mat, 16))
		glUniformMatrix4fv(s->normal_matrix, 1, GL_FALSE, su->normal_mat);

	if (s->palette_index != -1 && gl4_uniform_changed(u, GL4U_PALETTE_INDEX, &su->palette_index, 1))
		glUniform1f(s->palette_index, su->palette_index);

	/* (a polygon drawn with a general program: its key) */
	if (s->key_bits != -1)
	{
		const GLint key = (GLint)s->key;
		if (gl4_uniform_changed(u, GL4U_KEY_BITS, &key, 1))
			glUniform1i(s->key_bits, key);
	}
}
