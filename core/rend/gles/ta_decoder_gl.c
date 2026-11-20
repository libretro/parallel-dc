/* GPU-side TA processing in OpenGL 4.3: see ta_decoder_gl.h */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <features/features_cpu.h>
#include "gles.h"
#include "ta_decoder_gl.h"
#include "glprogram_cache.h"
#include "rend/ta_gpu.h"

#ifdef HAVE_GL4

/* The kernels, by TAG_K_*: SPIR-V for OpenGL, and the header's text, made
 * from rend/ta_gpu.h when the core was built (tools/rend/kernels_spv.sh,
 * which checks that they still are its) */
#include "rend/ta_gpu_gl_spv.inc"
#include "rend/ta_gpu_glsl.inc"
/* (a kernel for each TAG_K_*) */
typedef char ta_gl_kernels_check[sizeof(ta_gpu_gl_spv) / sizeof(ta_gpu_gl_spv[0]) == TAG_KERNELS + 1 ? 1 : -1];
/* (glDrawElementsIndirect()'s command: count, instances, first index, base
 * vertex, base instance: a record is an indexed indirect draw) */
typedef char ta_gl_command_check[TAG_C_WORDS == 5 ? 1 : -1];

#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif
#ifndef GL_CLIENT_STORAGE_BIT
#define GL_CLIENT_STORAGE_BIT 0x0200
#endif
#ifndef GL_PARAMETER_BUFFER_ARB
#define GL_PARAMETER_BUFFER_ARB 0x80EE
#endif
#ifndef GL_SHADER_BINARY_FORMAT_SPIR_V_ARB
#define GL_SHADER_BINARY_FORMAT_SPIR_V_ARB 0x9551
#endif
#ifndef GL_DRAW_INDIRECT_BUFFER
#define GL_DRAW_INDIRECT_BUFFER 0x8F3F
#endif
#ifndef GL_DISPATCH_INDIRECT_BUFFER
#define GL_DISPATCH_INDIRECT_BUFFER 0x90EE
#endif
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_COMMAND_BARRIER_BIT
#define GL_COMMAND_BARRIER_BIT 0x00000040
#endif
#ifndef GL_SHADER_STORAGE_BARRIER_BIT
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#endif
#ifndef GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT
#define GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT 0x00000001
#endif
#ifndef GL_ELEMENT_ARRAY_BARRIER_BIT
#define GL_ELEMENT_ARRAY_BARRIER_BIT 0x00000002
#endif
#ifndef GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS
#define GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS 0x90DB
#endif
#ifndef GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS
#define GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS 0x90DD
#endif
#ifndef GL_MAX_SHADER_STORAGE_BLOCK_SIZE
#define GL_MAX_SHADER_STORAGE_BLOCK_SIZE 0x90DE
#endif
#ifndef GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT
#define GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT 0x90DF
#endif
#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_R32UI
#define GL_R32UI 0x8236
#endif

/* What this needs of OpenGL 4.3 and later that the state manager (glsm)
 * has no function for: taken from the frontend by name when it is set up */
