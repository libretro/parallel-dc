#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
/*
 * abuffer.c
 *
 *  Created on: May 26, 2018
 *      Author: raph
 */
#include "gl4.h"

GLuint pixels_buffer;
static GLint64 pixels_buffer_size;	/* (bytes) */
GLuint pixels_pointers;
GLuint atomic_buffer;
gl4PipelineShader g_abuffer_final_shader;
gl4PipelineShader g_abuffer_clear_shader;
gl4PipelineShader g_abuffer_tr_modvol_shaders[ModeCount];
static GLuint g_quadBuffer = 0;
static GLuint g_quadVertexArray = 0;

#include <stdio.h>
#include "rend/oit_size.h"
#include "rend/modvol_clip.h"

#ifndef GL_COPY_READ_BUFFER
#define GL_COPY_READ_BUFFER 0x8F36
#endif
#ifndef GL_COPY_WRITE_BUFFER
#define GL_COPY_WRITE_BUFFER 0x8F37
#endif
#ifndef GL_R32UI
#define GL_R32UI 0x8236
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER 0x8D94
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif

#ifndef APIENTRY
#define APIENTRY
#endif
/* What glsm does not wrap: the entry points the frontend's loader
 * (glsym/glsym_gl.c) has made, which the glsm header does not name */
extern void (APIENTRY *__rglgen_glClearBufferSubData)(GLenum, GLenum, GLintptr, GLsizeiptr, GLenum, GLenum, const void *);
extern void (APIENTRY *__rglgen_glCopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);
#define glClearBufferSubData __rglgen_glClearBufferSubData
#define glCopyBufferSubData __rglgen_glCopyBufferSubData

/* The fragments a frame asks for, counted by the shaders (the counter goes
 * up for a fragment that finds no room too), read back without waiting:
 * after each band the counter is copied, on the GPU, into this frame's
 * slot of a small buffer, and a fence is put after the frame; a frame
 * whose fence has passed, polled before a frame is drawn, has its counts
 * read. Three frames back is the earliest a count is seen. The band
 * being drawn is gl4band (gl4.h). */
gl4Band gl4band;
enum { OIT_COUNT_SLOTS = 4, OIT_MAX_BANDS = 64 };
static GLuint count_buffer;
static void *count_fence[OIT_COUNT_SLOTS];	/* GLsync; null when the slot is free */
static u32 count_bands[OIT_COUNT_SLOTS];
static bool count_rtt[OIT_COUNT_SLOTS];	/* the slot's frame was a render to a texture */
static u32 count_head;		/* the slot the frame being drawn uses */
/* how many bands frames are drawn in: the screen's [0], renders to a texture [1] */
static u32 oit_bands[2] = { 1, 1 };
static u32 oit_roomy_frames[2];

#define MAX_PIXELS_PER_FRAGMENT "32"

