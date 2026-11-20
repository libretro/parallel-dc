/* Textures made on the GPU from their bytes in video memory: see
 * gl_texdecoder.h */
#include <stdlib.h>
#include <string.h>
#include "gles.h"
#include "gl_texdecoder.h"
#include "glprogram_cache.h"

/* The kernels' text (rend/texdecode.h), and their SPIR-V for OpenGL 4.6:
 * tools/rend/kernels_spv.sh made it, and checks that it is the header's */
#include "rend/texdecode_gl.inc"

#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x0002
#endif
#ifndef GL_TEXTURE_FETCH_BARRIER_BIT
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x00000008
#endif
#ifndef GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT
#define GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT 0x00004000
#endif
#ifndef GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#endif
#ifndef GL_SHADER_BINARY_FORMAT_SPIR_V
#define GL_SHADER_BINARY_FORMAT_SPIR_V 0x9551
#endif
#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif
#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#endif
#ifndef GL_NUM_PROGRAM_BINARY_FORMATS
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
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
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_R32UI
#define GL_R32UI 0x8236
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER 0x8D94
#endif
#ifndef GL_RGBA8UI
#define GL_RGBA8UI 0x8D7C
#endif
#ifndef GL_R8UI
#define GL_R8UI 0x8232
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4_REV
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif

#ifndef HAVE_OPENGLES2
#ifndef APIENTRY
#define APIENTRY
#endif
/* What glsm does not wrap: the entry points the frontend's loader
 * (glsym/glsym_gl.c) has made, which the glsm header does not name */
extern void (APIENTRY *__rglgen_glDispatchCompute)(GLuint, GLuint, GLuint);
extern void (APIENTRY *__rglgen_glShaderBinary)(GLsizei, const GLuint *, GLenum, const void *, GLsizei);
extern void (APIENTRY *__rglgen_glSpecializeShader)(GLuint, const GLchar *, GLuint, const GLuint *, const GLuint *);
#define glDispatchCompute __rglgen_glDispatchCompute
#define glShaderBinary __rglgen_glShaderBinary
#define glSpecializeShader __rglgen_glSpecializeShader
#endif

/* The binding points and the texture unit the kernels use: above what the
 * renderers use (their storage buffers 0 and 1, image unit 4, texture
 * units 0 to 5) */
#define TEXD_GL_BINDING 7

enum { MAX_JOBS = 2048, MAX_LEVELS = 4096, MAX_FENCES = 8 };
enum { PROGRAM_32, PROGRAM_8, PROGRAM_DRAW, PROGRAMS };

typedef struct Job
{
	GLuint texture;
	u32 first, count;	/* levels; a copy has none */
	u32 offset, rowLength, width, height;	/* a copy's */
	u8 type;
	bool copy;
	bool cancelled;
} Job;

typedef struct Level
{
	u32 pc[6];
	u32 mip;
} Level;

typedef struct Pending
{
	void *fence;		/* GLsync */
	u64 end;
} Pending;

static struct
{
	/* what has been done so far, for measuring */
	u64 statBytes, statLevels, statCopies, statFull;

	bool ready;
	bool compute;		/* OpenGL 4.3: compute kernels, the ring a storage buffer */
	bool persistent;	/* the ring stays mapped */
	bool fences;
	bool takes[5];
	bool native[3];

	GLuint ring;		/* the buffer */
	GLuint ringTexture;	/* OpenGL 3: what the kernels read the ring from */
	u32 ringRows;
	u8 *mapped;		/* the ring, mapped; or this side's memory for it */
	u8 *shadow;
	u64 size;
	u64 head, tail, flushed;	/* bytes handed out, given back, and up to where they went */
	Pending pending[MAX_FENCES];
	u32 pendingCount;

	GLuint programs[PROGRAMS];
	GLint locations[PROGRAMS][2];
	GLuint framebuffer;
	GLuint vertexArray;

	Job jobs[MAX_JOBS];
	Level levels[MAX_LEVELS];
	u32 jobCount, levelCount;
} td;
/* (what gltd_cancel() is told a job was added under) */
static u32 td_serial = 1;

/* The format and type of a texture of a type (TEX_TYPE_*), as its planar
 * texels go to it as they are */