static struct
{
	void (APIENTRYP shaderBinary)(GLsizei count, const GLuint *shaders, GLenum format, const void *binary, GLsizei length);
	void (APIENTRYP specializeShader)(GLuint shader, const GLchar *entry, GLuint n, const GLuint *index, const GLuint *value);
	void (APIENTRYP queryCounter)(GLuint id, GLenum target);
	void (APIENTRYP getQueryObjectiv)(GLuint id, GLenum pname, GLint *params);
	void (APIENTRYP getQueryObjectui64v)(GLuint id, GLenum pname, GLuint64 *params);
	void (APIENTRYP clearBufferSubData)(GLenum target, GLenum internalformat, GLintptr offset, GLsizeiptr size, GLenum format,
			GLenum type, const void *data);
	void (APIENTRYP bindBufferRange)(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
	void (APIENTRYP dispatchCompute)(GLuint x, GLuint y, GLuint z);
	void (APIENTRYP dispatchComputeIndirect)(GLintptr indirect);
	void (APIENTRYP drawElementsIndirect)(GLenum mode, GLenum type, const void *indirect);
	void (APIENTRYP multiDrawElementsIndirect)(GLenum mode, GLenum type, const void *indirect, GLsizei drawcount, GLsizei stride);
	/* (OpenGL 4.6, ARB_indirect_parameters; null without) */
	void (APIENTRYP multiDrawElementsIndirectCount)(GLenum mode, GLenum type, const void *indirect, GLintptr drawcount,
			GLsizei maxdrawcount, GLsizei stride);
	void (APIENTRYP vertexAttribDivisor)(GLuint index, GLuint divisor);
	void (APIENTRYP vertexAttribI4ui)(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
} gx;

/* A function of the frontend's, by @name or else @alternative (may be
 * NULL), into @fn (a pointer to a function pointer); whether there is one */
static bool gl_function(void *fn, retro_get_proc_address_t get, const char *name, const char *alternative)
{
	retro_proc_address_t f = get(name);
	if (f == NULL && alternative != NULL)
		f = get(alternative);
	memcpy(fn, &f, sizeof(f));
	return f != NULL;
}

/* The frames whose tables (and sorted indices) can be in flight at once: a
 * frame that finds its piece of the ring still the GPU's is parsed */
#define TA_GL_FRAMES 3

/* The buffers, in the kernels' bindings' order from TAG_GL_BINDING + 1 (the frame's data is TAG_GL_BINDING) */
enum { B_TAB, B_VTX, B_MOD, B_IDX, B_WORK, B_REC, B_CMD, B_FRM, B_TRD, B_TRP, B_COUNT };

typedef struct Resources
{
	GLuint pool;		/* the TA contexts' data */
	u8 *poolMap;
	GLuint buffers[B_COUNT];
	u32 *tables;		/* mapped: a piece for each frame */
	u32 tableWords;
	u32 slice, slices;
	TaGpuLayout layout;
	GLuint programs[TAG_KERNELS + 1];
	GLuint sorted;		/* (TA_GPU_SORT) the sorted indices of each frame of the ring */
	u8 *sortedMap;
	GLintptr sortSlot;	/* bytes; 0: none */
	GLuint instances;
	GLsync fences[TA_GL_FRAMES];
	u32 slotSerial[TA_GL_FRAMES];
	GLuint queries[2 * TA_GL_FRAMES];	/* GPU time, read once the frame's fence has signalled */
	bool timed[TA_GL_FRAMES];
	GLintptr shadowCommands;	/* bytes */
	u32 trParamsBytes;
} Resources;

static void resources_free(Resources *r)
{
	int i, k;
	if (r == NULL)
		return;
	for (i = 0; i < TA_GL_FRAMES; i++)
		if (r->fences[i] != NULL)
			glDeleteSync(r->fences[i]);
	if (r->queries[0] != 0)
		glDeleteQueries(2 * TA_GL_FRAMES, r->queries);
	for (k = 1; k <= TAG_KERNELS; k++)
		if (r->programs[k] != 0)
			glDeleteProgram(r->programs[k]);
	/* (a persistently mapped buffer is unmapped as it goes) */
	glDeleteBuffers(B_COUNT, r->buffers);
	if (r->pool != 0)
		glDeleteBuffers(1, &r->pool);
	if (r->sorted != 0)
		glDeleteBuffers(1, &r->sorted);
	if (r->instances != 0)
		glDeleteBuffers(1, &r->instances);
	free(r);
}

/* The decoder */
static struct
{
	/* for measuring */
	u64 statFrames;		/* decoded */
	u64 statSkipped;	/* left to the CPU */
	u64 statBusy;		/* ...of them, because the GPU still had this frame's piece of the ring */
	u64 statWhy[TA_GPU_WHY_COUNT];
	double statGpuMs;	/* GPU time of the decodes timed */
	double statGpuMaxMs;
	u64 statGpuTimed;
	u64 statSorted;
	double statSortUs;
	double statSortMaxUs;
	u64 statShadowed;

	Resources *res;
	bool ready;
	bool failed;		/* could not be made in this context: not tried again */
	bool sortWanted;
	bool sortable;
	bool prepared;
	bool recorded;
	bool busy;		/* this frame's piece of the ring is the GPU's still */
	int index;
	u32 serial;
	u32 done;
	u32 flags;
	GLuint boundIndirect, boundParameter;
	TaGpuFrame frame;
	GlTaDraw draw;
} ta;

static bool gl_has_extension(const char *name)
{
	GLint n = 0, i;
	glGetIntegerv(GL_NUM_EXTENSIONS, &n);
	for (i = 0; i < n; i++)
	{
		const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (e != NULL && !strcmp(e, name))
			return true;
	}
	return false;
}

static bool gl_version_at_least(int major, int minor)
{
	return gl.gl_major > major || (gl.gl_major == major && gl.gl_minor >= minor);
}

static bool program_linked(GLuint program)
{
	GLint ok = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	return ok != 0;
}

/* a compute shader made into a program; 0 if it is not one */
static GLuint link_compute(GLuint shader)
{
	GLint ok = 0;
	GLuint program;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		log[0] = 0;
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		WARN_LOG(RENDERER, "OpenGL: a TA processing kernel did not compile: %s\n", log);
		glDeleteShader(shader);
		return 0;
	}
	program = glCreateProgram();
	glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	glAttachShader(program, shader);
	glLinkProgram(program);
	glDetachShader(program, shader);
	glDeleteShader(shader);
	if (!program_linked(program))
	{
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

/* The kernels from SPIR-V (OpenGL 4.6, ARB_gl_spirv): the driver makes its
 * own code of them now */
static bool kernels_from_spirv(GLuint *programs)
{
	int k;
	if (gx.shaderBinary == NULL || gx.specializeShader == NULL)
		return false;
	for (k = 1; k <= TAG_KERNELS; k++)
	{
		const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
		gx.shaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V_ARB, ta_gpu_gl_spv[k], (GLsizei)ta_gpu_gl_spv_size[k]);
		gx.specializeShader(shader, "main", 0, NULL, NULL);
		programs[k] = link_compute(shader);
		if (programs[k] == 0)
			return false;
	}
	return true;
}

/* ---- The kernels from their text, through the programs kept on disk
 * (glprogram_cache.h): compiled the first time, taken from there after ---- */

static bool kernels_from_text(GLuint *programs)
{
	const size_t lines = sizeof(ta_gpu_glsl) / sizeof(ta_gpu_glsl[0]);
	const char **source = (const char **)malloc((lines + 1) * sizeof(const char *));
	bool ok = source != NULL;
	size_t i;
	int k;
	for (i = 0; ok && i < lines; i++)
		source[i + 1] = ta_gpu_glsl[i];
	for (k = 1; ok && k <= TAG_KERNELS; k++)
	{
		char prefix[128];
		sprintf(prefix, "#version 430\n#define TAG_SHADER_GLSL\n#define TAG_SHADER_GL\n#define TAG_KERNEL %d\n", k);
		source[0] = prefix;
		programs[k] = glprog_make_compute(source, (u32)(lines + 1));
		if (programs[k] == 0)
			ok = false;
	}
	free((void *)source);
	return ok;
}

static void delete_programs(GLuint *programs)
{
	int k;
	for (k = 1; k <= TAG_KERNELS; k++)
		if (programs[k] != 0)
		{
			glDeleteProgram(programs[k]);
			programs[k] = 0;
		}
}

/* ---- */

/* An immutable buffer of @bytes: for the GPU only (flags 0), or mapped persistently */
static GLuint make_buffer(GLsizeiptr bytes, GLbitfield flags, void **map, const void *data)
{
	GLuint b = 0;
	glGenBuffers(1, &b);
	glBindBuffer(GL_COPY_WRITE_BUFFER, b);
	glBufferStorage(GL_COPY_WRITE_BUFFER, bytes, data, flags);
	if (map != NULL)
		*map = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, bytes, flags & ~GL_CLIENT_STORAGE_BIT);
	glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
	return b;
}

/* (a read of every 64th byte of @n from @p) */
static u64 read_sum(const u8 *p, size_t n)
{
	u64 s = 0;
	size_t i;
	for (i = 0; i < n; i += 64)
		s += *(const volatile u64 *)(p + i);
	return s;
}

/* Whether reading the mapped memory goes at about the speed of ordinary
 * memory: write-combined memory, which some drivers give a persistent
 * mapping, is many times slower to read, and the SH4 reads every control
 * word back from it (and a save state all of it) */
static bool mapping_reads_fast(u8 *map, size_t bytes)
{
	u8 *plain = (u8 *)malloc(bytes);
	u64 dummy = 0;
	retro_time_t t0, t1, t2;
	double plainUs, mapUs;
	int r;
	if (plain == NULL)
		return false;
	memset(plain, 1, bytes);
	memset(map, 1, bytes);
	t0 = cpu_features_get_time_usec();
	for (r = 0; r < 4; r++)
		dummy += read_sum(plain, bytes);
	t1 = cpu_features_get_time_usec();
	for (r = 0; r < 4; r++)
		dummy += read_sum(map, bytes);
	t2 = cpu_features_get_time_usec();
	memset(map, 0, bytes);
	free(plain);
	plainUs = (double)(t1 - t0);
	mapUs = (double)(t2 - t1);
	INFO_LOG(RENDERER, "OpenGL: the TA contexts' mapped memory reads in %.0f us, ordinary memory in %.0f us (%u)\n",
			mapUs, plainUs, (unsigned)(dummy & 1));
	return mapUs <= 4 * plainUs + 200;
}

bool gltad_context_can_do(void)
{
	return !gl.is_gles && gl_version_at_least(4, 3) && (gl_version_at_least(4, 4) || gl_has_extension("GL_ARB_buffer_storage"));
}

/* (it stays on the CPU, and why) */
static bool no(const char *why)
{
	INFO_LOG(RENDERER, "OpenGL: TA processing stays on the CPU (%s)\n", why);
	ta.failed = true;
	return false;
}

/* (the decoder's resources in @r, made; false, and what was made left in
 * @r for the caller to free) */
static bool init_resources(Resources *r)
{
	const GLbitfield mapped = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
	glsm_ctx_proc_address_t proc;
	retro_get_proc_address_t get;
	GLint blocks = 0, bindings = 0, align = 0;
	GLint64 blockSize = 0;
	TaGpuSizes sz;
	u32 strips, work[9], workWords, alignWords, trdSlots, spares;
	u64 budget, most;
	retro_time_t t0;
	const char *made;
	bool kernels;
	double kernelMs;
	void *map = NULL;
	int i;

#ifdef MSB_FIRST
	/* (the kernels read the blocks little-endian) */
	return no("the kernels read the blocks little-endian");
#endif
	if (gl.is_gles || !gl_version_at_least(4, 3))
		return no("OpenGL 4.3 is needed");
	/* (what errors others left, so that those found below are this one's) */
	for (i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++)
		;
	if (gl.index_type != GL_UNSIGNED_INT)
		return no("16-bit indices");
	/* The frame's data where the SH4 writes it, read by the GPU there: a
	 * persistent mapping. (Without one each frame's data would be copied
	 * up as well; it stays on the CPU instead.) */
	if (!(gl_version_at_least(4, 4) || gl_has_extension("GL_ARB_buffer_storage")))
		return no("no persistently mapped buffers: OpenGL 4.4 or ARB_buffer_storage");
	proc.addr = NULL;
	if (!glsm_ctl(GLSM_CTL_PROC_ADDRESS_GET, &proc) || proc.addr == NULL)
		return no("no way to its functions");
	get = proc.addr;
	if (!gl_function(&gx.queryCounter, get, "glQueryCounter", NULL) || !gl_function(&gx.getQueryObjectiv, get, "glGetQueryObjectiv", NULL)
			|| !gl_function(&gx.getQueryObjectui64v, get, "glGetQueryObjectui64v", NULL)
			|| !gl_function(&gx.clearBufferSubData, get, "glClearBufferSubData", NULL) || !gl_function(&gx.bindBufferRange, get, "glBindBufferRange", NULL)
			|| !gl_function(&gx.dispatchCompute, get, "glDispatchCompute", NULL) || !gl_function(&gx.dispatchComputeIndirect, get, "glDispatchComputeIndirect", NULL)
			|| !gl_function(&gx.drawElementsIndirect, get, "glDrawElementsIndirect", NULL)
			|| !gl_function(&gx.multiDrawElementsIndirect, get, "glMultiDrawElementsIndirect", NULL)
			|| !gl_function(&gx.vertexAttribDivisor, get, "glVertexAttribDivisor", NULL) || !gl_function(&gx.vertexAttribI4ui, get, "glVertexAttribI4ui", NULL))
		return no("a function of OpenGL 4.3 is missing");
	gx.shaderBinary = NULL;
	gx.specializeShader = NULL;
	if (gl_version_at_least(4, 6) || gl_has_extension("GL_ARB_gl_spirv"))
	{
		gl_function(&gx.shaderBinary, get, "glShaderBinary", NULL);
		gl_function(&gx.specializeShader, get, gl_version_at_least(4, 6) ? "glSpecializeShader" : "glSpecializeShaderARB", "glSpecializeShaderARB");
	}
	/* Indirect draws of several at once (OpenGL 4.3), with their count from
	 * the GPU where there is ARB_indirect_parameters (OpenGL 4.6) */
	gx.multiDrawElementsIndirectCount = NULL;
	if (gl_version_at_least(4, 6))
		gl_function(&gx.multiDrawElementsIndirectCount, get, "glMultiDrawElementsIndirectCount", "glMultiDrawElementsIndirectCountARB");
	else if (gl_has_extension("GL_ARB_indirect_parameters"))
		gl_function(&gx.multiDrawElementsIndirectCount, get, "glMultiDrawElementsIndirectCountARB", NULL);
	glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &blocks);
	glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &bindings);
	glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
	glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &blockSize);
	/* (and the prepass's kernel's sorted indices after them) */
	if (blocks < B_COUNT + 1 || bindings < TAG_GL_BINDING + B_COUNT + 2)
		return no("not enough storage buffers for the kernels");
	if (align <= 0 || align > 4096)
		align = 256;

	ta_gpu_sizes(&sz);
	strips = sz.list[0] + sz.list[1] + sz.list[2];
	workWords = tag_work_layout(sz.verts, strips, sz.headers, work);
	r->slice = ta_data_size();
	/* The contexts: every frame in flight, one being drawn, and the few the
	 * game writes to at a time and renders to textures with; and spares, as
	 * many as a thirty-second of the memory there is to use allows, up to
	 * four more (Vulkan's rule): a frame that finds them all taken is drawn
	 * from the CPU's parse */
	budget = (u64)vram_budget_available_kib(&gl.vram_budget) * 1024 / 32;
	spares = budget / r->slice < 4 ? (u32)(budget / r->slice) : 4;
	r->slices = TA_GL_FRAMES + 4 + spares < 16 ? TA_GL_FRAMES + 4 + spares : 16;
	alignWords = (u32)align / 4;
	r->tableWords = (sz.table_words + alignWords - 1) / alignWords * alignWords;
	r->tableWords = (r->tableWords + 63) & ~63u;
	trdSlots = sz.verts + sz.headers;
	most = blockSize > 0 ? (u64)blockSize : 0;
	if ((u64)r->slice > most || (u64)sz.verts * TAG_V_WORDS * 4 > most || (u64)workWords * 4 > most
			|| (u64)trdSlots * TAG_C_WORDS * 4 > most || (u64)sz.headers * 2 * TAG_C_WORDS * 4 > most)
		return no("buffers larger than the driver's storage blocks");

	/* the kernels: from SPIR-V; else from the text, kept for the next time */
	t0 = cpu_features_get_time_usec();
	made = "SPIR-V";
	kernels = kernels_from_spirv(r->programs);
	if (!kernels)
	{
		delete_programs(r->programs);
		made = "text, or the programs kept";
		kernels = kernels_from_text(r->programs);
	}
	if (!kernels)
		return no("the kernels did not compile");
	kernelMs = (double)(cpu_features_get_time_usec() - t0) / 1000.0;

	/* The contexts' data: read back by the SH4 and by save states, so
	 * memory of the client's that reads at the speed of memory */
	r->pool = make_buffer((GLsizeiptr)r->slice * r->slices, mapped | GL_CLIENT_STORAGE_BIT, &map, NULL);
	r->poolMap = (u8 *)map;
	if (r->poolMap == NULL)
		return no("the TA contexts' buffer could not be mapped");
	if (!mapping_reads_fast(r->poolMap, 1 << 20))
		return no("its mapped memory is slow to read (write-combined)");
	r->buffers[B_TAB] = make_buffer((GLsizeiptr)r->tableWords * 4 * TA_GL_FRAMES, mapped | GL_CLIENT_STORAGE_BIT, &map, NULL);
	r->tables = (u32 *)map;
	if (r->tables == NULL)
		return no("the tables could not be mapped");
	r->buffers[B_VTX] = make_buffer((GLsizeiptr)sz.verts * TAG_V_WORDS * 4, 0, NULL, NULL);
	r->buffers[B_MOD] = make_buffer((GLsizeiptr)sz.modtrig * 9 * 4 + 4, 0, NULL, NULL);
	r->buffers[B_IDX] = make_buffer((GLsizeiptr)sz.idx * 4, 0, NULL, NULL);
	r->buffers[B_WORK] = make_buffer((GLsizeiptr)workWords * 4, 0, NULL, NULL);
	r->buffers[B_REC] = make_buffer((GLsizeiptr)strips * TAG_C_WORDS * 4, 0, NULL, NULL);
	/* (and a second command for each, for shadows: TAG_T_SHD_CMD) */
	r->buffers[B_CMD] = make_buffer((GLsizeiptr)sz.headers * 2 * TAG_C_WORDS * 4, 0, NULL, NULL);
	r->buffers[B_FRM] = make_buffer(TAG_O_WORDS * 4, 0, NULL, NULL);
	r->buffers[B_TRD] = make_buffer((GLsizeiptr)trdSlots * TAG_C_WORDS * 4, 0, NULL, NULL);
	r->trParamsBytes = sz.list[2] * TAG_P_WORDS * 4;
	r->buffers[B_TRP] = make_buffer(r->trParamsBytes, 0, NULL, NULL);
	/* the per-pixel renderer's strip numbers (TA_GPU_INSTANCE): 0, 1, 2... */
	{
		u32 *numbers = (u32 *)malloc((size_t)sz.list[2] * 4 + 4);
		u32 n;
		if (numbers == NULL)
			return no("no memory");
		for (n = 0; n < sz.list[2]; n++)
			numbers[n] = n;
		r->instances = make_buffer((GLsizeiptr)sz.list[2] * 4, 0, NULL, numbers);
		free(numbers);
	}
	/* The sorted indices: three a triangle, for as many triangles as a frame
	 * can have indices, and as many again for the depth prepass, and the
	 * prepass's checks (TAG_T_PREP), which the CPU writes once and the GPU
	 * reads once - but for the prepass's triangles the prepass's kernel
	 * makes nothing of (TAG_K_PREPASS) */
	if (ta.sortWanted)
	{
		const GLintptr slot = ((GLintptr)sz.idx * 2 * 3 * 4 + 255) & ~(GLintptr)255;
		r->sorted = make_buffer(slot * TA_GL_FRAMES, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT, &map, NULL);
		r->sortedMap = (u8 *)map;
		r->sortSlot = r->sortedMap != NULL ? slot : 0;
	}
	glGenQueries(2 * TA_GL_FRAMES, r->queries);
	if (glGetError() != GL_NO_ERROR)
		return no("its buffers could not be made");

	r->layout.words[0] = 0;		/* (each slice is bound from its start) */
	for (i = 0; i < 9; i++)
		r->layout.words[1 + i] = work[i];
	r->layout.words[10] = 0;
	r->layout.words[11] = sz.list[0] * TAG_C_WORDS;
	r->layout.words[12] = (sz.list[0] + sz.list[1]) * TAG_C_WORDS;
	r->shadowCommands = (GLintptr)sz.headers * TAG_C_WORDS * 4;

	if (!tactx_gpu_pool_attach(r->poolMap, r->slice, r->slices))
		return no("the TA contexts' pool");
	ta_set_gpu_processing(true);
	if (!ta_gpu_processing())
	{
		tactx_gpu_pool_detach();
		return no("the header pass's memory");
	}
	if (r->sortSlot != 0 && !ta_gpu_sort_reserve(true))
		r->sortSlot = 0;
	memset(&ta.draw, 0, sizeof(ta.draw));
	ta.draw.vertices = r->buffers[B_VTX];
	ta.draw.indices = r->buffers[B_IDX];
	ta.draw.modtrig = r->buffers[B_MOD];
	ta.draw.trParams = r->buffers[B_TRP];
	ta.draw.frame = r->buffers[B_FRM];
	ta.draw.instances = r->instances;
	ta.draw.sorted = r->sortSlot != 0 ? r->sorted : 0;
	ta.sortable = r->sortSlot != 0;
	ta.serial = 0;
	ta.done = 0;
	ta.index = 0;
	NOTICE_LOG(RENDERER, "OpenGL: TA processing on the GPU (%u frames of %u MB mapped, %u KB of tables a frame; "
			"the kernels from %s in %.1f ms; translucent groups drawn %s%s)\n",
			r->slices, r->slice >> 20, r->tableWords / 256, made, kernelMs,
			gx.multiDrawElementsIndirectCount != NULL ? "with their count" : "whole",
			r->sortSlot != 0 ? "; translucent triangles sorted from the frame's depths" : "");
	return true;
}