static const char *final_shader_source = SHADER_HEADER
"#define MAX_PIXELS_PER_FRAGMENT " MAX_PIXELS_PER_FRAGMENT
"\n"
"layout(binding = 0) uniform sampler2D tex;\n"
"uniform highp float shade_scale_factor;\n"
"\n"
"out vec4 FragColor;\n"
"\n"
"uint pixel_list[MAX_PIXELS_PER_FRAGMENT];\n"
"\n"
"\n"
"int fillAndSortFragmentArray(ivec2 coords)\n"
"{\n"
"	// Load fragments into a local memory array for sorting\n"
"	uint idx = imageLoad(abufferPointerImg, coords).x;\n"
"	int count = 0;\n"
"	for (; idx != EOL && count < MAX_PIXELS_PER_FRAGMENT; count++)\n"
"	{\n"
"		const Pixel p = pixels[idx];\n"
"		// Insertion sort. The entry before the first one is not looked at:\n"
"		// this used to read pixel_list[-1], and the pixel buffer at whatever\n"
"		// index that held, before finding out there was nothing there.\n"
"		int j = count - 1;\n"
"		while (j >= 0)\n"
"		{\n"
"			const Pixel jp = pixels[pixel_list[j]];\n"
"			if (!(jp.depth > p.depth\n"
"					|| (jp.depth == p.depth && getPolyNumber(jp) > getPolyNumber(p))))\n"
"				break;\n"
"			pixel_list[j + 1] = pixel_list[j];\n"
"			j--;\n"
"		}\n"
"		pixel_list[j + 1] = idx;\n"
"		idx = p.next;\n"
"	}\n"
"	return count;\n"
"}\n"
"\n"
"// Blend fragments back-to-front\n"
"vec4 resolveAlphaBlend(ivec2 coords) {\n"
"	\n"
"	// Copy and sort fragments into a local array\n"
"	int num_frag = fillAndSortFragmentArray(coords);\n"
"	\n"
"	vec4 finalColor = texture(tex, gl_FragCoord.xy / textureSize(tex, 0));\n"
"	vec4 secondaryBuffer = vec4(0.0); // Secondary accumulation buffer\n"
"	\n"
"	for (int i = 0; i < num_frag; i++)\n"
"	{\n"
"		const Pixel pixel = pixels[pixel_list[i]];\n"
"		const PolyParam pp = tr_poly_params[getPolyNumber(pixel)];\n"
"		bool area1 = false;\n"
"		bool shadowed = false;\n"
"		if (isShadowed(pixel))\n"
"		{\n"
"			if (isTwoVolumes(pp))\n"
"				area1 = true;\n"
"			else\n"
"				shadowed = true;\n"
"		}\n"
"		vec4 srcColor;\n"
"		if (getSrcSelect(pp, area1))\n"
"			srcColor = secondaryBuffer;\n"
"		else\n"
"		{\n"
"			srcColor = unpackUnorm4x8(pixel.color);\n"
"			if (shadowed)\n"
"				srcColor.rgb *= shade_scale_factor;\n"
"		}\n"
"		vec4 dstColor = getDstSelect(pp, area1) \? secondaryBuffer : finalColor;\n"
"		vec4 srcCoef;\n"
"		vec4 dstCoef;\n"
"		\n"
"		int srcBlend = getSrcBlendFunc(pp, area1);\n"
"		switch (srcBlend)\n"
"		{\n"
"			case ZERO:\n"
"				srcCoef = vec4(0.0);\n"
"				break;\n"
"			case ONE:\n"
"				srcCoef = vec4(1.0);\n"
"				break;\n"
"			case OTHER_COLOR:\n"
"				// the colour of what it is blended onto - which, when that is the second accumulation buffer, is that buffer's\n"
"				srcCoef = dstColor;\n"
"				break;\n"
"			case INVERSE_OTHER_COLOR:\n"
"				srcCoef = vec4(1.0) - dstColor;\n"
"				break;\n"
"			case SRC_ALPHA:\n"
"				srcCoef = vec4(srcColor.a);\n"
"				break;\n"
"			case INVERSE_SRC_ALPHA:\n"
"				srcCoef = vec4(1.0 - srcColor.a);\n"
"				break;\n"
"			case DST_ALPHA:\n"
"				srcCoef = vec4(dstColor.a);\n"
"				break;\n"
"			case INVERSE_DST_ALPHA:\n"
"				srcCoef = vec4(1.0 - dstColor.a);\n"
"				break;\n"
"		}\n"
"		int dstBlend = getDstBlendFunc(pp, area1);\n"
"		switch (dstBlend)\n"
"		{\n"
"			case ZERO:\n"
"				dstCoef = vec4(0.0);\n"
"				break;\n"
"			case ONE:\n"
"				dstCoef = vec4(1.0);\n"
"				break;\n"
"			case OTHER_COLOR:\n"
"				dstCoef = srcColor;\n"
"				break;\n"
"			case INVERSE_OTHER_COLOR:\n"
"				dstCoef = vec4(1.0) - srcColor;\n"
"				break;\n"
"			case SRC_ALPHA:\n"
"				dstCoef = vec4(srcColor.a);\n"
"				break;\n"
"			case INVERSE_SRC_ALPHA:\n"
"				dstCoef = vec4(1.0 - srcColor.a);\n"
"				break;\n"
"			case DST_ALPHA:\n"
"				dstCoef = vec4(dstColor.a);\n"
"				break;\n"
"			case INVERSE_DST_ALPHA:\n"
"				dstCoef = vec4(1.0 - dstColor.a);\n"
"				break;\n"
"		}\n"
"		const vec4 result = clamp(dstColor * dstCoef + srcColor * srcCoef, 0.0, 1.0);\n"
"		if (getDstSelect(pp, area1))\n"
"			secondaryBuffer = result;\n"
"		else\n"
"			finalColor = result;\n"
"	}\n"
"	\n"
"	return finalColor;\n"
"	\n"
"}\n"
"\n"
"void main(void)\n"
"{\n"
"	ivec2 coords = ivec2(gl_FragCoord.xy);\n"
"	// Compute and output final color for the frame buffer\n"
"	// Visualize the number of layers in use\n"
"	//FragColor = vec4(float(fillAndSortFragmentArray(coords)) / MAX_PIXELS_PER_FRAGMENT * 4, 0, 0, 1);\n"
"	FragColor = resolveAlphaBlend(coords);\n"
"}\n";

