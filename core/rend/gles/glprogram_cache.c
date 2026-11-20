/* OpenGL programs made without a frame waiting for one: glprogram_cache.h */
#include <stdlib.h>
#include <string.h>
#include "gles.h"
#include "glprogram_cache.h"
#include "paths.h"

#include <streams/file_stream.h>

#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#endif
#ifndef GL_NUM_PROGRAM_BINARY_FORMATS
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
#endif
#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif
#ifndef GL_COMPLETION_STATUS_KHR
#define GL_COMPLETION_STATUS_KHR 0x91B1
#endif
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif

static const char *const CacheFile = "gl_programs.cache";
#define CacheMagic 0x504C4746u	/* "FGLP" */
#define CacheVersion 1u
/* (a key list or a file larger than this is not one this wrote) */
#define MostKeys (1u << 16)
#define MostBytes ((int64_t)256 << 20)

/* (OpenGL ES 2's headers have their own name for the calling convention) */
#if defined(APIENTRY)
#define GLPROG_APIENTRY APIENTRY
#elif defined(GL_APIENTRY)
#define GLPROG_APIENTRY GL_APIENTRY
#else
#define GLPROG_APIENTRY
#endif
typedef void (GLPROG_APIENTRY *ProgramBinaryFn)(GLuint program, GLenum format, const void *binary, GLsizei length);
typedef void (GLPROG_APIENTRY *GetProgramBinaryFn)(GLuint program, GLsizei bufSize, GLsizei *length, GLenum *format, void *binary);
typedef void (GLPROG_APIENTRY *ProgramParameteriFn)(GLuint program, GLenum pname, GLint value);
typedef void (GLPROG_APIENTRY *MaxThreadsFn)(GLuint count);


/* An array that grows: @n of its @cap elements of @elem bytes used */
typedef struct glp_vec
{
	u8 *v;
	size_t n, cap, elem;
} glp_vec;

#define VEC_AT(vec, T, i) (((T *)(vec)->v)[i])

static bool vec_reserve(glp_vec *a, size_t cap)
{
	u8 *v;
	if (cap <= a->cap)
		return true;
	v = (u8 *)realloc(a->v, cap * a->elem);
	if (v == NULL)
		return false;
	a->v = v;
	a->cap = cap;
	return true;
}

/* @x appended, the array made larger if it must be (false: no memory) */
static bool vec_push(glp_vec *a, const void *x)
{
	if (a->n == a->cap && !vec_reserve(a, a->cap != 0 ? a->cap * 2 : 16))
		return false;
	memcpy(a->v + a->n * a->elem, x, a->elem);
	a->n++;
	return true;
}

static void vec_free(glp_vec *a)
{
	free(a->v);
	a->v = NULL;
	a->n = a->cap = 0;
}

/* (b's memory a's, and a's b's) */
static void vec_swap(glp_vec *a, glp_vec *b)
{
	const glp_vec t = *a;
	*a = *b;
	*b = t;
}

typedef struct Entry
{
	u64 hash;
	u32 format;
	u32 length;
	size_t at;		/* in cache.data */
} Entry;

typedef struct Fresh
{
	u64 hash;
	GLuint program;
} Fresh;

typedef struct Cache
{
	bool open;
	bool binaries;
	bool parallel;		/* the extension */
	bool background;	/* ...and seen to work */
	bool started;
	bool lost;		/* the context went: its programs are not asked for anything */
	bool dirty;
	bool failed;		/* the last glprog_make(): see glprog_failed() */
	u64 driver;
	ProgramBinaryFn programBinary;
	GetProgramBinaryFn getProgramBinary;
	ProgramParameteriFn programParameteri;

	glp_vec data;			/* the file as read: the binaries are in it (u8) */
	glp_vec entries;		/* by hash (Entry) */
	glp_vec known[GLPROG_LISTS];	/* from the file (u32) */
	glp_vec noted[GLPROG_LISTS];	/* this run's, not in known (u32) */
	glp_vec fresh;			/* compiled this run, to be kept (Fresh) */

	/* counts, for the log */
	u32 fromDisk, compiled, inFrames, generalDraws, refused;
} Cache;