static void format_of(int type, GLenum *format, GLenum *gltype)
{
	switch (type)
	{
	case TEX_TYPE_565: *format = GL_RGB; *gltype = GL_UNSIGNED_SHORT_5_6_5; break;
	case TEX_TYPE_5551: *format = GL_BGRA; *gltype = GL_UNSIGNED_SHORT_1_5_5_5_REV; break;
	default: *format = GL_BGRA; *gltype = GL_UNSIGNED_SHORT_4_4_4_4_REV; break;
	}
}

#ifndef HAVE_OPENGLES2
static bool has_extension(const char *name)
{
	GLuint i;
	for (i = 0; ; i++)
	{
		const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, i);
		if (extension == NULL)
			break;
		if (!strcmp(extension, name))
			return true;
	}
	while (glGetError() != GL_NO_ERROR)
		;
	return false;
}

static bool version_at_least(int major, int minor)
{
	return gl.gl_major > major || (gl.gl_major == major && gl.gl_minor >= minor);
}

static void delete_programs(void)
{
	int p;
	for (p = 0; p < PROGRAMS; p++)
	{
		if (td.programs[p] != 0)
			glDeleteProgram(td.programs[p]);
		td.programs[p] = 0;
	}
}

/* ---- The programs ---- */

/* A program of @stages shaders, from SPIR-V if @spv, from text otherwise.
 * 0 if it does not link. */