static void ta_log(const char *when)
{
	u32 shortFrames, allocated;
	tactx_gpu_pool_stats(&shortFrames, &allocated);
	NOTICE_LOG(RENDERER, "OpenGL: TA processing%s: %llu frames decoded and drawn on the GPU, %llu parsed on the CPU "
			"(%llu without a table, %llu with the GPU behind, %llu frame skipping, %llu sorted on the CPU, %llu the header pass leaves to the parse, "
			"%llu might run out of room); GPU time mean %.3f ms, max %.3f ms; pool: %u frames found it full, %u contexts allocated; "
			"%llu with a sorted pass (the sort mean %.1f us, max %.1f us), %llu with shadows the GPU decides\n",
			when, (unsigned long long)ta.statFrames, (unsigned long long)ta.statSkipped,
			(unsigned long long)ta.statWhy[TA_GPU_WHY_TABLE], (unsigned long long)ta.statBusy, (unsigned long long)ta.statWhy[TA_GPU_WHY_PARSE],
			(unsigned long long)ta.statWhy[TA_GPU_WHY_SORT], (unsigned long long)ta.statWhy[TA_GPU_WHY_WALK],
			(unsigned long long)ta.statWhy[TA_GPU_WHY_ROOM],
			ta.statGpuTimed ? ta.statGpuMs / ta.statGpuTimed : 0.0, ta.statGpuMaxMs, shortFrames, allocated,
			(unsigned long long)ta.statSorted, ta.statSorted ? ta.statSortUs / ta.statSorted : 0.0, ta.statSortMaxUs,
			(unsigned long long)ta.statShadowed);
}