static Cache cache;

/* (empty, its arrays of what they hold) */
static void cache_init(Cache *c)
{
	int l;
	memset(c, 0, sizeof(*c));
	c->data.elem = 1;
	c->entries.elem = sizeof(Entry);
	for (l = 0; l < GLPROG_LISTS; l++)
	{
		c->known[l].elem = sizeof(u32);
		c->noted[l].elem = sizeof(u32);
	}
	c->fresh.elem = sizeof(Fresh);
}

static void cache_free(Cache *c)
{
	int l;
	vec_free(&c->data);
	vec_free(&c->entries);
	for (l = 0; l < GLPROG_LISTS; l++)
	{
		vec_free(&c->known[l]);
		vec_free(&c->noted[l]);
	}
	vec_free(&c->fresh);
}

static u64 fnv1a(u64 h, const void *p, size_t n)
{
	const u8 *s = (const u8 *)p;
	size_t i;
	for (i = 0; i < n; i++)
		h = (h ^ s[i]) * 0x100000001B3ull;
	return h;
}

static u64 fnv1a_str(u64 h, const char *s)
{
	return fnv1a(h, s, strlen(s));
}

#define FnvBasis 0xCBF29CE484222325ull

static bool has_extension(const char *name)
{
	const char *all, *p;
	size_t len;
#ifndef HAVE_OPENGLES2
	if (gl.gl_major >= 3)
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
#endif
	all = (const char *)glGetString(GL_EXTENSIONS);
	if (all == NULL)
		return false;
	len = strlen(name);
	for (p = strstr(all, name); p != NULL; p = strstr(p + 1, name))
		if ((p == all || p[-1] == ' ') && (p[len] == ' ' || p[len] == 0))
			return true;
	return false;
}

static bool version_at_least(int major, int minor)
{
	return gl.gl_major > major || (gl.gl_major == major && gl.gl_minor >= minor);
}

/* (the reads of the file: bounds checked) */
typedef struct Reader
{
	const u8 *p;
	size_t left;
	bool ok;
} Reader;

static void reader_get(Reader *r, void *v, size_t n)
{
	if (r->left < n)
	{
		r->ok = false;
		r->left = 0;
		memset(v, 0, n);
		return;
	}
	memcpy(v, r->p, n);
	r->p += n;
	r->left -= n;
}

static u32 reader_u32(Reader *r)
{
	u32 v;
	reader_get(r, &v, sizeof(v));
	return v;
}

static u64 reader_u64(Reader *r)
{
	u64 v;
	reader_get(r, &v, sizeof(v));
	return v;
}

/* @n bytes put at the end of @out (false: no memory) */
static bool put(glp_vec *out, const void *v, size_t n)
{
	size_t want = out->cap != 0 ? out->cap : 4096;
	while (want < out->n + n)
		want *= 2;
	if (!vec_reserve(out, want))
		return false;
	memcpy(out->v + out->n, v, n);
	out->n += n;
	return true;
}

static int u32_order(const void *a, const void *b)
{
	const u32 x = *(const u32 *)a, y = *(const u32 *)b;
	return x < y ? -1 : x > y;
}

static int u64_order(const void *a, const void *b)
{
	const u64 x = *(const u64 *)a, y = *(const u64 *)b;
	return x < y ? -1 : x > y;
}

/* (by hash; one hash is not there twice) */
static int entry_order(const void *a, const void *b)
{
	return u64_order(&((const Entry *)a)->hash, &((const Entry *)b)->hash);
}

/* The path of the file, the caller's to free(); NULL without memory */
static char *cache_path(void)
{
	const size_t len = writable_data_path(NULL, 0, CacheFile);
	char *path = (char *)malloc(len + 1);
	if (path != NULL)
		writable_data_path(path, len + 1, CacheFile);
	return path;
}