static GLuint link_program(const u32 *spv, const size_t *spvSize, const char *const *sources, int stages, bool keep)
{
	static const GLenum computeStage[] = { GL_COMPUTE_SHADER };
	static const GLenum drawStages[] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	const GLenum *kinds = stages == 1 ? computeStage : drawStages;
	GLuint program = glCreateProgram();
	GLuint shaders[2];
	bool ok = true;
	int i;
	shaders[0] = shaders[1] = 0;
	for (i = 0; i < stages && ok; i++)
	{
		GLint status = GL_FALSE;
		shaders[i] = glCreateShader(kinds[i]);
		if (spv != NULL)
		{
			glShaderBinary(1, &shaders[i], GL_SHADER_BINARY_FORMAT_SPIR_V, (const u8 *)spv + (i == 0 ? 0 : spvSize[0]), (GLsizei)spvSize[i]);
			glSpecializeShader(shaders[i], "main", 0, NULL, NULL);
		}
		else
		{
			const GLchar *text = sources[i];
			glShaderSource(shaders[i], 1, &text, NULL);
			glCompileShader(shaders[i]);
		}
		glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &status);
		if (status != GL_TRUE)
		{
			char log[1024];
			log[0] = 0;
			glGetShaderInfoLog(shaders[i], sizeof(log), NULL, log);
			WARN_LOG(RENDERER, "OpenGL: a texture kernel did not compile: %s\n", log);
			ok = false;
			break;
		}
		glAttachShader(program, shaders[i]);
	}
	if (ok)
	{
		GLint status = GL_FALSE;
		if (keep)
			glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		ok = status == GL_TRUE;
	}
	for (i = 0; i < stages; i++)
		if (shaders[i] != 0)
		{
			if (ok)
				glDetachShader(program, shaders[i]);
			glDeleteShader(shaders[i]);
		}
	if (!ok)
	{
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

/* The text of kernel @kernel: @head, then the header (made when the
 * renderer starts: the caller frees it) */
static char *kernel_text(const char *head, int kernel)
{
	const size_t n = strlen(head) + 64 + sizeof(texdecode_glsl);
	char *t = (char *)malloc(n);
	if (t != NULL)
		sprintf(t, "%s#define TEXD_KERNEL_GL %d\n%s", head, kernel, texdecode_glsl);
	return t;
}

/* The programs, all of them now: from SPIR-V with OpenGL 4.6; else from
 * their text, through the programs kept on disk (glprogram_cache.h) */
static bool make_programs(void)
{
	const bool spirv = td.compute && (version_at_least(4, 6) || has_extension("GL_ARB_gl_spirv"));
	char head[96];
	char *text[4];
	bool ok;
	int p;
	if (spirv)
	{
		/* (the two stages' words one after the other) */
		static u32 draw[sizeof(texdecode_glspv_3) / 4 + sizeof(texdecode_glspv_4) / 4];
		size_t s32[1], s8[1], sd[2];
		s32[0] = texdecode_glspv_size[1];
		s8[0] = texdecode_glspv_size[2];
		td.programs[PROGRAM_32] = link_program(texdecode_glspv[1], s32, NULL, 1, false);
		td.programs[PROGRAM_8] = link_program(texdecode_glspv[2], s8, NULL, 1, false);
		memcpy(draw, texdecode_glspv_3, sizeof(texdecode_glspv_3));
		memcpy(draw + sizeof(texdecode_glspv_3) / 4, texdecode_glspv_4, sizeof(texdecode_glspv_4));
		sd[0] = sizeof(texdecode_glspv_3);
		sd[1] = sizeof(texdecode_glspv_4);
		td.programs[PROGRAM_DRAW] = link_program(draw, sd, NULL, 2, false);
		if (td.programs[PROGRAM_32] != 0 && td.programs[PROGRAM_8] != 0 && td.programs[PROGRAM_DRAW] != 0)
		{
			for (p = 0; p < PROGRAMS; p++)
			{
				/* (the locations the kernels give their uniforms: SPIR-V has no names) */
				td.locations[p][0] = 0;
				td.locations[p][1] = 1;
			}
			NOTICE_LOG(RENDERER, "OpenGL: texture kernels from SPIR-V\n");
			return true;
		}
		delete_programs();
		WARN_LOG(RENDERER, "OpenGL: the texture kernels' SPIR-V was refused: compiled instead\n");
	}

	/* The text of each: the version this context has, then the header */
	sprintf(head, "%s#define TEXD_SHADER_GLSL\n%s",
			td.compute ? "#version 430\n" : version_at_least(3, 3) ? "#version 330\n" : version_at_least(3, 2) ? "#version 150\n" : "#version 130\n",
			td.compute ? "#define TEXD_GL_RING 1\n" : "#define TEXD_GL_RING 2\n");
	for (p = 0; p < 4; p++)
		text[p] = kernel_text(head, p + 1);
	ok = text[0] != NULL && text[1] != NULL && text[2] != NULL && text[3] != NULL;

	/* Through the programs kept on disk (glprogram_cache.h): compiled the
	 * first time, taken from there after, with the renderers' own */
	if (ok)
	{
		td.programs[PROGRAM_DRAW] = glprog_make(text[2], text[3], false);
		if (glprog_failed())
			td.programs[PROGRAM_DRAW] = 0;
		if (td.compute)
		{
			const char *k32 = text[0], *k8 = text[1];
			td.programs[PROGRAM_32] = glprog_make_compute(&k32, 1);
			td.programs[PROGRAM_8] = glprog_make_compute(&k8, 1);
		}
	}
	for (p = 0; p < 4; p++)
		free(text[p]);
	if (!ok || td.programs[PROGRAM_DRAW] == 0 || (td.compute && (td.programs[PROGRAM_32] == 0 || td.programs[PROGRAM_8] == 0)))
		return false;
	NOTICE_LOG(RENDERER, "OpenGL: texture kernels from their text, or the programs kept\n");
	for (p = 0; p < PROGRAMS; p++)
	{
		if (td.programs[p] == 0)
			continue;
		td.locations[p][0] = glGetUniformLocation(td.programs[p], "texd_lv0");
		td.locations[p][1] = glGetUniformLocation(td.programs[p], "texd_lv1");
		if (!td.compute)
		{
			glc_use_program(td.programs[p]);
			glUniform1i(glGetUniformLocation(td.programs[p], "texd_ring"), TEXD_GL_BINDING);
		}
	}
	return true;
}
#endif

void gltd_init(void)
{
#if !defined(HAVE_OPENGLES2) && !defined(MSB_FIRST)
	/* What can be drawn into: a framebuffer with a level of a texture of
	 * each format, tried once now */
	static const GLenum internal[5] = { GL_RGB565, GL_RGB5_A1, GL_RGBA4, GL_RGBA8, GL_R8 };
	GLint was = 0;
	u64 bytes;
	int t;
#endif
	gltd_term();
#if !defined(HAVE_OPENGLES2) && !defined(MSB_FIRST)
	/* Desktop OpenGL 3 and later. (The kernels read the bytes little-endian,
	 * which a big-endian machine does not have them.) */
	if (gl.is_gles || gl.gl_major < 3)
	{
		INFO_LOG(RENDERER, "OpenGL: textures are converted on the CPU\n");
		return;
	}
	td.compute = version_at_least(4, 3);
	td.persistent = td.compute && (version_at_least(4, 4) || has_extension("GL_ARB_buffer_storage"));
	td.fences = version_at_least(3, 2) || has_extension("GL_ARB_sync");

	/* The ring: what textures take as they are in video memory - a few
	 * hundred KB in most frames, megabytes when a scene is loaded - a share
	 * of the video memory, 4 to 16 MB */
	bytes = (u64)vram_budget_available_kib(&gl.vram_budget) * 1024 / 256;
	if (bytes < (u64)4 << 20)
		bytes = (u64)4 << 20;
	if (bytes > (u64)16 << 20)
		bytes = (u64)16 << 20;
	if (!td.compute)
	{
		/* (as a texture of TEXD_GL_RING_WIDTH words a row: as many rows as the driver has) */
		GLint most = 0;
		u64 rows;
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &most);
		rows = bytes / (TEXD_GL_RING_WIDTH * 4);
		if (rows > (u64)(most > 0 ? most : 0))
			rows = (u64)(most > 0 ? most : 0);
		td.ringRows = (u32)rows;
		bytes = (u64)td.ringRows * TEXD_GL_RING_WIDTH * 4;
	}
	td.size = bytes & ~(u64)15;
	if (td.size < (1u << 20))
	{
		INFO_LOG(RENDERER, "OpenGL: textures are converted on the CPU (no room for a ring)\n");
		return;
	}
	while (glGetError() != GL_NO_ERROR)
		;
	if (td.compute)
	{
		glGenBuffers(1, &td.ring);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, td.ring);
		if (td.persistent)
		{
			glBufferStorage(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)td.size, NULL, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
			td.mapped = (u8 *)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)td.size, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
			if (td.mapped == NULL || glGetError() != GL_NO_ERROR)
			{
				/* (written through memory of this side's after all) */
				td.mapped = NULL;
				td.persistent = false;
				glDeleteBuffers(1, &td.ring);
				glGenBuffers(1, &td.ring);
				glBindBuffer(GL_SHADER_STORAGE_BUFFER, td.ring);
			}
		}
		if (!td.persistent)
			glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)td.size, NULL, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	}
	else
	{
		glGenTextures(1, &td.ringTexture);
		glActiveTexture(GL_TEXTURE0 + TEXD_GL_BINDING);
		glBindTexture(GL_TEXTURE_2D, td.ringTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, TEXD_GL_RING_WIDTH, td.ringRows, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);
	}
	if (!td.persistent)
	{
		td.shadow = (u8 *)malloc((size_t)td.size);
		td.mapped = td.shadow;
	}
	glGenFramebuffers(1, &td.framebuffer);
	glGenVertexArrays(1, &td.vertexArray);
	if (td.mapped == NULL || glGetError() != GL_NO_ERROR || !make_programs())
	{
		WARN_LOG(RENDERER, "OpenGL: the texture kernels could not be made: textures are converted on the CPU\n");
		gltd_term();
		return;
	}

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was);
	glBindFramebuffer(GL_FRAMEBUFFER, td.framebuffer);
	for (t = 0; t < 5; t++)
	{
		GLuint tex = 0;
		bool drawn;
		glGenTextures(1, &tex);
		glc_bind_texture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, internal[t], 8, 8, 0, t == 4 ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		drawn = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glc_delete_textures(1, &tex);
		/* (32-bit texels and indices by the compute kernels where there are any) */
		td.takes[t] = t >= 3 && td.compute ? true : drawn;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, was);
	td.native[0] = td.native[1] = td.native[2] = true;
	while (glGetError() != GL_NO_ERROR)
		;
	td.head = td.tail = td.flushed = 0;
	td.jobCount = td.levelCount = td.pendingCount = 0;
	td.ready = true;
	NOTICE_LOG(RENDERER, "OpenGL: textures are made on the GPU (%s, a ring of %u KB %s; 16-bit drawn %d%d%d, 32-bit %d, 8-bit %d)\n",
			td.compute ? "compute kernels" : "drawn", (u32)(td.size >> 10), td.persistent ? "mapped" : "written once a frame",
			td.takes[0], td.takes[1], td.takes[2], td.takes[3], td.takes[4]);