static void bind_indirect(GLuint buffer, bool parameter)
{
	GLuint *bound = parameter ? &ta.boundParameter : &ta.boundIndirect;
	if (*bound == buffer)
		return;
	*bound = buffer;
	glBindBuffer(parameter ? GL_PARAMETER_BUFFER_ARB : GL_DRAW_INDIRECT_BUFFER, buffer);
}

void gltad_term(void)
{
	if (!ta.ready)
		return;
	ta_log("");
	/* The frames in the pool go on in memory of their own; the buffers go
	 * now, and OpenGL keeps what the GPU still reads until it is done */
	ta_set_gpu_processing(false);
	tactx_gpu_pool_detach();
	bind_indirect(0, false);
	bind_indirect(0, true);
	resources_free(ta.res);
	ta.res = NULL;
	ta.ready = false;
	ta.sortable = false;
	ta.prepared = false;
	ta.recorded = false;
	memset(&ta.draw, 0, sizeof(ta.draw));
}

void gltad_setup(bool wanted)
{
	retro_time_t start;
	Resources *r;
	if (!wanted)
	{
		/* (turned off: and tried again when it is turned on) */
		gltad_term();
		ta.failed = false;
		return;
	}
	if (ta.ready || ta.failed)
		return;
	start = cpu_features_get_time_usec();
	r = (Resources *)calloc(1, sizeof(Resources));
	if (r == NULL)
	{
		no("no memory");
		return;
	}
	if (init_resources(r))
	{
		ta.res = r;
		ta.ready = true;
		NOTICE_LOG(RENDERER, "OpenGL: TA processing set up in %.1f ms\n", (double)(cpu_features_get_time_usec() - start) / 1000.0);
	}
	else
		resources_free(r);
}