/* The file: magic, version, the driver's key; for each list the number of
 * its keys and the keys; the number of binaries and each one's hash,
 * format, length and bytes; a hash of all that went before. A file of
 * another driver has its keys taken and its binaries left. */
static void read_file(void)
{
	char *path;
	RFILE *f;
	int64_t size;
	glp_vec data, entries, keys;
	u64 sum, driver;
	Reader r;
	u32 lists, l, n, i;
	bool read;

	path = cache_path();
	if (path == NULL)
		return;
	f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f == NULL)
	{
		free(path);
		return;
	}
	memset(&data, 0, sizeof(data));
	data.elem = 1;
	memset(&entries, 0, sizeof(entries));
	entries.elem = sizeof(Entry);
	size = filestream_get_size(f);
	read = size > 24 && size < MostBytes && vec_reserve(&data, (size_t)size)
			&& filestream_read(f, data.v, size) == size;
	filestream_close(f);
	if (!read)
	{
		vec_free(&data);
		free(path);
		return;
	}
	data.n = (size_t)size;
	memcpy(&sum, &data.v[data.n - 8], 8);
	if (fnv1a(FnvBasis, data.v, data.n - 8) != sum)
	{
		WARN_LOG(RENDERER, "OpenGL: %s is not whole, and is made again\n", path);
		vec_free(&data);
		free(path);
		return;
	}
	r.p = data.v;
	r.left = data.n - 8;
	r.ok = true;
	if (reader_u32(&r) != CacheMagic || reader_u32(&r) != CacheVersion)
		goto out;
	driver = reader_u64(&r);
	lists = reader_u32(&r);
	for (l = 0; l < lists && r.ok; l++)
	{
		n = reader_u32(&r);
		if (n > MostKeys || (size_t)n * 4 > r.left)
			goto out;
		memset(&keys, 0, sizeof(keys));
		keys.elem = sizeof(u32);
		if (n != 0)
		{
			if (!vec_reserve(&keys, n))
				goto out;
			memcpy(keys.v, r.p, (size_t)n * 4);
			keys.n = n;
		}
		r.p += (size_t)n * 4;
		r.left -= (size_t)n * 4;
		if (l < GLPROG_LISTS)
			vec_swap(&cache.known[l], &keys);
		vec_free(&keys);
	}
	if (!r.ok || driver != cache.driver || !cache.binaries)
		goto out;
	n = reader_u32(&r);
	if (!vec_reserve(&entries, n < MostKeys ? n : MostKeys))
		goto out;
	for (i = 0; i < n && r.ok; i++)
	{
		Entry e;
		e.hash = reader_u64(&r);
		e.format = reader_u32(&r);
		e.length = reader_u32(&r);
		if (!r.ok || e.length == 0 || e.length > r.left)
			goto out;
		e.at = (size_t)(r.p - data.v);
		r.p += e.length;
		r.left -= e.length;
		if (!vec_push(&entries, &e))
			goto out;
	}
	if (!r.ok)
		goto out;
	if (entries.n != 0)
		qsort(entries.v, entries.n, sizeof(Entry), entry_order);
	vec_swap(&cache.entries, &entries);
	vec_swap(&cache.data, &data);
out:
	vec_free(&entries);
	vec_free(&data);
	free(path);
}

static const Entry *find(u64 hash)
{
	size_t lo = 0, hi = cache.entries.n;
	while (lo < hi)
	{
		const size_t mid = (lo + hi) / 2;
		if (VEC_AT(&cache.entries, Entry, mid).hash < hash)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < cache.entries.n && VEC_AT(&cache.entries, Entry, lo).hash == hash ? &VEC_AT(&cache.entries, Entry, lo) : NULL;
}

/* the program from the file: false if it is not there or the driver will not take it */
static bool from_disk(GLuint program, u64 hash)
{
	const Entry *e;
	GLint ok = 0;
	if (!cache.binaries)
		return false;
	e = find(hash);
	if (e == NULL)
		return false;
	cache.programBinary(program, (GLenum)e->format, &cache.data.v[e->at], (GLsizei)e->length);
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		/* (made by an older build of the same driver version, say: compiled) */
		cache.refused++;
		return false;
	}
	cache.fromDisk++;
	return true;
}