static const char *clear_shader_source = SHADER_HEADER
"\n"
"void main(void)\n"
"{\n"
"	ivec2 coords = ivec2(gl_FragCoord.xy);\n"
"\n"
"	// Reset pointers\n"
"	imageStore(abufferPointerImg, coords, uvec4(EOL));\n"
"\n"
"	// Discard fragment so nothing is written to the framebuffer\n"
"	discard;\n"
"}\n";

static const char *tr_modvol_shader_source = SHADER_HEADER
"#define MAX_PIXELS_PER_FRAGMENT " MAX_PIXELS_PER_FRAGMENT
"\n"
"#define MV_MODE %d\n"
"\n"
"// Must match ModifierVolumeMode enum values\n"
"#define MV_XOR		 0\n"
"#define MV_OR		 1\n"
"#define MV_INCLUSION 2\n"
"#define MV_EXCLUSION 3\n"
"\n"
"void main(void)\n"
"{\n"
"	ivec2 coords = ivec2(gl_FragCoord.xy);\n"
"	\n"
"	uint idx = imageLoad(abufferPointerImg, coords).x;\n"
"	int list_len = 0;\n"
"	while (idx != EOL && list_len < MAX_PIXELS_PER_FRAGMENT)\n"
"	{\n"
"		const Pixel pixel = pixels[idx];\n"
"		const PolyParam pp = tr_poly_params[getPolyNumber(pixel)];\n"
"		if (getShadowEnable(pp))\n"
"		{\n"
"#if MV_MODE == MV_XOR\n"
"			if (FRAG_DEPTH >= pixel.depth)\n"
"				atomicXor(pixels[idx].seq_num, SHADOW_STENCIL);\n"
"#elif MV_MODE == MV_OR\n"
"			if (FRAG_DEPTH >= pixel.depth)\n"
"				atomicOr(pixels[idx].seq_num, SHADOW_STENCIL);\n"
"#elif MV_MODE == MV_INCLUSION\n"
"			uint prev_val = atomicAnd(pixels[idx].seq_num, ~(SHADOW_STENCIL));\n"
"			if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_STENCIL)\n"
"				pixels[idx].seq_num = bitfieldInsert(pixel.seq_num, 1u, 31, 1);\n"
"#elif MV_MODE == MV_EXCLUSION\n"
"			uint prev_val = atomicAnd(pixels[idx].seq_num, ~(SHADOW_STENCIL|SHADOW_ACC));\n"
"			if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_ACC)\n"
"				pixels[idx].seq_num = bitfieldInsert(pixel.seq_num, 1u, 31, 1);\n"
"#endif\n"
"		}\n"
"		idx = pixel.next;\n"
"		list_len++;\n"
"	}\n"
"	\n"
"	discard;\n"
"}\n";

static const char* VertexShaderSource =
"#version 430\n"
"\n"
"in highp vec3 in_pos;\n"
"\n"
"void main()\n"
"{\n"
"	gl_Position = vec4(in_pos, 1.0);\n"
"}\n";