bool gltad_ready(void)
{
	return ta.ready;
}

/* Every frame up to the newest whose fence has signalled is done: a fence
 * signals once all that was given before it is */
static void poll_done(void)
{
	Resources *r = ta.res;
	int s;
	for (s = 0; s < TA_GL_FRAMES; s++)
	{
		GLenum st;
		if (r->fences[s] == NULL)
			continue;
		st = glClientWaitSync(r->fences[s], 0, 0);
		if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED)
			continue;
		glDeleteSync(r->fences[s]);
		r->fences[s] = NULL;
		if ((s32)(r->slotSerial[s] - ta.done) > 0)
			ta.done = r->slotSerial[s];
		if (r->timed[s])
		{
			GLint available = 0;
			r->timed[s] = false;
			gx.getQueryObjectiv(r->queries[2 * s + 1], GL_QUERY_RESULT_AVAILABLE, &available);
			if (available)
			{
				GLuint64 q0 = 0, q1 = 0;
				double ms;
				gx.getQueryObjectui64v(r->queries[2 * s], GL_QUERY_RESULT, &q0);
				gx.getQueryObjectui64v(r->queries[2 * s + 1], GL_QUERY_RESULT, &q1);
				ms = (double)(q1 - q0) / 1e6;
				ta.statGpuMs += ms;
				if (ms > ta.statGpuMaxMs)
					ta.statGpuMaxMs = ms;
				ta.statGpuTimed++;
			}
		}
	}
	tactx_gpu_done(ta.done);
}