static void count_made(GLuint program, u64 hash)
{
	cache.compiled++;
	if (cache.started)
		cache.inFrames++;
	if (cache.binaries)
	{
		Fresh f;
		size_t i;
		f.hash = hash;
		f.program = program;
		cache.dirty = true;
		/* (a program's name is given again once the program is deleted:
		 * an older one of the name is gone) */
		for (i = 0; i < cache.fresh.n; i++)
			if (VEC_AT(&cache.fresh, Fresh, i).program == program)
			{
				VEC_AT(&cache.fresh, Fresh, i) = f;
				return;
			}
		if (cache.fresh.n == cache.fresh.cap && cache.started)
			return;	/* (no allocation in a frame: kept no more this run) */
		vec_push(&cache.fresh, &f);
	}
}

static GLuint compile(GLenum type, const char *const *pieces, GLsizei count)
{
	const GLuint shader = glCreateShader(type);
	glShaderSource(shader, count, (const GLchar **)pieces, NULL);
	glCompileShader(shader);
	return shader;
}

static void log_failure(GLuint program, GLuint vs, GLuint fs)
{
	char log[2048];
	GLuint shaders[2];
	int i;
	shaders[0] = vs;
	shaders[1] = fs;
	for (i = 0; i < 2; i++)
	{
		GLint ok = 0;
		if (shaders[i] == 0)
			continue;
		glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
		if (!ok)
		{
			log[0] = 0;
			glGetShaderInfoLog(shaders[i], sizeof(log), NULL, log);
			WARN_LOG(RENDERER, "OpenGL: a shader did not compile: %s\n", log);
		}
	}
	log[0] = 0;
	glGetProgramInfoLog(program, sizeof(log), NULL, log);
	WARN_LOG(RENDERER, "OpenGL: a program did not link: %s\n", log);
}

void glprog_open(void)
{
	static const GLenum names[] = { GL_VENDOR, GL_RENDERER, GL_VERSION };
	glsm_ctx_proc_address_t proc;
	u64 h = FnvBasis;
	bool procs;
	int l;
	size_t i;

	glprog_close();
	cache_init(&cache);
	cache.open = true;

	/* what the programs kept are good for: this driver */
	for (i = 0; i < 3; i++)
	{
		const char *s = (const char *)glGetString(names[i]);
		h = fnv1a_str(h, s != NULL ? s : "");
		h = fnv1a_str(h, "|");
	}
	cache.driver = h;

	proc.addr = NULL;
	procs = glsm_ctl(GLSM_CTL_PROC_ADDRESS_GET, &proc) && proc.addr != NULL;
	if (procs && (gl.is_gles ? gl.gl_major >= 3 : (version_at_least(4, 1) || has_extension("GL_ARB_get_program_binary"))))
	{
		GLint formats = 0;
		glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
		cache.programBinary = (ProgramBinaryFn)proc.addr("glProgramBinary");
		cache.getProgramBinary = (GetProgramBinaryFn)proc.addr("glGetProgramBinary");
		cache.programParameteri = (ProgramParameteriFn)proc.addr("glProgramParameteri");
		cache.binaries = formats > 0 && cache.programBinary != NULL && cache.getProgramBinary != NULL
				&& cache.programParameteri != NULL;
	}
	if (procs)
	{
		MaxThreadsFn maxThreads = NULL;
		if (has_extension("GL_KHR_parallel_shader_compile"))
			maxThreads = (MaxThreadsFn)proc.addr("glMaxShaderCompilerThreadsKHR");
		else if (has_extension("GL_ARB_parallel_shader_compile"))
			maxThreads = (MaxThreadsFn)proc.addr("glMaxShaderCompilerThreadsARB");
		if (maxThreads != NULL)
		{
			/* (as many threads as the driver likes) */
			maxThreads(0xFFFFFFFFu);
			cache.parallel = true;
		}
	}
	while (glGetError() != GL_NO_ERROR)
		;
	read_file();
	for (l = 0; l < GLPROG_LISTS; l++)
		vec_reserve(&cache.noted[l], 1024);
	vec_reserve(&cache.fresh, 1024);
	INFO_LOG(RENDERER, "OpenGL: programs %s, %u kept; the driver makes them %s\n",
			cache.binaries ? "kept on disk" : "not kept (no binaries)", (unsigned)cache.entries.n,
			cache.parallel ? "in the background, if it can" : "as it is asked");
}