#endif
}

void gltd_term(void)
{
	int t;
#ifndef HAVE_OPENGLES2
	u32 i;
#endif
	if (td.ready)
		NOTICE_LOG(RENDERER, "OpenGL: textures made on the GPU: %u levels, %u copied as they are, %u MB of bytes; %u not, for want of room\n",
				(u32)td.statLevels, (u32)td.statCopies, (u32)(td.statBytes >> 20), (u32)td.statFull);
#ifndef HAVE_OPENGLES2
	for (i = 0; i < td.pendingCount; i++)
		glDeleteSync((GLsync)td.pending[i].fence);
	td.pendingCount = 0;
	delete_programs();
	if (td.mapped != NULL && td.persistent && td.ring != 0)
	{
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, td.ring);
		glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	}
	if (td.ring != 0)
		glDeleteBuffers(1, &td.ring);
	if (td.ringTexture != 0)
		glc_delete_textures(1, &td.ringTexture);
	if (td.framebuffer != 0)
		glDeleteFramebuffers(1, &td.framebuffer);
	if (td.vertexArray != 0)
		glDeleteVertexArrays(1, &td.vertexArray);
#endif
	td.ring = td.ringTexture = td.framebuffer = td.vertexArray = 0;
	free(td.shadow);
	td.shadow = NULL;
	td.mapped = NULL;
	td.ready = td.compute = td.persistent = td.fences = false;
	for (t = 0; t < 5; t++)
		td.takes[t] = false;
	td.native[0] = td.native[1] = td.native[2] = false;
	td.jobCount = td.levelCount = 0;
	td.size = td.head = td.tail = td.flushed = 0;
}