void gltad_begin_frame(void)
{
	if (!ta.ready)
		return;
	ta.prepared = false;
	ta.recorded = false;
	if (++ta.serial == 0)
		ta.serial = 1;
	poll_done();
	ta.index = (ta.index + 1) % TA_GL_FRAMES;
	/* (its fence not yet signalled: its tables and sorted indices may still be read) */
	ta.busy = ta.res->fences[ta.index] != NULL;
	if (ta.sortable)
	{
		ta.draw.sortedOffset = (GLintptr)ta.index * ta.res->sortSlot;
		ta.draw.sortedMap = (u32 *)(ta.res->sortedMap + ta.draw.sortedOffset);
	}
}

bool gltad_prepare(TA_context *ctx, u32 want, bool *rv)
{
	ta.prepared = false;
	if (ctx->pool < 0 || ctx->rend.isRenderFramebuffer || ta.busy)
	{
		/* (a frame that found the pool full is in memory the GPU cannot read) */
		ta.statSkipped++;
		if (ta.busy && ctx->pool >= 0 && !ctx->rend.isRenderFramebuffer)
			ta.statBusy++;
		else
			ta.statWhy[TA_GPU_WHY_TABLE]++;
		return false;
	}
	ta.flags = want;
	ta.prepared = ta_gpu_prepare(ctx, &ta.res->layout, ta.res->tables + (size_t)ta.index * ta.res->tableWords,
			ta.res->tableWords, &ta.frame, ta.flags);
	if (!ta.prepared)
	{
		ta.statSkipped++;
		ta.statWhy[ta.frame.why < TA_GPU_WHY_COUNT ? ta.frame.why : TA_GPU_WHY_WALK]++;
		return false;
	}
	*rv = ta.frame.rv;
	if (ta_gpu_sorted_indices() != 0)
	{
		const double us = ta_gpu_sorted_ns() / 1e3;
		ta.statSorted++;
		ta.statSortUs += us;
		if (us > ta.statSortMaxUs)
			ta.statSortMaxUs = us;
	}
	if (ta.frame.shadows != 0)
		ta.statShadowed++;
	if (!ta.frame.rv)
		ta.prepared = false;
	return true;
}