void glprog_started(void)
{
	cache.started = true;
}

bool glprog_binaries(void)
{
	return cache.binaries;
}

u32 glprog_make(const char *vertex, const char *fragment, bool background)
{
	u64 hash = fnv1a_str(FnvBasis, "graphics|");
	GLuint program, vs, fs;

	hash = fnv1a_str(hash, vertex);
	hash = fnv1a_str(hash, "|");
	hash = fnv1a_str(hash, fragment);

	cache.failed = false;
	program = glCreateProgram();
	if (from_disk(program, hash))
		return program;

	vs = compile(GL_VERTEX_SHADER, &vertex, 1);
	fs = compile(GL_FRAGMENT_SHADER, &fragment, 1);
	glAttachShader(program, vs);
	glAttachShader(program, fs);

	/* Bind vertex attribute to VBO inputs */
	glBindAttribLocation(program, VERTEX_POS_ARRAY,      "in_pos");
	glBindAttribLocation(program, VERTEX_COL_BASE_ARRAY, "in_base");
	glBindAttribLocation(program, VERTEX_COL_OFFS_ARRAY, "in_offs");
	glBindAttribLocation(program, VERTEX_UV_ARRAY,       "in_uv");
	glBindAttribLocation(program, VERTEX_COL_BASE1_ARRAY, "in_base1");
	glBindAttribLocation(program, VERTEX_COL_OFFS1_ARRAY, "in_offs1");
	glBindAttribLocation(program, VERTEX_UV1_ARRAY,       "in_uv1");
	glBindAttribLocation(program, VERTEX_PP_ARRAY,        "in_pp");
#ifndef HAVE_OPENGLES
	glBindFragDataLocation(program, 0, "FragColor");
#endif
	if (cache.binaries)
		cache.programParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	glLinkProgram(program);
	if (background && cache.parallel)
	{
		/* (whether the driver really does it in the background: not done
		 * as soon as it is asked) */
		GLint done = 0;
		glGetProgramiv(program, GL_COMPLETION_STATUS_KHR, &done);
		if (!done)
			cache.background = true;
	}
	else
	{
		GLint ok = 0, said = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &ok);
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &said);
		/* (a driver that does not say why - an OpenGL that does nothing,
		 * headless.sh's - has its program taken as it is, as ever) */
		if (!ok && said > 0)
		{
			log_failure(program, vs, fs);
			WARN_LOG(RENDERER, "VERTEX:\n%s\nFRAGMENT:\n%s\n", vertex, fragment);
			glDeleteShader(vs);
			glDeleteShader(fs);
			glDeleteProgram(program);
			cache.failed = true;
			return 0;
		}
	}
	/* (they go with the program) */
	glDeleteShader(vs);
	glDeleteShader(fs);
	count_made(program, hash);
	return program;
}

u32 glprog_make_compute(const char *const *pieces, u32 count)
{
	u64 hash = fnv1a_str(FnvBasis, "compute|");
	GLuint program, cs;
	GLint ok = 0;
	u32 i;

	for (i = 0; i < count; i++)
		hash = fnv1a_str(hash, pieces[i]);

	program = glCreateProgram();
	if (from_disk(program, hash))
		return program;
	cs = compile(GL_COMPUTE_SHADER, pieces, (GLsizei)count);
	glAttachShader(program, cs);
	if (cache.binaries)
		cache.programParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		log_failure(program, cs, 0);
		glDeleteShader(cs);
		glDeleteProgram(program);
		return 0;
	}
	glDetachShader(program, cs);
	glDeleteShader(cs);
	count_made(program, hash);
	return program;
}