void initABuffer(void)
{
	GLint64 most, size;
	if (max_image_width > 0 && max_image_height > 0)
	{
		if (pixels_pointers == 0)
			pixels_pointers = glc_gen_texture();
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_2D, pixels_pointers);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, max_image_width, max_image_height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, 0);
		glBindImageTexture(4, pixels_pointers, 0, false, 0,  GL_READ_WRITE, GL_R32UI);
		glCheck();
	}

	/* The fragments, for the screen's size (rend/oit_size.h): made again
	 * when that changes it, which a render to a texture does not */
	glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &most);
	size = (GLint64)oit_buffer_bytes(screen_width, screen_height, vram_budget_available_kib(&gl.vram_budget), (u64)(most > 0 ? most : 0));
	if (pixels_buffer != 0 && size != pixels_buffer_size)
	{
		glDeleteBuffers(1, &pixels_buffer);
		pixels_buffer = 0;
	}
	if (pixels_buffer == 0 )
	{
		pixels_buffer_size = size;
		/* Create the buffer */
		glGenBuffers(1, &pixels_buffer);
		/* Bind it */
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, pixels_buffer);
		/* Declare storage */
		glBufferData(GL_SHADER_STORAGE_BUFFER, size, NULL, GL_DYNAMIC_COPY);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pixels_buffer);
		glCheck();
	}

	if (atomic_buffer == 0 )
	{
		GLint zero;
		/* Create the buffer */
		glGenBuffers(1, &atomic_buffer);
		/* Bind it */
		glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, atomic_buffer);
		/* Declare storage */
		glBufferData(GL_ATOMIC_COUNTER_BUFFER, 4, NULL, GL_DYNAMIC_COPY);
		glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, atomic_buffer);
		zero = 0;
		glBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(GLint), &zero);
		glCheck();
	}
	if (count_buffer == 0)
	{
		glGenBuffers(1, &count_buffer);
		glBindBuffer(GL_COPY_WRITE_BUFFER, count_buffer);
		glBufferData(GL_COPY_WRITE_BUFFER, OIT_COUNT_SLOTS * OIT_MAX_BANDS * sizeof(GLuint), NULL, GL_DYNAMIC_READ);
		glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
		glCheck();
	}

	if (g_abuffer_final_shader.program == 0)
		gl4CompilePipelineShader(&g_abuffer_final_shader, final_shader_source, VertexShaderSource);
	if (g_abuffer_clear_shader.program == 0)
		gl4CompilePipelineShader(&g_abuffer_clear_shader, clear_shader_source, VertexShaderSource);
	if (g_abuffer_tr_modvol_shaders[0].program == 0)
	{
		/* (the renderer's thread's) */
		static char source[16384];
		int mode;
		verify(strlen(tr_modvol_shader_source) + 11 < sizeof(source));
		for (mode = 0; mode < ModeCount; mode++)
		{
			sprintf(source, tr_modvol_shader_source, mode);
			gl4CompilePipelineShader(&g_abuffer_tr_modvol_shaders[mode], source, NULL);
		}
	}

	if (g_quadVertexArray == 0)
		glGenVertexArrays(1, &g_quadVertexArray);
	if (g_quadBuffer == 0)
	{
		glBindVertexArray(g_quadVertexArray);
		glGenBuffers(1, &g_quadBuffer);
		glBindBuffer(GL_ARRAY_BUFFER, g_quadBuffer); glCheck();

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (void*)0);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); glCheck();
		glBindVertexArray(0);
	}
	glCheck();

	/* Clear A-buffer pointers */
	glc_use_program(g_abuffer_clear_shader.program);
	gl4_uniforms_set(&g_abuffer_clear_shader);

	abufferDrawQuad();
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	glCheck();
}

void termABuffer(void)
{
   u32 i;
   int mode;
   if (pixels_pointers != 0)
   {
	  glc_delete_textures(1, &pixels_pointers);
	  pixels_pointers = 0;
   }
   if (pixels_buffer != 0)
   {
	  glDeleteBuffers(1, &pixels_buffer);
	  pixels_buffer = 0;
   }
   if (atomic_buffer != 0)
   {
	  glDeleteBuffers(1, &atomic_buffer);
	  atomic_buffer = 0;
   }
   if (count_buffer != 0)
   {
	  glDeleteBuffers(1, &count_buffer);
	  count_buffer = 0;
   }
   for (i = 0; i < OIT_COUNT_SLOTS; i++)
	  if (count_fence[i] != NULL)
	  {
		 glDeleteSync((GLsync)count_fence[i]);
		 count_fence[i] = NULL;
	  }
   count_head = 0;
   oit_bands[0] = oit_bands[1] = 1;
   oit_roomy_frames[0] = oit_roomy_frames[1] = 0;
   gl4band.height = 0;
   if (g_quadVertexArray != 0)
   {
	  glDeleteVertexArrays(1, &g_quadVertexArray);
	  g_quadVertexArray = 0;
   }
   if (g_quadBuffer != 0)
   {
	  glDeleteBuffers(1, &g_quadBuffer);
	  g_quadBuffer = 0;
   }
	glDeleteProgram(g_abuffer_final_shader.program);
	g_abuffer_final_shader.program = 0;
	glDeleteProgram(g_abuffer_clear_shader.program);
	g_abuffer_clear_shader.program = 0;
	for (mode = 0; mode < ModeCount; mode++)
	{
		glDeleteProgram(g_abuffer_tr_modvol_shaders[mode].program);
		g_abuffer_tr_modvol_shaders[mode].program = 0;
	}
}