bool gltd_ready(void)
{
	return td.ready;
}

bool gltd_takes(int type, u32 out)
{
	if (!td.ready || (unsigned)type >= 5 || !td.takes[type])
		return false;
	/* (the 32-bit and 8-bit kinds are made into their own types) */
	return out == TEXD_OUT16 ? (unsigned)type < 3 : out == TEXD_OUT32 ? type == TEX_TYPE_8888 : type == TEX_TYPE_8;
}

bool gltd_native(int type)
{
	return td.ready && (unsigned)type < 3 && td.native[type];
}

/* ---- The ring ---- */

#ifndef HAVE_OPENGLES2
static void reclaim(void)
{
	/* (the frames are done in the order they went) */
	u32 done = 0, i;
	while (done < td.pendingCount)
	{
		const GLenum r = glClientWaitSync((GLsync)td.pending[done].fence, 0, 0);
		if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
			break;
		glDeleteSync((GLsync)td.pending[done].fence);
		td.tail = td.pending[done].end;
		done++;
	}
	if (done != 0)
	{
		for (i = done; i < td.pendingCount; i++)
			td.pending[i - done] = td.pending[i];
		td.pendingCount -= done;
	}
}
#endif

u8 *gltd_begin(u32 bytes, u32 count, u32 *offset)
{
	int attempt;
	bytes = (bytes + 15) & ~15u;
	if (!td.ready || bytes > td.size || count > MAX_LEVELS)
	{
		td.statFull++;
		return NULL;
	}
	if (td.jobCount >= MAX_JOBS || td.levelCount + count > MAX_LEVELS)
		/* What the lists have goes to the GPU first, its bytes with it: the
		 * bytes are handed out after that */
		gltd_flush();
	for (attempt = 0; attempt < 2; attempt++)
	{
		const u64 at = td.head % td.size;
		const u64 skip = at + bytes > td.size ? td.size - at : 0;
		if (td.head + skip + bytes - td.tail <= td.size)
		{
			td.head += skip;
			*offset = (u32)(td.head % td.size);
			td.head += bytes;
			td.statBytes += bytes;
			return td.mapped + *offset;
		}
#ifndef HAVE_OPENGLES2
		if (attempt == 0 && td.persistent && td.fences)
			reclaim();
#endif
	}
	td.statFull++;
	return NULL;
}