bool glprog_failed(void)
{
	return cache.failed;
}

GlProgramStatus glprog_poll(u32 program)
{
	GLint v = 0;
	if (cache.parallel)
	{
		glGetProgramiv(program, GL_COMPLETION_STATUS_KHR, &v);
		if (!v)
			return GLPROG_PENDING;
	}
	glGetProgramiv(program, GL_LINK_STATUS, &v);
	return v ? GLPROG_READY : GLPROG_FAILED;
}

bool glprog_wait(u32 program)
{
	GLint ok = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok)
		log_failure(program, 0, 0);
	return ok != 0;
}

void glprog_note(GlProgramList list, u32 key)
{
	glp_vec *n = &cache.noted[list];
	const glp_vec *k = &cache.known[list];
	if (!cache.open)
		return;
	if (n->n == n->cap && cache.started)
		return;	/* (no allocation in a frame) */
	if (k->n != 0 && bsearch(&key, k->v, k->n, sizeof(u32), u32_order) != NULL)
		return;
	vec_push(n, &key);
	cache.dirty = true;
}

const u32 *glprog_known(GlProgramList list, u32 *count)
{
	*count = (u32)cache.known[list].n;
	return (const u32 *)cache.known[list].v;
}

void glprog_general_used(void)
{
	cache.generalDraws++;
}