void reshapeABuffer(int w, int h)
{
	if (pixels_pointers != 0)
	{
		glc_delete_textures(1, &pixels_pointers);
		pixels_pointers = 0;
	}

	initABuffer();
}

void abufferDrawQuad(void)
{

	static const float vertices[] = {
			-1,  1, 1,
			-1, -1, 1,
			 1,  1, 1,
			 1, -1, 1,
	};
	static const GLushort indices[] = { 0, 1, 2, 1, 3 };

	glBindVertexArray(g_quadVertexArray);
	glBindBuffer(GL_ARRAY_BUFFER, g_quadBuffer); glCheck();
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW); glCheck();

	glDrawElements(GL_TRIANGLE_STRIP, 5, GL_UNSIGNED_SHORT, indices); glCheck();
	glBindVertexArray(0);
	glCheck();
}

/* The translucent modifier volumes' draws change the lists from their
 * shaders (tr_modvol_shader_source), and nothing orders what the shaders of
 * two draws do to the same fragment but a memory barrier between them: a
 * GPU runs draws at once, in any order, where a software device runs them
 * one after the other. They go through here. Built with RACE_TEST
 * (tools/rend/speckle_test.sh; never in a release) the draws between two
 * barriers are drawn a triangle at a time, the draws' triangles taken in
 * turn, the last draw's first: one draw's shaders run while another's are
 * half done, as on a GPU, and a picture that changes then depends on an
 * order nothing keeps. */
#ifdef RACE_TEST
typedef struct ListHeld { gl4PipelineShader *shader; u32 cull; int first, count; } ListHeld;
#endif
typedef struct ListDraws
{
#ifdef RACE_TEST
	ListHeld held[256];
	u32 heldCount;
#endif
	int unused;
} ListDraws;

static void list_draws_record(gl4PipelineShader *shader, u32 cull, int first, int count)
{
	glc_use_program(shader->program);
	gl4_uniforms_set(shader);
	SetCull(cull);
	glDrawArrays(GL_TRIANGLES, first, count); glCheck();
}

static void list_draws_flush(ListDraws *l)
{
#ifdef RACE_TEST
	int v, more;
	for (v = 0, more = (int)l->heldCount; more != 0; v += 3)
	{
		u32 i;
		more = 0;
		for (i = l->heldCount; i-- > 0; )
			if (v < l->held[i].count)
			{
				list_draws_record(l->held[i].shader, l->held[i].cull, l->held[i].first + v, 3);
				more++;
			}
	}
	l->heldCount = 0;
#else
	(void)l;
#endif
}

static void list_draws_draw(ListDraws *l, gl4PipelineShader *shader, u32 cull, int first, int count)
{
#ifdef RACE_TEST
	if (l->heldCount == sizeof(l->held) / sizeof(l->held[0]))
		list_draws_flush(l);
	l->held[l->heldCount].shader = shader;
	l->held[l->heldCount].cull = cull;
	l->held[l->heldCount].first = first;
	l->held[l->heldCount].count = count;
	l->heldCount++;
#else
	(void)l;
	list_draws_record(shader, cull, first, count);
#endif
}