static void clear_words(GLuint buffer, GLintptr at, GLsizeiptr bytes, u32 value)
{
	glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
	gx.clearBufferSubData(GL_COPY_WRITE_BUFFER, GL_R32UI, at, bytes, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
}

/* The decode's steps: kernel @k over @groups workgroups (none: only the
 * barrier), or as many as the frame's words at @at say */
#define TAD_STEP (GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT)
static void dispatch(int k, u32 groups)
{
	glc_use_program(ta.res->programs[k]);
	if (groups != 0)
		gx.dispatchCompute(groups, 1, 1);
	glMemoryBarrier(TAD_STEP);
}

static void indirect(int k, u32 at)
{
	glc_use_program(ta.res->programs[k]);
	gx.dispatchComputeIndirect((GLintptr)at * 4);
	glMemoryBarrier(TAD_STEP);
}

void gltad_record(TA_context *ctx)
{
	Resources *r;
	u32 checks, scanGroups;
	GLuint frm;
	int b;
	if (!ta.prepared)
		return;
	ta.prepared = false;
	r = ta.res;
	if (r->queries[0] != 0)
		gx.queryCounter(r->queries[2 * ta.index], GL_TIMESTAMP);

	/* The sorted translucent indices (TA_GPU_SORT), and the prepass's checks
	 * after them, are the CPU's: written now, before the kernel that reads
	 * them is given (the mapping is coherent) */
	checks = ta.draw.sortedMap != NULL && ta_gpu_sorted_indices() != 0 ? ta_gpu_prepass_checks() : 0;
	gltad_write_sorted();

	/* what is not written this frame is left from the one before: these are */
	frm = r->buffers[B_FRM];
	clear_words(frm, 0, TAG_O_WORDS * 4, 0);
	clear_words(frm, TAG_O_ZMAX * 4, 4, 0x3F800000u);	/* fZ_max starts at 1 */
	if (ta.frame.slots != 0)
		clear_words(r->buffers[B_WORK], (GLintptr)r->layout.words[9] * 4, (GLsizeiptr)ta.frame.slots * 4, 0);
	if (ta.frame.trd != 0)
		clear_words(r->buffers[B_TRD], 0, (GLsizeiptr)ta.frame.trd * TAG_C_WORDS * 4, 0);
	glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

	gx.bindBufferRange(GL_SHADER_STORAGE_BUFFER, TAG_GL_BINDING, r->pool, (GLintptr)ctx->pool * r->slice, r->slice);
	gx.bindBufferRange(GL_SHADER_STORAGE_BUFFER, TAG_GL_BINDING + 1 + B_TAB, r->buffers[B_TAB],
			(GLintptr)ta.index * r->tableWords * 4, (GLsizeiptr)r->tableWords * 4);
	for (b = B_VTX; b < B_COUNT; b++)
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TAG_GL_BINDING + 1 + b, r->buffers[b]);
	/* (the prepass's kernel's: after the others) */
	if (checks != 0)
		gx.bindBufferRange(GL_SHADER_STORAGE_BUFFER, TAG_GL_BINDING + 1 + B_COUNT, ta.draw.sorted, ta.draw.sortedOffset, (GLsizeiptr)r->sortSlot);
	glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, frm);

	scanGroups = (ta.frame.verts + TAG_SCAN_N - 1) / TAG_SCAN_N;
	dispatch(TAG_K_UNPACK, (ta.frame.verts + ta.frame.modtrig + 63) / 64);
	/* (the vertices as unpacked, before TAG_K_PROVOKE moves colours) */
	if (checks != 0)
		dispatch(TAG_K_PREPASS, checks < 65535u ? checks : 65535u);
	dispatch(TAG_K_COUNT, scanGroups);
	dispatch(TAG_K_COUNTSUM, 1);
	dispatch(TAG_K_ENDS, scanGroups);
	indirect(TAG_K_SUMMARY, TAG_O_ARGS64);
	indirect(TAG_K_GROUPS, TAG_O_ARGSCH);
	dispatch(TAG_K_GROUPSUM, 1);
	indirect(TAG_K_STATES, TAG_O_ARGSCH);
	indirect(TAG_K_EMIT, TAG_O_ARGS64);
	dispatch(TAG_K_FINISH, (ta.frame.slots + 63) / 64);
	/* (a workgroup for each group named, up to the most a dispatch can have: then each takes several) */
	if (ta.frame.shadows != 0)
		dispatch(TAG_K_SHADOW, ta.frame.shadows < 65535u ? ta.frame.shadows : 65535u);
	if (ta.frame.trd != 0)
		indirect(TAG_K_TRDRAW, TAG_O_ARGS64);
	if (screen_height > 480)
		indirect(TAG_K_BLEED, TAG_O_ARGS1);
	if (ta.flags & TA_GPU_PROVOKE)
		indirect(TAG_K_PROVOKE, TAG_O_ARGS1);
	glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
	/* for whatever draws from them (and the renderers' copy of 1/fZ_max out of the frame's words) */
	glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT | GL_COMMAND_BARRIER_BIT
			| GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
	if (r->queries[0] != 0)
	{
		gx.queryCounter(r->queries[2 * ta.index + 1], GL_TIMESTAMP);
		r->timed[ta.index] = true;
	}
	/* the context's slice is read until this frame is done */
	ctx->gpu_serial = ta.serial;
	ta.recorded = true;
	if (++ta.statFrames % 600 == 0)
		ta_log(" so far");
}