void glprog_save(bool contextGoing)
{
	char *path;
	glp_vec out, written;
	size_t countAt, i;
	u32 count = 0, zero = 0, v32;
	u64 v64;
	RFILE *f;
	int l;

	if (!cache.open || !cache.dirty)
	{
		if (contextGoing)
		{
			cache.lost = true;
			cache.fresh.n = 0;
		}
		return;
	}
	memset(&out, 0, sizeof(out));
	out.elem = 1;
	memset(&written, 0, sizeof(written));
	written.elem = sizeof(u64);
	v32 = CacheMagic;
	put(&out, &v32, 4);
	v32 = CacheVersion;
	put(&out, &v32, 4);
	put(&out, &cache.driver, 8);
	v32 = GLPROG_LISTS;
	put(&out, &v32, 4);
	for (l = 0; l < GLPROG_LISTS; l++)
	{
		glp_vec keys;
		size_t w, r;
		memset(&keys, 0, sizeof(keys));
		keys.elem = sizeof(u32);
		vec_reserve(&keys, cache.known[l].n + cache.noted[l].n);
		if (cache.known[l].n != 0)
			memcpy(keys.v, cache.known[l].v, cache.known[l].n * 4);
		if (cache.noted[l].n != 0)
			memcpy(keys.v + cache.known[l].n * 4, cache.noted[l].v, cache.noted[l].n * 4);
		keys.n = cache.known[l].n + cache.noted[l].n;
		if (keys.n != 0)
			qsort(keys.v, keys.n, sizeof(u32), u32_order);
		/* (each one once) */
		for (w = r = 0; r < keys.n; r++)
			if (w == 0 || VEC_AT(&keys, u32, r) != VEC_AT(&keys, u32, w - 1))
				VEC_AT(&keys, u32, w++) = VEC_AT(&keys, u32, r);
		keys.n = w;
		if (keys.n > MostKeys)
			keys.n = MostKeys;
		v32 = (u32)keys.n;
		put(&out, &v32, 4);
		if (keys.n != 0)
			put(&out, keys.v, keys.n * 4);
		vec_swap(&cache.known[l], &keys);
		vec_free(&keys);
		cache.noted[l].n = 0;
	}
	/* the binaries: those made this run (where the programs are still
	 * there), and the file's others */
	countAt = out.n;
	put(&out, &zero, 4);
	if (cache.binaries && !cache.lost)
		for (i = 0; i < cache.fresh.n; i++)
		{
			const Fresh *fr = &VEC_AT(&cache.fresh, Fresh, i);
			GLint length = 0;
			GLenum format = 0;
			GLsizei got = 0;
			size_t at;
			u32 f32, l32;
			if (!glIsProgram(fr->program) || glprog_poll(fr->program) != GLPROG_READY)
				continue;
			glGetProgramiv(fr->program, GL_PROGRAM_BINARY_LENGTH, &length);
			if (length <= 0)
				continue;
			at = out.n;
			{
				/* (room for its record, then made what it is) */
				size_t want = out.cap != 0 ? out.cap : 4096;
				while (want < at + 16 + (size_t)length)
					want *= 2;
				if (!vec_reserve(&out, want))
					continue;
			}
			cache.getProgramBinary(fr->program, length, &got, &format, out.v + at + 16);
			if (got <= 0 || got > length)
				continue;
			f32 = (u32)format;
			l32 = (u32)got;
			memcpy(out.v + at, &fr->hash, 8);
			memcpy(out.v + at + 8, &f32, 4);
			memcpy(out.v + at + 12, &l32, 4);
			out.n = at + 16 + (size_t)got;
			vec_push(&written, &fr->hash);
			count++;
		}
	if (written.n != 0)
		qsort(written.v, written.n, sizeof(u64), u64_order);
	for (i = 0; i < cache.entries.n; i++)
	{
		const Entry *e = &VEC_AT(&cache.entries, Entry, i);
		if (written.n != 0 && bsearch(&e->hash, written.v, written.n, sizeof(u64), u64_order) != NULL)
			continue;
		put(&out, &e->hash, 8);
		put(&out, &e->format, 4);
		put(&out, &e->length, 4);
		put(&out, &cache.data.v[e->at], e->length);
		count++;
	}
	vec_free(&written);
	memcpy(out.v + countAt, &count, 4);
	v64 = fnv1a(FnvBasis, out.v, out.n);
	put(&out, &v64, 8);
	while (glGetError() != GL_NO_ERROR)
		;

	path = cache_path();
	f = path != NULL ? filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE) : NULL;
	if (f != NULL)
	{
		const bool ok = filestream_write(f, out.v, (int64_t)out.n) == (int64_t)out.n;
		filestream_close(f);
		INFO_LOG(RENDERER, "OpenGL: %u programs kept in %s (%u bytes)%s\n", count, path, (unsigned)out.n,
				ok ? "" : ": could not be written");
	}
	free(path);

	/* what is in the file now is what is looked in from now on */
	cache.entries.n = 0;
	cache.data.n = 0;
	if (cache.binaries)
	{
		Reader r;
		r.p = out.v + countAt + 4;
		r.left = out.n - countAt - 12;
		r.ok = true;
		for (i = 0; i < count && r.ok; i++)
		{
			Entry e;
			e.hash = reader_u64(&r);
			e.format = reader_u32(&r);
			e.length = reader_u32(&r);
			e.at = (size_t)(r.p - out.v);
			r.p += e.length;
			r.left -= e.length;
			vec_push(&cache.entries, &e);
		}
		if (cache.entries.n != 0)
			qsort(cache.entries.v, cache.entries.n, sizeof(Entry), entry_order);
		vec_swap(&cache.data, &out);
	}
	vec_free(&out);
	cache.fresh.n = 0;
	cache.dirty = false;
	if (contextGoing)
		cache.lost = true;
}

void glprog_log(const char *who)
{
	if (!cache.open)
		return;
	NOTICE_LOG(RENDERER, "OpenGL programs (%s): %u from disk, %u compiled (%u of them in frames), %u turned down by the driver; "
			"%u polygons drawn with the general program while theirs was not there%s\n",
			who, cache.fromDisk, cache.compiled, cache.inFrames, cache.refused, cache.generalDraws,
			cache.parallel ? (cache.background ? "; the driver compiles in the background" : "; the driver compiles as it is asked") : "");
}

void glprog_close(void)
{
	if (!cache.open)
		return;
	glprog_save(false);
	cache_free(&cache);
	cache_init(&cache);
}