/* what the draws before did to the lists is done before the draws after start */
static void list_draws_barrier(ListDraws *l)
{
	list_draws_flush(l);
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void DrawTranslucentModVols(int first, int count, bool useOpaqueGeom)
{
	/* (of a frame decoded on the GPU, its triangles are the GPU's) */
	const u32 triangles = TA_LIST_USED(pvrrc.mv_clip, ModTriangle) ? (u32)TA_LIST_USED(pvrrc.mv_clip, ModTriangle) : pvrrc.gpu ? pvrrc.gpu_modtrig : (u32)TA_LIST_USED(pvrrc.modtrig, ModTriangle);
	const ModifierVolumeParam *list, *params;
	int mod_base = -1;
	ListDraws listDraws;
	int cmv;
	if (count == 0 || triangles == 0)
		return;
	glBindVertexArray(gl4.vbo.modvol_vao);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);

	glc_disable(GL_DEPTH_TEST);
	glc_disable(GL_STENCIL_TEST);

	glCheck();

	list = useOpaqueGeom ? TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam) : TA_LIST_HEAD(pvrrc.global_param_mvo_tr, ModifierVolumeParam);
	params = &list[first];

	/* (the lists the translucent polygons' shaders wrote, done before the volumes read them) */
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

#ifdef RACE_TEST
	listDraws.heldCount = 0;
#endif

	for (cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam *param = &params[cmv];
		u32 mvFirst, mvCount, mv_mode, cull;
		gl4PipelineShader *shader;
		mv_clip_range_get(&pvrrc, list, param, &mvFirst, &mvCount);

		if (mvCount == 0)
			continue;

		mv_mode = TAW_GET(param->isp, ISP_Modvol_DepthMode);
		cull = TAW_GET(param->isp, ISP_Modvol_CullMode);

		verify(mvFirst + mvCount <= triangles);

		if (mod_base == -1)
			mod_base = mvFirst;

		if (!TAW_GET(param->isp, ISP_Modvol_VolumeLast) && mv_mode > 0)
			shader = &g_abuffer_tr_modvol_shaders[Or];	/* OR'ing (open volume or quad) */
		else
			shader = &g_abuffer_tr_modvol_shaders[Xor];	/* XOR'ing (closed volume) */
		list_draws_draw(&listDraws, shader, cull, mvFirst * 3, mvCount * 3);

		if (mv_mode == 1 || mv_mode == 2)
		{
			/* Sum the area: once the volume's toggles are done, and done before the next volume's start */
			list_draws_barrier(&listDraws);
			shader = &g_abuffer_tr_modvol_shaders[mv_mode == 1 ? Inclusion : Exclusion];
			list_draws_draw(&listDraws, shader, cull, mod_base * 3, (mvFirst + mvCount - mod_base) * 3);
			list_draws_barrier(&listDraws);
			mod_base = -1;
		}
	}
	list_draws_flush(&listDraws);
	glBindVertexArray(gl4.vbo.main_vao);
}

/* The bands this frame is drawn in, from the counts of the oldest frame
 * whose fence has passed: never this frame's, which nothing waits for.
 * More at once when that one had fragments that found no room; fewer when
 * a second of frames in a row would have fitted in fewer. Each band is
 * taken to need what the fullest one did, with a quarter more room. The
 * screen's frames and renders to a texture are counted apart. */