u32 gltd_add_decode(GLuint texture, int type, const struct texd_plan *plan, u32 offset)
{
	Job *job = &td.jobs[td.jobCount];
	const u32 d3d16 = plan->out != TEXD_OUT16 ? 0
			: TEXD_D3D16(type == TEX_TYPE_5551 ? TEXD_D3D_5551 : type == TEX_TYPE_4444 ? TEXD_D3D_4444 : TEXD_D3D_565);
	u32 i;
	job->texture = texture;
	job->first = td.levelCount;
	job->count = plan->levels;
	job->type = (u8)type;
	job->copy = false;
	job->cancelled = false;
	for (i = 0; i < plan->levels; i++)
	{
		const struct texd_level *lv = &plan->level[i];
		Level *l = &td.levels[td.levelCount++];
		l->pc[0] = offset / 4;
		l->pc[1] = lv->data;
		l->pc[2] = plan->pal;
		l->pc[3] = lv->wh;
		l->pc[4] = lv->sr;
		l->pc[5] = lv->mode | d3d16;
		l->mip = lv->mip;
	}
	td.statLevels += plan->levels;
	return td.jobCount++;
}

u32 gltd_add_copy(GLuint texture, int type, u32 offset, u32 row_length, u32 width, u32 height)
{
	Job *job = &td.jobs[td.jobCount];
	job->texture = texture;
	job->first = job->count = 0;
	job->offset = offset;
	job->rowLength = row_length;
	job->width = width;
	job->height = height;
	job->type = (u8)type;
	job->copy = true;
	job->cancelled = false;
	td.statCopies++;
	return td.jobCount++;
}

u32 gltd_serial(void)
{
	return td_serial;
}

void gltd_cancel(u32 job, u32 serial)
{
	if (serial == td_serial && job < td.jobCount)
		td.jobs[job].cancelled = true;
}

#ifndef HAVE_OPENGLES2
/* The ring's bytes from @from to @to (counts, not offsets) to where the
 * kernels read them, written through this side's memory */
static void upload(u64 from, u64 to)
{
	while (from < to)
	{
		const u64 at = from % td.size;
		const u64 n = to - from < td.size - at ? to - from : td.size - at;
		if (td.compute)
			glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)at, (GLsizeiptr)n, td.shadow + at);
		else
		{
			/* (whole rows: what the row has before is what it had, and is copied again) */
			const u32 row = TEXD_GL_RING_WIDTH * 4;
			const u32 first = (u32)(at / row), last = (u32)((at + n + row - 1) / row);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, first, TEXD_GL_RING_WIDTH, last - first, GL_RED_INTEGER, GL_UNSIGNED_INT, td.shadow + (size_t)first * row);
		}
		from += n;
	}
}
#endif