void gltad_end_frame(void)
{
	if (!ta.ready)
		return;
	bind_indirect(0, false);
	bind_indirect(0, true);
	if (!ta.recorded)
		return;
	ta.recorded = false;
	ta.res->fences[ta.index] = (GLsync)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	ta.res->slotSerial[ta.index] = ta.serial;
}

const GlTaDraw *gltad_draw(void)
{
	return &ta.draw;
}

void gltad_want_sort(bool on)
{
	ta.sortWanted = on;
}

bool gltad_can_sort(void)
{
	return ta.sortable;
}

void gltad_draw_group(u32 list_type, u32 first, u32 count, bool shadows)
{
	const Resources *r = ta.res;
	const GLsizei stride = TAG_C_WORDS * 4;
	if (list_type != ListType_Translucent || shadows)
	{
		bind_indirect(r->buffers[B_CMD], false);
		gx.drawElementsIndirect(GL_TRIANGLE_STRIP, GL_UNSIGNED_INT,
				(const void *)((shadows ? r->shadowCommands : 0) + (GLintptr)first * stride));
	}
	else if (gx.multiDrawElementsIndirectCount != NULL)
	{
		bind_indirect(r->buffers[B_TRD], false);
		bind_indirect(r->buffers[B_TRD], true);
		gx.multiDrawElementsIndirectCount(GL_TRIANGLE_STRIP, GL_UNSIGNED_INT, (const void *)((GLintptr)(first + 1) * stride), (GLintptr)first * stride,
				(GLsizei)count, stride);
	}
	else
	{
		bind_indirect(r->buffers[B_TRD], false);
		gx.multiDrawElementsIndirect(GL_TRIANGLE_STRIP, GL_UNSIGNED_INT, (const void *)((GLintptr)(first + 1) * stride),
				(GLsizei)count, stride);
	}
}

void gltad_bind_geometry(void)
{
	glBindBuffer(GL_ARRAY_BUFFER, ta.draw.vertices);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ta.draw.indices);
}

void gltad_bind_modvols(void)
{
	glBindBuffer(GL_ARRAY_BUFFER, ta.draw.modtrig);
}

void gltad_write_sorted(void)
{
	if (ta.draw.sortedMap != NULL && ta_gpu_sorted_indices() != 0)
		ta_gpu_sorted_write(ta.draw.sortedMap);
}

GLintptr gltad_bind_sorted(void)
{
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ta.draw.sorted);
	return ta.draw.sortedOffset;
}

#endif