u32 gl4OitChooseBands(bool rtt)
{
	const u32 slot = (count_head + 1) % OIT_COUNT_SLOTS;	/* the oldest in flight, when all are */
	u32 k;
	for (k = 0; k < OIT_COUNT_SLOTS; k++)
	{
		const u32 i = (slot + k) % OIT_COUNT_SLOTS;
		GLuint counts[OIT_MAX_BANDS];
		u32 drawnIn, fragments = 0, b, wanted, was;
		u32 *bands, *roomy;
		u64 room, frame, want;
		GLenum r;
		if (count_fence[i] == NULL)
			continue;
		r = glClientWaitSync((GLsync)count_fence[i], 0, 0);
		if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
			break;
		glDeleteSync((GLsync)count_fence[i]);
		count_fence[i] = NULL;

		drawnIn = count_bands[i] < OIT_MAX_BANDS ? count_bands[i] : OIT_MAX_BANDS;
		if (drawnIn < 1)
			drawnIn = 1;
		glBindBuffer(GL_COPY_READ_BUFFER, count_buffer);
		glGetBufferSubData(GL_COPY_READ_BUFFER, (GLintptr)(i * OIT_MAX_BANDS * sizeof(GLuint)), drawnIn * sizeof(GLuint), counts);
		glBindBuffer(GL_COPY_READ_BUFFER, 0);
		for (b = 0; b < drawnIn; b++)
			if (counts[b] > fragments)
				fragments = counts[b];

		room = (u64)pixels_buffer_size / OIT_FRAGMENT_BYTES;
		if (room < 1)
			room = 1;
		frame = (u64)fragments * drawnIn;
		want = (frame + frame / 4 + room - 1) / room;
		if (want > OIT_MAX_BANDS)
			want = OIT_MAX_BANDS;
		wanted = want < 1 ? 1 : (u32)want;
		bands = &oit_bands[count_rtt[i] ? 1 : 0];
		roomy = &oit_roomy_frames[count_rtt[i] ? 1 : 0];
		was = *bands;
		if (fragments > room)
		{
			u32 most = *bands;
			if (wanted > most)
				most = wanted;
			if (drawnIn + 1 > most)
				most = drawnIn + 1;
			*bands = most < OIT_MAX_BANDS ? most : OIT_MAX_BANDS;
			*roomy = 0;
		}
		else if (wanted < *bands)
		{
			if (++*roomy >= 60)
			{
				*bands = wanted;
				*roomy = 0;
			}
		}
		else
			*roomy = 0;
		if (*bands != was)
			NOTICE_LOG(RENDERER, "OpenGL per-pixel renderer: %u translucent fragments in a band of %u%s, room for %u: %u band%s\n",
					fragments, drawnIn, count_rtt[i] ? " (a render to a texture)" : "", (u32)room, *bands, *bands > 1 ? "s" : "");
		break;
	}
	return oit_bands[rtt ? 1 : 0];
}

void gl4OitBandBegin(void)
{
	static const GLuint zero = 0;
	/* The band before (or the frame before) may still be being put on the
	 * picture: its lists read from the first entries, which this band's
	 * polygons write again, and its pointers set back to nothing, which
	 * this band's polygons link from. Nothing of this band's starts before
	 * that is done. */
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_ATOMIC_COUNTER_BARRIER_BIT);
	/* the counter back at nothing, on the GPU, after what the band before drew */
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, atomic_buffer);
	glClearBufferSubData(GL_ATOMIC_COUNTER_BUFFER, GL_R32UI, 0, sizeof(GLuint), GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
}

void gl4OitBandEnd(u32 band)
{
	if (band >= OIT_MAX_BANDS)
		return;
	glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
	glBindBuffer(GL_COPY_WRITE_BUFFER, count_buffer);
	glCopyBufferSubData(GL_ATOMIC_COUNTER_BUFFER, GL_COPY_WRITE_BUFFER, 0,
			(GLintptr)((count_head * OIT_MAX_BANDS + band) * sizeof(GLuint)), sizeof(GLuint));
	glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

void gl4OitFrameEnd(u32 bands, bool rtt)
{
	if (count_fence[count_head] != NULL)
		/* (a slot still in flight is used again: its count is lost, not waited for) */
		glDeleteSync((GLsync)count_fence[count_head]);
	count_fence[count_head] = (void *)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	count_bands[count_head] = bands;
	count_rtt[count_head] = rtt;
	count_head = (count_head + 1) % OIT_COUNT_SLOTS;
}

void renderABuffer(void)
{
	/* Render to output FBO */
	glc_use_program(g_abuffer_final_shader.program);
	gl4_uniforms_set(&g_abuffer_final_shader);

	glc_disable(GL_DEPTH_TEST);
	glc_disable(GL_CULL_FACE);
	/* (the band's pixels alone: the others' lists are empty, and their picture is done) */
	gl4ScissorOff();
	/* what the last polygon blended with is no business of the lists being put on the picture */
	glc_disable(GL_BLEND);
	/* (and what the polygons' and the volumes' shaders wrote to the lists, done) */
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

	abufferDrawQuad();

	glCheck();

	/* Clear A-buffer pointers */
	glc_use_program(g_abuffer_clear_shader.program);
	gl4_uniforms_set(&g_abuffer_clear_shader);

	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	abufferDrawQuad();

	glActiveTexture(GL_TEXTURE0);

	glCheck();
}