void gltd_flush(void)
{
#ifndef HAVE_OPENGLES2
	GLint wasFramebuffer = 0, wasArray = 0, wasUnit = 0, viewport[4];
	bool copies = false, computed = false, drawn = false;
	u32 live = 0, i, l;
	int p;

	if (!td.ready)
		return;
	td_serial++;
	for (i = 0; i < td.jobCount; i++)
		live += !td.jobs[i].cancelled;
	if (live == 0)
	{
		td.jobCount = td.levelCount = 0;
		if (!td.persistent)
			td.head = td.tail = td.flushed = 0;
		return;
	}

	viewport[0] = viewport[1] = viewport[2] = viewport[3] = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &wasFramebuffer);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &wasArray);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &wasUnit);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glActiveTexture(GL_TEXTURE0 + TEXD_GL_BINDING);

	/* the frame's bytes where the kernels read them */
	if (td.compute)
	{
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, td.ring);
		if (!td.persistent)
			upload(td.flushed, td.head);
		else
			/* (written through the mapping: seen by what is recorded after this) */
			glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TEXD_GL_BINDING, td.ring);
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, td.ringTexture);
		upload(td.flushed, td.head);
	}

	/* The planar textures copied as they are: from the ring itself where it
	 * is a buffer, from this side's memory where it is not */
	for (i = 0; i < td.jobCount; i++)
	{
		const Job *job = &td.jobs[i];
		GLenum format, type;
		if (job->cancelled || !job->copy)
			continue;
		if (!copies)
		{
			copies = true;
			if (td.compute)
				glBindBuffer(GL_PIXEL_UNPACK_BUFFER, td.ring);
		}
		format_of(job->type, &format, &type);
		glBindTexture(GL_TEXTURE_2D, job->texture);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, job->rowLength);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, job->width, job->height, format, type,
				td.compute ? (const void *)(uintptr_t)job->offset : (const void *)(td.shadow + job->offset));
	}
	if (copies)
	{
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		if (td.compute)
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}
	if (!td.compute)
		glBindTexture(GL_TEXTURE_2D, td.ringTexture);
	else
		glBindTexture(GL_TEXTURE_2D, 0);

	/* 32-bit texels and indices by the compute kernels */
	if (td.compute)
		for (p = PROGRAM_32; p <= PROGRAM_8; p++)
		{
			bool bound = false;
			for (i = 0; i < td.jobCount; i++)
			{
				const Job *job = &td.jobs[i];
				const u32 out = p == PROGRAM_32 ? TEXD_OUT32 : TEXD_OUT8;
				if (job->cancelled || job->copy || ((td.levels[job->first].pc[5] >> 6) & 3) != out)
					continue;
				if (!bound)
				{
					glc_use_program(td.programs[p]);
					bound = true;
				}
				for (l = job->first; l < job->first + job->count; l++)
				{
					const Level *lv = &td.levels[l];
					glUniform4ui(td.locations[p][0], lv->pc[0], lv->pc[1], lv->pc[2], lv->pc[3]);
					glUniform2ui(td.locations[p][1], lv->pc[4], lv->pc[5]);
					glBindImageTexture(TEXD_GL_BINDING, job->texture, lv->mip, GL_FALSE, 0, GL_WRITE_ONLY,
							out == TEXD_OUT32 ? GL_RGBA8UI : GL_R8UI);
					glDispatchCompute(((lv->pc[3] & 0xFFFF) + 7) / 8, ((lv->pc[3] >> 16) + 7) / 8, 1);
				}
				computed = true;
			}
		}
	if (computed)
	{
		glBindImageTexture(TEXD_GL_BINDING, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8UI);
		glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	}

	/* and the rest drawn */
	for (i = 0; i < td.jobCount; i++)
	{
		const Job *job = &td.jobs[i];
		u32 out;
		if (job->cancelled || job->copy)
			continue;
		out = (td.levels[job->first].pc[5] >> 6) & 3;
		if (td.compute && out != TEXD_OUT16)
			continue;
		if (!drawn)
		{
			drawn = true;
			glBindFramebuffer(GL_FRAMEBUFFER, td.framebuffer);
			glBindVertexArray(td.vertexArray);
			glc_use_program(td.programs[PROGRAM_DRAW]);
			glc_disable(GL_BLEND);
			glc_disable(GL_DEPTH_TEST);
			glc_disable(GL_STENCIL_TEST);
			glc_disable(GL_SCISSOR_TEST);
			glc_disable(GL_CULL_FACE);
			glDisable(GL_DITHER);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		}
		for (l = job->first; l < job->first + job->count; l++)
		{
			const Level *lv = &td.levels[l];
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, job->texture, lv->mip);
			glViewport(0, 0, lv->pc[3] & 0xFFFF, lv->pc[3] >> 16);
			glUniform4ui(td.locations[PROGRAM_DRAW][0], lv->pc[0], lv->pc[1], lv->pc[2], lv->pc[3]);
			glUniform2ui(td.locations[PROGRAM_DRAW][1], lv->pc[4], lv->pc[5]);
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}
	}
	if (drawn)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glBindVertexArray(wasArray);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, wasFramebuffer);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	glActiveTexture(wasUnit);

	/* What the ring had up to here is the GPU's until this frame is done:
	 * its fence. Without a mapping the bytes went with the calls above, and
	 * the ring starts again with the next frame. */
	if (td.persistent)
	{
		td.flushed = td.head;
		if (td.fences)
		{
			if (td.pendingCount == MAX_FENCES)
			{
				/* (the newest fence goes, and this one, later, stands for
				 * what it stood for too) */
				td.pendingCount--;
				glDeleteSync((GLsync)td.pending[td.pendingCount].fence);
			}
			td.pending[td.pendingCount].fence = (void *)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			td.pending[td.pendingCount].end = td.head;
			td.pendingCount++;
		}
	}
	else
		td.head = td.tail = td.flushed = 0;
	td.jobCount = td.levelCount = 0;
#endif
}
